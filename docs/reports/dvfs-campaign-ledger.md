# DVFS/thermal campaign ledger — Fairphone 2 (msm8974pro)

**Append-only.** One entry per experiment/session action, blueprint §5 format
(`docs/porting/BLUEPRINT-kernel-feature-bringup.md`). Artifact root:
`~/Projects/msm8974-scratch/artifacts/`. Never edit past entries; append corrections.

---

## 6.12 DVFS series (SES3, 2026-07-31) — incremental ACU ladder

Anchor floor: `citronics-fp2-6.12-r1` (`69eea3a9f8f7`, no CPU DVFS, shipped, known
good). Each rung is ONE variable on the previous validated anchor, its own topic
off `6.12/baseline`, merged into a throwaway `6.12/int/<rung>` for the image, and
reverts to the last anchor on fail. Full port manifest:
`~/Projects/msm8974-scratch/DVFS-6.12-PORT-MANIFEST.md`. Ladder: D0 xpu-err-fatal
→ D1 clock plumbing → D2 isovoltage sweep (CP3) → D3 DVFS+voltage CX-pinned (CP4)
→ D4 thermal (CP6).

### D0 — xpu-err-fatal prerequisite *(pre-registered before run)*
- **Hypothesis:** arming TZ XPU (memory-protection) err-fatal on 6.12 is a
  no-op for the stable no-DVFS baseline (no frequency change), and is the
  prerequisite that makes later DVFS load survivable (6.18: unusable without it,
  resets 0.5–2 min under load).
- **Single variable vs r1:** +`6.12/topic/xpu-err-fatal` (2 commits, ported from
  6.18 b34e71475156 + 729ef9a8c17a; SCM-wrapper conflict resolved by keeping only
  the XPU block, dropping GPU-aperture funcs 6.12 lacks). Integration
  `6.12/int/d0` = `0ffbf4964e8b`; diff vs r1 = only the 3 xpu files (verified).
- **Criteria:** PASS = (a) no regression vs r1 — boots 3×, display+sensors+wlan+
  modem up; (b) XPU readback confirms armed (`qcom_scm.xpu_errfatal=2` reads
  enabled, or dmesg `xpu_err_fatal(config=…) -> 0`); (c) baseline stable under a
  brief 4-core busy-loop at the fixed boot freq. FAIL = any regression/reset.
  *(Load-survival BENEFIT is only measurable once DVFS drives higher freqs — D2+;
  D0 only proves no-regression + armed.)*
- **Result: PASS** (2026-07-31, battery FP2 e4f4c070). Caught a real port
  bug first: arming silently no-op'd because the fork's `late_initcall_sync`
  runs before `firmware:scm` probes — on 6.12 scm defers behind the
  `fc400000` GCC clock-controller (devlink confirmed), so `__scm` was NULL
  and the init bailed at `qcom_scm_is_available()`. The 6.18 ordering
  happened to work. **Fix:** arm from `qcom_scm_probe()` (helper, forward-decl)
  instead of a fixed-time initcall. After fix: `qcom_scm firmware:scm:
  xpu_err_fatal(config=0) -> 0` at t=0.97s, DT-driven, on **3/3 boots**;
  no regression (display/sensors/wlan/modem); survived 60s 4-core load +
  ~11 min continuous uptime, zero resets. Topic re-split into 2 clean commits
  (`d675bcd7` wrapper+probe-arm, `c90b48c4` DT); merged to `6.12/staging`
  (`12310d315c52`), pushed. **New anchor for D1.** (Load-survival benefit
  still unmeasured — needs DVFS; per pre-registration, D0 only claims
  no-regression + armed, both confirmed.)
- **Ledger note (method win):** the incremental gate turned an invisible
  "DVFS resets under load on 6.12" into a named, one-cause, fixed
  prerequisite bug — the whole point of not bulk-porting.

### D1 — Krait clock plumbing *(pre-registered before run)*
- **Hypothesis:** populating the Krait clock tree (per-CPU HFPLLs + L2 HFPLL +
  krait-cc-v2 controller) is stable on its own — the controller enables/locks
  the HFPLLs and sets up the secondary muxes at probe — even with cpufreq NOT
  driving frequency.
- **Single variable vs D0 anchor:** `6.12/topic/krait-clk` (7 commits: 5 clk
  driver fixes ported from 6.18, msm8974 hfpll_data, hfpll0-3/hfpll_l2/kraitcc
  DT nodes) + Kconfig `QCOM_HFPLL/KRAITCC/KRAIT_CLOCKS/KRAIT_L2_ACCESSORS`.
  **No** cpu_opp_table and **no** cpu `clocks=<&kraitcc>` wiring → cpufreq does
  not bind, CPUs stay at boot frequency. Integration `6.12/int/d1`
  (`3e2f96125a56`); diff vs D0 = only clk/hfpll/dtsi (verified).
