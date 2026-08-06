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
