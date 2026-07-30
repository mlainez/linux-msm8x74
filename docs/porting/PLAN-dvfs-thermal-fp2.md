# Application of the blueprint: DVFS + thermal throttling on Fairphone 2 (msm8974pro)

Concrete ACUs, interaction matrix, checkpoints and acceptance criteria for the FP2 CPU
frequency/voltage scaling and thermal throttling work.

**This is the one target-specific document of the set** — it instantiates the generic
method for Fairphone 2 / msm8974pro. The reusable parts live in
`BLUEPRINT-kernel-feature-bringup.md` (method) and `LAB-OPERATIONS.md` (devices, reference
corpus, target profile, buildroot loop). Read those first; everything below assumes the FP2
target profile has been derived from the oracle and that the reference reconnaissance
(pmaports, the msm8974 mainline fork, sibling msm8974 devices) has been done.

---

## 0. Strategic recommendation, before any of the below

Your actual business goal ("move to 6.18 so I can follow LTS") is currently **coupled** to
the hardest feature in the tree. Decouple them:

> **Ship 6.18 with DVFS gated off as soon as CP1 passes.** That delivers LTS security
> patches on a kernel you trust, and demotes DVFS from a blocker to an additive feature
> developed behind a switch.

This also creates the control you have never had. You know *6.16 without DVFS* is stable
(8 h+) and *6.18 with DVFS* is not. You do **not** know whether *6.18 without DVFS* is
stable — so every failure so far has had two candidate explanations (the LTS bump, or the
feature). CP1 collapses that ambiguity, and it is cheap.

---

## 1. Assets

| Role | Device | Use |
|---|---|---|
| **Oracle** | FP2 #1, rooted Android, vendor msm-3.4 stack | Read-only ground truth: frequency/voltage tables, thermal thresholds, corner behaviour, real silicon limits. Never reflash. |
| **DUT** | FP2 #2, carrier board (external power, USB-hub ethernet, no screen, serial via minicom) | All experiments. Headless DTB is production. |
| **Reference** | `FairphoneMirrors/android_kernel_fairphone_msm8974` (fp2-m-sibon), mainline history | Cited by file/commit. |
| **Anchor** | 6.16 no-DVFS image (known stable 8 h+); later, each CP-passing image | One-command rollback. |

Constraint to design around: **one DUT ⇒ cumulative clean runtime is the currency and
cannot be parallelised** (blueprint §7). And **no rail instrumentation** on the carrier —
brownout-vs-hang must be inferred from PMIC reset reason and behavioural bisection.

---

## 2. State of knowledge (ledger seed — do not re-litigate without new evidence)

**Ruled out, with evidence:**
| Suspect | Evidence it is not the cause |
|---|---|
| rpmpd CX switch (`7d004ab4e`) | Author is Luca Weiss (cherry-pick, Jan 2025); wiring is byte-identical on the stable 6.16 baseline and on 6.15 |
| GPU / non-secure IOMMU | Resets occur with the GPU node disabled, and on the display DTB where GPU is `disabled` |
| ModemManager SEGV storm | Mitigated (1.23.4 + hold + `RestartSec=30`); was a CPU-load trigger, not a root cause |
| SPM write-path defects | PORT-clear, vlevel encoding, PMIC_STS poll — all three fixed and present |

**Found and fixed (effect measured):** CX corner starvation for Krait — HFPLL digital and
L2 logic sit on VDDCX with no Apps-side vote; graded corners took lifetime 19 min → 95 min
→ 6 h 40+, then pinned at super-turbo to remove the corner-transition race.

**Fixed, effect NOT yet measured:** inverted HFPLL lock poll (`clk-hfpll.c`) — the poll
exited on "not locked", so `PLL_OUTCTRL` was enabled on an unlocked PLL. Landed in
`6.18/rc` `1bac29eca`; compiles clean; **no device validation yet**.

**Open, unquantified:** items in §4 marked `?`.

---

## 3. Target specification (fill from the oracle at CP0 — these are the numbers "done" means)

