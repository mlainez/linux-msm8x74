# Memory-map audit: vendor MSM8974**Pro** vs mainline (Fairphone 2)

**M2 of `docs/porting/PLAN-iommu-display-6.18.md`.** Static, authority 1 (vendor
FP2 kernel) against our own trees. Written 2026-08-06 with no device access.
Applies to **both series** — `qcom-msm8974.dtsi`'s `reserved-memory` is identical
on `6.12/rc` and `6.18/staging`.

**Why it matters:** a region the vendor removes from RAM but mainline hands to the
page allocator is a region the kernel may touch. If TrustZone protects it, that
touch is an XPU violation — random in time, more frequent under memory pressure,
invisible without the TZ log, and fatal only when `qcom,xpu-err-fatal` is armed.
That is hypothesis **H2**.

---

## 1. Correction to the 2026-08-06 seed entry

The seed entry used the **base** MSM8974 numbers. The FP2 is a **MSM8974Pro-AA**,
and the Pro device tree overrides both the hole and the ION carveouts:

| | base `msm8974.dtsi` | **Pro `msm8974pro.dtsi` (authoritative for FP2)** |
|---|---|---|
| `&memory_hole` | `<0x05d00000 0x07d00000>`, `<0x0fa00000 0x500000>` | **`<0x05a00000 0x7800000>`, `<0x0fa00000 0x500000>`** (`:1758`) |
| OTHER PIL heap | `0x05d00000 + 0x1e00000` | **`0x05a00000 + 0x2100000`** (`msm8974pro-ion.dtsi:17`) |
| MODEM heap | (not fixed) | **`0x08000000 + 0x5000000`** (`msm8974pro-ion.dtsi:25`) |

So the hole starts **3 MB lower** (`0x05a00000`, not `0x05d00000`) and ends
**higher** (`0x0d200000`, not `0x0da00000`) than first recorded. Everything below
uses the Pro numbers.

## 2. The two maps

**Vendor, Pro — removed from RAM entirely (`qcom,memblock-remove`):**

| Range | Size | What the vendor puts there |
|---|---|---|
| `0x05a00000 .. 0x0d200000` | 118 MB | OTHER PIL heap (`0x05a00000..0x07b00000`, 33 MB), MODEM heap (`0x08000000..0x0d000000`, 80 MB), and `0x07b00000..0x08000000` + `0x0d000000..0x0d200000` unclaimed slack |
| `0x0fa00000 .. 0x0ff00000` | 5 MB | SMEM, TZ, RFSA, rmtfs |

**Vendor, additionally reserved outside the hole:**

| Range | Size | Source |
|---|---|---|
| `0x03200000 .. 0x05000000` | 30 MB | `mdss_fb0` `qcom,memblock-reserve` (`msm8974-mdss.dtsi:108`) — the primary framebuffer / continuous splash |
| dynamic | 8 MB / 6.3 MB / 1 MB / 2.5 MB | EBI1 reservations: `mdss_fb0`, ION audio heap, two `msm8974.dtsi` buffers |
| dynamic (CMA) | 252 MB / 63 MB / 17 MB | `secure_mem`, `adsp_mem`, `qsecom_mem` — sizes only, no fixed address, so the kernel places them |

**Mainline (ours, both series) — all `no-map`:**

| Node | Range |
|---|---|
| `mpss_region` `mpss@8000000` | `0x08000000 .. 0x0d100000` |
| `mba_region` `mba@d100000` | `0x0d100000 .. 0x0d200000` |
| `wcnss_region` `wcnss@d200000` | `0x0d200000 .. 0x0dc00000` |
| `adsp_region` `adsp@dc00000` | `0x0dc00000 .. 0x0f500000` |
| `venus_region` `memory@f500000` | `0x0f500000 .. 0x0fa00000` |
| `smem_region` `smem@fa00000` | `0x0fa00000 .. 0x0fc00000` |
| `tz_region` `memory@fc00000` | `0x0fc00000 .. 0x0fd60000` |
| `rfsa_mem` `memory@fd60000` | `0x0fd60000 .. 0x0fd80000` |
| `rmtfs@fd80000` | `0x0fd80000 .. 0x0ff00000` |