- **Criteria:** PASS = boots, `clk_summary` shows the HFPLLs present and the
  krait-cc clocks registered, **zero `hfpll* failed to lock` WARN**, no
  regression (display/sensors/wlan/modem, 3 boots). FAIL = HFPLL-lock WARN
  (→ the `.l_val = 0x1c` seed becomes D1's attributed fix, cleanly isolated) or
  any reset/regression.
- **Deliberately excluded (l_val seed):** the uncommitted 6.18
  `.l_val = 0x1c` hfpll seed is NOT in D1's baseline — if the hfpll1 WARN fires
  (krait-cc enabling an unconfigured secondary PLL), that seed is the fix, and
  D1 will attribute it precisely instead of pre-applying an unproven patch.
- **Result: PASS after the pre-registered fix** (2026-07-31, battery FP2).
  First build surfaced exactly the predicted `hfpll1 failed to lock in 200 us
  (L_VAL 0)` WARN at `krait_cc_probe → krait_add_clks` — FAIL-BOUNDED, mechanism
  confirmed (krait-cc enables a secondary HFPLL before its rate is set; with no
  `.l_val`, `init_once()` writes no L register). Applied the `.l_val = 0x1c`
  seed (its own attributed commit, not pre-bundled). After fix, 3/3 boots:
  **zero WARN**, all 4 CPU HFPLLs locked at 960 MHz + L2 HFPLL at 729.6 MHz,
  no regression (display/sensors/wlan/modem/xpu). Topic `6.12/topic/krait-clk`
  (8 commits) merged to `6.12/staging` (`9fd16c1c049b`), pushed. **Anchor D2.**
- **Method note:** D1 predicted its own failure mode and fix in the
  pre-registration, then confirmed both — the l_val seed is now evidence-backed
  and committed rather than an unproven worktree experiment.

### D2 — isovoltage frequency sweep (CP3) *(design + pre-registration)*
- **Anchor:** D1 = `6.12/staging 9fd16c1c049b`. Boot CPU freq measured **960 MHz**
  (all 4 krait pri-muxes), stable → boot voltage supports ≤ 960 MHz.
- **Design finding (authority: `drivers/cpufreq/qcom-cpufreq-nvmem.c` on 6.12):**
  `match_data_krait` uses `.genpd_names = generic_genpd_names = {"perf", NULL}`
  and **no cpu-supply regulator** — 6.12 models krait perf via a genpd +
  `required-opps`, not `regulator_set_voltage`. So scaling CPU frequency does
  NOT drive the APC/SAW2 rail directly; mainline krait on msm8974 is effectively
  isovoltage already. The fork (6.18) instead attaches `"cx"` (rpmpd corner).
  **This is a real wiring fork to resolve before flashing anything that sets CPU
  frequency** (which "perf"/"cx" domain the CPU OPP votes; how the SPM gang-rail
  in D3 later adds real APC voltage scaling to exceed the boot envelope).
- **Hypothesis:** frequency scaling across 300–960 MHz (within the boot-voltage
  envelope, no APC voltage change) is stable — proving the clock path
  (krait-cc mux switching + HFPLL rate changes) independent of the voltage path.
- **Planned single variable vs D1:** a freq-only CPU OPP table capped at
  960 MHz (opp-hz + required-opps CX-pinned super_turbo; NO opp-microvolt) +
  cpu `clocks=<&kraitcc N>` + `operating-points-v2` + the CX genpd attach.
  Rail never driven → stays at boot voltage → safe.
- **Criteria (to pre-register at run):** full 300–960 sweep stable, then a
  transition-storm (L3) clean; zero HFPLL WARN; no regression. Also reads X1
  (DUT speed bin/PVS via the nvmem driver once it binds).
- **Status: design open** — pausing before implementing the CPU-frequency DT to
  settle the perf-vs-cx genpd wiring correctly (voltage-safety); NOT rushing an
  undervolt/overvolt config onto the device at the end of a long session.
  Next session resumes here with the genpd/required-opps design.

- **D2 IMPLEMENTED + RESULT (2026-07-31): clock path PASS, storm FAIL-UNKNOWN.**
  Integration `6.12/int/d2` (`9acda161090a`), one-variable diff vs D1 (OPP DT +
  cpufreq match_data). Wiring: 6.12 `match_data_krait` has NO genpd/cpu-supply
  (pure clock); added `match_data_msm8974` attaching the rpmpd `"cx"` domain.
  - **Clock path PASS:** cpufreq exposes the capped 300–960 MHz table; sweep
    confirmed krait-cc REALLY reprograms the hardware (set 300→mux 307 MHz, set
    960→hfpll0 960 MHz, set 422→hfpll0 844.8 MHz÷2); CX corner pinned at perf
    state 6 (super_turbo), `cx_ao on 6` held by cpu0 — genpd+required-opps works;
    zero HFPLL WARN.
  - **Transition-storm FAIL:** an aggressive detached storm (~400 transitions/s)
    ran ~2500 iters (up 190→196 s, warn=0) then **reset the SoC** — on battery
    (UVLO excluded), isovoltage, CX pinned. So **rapid clock transitions alone
    destabilise the SoC, independent of voltage scaling** — a major narrowing
    (the isovoltage isolation did its job).
  - **UNATTRIBUTED — observability gap:** the storm reset was *harder* than a
    normal reboot — ramoops did NOT preserve the dead boot's console (DRAM lost;
    contrast D0's warm reboots, which preserved it), and 6.12 does not print the
    PON reason (the fork's `pon-reason` topic is NOT ported). So warm/PS_HOLD vs
    hard vs watchdog is unknown. Per blueprint §6, **STOP — do not patch a guess.**
  - **Verdict: FAIL-UNKNOWN.** D2 NOT merged; anchor stays D1. Device runs the
    D2 image (stable at normal governor; only the pathological storm resets).
- **Next action (back to P1 — observability first):** port the reset-reason
  instrument to 6.12 before any more DVFS — the fork's `pon-reason` topic (PON
  reason print at boot) so a storm reset is attributable, and verify ramoops
  captures a storm reset (may need the reset to be warm/recoverable). Only then
  re-run the D2 storm for an attributable verdict, and throttle the storm to a
  realistic rate as one axis (400/s is pathological; a governor-realistic storm
  is the real target). This mirrors the O-layer gate the ladder skipped on 6.12.

- **O2 ported + D2 storm RE-RUN (2026-07-31, corrected):** ported the fork's
  `pon-reason` reporter to 6.12 (`6.12/topic/pon-reason`; adapted the
  6.18 `qcom_pon`→6.12 `pm8916_pon` rename). Confirmed working: boot now prints
  `pm8916-pon … previous power-off: <reason>`. Rebuilt D2+O2 integration
  (staging + krait-cpufreq + pon-reason). **The earlier storm reset did NOT
  reproduce:** a *confirmed* inline storm did **105,360 transitions at ~1170/s
  over 90 s with zero HFPLL WARN and no reset**. So the one-off reset was
  non-deterministic (early-boot timing / fluke), not a deterministic clock-path
  failure — the isovoltage clock path is robust under transition stress. (The
  first "survived 120 s" was a false pass: the detached storm had silently died;
  caught by checking the governor + empty log. Now launched as a verified
  `start-stop-daemon`.)
- **D2 status: clock path PASS; long storm-soak RUNNING** (target ≥ ~75 min to
  exclude the ~19 min-MTBF class at 95%, per §7) with O2 armed to attribute any
  reset. On clean soak → D2 PASS, merge to staging as the D3 anchor. Any reset →
  O2 prints the PON reason at recovery (PS_HOLD vs watchdog vs UVLO) → bounded.

---

## State of knowledge (seed, from PLAN §2 — confirmed 2026-07-30)

**Ruled out, with evidence (do not re-litigate without NEW evidence):**

| Suspect | Evidence it is not the cause |
|---|---|
| rpmpd CX switch (`7d004ab4e`) | cherry-pick by Luca Weiss (Jan 2025); byte-identical wiring on stable 6.16 baseline and 6.15 |
| GPU / non-secure IOMMU | resets occur with GPU node disabled, and on display DTB where GPU is `disabled` |
| ModemManager SEGV storm | mitigated (1.23.4 + hold + RestartSec=30); was a CPU-load trigger, not root cause |
| SPM write-path defects (PORT-clear, vlevel encoding, PMIC_STS poll) | all three fixed and present |

**Found and fixed (effect measured):** CX corner starvation for Krait — graded corners
took idle lifetime 19 min → 95 min → 6 h 40+; then pinned super-turbo. The re-graded
"vote by rate" (`11c9727035d8`) re-introduced ~19 min idle PS_HOLD MTBF in the field;
reverted by `1a41629d4758` (see its commit message for the full evidence chain).

**Fixed, effect NOT yet measured on device:** inverted HFPLL lock poll (`clk-hfpll.c`),
in the rc tree. Validation lands at CP3/X4.

**Uncommitted, preserved, NOT in any tree:** hfpll `.l_val = 0x1c` seed
(`~/Projects/msm8974-scratch/preserved/hfpll-lval-seed-uncommitted-20260730.patch`);
was part of the `-dirty` #17 kernel the DUT ran on Ubuntu. Decide: commit to a topic
branch or drop, before CP3.

---

## SES1 / 2026-07-30 / session 1: bench restore + campaign bootstrap

### SES1-A — DUT Ubuntu-image evidence salvage (pre-reflash)
- **Manifest of the salvaged system:** Ubuntu 26.04, kernel `6.18.40-g1a41629d4758-dirty`
  #17 (= topic head + l_val seed), DTB `qcom-msm8974pro-fairphone-fp2-6.18.dtb`,
  hand-swapped via extlinux on `mmcblk0p20p1` — monolithic, **no modules**.
- **Result:** artifacts + full analysis in
  `~/Projects/msm8974-scratch/artifacts/fp2-dut/20260730-preflash-ubuntu/`
  (`SALVAGE-SUMMARY.md`). Highlights: 9-boot history — 2× **UVLO** power-offs
  (carrier brownout signature), 1× **PS_HOLD** power-off (ambiguous: soft reboot vs
  silent reset), boots -7/-6 died in early boot after SMPL power-ons;
  **zero** `timeout setting the voltage` in the whole journal; RCU "expedited stall"
  INFOs (10) all belong to the **6.16.12** boot, not 6.18; live genpd shows the pinned
  config holds `cx_ao` at perf state 6 (votes land on `cx_ao`, not `cx`).
- **Ledger updates:** the known-stable 6.16 evidence and all recent 6.18 test evidence
  were gathered on **Ubuntu** userspace → CP1 must re-baseline on buildroot.

### SES1-B — branch-state correction (supersedes the session briefing)
- `6.18/rc` = `ac185c36cc06`, `6.18/staging` = `f21a650e2e04`, trees **identical**;
  both include `xpu-err-fatal` AND `11c9727035d8` "vote the CX corner by rate"
  (the ~19 min idle-MTBF configuration).
- `6.18/topic/cx-corner-idle-reset` = `1a41629d4758` (rc tree + revert to pinned CX);
  pushed to origin 2026-07-30. **Reconciling the revert into staging/rc is open work.**

### SES1-C — oracle probe (read-only; oracle untouched)
- **Manifest:** `utils/oracleprobe.sh` (reviewed read-only before run), oracle
  FP2 #1, Android 10 `23.02.0-rel`, kernel `3.4.113-perf-ge8679ce1538`, root OK.
- **Result:** `~/Projects/msm8974-scratch/artifacts/fairphone2/oracle/20260730T191748Z/`;
  profile installed at `<BR2_EXTERNAL>/board/fairphone2/target-profile.env`.
  - **X2 answered:** vendor `cpuinfo_max_freq` = **2 265 600 kHz**. The 2342.4/2419.2/
    2457.6 MHz OPPs (bin-3-only) are never used by the vendor stack on this model.
    With the DUT die = `speed1-pvs12-v1` (prior session, re-verify on vehicle at X1),
    2265.6 MHz is the ceiling on both counts.
  - **Vendor OPP set:** 14 frequencies (`target-profile.env: VENDOR_FREQ_TABLE`) vs
    **31** in our mainline DT — mainline makes ~2× the transition stops.
  - **Vendor thermal:** thermal-engine = per-CPU **shutdown at 115 °C** only;
    frequency mitigation is kernel msm_thermal: **starts at 60 °C**, hysteresis 10,
    freq-step 2, core-offline 80 °C, therm-reset 115 °C
    (`fairphone/rel/10/fp2/22.08.0-rel:arch/arm/boot/dts/msm8974.dtsi`).
    Mainline DT's 90 °C passive trip is 30 °C above vendor mitigation start.
  - **Idle rails (display on):** CX corner request 5 with `_ao`=5; MX 675 mV;
    `krait-power-regulator/retention_uV` exists (raw/regulator_debugfs_3p4).
  - Oracle has `/proc/last_kmsg` (RAM console) — vendor's own silent-reset evidence
    channel; our equivalent is lk2nd-relocated ramoops.
- **Vendor tree validation (LAB-OPS §2.4): PARTIAL.** `-ge8679ce1538` not resolvable:
  FairphoneMirrors ends at `rel/10/fp2/22.08.0-rel` (< oracle's 23.02). Adopted 22.08
  head as register-level reference; pmaports' LineageOS pin `284400aea4b9` is an
  ancestor of it. Details in `<BR2_EXTERNAL>/board/fairphone2/references.md`.

### SES1-D — reference recon
- pmaports (unshallowed, blobless): FP2 and all 12 sibling msm8974 devices are
  **testing** maturity; SoC kernel = msm8974-mainline `v6.15.11-msm8974`, and the
  known-good community config ships **without** `QCOM_HFPLL`/`KRAITCC`/
  `ARM_QCOM_CPUFREQ_NVMEM` (and without PSTORE) — postmarketOS runs msm8974 with **no
  CPU DVFS**. Independent support for the CP1 ship-without-DVFS strategy.

### SES1-E — vehicle image build (this session's deliverable)
- **Hypothesis:** the buildroot image built from the exact tree with the most on-device
  evidence (`1a41629d4758`, pinned CX) boots and serves as the campaign vehicle;
  the single deliberate change vs the salvaged evidence base is the userspace stack
  (Ubuntu → buildroot).
- **Manifest:** kernel `file:///var/home/marc/Projects/linux-msm8x74` @
  `1a41629d475842fb5033a0a9ba61fdc9c84299b8`; BR2_EXTERNAL commit: dirty (see
  `~/Projects/msm8974-scratch/preserved/buildroot-fp2-dirty-20260730.patch` for the
  pre-session state); defconfig `fairphone2_defconfig` + this session's deltas:
  1. `BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION` branch ref → **full SHA** (LAB-OPS §7.3);
  2. `BR2_PACKAGE_REBOOT_MODE=y` (was never enabled on FP2 — no image ever had a
     userspace path to fastboot);
  3. `linux.config`: `SYSCON_REBOOT_MODE` m→**y** (recovery-critical),
     `PSTORE/PSTORE_RAM/PSTORE_CONSOLE/PSTORE_PMSG=y` (mirrors the validated
     citronics-kernel `msm8x74-6.18-rc.config`) — the in-DTS ramoops node was inert
     in every previous buildroot image;
  4. overlay extlinux cmdline += `lk2nd.pass-ramoops` — **required**: lk2nd relocates
     the ramoops node to its own scratch region, the only region its shell `pstore`
     command and `fastboot oem ramoops console` read (lk2nd/ramoops/ramoops.c);
     confirmed composition in salvaged dmesg (node named `ramoops@ff00000`, reg
     rewritten to `0x30f80000`).
- **Criteria (pre-registered):** §8 image checks all pass before flash; after flash:
  boots to userspace, serial console live, SSH at 10.0.42.1, `uname -r` matches
  `/lib/modules`, `reboot-mode bootloader` lands in fastboot, kernel-tests runnable.
  This is a CP0 bring-up gate, not a stability claim — **no soak credit**.
- **Result (carrier board):** image booted cleanly to userspace, then entered a
  boot→reset loop: silent reset each cycle at WCNSS remoteproc power-up (~6.5 s),
  next boot's PON = **UVLO** — caught LIVE on serial (`scratchpad/serial-snap.txt`,
  archived in artifacts). Confirms the carrier's direct-VBAT feed cannot survive the
  WCNSS inrush; **the carrier bench is retired as a validation vehicle**. Bonus
  on-device evidence: `WARNING … __clk_hfpll_enable: hfpll1 failed to lock in 200 us
  (L_VAL 0)` — exactly the defect the preserved `.l_val = 0x1c` patch addresses
  (and proof the rc HFPLL lock-poll fix detects real lock failures).
- **DUT swap (operator):** carrier board retired; new DUT = **real FP2 + battery**,
  fastboot serial `e4f4c070`, lk2nd installed, **no UART**. Different die: X1
  (speed/PVS/version) re-opens for this unit.
- **Result (real FP2, BV-B image):** boots clean; `uname -r`=6.18.40=modules;
  lk2nd honors `pass-ramoops` (region relocated to `0x30f80000`, console live);
  PON correctly classifies a `fastboot reboot` as PS_HOLD; all 3 remoteprocs
  running **on battery** (no UVLO — carrier exonerated the image); HFPLL WARN
  reproduces; `scaling_available_frequencies` = exactly the vendor 14-entry table,
  ceiling 2265.6 MHz (bin gating produces Android parity on this die);
  kernel-test-iommu 12/12 with GPU submit.

### SES1-F — BV-A no-DVFS baseline built, flashed, validated (operator-directed)
- **Decision (Marc):** the standing campaign baseline must be the S0 shape —
  6.18 with DVFS gated off (CP1 control) — not the DVFS tree. BV-B image preserved
  for later at `~/Projects/msm8974-scratch/preserved/sdcard-1a41629d4758-dvfs-BVB-20260730.img`.
- **Manifest:** same kernel SHA `1a41629d4758`; variant `fairphone2_nodvfs_defconfig`
  + `board/fairphone2/linux-fragments/nodvfs.fragment` (`QCOM_HFPLL`, `KRAITCC`,
  `ARM_QCOM_CPUFREQ_NVMEM` off — the postmarketOS community shape); new package
  **soak-logger** (fsync'd 30 s sampler, starts at boot, `/var/log/soak/soak-boot-<bootid>.log`);
  full manifest `MANIFEST-BVA-20260730.env` (sdcard md5 recorded there).
- **Result:** §8 checks pass; boots on the real FP2; **no cpufreq policies** (DVFS
  confirmed off); zero hfpll dmesg lines; 3 remoteprocs up; kernel-test-iommu 12/12;
  soak-logger sampling from boot (temps 37–50 °C idle); **ramoops console SURVIVES
  a warm PS_HOLD reboot** (`console-ramoops-0`, 22 KB, previous boot's full log) —
  the read side just needs `mount -t pstore pstore /sys/fs/pstore` (busybox-init
  never mounts it; fstab fix added to the overlay for the NEXT image build — the
  running image needs the manual mount, kernel-side logging unaffected).
- **Open (FAIL-BOUNDED):** `reboot-mode bootloader` performs a plain reboot instead
  of entering fastboot. Driver binds (`fe805000.sram:reboot-mode`), DT writes
  `0xFE805000+0x65C`, and lk2nd's 8974pro path reads the SAME address
  (`lk2nd/target/msm8974/init.c: check_reboot_mode`, `RESTART_REASON_ADDR_V2`) —
  so the address matches; the failure is either the syscon write not landing
  (XPU on IMEM? — vendor kernels write it, so writes are possible) or stock aboot
  scrubbing/ignoring the value before lk2nd. Diagnose next session (safe probe:
  regmap debugfs read of 0x65c, NEVER /dev/mem). Until fixed: flashing needs the
  bench key-combo (operator present).

### SES1-G — CP1 idle soak STARTED (X6)
- **Hypothesis:** 6.18 base (this fork's tree) without DVFS is stable — the LTS bump
  alone is not implicated.
- **Single variable vs BV-B:** the DVFS driver set (config fragment); same SHA,
  same DTB, same userspace.
- **Manifest:** `MANIFEST-BVA-20260730.env`; running boot began ~19:48 UTC 2026-07-30.
- **Criteria (pre-registered, PLAN CP1):** ≥ **21 h** continuous clean, idle +
  light load; telemetry complete to the last sample; zero unexplained boots
  (per-boot journal tails + PON reasons + ramoops, not wall-clock timestamps).
  Soak arithmetic: 21 h excludes MTBF ≥ ~7 h at 95 % (blueprint §7).
- **Result (2026-07-30 ~20:35Z): FAIL — reset after ~47.5 min idle.**
  Evidence (in `~/Projects/msm8974-scratch/artifacts/fp2-dut/20260730-cp1-reset1/`):
  - PON on the next boot: **previous power-off PS_HOLD (MSM-controlled)**,
    `warm_reset=0x0002`, power-on "hard reset"/USB-charger — on battery, so NOT
    UVLO, NOT external power.
  - **console-ramoops-0 captured the dying boot** (24.8 KB — the harness's first
    read silent reset): normal boot activity ends t=33 s (`l7: disabling`), then
    silence until exactly one line, **`l24: voltage operation not allowed` at
    t=2849.8 s**, then reset. l24 = pm8941 LDO24 (3.075 V, `regulator-boot-on`,
    supplied from `vreg_boost`). Someone attempted a voltage set on l24 at that
    moment and was denied by constraints; the reset followed. Same denial message
    appears benignly at t≈2.9 s during boot on other runs — the actor, not the
    message, is the lead (suspects: qcom-smbb charger/USB event path, given the
    USB-charger PON context).
  - **Instrument defect found:** the dying boot's soak telemetry was LOST —
    buildroot mounts `/var/log` on tmpfs, so soak-logger's fsync went to RAM.
    Fixed: LOG_DIR → `/root/soak` (rootfs).
  - **Verdict: FAIL-UNKNOWN** (mechanism not established; one evidence thread).
    Consequence: the *fork* 6.18 tree resets at idle **without the DVFS stack**
    — DVFS drivers are not the sole cause. Prior CP1 framing is superseded.
  - **Routing (operator decision, Marc):** deepest isolation next — a **vanilla
    ground truth** with zero fork patches (below).

### SES1-H — CP1-v: vanilla 6.18.39 ground truth (pre-registered)
- **Hypothesis:** pure stable v6.18.39 — the fork's base commit, zero fork
  topics — in the postmarketOS shape (no CPU DVFS) is idle-stable on this DUT.
  Falsified by: a PS_HOLD idle reset on this image.
- **Baseline selection:** first-parent history of `6.18/staging` shows the FIRST
  topic ever merged was `6e6b3f338e45` (dvfs-spm), directly on
  **`f89c296854b755a66657065c35b05406fc18264d` ("Linux 6.18.39")** — so "the
  commit right before any DVFS topic" is the pure stable base itself. No
  pre-DVFS state containing adsp-sensors exists (they merged later); per
  operator instruction they are left out rather than fabricating a tree.
- **Single variable vs BV-A:** the fork patch set (removed). **Known second
  delta, unavoidable:** DTB is the in-tree *display* variant (headless DTS is a
  fork addition) — display/MDSS active is an environmental difference to keep
  in mind when comparing; if vanilla proves stable, re-adding fork topics under
  the same display DTB isolates cleanly.
- **Manifest:** `fairphone2_vanilla618_defconfig` (nodvfs fragment kept; overlay
  override boots `qcom-msm8974pro-fairphone-fp2.dtb`; `lk2nd.pass-ramoops`
  INJECTS the ramoops node — vanilla DT has none); kernel release 6.18.39;
  soak-logger now writes to `/root/soak`.
- **Criteria (pre-registered):** same as CP1 — ≥21 h continuous clean idle,
  telemetry complete, zero unexplained boots. Any PS_HOLD reset = FAIL and,
  because the tree is pure stable, implicates base/config/environment rather
  than fork patches.
- **Result: soak STARTED 2026-07-30 ~22:00 UTC** after full validation: kernel
  6.18.39 = modules dir, zero cpufreq policies, lk2nd successfully **injected**
  the ramoops node (`ramoops@30f80000` — vanilla DT has none), console→ramoops
  live, all 3 remoteprocs running on pure stable, soak-logger sampling to
  `/root/soak` (rootfs), idle temps 37–53 °C. *(soak outcome pending)*
- **Interim read 2026-07-31 morning (read-only, soak undisturbed):** single
  boot-id, uptime 30 645 s ≈ **8.5 h continuous**, 1019 unbroken samples,
  temps 37–48 °C, zero resets. P(8.5 h clean | fork's ~47 min MTBF) ≈ 2×10⁻⁵ —
  the fork tree's idle-reset mechanism is effectively absent on pure v6.18.39.
  Gate remains the pre-registered 21 h (~19:00 UTC). Note: overnight the phone
  ran on an external supply with NO host link; the console-ramoops record
  recovered this morning (17 KB, a boot cut at t=0.64 s) is the interrupted
  boot from last night's flash entry, archived in
  `~/Projects/msm8974-scratch/artifacts/fp2-dut/20260731-cp1v-interim/`.
- **FINAL (2026-07-31, soak ended early by operator direction): PASS-CAVEAT.**
  **8.7 h continuous clean** (uptime 31 368 s, 1043 gap-free samples, max load
  0.71, temps ≤ 54 °C). Caveat: 8.7 h < the pre-registered 21 h — bounds
  MTBF > ~2.9 h at 95 %, and excludes the fork's ~47 min mode at
  P ≈ 2×10⁻⁵. Stopped to build the operator-directed release baseline (SES2-A).
  Archived: `~/Projects/msm8974-scratch/artifacts/fp2-dut/20260731-cp1v-final/`.
- **KEY FINDING — the l24 event is benign on vanilla:** `l24: voltage operation
  not allowed` fired at t=649 s AND t=30 507 s during the clean soak; the
  system survived both. On the fork tree the same message was the last line
  before the PS_HOLD death (SES1-G). So the actor (USB/charger event path —
  phone was on a charger overnight) is routine; the fork differs in what
  follows. **Top-ranked new hypothesis: the fork's `xpu-err-fatal` topic arms
  TZ XPU err-fatal, converting an (otherwise silently-demoted) XPU violation
  on that event path into a TZ-initiated PS_HOLD reset.** Disproof experiment
  (cheap, single-variable): vanilla v6.18.41 + ONLY the two xpu-err-fatal
  commits → if it resets at a charger/l24 event, hypothesis confirmed; if it
  soaks clean, refuted. Pre-register before running.

---

## SES2 / 2026-07-31 / session 2: release baseline

### SES2-A — ultimate baseline: v6.18.41 + adsp-sensors (operator-directed)
- **Hypothesis:** latest LTS (v6.18.41) plus ONLY the adsp-sensors topic is
  idle-stable (the release shape: LTS security current, sensors working, no
  DVFS, no other fork topics — notably NO xpu-err-fatal arming).
- **Tree:** new branch `6.18/baseline` = `02d768689518eb854a72d9fd49c89182b1ebb1b7`
  (merge --no-ff of `6.18/topic/adsp-sensors` @ 4df870bacbe8, 16 commits based
  on v6.18.39, onto stable `2fe596715f84` "Linux 6.18.41"). Clean merge; the
  build is the compile-drift check. Topic includes `7d004ab4e69d` (rpmpd
  power domains) — already ruled out as a reset cause with evidence.
- **Two deltas vs CP1-v** (accepted, operator call): stable .39→.41 and the
  adsp topic. If this baseline fails its soak, bisect between those two.
- **Manifest:** `fairphone2_baseline_defconfig` (same config lineage: nodvfs
  fragment, display DTB, lk2nd-injected ramoops, soak-logger→/root/soak).
- **Criteria (pre-registered):** ≥ 21 h continuous clean idle+light-load,
  gap-free telemetry, zero unexplained boots ⇒ baseline blessed as the
  release candidate lineage; longer accumulation (CP7-style) continues on it.
  Sensors acceptance at flash time: qcom_smgr discovers the LSM330D accel/gyro
  + AK8963 mag in dmesg, adsp remoteproc running.
- **Result: flashed and validated 2026-07-31; RELEASE SOAK RUNNING** (soak-log
  epoch 1785480862). Validation: 6.18.41 = single modules dir; **display DTB
  only** (operator requirement — no headless artifact in image, extlinux `fdt`
  verified pre-flash); zero cpufreq policies; ramoops registered at
  0x30f80000 with console mirroring; **sensors PASS** (qcom_smgr discovered
  LSM330D accel + gyro, AK8963 mag at t≈7.6 s); all 3 remoteprocs running;
  soak-logger sampling to /root/soak. `6.18/baseline` and the topic pushed to
  origin. *(soak verdict pending — gate ≥ 21 h)*
- **Instrument caveats on vanilla:** the PON-reason dmesg prints are a FORK
  patch (`6.18/topic/pon-reason`) — vanilla logs no power-off reason at boot.
  Evidence chain for this soak = ramoops console + fsync'd telemetry; PON
  registers remain readable post-hoc via spmi regmap debugfs if a reset occurs.
- **Access notes:** the day's SSH key-auth failures (incl. the BV-A
  "authorized_keys lost" scare in SES1-G) were all workstation-side: the
  operator ssh-agent died and the personal key is passphrase-locked. Campaign
  key (passphrase-less): `~/Projects/msm8974-scratch/preserved/dut_ed25519`,
  installed for root@10.0.42.1. Operator reports `reboot-mode bootloader`
  DID enter fastboot when run from his console (contradicts SES1-F's failed
  poll — retest properly before trusting either way).

---

### SES2-B — infrastructure + findings (2026-07-31)
- **stable-sync workflow extended** to also merge `linux-6.18.y` into
  `6.18/baseline` (commit `dc3e699a0109` on `6.18/staging`, the default branch
  the scheduled workflow runs from; push operator-approved). Baseline stays
  security-current with no other topics.
- **Display root cause (operator question):** the black screen on display-DTB
  6.18 kernels is NOT config — the built kernel has the full pipeline
  (`DRM_MSM/MDP5/DSI/28NM_PHY/fbcon` all =y). The FP2 **panel node and its
  `fairphone,fp2-panel` driver are out-of-tree patches in msm8974-mainline**
  (verified present in `qcom-msm8974-6.15.y` of `~/Projects/linux-msm8974-upstream`,
  absent from upstream v6.18.41 AND from every fp2.dts in this fork) — the
  6.16-era deb kernels descended from that lineage; the 6.18 fork never ported
  the display topic. Fix = port as `6.18/topic/fp2-panel` (DT node + panel
  driver chain per the porting rules), soak as its own rung.
- **reboot-mode RESOLVED (operator-confirmed):** `reboot-mode bootloader` does
  enter fastboot; usage is fire-and-forget (the command blocks the SSH pipe —
  issue with a timeout, then poll `fastboot devices`). SES1-F's FAIL was a
  false negative from the blocking pipe + poll interaction.

### SES2-C — display bring-up on 6.18 (the day's main effort)
Operator requirement: no baseline without 6.16 feature parity, i.e. the screen
must work. Chain of root causes found and fixed, each from the vendor tree
(authority 1) once the wall protocol was applied:

| # | Root cause | Fix | Verified |
|---|---|---|---|
| 1 | Panel driver + DT never ported to 6.18 (out-of-tree in msm8974-mainline) | `6.18/topic/fp2-panel`: generated OTM1902B/S6D6FA1 drivers (6.16.y regeneration — the 6.15 ones use removed `mipi_dsi_*_write_seq` APIs) + DT display wiring, GPU enable deliberately excluded | panel binds, backlight DCS answers |
| 2 | Display controller deferred forever: `qnoc` interconnects fail `-ENOENT` without the msm8974 RPM bus clocks | merged `6.18/topic/smd-rpm-clocks` into baseline | interconnects probe, MDP binds |
| 3 | `mmss_s0_axi_clk` "stuck at off" hang | cherry-picked the MMSSNOC-parent fix with a proper message (vendor lineage `e604a98f5daf`) | boot survives display init |
| 4 | **TE never enabled** — vendor injects DCS 0x35 from the *mdss framework* (`qcom,mdss-dsi-te-dcs-command`), so it exists in no panel blob and no generated driver | `set_tear_on(VBLANK)` in both panels' `on()` | `pp done time out` stops recurring; flushes complete at ~30 fps |
| 5 | Panel rails unclaimed by the panel driver | `vdd`(l22)/`vddio`(l12) bulk-enable + DT supplies | rails held with consumers |
| 6 | **LPAE page tables on an SMMU that only implements V7S** — vendor programs `TTBCR=0` + short-descriptor + PRRR/NMRR on every 8974 MMSS SMMU | `ARM_V7S` format for the secure msm8974 instance (TTBR0 32-bit, ASID in CONTEXTIDR) | CB dump reads vendor-identical: `SCTLR=000010eb TCR=0 FSR=0` |
| 7 | Vendor BFB/prefetch block (18 impl-defined globals) never programmed | write the vendor register/value pairs after `restore_sec_cfg` | "programmed 18 BFB registers" at boot |
| 8 | Fork's IOMMU port keyed 3 IP quirks + the TZ gate on `nonsecure`, breaking any secure instance (incl. silently skipping `restore_sec_cfg` → raw PAs on the bus) | re-keyed on the `qcom,msm8974-iommu` compatible / `sec_id` presence; `INTR_SEL_NS` write made unreachable on 8974; runtime-PM forbidden for the GDSC-backed secure instance | boots, attaches, zero faults |

**Result: STILL BLACK.** Everything verifiable is nominal — panel initialized and
backlit, DSI link at 1080p rates, TE pulsing, MDP5 committing/flushing, IOMMU
attached with vendor-identical CB state, zero context faults, BFB programmed.
Writing 8 MB of noise into `/dev/fb0` produces no visible change, so the panel
is not displaying our framebuffer at all. A no-WCNSS variant (isolation build)
boots stable with the same black screen ⇒ two independent open items:
(a) scanout produces no pixels despite clean translation; (b) WCNSS bring-up +
active display DMA = silent reset/bootloop (only with display enabled).

**Verdict: FAIL-UNKNOWN** on display scanout. Per blueprint §6 the campaign
stops iterating and records the state of the art.

### SES2-D — why this is hard: the upstream record (authority 3/5)
- The carveout was removed **because of us**: Rob Clark, `eab7766c79fd` —
  *"standing in the way of drm_gpuvm / VM_BIND support … frequently broken and
  rarely tested. And I think only needed for a 10yr old not quite upstream SoC
  (msm8974). Maybe we can add support back in later, but I'm doubtful."*
  The no-IOMMU hard-fail (`c94fc6d35685`) is a consequence of the drm_gpuvm
  conversion. ⇒ a fork-carried carveout revert is a permanent fight with
  VM_BIND churn, not a one-time port (confirmed: reverse-applying conflicts in
  every touched file).
- **Nobody has solved the IOMMU path.** In the removal thread Luca Weiss (FP2's
  own mainline maintainer) states he and Matti Lehtimäki have *"a semi-working
  branch but hitting random issues with it"* and that *"nobody who really knows
  GPU and IOMMU bits has looked at this in recent years"*; Dmitry Baryshkov:
  *"MSM8974 is quite upstream, but anyway, let's drop it."* No replacement was
  proposed. msm8974-mainline's newest branch is `qcom-msm8974-6.16.y` and
  pmaports ships 6.15.11 — **the whole family has never crossed this removal.**
  Our result today (translation clean, scanout black, random resets) reproduces
  their "semi-working with random issues" independently.
- **Third path, fully supported in 6.18, no carveout and no SMMU:** simpledrm
  on lk2nd's own framebuffer. `CONFIG_DRM_SIMPLEDRM` exists (currently `=m` in
  our config) and lk2nd already emits a `simple-framebuffer` node with a
  `no-map` `/reserved-memory` region behind `lk2nd.pass-simplefb`
  (`lk2nd/display/simplefb.c`). Gives boot log + console + a framebuffer for
  userspace with zero fork burden; does **not** give KMS (no mode set, no
  GPU→panel compositing, no DRM panel power control) ⇒ not full 6.16 parity.

### SES2-E — pivot to 6.12 LTS baseline (operator decision), display + parity achieved
After 6.18 display proved unsolved (SES2-C/D), operator chose 6.12 as the
production baseline: same LTS EOL (Dec 2028) but drm/msm still has the vram
carveout there, so display works the proven way.
- **`6.12/baseline`** (`3574d3a3652a`) = stable `linux-6.12.y` (6.12.100) +
  msm8974-mainline `qcom-msm8974-6.12.y` (39 commits: FP2 display DT, panel
  drivers, smd-rpm clocks, MMSSNOC fix, rpmpd, remoteproc single-PD) + docs.
  One trivial `qcom_wcnss.c` conflict (kept stable's upstreamed form).
  **On-device: display works first boot** — `[drm] using 192m VRAM carveout`,
  fbcon on panel, 3/3 clean boots.
- **`6.12/topic/adsp-sensors`** (`3a3a9eef042a`, 14 commits) = the sensors set
  ported from the 6.18 topic (which carries -x provenance to the 6.15
  originals). Base-drift fixes folded in (compile-caught, not cherry-pick
  visible): the `adsp`→`pas` rename absent on 6.12; the QRTR-bus commit's
  modpost `do_*_entry` contract and `device_find_child` const-match prototype
  are pre-const on 6.12.
- **`6.12/staging`** (`69eea3a9f8f7`) = baseline + adsp-sensors. **Tree verified
  byte-identical** to the hand-validated intermediate (`a809171a86d1`), so its
  on-device result stands without a reflash.
- **Validation 2026-07-31 (operator gate: 2–3 clean boots, no soak — kernel is
  production-proven, sensors non-destructive):** 3/3 clean boots, every boot:
  display (fbcon), sensors (LSM330D accel+gyro, AK8963 mag; **live data** via
  `adsp_sensors_demo` — accel Y≈8.9 m/s² gravity), WLAN (`wcn36xx` fw loaded,
  `wlan0`), modem (3 remoteprocs running, `wwan0` QMI+AT). Manifest
  `MANIFEST-612-STAGING-20260731.env`.
- **Verdict: PASS** — release-grade by operator's gate. Next: promote to
  `6.12/rc`/`6.12/release`, then DVFS + thermal on 6.12.
- Infra: stable-sync workflow now syncs `6.12/{baseline,staging,rc}` from
  `linux-6.12.y` and the 6.18 branches from `linux-6.18.y` (`a21574de7fdd`).

## Current state (updated 2026-07-31, release-baseline soak running)

- **Checkpoint:** SES2-A release soak RUNNING on `6.18/baseline`
  (`02d768689518` = v6.18.41 + adsp-sensors only; display DTB). Gate: ≥ 21 h
  clean → bless as release lineage. CP1-v closed PASS-CAVEAT (8.7 h clean).
  CP1 fork-no-DVFS remains FAIL-UNKNOWN with the XPU-err-fatal hypothesis
  top-ranked (disproof experiment pre-registered in SES1-H FINAL).
- **Images:** release baseline = `output-fp2/images/sdcard.img`
  (`MANIFEST-BASELINE-20260731.env`). Vanilla-6.18.39, BV-A, BV-B all
  preserved with manifests in `~/Projects/msm8974-scratch/preserved/`.
- **Single next action:** leave the release soak untouched ≥ 21 h; then read
  `/root/soak/` + pstore before anything else. On PASS: decide promotion
  (staging rebuild from `6.18/baseline`?) with the operator, run the
  xpu-err-fatal disproof next, and re-add topics one soak-rung at a time
  (~2.5 h rungs suffice against the 47-min mode).

## Previous state (end of session 1, superseded)

- **Checkpoint:** CP1 (fork tree, no DVFS) = **FAIL-UNKNOWN** after one ~47.5 min
  idle PS_HOLD reset (SES1-G) — the fork tree resets at idle without the DVFS
  drivers. **CP1-v (vanilla v6.18.39 ground truth) soak RUNNING since
  2026-07-30 ~22:00 UTC** (SES1-H). CP0 residue: induced silent-reset
  classification, reboot-mode retest (operator says it works; my poll said no),
  collector/verify/flash wrappers, X1 fuse read on this die, PON-reason reading
  on vanilla via regmap.
- **Images:** CP1-v vanilla = current `output-fp2/images/sdcard.img`
  (`MANIFEST-VANILLA618-20260730.env`). BV-A fork-no-DVFS and BV-B fork-DVFS
  preserved in `~/Projects/msm8974-scratch/preserved/` with manifests.
- **DUT:** real FP2 + battery, `e4f4c070`, lk2nd, no UART. SSH key:
  `~/Projects/msm8974-scratch/preserved/dut_ed25519` → root@10.0.42.1.
  Carrier board retired.
- **Single next action:** leave the CP1-v soak untouched ≥ 21 h (until
  2026-07-31 ~19:00 UTC); next session starts by reading `/root/soak/`,
  pstore (`mount -t pstore pstore /sys/fs/pstore`) and — if a reset occurred —
  PON registers via regmap debugfs, BEFORE anything touches the device.
  - CP1-v clean ⇒ the fork's own topics (or the headless DTB / fork DT changes)
    are implicated → bisect by re-adding topic groups on the vanilla base.
  - CP1-v resets ⇒ pure stable 6.18 + this config/environment is implicated →
    council with upstream-diff lens (6.16→6.18) before any fork work resumes.

---

# SESSION 3 (2026-07-31) — 6.12 DVFS: D2 attributed FAIL-BOUNDED, D3 built

## D2 (isovoltage clock path) — verdict CORRECTED: FAIL-BOUNDED

The prior entry ("D2 clock path PASS, storm-soak running") was premature and
is **superseded**. A transition storm *alone* (idle, ~1170/s) ran clean to
>2M transitions — but that was the wrong stimulus. Re-reading the 6.18
synthesis (idle-storm clean, **pinned load** resets <30 s) reframed it: the
one earlier reset happened under `resize2fs` **load**, not idle. A load+storm
hunt (4 busy cores + transition storm, O2 armed) **reproduced the reset**:

- ~10 min clean under load (load 5–8, 2.26M transitions, 0 HFPLL warn), then
  the SoC reset. O2 on the recovery boot: **`previous power-off: PS_HOLD
  (MSM-controlled shutdown)`**, `pon=0x11 warm_reset=0x0002 poff=0x0002`.
  Not PMIC UVLO, not PMIC watchdog — an SoC-side power-hold drop.
- Ramoops console (preserved) ends at normal boot completion (~8 s) with
  **nothing after** through ~10 min of load → classic *silent* reset; the
  kernel logged no WARN/oops/stall before PS_HOLD.
- Live genpd on the failing kernel: `cx_ao` **on**, `genpd:0:cpu0` voting
  **perf 6** — CX *was* attached (the boot `-517` were transient defers that
  resolved). CX is **not** the cause.
- Mechanism: D2 drives **no** Krait APC/L2 rail voltage (`cpu-supply` absent).
  Under sustained load the rail has no per-OPP margin → IR-drop brownout →
  PS_HOLD. CX-pinning at super_turbo only stretched MTBF (6.18 <30 s → 6.12
  ~10 min), it did not remove the failure.
- Evidence: `~/Projects/msm8974-scratch/evidence/d2-psh-reset-20260731/`
  (console-ramoops-0, recovery-boot-dmesg.txt, hunt-watch.log).

**⇒ D2 is not safe under load; the fix is not in the clock layer.**

## D3 (SAW2 gang-rail voltage) — BUILT, pre-registered soak pending

Fix = give the OPP layer a regulator for the shared Krait gang rail. Ported
from the fork's 6.18 line, **no invented values** (oracle FP2 confirms the
working stack scales to 2265600 kHz with voltage tracking):

- **Driver** `6.12/topic/spm-gangrail` (`c6ccc851bb50`): `spm.c` end-state of
  the 9 gang-rail commits (`401fec35e189..0e91a5cc7bec`). 6.12 had already
  dropped the per-CPU v2.1 infra upstream, so the chain does not cherry-pick
  (add-then-remove of code 6.12 lacks); squashed to the reviewed end state
  (margin add+remove nets to none → no `margin_sel`; `set_vdd_ret` present;
  new `qcom,msm8974-saw2-v2.1-l2` → `spm_reg_8974_8084_l2`,
  `smp_set_vdd_v2_1_l2` clears VCTL PORT, writes vlevel, verifies PMIC_STS).
- **DT** on `6.12/topic/krait-cpufreq` (`fcf7046e2596`): `saw_l2_vreg`
  regulator child (350000–1275000 µV) under `saw_l2`; `cpu-supply =
  <&saw_l2_vreg>` on all 4 cores (single shared rail); per-OPP `opp-microvolt`
  triplets, our die `speed1-pvs12-v1` = **800/800/800/800/810/820 mV** across
  300/422.4/652.8/729.6/883.2/960. **One variable vs D2:** freq cap stays at
  960 MHz — add voltage, not higher OPPs. CX stays pinned super_turbo.
- **Config:** `CONFIG_QCOM_SPM=y` pinned built-in (regulator registers before
  cpufreq probes cpu-supply).
- **Integration** `6.12/int/d3` (`fed71b1e5917`) = staging(D0+D1) +
  krait-cpufreq(D2+D3 DT) + spm-gangrail + pon-reason(O2). Merged clean.
  Build running (defconfig `fairphone2_612d3_defconfig`, SHA-pinned).

### D3 pre-registered pass/fail (soak arithmetic)

- **Stimulus:** the exact hunt that killed D2 — 4-core busy load + transition
  storm, O2 armed, reset-watcher reads PON on recovery. (Must use the *load*
  stimulus; idle storm is known not to trigger it.)
- **PASS (rail fix proven):** survives the load-storm **≥ 40 min** clean
  (~4× the observed ~10 min load-MTBF ⇒ >95% exclusion of that class), then
  extend toward the §1.1 gate (≥60 min, ideally overnight, idle+load).
- **FAIL:** any reset. Read PON: still `PS_HOLD` ⇒ voltage values/approach
  need revisiting (check live `saw_l2_vreg` voltage vs opp-microvolt, and
  whether set_vdd is actually reaching the SAW2); UVLO ⇒ rig/power artifact.
- **First check on the D3 boot (before soak):** `regulator_summary` shows
  `saw_l2_vreg` present and at the OPP voltage; `dmesg` shows spm bound
  `saw2-v2.1-l2` and **no** lingering `Failed to set OPP config`; cpufreq
  cur_freq follows setspeed.

## D3 RESULT (2026-07-31) — gang rail works, but per-transition write RESETS

D3 built, flashed, functionally verified: `qcom_spm` bound `f9012000`,
`saw_l2_vreg` (regulator name "spm") registered 350–1275 mV, and the rail
**tracks frequency** (300–652=800, 729=805, 883=825, 960=835 mV; ~+15 mV over
the speed1-pvs12-v1 spec, safe). So the OPP→regulator→SAW2 path is live — the
load margin D2 lacked is present.

**But D3 resets under frequency-transition stress, silently, and FASTER than
D2:**
- Pinned storm+4-core load: reset in **~8–20 s** (`warm_reset=0x0002`, silent
  PS_HOLD, ramoops shows no spm/saw/WARN).
- **Idle storm (transitions only, NO load): reset in ~37 s** (126000 iters at
  3400/s), same silent PS_HOLD. **D2 survived this same idle-storm
  indefinitely** (>2M transitions). Evidence:
  `~/Projects/msm8974-scratch/evidence/d3-reset-20260731/`.

**⇒ Diagnosis:** the reset is caused by the **per-transition SAW2 gang-rail
voltage write** (`smp_set_vdd_v2_1_l2`), not by load and not by the clock path.
D2 (no voltage writes, static rail) is transition-stable; D3 (writes the SAW2
VCTL on every OPP change) is not. The storm is not "invalid" after all — the
idle variant carries no load yet still resets, so this is a real per-write
fault, not the undervolt-under-forced-load artifact I first suspected.

**Unifying hypothesis for the campaign's core silent reset:** if each gang-rail
voltage write has a small per-write probability of glitching the rail → PS_HOLD,
then rate explains MTBF: storm 3400/s → 37 s; a real governor a few/s → the
notorious 5–20 min. The 37 s repro is a **fast, reliable handle on the bug** —
far better than the field's minutes-to-hours.

**This is a §3.1 debugging wall** (hardware symptom, 2 attempts, >1 h). Stop
guessing; walk the authority order for the *correct* Krait voltage-transition
sequence (ordering vs the clock change, ramp/settle, locking vs per-CPU SAW
cpuidle, PMIC handshake). Note the 9 ported commits are the fork's 6.18
*attempt*; whether 6.18 ever soaked stable with them is itself an open question
to check (fork history). Candidate angles: (a) is `smp_set_vdd_v2_1_l2` missing
a ramp/settle or a lock the vendor holds; (b) does mainline msm8974 even drive
APC voltage, or is the SAW2 gang-rail a downstream invention that should be
replaced by a static safe rail (D2 was transition-stable); (c) the excluded
margin commit a0386dc2a86e.

## D3v2 (2026-07-31) — dynamic phase/FTS implemented; the #1 specced fix FAILS

Implemented DVFS-TRANSITION-FIX-SPEC §5 on 6.12 `spm.c` (topic spm-gangrail
`f817aefc187b`, int/d3 `6dc1953a9245`): every voltage change now also writes
SMPS phase count (VCTL port 1) + FTS PFM/PWM (VCTL port 2) matched to the
operating point, polling PMIC FSM-idle (PMIC_STS[17:16]==0, register-verified
vs vendor spm-v2.c). Built clean, flashed, rail still tracks freq (800→835 mV).

**Result: still resets under the idle transition-storm, fast.**
- 300↔960 idle-storm: reset ~30 s (≈ D3's 37 s).
- **729.6↔960 idle-storm (spec's "safe" narrow/high window): reset ~13 s.**

Hard conclusions:
1. The fork's **top-ranked fix (dynamic phase/FTS) does not stop the reset** on
   6.12. Open sub-question: does §5 silently no-op via an FSM-poll timeout, or
   genuinely not help? No runtime observability yet — a debug-counter build
   would tell.
2. **6.12 is at least as bad as the 6.18 spec, probably worse:** the spec found
   729.6↔960 / small-ΔV windows survive 400–600k flips; on 6.12 even 729.6↔960
   dies in ~13 s. The per-transition fault fires on essentially *any* DVFS
   transition here.

Implication: **the whole goal (safe DVFS + thermal — and thermal throttling
needs freq scaling, so it inherits this) is blocked by one unsolved
per-transition PS_HOLD fault** the fork already spent a spec + WIP mitigations
on and PARKED. Fixed-frequency (no VCTL writes) is stable for hours; free DVFS
is not.

Remaining untried angles (all uncertain, each a build/flash cycle):
- spec §6 fallback: arm the L2 SPM sequencer vendor-way (saw2-spm-ctl=0x1) — but
  the fork already tried arming and it "made it worse" (spec §4.2).
- observability build (count phase/FTS writes + FSM-poll failures) to learn
  whether §5 no-ops before abandoning it.
- bisect the 6.12-vs-6.18 delta that makes 6.12's narrow window die where
  6.18's survived.
- full coeff+load phase policy (spec §5.2 option B) vs voltage-only.

Device parked on `powersave` (fixed 300 MHz, transition-quiet, stable).

## D3v3 A/B (2026-07-31) — phase/FTS is HARMFUL; #1 spec fix refuted

Made phase/FTS runtime-toggleable (module param `l2_phase_fts`, spm-gangrail
`ae23ddf7b5df`, int/d3 `562398e5991b`) + FSM-poll-timeout warn + info_once.
On-device facts:
- Instrumentation: `phase/FTS active` fires (path runs) and **0 FSM-poll
  timeouts** — the port-1/2 writes land cleanly. So §5 is NOT a silent no-op.
- **Clean A/B, same kernel/boot, identical 300↔960 idle-storm:**
  - fix ON  (phase/FTS): reset at **~34,800 flips (~15 s)**
  - fix OFF (voltage-only, = D3): reset at **~99,600 flips (~36 s)**
  ⇒ dynamic phase/FTS (policy A) makes the reset **~3× sooner** — HARMFUL, not
  the fix. Refutes DVFS-TRANSITION-FIX-SPEC §5 (top candidate) on 6.12, at least
  with the voltage-only coeff policy.

Standing facts after D3v3:
- Best config so far = phase/FTS OFF (default flipped to off / commit to be
  reverted). Even so, free DVFS resets at ~99.6k flips (~36 s storm ⇒ ~hours at
  field ~1/s) — the goal is still not met.
- The per-transition PS_HOLD fault is confirmed real, silent, rate-scaled, and
  resistant to: isovoltage (D2), gang-rail voltage scaling (D3), dynamic
  phase/FTS (D3v3, harmful). The fork itself parked this after a spec + WIP.

Untried / open (for a steer): spec §6 arm-SPM (fork said "made it worse"); the
fork's own ~3× mitigation 4b2508 (disarm L2 SPM seq + irq-off across the VCTL
write) which THIS port omits; coeff+load policy B; bisect the 6.12↔6.18 delta;
or accept fixed-frequency (stable for hours) / no free DVFS as the shippable
6.12 and keep the reset as tracked research.

## D3-preempt + ROOT CAUSE CONFIRMED (2026-08-01) — collapse-vs-write overlap

Vendor source analysis (fp2 BSP, subagent) found the one thing the vendor does
around the register-identical VCTL write that we don't: it runs it
**preemption-disabled** (get_cpu/put_cpu) and pins the whole transition to the
target CPU at SCHED_FIFO, so the writing CPU cannot be scheduled away or enter
cpuidle mid-handshake. (Also affirmatively confirmed phase/FTS is NOT the
vendor mechanism — use-phase-switching absent — matching the A/B.)

Fix 1 — **preempt_disable() around the L2 set_vdd** (spm-gangrail
`4c98238ddbd0`, int/d3 `015852295b26`): idle-storm survived **~540k flips /
~190 s** vs D3's ~100k / 13–37 s — a **~5×** gain. Root-cause direction
validated, but still eventually reset (preempt-off covers only the SAW write,
not the sleeping clk_set_rate that follows).

Fix 2 discriminator — **disable the per-core `cpu-spc` deep-idle state**
(runtime, all 4 CPUs) on top of preempt-fix: idle-storm ran **1.8M+ flips /
550s+ with ZERO resets** and still going. **This confirms the mechanism:** the
per-core SPM **power-collapse** (`cpu-spc`) running concurrently with the L2
gang-rail VCTL write corrupts the shared SAW/rail → silent PS_HOLD. Preempt-off
stops the *local* core collapsing during the write (the 5×); disabling cpu-spc
removes the collapse entirely (the rest).

**First stable free-DVFS configuration on this fork:** preempt-off + cpu-spc
disabled. Costs per-core deep-idle power (cores idle at WFI, not power-collapse)
— a documented tradeoff, not the power-optimal end state. 60-min + load soak
running (watcher).

Next (power-optimal, future): instead of disabling cpu-spc, *coordinate* it —
serialize the gang VCTL write against per-core collapse (the vendor keeps
cpuidle AND does DVFS). Candidate: preempt-off (have it) + a lock/gate shared
between smp_set_vdd_v2_1_l2 and the cpuidle-enter path, and/or the vendor's
retention-floor IPI. For a shippable stable-DVFS milestone now, disable cpu-spc
via DT/kernel default; optimize idle power afterward. Persist the cpu-spc
disable (DT idle-state removal or default-disabled) — the runtime sysfs setting
is lost on reboot.

# SESSION 4 (2026-08-01) — overnight PASS; full OPP; LOAD-reset root-caused

## Overnight idle-storm soak: PASS (the checkpoint holds)
- 7.15 h continuous, **82.5M transitions, 0 resets** (boot count stayed 2),
  preempt-fix + cpu-spc-off. Health green: remoteprocs up, 0 IOMMU faults,
  network+modem+sensors fine, rail scaling live.

## Build A (full OPP range): validated
- cpu_opp_table extended to the 14 stock rates (300→2265.6 MHz), per-bin
  voltages from the fork's 6.18 DT; cpu-spc disable persisted in DT
  (krait-cpufreq `f89eae2b67e5`). On device: 14 freqs, rail 800→1060 mV
  monotonic, **630k full-range transitions clean** incl. 300↔2265.6 big-ΔV.

## LOAD reset (the remaining killer): root-caused via Marc's VBAT instrument
- Build B (thermal DT wired, `83a56b003aca`) reset under pinned 4-core load in
  30–144 s at *any* freq (960 = 2265.6), PS_HOLD, temps 50–60 °C (not thermal).
- **VBAT telemetry** (PM8941 VADC ch6 VBAT_SNS raw, calibrated per-sample from
  the on-die 625/1250 mV refs, ×3 prescale; 35 Hz fsync logger): VBAT **flat at
  4.19–4.20 V through the moment of death** (min 4.169 V / 2407 samples) ⇒
  battery/charger path EXONERATED. IADC reads raw=0 (needs work; parked).
- Discriminator (devmem, no rebuild): force SMPS **PWM (VCTL port2=0x80) +
  4-phase (port1=3)** once → the identical load ran **~8 min and counting**
  (67 °C, healthy) vs 30–144 s deaths. **Root cause: the bootloader leaves the
  gang-rail buck in a light-load config; sustained multi-core current droops
  the SoC-side rail → silent PS_HOLD while VBAT stays perfect.** Idle storms
  never drew the current — why every idle test passed.
- Fix committed: `8ecbddae6e91` "soc: qcom: spm: boot the msm8974 gang-rail
  SMPS into PWM/4-phase" — static, once at probe, FSM-idle-confirmed, vendor
  50 µs settles. Per-transition management stays out (A/B-tested 3× worse).
  Cost: PFM idle efficiency; tracked with the deep-idle coordination item.

## The msm8974 silent reset — complete picture (two mechanisms, both fixed)
1. **Transition face:** per-core cpu-spc power-collapse racing the gang-rail
   VCTL write → fix: preempt-off write + cpu-spc off (82.5M transitions clean).
2. **Load face:** SMPS light-load boot config vs sustained multi-core current
   → fix: static PWM/4-phase at probe (~8 min live vs 30–144 s; soak pending).

## Build C = `e363f90b9df6` (int/d3): staging + cpu-thermal (full OPP +
cpu-spc-off + 90/105 °C trips + cooling-maps + CONFIG_CPU_THERMAL=y) +
spm-gangrail (regulator + preempt + SMPS boot config) + pon-reason.
Pending: flash → load @960 → load @2265.6 (thermal-throttle engagement =
D4 validation) → idle-storm regression → merge topics to staging, §1.1 soak.

TODO (Marc's ask): dedicated `battery-telemetry` topic off the no-DVFS
baseline — formalize the VBAT instrument (VADC built-in, logger tool in
kernel-tests, and investigate IADC current raw=0).

## D4 staged thermal — VALIDATED (2026-08-01, after Marc's envelope correction)

Marc's field data: this board runs **days at 90 °C / 960 MHz / full load**
(no-DVFS kernels, bootloader ~1.0 V rail). So temperature alone is not the
limit — the failure corner is high-freq × high-temp × minimum bin voltage.
The interim 60 °C-trip attempt was over-conservative and was dropped unbuilt.

**Staged policy** (cpu-thermal `a72c2608c04b`): warm passive trip (FP2: 70 °C,
hyst 5) throttles within cooling states 0–7 (2265.6→1036.8); hot trip 88 °C
(hyst 4) floors the cap at state 8 (**≤960 MHz — the days-proven point**);
critical 105 °C. Result on device (Build E `21cdb7d443ed`): max-effort 4-core
load bursts to 2265.6, staircases down, and settles at **~79 °C / 1497.6 MHz
(throttle state 4–5), stable for 20+ min and counting** — vs ~10.5 min to
reset at ~90 °C/1728–1958 under the flat-90 °C policy. No reset, no critical
trip. Watch item: DVFS runs 960 at the 835 mV bin value vs the ~1.0 V of the
multi-day campaign; if the hot floor misbehaves at 835 mV, bump that OPP.

## Staging merge + staging image
All topics merged into `6.12/staging` = `69762aed3891` (krait-cpufreq,
spm-gangrail, cpu-thermal, pon-reason, battery-telemetry) and pushed; tree
identical to the validated int/d3 plus the battery-telemetry VADC one-liner.
Build F (pinned to the staging SHA) prepared for the §1.1 gate run: flash →
health sweep → ≥60 min idle soak with a real governor (no pins) → then
rc promotion is a human decision.

Bench notes: device SSH is password auth (root/root) per Marc — no key dance
after flashes. Remote pushes unlocked.

# SESSION 5 (2026-08-02) — battery A/Bs, BCL mitigation, §1.1 idle gate PASS

## The load-death picture, finally complete (battery-swap A/Bs)
- Android oracle ran ~30 min flat-out on the "bad" pack (and FP2 Android has
  NO BCL: no /sys/devices/platform/battery_current_limit, none in
  thermal-engine-8974.conf) → pack functional; contamination note: oracle was
  on AC (1.5 A) vs DUT on a 0.5 A host port.
- Build E replica + healthy pack: **43 min continuous max-effort load, 0
  resets** (ended by battery swap, not failure) vs Build G same pack 8.4 min →
  the v2-era BCL driver's 1 s VADC polling degraded load stability; v2.1
  drops to 30 s polls.
- Drained pack (~3.86 V): load at ANY freq (2265.6/960/729.6, rail up to a
  devmem-forced 1.0 V) died 0.5–3 min → NOT rail voltage, NOT frequency;
  input-side transient behavior. **At the 300 MHz cap the same 4-core load ran
  29+ min clean** → the preventive cap is the effective mitigation.

## BCL v2.1 (qcom_vbat_freqcap) — Marc's "don't reset on drained battery"
- Preventive: trips on resting VBAT (FP2: <4.05 V → cap 300 MHz; release
  >4.20 V; poll 30 s; hotplug off by default — the kernel-side freq_qos cap
  alone was measured sufficient and userspace cannot undo it).
- Validated on-device: cap engaged 9 s after boot on the drained pack; 29+
  min of 4-core load, no reset, 53 °C.

## §1.1 IDLE GATE: **PASS** (final image 66fb2453edd9 = staging+bcl v2.1)
- 66 min, schedutil, full range (mitigation thresholds parked for the soak),
  no pins, no load, single boot, zero kernel warnings. Health green
  (remoteprocs 3/3, 0 IOMMU faults, sensors 3/3).

## Staging
- `6.12/staging` = `51d6ce569416` (bcl merged after merged-image validation);
  tree bit-identical to the validated int/d3. LOCAL ONLY (no pushes, Marc).

## Remaining
- Overnight LOAD soak on the final image (tonight) → then rc promotion is
  Marc's call. Root-cause research track (why any-freq load kills at low VBAT
  without the cap; charger-path/l24/XPU; wall-charger A/B; exact safe-freq
  edge 300–729) continues behind the mitigation.

# INVESTIGATION R1 (2026-08-02): root cause of the mid-VBAT load reset
## (Marc's directive: no workarounds — real root cause, oracle-equivalent
## stability. BCL cap demoted to parked experiment; load soak cancelled.)

## Pre-registered facts (all measured, this campaign)
F1 Idle: bulletproof at any VBAT (82.5M transitions overnight).
F2 Load 4x300 MHz at 3.86 V: stable 29+ min.
F3 Load ≥729.6 MHz at ≲4.0 V: silent PS_HOLD in 0.5–3 min — INDEPENDENT of
   cpu frequency (729.6/960/2265.6 alike) and of CPU rail voltage (bin values
   and devmem-forced 1.00 V alike).
F4 Load full-range at ≥4.17 V: 30–43 min clean (longest run ended by user).
F5 Same old pack under Android at 3.85 V (on AC 1.5 A): 30 min load-22, no
   BCL, thermal throttling only.
F6 VBAT telemetry (35 Hz, fsync): flat at the death instant — no slow sag.
F7 PON: PS_HOLD (0x0002) for load deaths; separate rarer signature
   poff=0x0000+warm_reset for idle deaths around battery-swap/charger events.
F8 Ramoops: console silent to the end — kernel never sees it coming.
F9 Confound known: our failures were on a ~0.5 A USB host port; Android's
   clean run was on AC. Not yet controlled.

## Hypotheses (pre-registered, ranked)
H1 Charger input-path collapse: mainline smbb lacks vendor AICL/VIN_MIN
   input-collapse regulation; at mid battery the charger pulls max input
   current, load transients collapse VBUS/VPH, PMIC disturbance (l24/OVP/
   reverse-boost events) upsets the SoC. Predicts: battery-dependence (full
   pack = no charge current = no collapse), idle safety, 300-MHz safety
   (demand ≈ input), Android immunity (AICL), wall-charger immunity.
H2 XPU err-fatal is the executioner for a violation generated on that
   disturbance path (D0 arms it; vendor TZ also armed but vendor never
   generates the violation). Predicts: disarming xpu_errfatal turns resets
   into hangs/survival.
H3 Clock-source edge: 300 MHz runs from the aux mux; ≥422.4 engages HFPLLs.
   The survive/die edge may be the PLL-engagement edge (PLL supplies?).
H4 Missing per-rung MX/vdd_mem votes (vendor acpuclock voted vdd_mem/
   vdd_dig per rung; our port pins CX only).

## Authority walk (all five, parallel, per BLUEPRINT §6.1)
A1 vendor charger source (qpnp-charger vs smbb) — agent running.
A1b vendor acpuclock/krait supplies (vdd_mem/vdd_dig votes, HFPLL supplies,
    krait-cc rung↔source map) — agent.
A2 live oracle: charger runtime state under load at mid battery (old pack is
   in the Android device NOW) — agent over adb.
A3 sibling ports: msm8974-mainline/pmaports smbb patches or documented load
   resets — agent.
A4 fork history: our own charger/l24/CP1 artifacts — self.
A5 upstream history: smbb/pm8941 fixes after v6.12 — agent.

## Discipline
Device experiments ONLY after authority synthesis, one variable each,
pre-registered expected outcomes (E1 wall-charger A/B, E2 charging-disabled,
E3 xpu disarm, E4 422.4/652.8 edge, E5 nr_cpus). Every experiment ends with
PON+ramoops read. Ledger updated per result.

## R1 update — Authority 4 (fork history) consumed
Source: buildroot-external/package/msm8974-diag/RESET-COMPARISON.md (the
2026-07-26/27 Android-vs-ours systematic comparison on the phone) + CP1
artifacts.

Adopted from its closed list (evidence recorded there): voltage tables/margin,
MX/CX starvation (Android leaves MX disabled too — H4 KILLED), modem, AVS,
HFPLL programming, thermal, PMIC over-temp, steady-state supply — RULED OUT.
Rail *droop under load* was explicitly left OPEN there ("correct setpoint, no
fault record, death only under load, clean idle — exactly the observed
signature"); our SMPS PWM/4-phase fix later moved MTBF 30 s → 8–43 min but
did not close the mid-VBAT case.

NEW TOP SUSPECT (H5, from its §2.2 "structural, top suspect", never closed):
per-core Krait power delivery unmanaged on ours. Android: per-core krait0..3
regulators (LDO/BHS/bypass per core) over the gang rail; live registers
APC_PWR_GATE_MODE=0x21, APC_PWR_GATE_DLY=0x30430600, MDD_CONFIG/MODE=
0x190/0x2, LDO_VREF_SET managed. Ours: ALL ZERO / unmanaged, asymmetric
LDO_VREF_SET, on a KPSS revision (0x20010000) where these registers decide
the core power-switch mode. Mechanism candidate: per-core switch drop under
current (after the gang rail — explains why forcing the rail to 1.0 V did
not help), modulated by input conditions (battery) via switch supply.

Methodological flag: the 2026-07-27 XPU err-fatal load A/B (disabled=30 s,
enabled=600 s+) did not record battery state — potentially battery-confounded,
like several of this week's own results. The err-fatal arming itself is an
empirical fix with an admittedly opaque mechanism (doc says so) — Marc's
directive applies to it too, eventually.

H-list now: H1 charger input collapse (battery-dependence), H5 per-core APC
power gates (load-dependence), H2 XPU-as-executioner (signature), H3 clock-
source edge (300 vs ≥422). H4 dead. Awaiting A1/A1b/A2/A3+A5 agents.

## R1 update — Authorities A2 (oracle live), A3 (siblings), A5 (upstream) consumed
- A2 REFINES H1: Android's input is CDP@1.5A (same limit as our DT), NO AICL
  running, VIN_MIN never exercised, USBIN rock-steady 4.52V under full load.
  Android's resilience = PMIC battery-supplement mode (battery sources the
  deficit, +0.77A discharge while "Charging", Vbat 3.74V routine). The naive
  "1.5A into a 500mA port" story is NOT what saves Android.
- A3/A5: nobody in siblings/pmaports ever hit or tested this; upstream has
  ZERO functional smbb changes v6.12..v6.18; our charger DT values are from a
  2022 provenance-less commit. If charger-side, the fix must be written new.
- Existing evidence vs H1-oscillating: through EVERY killer-load death, the
  USB gadget link (VBUS-dependent) stayed up until the reset instant — cyclic
  VBUS collapse would have dropped networking first. H1 narrows to
  "single terminal collapse" or dies.

## R1 pre-registered experiments E0a/E0b (run when VBAT < ~4.0, drain running)
E0a: killer load (4-core, ≥729.6) at ~1.5A input; sample /proc/interrupts
  (smbb usbin-valid/chg-gone lines) every 2s fsync'd + watch host-side USB
  events. EXPECT if input-collapse: IRQ anomalies / link flap before death.
  EXPECT if H5/internal: clean IRQ counts to the death instant.
E0b: same load with input limit forced 100000 uA (battery-only; runtime knob
  /sys/class/power_supply/smbb-usbin/charge_control_limit, restore after).
  EXPECT if charger/input path is causal: reset STOPS (or signature changes).
  EXPECT if H5/internal: reset persists identically -> H1 DEAD, H5 leads.
H5 discriminator (pending A1b agent data): program the per-core APC
  PWR_GATE_MODE/DLY (+MDD, LDO_VREF) to the vendor's live values via devmem
  (register offsets from vendor krait-regulator source), rerun killer load.
  No rebuild needed if offsets confirm ACS-space registers.

## R1 — ROOT CAUSE FOUND (register-proven, 2026-08-02)
Per-core pinned benchmarks at three OPPs: CPU0 scales exactly with the OPP
(451/188/60 Miter/s at 2265.6/960/300); **CPUs 1-3 constant ~191 Miter/s
(≈960 MHz) at every OPP — they have never scaled.** Cause: `opp-shared` in
the D2-ported cpu_opp_table → single cpufreq policy → cpufreq-dt sets only
CPU0's clock; cores 1-3 remain at boot rate. (6.18 fork and Android: four
per-core policies; the RESET-COMPARISON doc recorded this and the port
ignored it.) Meanwhile the gang-rail voltage follows CPU0's OPP
(800→1060 mV) while cores 1-3 need ≥820 mV for their fixed 960 MHz.
MECHANISM: any low CPU0-OPP undervolts loaded cores 1-3 → silent PS_HOLD;
idle immune (WFI); Android immune (all cores scale); battery level sets the
margin; caps lowered the rail and made it worse. Also found on the way, real
and separate: krait-cc sec-mux parent map inverted vs vendor register truth
(upstream parent_data regression) — OPP-300/aux writes hw sel of QSB.

## R1 mechanism-proof pair (pre-registered; VBAT ~4.0 = repro zone)
P1: OPP pinned 300000 (rail 800 mV), load ONLY cores 1-3 (affinity spinners;
    cpu0 idle). PREDICT: dies fast (960 MHz cores at 800 mV under load).
P2: OPP pinned 300000 (same rail), load ONLY cpu0 (in-spec 300 MHz@800 mV),
    cores 1-3 idle/WFI. PREDICT: survives (bounded 10-min observation).
Outcome matrix: P1 dead + P2 alive ⇒ mechanism confirmed ⇒ fix = drop
opp-shared (per-core policies; regulator core aggregates max across the four
cpu-supply consumers = vendor's gang-voltage-max semantics) + fix sec-mux map.

## R1 continued — topology fix necessary but insufficient; CHARGER implicated
- Fixes b306ead1660b (sec-mux inversion) + f3137a48ac00 (drop opp-shared)
  VALIDATED structurally on-device (image c7b087d10702): 4 per-core policies,
  all cores scale (60/191/452 Miter/s tracked per OPP), voltage aggregation =
  max of requests (1060 mV with one core at 2265.6). NECESSARY fixes.
- V4 (old killer, load+transitions) on the fixed kernel at resting ~4.0 V:
  still PS_HOLD, ~40 transitions. H5 devmem (APC gates to vendor values):
  ~170 transitions — margin contributor, not mechanism.
- E0b/E0a series sealed the charger: charging ACTIVE -> death at 40-170
  transitions (and chg-gone IRQs on record, 62 on one boot); charging absent
  (100 mA limit, latched-off, or engagement bug) -> 300/660+ transitions
  ALIVE, at the lowest VBAT of the campaign (3.67 loaded), chg-gone=0
  throughout. 4-for-4 correlation.
- NEW defect found on the way: mainline smbb does not re-engage charging
  without a physical insertion edge (plain reboot at VBAT 3.86 stays
  Discharging, chg-fast IRQ never fires). Tracked separately.
- FIX WRITTEN (937177edfdce, topic smbb-input-collapse): vendor-grounded
  chg-gone/ARB response (charge off + force-run-on-battery 1 s, then
  restore) + REV_BST comparator config enabled and corrected (the mainline
  #if 0 block also had the unlock magic in the mask field).
- Build R2 (all three fixes) compiling. NEXT: Marc replugs USB (restore
  charging) -> stock-kernel charging-active death re-confirmed with IRQ log
  -> flash R2 -> same test must survive -> then full gate ladder from zero.

## R1 correction — charger correlation BROKEN by counterexample (honesty entry)
Sealing run (charging verified ACTIVE: charge_type=Fast, battery supplementing
under load): 320+ transitions and climbing at the WORST VBAT of the campaign
(3.65 loaded), chg-gone frozen. The 4-for-4 charging-death correlation is now
4-for-5. Re-examination: the two post-topology-fix deaths (40, 170
transitions) vs survivals (300/320+/660+, several censored by operator stop)
are consistent with draws from one heavy-tailed distribution — the single-
measurement trap RESET-COMPARISON explicitly warns about, repeated here.
Confounder candidate for the two deaths: both were on the first boots after
flashing (first-boot resize/init churn concurrent with the test load).
What stands: topology fixes moved the killer MTBF from <30 s consistently to
worst-draw 40 / median hundreds+ transitions. The smbb defects remain real
(miswritten #if 0 comparator, no collapse response, no charging re-engagement
without insertion edge) and the fix (937177edfdce) stays queued — but claims
now require distribution-scale evidence: LONG horizons + repeats per arm.
Protocol: current charging-active killer continues to a multi-hour horizon on
the fixed kernel (c7b087d10702). R2 (with smbb fix) flashes after; then the
formal gate ladder decides.

## R1 horizon trial — pre-registered stopping criterion
The two post-topology-fix deaths were at 40 and 170 transitions. STOP RULE:
survival past 1700 transitions (10x the worst death) under charging-active
worst-battery conditions = the topology-fixed kernel's distribution is
established as fundamentally moved (worst-draw evidence bounded); proceed to
flash R2 (adds the smbb fix) and run the formal §1.1 gate ladder from zero.
A death before 1700 = PON-attributed, ends the trial, and the smbb fix's
A/B gains a clean baseline number.

## R1 — smbb fix FAILS first hardware contact; reverted (honesty entry)
On R2 (with 937177edfdce): (a) physical replug no longer re-engages charging
(it did on the same day's stock kernel: insertion -> Charging + chg-fast);
charge_type=N/A with chg-gone=4 fired through the new handler — suspect the
assumed CHG_CTRL bit0 FORCE_BATT semantics (taken from vendor qpnp 0x49
bit0) do not hold here and the handler leaves charging blocked, and/or the
REV_BST comparator write changes chg-gone semantics at insertion; (b) an
IDLE unplug produced a PS_HOLD death (new signature for idle) with the
handler cycling CHG_CTRL during the removal transient. Verdict: fix reverted
from int/d3 (merge revert); topic parked for redesign with silicon-verified
bit semantics + guards against insertion-correlated chg-gone + its own
plug/unplug validation suite. The three smbb DEFECTS remain real and open.
R3 (topology fixes only — the 1840-transition trial-proven content) building;
idle gate re-runs on it. Also noted: the R2 idle gate was interrupted by the
unplug death at ~40 min (clock restarted).

## R1 — BCL fate decided: REMOVE (all three authorities agree)
User directive: after the R3b idle gate, "remove that 300MHz cap or at least
make it more logical because this is useless in practice" (a 4.05 V trip sits
above most of the discharge curve; the device would live at 300 MHz).
Authority findings gathered during the gate:
1. Vendor source (fairphone2-kernel drivers/power/battery_current_limit.c):
   stock BCL never trips on raw VBAT; it models available current,
   iavail_ma = (vbatt_mv - vbat_min) * 1000 / rbatt_mohm, mitigating only
   when userspace (thermal-engine) arms a mA threshold.
2. Live oracle (FP2 stock, old pack fully charged): /sys/devices/qcom,bcl.75
   mode=enabled, poll 10 s, rbat=209 mohm, vbat_min=3400 mV, iavail=4736 mA —
   and BOTH mitigation thresholds DISABLED (value 0). Stock Android ships
   this device with battery-based CPU mitigation UNARMED. thermal-engine
   runs; msm_thermal parameters show enabled=N.
3. Own trial: 1,840 charging-active worst-battery transitions at full
   frequency range with the cap parked — no reset (>=10x worst death).
Verdict: revert 6.12/topic/bcl merge (51d6ce569416) from the integration ->
R4. The topic branch retains the code; any future re-introduction must use
the vendor iavail model, not a raw-voltage trip. Gates re-run on R4 as the
final content (R3b gate = supporting data; single delta = BCL removal).

## R3b — §1.1 IDLE GATE: PASS (111.3 min, formally verified from device log)
Kernel 7b678aaffb49 (tree byte-identical to trial kernel c7b087d10702).
Evidence (/root/soak/idle-gate-r3b.log, fsync'd 30 s samples): 223 samples,
0 uptime regressions, 0 gaps > 61 s, up=474 s -> 7119 s = 111.3 min
continuous (171% of the 65 min floor). Full-range per-core DVFS under
schedutil throughout: per-policy freq histograms all multi-rung
(p0: 300k..2265.6k incl. 13 samples at max; p1 up to 1958.4k; p2 up to
2265.6k; p3 up to 1728k), histograms differ per policy = independent
scaling. Charging active (Fast) the whole gate, VBAT 3.80->3.86 V,
temps ~52-53 C. PON shows no reset since the deliberate flash reboot.
Conditions/deltas: BCL parked via module params (low=3.0 V); soak-logger
not present as a unit; no pins. Host watcher died at user logout (user
systemd unit, no linger) — verdict taken from the device-side log per
design; lesson: enable-linger or judge on DUT uptime only (done).
Replug test on R3b earlier: unplug survived idle + charging re-engaged
to Fast on live insertion (host USB log 15:35:15/15:35:30 proves the
physical event) — both R2 smbb-fix failure modes absent after revert.

## R4 — BCL removed per user directive; gates to re-run on final content
User: "we need to remove that 300MHz cap or at least make it more logical".
int/d3 = 25d9f7b2ce9e: revert of bcl merge 51d6ce569416 (driver + Kconfig +
Makefile + DT node, -262 lines) + CONFIG_QCOM_VBAT_FREQCAP scrubbed from
buildroot linux-612.config. Single delta vs R3b = BCL removal. R4 idle gate
+ overnight load soak = the formal gates on shipping content.

## R4 night soak ATTEMPT 1: INVALID TEST (harness bug), death real but unattributable
Timeline (log preserved: /root/soak/night-soak-r4-attempt1.log on DUT):
- v1 soak started (no guard); 10 min into P0 a guarded v2 was swapped in.
  killall sent TERM to v1, but busybox sh delivers pending signals only
  after the current command - v1 was inside sleep 3900 - AND the trap had
  no exit. v1 logged STOPPED at up=4071 and CONTINUED into its load loop.
- Result: TWO interleaved schedules offset 585 s from up=4071 (double spin
  load, dueling policy0 flip drivers, each kill_load truncating the other).
- Charger status flipped Discharging ~60 s into (doubled) load; pack sank
  3.80 -> 3.65 V over 2.2 h. Guard floor 3.55 V never reached before death.
- DEATH at up~11880 during (nominally) PB-split-cpu1-c3: silent PS_HOLD
  (pon=0x11 warm_reset=0x0002 poff=0x0002), console-ramoops confirms zero
  kernel output at death (no oops/panic - the classic silent signature),
  VBAT ~3.69 V, battery-only-equivalent margin. Same family as the
  documented input-power wall; NOT attributable to kernel content given
  the contaminated conditions. Kernel survived 2.2 h of accidental
  double-torture on a sinking pack before dying.
- Still valid: P0 window up 171-4071 was pure idle (both instances idle) =
  SECOND clean 65-min idle-gate pass on R4. Charger re-engaged by itself
  at reboot (Fast). Death-boot ramoops also shows mss first-try boot
  failure (-110, recovered later) + early-boot RCU expedited stall notice
  - filed as observations.
Harness bugs fixed in v3 (single variable discipline applies to the
instrument too): trap now exits; PID-file single-instance guard; phase
attribution via file (all attempt-1 samples falsely said P0-idle); guard
floor raised to 3.70/3.85 V (death band was 3.65-3.69); GUARD-giveup
parks idle forever after 45 min of no recovery instead of risking the DUT.
ATTEMPT 2 armed same night on the same R4 content, single verified
instance.

## R4 night ATTEMPT 2 + morning: TWO more deaths — the load+transition face
## is ALIVE on R4, and one death was IDLE. Trial validity now in doubt.
Attempt 2 (clean harness, single instance, charger held all night, VBAT
3.9-4.0, guard never needed): P0 idle gate PASSED (130/130 samples, third
idle pass on R4). Then DEATH 162 s into PA-transforce-c1 (cores 1-3 @960
loaded, policy0 userspace flips 300<->2265.6 per 4 s). pon=0x11
warm_reset=0x0002 poff=0x0002, ramoops silent. Temps 62-76 C. Note: the
thermal staircase was re-capping policy0 (cur 1267/1728 vs set 2265.6)
during the flips.
Death #3 (morning): EXP-1 (PA replica) never started - scp died mid-
transfer; the device reset IDLE ~12 min into the boot (54 C, charging,
schedutil). Same PS_HOLD signature. The death boot's ramoops shows
repeated sdhci mmc1 pwr_irq timeouts (also seen on the attempt-1 death
boot at t=17-25 s, but ZERO on the current boot) - marker of degraded
post-reset boots, parked as observation.
HONESTY: the 1,840-transition trial ran on the kernel WITH the BCL driver
present, on a 3.7-3.8 V pack, BEFORE the "BCL caps policy0 below 4.05 V"
lesson was learned at R2 validation. If BCL's freq_qos cap was active,
policy0's forced flips were clamped (possibly 300<->300 no-ops) and the
trial validated far less than recorded. The trial's flip evidence is
QUARANTINED until the v4 driver's actual effect is re-established.
Status: all load experiments HELD. §3.1 wall protocol engaged - parallel
authority reads running (vendor Krait transition invariants incl. L2
fmax/voltage coupling + APC LDO/BHS; own-tree audit of L2 clock
management, voltage aggregation path, transition ordering). Suspicion
list: L2 clock/voltage coupling absent in port (capability matrix R4 row
was already marked "gap"); APC power-gate config (H5, known 4x margin);
idle death resembles the 6.18 CX-corner idle-reset family.

## SYNTHESIS 2026-08-03: the remaining silent-reset mechanism (authority
## reads complete; conclusions drawn per user request, soaks halted)

### Verified facts (live registers + both authority reads)
1. All four APC power-gate blocks are UNPROGRAMMED on the running R4 DUT:
   APC_PWR_GATE_MODE=0x0, APC_PWR_GATE_DLY=0x0 at
   0xf9088000/0xf9098000/0xf90a8000/0xf90b8000 (+0x1c/+0x20), global
   PWR_GATE_CONFIG(0xf9011044)=0x0. KPSS_VERSION=0x20010000 (pro silicon).
   Vendor (krait-regulator.c kvreg_hw_init/glb_init) requires, BEFORE any
   rail movement: PWR_GATE_CONFIG=0x0308736E, per-core MDD_CONFIG_CTL=0x190
   + MDD_MODE=0x2, APC_PWR_GATE_DLY=0x30430600, APC_PWR_GATE_MODE=0x21
   (HW sequencer enabled, BHS mode).
2. Vendor dynamically manages per-core LDO-vs-BHS on EVERY gang voltage
   change (FP2 numbers: siblings at 960 MHz/840 mV go to internal LDO at
   827.5 mV whenever vmax>=977.5 mV, and MUST return to BHS before the
   rail drops - decrease path is mode-first-then-rail). Our port has none
   of this; with MODE=0 the LDO is at least not engaged, but the
   sequencer that makes gate transitions make-before-break is OFF.
3. The historical H5 devmem experiment (programming exactly these
   registers) gave ~4x transition survival (170 vs 40) - the strongest
   on-device evidence tying this block to the death mechanism.
4. L2-undervolt hypothesis REFUTED by the vendor read: L2 logic is powered
   from the CX corner (pinned SUPER_TURBO in our DT = protected), not the
   swinging gang rail. Our static L2 @729.6 MHz (verified in boot log,
   krait-cc leaves it at boot rate; vendor would run 960-1728 MHz under
   load) is a PERFORMANCE gap, not the brownout.
5. Our spm.c static 4-phase+PWM probe config conservatively covers the
   vendor's dynamic phase/PFM management (vendor: phases from load,
   4 phases for 4 loaded cores; PFM only if 1 core online and idle).
6. Mainline transition ordering (volt-then-clk up / clk-then-volt down,
   820<->1040 mV swings, 176 us ramp, HFPLL full disable+relock per flip,
   park on sec-mux/aux) matches vendor EXCEPT: no SCHED_FIFO boost (longer
   parked windows), and a 200 us lock-poll give-up that switches onto a
   possibly-unlocked PLL (no WARN observed - not currently firing).

### Conclusion
The load+transition silent reset (and plausibly the rarer idle death -
schedutil swings the same rail at idle) is attributed with high
confidence to gang-rail voltage swings executed while the per-core APC
power-gate/MDD hardware is unconfigured (vendor invariant I3 violated
outright; I2's dynamic mode management absent). The R1 fixes (opp-shared,
sec-mux order) were real defects but addressed a different layer; the
1,840-flip trial that "validated" them is quarantined (BCL cap suspicion).

### Next session plan (pre-registered)
1. Port vendor kvreg_hw_init/glb_init as a proper boot-time config (all
   four cores + global): PWR_GATE_CONFIG=0x0308736E, MDD 0x190/0x2,
   DLY=0x30430600, MODE=0x21 (sequencer-BHS, static - no LDO use, so no
   dynamic mode management needed). Candidate home: spm.c probe or a
   small qcom,krait-apc platform driver on 6.12/topic/krait-apc.
2. Re-run EXP-1 (PA replica): prediction = death MTBF rises >>162 s; then
   staged gates per section 1.1 from scratch.
3. If deaths persist: next candidates in order = SCHED_FIFO-boosted
   transitions (I9), load-hint/phase dynamics (I1/I10), HFPLL lock-fail
   logging (I7).
4. Trial harness rebuild: transition driver must verify flips via
   total_trans + scaling_cur_freq (never trust write counts; BCL lesson).

## 2026-08-03 (day): krait-apc fix built per synthesis; oracle-verified plan
EXP-0 (runtime devmem programming) ABORTED as method: Marc rejected the
batch and reported a devmem write reset the DUT - runtime writes to the
live power-gate block are unsafe. Data point folded into the design:
programming must happen at boot before the cores scale (vendor does it at
regulator probe; we do it in krait_cc_probe BEFORE CPU clock registration,
so no cpufreq transition can precede it by construction).
Oracle plan verification (no /dev/mem on stock kernel; used vendor
regulator debugfs live): per-core requests 800/890/890/890 mV idle ->
1040 mV rail with cpu0 forced to 2265.6 (matches our DT bin exactly);
ALL cores stayed mode=BHS through the swing; LDO engages only for cores
<=850 mV under a high rail (formula check). Static sequencer-BHS is a
conservative subset of observed vendor behavior. PLAN CONFIRMED.
6.12/topic/krait-apc = ed54c8b527ca (stacked on krait-clk; both touch
krait-cc.c): glb PWR_GATE_CONFIG 0x0308736e (KPSS>2.0), per-core MDD
0x190/0x2 + DLY 0x30430600 + MODE 0x21, before clk registration.
Compiles warning-free. int/d3 = f97cca4b2547 (R5). Build R5 running.
EXP-1 pre-registration (on R5): PA replica (3x spin@960 + policy0
300<->2265.6 per 4 s). Baseline deaths: 162 s and <60 s. Stop rule:
1800 s / 450 transitions = >10x baseline -> hypothesis confirmed, then
repeat 2x before gates. Death -> fallbacks per synthesis (SCHED_FIFO
boost, phase dynamics, HFPLL lock logging).

## EXP-1 on R5: DEATH at ~283 s — APC hypothesis REFUTED as sole mechanism
R5 (krait-apc live, registers verified programmed at boot: MODE=0x21,
DLY=0x30430600, MDD 0x190/0x2, GCC 0x0308736E) died the PA killer at
~283 s into the phase (baseline 162 s / <60 s). Silent PS_HOLD again:
next boot pon=0x11 warm_reset=0x0002 poff=0x0002, no console output.
NEAR-MISS METHODOLOGY NOTE: the death was almost misread as "script
mysteriously stopped": the reboot fit between two 30-s watcher pings,
fakeclock+no-NTP scrambled wall time (chrony has no reference on the
host-only gadget net), and the fresh boot's soak-logger + default
governors mimicked a continuously-alive system. Resolved by CONTENT:
PON warm_reset=0x0002 on the current boot, two distinct boot_ids in
/root/soak, and policy time_in_state age == current uptime (proving
EXP-1 never ran in the current boot). Rule reaffirmed: never trust DUT
wall time; judge boots by PON + boot_id + uptime-anchored logs.
Verdict: keep krait-apc (vendor-mandatory, correct; 283 vs 162 s single
draws suggest margin, not cure) but the killer has another layer. Next
per pre-registered fallbacks: (a) SCHED_FIFO/target-CPU transition
execution (vendor I9), (b) phase/load dynamics (I1/I10), (c) HFPLL
lock-failure logging (I7), (d) voltage-sequencing overlap (I6).
Decisive single-variable discriminator to run FIRST: PA with the rail
HELD at max (pin one sibling policy min_freq at 2265.6 so the aggregate
never drops below 1060 mV) — frequency swings continue, voltage swings
stop. Survival ⇒ the race is in the VOLTAGE path; death ⇒ clock path.

## R5 EVIDENCE DISCARDED per Marc; clean retest running; PON ambiguity found
Marc's objection sustained: the R5 conclusions (EXP-1 283 s death AND the
"idle death at 07:33") are contaminated - devmem reads of the APC block
were performed on those boots (the /dev/mem XPU hazard class; Marc also
observed a devmem-triggered reset), and the DUT clock was wrong until
07:35, making the timeline reconstruction unreliable. ALL R5 verdicts
discarded.
NEW FACT from the retest reboot: a deliberate soft `reboot` ALSO leaves
pon=0x11 warm_reset=0x0002 poff=0x0002 - identical to what was being
read as the silent-death signature. PON alone cannot distinguish an
orderly kernel reboot from the silent reset. Registry now writes a
clean-stop marker at orderly shutdown (soak-logger S90 stop hook,
committed c96b1812+); a boot with no matching clean-stop ended uncleanly.
Retest protocol (running): clean reboot -> clock synced at boot -> APC
verified from kernel boot print ONLY (zero devmem this and all future
test boots) -> 60 min untouched idle window (soak-logger v2 sole
recorder) -> EXP-1 via exp-run singleton. Judgment: BOOTS registry +
uptime-anchored logs only.

## EXP-2 VERDICT: THERMAL-LIMIT PATH INDICTED (clean, pre-registered)
EXP-2 (PA killer, CPU thermal zones disabled, own 95C guard, clean boot,
verified load, singleton): survived to flip 106 (212 full-span loaded
transitions, ~850 s) = 5.3x past the flip-20/165 s death point where BOTH
thermal-enabled runs died on schedule. Ended by its own heat guard
(zone6 96C) - NOT a reset; restore path worked (zones re-enabled, load
killed, device alive, cooling).
Extremes tolerated without thermal caps: zone temps to ~102C, VBAT sag
to 3.58 V Discharging under unthrottled 4-core+flip load - no reset.
Combined with history (staircase alone under steady max load = fine;
43-min record), the mechanism is the INTERACTION: thermal cap updates
colliding with in-flight transitions on the same policy.
Also: Marc spotted sdhci_msm mmc1 pwr_irq timeouts DURING the loaded
flip phase (flips 7-11, none at idle) on a clean boot - first observable
precursor: PMIC/SPMI power-control handshakes degrade under rail-swing
load; PS_HOLD death is PMIC-mediated. Add pwr_irq count to soak-logger.
Next: (1) check CONFIG_PSTORE_FTRACE availability for a death X-ray
(ftrace into ramoops survives the reset); (2) EXP-3 discriminator:
thermal ON, flips 300<->1267.2 (below any staircase cap -> no
limit-vs-target collision, rail still swings under load): survival past
the thermal zone = collision confirmed; death = capped-operation
electrical pattern. Then the code hunt in cpufreq/QoS/step_wise.

## Code-hunt verdict + THE FIX: spm vsel cache poisoning (R6)
Deep code audit (both cpufreq paths, OPP, regulator, SPM, thermal):
per-policy serialization is airtight (policy->rwsem both paths; no
same-policy A/B interleave); regulator max-aggregation is correct; NO
cross-policy undershoot in the vote logic. The bug found instead:
**spm_set_voltage_sel commits drv->volt_sel (the get_voltage_sel cache)
BEFORE the PMIC handshake and never rolls it back on -ETIMEDOUT.** One
timed-out write -> regulator core retries the identical request ->
retry compares against the poisoned cache -> "already there" -> success
WITHOUT hardware write, WITHOUT ramp delay -> OPP raises the CPU clock
against a rail that never moved -> silent PS_HOLD. The cache stays
wrong until reboot (persistent-degradation shape). The timeout print is
deferred (printk under preempt_disable) and the brownout follows within
the same transition -> the message never flushes -> matches the
zero-trace signature of every death. Thermal staircase multiplies
transitions up to 16-64x/s (250 ms polls x 4 zones, one step_wise step
each) -> multiplies the dice rolls -> explains thermal-on dying 5x
faster (EXP-2). pwr_irq canary = independent evidence the PMIC-comm
path degrades under rail-swing stress (16 timeouts persisting into
idle before the idle death).
FIX (6.12/topic/spm-vsel-integrity c1076b55dd05, stacked on
spm-gangrail): volt_sel_req = the request (input to set_vdd);
volt_sel = last CONFIRMED selector, committed only after the PMIC
handshake, both v1.1 and v2.1 writers; probe seeds both. A failed
write now leaves an honest cache -> the retry really writes.
R6 = int/d3 + this fix, building. Secondary candidates kept from the
audit (if R6 still dies): the 176-256 us sleeping ramp window vs load
steps; OPP current_opp staleness inversions (#3/#4); L2 voteless rail
floor at hot-trip (#5, needs 88 C). EXP-3 (thermal-on subcap flips,
sickness meter live) running on R5 meanwhile.

## EXP-3: SURVIVED (450/450 transitions, thermal ON, sub-cap flips, pwrirq=0)
Matrix complete: EXP-1 fullspan+thermal = dead ~163 s (x2); EXP-2
fullspan no-thermal = survived (heat-guard end, SICK: pwr_irq at flips
7-11); EXP-3 subcap+thermal = survived 450/450, ZERO sickness. The
pwr_irq degradation tracks rail-swing AMPLITUDE (800<->1120 sickens,
800<->955 does not; HFPLL relock identical in both = exonerated).
Only cap-vs-target collision kills. EXP-4 (sdhci unbind) held in
reserve per Marc's controller question - moot if R6 survives.
R6 = 04b989cb2347 (int/d3 + spm-vsel-integrity) flashing; killer test
predictions pre-registered: (a) survival; (b) ideally now-VISIBLE
"timeout setting the voltage" dmesg lines + pwr_irq sickness during
the run (the fix lets the deferred printk flush) = full confirmation.

## R6 KILLER TEST: SURVIVED 450/450 (11x death horizon) — vsel fix validated
Full killer (3x spin@960 + policy0 300<->2265.6/4s, thermal ON, collision
verified live: cap clamping 2265.6->1267/1574/1728 for the entire back
half, zones to 83 C): SURVIVED all 450 transitions. R5 died twice at ~40.
Single variable = spm-vsel-integrity (c1076b55dd05). ATTRIBUTION CAVEAT
(pre-registered middle outcome): zero "timeout setting the voltage" msgs
= no handshake timeout observed; empirical validation strong (one merge,
11x), microscopic account not fully pinned. ALSO: no pwr_irq sickness
during the run (R5 sickened by flip 11 under LESS stress); sickness
appeared ~15 s AFTER the run ended, at post-load idle (schedutil full-
span hops on hot device) - same pattern as R5 post-EXP-3. mmc1 =EMPTY SD
SLOT (card:none; rootfs on mmc0 which is pristine) -> canary demoted to
harmless sensor. Open: why does the empty slot FSM slow under big swings;
why no sickness during the run but promptly at post-run idle.
2026-08-03 afternoon events: R6 recharge boot showed charger NOT
engaging (Discharging, chg-fast never fired, replug seen but no charge
cycle started); one unexplained ORDERLY stop (clean-stop marker, no
commander) + one mid-boot reset (pre-registry, likely brownout on 3.7 V
non-charging pack). Old pack at 14% -> moved to oracle (charging, oracle
reports input as AC). Full pack (4.32 V) swapped into DUT; swap boot
correctly registered poff=0x2000 (battery-pull signature).
STATUS: idle gate R6 running autonomously (phase P0-idle-gate-R6, no
moving parts: soak-logger + BOOTS only; verdict = uptime + log on
session resume).
NEW MANDATE (Marc): charger must engage whenever cable is present -
smbb engagement defect promoted to active item, fix BY THE METHODOLOGY:
1) vendor source: qpnp-charger insertion/engagement path (APSD, VIN_MIN,
   chg_gone/uvd handling, boot-with-cable vs runtime insertion);
2) live oracle: engagement sequence + charger-type detection (AC vs USB;
   note "AC powered: true" on oracle - input detection may be the fork);
3) own tree: qcom_smbb.c engagement conditions incl. why boot-with-cable
   engaged on R3b/R4 but not on today's R6 boots (100% pack = ambiguous,
   3.7 V pack = genuine failure);
