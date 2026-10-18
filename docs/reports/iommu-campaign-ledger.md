# IOMMU / display / XPU campaign ledger — Fairphone 2 (msm8974pro), 6.18

**Append-only.** One entry per experiment or session action, blueprint §5 format
(`docs/porting/BLUEPRINT-kernel-feature-bringup.md`). Plan:
`docs/porting/PLAN-iommu-display-6.18.md`. Artifact root:
`~/Projects/msm8974-scratch/artifacts/`. Never edit past entries; append
corrections.

Every entry states the **authority** that produced the hypothesis (1 vendor
source, 2 live oracle, 3 sibling ports, 4 our history, 5 upstream history) and,
for device results, the **image and DTB** it ran on.

---

## SEED / 2026-08-06 — campaign opened, no code written

State carried in, from earlier sessions and from today's static reading. The
plan document holds the detail; this is the index of what is already settled so
no entry below re-derives it.

**Settled (display, on-device, earlier sessions):** eight root causes fixed
(panel drivers, RPM bus clocks, `mmss_s0_axi_clk` reparent, TE via DCS 0x35,
panel rails, **V7S page tables**, **BFB block**, **`restore_sec_cfg` gating**);
the resulting configuration attaches the MDP SMMU with **zero context faults**
and still shows **black**; `simpledrm` on lk2nd's framebuffer **does** show
pixels (glitches + boot loop); WCNSS bring-up with display active resets the
SoC, independently of the black screen. Source: `6.18-display-findings.md`.

**Settled (XPU):** the vendor arms err-fatal unconditionally
(`scm-xpu.c`, `SCM_SVC_MP`/`0x0e`, **ENABLE = 0x0**, READ = 0x2); our port
matches and read-back proved the device's firmware default is *disabled*; on the
rig, armed → 2 PS_HOLD idle deaths in ~16 min, disarmed → 83 min clean, radios
up in both. Authority 1 + our own measurements.

**Open contradiction (must be resolved, not smoothed over):** the DUT measured
the opposite sign on 2026-07-26 (disarmed → reset in ~30 s under load; armed →
600 s clean). Most plausible reconciliation is that those deaths were the SPM
vsel cache poisoning fixed in `c1076b55dd05` and the arming was a confound.
Owner: CP1.

**New, today, authority 1 (static, no device):**

1. **The TZ diagnostic region is a readable instrument we have never used.**
   `qcom,tz-log@fe805720`, `reg = <0xfe805720 0x1000>` (`msm8974.dtsi:2084`),
   parsed by the vendor's `tz_log.c` into a self-describing header plus
   `vmid_info[]`, `boot_info[]`, `reset_info[] = {reset_type, reset_cnt}` per
   CPU, `int_info[] = {int_num, int_info, int_desc[], int_count[per CPU]}` and
   an ASCII ring buffer. The vendor's own comment on `int_desc` gives the
   example *"Secure Timer, **EBI XPU**"* — so which XPU fired, how often and on
   which CPU is **readable**. It is designed for HLOS reads, so it is not the
   `/dev/mem` reset hazard that RPM MSG RAM and IMEM are.
2. **A memory-map delta big enough to explain the idle deaths.** The vendor
   removes `0x05d00000 + 0x07d00000` and `0x0fa00000 + 0x00500000` from RAM
   (`qcom,msm-mem-hole`, `msm8974.dtsi:2325`); mainline reserves
   `0x08000000..0x0f500000` and `0x0fa00000..0x0ff00000`. **The 35 MB at
   `0x05d00000..0x08000000` is vendor-reserved and mainline hands it to the page
   allocator**, and `msm8974-ion.dtsi:66` puts `qcom,ion-heap@23 /* OTHER PIL
   HEAP */` at exactly `0x05d00000 + 0x1e00000`. PIL carveouts are XPU-locked by
   TrustZone at authenticate-and-reset, whether or not Linux knows. → **H2**.
