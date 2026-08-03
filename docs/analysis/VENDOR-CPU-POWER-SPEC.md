# MSM8974 (FP2) vendor CPU-power spec — for the 6.18 structural port

Purpose: capture EXACTLY what the vendor (Android 3.4 BSP + live FP2) does for
Krait CPU voltage/idle, so the 6.18 port replicates it faithfully. Three
sources: live Android FP2 (adb, device 81314fe6), vendor BSP
(~/Projects/fairphone2-kernel), our mainline fork (~/Projects/linux-msm8x74).

Root cause recap: our fork changes the SHARED gang rail (L2 SAW VCTL) with zero
coordination against per-core idle state or SMPS current mode → silent
brownout/FSM race → PS_HOLD. The rail-write primitive is register-identical to
the vendor's; the fix is the missing coordination layer.

---

## 1. Live FP2 device-tree (authoritative per-board values)

### krait-pdn@f9011000  (compatible "qcom,krait-pdn" = the krait-regulator device)
- reg: apcs_gcc 0xf9011000/0x1000 ; phase-scaling-efuse 0xfc4b80b0/8
- qcom,pfm-threshold = 76            (load coeff below which → PFM, if 1 core online)
- qcom,phase-scaling-factor-bits-pos = 16
- qcom,use-phase-scaling-factor      (bool: ON)
- qcom,use-phase-switching           (bool: ON)  ← dynamic phase mgmt enabled
- qcom,valid-scaling-factor-versions = <0 1 0 0>

### per-core regulator@f90{8,9,a,b}8000 (compatible "qcom,krait-regulator") — 4×, identical bar cpu-num/reg
- qcom,cpu-num              = 0 / 1 / 2 / 3
- qcom,ldo-threshold-voltage = 850000 uV   (core uV <= this AND fits headroom → LDO; else BHS)
- qcom,ldo-delta-voltage     =  50000 uV   (LDO output = core_uV - delta)
- qcom,headroom-voltage      = 150000 uV   (gang must be >= ldo_out + headroom for LDO to regulate)
- qcom,ldo-default-voltage   = 750000 uV
- qcom,retention-voltage     = 675000 uV   (== krait-power-regulator/retention_uV live)
- regulator-min/max          = 500000 / 1100000 uV
- reg: acs 0xf90{8,9,a,b}8000/0x1000 ; mdd +0x2800/0x1000   (reg-names "acs","mdd")

### L2/APCS-master SPM  qcom,spm@f9012000 (compatible "qcom,spm-v2")
- qcom,L2-spm-is-apcs-master (bool)   qcom,core-id=0xffff
- qcom,saw2-spm-ctl = 0x1  (sequencer ARMED — our WIP wrongly disarms it)
- qcom,saw2-cfg = 0x14 ; qcom,saw2-spm-dly = 0x3c102800 ; qcom,vctl-timeout-us = 50
- qcom,vctl-port=0  qcom,phase-port=1  qcom,pfm-port=2
- qcom,saw2-avs-ctl/limit/dly/hyst = 0 (AVS OFF — matches our fork)
- qcom,saw2-pmic-data0 = 0x02030080 ; pmic-data1 = 0x00030000
- seqs: saw2-spm-cmd-gdhs = 00 32 42 03 44 50 02 32 50 0f
        saw2-spm-cmd-pc   = 00 10 32 b0 11 42 07 01 b0 12 44 50 02 32 50 0f
        saw2-spm-cmd-ret  = 1f 00 03 00 0f

### per-core SPM  qcom,spm@f90{8,9,a,b}9000
- armed (saw2-spm-ctl set), seqs: saw2-spm-cmd-pc / -ret / -spc / -wfi (the 4 cpuidle states)