| Quantity | Source | Value |
|---|---|---|
| Max sustained CPU frequency on this silicon | oracle `cpuinfo_max_freq` | *TBD* |
| Frequency table actually used by vendor | oracle acpuclk / scaling_available_frequencies | *TBD* |
| Voltage per frequency for **the DUT's bin** | vendor table for DUT's speed/PVS/version | *TBD* |
| CX corner per CPU/L2 frequency | vendor `qcom,l2-fmax`, `hfpll-dig-supply` | *TBD* |
| MX floor and its coupling to CX | vendor pm8841 config / RPM behaviour | *TBD* |
| Thermal trip temperatures + mitigation steps | oracle `thermal-engine.conf`, `msm_thermal` params | *TBD* |
| Lowest frequency vendor ever mitigates to | oracle under thermal load | *TBD* |

**Known so far from the tree (verify against oracle):** OPPs ≤ **2265.6 MHz** are marked
`opp-supported-hw = <0xf>` (all speed bins) with fixed generic `opp-microvolt` fallbacks;
the top three (2342.4 / 2419.2 / 2457.6 MHz) are `<0x8>` (speed bin 3 only) with **no**
generic fallback. 2265.6 MHz corresponds to the Snapdragon 801 MSM8974AB rating. Two
consequences to check early (X1/X2 below): whether the DUT is bin 3 at all, and whether
`0xf` (widened from `0xa` in `39f97169c`) exposes frequencies or voltage rows that this
die was never binned for.

---

## 4. ACU inventory

`?` = unvalidated. Kill switch is what makes combination testing cheap.

### Layer O — Observability (prerequisite, blueprint P1)
| ID | Capability | Kill switch | Accept |
|---|---|---|---|
| O1 | Serial console + full boot log captured | — | Log of a full boot retained off-device |
| O2 | Reset-reason capture (pm8941 `pon@800`: PON/WARM_RESET/POFF reason; lk2nd banner) | — | A deliberately induced reset is correctly classified |
| O3 | fsync'd device telemetry: uptime, per-core freq, rail µV, CX corner, tsens temps, at ≤15 s | systemd unit | Data survives an unclean reset up to the last sample |
| O4 | Detectors: softlockup, hardlockup, RCU stall verbose; debug variant adds lockdep + `DEBUG_ATOMIC_SLEEP` | cmdline | A synthetic hang prints before reset |
| O5 | Anchor image + per-build manifest (src SHA, config hash, DTB hash, userspace) | — | Rollback in one command; every test image identifiable |
| O6 | Oracle extraction harness (scripted dumps, §5) | — | All §3 rows populated |

### Layer B — Base
| ID | Capability | Kill switch | Accept |
|---|---|---|---|
| B1 | 6.18 LTS base stable with all DVFS/thermal gated off | build variant BV-A | CP1 |

### Layer F — Frequency (clock) path
| ID | Capability | Depends | Shared state | Kill switch | ? |
|---|---|---|---|---|---|
| F1 | HFPLL enable/lock correctness | — | HFPLL regs, `h->lock` (IRQs off) | build | ? |
| F2 | krait-cc mux + secondary-PLL switching + L2 clock | F1 | Krait mux, L2 clock | build | ? |
| F3 | **Frequency scaling at fixed (isovoltage) rail** | F1,F2 | CPU clock only | BV-E DTB | ? |
| F4 | Full frequency range incl. bin gating | F3,V2 | — | `scaling_max_freq` | ? |

### Layer V — Voltage path
| ID | Capability | Depends | Shared state | Kill switch | ? |
|---|---|---|---|---|---|
| V1 | SAW2 gang-rail write + readback correctness | — | L2 SAW2 VCTL/PMIC_STS, SPMI | — | fixed, verify |
| V2 | Fuse → speed/PVS/version read correctness | — | qfprom | dyndbg | ? |
| V3 | OPP voltage row selection (fuse row vs generic fallback) | V2 | CPU rail | DT variant | ? |
| V4 | Aging margin (`qcom,vdd-margin-microvolt`, now 50 mV) | V1 | CPU rail | DT value | ? |
| V5 | AVS | V1 | AVS regs | **kept OFF — out of scope this campaign** | — |

