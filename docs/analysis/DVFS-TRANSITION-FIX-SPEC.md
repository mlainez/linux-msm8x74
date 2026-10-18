# MSM8974 (FP2) DVFS-transition reset — analysis + fix spec

Status: analysis before implementation (Marc: "be thorough, spec the behaviour
first"). Sources: live Android vendor kernel (adb 81314fe6), vendor BSP
(~/Projects/fairphone2-kernel), our fork (6.18/topic/l2-saw-regulator-only).

---

## 1. Confirmed mechanism (what we're fixing)

- The reset is a **per-DVFS-transition event**; probability scales with the
  **ΔV / frequency span** covered, and is **worst from a settled state**.
  nr_cpus=1 hammer (clean, robust tool):
  - span ≤ ΔL_VAL 40 (≤~200 mV): SURVIVES 400k–600k flips (incl. reaching top
    OPP 2265.6 and bottom 729.6, incl. crossing the VCO band at 1248 MHz).
  - full span L38↔L118 (729.6↔2265.6, ~350 mV): DIES ~1000–4400 flips, whether
    direct, stepped-through-1497.6, fast, or 50 ms-settled.
- At field DVFS rates (~1/s) a ~1/1000–1/4400 per-transition probability ≈ the
  observed idle MTBF (min–hours). Hammer = valid accelerator.
- **RULED OUT:** shared-rail/core-count (nr_cpus=1 dies same as 4-core), VCO
  band (mid-band crossing survives), low-freq/low-voltage regime (729.6↔960
  survives), Linux lock serialisation (gang-lock: no change), SMPS phase
  *capacity* as tested (forcing 4-phase+PWM: no help — but see §4, that test was
  compromised), voltage-settle/ramp (we already over-wait vs vendor;
  intermediate stepping WITH 50 ms settle still dies), per-core LDO / Stage C
  (vendor does the full span **all-BHS** and survives — LDO not the reason).
- **★ Vendor survives the identical hammer** (40000 full-span flips, zero reset)
  ⇒ real, fixable per-transition defect in OUR transition path, not HW.

## 2. Vendor DVFS-transition behaviour (exact)

On msm8974 the L2/APCS SAW @ 0xf9012000 is the single voltage+phase+FTS master
(qcom,L2-spm-is-apcs-master). All control is VCTL writes differing only by the
port field (VCTL bits[18:16]); PMIC_STS reflects data only for port 0.

**Every CPU DVFS voltage change runs `krait_power_set_voltage` → `_set_voltage`
under the global `krait_power_vregs_lock` mutex:**
1. gang voltage = max(all cores' uV), rounded to LV_RANGE_STEP(5000).
2. rise: set gang voltage first → mb → udelay(ΔV/SLEW_RATE, 2395 uV/us) → then
   per-core LDO/BHS; fall: LDO/BHS first → then lower gang.
3. **`pmic_gang_set_phases(coeff_total)`** — runs on EVERY transition (§2a).
4. gang-voltage write = `msm_spm_set_vdd(0, setpoint)` = SAW VCTL port 0 → poll
   PMIC_STS[7:0]==vlevel (== our smp_set_vdd_v2_1_l2, register-identical).

### 2a. Phase + FTS management  (pmic_gang_set_phases, krait-regulator.c:462)
Runs every transition. `n_online` = # enabled cores; `load_total` = Σ per-core
load; `coeff_total` from §2b.
- Gated: does nothing until every online core has cast its first vote
  (manage_phases latches true once no core still has online_at_probe set).
- **PFM path:** if `load_total <= pfm_threshold(76) && n_online == 1 &&
  krait_pmic_is_ready()`: switch FTS→PFM (msm_spm_enable_fts_lpm(0x00)) if not
  already; return (no phase change). [= light-load / near-idle single core]
- Else if currently PFM: switch FTS→PWM (msm_spm_enable_fts_lpm(0x80)),
  udelay(50).
- **Phase count:** coeff_total <1e6 →1 ; <2e6 →2 ; else →4. Cap at n_online.
  If changed: write phases (msm_spm_apcs_set_phase(count-1)) = SAW VCTL port 1,
  value=count-1, poll PMIC_STS FSM idle (PMIC_STS[17:16]==0); if raising count,
  udelay(50) after.
- Register: FTS mode = VCTL port 2 (bits[18:16]=2), data byte 0x00 PFM/0x80 PWM.
  Phase = VCTL port 1 (bits[18:16]=1), data byte = count-1 (0/1/3). Both poll
  the PMIC-FSM-idle state, NOT the data (data only updates for port 0).

### 2b. Coeff math (krait-regulator.c:344)
- `coeff2(uV) = (uV<=850000 ? 612229*mV/1000-211258 : 892564*mV/1000-449543) *
  sf/4`  (sf=4 default; mV=uV/1000). ~voltage-only term, ~300k–500k/core.
- `coeff1(actual_uV,req_uV,load) = 330*load + load*673*(actual*1000/req)/1000`.
  ~load term; load in the DT's `ua` units (single busy core ≈ hundreds, idle
  ≈ tens; pfm_threshold=76 ⇒ PFM when ~idle).
- BHS core: coeff uses pmic_vmax for actual_uV. LDO core: uses uV-ldo_delta.
- **load source:** DT voltage plan gives (fmax, uv, **ua**) per level;
  clock.c:93 `regulator_set_optimum_mode(r, ua[level])` → DRMS →
  krait_power_get_optimum_mode → kvreg->load → coeff. So load is a per-OPP DT
  constant, not measured.

### 2c. Other per-transition vendor behaviour
- `set_cpu_freq` boosts the calling task to **SCHED_FIFO** while INCREASING freq
  ("avoid cpu starvation in the acpuclk_set_rate path"); restores after.
- L2 SPM sequencer kept **armed** (saw2-spm-ctl=0x1) with pc/ret/gdhs seqs +
  pmic-data. (Our WIP disarms it.)
- SLEW_RATE 2395 uV/us (we use ramp_delay 1250 = wait longer).

## 3. Our fork's transition behaviour (6.18)
- cpufreq-dt → dev_pm_opp_set_rate(target): regulator_set_voltage(saw_l2_vreg,
  ramp_delay 1250 → OPP core waits ΔV/1250) + clk_set_rate (krait-cc → HFPLL,
  identical set_rate to vendor). NO phase mgmt, NO FTS PFM/PWM switching, NO
  per-core load, NO SCHED_FIFO. L2 SPM sequencer DISARMED (WIP regulator_only).
- SMPS phase/FTS = whatever the bootloader + platsmp left (platsmp writes port 1
  = 4 phases at boot, static; FTS mode unknown/static).
- All-BHS (platsmp forces BHS) — same as vendor.

## 4. Gap analysis — ranked candidates for the per-transition defect
Vendor==us on: all-BHS, OPP voltages (per bin), HFPLL set_rate, ramp (we wait
MORE). So the defect is in what the vendor does per-transition and we don't:

1. **Dynamic FTS PFM/PWM + phase management (§2a) — TOP.** The vendor
   reconfigures the SMPS mode/phases on every transition; we never touch them so
   the SMPS runs a STATIC config (4 phases + whatever FTS mode) across the whole
   span. A multi-phase buck is typically UNSTABLE at light load; the correct
   light-load mode is PFM / fewer phases. If our static config is PWM/4-phase, it
   is mis-configured at the light-load (low-freq) end, and a big excursion into
   that regime glitches. NB: the earlier "force 4-phase+PWM didn't help" test was
   (a) 4-core + fragile hammer, (b) platsmp already 4-phase so it was ~no-op, and
   (c) forced MORE phases — the OPPOSITE of the light-load fix. It did NOT test
   dynamic PFM/fewer-phases. So this is UNVALIDATED, not refuted.
2. **Armed L2 SPM sequencer (§2c).** Vendor keeps it armed; we disarm. Our
   earlier "arming made it worse" was arming the wrong way (STBY, no pmic-data,
   fighting the manual VCTL writes). Lower priority; revisit if #1 fails.
3. **SCHED_FIFO on increase (§2c).** Trivial to add; on nr_cpus=1 (no preemptor)
   it shouldn't matter, and the field bug is slow DVFS — low priority, but free.
4. SLEW_RATE — refuted (we wait longer).

## 5. Behaviour to implement (spec) — dynamic FTS/phase management in spm.c

Goal: on every L2-SAW voltage change, additionally set the SMPS FTS mode and
phase count to match the operating point, via the SAW VCTL port-1/port-2 writes,
reproducing the vendor's intent (PFM/few-phases when light, PWM/more-phases when
heavy). This is a self-contained addition to drivers/soc/qcom/spm.c using
registers we already map; NO new driver, NO DT regulator surgery.

