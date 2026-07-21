# Contributing to this msm8974 kernel fork (humans and AI agents)

This is a downstream Linux kernel fork for **Qualcomm MSM8974** devices
(Fairphone 2 and friends), tracking the upstream `msm8974-mainline` stable
branches. It feeds two consumers:

- **citronics-kernel** → Debian/Ubuntu kernels for users to try
  (`debos-citronics`, `deb-packages`).
- **buildroot** (headless/embedded images).

Read this file before creating branches or commits.

---

## 1. Branching strategy

Names are **version-first**, slash-namespaced. The SoC (`msm8974`) is the whole
fork, so it is *not* repeated in branch names; the **kernel version is the top
axis** because several are maintained in parallel (`6.11`, `6.15`, …).

```
<kver>/topic/<feature>   →   <kver>/staging   →   <kver>/rc   →   <kver>/release
        (one feature)          (integrate all)      (candidate)      (blessed)
```

| Tier | Example | Purpose | Lifecycle |
|------|---------|---------|-----------|
| **base** | `upstream/qcom-msm8974-6.15.y` | the upstream stable branch topics fork from | tracked; never committed to. Topics are pinned to a tested base commit; advancing upstream is a deliberate, re-tested step. |
| **topic** | `6.15/topic/gpu-iommu`, `6.15/topic/dvfs-spm`, `6.15/topic/adsp-sensors`, `6.15/topic/audio` | exactly one feature; clean, rebasable, **upstreamable** | long-lived; rebased onto base |
| **staging** | `6.15/staging` | merge **all** topics; flashed to devices for combined testing | fast-moving; may be reset/rebuilt |
| **rc** | `6.15/rc` | release candidate — the "try it out" kernel citronics-kernel builds | cut from staging when it passes on-device tests |
| **release** | `6.15/release` | blessed, stable; what ships | fast-forwarded from rc when validated; tag releases here |

Rules:

- **A topic branch holds one feature and nothing else.** Keep it clean enough
  to submit upstream. Do not put integration merges, other topics, or
  device-only hacks in it.
- **Do not commit directly to `staging`/`rc`/`release`.** They are built by
  merging topics (`git merge --no-ff <kver>/topic/*`). The only non-topic
  commit allowed on the integration branches is fork meta such as this file.
- **Testing a single topic:** build/flash the topic branch directly. Device
  test tooling (e.g. the buildroot `kernel-tests` package) lives *outside* the
  kernel tree, not in topic branches. Only cut a throwaway `<kver>/test/<x>`
  if a topic needs disposable device hacks.
- **Device variants are DTBs, not branches.** One kernel builds both the
  display-on (`qcom-msm8974pro-fairphone-fp2.dtb`) and headless
  (`…-fairphone-fp2-headless.dtb`) device trees; the bootloader picks one.
  Never fork a branch just to toggle a board option.
- **What each consumer builds:** citronics-kernel (Debian/Ubuntu try-out) →
  `<kver>/rc`; stable → `<kver>/release`. buildroot → `<kver>/release` (headless
  DTB selected at build time). Repoint those recipes at the new names before
  deleting any legacy branch.

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

- **Author of every commit is `Marc Lainez <marc.lainez@gmail.com>`.** When
  importing a patch with a different `From:`, rewrite the author.
- **No `Signed-off-by:`.** These are fork branches, not upstream submissions,
  so no Developer Certificate of Origin is required — the author line is enough.
  Strip `Signed-off-by:` from imported patches.
- **No co-authorship** — never `Co-developed-by:`/`Co-authored-by:`, and the AI
  is never a co-author.
- **Do add an AI-assistance disclosure** when an assistant materially helped,
  per the kernel.org convention (§3) — a single `Assisted-by:` trailer. This is
  disclosure, not co-authorship, e.g.:

  ```
  Assisted-by: Claude:claude-opus-4-8
  ```

  If a `Signed-off-by:` is ever needed (an actual upstream posting), that is a
  human action taken by the maintainer at submission time, not by the assistant.

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
  `gpu-iommu-submit` helper. Validate a change on-device before promoting a
  topic into `staging`, and `staging` before cutting `rc`.
- On-device DMA/IOMMU changes must be checked against dmesg for context faults
  and hangs — a userspace ioctl returning success is not proof (see the
  `kernel-test-iommu` submit test, which judges by dmesg, not the fence).