## 3. Differences, and what each one means

| Range | Vendor | Mainline | Verdict |
|---|---|---|---|
| **`0x05a00000 .. 0x08000000`** (38.6 MB) | removed from RAM; OTHER PIL heap occupies the first 33 MB | **allocatable** | **the H2 candidate** — the only large window the vendor withholds and we do not |
| `0x08000000 .. 0x0d200000` | removed (MODEM heap + slack) | reserved (`mpss` + `mba`) | match — mainline reserves 1 MB more than the Pro modem heap uses |
| `0x0d200000 .. 0x0fa00000` | *in RAM* on the vendor | reserved (`wcnss`, `adsp`, `venus`) | mainline reserves **more**; harmless (we place firmware differently) |
| `0x0fa00000 .. 0x0ff00000` | removed | reserved (smem/tz/rfsa/rmtfs) | exact match |
| `0x03200000 .. 0x05000000` (30 MB) | reserved (`mdss_fb0` framebuffer) | **allocatable** | **not** an XPU hazard (see §4), but it is where lk2nd's hand-over framebuffer lives |
| CMA-style `secure_mem`/`adsp_mem`/`qsecom_mem` | sizes only, kernel-placed | absent | no fixed address to collide with; not an audit item |

## 4. What this does *not* prove — H2 is now weaker, and better specified

The seed entry justified H2 with "PIL carveouts are XPU-locked by TrustZone at
authenticate-and-reset". The audit sharpens that into a claim that can be wrong:

- The vendor's reason for withholding `0x05a00000..0x08000000` is that **HLOS
  itself allocates there** (an ION CARVEOUT heap it hands to PIL), not
  necessarily that TZ protects the range. TZ locks a PIL region *while* an image
  is authenticated and running — for images loaded *from that heap*.
- Mainline does not load firmware from there at all: `mpss`, `mba`, `wcnss` and
  `adsp` images go into the regions at `0x08000000+`, which we do reserve.
- Therefore whether anything holds an XPU lock over `0x05a00000..0x08000000` on
  *our* boots depends on what SBL/TZ (or lk2nd) locked before Linux started —
  which is exactly what the TZ diagnostic log (**O1**) reports and what no amount
  of DT reading can settle.

**Consequence for the plan:** H2 stays on the list and its test stays cheap (one
`no-map` reservation, err-fatal armed, soak), but it is **no longer the leading
hypothesis on the strength of this audit alone**. O1 (which XPU fired, how often)
must come first, or CP2 risks being a lucky shot: a reservation that happens to
move an allocation pattern would "fix" the symptom without proving the mechanism.

The framebuffer gap at `0x03200000` is a different animal: nothing suggests TZ
protects it, and on mainline the display never uses that address (6.12 takes
`msm.vram=192m` from wherever the kernel allocates it; 6.18 uses SMMU-translated
pages). It matters only for the **simpledrm control**, where lk2nd's live
framebuffer sits in that low region and is protected only by the `no-map`
`/reserved-memory` node lk2nd injects itself. Worth verifying in the injected DT
before blaming stride/format for the glitches (**D5**).

## 5. Concrete follow-ups

1. **O1 before CP2** (see §4). Prediction to record: if H2 is right, the TZ log
   names an EBI/DDR XPU whose count grows at idle, and the growth accelerates
   under a memory-pressure profile (**L5**).
2. **Reservation patch, ready but not leading:** add a `no-map`
   `reserved-memory` node for `0x05a00000 + 0x2600000` on the FP2 (or the SoC
   dtsi, since the hole is SoC-level on the Pro). Costs 38.6 MB of 2 GB.
3. **Verify lk2nd's injected framebuffer node** covers what it hands over,
   before touching D5.
4. **No change proposed for `0x0d200000..0x0fa00000`:** mainline reserving more
   than the vendor is safe, and those regions carry our firmware.
