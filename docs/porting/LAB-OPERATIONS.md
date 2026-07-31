# Lab operations: two-device mainline porting, any target

Companion to `BLUEPRINT-kernel-feature-bringup.md`. **Target-agnostic and
feature-agnostic**: this describes how to work with the devices, how to discover the facts
about a target, how to find the best existing port to stand on, and how the buildroot
external tree ties the local loop together. It applies to any phone/SoC you bring up, and
to any feature.

Nothing about a specific device belongs in this file. Per-target facts live in a **target
profile** that is *discovered* (§3–§4), not hand-written.

---

## 1. Three sources of truth — keep them straight

| Source | Authoritative for | Never use it for |
|---|---|---|
| **Oracle device** (the target running its vendor/stock stack) | Hardware facts of *this model*: SoC/PMIC identity, partition layout, console, tables, vendor policy, register-level behaviour | Anything per-die (fuses, bins, calibration) — those are read on the DUT |
| **Reference corpus** (mainline, the SoC's mainline fork, postmarketOS pmaports, vendor tree) | What is already ported and how upstream models it; known-good configs; the canonical fork to base on | Assuming another device's quirks apply to yours |
| **Workstation / operator** | Local paths, physical wiring, access | Device facts — ask the oracle instead |

Only the third class needs a human. Ask for these and nothing more:

| # | Parameter | Value |
|---|---|---|
| 1 | **`BR2_EXTERNAL` — path to the buildroot external tree** (required, blocks all work) | `____________` |
| 2 | Path to the buildroot tree + its version/tag | `____________` |
| 3 | Kernel source path / remote + branch under development — **optional**; if not given, resolve it per §2.4 instead of asking again | `____________` |
| 4 | Reference-corpus directory (where §2 clones go) | `____________` |
| 5 | Artifact/ledger output directory | `____________` |
| 6 | Workstation serial adapter device (which `/dev/tty*` the target's UART is wired to) | `____________` |
| 7 | Physical access to the devices right now? (gates brick-risk flashes) | `____________` |

Everything else — SoC, PMIC, partition names, console baud, DTB name, flash method,
reset-reason mechanism, RTC presence — is **derived** in §3 and §4.

---

## 2. Reference corpus: find the best existing port before writing code

Do this first. Someone has very likely already mainlined part of this SoC, and standing on
their work is the difference between weeks and months.

### 2.1 postmarketOS is the primary source

pmaports is a curated, continuously-tested record of what actually boots on real phones.
Clone it into the reference directory and mine it:

```
git clone --filter=blob:none https://gitlab.com/postmarketOS/pmaports.git
```

What to extract:

- **`device/*/device-<vendor>-<codename>/deviceinfo`** — effectively a ready-made target
  profile: kernel cmdline, flash method, DTB name, boot-image geometry/offsets, partition
  hints, USB-network settings, architecture, whether a framebuffer exists. Cross-check this
  against what the oracle reports (§3); where they disagree, the oracle wins for hardware
  and pmaports usually wins for "what mainline expects".
- **`device/*/linux-<vendor>-<codename>/APKBUILD`** — the **exact kernel fork, tag/commit
  and patch set** a working port uses. This is the answer to "which fork should we base on".
- **`device/*/linux-*/config-*.<arch>`** — a **known-good kernel configuration**. Extremely
  valuable: it tells you which drivers a working device enables, which is often the missing
  piece when a feature is silently inert.
- **The directory category is a maturity signal**: `main` > `community` > `testing` >
  `downstream`. A device under `downstream` runs a vendor kernel — useful for hardware
  facts, not for mainline guidance.
- **SoC-level kernel packages** (a single `linux-postmarketos-<soc-family>` shared by many
  devices) identify the canonical community fork for the whole SoC — usually the best base.
- **Sibling devices on the same SoC**: if another phone with your SoC has a feature working,
  its config, DT and patches are the shortest path.
- The postmarketOS **wiki device pages** carry per-feature support tables (works / partial /
  broken). Treat as a map of what is realistic, not as technical truth.

### 2.2 The rest of the corpus, and how much to trust each

Clone what you will grep repeatedly; record the commit you read.

| Source | Use for | Trust |
|---|---|---|
| Mainline kernel (and `linux-next`) | What is already upstream; **sibling devices on the same SoC family** already have DTs and drivers | Highest for upstream modelling |
| The SoC's community mainline fork (commonly a `<soc>-mainline` project/org, often with a mainlining-status README) | In-flight work, patches not yet upstream, cherry-pick sources | High |
| pmaports (§2.1) | Canonical fork, known-good config, deviceinfo | High for "what works" |
| Vendor / downstream kernel (stock Android source, CAF/CodeLinaro tags, LineageOS) | **Register-level truth**: sequences, magic values, rail/corner tables, thermal policy | Highest for hardware semantics |
| Bootloader projects (e.g. lk2nd for Qualcomm) | Boot chain, DT selection, fastboot behaviour, recovery | High for boot chain |
| Mailing lists (lore.kernel.org, the SoC's list) | Patches in flight, maintainer objections, why something was rejected | High, but check status |
| Wikis, forums, XDA | Leads, device quirks, key combos | Low — verify before acting |

The corpus is not only for the initial recon: it is the **first stop whenever
the port misbehaves**. The escalation checklist and authority order (vendor
source → live oracle → sibling ports → this fork's own history → upstream
history) live in `BLUEPRINT-kernel-feature-bringup.md` §6.1 — apply it on
symptom persistence, not only on formal soak failures. The recurring trap it
exists to break: debugging by reading the tree being ported *to*, which is the
one source known not to contain the answer.

Two rules that have already cost time here:

- **Clone with `--filter=blob:none`, not `--depth=1`, for any tree where you may need
  history.** A shallow clone cannot resolve the commit that introduced a bug, so you cannot
  produce a `Fixes:` tag or bisect upstream — this exact limitation blocked a `Fixes:`
  trailer in an earlier session.
- **Record provenance for every borrowed fact**: source, file, commit. "Downstream does X"
  without a citation is not evidence.

### 2.3 Output of this phase

A short **reference report** in the ledger: the chosen base fork and why, the sibling
devices worth mining, the known-good config to diff against, and the per-feature support
picture for this SoC. Plus the list of clones with their commits.

### 2.4 When no kernel source is provided: resolve both of them

If the operator did not supply a kernel tree, **do not ask again — find them.** Two
distinct trees are needed and they serve different purposes:

| | Purpose | Authority |
|---|---|---|
| **Mainline base** | The tree you develop on and ship | What upstream already models and how |
| **Downstream / vendor tree** | Read-only reference | **Register-level truth**: sequences, magic values, rail/corner tables, thermal policy, fuse formulas |

Both are resolved from the oracle probe output — no guessing required.

**Search keys the probe already gives you:** `CODENAME`, `MODEL`, `VENDOR`,
`BOARD_PLATFORM`, `SOC_*`, `VENDOR_KERNEL` (the version string), the `-g<sha>` suffix in
that version string, and `VENDOR_OS_BUILD` (the build fingerprint).

**Resolving the mainline base**
1. Search pmaports for the codename (`device/*/device-*-<codename>/`). Its sibling
   `linux-*` package's `APKBUILD` names the **exact fork, tag/commit and patches**, and the
   `config-*` file is a known-good configuration. This is the default answer.
2. If the device is absent from pmaports, look for a **sibling device on the same SoC** and
   use its SoC-level kernel package — usually the canonical community fork for the family.
3. Failing that, check mainline itself for a DT matching the codename or the SoC family,
   and the SoC's community mainline project.
4. **Validate before adopting:** the tree should contain a DT for the codename (or at
   minimum the SoC `dtsi`), build for the target architecture, and have recent activity.
   Prefer whatever pmaports' `main`/`community` devices actually ship.

**Resolving the downstream / vendor tree**
1. The `-g<sha>` suffix in the vendor kernel version string is `git describe` output — it is
   the **abbreviated commit of the vendor tree**. If that SHA resolves in a candidate
   repository, you have found the right tree (or a very close fork). This is the strongest
   single check available.
2. Use `VENDOR_OS_BUILD` (fingerprint) and the Android release to locate the matching GPL
   source drop: the OEM's own source portal, community mirrors of it, or the
   LineageOS-style `android_kernel_<vendor>_<soc-or-device>` repositories.
3. If the OEM drop is missing or incomplete, fall back to the SoC vendor's own release
   baseline for that platform and Android version (CAF/CodeLinaro-style release tags keyed
   on `BOARD_PLATFORM` plus the Android version).
4. **Validate before trusting:** it should contain a defconfig and a board/DT file matching
   the codename or platform, and its reported kernel version should match `VENDOR_KERNEL`
   from the probe. A vendor tree for the wrong variant will quietly give you wrong register
   values, which is worse than having none.

**Record in the reference report and the profile:** for each tree — remote URL, branch/tag,
resolved commit, how it was validated, and which of the two roles it fills. Clone per §2.2
(`--filter=blob:none` when history may be needed). Never cite a downstream fact without the
tree, file and commit it came from.

If either tree cannot be resolved with confidence, say so explicitly and list the
candidates with their evidence — an unvalidated vendor tree presented as authoritative is
how wrong magic values enter a port.

---

## 3. Device roles

Fixed roles. Do not blur them.

### 3.1 Oracle — the target running its vendor/stock stack

Its value is that **it works**. It is a measuring instrument, not a build target.

- **Never reflash, wipe or update it.** If it stops working, ground truth for the platform
  is gone.
- Access is normally `adb` with root (`adb root`, or `su -c`). Prefer pulling files to
  transcribing terminal output.
- Best question to put to it: *"is this a port bug, or is it silicon?"* If the vendor stack
  cannot do X on this hardware either, X is not a porting target.
- **Per-die caveat:** the oracle is usually a *different unit* from the DUT. It yields the
  **model-level facts, algorithms, tables and policy**; fuse bins, calibration values and
  aging behaviour must be read on the DUT.
- **The oracle is a standing instrument, not a one-time profile source.** It is
  consulted at P0 for the target profile — and then again *every time the DUT
  does something the working stack does not*. Mid-debug it can answer, live and
  in minutes: the working runtime state of any subsystem (debugfs — regulators,
  clocks, IOMMU, RPM; sysfs; `/proc/interrupts`; loaded config), the vendor's
  actual init/teardown ordering (via its logs), and "does the working stack even
  use this mechanism?". **Before writing any debug instrumentation for the DUT,
  check whether the oracle already exposes the answer.** Keep it plugged in for
  the whole campaign; an unplugged oracle silently degrades every debugging
  session that follows.

### 3.2 DUT — the device running the kernel under development

- Always restorable to an **anchor image** (the last build that passed a checkpoint).
- Bench harness varies (intact phone, or board on a carrier/rig with external power and USB
  networking). Record what applies; it changes the recovery and evidence story.

### 3.3 When one device must play both roles

Sequential reflashing between vendor and mainline is possible but costs the ability to
cross-check live, and risks the oracle. Avoid it. If unavoidable: verify a reliable path
back to the vendor image *before* the first mainline flash, and take the full §4 profile
dump while the vendor stack is still installed.

---

## 4. Derive the target profile from the oracle

Run the probe script (`utils/oracle-probe.sh`, delivered alongside this document; keep it
in the external tree). It collects raw dumps into the artifact directory and emits a
machine-readable profile. **Do not hand-enter these values.**

### 4.1 What the oracle can tell you

Names below are typical of Qualcomm/Android targets; the probe tries each and records what
exists. Adapt the list per platform family rather than assuming.

| Profile field | Where it comes from | Scope |
|---|---|---|
| Vendor, model, codename, arch | `getprop ro.product.*`, `ro.board.platform`, `ro.hardware` | model |
| SoC identity, revision, family, machine | `/sys/devices/soc0/*` (soc_id, family, machine, raw_id, hw_platform, platform_version) | model |
| PMIC model / die revision | `/sys/devices/soc0/pmic_*`, regulator names in the regulator summary | model |
| **Console device + baud** | `/proc/cmdline` (the vendor's own `console=`/`earlycon`) | model |
| **Partition table with names and roles** | `ls -l /dev/block/*/by-name/`, `/proc/partitions`, sizes from `/sys/class/block/*/size` | model |
| Bootloader identity | `getprop ro.bootloader`, `ro.boot.*`, `/proc/cmdline` | model |
| **Full vendor device tree** | `/proc/device-tree/` or `/sys/firmware/devicetree/base` (pull the whole directory) | model |
| Vendor kernel config | `/proc/config.gz` | model |
| Rails, consumers, voltages | debugfs regulator summary, at idle and under load | model |
| Clock tree state | debugfs clock summary | model |
| Power-domain / corner behaviour | platform-specific debugfs (e.g. RPM stats) | model |
| Thermal zones, trips, vendor policy | `/sys/class/thermal/*`, vendor thermal daemon config, thermal module parameters | model |
| **Reset-reason mechanism** | boot log lines about power-on/warm-reset reasons; `/sys/fs/pstore/`, `/proc/last_kmsg` | model |
| **Persistent-log capability** (pstore/ramoops/RAM console) | presence of `/sys/fs/pstore` or a RAM-console node in the vendor DT | model |
| RTC presence | `/dev/rtc*`, boot log | model |
| CPU topology, frequency tables, limits | `/proc/cpuinfo`, `cpufreq` sysfs | model |
| Speed bin / PVS / calibration fuses | boot log fuse prints | **die — re-read on DUT** |

### 4.2 Two high-value items people miss

- **The vendor device tree** (`/proc/device-tree`) is the platform's own hardware
  description: partition labels, rail configurations, thermal policy, clock/corner maps,
  reserved memory. Pull it whole and decompile as needed. It is frequently more complete
  than the vendor source you can find online.
- **A persistent log region (pstore/ramoops)**, if the target has one, converts a class of
  *silent* resets into recoverable kernel output across the reboot. If the vendor DT
  reserves such a region, replicate it in the mainline DT early — it is one of the highest
  leverage things available on a platform that resets without saying anything.

### 4.3 Where the profile lives

Store it per target in the external tree, next to that target's board files, so builds and
wrappers read the same facts:

```
<BR2_EXTERNAL>/board/<target>/target-profile.env     # derived, machine-readable
<BR2_EXTERNAL>/board/<target>/references.md          # §2.3 reference report
<artifacts>/<target>/oracle/<date>/                  # raw dumps, provenance
```

Tag every field `model` or `die`. Only `model` fields may be trusted from the oracle;
`die` fields must be re-read on the DUT and re-read again if hardware is swapped.

---

## 5. DUT delivery model and what "changing the kernel" costs

Targets differ; record which pattern applies in the profile, because it determines the
whole loop:

| Pattern | Kernel update means | Notes |
|---|---|---|
| Separate boot + rootfs partitions | Reflash the boot image | Simplest |
| **Full OS disk image inside a data partition**, subpartitions mapped by the initramfs (e.g. via `kpartx`) | Rebuild the image, reflash that partition — **wipes everything on the device** | Collect evidence *before* flashing |
| A/B slots | Flash the inactive slot, switch | Keeps a fallback by design |
| Bootloader-loaded kernel (network/USB) | No flash | Fastest iteration when available |

Two invariants regardless of pattern:

- **Kernel release string must match the modules directory.** If `uname -r` no longer
  matches `/lib/modules/<ver>`, every modular driver disappears — including whatever
  provides networking, so the DUT drops off the network and only the console remains.
- **Never write to the boot area of the running rootfs.** Rebuild and reflash. Live-rootfs
  edits corrupt boot files and produce bootloader-level failures that look like bricks.
  Module-only iteration on a *matching* kernel is fine.

---

## 6. Flashing and recovery

1. Flash **only** the partitions the profile lists as allowed for this target. Nothing else
   is a valid destination.
2. **Verify the image before flashing** (§8). A flash plus a failed boot costs far more than
   an inspection.
3. **Know the recovery path before you flash.** Record all available routes in the profile,
   and verify each once, deliberately, while everything still works. In order of convenience:
   - **Reboot straight into the bootloader's flash mode from the running system.** Many
     targets support a reboot-mode mechanism: the kernel writes a magic value into a
     persistent register or SoC memory region (on Qualcomm, typically an IMEM
     `syscon-reboot-mode` node) and the bootloader reads it on the next reset. Where a
     helper for this is packaged (e.g. a `reboot-mode` tool invoked as
     `reboot-mode bootloader`), it behaves like a normal reboot but lands in fastboot, which
     makes the whole build→flash loop remote and removes the need to be at the bench.
     **Two preconditions, both of which must be verified per image (§8): the helper must be
     installed, and the DTB actually flashed must carry the reboot-mode node.** A DTB variant
     that drops it silently costs you remote flashing.
   - The bootloader's own key combo at power-on — the fallback whenever the DUT no longer
     boots far enough to run userspace, which is precisely when a bad kernel is being
     investigated.
   - A low-level SoC download/emergency mode, and knowledge of which parts of the boot chain
     are effectively unbrickable.

   Because the first route depends on a booting system, **never rely on it alone** for a
   change that could prevent boot; confirm the key-combo route and physical access first.
4. Confirm **physical access** before any change that could prevent boot.
5. Keep the **anchor image** restorable in one command.

---

## 7. Buildroot external tree — backbone of the local loop

All device-side tooling, harnesses, variants and image assembly live here, so everything is
versioned, reproducible and reusable across targets and features.

```
make BR2_EXTERNAL=<path> <target>_<variant>_defconfig
make
```

Multi-target layout:

```
<BR2_EXTERNAL>/
  external.desc  external.mk  Config.in
  configs/  <target>_<variant>_defconfig        # one per (target, variant)
  package/
    kernel-tests/                # pass/fail suites (TAP), per capability
    <obs-package>/               # observability instruments (see below)
  board/<target>/
    target-profile.env           # §4.3, derived from the oracle
    references.md                # §2.3
    rootfs-overlay/              # units, sysctl, cmdline
    linux-fragments/             # kernel config fragments per variant
    dts-overlays/                # DT variants where no runtime switch exists
    post-build.sh post-image.sh  # image assembly + manifest emission
  utils/
    oracle-probe.sh              # §4 profile discovery
    build-variant  verify-image  flash-dut  collect-dut
```

### 7.1 Two package families

| Family | Contains |
|---|---|
| **Observability** | Long-lived instruments: telemetry sampler (fsync'd, survives an unclean reset), reset-reason reader, soak runner, state dumpers, log/manifest collector |
| **Test harness** | Pass/fail suites with unambiguous verdicts, plus the helpers they drive |

Dividing line: **if it answers "what is the hardware doing?" it is an instrument; if it
answers "did this work?" it is a test.** Both are packages — never ad-hoc scripts pasted
onto the device. A tool that exists only in a shell history cannot be re-run next session,
and that is exactly how evidence gets lost between sessions. Anything that must observe a
failure has to start unattended at boot, because the interesting failures happen when
nobody is typing.

### 7.2 Variants: how the blueprint's switch matrix becomes real

- **Runtime switches** (cmdline, sysfs, module parameters) — preferred: no rebuild, so
  combination testing stays cheap.
- **Kernel config fragments** — for what must be compiled in or out (e.g. a debug variant
  with lock and atomic-context checking).
- **DT variants** — only where a property has no runtime equivalent. Keep them as overlays
  or clearly named alternatives, never as edits smuggled into a topic branch.

One defconfig per variant, so any run is fully described by `(defconfig, runtime switches)`
and reproducible months later.

### 7.3 Pin sources by commit, never by branch

Pin the kernel to a full commit SHA. Buildroot's download cache snapshots *branch* refs and
silently reuses stale sources, so a branch-pinned build may not contain the change under
test — and nothing in the build log reveals it.

---

## 8. Verify every image before flashing

- kernel release string is the one just built;
- `/lib/modules/<that release>/` exists and holds the expected modules, **including the
  network driver that keeps the DUT reachable**;
- the DTB is the intended variant and the nodes this experiment depends on have the expected
  `status`;
- observability units and test suites present and enabled;
- **the remote-recovery path survives this image**: the reboot-into-bootloader helper is
  installed, and the DTB being flashed still carries the reboot-mode node (§6.3). Losing
  either turns every subsequent flash into a trip to the bench;
- a **manifest** is embedded: kernel SHA, config hash, DTB hash, defconfig, external-tree
  revision, timestamp.

An image without a manifest is an anonymous experiment.

---

## 9. Workflow wrappers

| Wrapper | Does |
|---|---|
| `oracle-probe` | Derives/refreshes the target profile (§4) |
| `build-variant <target> <variant> <kernel-SHA>` | Configures, pins the SHA, builds, emits the manifest |
| `verify-image <image>` | Runs every §8 check; refuses on mismatch |
| `flash-dut <target> <images…>` | Flashes only profile-allowed partitions, after confirming evidence was collected |
| `collect-dut [--pre-flash]` | Pulls telemetry, journals, kernel log, manifest, reset reason into the artifact dir |

Make `flash-dut` refuse unless `collect-dut` has run since the last boot. On targets whose
OS lives in a reflashed data partition, destroying the evidence of the run you are
investigating is the most common irreversible mistake.

---

## 10. Evidence handling

- Telemetry **fsync'd and timestamped**, sampling faster than the shortest failure interval
  of interest — a silent reset leaves only what already reached storage.
- **Collect before every reflash** when the flash wipes the OS image.
- Enable **pstore/ramoops** if the target supports it (§4.2) — it can turn silent resets
  into readable logs.
- Capture the **reset reason** on the next boot. A reset with no kernel output is *either* a
  rail brownout *or* a hard hang caught by a firmware watchdog; the reset-reason register
  is usually the cheapest way to tell them apart.
- If the target has no battery-backed RTC, boot-time listings are unreliable after unclean
  resets (clock skew). Trust per-boot log tails and the monotonic uptime series.
- File every run in the ledger with its manifest. An experiment absent from the ledger will
  be repeated by a future session.

---

## 11. Forbidden actions

1. Reflashing, wiping or updating the **oracle**.
2. Flashing partitions not listed as allowed in the target profile.
3. Writing to the boot area of the **running** rootfs.
4. Pinning a build to a branch ref instead of a commit SHA.
5. Flashing without image verification or without collecting evidence.
6. A brick-risk flash with no confirmed physical access and no verified recovery procedure.
7. Installing device-side tooling ad hoc instead of as a package.
8. Changing more than one variable between two runs.
9. Carrying **die-scoped** values from the oracle to the DUT.
10. Repeating a borrowed fact without its source, file and commit.
11. Testing a hardware-behaviour hypothesis on the DUT before checking how the
    downstream driver programs the same block, and before checking whether the
    live oracle can answer the question directly (blueprint §6.1).
12. Serial trial-and-error against the DUT while an unconsulted authority
    (vendor source, oracle, sibling port, fork history, upstream log) could
    decide the question — parallelize the reading instead (one subagent per
    authority where the tooling allows it).

---

## 12. Recipe: adding an instrument or a test

1. Decide the family (§7.1).
2. Add it as a buildroot package; add a unit in the board overlay if it must run unattended.
3. Tests emit TAP and one unambiguous verdict; instruments emit append-only,
   machine-readable records, one per sample.
4. Judge device state from **kernel logs and hardware registers**, never from a userspace
   call returning success — an ioctl returning 0 proves nothing about the hardware.
5. Add it to the variant defconfigs that need it and reference it from the relevant ACU's
   `Instrument:` field.
6. Commit it in the same session it was written, so the next session inherits the capability
   instead of re-inventing it.