### Layer R — Shared rails / corners
| ID | Capability | Depends | Shared state | Kill switch | ? |
|---|---|---|---|---|---|
| R1 | CX corner vote for Krait, pinned super-turbo | rpmpd | **VDDCX** (SoC-wide) | `required-opps` DT | fixed |
| R2 | CX corner *graded* with frequency | R1 | VDDCX + transition ordering | DT variant | ? (raced) |
| R3 | **MX ≥ CX invariant** | R1 | pm8841_s1 (MX), VDDCX | — | ? |
| R4 | L2 rate → corner mapping (vendor `qcom,l2-fmax`) | F2,R1 | L2 clock, VDDCX | DT | ? gap |
| R5 | Remoteproc CX proxy interaction (ADSP/modem/WCNSS) | R1 | VDDCX aggregation | disable remoteprocs | ? |

### Layer I — Idle
| ID | Capability | Shared state | Kill switch | ? |
|---|---|---|---|---|
| I1 | SPC power collapse coexisting with DVFS | SAW2 sequencer ↔ VCTL, CX | `cpuidle.off=1` / disable `state1` | ? |
| I2 | Idle governor choice (menu vs teo/ladder) | SPC entry rate | `cpuidle.governor` | ? |

### Layer T — Thermal
| ID | Capability | Depends | Kill switch | ? |
|---|---|---|---|---|
| T1 | tsens per-core sanity/calibration (vs oracle + pm8941 die ADC) | — | — | ? |
| T2 | Zones + trip points (currently 90 °C passive / 105 °C crit) | T1 | DT | ? |
| T3 | Cooling device binding / cooling maps | T2,F4 | DT | ? |
| T4 | Passive throttling under sustained load | T3,F4,V3 | zone `mode` | ? |
| T5 | Critical trip / emergency shutdown path | T2 | DT | ? |
| T6 | Hysteresis / anti-oscillation behaviour | T4 | DT | ? |

### Layer M — Multicore
| ID | Capability | Shared state | ? |
|---|---|---|---|
| M1 | Gang-rail aggregation: fastest core sets the shared voltage | CPU rail | ? |
| M2 | Concurrent transitions on 4 cores (locks, `smp_call`, races) | SAW2, HFPLLs | ? |
| M3 | CPU hotplug + DVFS | rail votes, PD refcounts | ? |

### Layer L — Load / duration profiles (test modes, not features)
L1 idle soak · L2 sustained-load soak (thermal steady state) · L3 **transition-storm**
soak (rapid freq changes — worst case for races) · L4 real workload with peripherals
(USB-eth, wifi, modem, ADSP sensors).

---

## 5. Oracle extraction (O6) — run on the rooted Android phone

Read-only. Capture everything to files, commit them alongside the ledger.

1. **Frequency limits/table** — `cpuinfo_max_freq`, `scaling_available_frequencies`,
   vendor acpuclk table (`/d/acpuclk/*`, or `dmesg | grep -i acpu`), `/proc/config.gz`.
2. **Bin identity + tables** — `dmesg | grep -iE "pvs|speed.?bin|efuse"`; the vendor
   frequency/voltage table for each bin (from the vendor tree if not exposed at runtime).
3. **Rails** — `/d/regulator/regulator_summary` (CPU rail, CX = pm8841 s2, MX = s1) at
   idle and under load; `/d/krait-regulator/*` if present.
4. **Corners / RPM** — `/d/rpm_stats`, `/d/rpm_master_stats`, `/d/rpm_send_msg`; observe
   CX/MX while ramping frequency → derives the R1/R2/R4 mapping and the **R3 invariant**.