3. **A candidate deliberate violation for calibration.** The gpio62 card-detect
   declaration (a pin the vendor explicitly does not use for card detect,
   `board-8974-gpiomux.c:754`) produced SoC resets. TLMM pins have per-master
   ownership, which is XPU-guarded territory. → **X3**.

**Pre-registered next actions (in order, from plan §9):** oracle TZ-log baseline
capture; build O1 **on 6.12** (stable series, armed err-fatal, no display
variable); calibrate it with X3; finish the static memory-map audit (M2); read
the 6.16 fork's MDSS/IOMMU DT as the closest working SMMU reference.

**Predictions recorded now**, so CP1/CP2 cannot be rationalised afterwards:

- If **H2** holds, XPU counts grow at idle and faster under memory pressure, and
  reserving the PIL hole removes the resets with err-fatal still armed.
- If **H1** holds instead, the growth tracks display/GPU traffic rather than
  idle, and the named XPU is the DDR/EBI one.
- If counts stay **flat** through a reset, the err-fatal correlation is a
  confound and the whole XPU thread is misframed — that outcome must be
  recorded as such, not explained away.

---

## 2026-08-06 — 6.18 branch set reduced to the campaign, and one retraction

No device access this session, so this is bookkeeping plus one static
verification that changed a decision.

