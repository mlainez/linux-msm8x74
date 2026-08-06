# Upstream sweep for msm8974 — what the versions between and after our bases hold

**Authority 5 (upstream history), standing document.** Our two maintained series
are **6.12 (production)** and **6.18 (base, LTS)**; every other version is
reference material. This file records what a sweep found, with a verdict per
item, so the same ranges are not re-read from scratch.

Started 2026-08-06, no device access. Ranges covered: **v6.12 → v6.18** (done);
**v6.19 →** (open, see §5).

---

## 1. Method — and the trap that makes the cheap check wrong

`git merge-base --is-ancestor <sha> <branch>` answers *"is this commit in our
history"*, **not** *"is this fix in our tree"*. The msm8974-mainline fork
cherry-picks upstream patches, so equivalent code arrives under a different SHA.

Paid for immediately: `65991ea8a6d1` ("remoteproc: qcom_wcnss: Handle platforms
with only single power domain") is **not** an ancestor of `6.12/rc`, yet
`6.12/rc`'s `qcom_wcnss.c` already contains the single-PD handling
(`wcnss->num_pds = 1` plus two "Handle single power domain" paths). A
SHA-only check would have produced a confident, wrong backport.

**Rule: confirm by content** — grep for the identifier, constant or comment the
patch introduces — before calling anything missing.

## 2. v6.12 → v6.18, by path

| Path | Upstream work in the window | In 6.12? | In 6.18? | Verdict |
|---|---|---|---|---|
| `drivers/thermal/qcom/tsens*` | 6 commits: IPQ5018 support, v1-without-RPM, "strictly evaluate for IP v2+", V2 enable/calibration, MSM8937, `remove()` conversion | n/a | yes | **the ack/re-arm storm defect is still unfixed upstream.** Nothing here addresses it. Our storm analysis stands; a fix remains ours to write |
| `drivers/iommu/arm/arm-smmu/qcom_iommu.c` | `ced24bf4352c` Fix pgsize_bitmap, `db64591de4b2` Remove iommu_ops pgsize_bitmap, `e70140ba0d2b` remove_new relic | n/a (no IOMMU on 6.12) | yes | present in our base; relevant to the V7S page-size set — **re-read if page-size behaviour surprises us** |
| `drivers/gpu/drm/msm/` (MDP5 + KMS/GEM core) | 20+ commits: the **drm_gpuvm conversion** (`111fdd2198e6`, `057e55f337c5`, `37889600f58e`, `3bebfd53af0f`), `eab7766c79fd` **remove vram carveout**, `c94fc6d35685` **stop supporting no-IOMMU**, `618c11ea0b4a` msm_iommu_new() no longer returns NULL, `98290b0a7d60` KMS can be disabled, `e10e1a4010f3`/`0bb2335f06cc`/`0c2dda82b145`/`a409b78fcdf7` KMS data moves, format-info plumbing (`81112eaac559`, `a34cc7bf1034`, `1506b103105e`) | no | yes | **this is why 6.18 needs the SMMU.** The carveout removal is entangled with the gpuvm rework — confirming the findings report's judgement that reverting it is a permanent fight |
| `drivers/soc/qcom/ubwc_config.c` | `197713d0cf01` no-UBWC configuration for msm8974/8916/8226/8939 | n/a (no central UBWC db on 6.12) | yes | **RULED OUT as a display suspect** — see §3 |
| `drivers/remoteproc/qcom_wcnss.c` | `65991ea8a6d1` single power domain | **yes, by content** | yes | nothing to do (see §1) |
| `drivers/remoteproc/qcom_q6v5_mss.c` | `4641840341f3` single power domain | **yes, by content** (3 hits) | yes | nothing to do |
| `drivers/remoteproc/qcom_q6v5_mss.c` | `581e3dea0ece` **MBN loading on msm8974** — skips a 0x1000 ELF header when the MBA image is ELF (`MSM8974_B00_OFFSET`) | **no** (0 hits) | yes (3 hits) | **WATCH / adopt on demand.** Inert while our MBA blob is raw — our 6.12 radios come up today — but it decides whether an ELF-wrapped `mba.mbn` boots. Adopt if firmware packaging changes (e.g. a distro `linux-firmware` instead of citronics-firmware) |
| `drivers/clk/qcom/mmcc-msm8974.c`, `drivers/interconnect/qcom/msm8974.c` | header-include cleanups, `remove()` conversion | no | yes | cosmetic, ignore |
| `drivers/mmc/host/sdhci-msm.c` | see the DVFS ledger's session-6 entry: `db58532188eb` is 6.17's and already in 6.12.43 as `6f38d9ae4b6c`; `20a0c37e4406` likewise | yes | yes | closed |
| `arch/arm/boot/dts/qcom/qcom-msm8974*` | `1afdd80d1e02` DSI phy clock-ID header, `7b49c9cf4b77` lower-case labels, `8bcf94778ed3` node-name underscores, several other boards' aliases/buttons | partially | yes | **rebase hazard, not a fix:** the label/node-name churn is what will conflict when a topic moves between series. Named here so it is expected |
| msm8974 IOMMU DT | **nothing** — mainline has zero `iommu@` nodes for msm8974 at 6.12, 6.16 *and* 6.18 | — | ours only | cross-reference: the IOMMU ledger's D8 entry. Our SMMU description is fork-original |

## 3. Ruled out: UBWC as a display suspect

`45a2974157d2` made the MDSS driver **error out** when the UBWC database has no
entry for the platform, and `197713d0cf01` added the msm8974 entry
(`no_ubwc_data`). The lookup is `of_match_node(qcom_ubwc_configs, of_root)`
against the **root** node, so what matters is the board's root compatible list —
the FP2's is `"fairphone,fp2", "qcom,msm8974pro", "qcom,msm8974"`, which contains
`qcom,msm8974`. The match therefore succeeds and MDSS gets a valid (all-zero)
UBWC config on our base.

Recorded because it is exactly the kind of item that gets "discovered" twice:
plausible mechanism, right symptom class (MDSS refusing to come up), and wrong.

## 4. What the window did *not* contain

No upstream work on: the TSENS ack/re-arm invariants; msm8974 SMMU support of any
kind; MDP5 QoS/VBIF programming; Krait DVFS (clk-krait, krait-cc, hfpll, SPM
gang-rail) beyond what we already carry. Every one of those remains ours, which
is consistent with 6.12 being a strict superset of 6.18's clk work (IOMMU ledger,
2026-08-06 entry).