5. **Thermal policy** — `/vendor/etc/thermal-engine.conf` (or `/system/etc/…`),
   `/sys/module/msm_thermal/parameters/*`, `/sys/class/thermal/thermal_zone*/{type,temp,trip_point_*}`.
6. **Live target curve** — log freq + rail µV + temps through an idle→full-load→cooldown
   ramp. This is the behaviour the mainline stack must approximate.

> Oracle caveat: different die. It yields **algorithm + tables**; the DUT's own
> speed/PVS/version must be read on the DUT (X1).

---

## 6. Build variants and switch matrix (blueprint P4)

| Variant | Content | Purpose |
|---|---|---|
| **BV-A** | DVFS + thermal gated off (single OPP, thermal zones disabled) | B1 / CP1 baseline |
| **BV-B** | Full stack, everything runtime-gateable | main workhorse |
| **BV-C** | BV-B + lockdep, `DEBUG_ATOMIC_SLEEP`, RCU-stall verbose | catch hangs/atomic bugs (D-item) |
| **BV-D** | BV-B with corner strategy variants (pinned vs graded) and margin values | R1/R2/V4 |
| **BV-E** | **Isovoltage**: every OPP pinned to one safe voltage (e.g. 1.10 V), frequency free | **F3 — separates clock from voltage** |

Runtime switches used across runs: `scaling_min_freq`/`scaling_max_freq`,
`scaling_governor`, `cpuidle.off=1` / per-state `disable`, `cpuidle.governor`, thermal zone
`mode`, remoteproc unbind, sensor modules unloaded.

Branching: one `6.18/topic/*` per ACU cluster; throwaway `6.18/test/*` for instrumented or
isovoltage variants (device-only hacks never enter a topic branch).

---

## 7. Interaction matrix — the pairs that MUST be tested together

**E** = expected interaction (shared resource named) → mandatory. Everything not listed is
**I** (independent) or **U** (test only after all E cells).

| Pair | Shared resource | Why it can only fail in combination |
|---|---|---|
| F3 × V3 | CPU gang rail | The core DVFS contract: every frequency needs its voltage |
| F3 × R1 | **VDDCX** | HFPLL digital + L2 logic are on CX; fast clocks on a low corner brown out (this was the real regression) |
| F1 × I1 | HFPLL regs, IRQs-off window | PLL re-enable on idle exit; historic "idle-state-dependent" signature |
| V1 × I1 | SAW2 VCTL / sleep sequencer | The sequencer can leave a stale VCTL PORT behind → voltage write misrouted |
| F4 × V2/V3 | fuse bin | Bin selects *both* the legal frequency set and the voltage rows; a wrong bin mismatches them |
| V3 × R3 | pm8841 MX/CX | Rail ordering (MX ≥ CX); no MX aggregator exists on 8974 |
| R1 × R5 | VDDCX aggregation | Remoteproc proxy votes drop at handover; CPU vote must stand alone |
| R4 × F2 | L2 clock + CX | L2 rate raised without its corner vote |
| M1 × V3 | shared rail | Fastest core must define voltage for all four |
| M2 × V1 | SAW2, `smp_call` | Concurrent transitions racing one rail |
| T4 × F3/V3 | freq+volt transitions | Thermal becomes an *involuntary* transition source |
| T4 × R2 | CX corner transitions | Throttling drives corner changes — the race R2 was pinned to avoid |
| M3 × R1 | genpd refcounts | Hotplug may drop the CX vote |
| L3 × (all) | transition rate | Storms convert rare races into reproducible ones |
| L4 × R5/T4 | CX, thermal, buses | Peripherals add CX voters and heat simultaneously |

---

## 8. Execution ladder and checkpoints

Each checkpoint has pre-registered criteria and fail-routing. `FAIL-UNKNOWN` at **any**
gate = stop, improve observability or convene the reassessment council (blueprint P8).
Soak durations follow blueprint §7.