### CPU clock  qcom,clock-krait@f9016000 (compatible "qcom,clock-krait-8974")
- cpu0..3-supply = &krait0..3 (phandles 0x5b-0x5e)
- hfpll-analog-supply = pm8841 (0x60) ; hfpll-dig / l2-dig-supply = CX (0x5f)
- qcom,l2-fmax: 1036800000→corner4, 1305600000→corner5, 1728000000→corner7 (approx from hex)

### runtime (live, interactive gov, idle-ish)
- per-core krait regulator voltages: 890/830/935/830-ish mV, modes NORMAL(2)/IDLE(4)
- cpuidle states: C0 wfi, C1 retention, C2 standalone_pc, C3 pc
- rpm_master_stats: APSS numshutdowns=0 (CPU never enters RPM system sleep)

---

## 2. Vendor BSP algorithm  (krait-regulator.c / spm-v2.c / msm-pm.c)

### 2a. krait-regulator.c  (per-core LDO/BHS + gang + phases)
Two platform drivers: `qcom,krait-pdn` (gang parent) + `qcom,krait-regulator`
(4 per-core children, instantiated via of_platform_populate in pdn probe).

**Voltage set (per-core regulator ops, all under gang mutex):**
`krait_power_set_voltage` → round to LDO step if < ldo_threshold → `_set_voltage`:
set kvreg->uV, `vmax = max(all cores' uV)` rounded to LV_RANGE_STEP(5000); if
RISE: `set_pmic_gang_voltage(vmax)` (=msm_spm_set_vdd, the SAW VCTL) → mb →
udelay(dV/SLEW_RATE, SLEW_RATE=2395 uV/us) → `configure_ldo_or_hs_all`; if FALL:
LDO/HS config first → then lower gang. Then `pmic_gang_set_phases`.

**LDO vs BHS decision (configure_ldo_or_hs_one):** use LDO iff
`uV <= ldo_threshold(850000) && (uV - ldo_delta(50000) + headroom(150000)) <= vmax`
and !force_bhs and !ldo_disable; else BHS.

**Switch mechanism (IPI to target core, smp_call_function_single(cpu,...,1)):**
- HW-seq path (KPSS version > 0x20000000 = 2P0 — FP2 qualifies): ONE masked
  write to `APC_PWR_GATE_MODE[6:4]` = mode (PC=0/LDO=1/BHS=2/DT=3/RET=4), then
  udelay(1). set LDO VREF first via APC_LDO_VREF_SET.
- old path (<=2P0): manual APC_PWR_GATE_CTL gate dance (BHS_EN/SEG_EN/LDO_BYP/
  LDO_PWR_DWN) — not needed for FP2 if it is >2P0 (CONFIRM KPSS ver on device).

**APC regs (base = per-core "acs" @ f90{8,9,a,b}8000):** APC_PWR_GATE_CTL 0x14,
APC_LDO_VREF_SET 0x18 (VREF_LDO[6:0], VREF_RET[14:8]; uV=465000+reg*5000),
APC_PWR_GATE_MODE 0x1C (switch mode [6:4]), APC_PWR_GATE_DLY 0x20, CPU_PWR_CTL
0x04 (online bit 0x80). MDD ("mdd" @ +0x2800): MDD_CONFIG_CTL 0x00, MDD_MODE
0x10 (0x2=enable).

**Phase math:** coeff2(uV,sf): mV=uV/1000; uV<=850000: 612229*mV/1000-211258
else 892564*mV/1000-449543; *sf/4 (sf=4 default or efuse). coeff1(act,req,load)=
330*load + load*673*(act*1000/req)/1000. coeff_total=Σ(coeff1+coeff2) over
enabled cores (LDO cores use uV-delta; HS cores use pmic_vmax). phase_count:
<1e6→1, <2e6→2, else 4; cap at n_online. PFM if load_total<=pfm_threshold(76) &&
n_online==1 && krait_pmic_is_ready → msm_spm_enable_fts_lpm(PFM=0x00); else PWM
(0x80). Phase write path = §2b (msm_spm_apcs_set_phase). manage_phases stays
OFF until every online core has cast its first vote (online_at_probe cleared).

