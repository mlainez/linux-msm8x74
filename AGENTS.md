# Contributing to this msm8974 kernel fork (humans and AI agents)

This is a downstream Linux kernel fork for **Qualcomm MSM8974** devices
(Fairphone 2 and friends). From the 6.18 series onwards it tracks the
**mainline LTS stable branch (`linux-6.18.y`) directly** — the historical
`msm8974-mainline` upstream is end-of-life, and this fork owns its patches
now (planned horizon: at least two years on 6.18). It feeds two consumers:

- **citronics-kernel** → Debian/Ubuntu kernels for users to try
  (`debos-citronics`, `deb-packages`).
- **buildroot** (`citronics/buildroot-fp2`, headless/embedded images).

Read this file before creating branches or commits.

---

## 1. Branching strategy

Names are **version-first**, slash-namespaced. The SoC (`msm8974`) is the whole
fork, so it is *not* repeated in branch names; the **kernel version is the top
axis** because several are maintained in parallel (`6.15`, `6.18`, …).

```
<kver>/topic/<feature>   →   <kver>/staging   →   <kver>/rc   →   <kver>/release
        (one feature)          (integrate all)      (candidate)      (blessed)
```

| Tier | Example | Purpose | Lifecycle |
|------|---------|---------|-----------|
| **base** | `stable/linux-6.18.y` (6.18+); `upstream/qcom-msm8974-6.15.y` (legacy 6.15) | the stable branch topics fork from | tracked; never committed to. Topics are pinned to a tested base commit (e.g. `v6.18.39`); advancing the base is a deliberate, re-tested step. |
| **topic** | `6.18/topic/gpu-iommu`, `6.18/topic/dvfs-spm`, `6.18/topic/adsp-sensors`, `6.18/topic/smd-rpm-clocks`, `6.18/topic/mmcc-mmssnoc-fix` | exactly one feature/fix-set; clean, rebasable, **upstreamable** | long-lived; rebased onto base |
| **staging** | `6.18/staging` | merge **all** topics; flashed to devices for combined testing; receives automated stable merges (§5) | fast-moving; may be reset/rebuilt |
| **rc** | `6.18/rc` | release candidate — the "try it out" kernel citronics-kernel builds | promoted from staging when it passes on-device validation (§1.1) |
| **release** | `6.18/release` | blessed, stable; what ships | fast-forwarded from rc when validated; tag releases here |

Rules:

- **A topic branch holds one feature and nothing else.** Keep it clean enough
  to submit upstream. Do not put integration merges, other topics, or
  device-only hacks in it.
- **Do not commit directly to `staging`/`rc`/`release`.** They are built by
  merging topics (`git merge --no-ff <kver>/topic/*`). The only non-topic
  commits allowed on integration branches are fork meta (this file, CI
  workflows) and the automated stable merges (§5).
- **Testing a single topic:** build/flash the topic branch directly. Device
  test tooling (e.g. the buildroot `kernel-tests` package) lives *outside* the
  kernel tree, not in topic branches. Only cut a throwaway `<kver>/test/<x>`
  if a topic needs disposable device hacks.
- **Device variants are DTBs, not branches.** One kernel builds both the
  display-on (`qcom-msm8974pro-fairphone-fp2.dtb`) and headless
  (`…-fairphone-fp2-headless.dtb`) device trees; the bootloader picks one.
  Never fork a branch just to toggle a board option.
- **What each consumer builds:** citronics-kernel (Debian/Ubuntu try-out) →
  `<kver>/rc`; stable → `<kver>/release`. buildroot → `<kver>/rc` or
  `<kver>/release` (headless DTB selected at build time). Repoint those
  recipes at the new names before deleting any legacy branch.

### 1.1 Promotion gates (the actual workflow)

Promotion is **evidence-driven**; each tier has a gate that must be passed
*on the device* before moving up:

1. **fix/feature → topic**: compiles warning-free for its own files with the
   ARM cross toolchain; one logical change per commit (bisectable).
2. **topic → staging**: `git merge --no-ff` into staging, then build a **full
   image** from staging and validate on-device. A topic is never considered
   "in" until the *merged* image passed — topics can interact (clock, genpd,
   and probe-ordering bugs regularly only appear in the combination).
3. **staging → rc**: promote (`git branch -f <kver>/rc <kver>/staging`) only
   after the full validation suite passes on the staging image:
   - the new feature demonstrably works (not just probes — e.g. a GPU submit
     with fence completion and a dmesg check, live sensor data, …);
   - **regression sweep** over everything previously working: GPU submit +
     0 IOMMU faults, modem/WCNSS/ADSP remoteprocs up, QRTR clean, network;
   - **stability soak**: several minutes of growing uptime at idle (this SoC
     resets *silently* — no panic on serial — when power/clock state is
     wrong; a boot that "looks fine" for 60 s is not evidence).
4. **rc → release**: human decision after wider testing; tag on release.

If a gate fails, the offending topic gets fixed *on the topic branch* and
re-merged; staging may be rebuilt. `rc`/`release` never receive unvalidated
work (the automated security sync of §5 is the sole exception).

### 1.2 Device-testing protocol (hard-won rules)