### 5.1 New register helpers (spm.c)
- `spm_saw_set_phase(drv, count)`: VCTL = FIELD(PORT,1)|FIELD(VLVL,count-1);
  write; poll PMIC_STS FSM-idle (add SPM_2_1_PMIC_STS_STATE = GENMASK(17,16),
  wait ==0, ~vctl-timeout 50us).
- `spm_saw_set_fts(drv, pwm)`: VCTL = FIELD(PORT,2)|FIELD(VLVL, pwm?0x80:0x00);
  write; poll FSM-idle.
- Both under spm_gang_lock (same lock as the vctl write), RST NOT needed for
  port1/2 (only set_vdd does RST). Order per vendor: FTS mode change before phase
  change; udelay(50) after a PFM→PWM exit and after raising phase count.

### 5.2 Policy (how to pick count + mode without the opaque coeff/load table)
The coeff/load table is bin-specific DT data we don't have cleanly. Two options
for the FIRST cut, to be A/B'd with the hammer:
- **(A) Faithful-lite:** derive a per-OPP (mode,phase) from voltage + online
  count: PFM when n_online==1 AND target voltage <= a low threshold (≈ our
  lowest 1-2 OPP voltages, standing in for pfm_threshold); else PWM. phase_count
  = clamp by a voltage->{1,2,4} map calibrated to the vendor's 1e6/2e6 coeff
  breakpoints (coeff2-only estimate: ~1e6 ≈ ? mV, ~2e6 ≈ ? mV — compute from
  §2b), capped at n_online.