**Constants:** PMIC_VOLTAGE_MIN 350000, MAX 1355000, LV_RANGE_STEP 5000,
KRAIT_LDO_VOLTAGE_OFFSET/MIN 465000, KRAIT_LDO_STEP 5000, CORE_VOLTAGE_BOOTUP
900000, LDO_UV_MAX 750000. DT props (per-core, ALL "-voltage" suffix, mandatory):
headroom/retention/ldo-default/ldo-threshold/ldo-delta-voltage, cpu-num,
ldo-disable(bool). pdn: use-phase-switching(bool), pfm-threshold(u32), phase-
scaling efuse props. pmic_min_uV_for_retention = min(retention_uV+headroom_uV),
computed NOT from DT.

**Probe init (glb_init + kvreg_hw_init, boot cpu only for hw_init):**
PWR_GATE_CONFIG(gcc 0x44)=0x0308736E (>2P0), APC_PWR_GATE_DLY=0x30430600,
APC_PWR_GATE_MODE=0x21 (HW seq, BHS), MDD_CONFIG_CTL=0x190, MDD_MODE=0x2. Each
core starts mode=HS, force_bhs=true until online detected. Secondary cores get
BHS init via secondary_cpu_hs_init (+ forces L2 SAW max phases 0x10003@0x1c
until manage_phases on). CPU-hotplug notifier forces BHS around up/down.

**Port caveats:** 3.4-era (register_hotcpu_notifier→cpuhp, __devinit, regulator_
register→devm, mach iomap). Preserve the smp_call_function_single(...,1)
cross-core writes — load-bearing. Calls into SPM: msm_spm_set_vdd,
msm_spm_apcs_set_phase, msm_spm_enable_fts_lpm; into PM: msm_pm_enable_retention.

### 2b. spm-v2.c / spm_devices.c / msm-pm.c

**THE KEY SIMPLIFICATION: phase + PFM/PWM = same VCTL write as VDD, diff port.**
L2 SAW VCTL @ 0xf9012000+0x1c, bits[7:0]=data, bits[18:16]=port:
- port 0 = VDD: write vlevel; poll PMIC_STS[7:0]==vlevel (data updates only for port 0).
- port 1 = PHASE: write (phase_count-1) i.e. 0/1/3 for 1/2/4 phases; poll PMIC_STS[17:16]==IDLE(0).
- port 2 = FTS mode: write 0x00 (PFM) or 0x80 (PWM); poll PMIC_STS[17:16]==IDLE(0).
(pfm-port=2, phase-port=1, vctl-port=0 from DT.) Each write = RST kick not needed
for port1/2 (only set_vdd does RST). msm_spm_apcs_set_phase(n-1) and
msm_spm_enable_fts_lpm(mode) both target the single L2 apcs-master SAW.
⇒ **Stage B (phases+PFM/PWM) is implementable as raw port-1/2 VCTL writes on the
existing saw_l2 — NO per-core regulator infra needed.**

**SPM LPM sequencer (per-core + L2):** seqs loaded once at init into SEQ_ENTRY
(0x80+4i); arm = SPM_CTL(0x30) bit0=1, sequence start index = bits[10:4].
DISABLED = clear bit0. Our fork's disarm = clear bit0 on L2 (vendor keeps it 1).

**set_vdd offsets (v2, = ours):** PMIC_STS 0x14, RST 0x18, VCTL 0x1C, AVS_CTL
0x20, PMIC_DATA_0/1 0x40/0x44, SEQ_ENTRY 0x80, SPM_CTL 0x30, VERSION 0xFD0. AVS
inert on 8974 (avs-ctl=0). apcs-master ⇒ gang write runs locally under
get_cpu()/put_cpu() (no IPI); per-core (non-8974) would IPI to target core.