- **Flash full images via fastboot only** — on the FP2, flash **only** the
  `boot` (lk2nd) and `userdata` (buildroot `sdcard.img`) partitions, nothing
  else. Do **not** hot-swap `/boot/zImage` or DTBs over SSH: writes to the
  running rootfs have corrupted boot files twice (lk2nd "Invalid device tree
  header"), and a kernel whose `uname -r` no longer matches
  `/lib/modules/<ver>` loses all modular drivers — including USB-ethernet and
  WiFi, i.e. the device silently drops off the network.
- **Verify the built image before flashing** (debugfs on `rootfs.ext2`):
  DTB node status (e.g. gpu/adsp `okay`), expected modules under
  `/lib/modules/<ver>`, expected kernel release. Buildroot's git download
  cache snapshots *branch* refs and silently reuses stale sources — for test
  builds, pin `BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION` to the **commit SHA**.
- Kernel-only module iteration on a matching running kernel over SSH is fine
  (modules only — never files under `/boot`).
- On-device DMA/IOMMU changes must be checked against dmesg for context
  faults and hangs — a userspace ioctl returning success is not proof (see
  `kernel-test-iommu`, which judges by dmesg, not the fence).

---

## 2. Commit conventions

Follow the upstream rules — <https://www.kernel.org/doc/html/latest/process/submitting-patches.html>:

- **Subject:** `subsystem: imperative summary`, ≤ ~75 chars, lower-case after
  the prefix, no trailing period. Match the prefix the subsystem already uses
  (`iommu/qcom:`, `drm/msm:`, `ARM: dts: qcom:`, `soc: qcom: spm:`, …).
- **Body:** wrapped at ~75 columns; explain **what and why** (and user-visible
  impact / trade-offs), not a line-by-line "how". Reference commits as
  `<12+ hex> ("oneline subject")`.
- **One logical change per commit.** A bug fix, a cleanup, and a new feature are
  separate commits; each must build on its own (bisectable).
- **`Fixes:`** for regressions/bugfixes: `Fixes: <12+ hex> ("subject")`.
  Add `Link:`/`Closes:` (prefer lore.kernel.org) when there's a discussion/report.

### Author identity and trailers (fork policy)

- **Work authored here** is committed as `Marc Lainez <marc.lainez@gmail.com>`.
- **Existing commits from other trees are cherry-picked, not rewritten**:
  `git cherry-pick -x` preserves the original author (e.g. msm8974-mainline
  contributors) and records provenance. Attribution to the original author is
  deliberate policy — do not re-author other people's patches as ours.
- **Never add a new `Signed-off-by:`.** These are fork branches, not upstream
  submissions, so no DCO is required. Trailers already present on
  cherry-picked commits are kept as-is (they are part of the original
  commit's history); just never *add* one.
- **No co-authorship** — never `Co-developed-by:`/`Co-authored-by:`, and the AI
  is never a co-author.
- **Do add an AI-assistance disclosure** on Marc-authored commits when an
  assistant materially helped, per the kernel.org convention (§3) — a single
  `Assisted-by:` trailer. This is disclosure, not co-authorship, e.g.:

  ```
  Assisted-by: Claude:claude-opus-4-8
  ```

  If a `Signed-off-by:` is ever needed (an actual upstream posting), that is a
  human action taken by the maintainer at submission time, not by the assistant.

### Porting a feature set between kernel versions

When bringing a topic from an older series (e.g. `6.15/topic/x` → `6.18/topic/x`):

1. Identify the **complete** commit chain, including base-infrastructure
   commits the topic silently depends on (`git log --follow` each touched
   file; missing one out-of-series one-liner cost a full debug cycle once).
2. Cherry-pick with `-x` in original order onto the new base tag.
3. A **clean apply is not a correct port** — surrounding code drifts. Compile
   the touched objects, and treat subsystem renames (`adsp`→`pas`), moved
   tables, and changed hook points as review points, not noise.
4. Validate on-device through the §1.1 gates like any new topic.

---

## 3. AI coding-assistant guidelines (kernel policy)

Per <https://www.kernel.org/doc/html/latest/process/coding-assistants.html>,
when work from this fork is submitted **upstream**:

- **Licensing:** all code must be GPL-2.0-only compatible; every new file gets
  a correct `SPDX-License-Identifier`.
- **DCO / `Signed-off-by`:** only a human can certify the Developer Certificate
  of Origin. **An AI agent must never add `Signed-off-by`.** The human submitter
  adds their own `Signed-off-by` and takes full responsibility for reviewing the
  code, its correctness, and its legal compliance.
- **Disclosure:** AI assistance is disclosed with an `Assisted-by:` trailer,
  e.g. `Assisted-by: Claude:claude-opus-4-8 sparse coccinelle` (agent:model,
  then any specialized analysis tools actually used — not git/gcc/make). This
  fork adds that trailer on AI-assisted commits (§2); it is disclosure, not
  co-authorship.
- **Process:** follow the kernel coding style, `submitting-patches`, and the
  wider development-process docs. The assistant assists; the human is
  accountable for every line submitted.

---

## 4. Build & test (quick reference)

- Topics/integration are built and flashed via the buildroot at
  `citronics/buildroot-fp2`; the `kernel-tests` package there provides TAP
  suites (`kernel-test-iommu`, `-dvfs`, `-thermal`, `-voltage`) plus the
  `gpu-iommu-submit` helper and `adsp_sensors_demo`. Validate per the §1.1
  gates.
- Debian/Ubuntu kernel packages are built by the `Citronics/citronics-kernel`
  project from `<kver>/rc` and `<kver>/release` (see its `kernels.conf`); this
  repo carries no kernel-build CI of its own.

---

## 5. Staying current with LTS security patches

`6.18/staging` and `6.18/rc` are kept up to date with mainline
`linux-6.18.y` automatically by `.github/workflows/stable-sync.yml`
(scheduled): it fetches the stable branch and merges it into both branches,
pushing on a clean merge and opening an issue on conflict. Rationale:
security fixes must not wait for the next manual integration round; a stable
merge is the one case where `rc` moves without a full §1.1 revalidation —
follow up with an on-device check when convenient.
