# Application of the blueprint: MDP IOMMU + display + XPU on 6.18 (msm8974pro / FP2)

Companion to `PLAN-dvfs-thermal-fp2.md`, same methodology
(`BLUEPRINT-kernel-feature-bringup.md`): ACU decomposition, pre-registered
experiments, one variable per rung, the authority order at every wall, and a
verdict per checkpoint. Ledger: `docs/reports/iommu-campaign-ledger.md`.

**Written 2026-08-06, before any new code.** Everything in §2 was established
on-device in earlier sessions and must not be re-derived; §9 is what to run
first.

---

## 0. Strategic frame — why this campaign, and what it is really about

6.12 ships today because drm/msm still has the vram carveout there. 6.18
removed it (msm8974 was its only user) and made no-IOMMU KMS a hard `-ENODEV`,
so on 6.18 **the display exists only if the MDP goes through its SMMU**. That
is the whole reason this campaign exists.

But the campaign has a second thread that turned out to be entangled with the
first, and it is the one that actually decides whether *anything* we ship is
trustworthy:

> **An XPU (bus protection unit) violation happens on this SoC in our kernel,
> and we do not know which master or which region.**

The two threads meet in one mechanism: **a master whose translation is wrong or
absent emits raw physical addresses, and those land in regions TrustZone
protects.** That is not speculation added here - it is written in the earlier
findings (`6.18-display-findings.md` §2.6: with LPAE instead of V7S "the bank
translates garbage: no fault, black scanout, and occasional fetches into
protected DDR (a plausible silent-reset mechanism)"; §2.8: a missed
`restore_sec_cfg()` means "traffic then bypasses translation with raw physical
addresses").

So: **IOMMU correctness and XPU silence are the same investigation.** Treat a
clean XPU record as part of the definition of a working IOMMU, not as a
separate nice-to-have.

Prize if this campaign lands: 6.18 becomes shippable (display + GPU + radios),
the silent-reset class is closed by attribution rather than by disarming a
safety mechanism, and the same instruments serve VENUS later.

### 0.1 What "display working" means here (operator directive, 2026-08-06)

**The target is the same stack 6.12 ships, with its memory translated instead of
carved out.** 6.12 is not a framebuffer hack: it runs **drm/msm KMS**
(`qcom,msm8974-mdp5` + a real panel driver), and its DT contains **no
`mdp_iommu` at all** — GEM memory comes from the vram carveout
(`msm.vram=192m msm.allow_vram_carveout=1`). So the only thing 6.18 changes is
*where the pixels live*: the carveout is gone, therefore the MDP must reach its
buffers through its SMMU.

Consequences, stated so no rung drifts:

- **`simpledrm` is an instrument, not a path.** It proved the panel, the DSI
  link and lk2nd's scanout can put pixels on the glass on 6.18 — which is
  exactly why it is valuable as a control (§2.3) — but it is not a KMS driver,
  it cannot carry the panel/backlight/TE stack, and nothing can be integrated on
  top of it. It never becomes the shipping configuration.
- **Definition of done for the display half:** `/dev/dri/card0` from drm/msm with
  the MDP5 pipeline, the FP2 panel driver bound, TE-driven command mode, correct
  pixels, 0 SMMU faults and 0 XPU count growth — i.e. CP6, not CP4 or CP5.
- **Sequencing (operator priority):** a *stable 6.18 baseline with working
  display on a full phone (panel + battery)* comes first; the 6.12 DVFS work is
  integrated onto it **afterwards**, as its own rung (CP7a), not in parallel.
  The DVFS stack is already validated on 6.12, so it must arrive as a
  single-variable addition to an already-stable display baseline — never
  co-debugged with it.
- **Which device:** this half runs on the DUT (real FP2, battery, panel). The rig
  has no panel and its DTB disables `&mdss`, so it can only serve the XPU/radio
  track — those two tracks run on two devices and are therefore allowed to
  proceed in parallel without violating one-variable-at-a-time.

---

## 1. Assets

| Asset | Role | Where |
|---|---|---|
| **Authority 1 — vendor FP2 kernel** | register-level truth | `~/Projects/fairphone2-kernel` (3.4). Key files: `arch/arm/mach-msm/scm-xpu.c`, `arch/arm/mach-msm/tz_log.c`, `arch/arm/boot/dts/msm8974.dtsi`, `msm8974-ion.dtsi`, `msm8974-mdss.dtsi`, `board-8974-gpiomux.c` |
| **Authority 2 — live oracle** | the working stack's runtime state | rooted Android FP2 over adb. Prior captures: `~/Projects/msm8974-scratch/artifacts/fairphone2/oracle/` (incl. `20260731-mdp-regs/` = working MDP/DSI registers) |
| **Authority 3 — sibling ports** | what mainline-adjacent trees do | `~/Projects/linux-msm8974-upstream`, tags `v6.15.11-msm8974` / `v6.16.12-msm8974`; pmaports devices |
| **Authority 4 — our own history** | branches, reverts, abandoned attempts | `6.18/topic/{mdp-iommu,fp2-panel,display-carveout,gpu-iommu,xpu-err-fatal,reset-forensics}` |
| **Authority 5 — upstream history** | 6.16 → 6.18 regressions | `v6.16..v6.18` in this tree (both tags present locally) |
| **DUT** | the only display-capable target | real FP2 + battery + panel, lk2nd, ramoops |
| **Rig** | XPU/radio work, no display | FP2 mainboard on carrier, **no panel** (`&mdss` disabled in its DTB), UART + USB net. Validated stable: 6 h 45 min four-core soak |

**Consequence to plan around:** the rig cannot test display, and the DUT has no
UART. The XPU thread runs on the rig; the display thread runs on the DUT; only
the final soak needs both properties on one device.

---

## 2. State of knowledge — do not re-litigate without new evidence

### 2.1 Display: eight root causes already found and fixed

From `docs/reports/6.18-display-findings.md` §2 (all committed, all reusable):

1. panel drivers never ported — regenerate from `qcom-msm8974-6.16.y` (the
   `*_multi` DSI API);
2. display controller deferred forever — needs the msm8974 **RPM bus clocks**
   (`6.18/topic/smd-rpm-clocks`) or `qnoc-msm8974` never probes;
3. `mmss_s0_axi_clk` "stuck at off" — reparent to `mmss_mmssnoc_axi_clk` with
   `CLK_SET_RATE_PARENT | CLK_OPS_PARENT_ENABLE`;
4. **TE never enabled** — the vendor's DCS 0x35 is injected by the downstream
   *mdss framework* from `qcom,mdss-dsi-te-dcs-command`, so it is in no command
   blob and in no generated driver; without it MDP5 ping-pong never completes;
5. panel rails unclaimed (`vdd` = pm8941 l22 3.0 V, `vddio` = l12 1.8 V);
6. **wrong page-table format** — the vendor programs ARMv7 short-descriptor
   (`TTBCR=0`, PRRR/NMRR) on *every* 8974 MMSS SMMU; mainline uses LPAE.
   `ARM_V7S` verified on-device (`SCTLR=000010eb TCR=0 FSR=0`);
7. **vendor BFB block never programmed** — 18 implementation-defined globals at
   `SMMU+0x2008..0x2540`, which downstream programs on every 8974 SMMU
   including TZ-managed ones; now in `qcom,iommu-bfb-regs/-data`;
8. **the fork's IOMMU port broke secure instances** — `restore_sec_cfg()` was
   gated on a `-sec` child existing, so the MDP shape (`secure-id 1` + one `-ns`
   bank) silently skipped the call that installs SMR/S2CR/CBAR. Also re-keyed:
   the fatal `CB+0x10` (TCR2) write, `SCTLR.AFFD` vs `AFE`, and the
   `SMMU+0x2000` write (on 8974 that is `MICRO_MMU_CTRL`, not
   `SMMU_INTR_SEL_NS`; writing `0xffffffff` there boot-loops the SoC).

### 2.2 Display: the verified failing state

With all of the above applied: panel probes and answers DCS, TE pulses, DSI
registers match the oracle, MDP5 commits `plane-0 → CRTC[83]` every ~30 ms,
IOMMU attached with `restore_sec_cfg` succeeding and BFB programmed, **zero
context faults ever** — and **the screen is black**. 8 MB of `/dev/urandom`
into `/dev/fb0` changes nothing: black, not garbage, so this is not a
wrong-mapping signature.

Second, independent failure: with display enabled, **WCNSS bring-up at t≈8.3 s
triggers a silent reset / boot loop**; a WCNSS-firmware-removed variant boots
stable (still black).

### 2.3 Display: `simpledrm` does put pixels on the panel — as a *control*

`DRM_SIMPLEDRM=y` + **headless** DTB + lk2nd handing over its live framebuffer
(`lk2nd.pass-simplefb=autorefresh,relocate,xrgb8888`, plus
`clk_ignore_unused regulator_ignore_unused`) → pixels appeared, with glitches
and a boot loop. No drm/msm and no IOMMU involved. **This is the single most
important asymmetry in the campaign** and §8 CP4 is built on it — as a
diagnostic that bisects "can this hardware display at all on 6.18?" away from
"does drm/msm's MDP5 path deliver data?". Per §0.1 it is not a candidate
shipping configuration.

### 2.4 Dead ends — do not repeat

- **Reverting the carveout removal.** Reverse-applies conflict in every touched
  file because 6.18's VM_BIND/drm_gpuvm rework moved all of it; upstream
  intends it gone. Permanent fight, not a one-time port.
- **Debugging from the mainline side.** Items 4 and 6 above were ~30 lines of
  vendor source each, after hours of mainline-side theorising. This is why the
  wall protocol (blueprint §6.1) exists.

### 2.5 XPU: what is established

- **The vendor arms err-fatal unconditionally at early boot**:
  `arch/arm/mach-msm/scm-xpu.c` → `scm_call(SCM_SVC_MP, XPU_ERR_FATAL,
  {config = ERR_FATAL_ENABLE = 0x0, spare = 0})`. Note the polarity: **enable is
  0**, disable is 1, and **2 reads the current state**.
- **Our port matches it** (`b34e71475156`): same svc/cmd/args, opted in per board
  via `qcom,xpu-err-fatal` on the SCM node, with `qcom_scm.xpu_errfatal=`
  (0/1/2) for diagnostics. Read-back confirmed the state changes (1 → 0), i.e.
  **firmware default on this device is disabled**.
- **Rig evidence (2026-08-04):** armed → 2 PS_HOLD deaths in ~16 min of *idle*,
  with ramoops showing all four cores at 300 MHz, the gang rail static
  (`vdd_timeouts 0`, `volt_sel == volt_sel_req`), 65 °C, 4.9 V — the kernel
  doing nothing and TZ resetting the SoC anyway. Disarmed → 43 min idle + 40 min
  four-core load clean. Radios were up in both.
- **Contradiction, unresolved:** on the DUT (2026-07-26) the opposite was
  measured — with err-fatal *disabled* a four-core load reset in ~30 s, and
  arming it survived 600 s. Both cannot be causal. Most plausible reconciliation:
  the 30 s deaths were the SPM vsel cache poisoning later fixed in
  `c1076b55dd05`, and the arming was a confound. **This contradiction is a
  first-class deliverable of CP1 — do not paper over it.**
- **A known-trigger candidate exists.** Declaring `cd-gpios = <&tlmm 62 …>` on
  the FP2 (a pin the vendor explicitly does *not* use for card detect,
  `board-8974-gpiomux.c:754`) produced SoC resets. TLMM GPIOs have per-pin
  ownership; a write to a pin owned by another master is exactly what an XPU
  guards. If this reproduces, it is a **violation we can cause on demand**,
  which is how you calibrate an instrument.

### 2.6 XPU: the instrument we have not used yet

The vendor exposes the TrustZone diagnostic region and it is **designed for HLOS
reads** (so, unlike RPM MSG RAM / IMEM via `/dev/mem`, reading it is not itself
a reset hazard — see the `devmem-xpu-hazard` note):

- DT: `qcom,tz-log@fe805720`, `reg = <0xfe805720 0x1000>` (`msm8974.dtsi:2084`).
- Layout (`tz_log.c`): a self-describing header (magic, version, cpu_count and
  `*_off` offsets) followed by `vmid_info[]`, `boot_info[]`,
  **`reset_info[] = {reset_type, reset_cnt}` per CPU**,
  **`int_info[] = {int_num, int_info, int_desc[], int_count[per CPU]}`**, and an
  ASCII ring buffer.
- The vendor's own comment on `int_desc` names the case we are chasing:
  *"ASCII text describing type of interrupt e.g: Secure Timer, **EBI XPU**"*.

So the XPU question is **readable, not guessable**: which XPU fired, how often,
on which CPU, plus TZ's own reset reason — and the ring buffer often carries the
offending master/address.

### 2.7 The memory-map delta (new, 2026-08-06, static)

The vendor removes two holes from RAM (`msm8974.dtsi:2325`,
`qcom,msm-mem-hole`): `0x05d00000 + 0x07d00000` and `0x0fa00000 + 0x00500000`.
Mainline's `reserved-memory` covers `0x08000000..0x0f500000` and
`0x0fa00000..0x0ff00000`.

**Delta: `0x05d00000..0x08000000` (35 MB) is vendor-reserved and mainline gives
it to the page allocator.** Of that, `msm8974-ion.dtsi:66` places
`qcom,ion-heap@23 /* OTHER PIL HEAP */` at exactly `0x05d00000 + 0x1e00000`
(30 MB) — a **peripheral-image-loader carveout**. PIL regions are XPU-locked by
TrustZone at authenticate-and-reset time, and the lock survives whether or not
Linux knows about the region.

If TZ (or SBL, or lk2nd's PIL usage) locks any part of that range on this
device, then an ordinary kernel allocation touching those pages is an
APPS-master XPU violation — random in time, more frequent under memory
pressure, invisible without the TZ log, and fatal only when err-fatal is armed.
That matches the rig signature (idle deaths minutes apart, kernel doing
nothing) better than anything else on the table. **Hypothesis H2 in §8.**

It is not the only reservation we lack. `msm8974-mdss.dtsi` gives the primary
framebuffer its own carveout — `mdss_fb0`:
`qcom,memblock-reserve = <0x03200000 0x01E00000>` (30 MB at
`0x03200000..0x05000000`) — plus EBI reservations for the audio heap
(`0x614000`), `qsecom_mem` and `adsp_mem`. Mainline reserves none of these at
those addresses. Two consequences: **M2 is a bigger audit than the PIL hole
alone**, and lk2nd's hand-over framebuffer (the §2.3 control) lives in this same
low region, which is a candidate explanation for the simpledrm *glitches*
independent of stride/format.

### 2.8 Display: two vendor register blocks mainline does not program (new, 2026-08-06, authority 1)

Same class of defect as root cause §2.1.7 (the BFB block), found by reading
`msm8974-mdss.dtsi` rather than the mainline side:

```
qcom,vbif-settings = <0x0004 0x00000001>, <0x00D8 0x00000707>,
                     <0x00F0 0x00000030>, <0x0124 0x00000001>,
                     <0x0178 0x00000FFF>, <0x017C 0x0FFF0FFF>,
                     <0x0160 0x22222222>, <0x0164 0x00002222>;
qcom,mdp-settings  = <0x02E0 0x000000E9>, <0x02E4 0x00000055>,
                     <0x03AC 0xC0000CCC>, <0x03B4 0xC0000CCC>,
                     <0x03BC 0x00CCCCCC>, <0x04A8 0x0CCCC0C0>,
                     <0x04B0 0xCCCCC0C0>, <0x04B8 0xCCCCC000>;
```

These are the MDP's **VBIF (bus interface) limits and AXI priorities** and a set
of MDP QoS/clock-gate-control registers, written by the downstream mdss driver on
every 8974. Two of the VBIF entries (`0x0160`, `0x0164`) are the out-AXI
priority/ordering words and `0x00D8` is a read-limit configuration — exactly the
arbitration knobs that decide whether the display's real-time fetch wins against
another master on BIMC.

Why this matters for *both* open display failures:

- **Black with zero faults** (§2.2) is consistent with data never arriving at the
  DSI, which is what starved or wrongly-limited AXI reads look like — no fault
  is raised because nothing illegal was attempted.
- **The WCNSS-bring-up reset** (§2.2) is the display sharing BIMC with a burst of
  firmware-load traffic, i.e. precisely the case these priorities exist for. This
  is the concrete form of hypothesis H4.

Also from the vendor's v2 IOMMU overrides (`msm8974-v2-iommu.dtsi:136`), which
confirm our port and add two requirements to audit:
`&mdp_iommu { vdd-supply = <&gdsc_mdss>; qcom,iommu-enable-halt; ... }` with the
same **18-register BFB block** we already carry. So the MDP SMMU's registers are
only reachable with the **MDSS GDSC powered**, and the vendor **halts the SMMU**
around reprogramming. Both are ACUs below (D9), not assumptions.

---

## 3. Target specification — the numbers "done" means

Fill the unknowns from the oracle at CP0; every one of these is a gate, not an
aspiration:

| Property | Target |
|---|---|
| KMS | 1080p command-mode panel, TE-driven, pixels correct (a test pattern is recognisable, not just "not black") |
| MDP SMMU | attached, V7S, `restore_sec_cfg` OK, BFB programmed, **0 context faults** over the full soak |
| GPU | submit + fence completion with **0 faults** (already achieved non-secure; must survive alongside display) |
| Radios | modem + WCNSS + ADSP up, QRTR clean, with display active |
| **XPU** | **err-fatal armed** and `int_count` for every XPU description **unchanged** over the soak; TZ `reset_info` counters unchanged |
| Stability | ≥ 60 min at idle *and* ≥ 60 min mixed load with display active and radios up (blueprint §7 arithmetic against a 5–20 min MTBF failure mode) |
| Memory | every mainline reservation justified against the vendor map (§2.7 closed) |

Oracle numbers to capture (authority 2, CP0):

- `/sys/kernel/debug/tzdbg/{interrupts,reset_info,boot_info,log}` — **does
  Android see XPU interrupts at all, and with what descriptions?** This single
  capture decides whether a nonzero count is normal or is our bug.
- MDP SMMU context-bank state: `SCTLR`, `TTBR0/1`, `TCR`, `CBAR`, `SMR`/`S2CR`
  per SID, `FSR`, plus the 18 BFB registers as Android leaves them.
- MDP/BIMC QoS: client priorities, `qcom,msm-bus` vectors for MDP, danger/safe
  LUT programming (candidate for §2.2's black screen and for the display+WCNSS
  collision).
- `/proc/iomem` + the live device tree's memory reservations, to complete §2.7.

---

## 4. ACU inventory

### Layer O — Observability (prerequisite; nothing below starts before these)

- **O1 TZ diag reader.** Read-only mapping of `0xfe805720` (syscon/`mmio-sram`
  + `export`, never `/dev/mem`), parsed per the self-describing header, exposed
  in debugfs: `int_info` (num/type/desc/per-CPU counts), `reset_info`, ring
  buffer. *This is the highest-value ACU in the campaign.*
- **O2 SMMU state dump** — per-CB register snapshot on demand (exists on
  `6.18/topic/mdp-iommu` as a DEBUG dump; promote to a stable debugfs node).
- **O3 MDP5 snapshot hook** (exists, `6.18/topic/display-carveout`).
- **O4 pon-reason decoder** (exists; ported to 6.12 as well).
- **O5 ramoops via lk2nd** (`lk2nd.pass-ramoops`, no ECC) (exists).
- **O6 XPU err-fatal read-back** (`qcom_scm.xpu_errfatal=2`) (exists).
- **O7 IOMMU bypass detector** — assert at attach that translation is actually
  on for every SID the MDP uses (SMR valid + S2CR type=translate + CBAR points
  at our CB), and log loudly if any SID is left bypassing.

### Layer M — Memory map

- **M1** reserve the vendor's PIL hole (`0x05d00000 + 0x1e00000`, or the whole
  `0x05d00000..0x08000000`) as `no-map`.
- **M2** audit every remaining vendor reservation (audio `0x614000` EBI,
  `qsecom_mem`, `adsp_mem`, `secure_mem`) against ours; each difference is
  either justified in a comment or fixed.
- **M3** confirm `no-map` vs `reusable` semantics for each of our regions (a
  `reusable` region handed to CMA is still touchable by the allocator).

### Layer I — IOMMU

- **I1** `restore_sec_cfg` gating (done, §2.1.8) — keep a regression test.
- **I2** V7S page tables for the secure msm8974 instance (done).
- **I3** BFB block (done).
- **I4** `MICRO_MMU_CTRL` vs `INTR_SEL_NS` keying (done).
- **I5** SID/SMR audit for MDP against the oracle (open).
- **I8 V7S vs LPAE — re-test the format decision** (added 2026-08-06). Our port
  forces `ARM_V7S` on the msm8974 secure instance on vendor authority, and on one
  on-device observation ("with LPAE the bank translates garbage: no fault, black
  scanout"). **That observation is confounded:** it predates the fix for the
  `restore_sec_cfg` gating bug, and an uninstalled stream mapping produces exactly
  the same symptom (traffic bypasses translation) whatever the page-table format
  is. Meanwhile **mainline msm8916 runs its MDP through this same driver with the
  default `ARM_32_LPAE_S1`** and is validated in the field. So: with the gating fix
  in place, try LPAE. Cheap (one allocator argument), decisive, and it decides how
  much hand-rolled divergence from upstream the port has to carry.
  - LPAE works ⇒ drop the V7S path; the port becomes far more upstreamable.
  - LPAE still black ⇒ V7S is validated on evidence rather than authority alone,
    and reference B below shows the upstream-shaped way to carry it.
- **I9 mine the two working references** (added 2026-08-06):
  - **A — msm8916 `apps_iommu` (arm64 `msm8916.dtsi`, mainline).** `iommus =
    <&apps_iommu 4>` on `mdss_mdp`, instance `compatible = "qcom,msm8916-iommu",
    "qcom,msm-iommu-v1"` with **`qcom,iommu-secure-id = <17>`** and *mixed*
    contexts: `iommu-ctx@3000` VFE `-sec`, `iommu-ctx@4000` MDP_0 **`-ns`**. That is
    our exact shape (secure-id instance, non-secure MDP bank), it uses
    `asid == SID == CB index`, and it works. **This is the sibling reference D8
    said did not exist** — it does, for the driver, just not for this SoC.
  - **B — apq8064 `mdp_port0`/`mdp_port1` + `drivers/iommu/msm_iommu.c`
    (mainline).** `iommus = <&mdp_port0 0 …>` on the display node, driven by the
    only mainline IOMMU driver that programs **`ARM_V7S`**
    (`alloc_io_pgtable_ops(ARM_V7S, …)`). Krait-era hardware, one SoC generation
    before ours. Reference for the V7S configuration if I8 keeps it.
  - **C — `backup-iommu-msm8974` (LOCAL ONLY, not on origin).** The 6.15-era
    ancestor of this work: MDP/GPU/Venus IOMMU nodes, the wirings, defconfig, the
    `pm_runtime_resume_and_get` conversion and the non-secure SCM/`INTR_SEL_NS`
    skip. **Do not delete it** — it is the one branch here that a `git fetch`
    cannot restore.
- **I6** secure vs non-secure CB assignment: which CB may Linux own on a
  TZ-managed instance, and what TZ expects to keep (open).
- **I7** GPU instance interaction: both instances live at once (open).

### Layer N — Bus / interconnect (added 2026-08-06 from the 7.x sweep)

- **N1 adopt the upstream msm8974 interconnect rework** (`docs/analysis/UPSTREAM-SWEEP-msm8974.md` §6):
  9 interconnect/binding commits + the DT bus-clock drop, which replace our
  `smd-rpm-clocks` workaround with upstream's design. **Trial-backported and
  compile-tested on `6.18/topic/icc-rework`** (10/11 apply clean; the rpmpd DT
  commit conflicts with our own equivalent and is skipped). Untested on hardware.
  Sequence it **before D6**: upstream's own commit message says the QoS path is
  unsorted, so programming QoS on top of the pre-rework interconnect builds on
  sand.
- **N2 the INT_MAX bandwidth hazard.** `aa60d907b3c2` sets `get_bw` to return 0
  "to prevent initial setup from programming INT_MAX into the RPM (which otherwise
  might hang the platform)". We have an adjacent symptom: our MDP SMMU node stays
  `disabled` in the SoC dtsi because probing it before the display stack is up
  "touches the MMSS bus un-arbitrated and resets the SoC". Test whether N1 removes
  that constraint before inventing another explanation.

### Layer D — Display

- **D1** panel drivers (done), **D2** TE (done), **D3** rails (done),
  **D4** RPM bus clocks + MMSSNOC reparent (done).
- **D5** simpledrm stride/format agreement with lk2nd (open; explains glitches —
  but see §2.7: its buffer sits in the low region we do not reserve).
- **D6** **`qcom,vbif-settings` + `qcom,mdp-settings`** (§2.8): 8 + 8
  implementation-defined registers the vendor writes and mainline does not.
  Prime suspect for both the black screen and the WCNSS collision. Port as DT
  properties written after the MDSS GDSC is on, mirroring how the BFB block was
  done (open).
- **D7** scanout path proof: which stage stops the pixels — DSI FIFO, MDP
  fetch, or the panel's own RAM write (open; the black-not-garbage signature
  says "no data arrived", so instrument the DSI byte counters).
