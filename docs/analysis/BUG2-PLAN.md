# Bug #2 (cross-core gang race) — implementation & test plan

## Diagnosis (confirmed via differential method, Android = control)
- The DVFS reset has TWO independent causes. #1 (CX-corner per-transition vote)
  is fixed by **fix B** (pin all CPU OPP required-opps → super_turbo). #2 remains.
- **#2 = concurrent per-core DVFS transitions racing on the shared saw_l2 rail.**
  - 4-core ASYNC hammer (cores at persistently different freqs): fork RESETS,
    Android SURVIVES (same shell hammer, parallel). Lockstep + single-core survive.
  - The shared rail IS correctly max-aggregated (verified: cpu0=2265 + cpu1-3=300
    → PMIC_STS/VCTL = 0xD4 high; all-300 → 0xA0). So NOT a voltage brownout.
  - All 4 cores run on BHS on the one shared rail (APC PWR_GATE_CTL=0x403F3F7F,
    LDO powered down) — per-core LDO unused.
- **Vendor difference (krait-regulator.c):** ONE global mutex
  (`krait_power_vregs_lock`) is held across the ENTIRE transition — shared-rail
  SPM write + every core's LDO/BHS mux — so only one core transitions at a time,
  rail is only ever written to `get_vmax()`, with strict rail-vs-mux ordering
  (raise rail before moving onto it; lower after). Per-core LDO is POWER ONLY,
  not the anti-glitch (subagent-confirmed). The fork serializes only the SAW
  register write (gang-lock), NOT the whole cross-core transition → race.

## Fix options
- **Option A (minimal, correctness — RECOMMENDED first):** serialize the whole
  per-core CPU DVFS transition (regulator + clk) across all 4 cores so only one
  transitions at a time, matching the vendor's single mutex. Keep the shared
  saw_l2 rail + existing max-aggregation. Smallest change that removes the race.
- **Option B (full vendor-faithful, power):** port per-core Krait LDO/BHS
  regulators (krait0-3, APC ACS base 0xf9088000 + n*0x10000, MDD 0xf908a800+...,
  APC_PWR_GATE_CTL=+0x14 LDO_BYP, APC_LDO_VREF_SET=+0x18, MODE=+0x1C). Lets low
  cores run below the shared rail (power) + reduces shared-SAW traffic. Big new
  driver; still needs A's serialization for the BHS/shared-rail part. Follow-up.

## Implementation plan (Option A)
1. Add a global serialization so no two CPUs run dev_pm_opp_set_rate (rail+clk)
   concurrently. Candidate mechanisms to evaluate at code time (pick cleanest):
   a. small fork cpufreq shim wrapping the target with a global mutex, or
   b. a global mutex bracketed by a CPUFREQ PRE/POST transition notifier, or
   c. serialize inside the OPP/regulator path shared by the 4 CPUs.
   Must be a MUTEX (transition includes HFPLL relock ~ms; cannot hold the raw
   gang-lock spinlock that long).
2. Preserve fix B (CX pin) — orthogonal, still required.
3. Keep it minimal + upstreamable; no per-core LDO in Option A.

## Test plan (DIFFERENTIAL — every test + read on BOTH devices; judge by the difference)
Control invariant: Android is known-good on identical silicon; the fix is proven
only when the fork MATCHES Android under the same stimulus.
- **T0 reproduce (done):** shell async hammer → fork resets, Android survives.
- **T1 fix (async):** shell async hammer (4-core, persistent disparity) on
  fork(fixed) must survive ≥30 min in parallel with Android surviving too.
- **T2 regression:** single-core + 4-core lockstep still survive (fix B intact).
- **T3 differential reads:** rail setpoint 0xf9012014/0xf901201C + per-core APC
  0xf90N8014/18 read the SAME way on both, at rest and under disparity — the
  fixed fork must behave like Android (rail tracks max; no wedge).
- **T4 realistic soak:** bursty real load (not torture) on both, long soak;
  fork(fixed) must not reset where Android doesn't.
- **T5 §1.1 gates:** GPU IOMMU 0 faults, remoteprocs up, QRTR/network, DVFS full
  range, 60min+ growing-uptime soak; then topic→staging.
Metric everywhere: fork(fixed) resets iff Android resets (ideally neither).

## Notes
- Kept clean: fix B built from source = dtb-fixB (md5 8f70fe12). Currently on the phone.
- Portable shell hammer.sh on both devices (Android /data/local/tmp, fork /home/citro).
- clk-hfpll 10ms lock-wait + spm.c gang-lock WIP are separate, harmless, keep.