**Verified (authority 4, our own history):** the 6.12 line is a **strict superset
of the 6.18 clk work.** `clk-hfpll.c` is byte-identical between `6.12/rc` and
`6.18/rc` (so the lock-poll polarity, atomic bound, mode-register mask and
spinlock fixes are in both); `krait-cc.c` has **91 more lines** on 6.12 and
`hfpll.c` 12 more. On top of that, 6.12 carries two clk commits 6.18 never got
(`b306ead1660b` inverted secondary-mux parent order, `a17470165b1c` the lockable
`L_VAL` seed) and the whole validated SPM chain (gang-rail support, the
preempt-disabled write, PWM/4-phase boot, **`c1076b55dd05`** "only cache a
voltage selector the PMIC confirmed", the rail counters) plus the APC sequencer.

**Retraction:** earlier today the `L_VAL` seed was cherry-picked to
`6.18/topic/krait-clk-fixes` "so the fix is not lost". That was churn on a
non-problem — the fix is on 6.12, which is where the DVFS stack will be taken
from at CP7a. The cherry-pick and its branch are retired.

**Retired (local refs only; every one still on `origin`, nothing was pushed, so
`git fetch` restores any of them):**

| Branch | Why |
|---|---|
| `6.18/test/l2-rate-pin` | throwaway `[TEST BRANCH]` L2 rate pin, per the naming convention |
| `6.18/topic/l2-saw-regulator-only` | 3 WIP idle-reset experiments, superseded by `c1076b55dd05` |
| `6.18/topic/cx-corner-idle-reset` | a revert of a 6.18 DVFS experiment; its one useful commit (the CI 6.12/rc watch) was carried to `6.18/staging` first |
| `6.18/topic/krait-clk-fixes` | superseded per above (and the day's redundant cherry-pick) |
| `6.18/topic/hfpll-lock-poll` | its fixes are in both series already |
| `6.18/topic/spm-voltage-fixes` | superseded by the 6.12 SPM chain |
| `6.18/topic/dvfs-spm` | the 6.18 DVFS attempt, including two reverted margin probes |
| `6.18/topic/cpufreq-cx-corner` | CX-corner experiments, one already reverted |
| `6.18/topic/android-dvfs-parity` | OPP/CX-domain parity experiments |

Their commits remain in `6.18/staging` history; only the refs are gone. **When
the remote is tidied** (needs a push, deliberately not done):
`git push origin --delete <each of the nine>`.

**Kept, and why:** `baseline`, `staging`, `rc`; the display campaign —
`fp2-panel`, `mdp-iommu`, `display-carveout` (misnamed: its unique content is the
MDP5 KMS snapshot hook, the carveout revert was abandoned); the display's
prerequisites — `smd-rpm-clocks`, `mmcc-mmssnoc-fix`; `gpu-iommu` (shares the
qcom-iommu driver with the MDP instance); `adsp-sensors`; the instruments —
`pon-reason`, `reset-forensics`, `xpu-err-fatal`; and `ci-dispatch-guard`.

**Consequence for the plan:** CP7a is now unambiguous — 6.18 gets its DVFS by
merging the 6.12 topics, not by reviving anything on the 6.18 side. Nothing that
was retired needs to be read again.

**Also found while auditing the display half (authority 5, static):** 6.18's
drm/msm has **no VBIF support for MDP5 at all** — `vbif` appears only in the
`dpu1` catalogs, and `mdp5_kms.c` maps a single register window (`mdp_phys`),
while the vendor's MDSS node declares `reg-names = "mdp_phys", "vbif_phys"`. So
D6 (§2.8 of the plan) is not a matter of writing eight values: **the VBIF window
is not even mapped on this path**, and the port has to add the register range to
the DT and the driver first. Recorded before writing any code, because it changes
D6 from "program a table" to "add a register window, then program a table".

---

## 2026-08-06 (later) — D9 closed as a parity fix, M2 done, H2 corrected downward

Still no device. Three results, one of which corrects this morning's entry.

### D9 — vendor-parity gap found and closed (authority 1)

**Finding.** The downstream driver halts the MMU before programming a context
bank and does **not** care whether the instance is TrustZone-managed. In
`msm_iommu-v1.c` `__attach_iommu()` (~line 745) the secure sequence is
`msm_iommu_sec_program_iommu()` → `program_iommu_bfb_settings()` →
`iommu_halt()` → `__program_context(..., is_secure, ...)`; only the
*detach*-time halt is gated on `!is_secure` (~line 826). `iommu_halt()` itself
is gated on `halt_enabled`, i.e. the DT property **`qcom,iommu-enable-halt`**,
which `msm8974-v2-iommu.dtsi` sets on **both** MMSS instances — the MDP one
included.

Our port halted only when `qcom_iommu->nonsecure`, so the MDP instance
(`secure-id 1`, single `-ns` bank) had its TTBR0/TCR/MAIR/SCTLR written with the
MMU still running.

**Fixed** on `6.18/topic/mdp-iommu`: `3e22a6c9cecd` (driver, reads the vendor's
own property rather than deriving the behaviour from the security model, and
follows the vendor's log-and-continue timeout semantics so a busy SMMU degrades
to the old behaviour instead of failing the attach and taking the display down)
and `7346f70c0dd8` (DT opt-in). Also releases the halt on every error exit — a
failure after the non-secure halt previously returned with `HALT_REQ` latched,
leaving the SMMU halted for good. **Compile-tested only** (ARM,
`qcom_defconfig` + `CONFIG_QCOM_IOMMU=y`; FP2 DTB builds and carries the
property). No device access.

**Deliberately not claimed as the black-screen cause.** The MDP context-bank
state was already verified on-device against the oracle
(`SCTLR=000010eb TCR=0 FSR=0`), which is evidence *against* corrupted CB
programming. This is a correctness/parity fix that removes a variable; D6 and D7
remain the leading display hypotheses.

**Also verified while there (no change needed):** the DT already gives the MDP
SMMU `power-domains = <&mmcc MDSS_GDSC>` plus `MDSS_AHB`/`MDSS_AXI` clocks, so
the "register write lost to a gated GDSC" version of D9 does not apply.

### D6 — bigger than it looked (authority 5, static)

6.18's drm/msm has **no VBIF support for MDP5 at all**: `vbif` appears only in
the `dpu1` catalogs, and `mdp5_kms.c` maps a single window (`msm_ioremap(pdev,
"mdp_phys")`), while the vendor's MDSS node declares
`reg-names = "mdp_phys", "vbif_phys"`. So porting the vendor's eight
`qcom,vbif-settings` values requires **adding the register range to the DT and
mapping it in the driver first**. Recorded before writing code, because it turns
D6 from "program a table" into a two-part change.

### M2 — done: `docs/analysis/MEMORY-MAP-AUDIT-msm8974pro.md`

**Correction to this morning's seed entry:** it used the **base** MSM8974
numbers. The FP2 is a Pro, and `msm8974pro.dtsi:1758` overrides the hole to
`<0x05a00000 0x7800000>` (not `<0x05d00000 0x07d00000>`), with
`msm8974pro-ion.dtsi` putting the OTHER PIL heap at `0x05a00000 + 0x2100000`
and a MODEM heap at `0x08000000 + 0x5000000`. The H2 window is therefore
**`0x05a00000..0x08000000` (38.6 MB)**, three megabytes lower than recorded.

Everything else in the map reconciles: `0x08000000..0x0d200000` matches
(`mpss` + `mba`), `0x0fa00000..0x0ff00000` matches exactly (smem/tz/rfsa/rmtfs),
and mainline reserves *more* than the vendor at `0x0d200000..0x0fa00000`, which
is harmless. One further vendor-only reservation exists — `mdss_fb0`'s 30 MB
framebuffer at `0x03200000..0x05000000` — but it is not an XPU hazard (see
below); it matters only to the simpledrm control, whose lk2nd framebuffer lives
in that low region.

**H2 corrected downward.** The seed justified it with "PIL carveouts are
XPU-locked by TZ at authenticate-and-reset". The audit shows the vendor's reason
for withholding that window is that **HLOS itself allocates there** (an ION
CARVEOUT heap handed to PIL) — and mainline loads no firmware from it, using the
`0x08000000+` regions we do reserve. So whether any XPU lock covers
`0x05a00000..0x08000000` on *our* boots depends on what SBL/TZ/lk2nd locked
before Linux started, which only O1 can report.

**Consequence, recorded before CP2 runs:** O1 comes first. A reservation that
merely moves an allocation pattern would "fix" the symptom without proving the
mechanism — the definition of a lucky shot. H2's test stays cheap and stays on
the list; it is no longer the leading hypothesis on the strength of this audit
alone.

---

## 2026-08-06 (later still) — D8: there is no working reference for MDP-through-SMMU on msm8974

Authority 3 (sibling ports) and authority 5 (upstream) are **empty** for this
work, which the plan assumed they were not.

**Measured, static:**

| Tree | `iommu@` nodes in `qcom-msm8974.dtsi` |
|---|---|
| mainline v6.12 | **0** |
| mainline v6.16 | **0** |
| mainline v6.18 | **0** |
| pmaports fork `v6.16.12-msm8974` | **0** (and `mdp_iommu` appears nowhere) |
| our `6.12/rc` | 0 (consistent — 6.12 uses the vram carveout) |
| our `6.18/staging` | **3**: `mdp_iommu@fd928000`, `gpu_iommu@fdb10000`, `venus_iommu@fdc84000` |

So the entire msm8974 SMMU device-tree description is **fork-original**, derived
from authority 1, and **nobody upstream or in the sibling fork has ever run the
MDP through its SMMU on this SoC**. No board anywhere wires
`iommus = <&mdp_iommu ...>`.

**What the sibling fork did instead**, on its 6.16 base: `ebe7a4353e07 Revert
"drm/msm: Limit command submission when no IOMMU"` — i.e. it keeps the
carveout/no-IOMMU path alive rather than translating the MDP. That is the same
strategy 6.12 ships here.

**Consequences, recorded so CP6 is planned with its real risk:**

1. **CP6 is a first, not a port.** Comparisons can only come from authority 1
   (vendor register sequences) and authority 2 (the oracle's live MDP/SMMU
   register dumps). When something disagrees, there is no third tree to arbitrate
   — which raises the value of the oracle captures in plan §5 from "useful" to
   "the only referee".
2. **It explains the shape of the failure.** "Attached, zero faults, black" with
   no reference implementation is what a first port looks like when one
   implementation-defined block is still missing — the same signature the BFB
   block and the V7S format each produced before they were found. D6
   (`vbif`/`mdp-settings`, and now known to need a register window added) is the
   next such block in that series.
3. **Base choice, stated once and not re-litigated:** 6.16 still has the vram
   carveout *and* the sibling fork's revert, so a working *drm/msm* display on
   6.16 is a known-cheap path, while on 6.18 it requires this first-ever port.
   The operator has chosen 6.18; this entry only records that the cost
   difference is now measured rather than assumed, so the choice can be revisited
   on evidence rather than on mood.

**Not a dead end:** the fork's revert is irrelevant *on 6.18* — the carveout code
itself is gone there (`eab7766c79fd`), and restoring it is the fight the findings
report already rejected (conflicts in every touched file after the VM_BIND
rework). MDP-through-SMMU remains the only 6.18 path, and it is the one the
campaign is built around.

---

## 2026-08-06 (end of session) — authority 5 swept to the upstream horizon (v6.19.14)

`stable/linux-6.19.y` fetched (it was absent from this clone); v6.19.14
(2026-04-22) is the newest stable branch kernel.org carries, so the sweep now
reaches the upstream horizon. Full result:
`docs/analysis/UPSTREAM-SWEEP-msm8974.md`.

**For this campaign, three conclusions:**

1. **D8 holds at 6.19.** No msm8974 SMMU support and **no MDP5 VBIF/QoS support**
   exists at any upstream version. MDP-through-SMMU on this SoC is still a first,
   and D6 is still ours to write.
2. **One base-move hazard recorded:** `fd714986e4e4` (iommu core: pass the old
   domain to `attach_dev` callbacks) changes the signature our
   `qcom_iommu_attach_dev` implements. Harmless while 6.18 is the base; it is work
   the day we move.
3. **One upstream fix deliberately not adopted:** `6a3908ce56e6` (device leak in
   `qcom_iommu_of_xlate()` — our base releases the reference only on error paths,
   so one refcount leaks per attached master). It only prevents driver unbind,
   which we never do, and it is precisely what stable 6.18.y backports.
   Cherry-picking it ourselves would collide with the automated stable merge on an
   integration branch. **Watch, don't pick.**

**Corroboration worth recording:** upstream 6.19 fixed the msm8974 ADSP to take
the CX power domain (`a1f2c2d55a81` + binding `3d447dcdae53`) — the same change,
for the same reason, that our `adsp-sensors` work already carries on **both**
series. Our fix predates theirs and matches it. When a base move brings that
commit in, our local edit to the resource table becomes redundant and should be
dropped rather than merged around.

**And a reference for D5:** `84df51667a19` adds `simple-framebuffer` to a mainline
msm8226 board **in the board DT**, which is worth comparing against our
lk2nd-injected `/chosen/framebuffer` when the simpledrm glitches are picked up.

---

## 2026-08-06 (7.x sweep) — upstream is reviving msm8974, and it lands on our display path

**Correction first:** the previous entry called v6.19.14 "the upstream horizon".
Wrong — stable carries `linux-7.0.y` and `linux-7.1.y`, and 42 `v7*` tags were
already in this clone. The earlier check used a grep pattern that only matched
`linux-6.*`. Horizon is **v7.1.6**.

**7.x contains a deliberate effort to make msm8974 work on modern kernels**
(March 2026): 15 commits, of which the interconnect series is the one that matters
to us. Full table in `docs/analysis/UPSTREAM-SWEEP-msm8974.md` §6.

### Why it matters

Our display root cause #2 was "`qnoc-msm8974` fails to probe unless the msm8974
**RPM bus clocks are re-added**" (`6.18/topic/smd-rpm-clocks`, now in
`6.18/baseline`). **Upstream went the other way: fix the driver, delete the
clocks** (`aa60d907b3c2` switches msm8974 to the main icc-rpm driver;
`6453ad0865b6` drops the bus clocks from the DT as an abuse of internal NoC
clocks). So our workaround is not merely non-upstreamable — it is the inverse of
the accepted fix.

Two statements in upstream's own commit message bear on open questions:

1. `get_bw` returns 0 "to prevent initial setup from programming INT_MAX into the
   RPM (**which otherwise might hang the platform**)". Compare our own DT comment:
   the MDP SMMU node stays `disabled` in the SoC dtsi because probing it before
   the display stack is up "touches the MMSS bus un-arbitrated and resets the
   SoC". **Candidate shared mechanism** → ACU N2.
2. It ignores `-ENXIO` from firmware "until the **QoS programming** is sorted
   out" — independent upstream confirmation that MDP/bus QoS does not exist
   upstream, i.e. **D6 is missing, not hidden**, and N1 should precede it.

### N1 trial-backport — feasibility answered, not assumed

Branch **`6.18/topic/icc-rework`** off `6.18/baseline`:

- **10 of 11 commits cherry-pick clean** (`-x`): the 9 interconnect/binding
  commits plus `6453ad0865b6` (DT bus-clock drop).
- **1 conflicts and is skipped:** `df7c440c904f` (SoC-wide rpmpd migration)
  collides with our own equivalent from `adsp-sensors`. Expected, not a problem.
- **Compile-tested:** `drivers/interconnect/qcom/msm8974.o` builds, FP2 DTB
  builds, and the DTB verifies the intended shape — five NoC nodes with **no**
  clocks, `interconnect@fc478000` (MMSSNOC) keeping a single `"bus"` clock.
- **UNTESTED ON HARDWARE.**

**Nuance found while checking whether `smd-rpm-clocks` becomes redundant:** it
does **not** become fully redundant. After the rework exactly one DT reference to
an RPM bus clock survives — `RPM_SMD_OCMEMGX_CLK` on the `qcom,msm8974-ocmem`
node — and **upstream v7.1.6 keeps the identical reference**. So drop the NoC
bus-clock part only; do not delete the topic wholesale.

### Pre-registered predictions for N1 (before any device runs it)

1. `qnoc-msm8974` probes **without** the re-added NoC bus clocks, and the display
   controller no longer waits forever on MMSSNOC.
2. If the INT_MAX hazard was biting us, MMSS-related resets change — possibly
   including the constraint that keeps our MDP SMMU node `disabled` until the
   display stack is up.
3. If neither changes, N1 is still correct (it aligns us with upstream and removes
   a workaround) but explains nothing, and D6/D7 remain the display leads.

### Also settled by this sweep

- **D8 holds at v7.1.6:** still **zero** `iommu@` nodes for msm8974 in the newest
  kernel that exists. No upstream SMMU support has ever shipped for this SoC.
- **FP2's MDP5 support survives 7.x pruning:** `a6f081ec4ce6` removed MSM8974**v1**
  and renamed v2 to plain `msm8x74_config`, matched at `.revision = 2` — which is
  the FP2. `429ebd815bbc` (drop single-flush) and `e224e3a167bc` (drop MDP5 1.0
  workarounds) are base-move checks, not fixes.
- **TSENS ack/re-arm storm still unfixed at 7.1.6** (only an `lmh` IRQ-flag change
  in the whole range). Third confirmation; that fix is ours.
- **Rebase hazard:** `2026159372bb` (`iommu/qcom`: scoped OF child loop) touches
  the child-loop region our port modifies.

---

## 2026-08-06 (reuse hunt) — D8 CORRECTED: a working sibling reference exists, and it confounds the V7S decision

**Correction to this morning's D8 entry.** It concluded "there is no working
reference for MDP-through-SMMU on msm8974 … authority 3 is empty for this work".
The literal claim (no *msm8974* board wires `iommus = <&mdp_iommu …>` anywhere)
still holds, but the conclusion drawn from it was wrong and it shaped the plan:
**mainline runs an MDP through this very driver on a sibling SoC.** I concluded
"no reference" from a query scoped to msm8974 instead of to the driver. That is
the mistake the wall protocol exists to prevent, made while writing the wall
protocol into a plan.

### Reference A — msm8916 `apps_iommu` (mainline, arm64 `msm8916.dtsi`)

```
mdss_mdp: display-controller@1a01000 { iommus = <&apps_iommu 4>; }
apps_iommu: iommu@1ef0000 {
	compatible = "qcom,msm8916-iommu", "qcom,msm-iommu-v1";
	ranges = <0 0x01e20000 0x20000>;   reg = <0x01ef0000 0x3000>;
	qcom,iommu-secure-id = <17>;
	iommu-ctx@3000 { compatible = "qcom,msm-iommu-v1-sec"; };   /* VFE  */
	iommu-ctx@4000 { compatible = "qcom,msm-iommu-v1-ns";  };   /* MDP_0 */
};
```

This is **our exact shape** — a `secure-id` instance whose MDP context bank is
**non-secure**, which is what the findings report called "exactly the MDP shape"
and what our bug #8 fix (`restore_sec_cfg` must not be gated on a `-sec` child
existing) makes msm8974 behave like. It also confirms the convention our port
relies on: `asid == SID == CB index` (SID 4 ↔ CB at +0x4000). And msm8916 display
is validated in the field by every pmOS msm8916 device.

