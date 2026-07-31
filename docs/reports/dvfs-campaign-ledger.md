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
