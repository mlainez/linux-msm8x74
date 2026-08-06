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
