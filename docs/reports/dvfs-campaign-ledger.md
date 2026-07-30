# DVFS/thermal campaign ledger — Fairphone 2 (msm8974pro)

**Append-only.** One entry per experiment/session action, blueprint §5 format
(`docs/porting/BLUEPRINT-kernel-feature-bringup.md`). Artifact root:
`~/Projects/msm8974-scratch/artifacts/`. Never edit past entries; append corrections.

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
- **Result:** *(next session: read soak log + PON + pstore before ANY reflash)*

---

## Current state (end of session 2026-07-30)

- **Checkpoint:** CP0 largely proven on the new DUT (PON classification of an
  induced software reboot ✔, ramoops console across warm reboot ✔, fsync'd
  telemetry from boot ✔, manifested SHA-pinned builds ✔). CP0 residue: induced
  *silent*-reset classification (e.g. watchdog) not yet demonstrated; reboot-mode
  fastboot path broken (see SES1-F); collector + verify/flash wrappers not yet
  scripted; X1 fuse read on THIS die pending.
- **Anchor:** **BV-A image** (`MANIFEST-BVA-20260730.env`,
  `output-fp2/images/sdcard.img`, preserved copies in `~/Projects/msm8974-scratch/preserved/`).
  Restore: put DUT in fastboot (key combo) →
  `fastboot flash userdata ~/Projects/msm8974-scratch/preserved/sdcard-1a41629d4758-dvfs-BVB-20260730.img` (BV-B)
  or rebuild-free BV-A from `output-fp2/images/sdcard.img`.
- **DUT:** real FP2 + battery, `e4f4c070`, lk2nd, no UART. Carrier board retired.
- **Single next action:** let the CP1 soak run ≥ 21 h untouched; next session begins
  by reading `/var/log/soak/`, PON reasons and (manually mounted) pstore BEFORE
  anything else touches the device.