## 5. v6.18 → v6.19.14 — DONE (2026-08-06)

`stable/linux-6.19.y` fetched; newest release **v6.19.14** (2026-04-22), which is
the newest stable branch the kernel.org stable repo carries, so this is the
current upstream horizon.

**The whole-tree `--grep=msm8974` result is two commits**, both the same change:

| Commit | Subject | Ours? |
|---|---|---|
| `a1f2c2d55a81` | `remoteproc: qcom_q6v5_pas: Use resource with CX PD for MSM8974` — points `qcom,msm8974-adsp-pil` at `msm8996_adsp_resource`, "which has cx under proxy_pd_names" | **already have it, by content**, on *both* series |
| `3d447dcdae53` | `dt-bindings: remoteproc: qcom,adsp: Make msm8974 use CX as power domain` | matching DT already ours: `power-domains = <&rpmpd MSM8974_VDDCX>`, `power-domain-names = "cx"` |

Our `adsp-sensors` work reached the same conclusion, by the same reasoning,
before upstream landed it — independent corroboration of a fix we shipped.
**Rebase note:** when a base move brings `a1f2c2d55a81` in, our local change to
that table becomes redundant and should be dropped in favour of upstream's rather
than merged around.

**Per-path, everything else:**

| Path | 6.19 work | Verdict |
|---|---|---|
| `drivers/iommu/arm/arm-smmu/qcom_iommu.c` | `6a3908ce56e6` fix device leak on `of_xlate()` (moves `put_device()` so the **success** path releases the reference too — our base leaks one device refcount per attached master); `fd714986e4e4` iommu core: **pass the old domain to `attach_dev` callbacks** | **`6a3908ce56e6`: WATCH, do not cherry-pick.** It only prevents driver unbind, which we never do, and it is exactly the kind of fix stable 6.18.y picks up — self-adopting it would collide with the automated stable merge on an integration branch. **`fd714986e4e4`: base-move hazard** — it changes the `attach_dev` signature our `qcom_iommu_attach_dev` implements, so a future move off 6.18 touches our port |
| `drivers/gpu/drm/msm/` (102 commits; MDP5/KMS subset) | `7be36c7b60c8` mdp5 → `drm_atomic_get_new_crtc_state()`, `95eacb81d0d9` `drm_crtc_vblank_waitqueue()`, `89194773f573` NULL check in `vm_op_enqueue()`, kernel-doc fixes | **nothing for us.** No returning no-IOMMU path, no msm8974 SMMU support, **no MDP5 VBIF/QoS support** — so the D8 verdict (no working reference for MDP-through-SMMU anywhere) holds at 6.19 as well, and D6 remains ours to write |
| `arch/arm/boot/dts/qcom` (ARM 32-bit, 6 commits) | msm8960 and msm8226 only — **no msm8974**. One is worth knowing: `84df51667a19 ARM: dts: qcom: msm8226-samsung-ms013g: add simple-framebuffer` | reference for the simpledrm control: a mainline board declaring `simple-framebuffer` **in the board DT**, versus our lk2nd-injected `/chosen/framebuffer`. Compare the two when D5 (stride/format) is picked up |
| `drivers/thermal/qcom` | **0 commits** | the TSENS ack/re-arm storm is unfixed at 6.19 too. Ours to write, confirmed twice now |
| `drivers/soc/qcom/spm.c` | **0 commits** | our gang-rail work stands alone upstream |
| `drivers/pmdomain/qcom/rpmpd.c` | 1 commit | check at base-move time; nothing msm8974-specific |
| `drivers/cpufreq/qcom-cpufreq-nvmem.c` | 3 commits, all **ipq806x** (match-list sentinel, warning, SMEM fallback) | irrelevant to Krait/msm8974 |