Reusable: the DT shape and conventions, and the fact that the driver's secure-id +
`-ns`-bank path is a *working* path rather than an untrodden one.

### Reference B — apq8064 `mdp_port0/1` + `drivers/iommu/msm_iommu.c` (mainline)

`iommus = <&mdp_port0 0 …>` on the display node, four instances
(`mdp_port0`, `mdp_port1`, `gfx3d`, `gfx3d1`, `compatible = "qcom,apq8064-iommu"`),
driven by the only mainline IOMMU driver that programs **`ARM_V7S`**
(`msm_iommu.c:351 alloc_io_pgtable_ops(ARM_V7S, …)`). Krait-era hardware, one SoC
generation before ours.

Reusable: an upstream-shaped V7S configuration, if we keep V7S.

### The decision this changes — new ACU I8 (V7S vs LPAE)

**msm8916's MDP works with `ARM_32_LPAE_S1`** — the only format mainline
`qcom_iommu.c` allocates. Our port forces V7S for the msm8974 secure instance on
vendor authority *plus* one on-device observation: "with LPAE the bank translates
garbage: no fault, black scanout".

**That observation is confounded.** It was made before the `restore_sec_cfg`
gating bug was found, and an uninstalled stream mapping makes traffic bypass
translation entirely — producing no fault and a black screen **whatever the
page-table format is**. So V7S may be carrying credit that belongs to bug #8.