**Retention (C1):** msm_pm_retention → msm_spm_set_low_power_mode(POWER_RETENTION,
notify_rpm=false) → WFI → restore CLOCK_GATING. Gated by
msm_pm_ldo_retention_enabled; msm_pm_enable_retention(false) IPIs retention_cpus
(empty handler) to force-wake before the LDO retention floor is pulled. Retention
LDO target = 675000 (VREF_RET bits[14:8] of APC_LDO_VREF_SET); arm/disarm
threshold = retention_uV+headroom = 825000 gang uV.
NB: our fork does NOT do retention (only WFI+SPC); SPC fully powers the core off
(no voltage floor), so retention-gating is NOT our reset cause — it's an Android
power-efficiency feature (Stage D, later).

## 4. RECOMMENDATION: Stage B (phases + PFM/PWM) FIRST

**Recommend B before C.** Reasons:
1. Evidence: SPC-off made the hammer die FASTER (more active cores = worse) =
   current/droop signature → the SMPS current-capacity controls (phase count +
   PFM/PWM) are the most-supported mechanism. Per-core LDO (Stage C) is about
   per-core efficiency/decoupling, weaker link to the reset.
2. Cost: Stage B = raw port-1/2 VCTL writes on the EXISTING saw_l2 (§2b). No new
   driver, no DT surgery, no framework port. Small diff, fast hammer test.
3. Decisiveness: one build cycle tells us if current management is the fix. If
   yes → cheap structural win. If no → escalate to Stage C (per-core regulators,
   the big backbone), now justified by evidence.

**Confirm-before-code (de-risk):** the phase/PFM path was only ever tested as
STATIC PWM under LOAD (refuted for the load bug). Phase COUNT (port 1) and
PFM/PWM under the IDLE HAMMER were never tested. Before writing driver code, do
a 10-min on-device confirmation: force max phases (port1=3) + PWM (port2=0x80)
via /dev/mem on the test phone, run the freq-hammer, compare flips-to-death vs
~4800 baseline. Survives/much-better ⇒ build Stage B in the driver. (This is a
diagnostic to aim the structural work, not a shipped patch.)

**Stage B driver shape (if confirmed):** in drivers/soc/qcom/spm.c, add L2-SAW
phase + FTS-mode control (port-1/2 VCTL writes + PMIC_STS FSM-idle poll); drive
it from the CPU-voltage/regulator path by a load/online-cpu proxy (start simple:
phase_count = clamp(n_online → 1/2/4); PFM when 1 online + low OPP, else PWM).
Later refine toward the vendor coeff math. DT-gate it (qcom,use-phase-switching,
qcom,pfm-threshold, phase-port, pfm-port) like the vendor.

**Stage C/D (later, if needed):** C = per-core qcom,krait-regulator driver
(LDO/BHS via APC_PWR_GATE_MODE HW-seq on KPSS>2P0, gang=max, ordered rise/fall,
global lock); D = arm L2 SPM seq the vendor way + C1 retention state + retention
gating. Full fidelity = A+B+C+D = exactly Android.

## 5. EXPERIMENT RESULTS + FINAL DECISION (2026-07-27 late)

**Stage B (phase/PFM) REFUTED.** Forced 4-phase+PWM hammer: 4600/6800/10400
(median ~6800) vs ~4800 baseline — within 7x noise, no survival. Confound:
platsmp.c already sets 4 phases at boot, so this only tested PWM. Forcing MAX
current capacity does nothing ⇒ not a current-capacity fix.

**Settle/ordering hypothesis REFUTED without a build.** Our spm.c L2 reg_data
already has `.ramp_delay = 1250` (uV/us) + `.set_voltage_time_sel =
regulator_set_voltage_time_sel`, so the OPP/regulator core waits ~280us after a
350mV rise — LONGER than the vendor's 146us (SLEW_RATE 2395). We over-wait, not
under-wait. Undervolt-on-rise is not the cause.