**Net:** 6.19 contains **no fix we need and none that unblocks the display**. The
one msm8974 fix in it we already had; the one adopt-shaped fix is deliberately
left to stable; and the two things the campaign wants from upstream — msm8974
SMMU support and MDP5 QoS/VBIF — do not exist at any version.

## 6. Sweep list for the next release

Same table shape, content-verified per §1:

1. `drivers/gpu/drm/msm/` — above all, whether **any** msm8974/MDP5 work landed:
   a return of a no-IOMMU path, MDP5 QoS/VBIF support, or `mdp5_kms` changes.
2. `drivers/iommu/arm/arm-smmu/qcom_iommu.c` — anything that would collide with
   our V7S/BFB/secure-instance changes, or make them unnecessary.
3. `arch/arm/boot/dts/qcom/qcom-msm8974*` — new nodes (especially any `iommu@`),
   and further label/node-name churn.
4. `drivers/thermal/qcom/tsens*` — is the ack/re-arm defect finally fixed?
5. `drivers/remoteproc/qcom_{wcnss,q6v5_mss}.c` — msm8974 firmware/PD handling.
6. `drivers/soc/qcom/{spm.c,rpmpd.c}`, `drivers/cpufreq/qcom-cpufreq-nvmem.c` —
   CP7a (integrating the 6.12 DVFS stack) has to land on top of whatever these
   look like on our base.
7. Anything matching `--grep=msm8974 -i` across the whole tree, which is how the
   6.13→6.18 items above were found.

**Standing instruction:** re-run this sweep at each new release, not only when
something breaks. It cost one session to find that the SD `pwr_irq` "missing
backport" was already in our tree and that the storm fix we shipped was wrong;
both would have been caught by a sweep with the §1 rule.