- **(B) Full port:** add a per-OPP `ua` load table (from vendor DT for our bin)
  + the exact coeff math. Higher fidelity, more code + data to source.
Recommend (A) first (small, testable); escalate to (B) only if (A) is close but
imperfect. Both are DT-gated (qcom,use-phase-switching, qcom,pfm-threshold,
phase-port, pfm-port) like the vendor so it's opt-in and upstreamable.

### 5.3 Integration point
Call the FTS/phase update from the L2 regulator's set_voltage path
(spm_set_voltage_sel / after smp_set_vdd_v2_1_l2 lands), i.e. on every CPU OPP
change, under the gang lock, AFTER the voltage write on a rise / matching the
vendor's ordering. Also fold in the SCHED_FIFO-on-increase (§2c) as it's free.

## 6. Open questions / risks
- **Direction of the phase fix (more vs fewer phases).** Hypothesis: light-load
  needs PFM/fewer phases; our static 4-phase/PWM is wrong at the low end. The
  implementation makes it DYNAMIC, so it self-corrects either way. Confirm with
  the hammer.
- **Reading back phase/FTS on the vendor** is not possible (no /dev/mem on
  Android) — can't directly copy the vendor's per-OPP table; hence policy (A).
- **PMIC_STS FSM-idle bits** (port1/2 poll) assumed PMIC_STS[17:16]==0 per vendor
  spm-v2 (msm_spm_drv_get_sts_pmic_state); verify offset on our SAW.
- If dynamic FTS/phase does NOT fix it → next: arm the L2 SPM the vendor way
  (§4.2), then reconsider.

## 7. Validation
- Build combined kernel (XPU + this) ; nr_cpus=1 first for a clean single-var
  test: hammer 729.6<->2265.6 (the fatal full span) — target SURVIVE 600s
  (vendor does 40000+). Then re-enable cores (revert extlinux nr_cpus=1),
  4-core hammer + full §1.1 gates + idle soak.
- Keep the hammer batch (robust tool, base64 deploy) as the harness.