Pre-registered, before any device runs it: with the gating fix in place, allocate
LPAE instead of V7S and change nothing else.

- **LPAE works** ⇒ delete the V7S path. That removes the largest hand-rolled
  divergence in the port and makes it plausibly upstreamable.
- **LPAE still black** ⇒ V7S stands on evidence instead of authority alone, and
  reference B shows how to carry it in an upstream shape.

Either outcome is worth more than the current state, where the two changes are
credited jointly and neither is isolated.

### Reference C — `backup-iommu-msm8974`, and a warning

The 6.15-era ancestor of this work (April 2026, 8 commits: MDP/GPU/Venus IOMMU
nodes, the wirings, defconfig, `pm_runtime_resume_and_get`, and the non-secure
SCM/`INTR_SEL_NS` skip). It is **local-only — not on origin** — so unlike the nine
branches retired today, a `git fetch` cannot bring it back. **Do not delete.**

---

## 2026-08-06 (panel focus) — the oracle's MDP5 reference does not exist, and D7 gets specific

Operator directive: **make the panel work with drm/msm on 6.18 first.** Static
work only, no device.

### The evidence base has a hole exactly where the bug is

`artifacts/fairphone2/oracle/20260731-mdp-regs/` was believed to hold the working
stack's MDP register state. It does not:

- **All four files are byte-identical** (`md5 7cc296b5…`). The offset/length
  arguments were ignored; the same window was written four times.
- That window is **16 lines (0x0..0xFF) with exactly one non-zero word**:
  `10020001` at offset 0, i.e. MDP5 `HW_VERSION`. Everything else reads zero —
  including `DISP_INTF_SEL` (+0x4) and `SMP_ALLOC_W` (+0x80, 8×4), which **cannot
  both be zero on a display that is actively scanning out**. So the base/stride
  was wrong as well as the offsets.
- The **DSI capture is fine** (`oracle-dsi0.txt`, 168 non-zero words, plausible
  lane/timing values), which is why "DSI matches the oracle" was a sound claim.

Consequence: the D8 statement that "the oracle is the only referee" has a hole —
for **MDP5**, the referee was never recorded. And MDP5 is precisely where the
failure is bracketed: DSI matches, panel answers DCS, TE pulses, MDP5 commits and
flushes, zero IOMMU faults, nothing on the glass.

**Fixed for next time:** `~/Projects/msm8974-scratch/rig/mdpdump.sh` — dumps the
authority-1 window set (`msm8974-mdss.dtsi`: MDP top, CTL0/1, VIG0/RGB0/DMA0
pipes, LM0, DSPP0, INTF1, DSI0) and **refuses to pass silently**: it checks that
the requested offset appears in the output, that a window has more than one
non-zero word (else the MDSS clocks are off or the base is wrong), and that no two
windows are identical — the exact 2026-07-31 failure. Syntax- and
arithmetic-tested locally.