### CP0 — Harness ready *(no feature work before this)*
ACUs: O1–O6. **Accept:** an induced reset is correctly classified by O2; telemetry
survives an unclean reset; a synthetic hang is printed by O4; all §3 target rows populated
from the oracle; anchor rollback demonstrated.
**Fail:** fix the harness. Never proceed on unattributable failures.

### CP1 — 6.18 LTS base without DVFS  ← **the missing control, highest priority**
Variant BV-A. **Accept:** ≥ **21 h** continuous clean (excludes the ~7 h mode), idle +
light load, telemetry complete, zero unexplained boots.
**Pass ⇒ ship it**: 6.18 becomes the LTS-tracking production kernel with DVFS off, and the
feature work below proceeds without blocking your LTS goal. Set as new anchor.
**Fail ⇒ the LTS bump itself is implicated**, not DVFS. Route: council with an
upstream-diff lens (cpuidle menu governor, genpd/pmdomain, regulator core, tsens,
smd-rpm clocks); do **not** start DVFS work.

### CP2 — Voltage path static correctness *(bench, no soak)*
ACUs: V1, V2, V3, plus X1/X3 below. **Accept:** (a) for every selector written, PMIC_STS
reads back the requested value, zero `timeout setting the voltage`; (b) the DUT's
speed/PVS/version read from the fuse is plausible and **matches an actual row** — i.e. the
generic `opp-microvolt` fallback is *not* silently in use; (c) each OPP's applied voltage
matches the vendor table for that bin (oracle) within a stated tolerance.
**Fail-bounded ⇒** fix row selection / bin gating before any soak. This gate is cheap and
can invalidate months of soaking, so it runs before CP3.

### CP3 — Frequency-only, isovoltage (clock path proven)
Variant BV-E, rail pinned high, thermal off, cpuidle on. **Accept:** full frequency sweep
stable, then ≥ **5 h** transition-storm (L3) clean.
**Pass ⇒** the clock path (F1/F2/F3, incl. the HFPLL lock fix) is sound; any remaining
instability is voltage/corner/thermal. **Fail ⇒** blame F1/F2 (PLL lock, mux switching,
L2) — a decisive and rarely-run isolation.

### CP4 — DVFS: frequency + voltage, CX pinned
Variant BV-B, thermal off, `V4` margin as configured. **Accept:** ≥ **21 h** idle (L1) and
≥ **5 h** sustained load (L2) clean, plus ≥ **5 h** L3. Zero voltage-timeout messages.
**Fail-bounded ⇒** V3/V4/R1. **Fail-unknown ⇒** council.

### CP5 — Interaction pairs
Every **E** cell in §7 gets its targeted run (short, aimed at the specific race; L3-style
storms where relevant). **Accept:** each pair passes its own criterion; specifically R3
(MX ≥ CX holds at the pinned corner, measured) and R5 (CX vote survives remoteproc
handover and an ADSP restart).
**Fail ⇒** fix at the responsible ACU, re-run CP4 (combinations invalidate the prior pass).

### CP6 — Thermal
ACUs T1–T6 with DVFS on. **Accept:** (a) tsens agrees with the pm8941 die ADC and the
oracle within a stated tolerance; (b) under sustained load the frequency *demonstrably*
drops at the passive trip and recovers, with no oscillation across the hysteresis band;
(c) the mitigation floor is at or above the vendor's lowest mitigation step; (d) the
critical path is verified by a controlled test, not by inference; (e) full L2 soak at
thermal steady state.
**Fail ⇒** T-layer; re-run CP4's L2 afterwards.

### CP7 — Full stack, release-grade
All ACUs default-on, BV-B, real workload. **Accept:** ≥ **3 weeks cumulative** clean
runtime including at least one continuous ≥ **48 h**, across L1–L4; regression sweep
(GPU submit with zero IOMMU faults, remoteprocs up, QRTR clean, network) per the project's
existing suite; zero unexplained boots in `journalctl --list-boots` (using per-boot tails,
not the skewed timestamps).
**Fail ⇒** back to the implicated layer.