4) upstream history for qcom_smbb fixes.
Fix gets its own plug/unplug validation suite (bench rules from the
reverted 937177edfdce attempt apply: silicon-verified bit semantics,
insertion-guarded handlers).

## 2026-08-03 CASE CLOSED #2: idle battery-signature deaths = smbb BAT_IF
## misconfiguration. Fix validated behaviorally (R7 verdict run PASSED).
Evidence chain: 4 idle deaths (poff=0x2000, instant cut at healthy VBAT
4.10-4.30, chgirq static, health Good in samples) all with ONE pack; same
pack runs all day under vendor kernel (Marc's key observation); other
pack passed 69-min gate in same slot/kernel (R6, gate 4, one asterisk:
~16 s SPMI-warning perturbation from a register-dump accident at ~2280 s).
Mechanism (from vendor deep-read): mainline armed BTC comparators
(BAT_BTC_CTRL COMP_EN + range) while never forcing the thermistor bias
(BAT_IF_VREF_BAT_THM_CTRL 0x124A[7:6]) nor selecting the presence source
(BAT_IF_BPD_CTRL 0x1248[1:0]); floating-thermistor sensing lets the PMIC
transiently read battery-absent -> hardware battery-pull power cut =
poff 0x2000. Vendor on FP2: BPD=THM, VREF forced on, comparators LEFT
UNARMED (mask=0 no-op).
FIX: 6.12/topic/smbb-batif-safety b7d064b2eb9d (program BPD+VREF vendor
values, stop arming BTC comparators/range). R7 = int/d3 eab3d5f7607e.
VERDICT RUN (deadly pack + R7): 66 min idle at 4.24-4.31 V, 1 Hz health
poll ZERO flickers, zero resets. Attribution note: comparator never
caught in the act (no flicker) - conviction is by elimination +
mechanism + single-variable outcome change; strongest non-invasive form.
Ops lessons hard-learned today: REJECTED tool commands still execute
remote side effects (3 confirmed cases - treat as executed, verify);
PMIC regmap debugfs reads are unsafe at any granularity (full dump AND
seek reads spray refused SPMI addresses -> pmic_arb WARN storms; 40+40
warnings) - register verification belongs in the driver as bounded
probe prints; reflash wipes /root incl. BOOTS registry (pull logs
before flashing).
SCOREBOARD 2026-08-03: two root causes fixed and validated in one day -
(1) spm vsel cache poisoning (DVFS killer; 450/450 at 11x death horizon)
(2) smbb BAT_IF sensing (idle deaths; 66-min verdict run).
REMAINING for §1.1 on R7: killer replication on R7 (R6-validated content
+ charger patch only), full idle gate (banked: this verdict run counts),
overnight load soak, staging merge. Charger tier-2 (engagement watchdog,
EOC re-arm, trickle workaround, REV_BST corrected) = next topic, with
the plug/unplug validation suite.
