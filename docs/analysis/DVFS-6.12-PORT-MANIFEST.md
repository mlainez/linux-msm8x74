# DVFS 6.12 port manifest (recon 2026-07-31)

Fork tip `1a41629d4758` (6.18/topic/cx-corner-idle-reset) → 6.12 base.
6.12/baseline ALREADY has rpmpd conversion; missing = whole Krait CPU-DVFS layer.

## Driver commits to cherry-pick (oldest→newest)
- clk-hfpll `84d91d97e49b` atomic lock poll + polarity + WARN
- clk-hfpll `23a938ed3c59` mask mode reg before test
- hfpll.c `fd9189a2ec0b` msm8974 hfpll_data + match (FOUNDATIONAL, also DT+spm)
- hfpll.c `7fc3ec8151dc` fast_io  → **ALREADY IN 6.12, SKIP**
- clk-krait `fc54bd8e3a0c` u8 parent index guard
- krait-cc `ddd34a589a30` ABORT_RATE_CHANGE restore
- krait-cc `8c8328961a45` apu_aux→acpu_aux
- cpufreq-nvmem `a474697820e4` krait CX PD → **TRANSLATE to 6.12 genpd_names API**:
  `static const char *krait_genpd_names[] = {"cx", NULL};` `.genpd_names = krait_genpd_names;`
- spm.c cluster (ALL absent in 6.12): `401fec35e189 609e2c283b38 2bc1999098fb
  8ec3dd7bb397 aa2ed4e190b5 d2e1d8bf08e7 7e336d0ec0eb 29018f8942a3 0e91a5cc7bec`
  (a0386dc2a86e margin is neutralised by 0e91a5cc — net no margin)

## DT delta (net, on 6.12/baseline qcom-msm8974.dtsi)
- cpu_opp_table (operating-points-v2-krait-cpu, nvmem speedbin, 33 OPP rows,
  every row required-opps=<&rpmpd_opp_super_turbo> PINNED, opp-supported-hw
  0xf up to 2265600000, 0x8 for top 3; per-bin opp-microvolt-speed{1,3}-pvs{0..15}-v{0,1})
- CPU nodes: clocks=<&kraitcc 0..3>, operating-points-v2, cpu-supply=<&saw_l2_vreg>,
  power-domains=<&rpmpd MSM8974_VDDCX_AO>, power-domain-names="cx", #cooling-cells=2
- hfpll0..3 @f908a000/f909a000/f90aa000/f90ba000, hfpll_l2 @f9016000
- kraitcc: qcom,krait-cc-v2, clocks=hfpll0..3+hfpll_l2
- saw_l2_vreg regulator child under saw_l2 (350000..1275000)
- qfprom_speedbin efuse@fc4b80b0 + speedbin_efuse@0 reg<0x0 0x8>
- FP2 dts: disable 14 OPPs to vendor set; cpu_alert 90000/hyst10000, cpu_crit 105000/hyst3000

## Kconfig: add CONFIG_QCOM_HFPLL, CONFIG_KRAITCC (rest already enabled)

## HARD PREREQUISITE (my finding): xpu-err-fatal (6.18/topic/xpu-err-fatal,
   commits b34e71475156 + 729ef9a8c17a) — DVFS resets in 0.5-2min under load
   without it. NOT in 6.12. Port before any freq scaling.

## Port-critical
1. CX PINNED super_turbo, not scaled (idle brownout). Do not "optimize".
2. cpufreq PD API: 6.12 genpd_names (NULL-term), NOT 6.18 pd_names/num_pd_names.
3. skip hfpll fast_io (already in 6.12).
4. spm.c set_vdd_ret propagation + PMIC_STS verify + VCTL PORT clear + 5mV/70 range.