### CP8 — Promotion
Merge topics → staging → rc per the project's flow; on-device validation before rc; tag on
release. Split out the genuinely upstreamable fixes (the HFPLL lock-poll defect affects
msm8974, msm8976 a53/a72/cci and qcs404) as standalone patches with `Fixes:` provenance.

---

## 9. Run these first — cheap and decisive

Ordered by information per hour. X1–X3 need no soak at all.

| # | Experiment | Answers | Cost |
|---|---|---|---|
| **X1** | Enable dyndbg on `qcom-cpufreq-nvmem`; read the DUT's speed bin / PVS / version and which `opp-microvolt-speed*-pvs*-v*` row is chosen | Is the correct voltage row used, or the generic fallback? Is the DUT bin 3 (i.e. are the top 3 OPPs even legal)? | minutes |
| **X2** | Oracle: `cpuinfo_max_freq` + vendor table + whether it ever uses > 2265.6 MHz | Is the 2457.6 MHz ceiling an overclock for this part? | minutes |
| **X3** | Static voltage sweep: write each selector, read back PMIC_STS; watch for `timeout setting the voltage` | Is the gang-rail write path trustworthy at all? | ~30 min |
| **X4** | BV-E isovoltage frequency storm (CP3) | Clock path vs voltage path — the big fork in the road | ~5 h |
| **X5** | With CX pinned super-turbo, read CX corner and pm8841_s1 (MX) | Does MX ≥ CX hold? Did the CX fix create a new hazard? | minutes |
| **X6** | CP1: 6.18 with DVFS off | Is the LTS bump itself sound? Unblocks shipping. | ~21 h |

X1, X2, X3 and X5 are all quick and can be done in one sitting; X6 runs overnight in
parallel with nothing else (single DUT). If X1 or X2 shows a bin/row mismatch, fix that
*before* spending nights on soaks — it would make every previous voltage conclusion suspect.

---

## 10. Open items carried in from earlier sessions

| Item | Status | Where it lands |
|---|---|---|
| HFPLL inverted lock poll fix | Landed, unvalidated | CP3 (X4) |
| `hfpll_regmap_config` lacks `.fast_io` ⇒ `regmap_read` takes a mutex under `spin_lock_irqsave` | Hypothesis | BV-C debug run; `.fast_io = true` as a separate commit if it fires |
| MX ≥ CX with CX pinned | Hypothesis | CP5 / X5 |
| Top-OPP headroom vs 50 mV margin | Hypothesis | CP2 / X1 / X2, then CP4 |
| L2 rate → corner mapping (`qcom,l2-fmax` equivalent) possibly absent upstream | Suspected gap | R4, CP5 |
| cpuidle menu-governor rework changing SPC entry rate | Hypothesis, possibly moot | I1/I2, CP5 |
| WCNSS vddmx vote rejected (`s1: unsupportable voltage range`) | Real, separate track | Own topic; blocks nothing here but interacts with R3 |
| `notify-kernel-build` skips silently without `KERNEL_DISPATCH_TOKEN` | Real, infrastructure | Fix the guard to fail loudly; add the secret |
| Commit messages reference `arch/arm/configs/msm8974_defconfig` which does not exist in-tree | Discrepancy | Confirm the real config source (citronics-kernel / buildroot) at CP0; verify `QCOM_RPMPD`, `QCOM_HFPLL`, `KRAITCC`, `QCOM_SPM`, `ARM_QCOM_CPUFREQ_NVMEM` are actually enabled — the CX votes are inert otherwise |

---

## 11. Ledger and handoff

Keep `docs/reports/dvfs-campaign-ledger.md` in-repo, append-only, one entry per experiment
using the blueprint §5 record. Each session ends by updating: entries, the ruled-out table
(§2), the current checkpoint, the current anchor, and **the single next action**. Promote
discovered invariants (e.g. "Krait needs an explicit CX corner vote", "MX ≥ CX has no
aggregator on 8974") into `AGENTS.md`/`CLAUDE.md`, where durable project rules live.