### D7, made specific: two mechanisms produce "black, not garbage, with pp done"

Both are invisible as faults, which fits every symptom we have:

1. **SMP starvation.** MDP5 on this SoC has a Shared Memory Pool
   (`MDP_CAP_SMP`; vendor `qcom,mdss-smp-data = <22 4096>`; the FP2's
   `msm8x74_config` declares `mmb_count = 22, mmb_size = 4096` with per-client
   allocations). If the pipe feeding the framebuffer has no/insufficient SMP
   blocks allocated, it fetches nothing and **sends a black frame** — the transfer
   completes, `pp done` fires, and the panel faithfully displays black. Read
   `SMP_ALLOC_W/R` (+0x80 / +0x130) and compare against the pipe in use.
2. **Layer never staged / mixer outputs border.** If `CTL_LAYER_n` does not stage
   the pipe into the mixer, or `LM_BLEND_OP_MODE`/`LM_BORDER_COLOR` leave the
   border showing, the mixer emits black regardless of what the pipe fetched. Read
   `CTL_LAYER`, `CTL_FLUSH`, `LM_OUT_SIZE`, `LM_BLEND_OP_MODE`,
   `LM_BORDER_COLOR`.

Note which hypotheses this **demotes**: instrumenting DSI byte counters (the
earlier D7 formulation) tests a later stage than the suspected one, and the
"8 MB of /dev/urandom into /dev/fb0 changed nothing" observation is consistent with
both mechanisms above — a pipe that fetches nothing, or a mixer that ignores it,
produces black no matter what the buffer holds. Neither requires the IOMMU to be
wrong, which is consistent with zero faults and with oracle-matching CB state.

### Next actions for the panel, in order

1. **Re-capture the oracle MDP5 state** with `mdpdump.sh` (display on, root) —
   one command, self-verifying. This is the missing referee.
2. **Dump the same windows from our failing 6.18 boot.** The MDP5 KMS snapshot
   hook (`e6ed2bc74841`, on `6.18/topic/display-carveout`) already exists; extend
   it to emit the same window set in the same format so the comparison is a diff,
   not an interpretation.
3. **Diff, and read SMP first.** If SMP allocation differs, that is the answer and
   it is a driver fix, not an IOMMU one.
4. Only then D6 (VBIF/QoS) and I8 (V7S vs LPAE), which remain queued and are
   unaffected by this.
