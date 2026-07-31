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