**All cheap hypotheses exhausted** (Linux lock, phases/PFM/PWM, settle). Each
should have helped if the mechanism were simple; none did. Register write is
vendor-identical; ramp is over-waited; current capacity is maxed. What remains
is the STRUCTURAL difference: our 4 cores are hard-tied to the shared gang rail
(BHS always on, platsmp forces it), so every VCTL transient hits all cores;
Android's per-core LDO/BHS + armed sequencer + retention is a coordinated
subsystem. SPC-being-protective (collapsed core = BHS-gated = disconnected from
rail) is the standing evidence that connection-to-rail-during-transient is the
hazard.

**DECISION: build Stage C (per-core krait regulators) then D (armed seq +
retention) = the full vendor subsystem = what Android proves stable.** No more
cheap experiments — exhausted. This is the guaranteed structural path Marc
directed. Multi-session build; validate each increment with the hammer.

## 3. Mapping to mainline 6.18 landing points

### Our fork today (6.18/topic/l2-saw-regulator-only)
- DT: all 4 CPUs `cpu-supply = <&saw_l2_vreg>` — ONE shared regulator; the
  regulator core aggregates consumers to MAX = the gang rail. (dtsi ~L48-117)
- `saw_l2: power-manager@f9012000` "qcom,msm8974-saw2-v2.1-l2" + child
  `saw_l2_vreg: regulator` → drivers/soc/qcom/spm.c smp_set_vdd_v2_1_l2 (=gang
  VCTL write, register-identical to vendor msm_spm_set_vdd). (dtsi ~L1996)
- per-core `saw0..3` @ f9089000/f9099000/f90a9000/f90b9000
  "qcom,msm8974-saw2-v2.1-cpu" → used by cpuidle for SPC. (dtsi ~L2087+)
- cpuidle-qcom-spm.c: 2 states only — WFI (state0) + SPC (qcom,idle-state-spc);
  qcom_cpu_spc: set SPC → cpu_suspend → set STBY. NO retention, NO
  standalone/full-pc split.
- OPP: per-bin opp-microvolt (speed1-pvsN); OPP core sets saw_l2_vreg voltage.
- WIP a765dd4cb8f5: L2 SPM sequencer DISARMED (regulator_only) + spm_gang_lock
  (both to be REPLACED by this port).

### Gap → landing point
| vendor mechanism | our fork | 6.18 landing point |
|---|---|---|
| gang rail = SAW VCTL | HAVE (saw_l2_vreg) | keep; becomes per-core regs' parent supply |
| per-core krait reg (LDO/BHS) | none | NEW driver drivers/regulator/qcom-krait-regulator.c + 4 DT nodes (acs @f90{8,9,a,b}8000, mdd +0x2800); cpu-supply → &krait0..3, krait*-supply → &saw_l2_vreg |
| dynamic phases + PFM/PWM | none | extend spm.c (phase-port=1, pfm-port=2) or the new reg driver; pfm-threshold 76 |
| retention (675mV) gating | none | add C1 retention idle-state + gate rail<->idle like msm_pm_enable_retention |
| armed L2 SPM seq (spm-ctl=1) | disarmed (WIP) | arm with vendor pc/ret/gdhs seqs + pmic-data (undo disarm) |
| global mutex | regulator-core mutex | the new krait reg driver's lock (like krait_power_vregs_lock) |

Note: mainline has NO krait-power-regulator (never upstreamed for 8974). This
is a fresh port; frameworks differ (3.4 → 6.18 regulator/cpuidle/genpd), so it
is a re-implementation of the vendor ALGORITHM, not a copy of the code.

## 4. Recommendation: Stage B (phases) vs Stage C (per-core regulators) first
<!-- filled after §2 agents return -->
- Evidence leaning so far: SPC-off made the hammer die FASTER (more active
  cores = worse) = current/droop signature → favors phase/PWM mgmt (Stage B)
  as the cheapest high-value slice. Confirm against §2 (does phase mgmt need
  the per-core reg infra, or can it stand alone on the shared saw_l2?).