- **D8** compare against the **6.16 fork**, which has both the carveout *and* the
  SMMU support: the closest working reference for MDP-through-SMMU state
  (authority 3, open).
- **D9** MDP SMMU power/halt requirements (§2.8): `vdd-supply = <&gdsc_mdss>`
  and `qcom,iommu-enable-halt`. Verify our instance is only touched with the
  MDSS GDSC enabled, and that reprogramming halts the SMMU as the vendor does
  (open — a lost register write here looks exactly like "attached, no faults,
  no pixels").

### Layer X — XPU

- **X1** read `int_info`/`reset_info` before and after a controlled window
  (needs O1).
- **X2** armed/disarmed A/B on identical images (method exists;
  `qcom_scm.xpu_errfatal=` makes it a cmdline flip, no rebuild).
- **X3** **calibration by deliberate violation** — re-declare the gpio62
  card-detect on a throwaway branch, confirm it resets, and confirm O1 names
  the XPU. An instrument that has never seen a true positive is not an
  instrument.
- **X4** M1 + armed err-fatal (the H2 test).

### Layer L — Load/duration profiles (test modes, not features)

- **L1** idle with radios up (the rig's killer profile), **L2** four-core load,
  **L3** display active + radios bring-up (the DUT's killer profile),
  **L4** GPU submits + display, **L5** memory-pressure profile (allocate and
  touch most of RAM — the specific stimulus H2 predicts is dangerous).

---

## 5. Oracle extraction (authority 2) — run before touching code

One adb session, everything captured to
`~/Projects/msm8974-scratch/artifacts/fairphone2/oracle/<stamp>/`:

1. `cat /sys/kernel/debug/tzdbg/interrupts` (twice, 30 min apart, idle) —
   baseline XPU counts on a *working* stack.
2. `cat /sys/kernel/debug/tzdbg/reset_info` and `.../log`.
3. MDP SMMU CB + global registers, and the BFB registers, via the existing
   register-dump helper.
4. MDP/BIMC QoS registers and `qcom,msm-bus` state.
5. `/proc/iomem`, `/proc/device-tree` memory reservations, `dmesg | grep -i pil`
   (which PIL regions Android actually loads, and where).
6. `getprop | grep -i xpu`, and whether `scm-xpu` armed successfully in its
   boot log.

---

## 6. Build variants (blueprint P4 — switchability, no rebuild per experiment)

| Variant | Contents | Purpose |
|---|---|---|
| **V1** | 6.18 baseline + display via SMMU | reproduce §2.2 |
| **V2** | `fairphone2_simplefb_defconfig` | §2.3 path |
| **V3** | V1 with modem/WCNSS firmware removed | isolate the display×radio collision |
| **V4** | rig DTB, headless, radios up | XPU thread (err-fatal via cmdline) |
| **V5** | V4 + M1 (PIL hole reserved) | the H2 test |

Everything that can be a cmdline flag must be one: `qcom_scm.xpu_errfatal=`,
`msm.vram`, `clk_ignore_unused`, governor. DTB-only changes are patched in place
with `fdtput` and verified by decompiled diff — never rebuilt (see the
`build-economy` rule).

---

## 7. Interaction matrix — pairs that must be tested together

| Pair | Why |
|---|---|
| display × radios | measured collision at WCNSS bring-up (§2.2) |
| display × IOMMU | the black screen persists with faults at zero; both must be observed together |
| XPU err-fatal × radios | the rig's idle deaths only occurred with radios up |
| XPU err-fatal × memory pressure | H2 predicts allocation-driven violations |
| IOMMU × memory map | a bypassing SID emits *physical* addresses into whatever the map protects |
| display × DVFS | the 6.12 stack ships DVFS; 6.18 must not regress it |
| GPU IOMMU × MDP IOMMU | two instances, one TZ, shared globals |

---

## 8. Execution ladder and checkpoints

Hypotheses under test, stated now so results cannot be rationalised later:

- **H1** The XPU violation comes from a master emitting untranslated (physical)
  addresses — i.e. an IOMMU bypass/format defect. *Predicts:* the violating XPU
  is the one guarding DDR/EBI, and the count grows when display or GPU traffic
  runs, not at pure idle.
- **H2** The XPU violation comes from the **APPS** master touching the vendor's
  PIL carveout at `0x05d00000` that mainline leaves allocatable. *Predicts:*
  counts grow at idle too, faster under memory pressure (L5), and M1 removes
  them entirely.
- **H3** The violation is unrelated to both and comes from a peripheral whose
  DT region differs from the vendor's (rmtfs, smem, audio). *Predicts:* it
  correlates with that peripheral's activity and M2 fixes it.
- **H4** ("active display traffic destabilises 6.18", from the earlier session)
  the display instability is *not* XPU at all but bus/QoS arbitration or a
  power transient. *Predicts:* V3 (display, no radios) is stable, and the TZ
  XPU counts stay flat across a display run.

### Track structure and order (operator priority, §0.1)

The checkpoints keep their numbers for traceability, but they run as **two tracks
on two devices**, and the display track is the critical path:

| | Track A — display (DUT: panel + battery) | Track B — XPU (rig: no panel, radios up) |
|---|---|---|
| order | **CP0a → CP3 → CP4 → CP6 → CP7a → CP7 → CP8** | CP0b → CP1 → CP2 |
| goal | a stable 6.18 baseline whose **drm/msm** device drives the panel | name the violating XPU and close it by attribution |
| gate into CP7 | pixels correct, 0 faults | XPU counts flat with err-fatal **armed** |

Both tracks share CP0's instruments, and Track B's verdict is a **prerequisite of
CP7, not of CP6**: the display baseline may be declared working while the XPU
question is still open, but nothing is promoted until it is closed.

**CP7a — integrate the 6.12 DVFS stack** onto the *already stable* display
baseline: krait clocks, SPM gang rail (including `c1076b55dd05`), the OPP table,
thermal trips and the cooling maps, as one merge of validated topics and nothing
else. Prediction to record before it runs: display and DVFS are independent, so a
regression here means a shared resource (MMSS/CX rail votes, BIMC arbitration, or
the CX corner) and the interaction matrix §7 row "display × DVFS" is where it
will show. Never co-debug this with an unstable display.

### CP0 — Instruments ready *(gate: no feature work before this passes)*

Split by track: **CP0a** = O2/O3/O5 + the oracle's MDP/SMMU/QoS captures (Track
A needs no TZ log to start); **CP0b** = O1 + X3 calibration (Track B cannot start
without them).

O1 built and **calibrated by X3** (a violation we caused shows up in the log);
oracle baseline (§5) captured; V4/V5 buildable. Verdict requires: O1 output on
the rig, plus the oracle's own `interrupts` capture for comparison.

### CP1 — XPU attribution *(the fork's oldest open question)*

Rig, V4, err-fatal **disarmed** (so the machine survives while violating), L1
for 60 min, then read O1. Also resolves §2.5's DUT-vs-rig contradiction by
reading TZ's own `reset_info` on both.

- Pass = the violating XPU is named with a growing count.
- Fail-unknown = counts flat ⇒ the reset is not an XPU violation and the
  err-fatal correlation is a confound; re-open with the reassessment council.

### CP2 — Memory-map parity (H2)

M1 + M2 applied, err-fatal **armed**, rig, L1 then L5.

- H2 confirmed = zero resets and flat XPU counts where CP1 saw growth.
- H2 refuted = resets continue ⇒ go to H1/H3 with CP1's naming.

### CP3 — IOMMU correctness audit (H1)

Static: every SID the MDP and GPU use, checked against the oracle's SMR/S2CR;
O7 asserting at attach. On-device: CB state dump vs oracle, before and after
`restore_sec_cfg`. No display expected to work yet — this is about proving *no
master bypasses translation*.

### CP4 — The display asymmetry (H4)

The one experiment the earlier session named as "the thread to pull": V1 vs V2
vs V3 with identical instruments.

- If V3 (display, radios removed) soaks clean, the instability is arbitration
  or power, and D6 (QoS) becomes the main line.
- If V2 (simpledrm, no IOMMU, no drm/msm) shows pixels while V1 stays black
  with zero faults, the defect is inside drm/msm's MDP5 path, not the SMMU —
  and D7 must localise which stage drops the data.

### CP5 — simpledrm cleanup

D5 (stride/format) for the glitches; then the boot loop under CP4's verdict.

### CP6 — Display through the SMMU, correct pixels

Only after CP3 and CP4. Gate: recognisable test pattern, 0 faults, 0 XPU count
growth.

### CP7 — Full stack soak, release-grade

Display + GPU + radios + DVFS, err-fatal **armed**, ≥ 60 min idle and ≥ 60 min
mixed load, per §1.1 of `AGENTS.md`. Instrumented with the device-side fsync'd
logger; check `journalctl --list-boots` for unexplained boots.

### CP8 — Promotion

`6.18/topic/*` → `6.18/staging` → `6.18/rc` per `AGENTS.md` §1.1, and only then
is 6.18 a candidate to replace 6.12 as production.

---

## 9. Run these first — cheap and decisive

Ordered for the operator priority in §0.1 (display first). Items 1–3 need no
device at all and are the next actions regardless of hardware availability.

1. **Port `qcom,vbif-settings` + `qcom,mdp-settings`** (D6, §2.8) — static work,
   the same shape as the BFB port that is already proven, and the leading
   candidate for both the black screen and the WCNSS collision. Write it, do not
   test it yet; it goes into the CP4 variant matrix as one variable.
2. **Finish the static memory-map audit** (M2, §2.7): the PIL hole at
   `0x05d00000`, the framebuffer carveout at `0x03200000`, audio/qsecom/adsp EBI
   reservations. Pure reading; H2 and the simpledrm glitches both depend on it.
3. **Read the 6.16 fork's MDSS/IOMMU DT and drivers** (D8, authority 3): 6.16 has
   the carveout *and* the SMMU support, so it is the closest working reference
   for MDP-through-SMMU state; diff its MDP SMMU wiring against ours.
4. **Audit D9** (MDSS GDSC + `qcom,iommu-enable-halt`) against our `mdp_iommu`
   node and the qcom-iommu probe path — static, and a lost register write here
   presents exactly as "attached, no faults, no pixels".
5. **Oracle captures** (§5) as soon as the phone is available: MDP/SMMU/QoS
   registers for Track A, and `tzdbg/{interrupts,reset_info,log}` for Track B.
   Five minutes, zero risk, and the TZ capture decides whether nonzero XPU counts
   are normal on this hardware at all.
6. **O1 on 6.12, not 6.18** (Track B): 6.12 is the stable series and it *has* the
   armed err-fatal and a reproducible reset case, so building the instrument
   there removes the display variable entirely. Then **X3 calibration**
   (deliberate gpio62 violation on a throwaway branch) — the only way to know O1
   works.

---

## 10. Open items carried in

- The **DUT-vs-rig err-fatal contradiction** (§2.5) — CP1 owns it.
- `KERNEL_DISPATCH_TOKEN` missing, so `6.12/rc`/`6.18/rc` pushes do not
  auto-trigger citronics-kernel builds (manual `gh workflow run` today).
- The SD `pwr_irq` storm mechanism (see the ledger's session-6 entry): one
  `/proc/interrupts` reading on each series settles it; unrelated to display but
  it shares the "silent, tolerated fault" character.
- TSENS ack/re-arm defect (still unfixed upstream) — a known interrupt storm
  that will show up in any soak instrumentation.
- 6.16 as a possible production base (it still has the vram carveout), which
  would change this campaign's urgency but not its content.

## 11. Ledger and handoff

Append to `docs/reports/iommu-campaign-ledger.md`, blueprint §5 format
(pre-registered → completed), one entry per experiment, corrections appended
rather than edited. Every entry names the authority that produced the
hypothesis, and every device result names the image and DTB it ran on.
