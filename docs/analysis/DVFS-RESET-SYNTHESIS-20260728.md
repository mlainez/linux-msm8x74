# MSM8974 FP2 silent PS_HOLD reset — 5-agent synthesis (2026-07-28)

Status: **DVFS is NOT solved, not usable.** The LOAD reset is fixed+shipped
(XPU err-fatal). The idle/transition reset is open. Today's clean-ish test:
cxrevert (rc + revert 11c9727, CX pinned super_turbo, no experimental stack)
**reset at idle at ~27 min** under schedutil — so the current lead is dead.

## Paradigm shift (the through-line all 5 agents converge on)
We have been treating this as a DVFS *value* problem (which corner/voltage/clock).
The evidence says it is:
1. a per-**transition**, probabilistic fault (bare idle and bare connect are clean;
   only DVFS transitions reproduce it);
2. whose likely mechanism is a **synchronous RPM/SMD vote — or a bus/NoC transaction —
   racing an UNCOORDINATED clock/rail transition** (adding a 2nd per-transition RPM vote,
   DDR-BW, made it ~10x worse; crossing the CX corner boundary fires such a vote);
3. that we have **never actually read** — the "clean PS_HOLD" signature cannot distinguish
   a DVFS fault from XPU / NoC-bus-error-fatal / TZ-err-fatal / RPM-err-fatal (an XPU
   violation produced a bit-identical signature).

## SOLID — high confidence, keep
- XPU err-fatal fix resolves the **LOAD** reset (shipped rc3.15; works; mechanism opaque).
- CPU power-collapse (SPC) is **protective, not causal** (SPC-off makes it worse — a
  collapsed core is head-switched off the shared rail).
- Per-core LDO/BHS is **not** the differentiator — Android runs all-BHS at these OPPs too.
- Voltage magnitude / margin / PVS is not it (values correct; +200 mV made it worse; UVLO-tainted).
- HFPLL inverted lock-poll was a real latent defect but **not the cause** (59->0 mechanical verify).
- rpmpd-CX-floor and MX-inversion exonerated (structural, not soak-based).
- **`nom` is NOT too low at idle.** Android idles the digital rail *lower* (SVS_SOC at
  300-883 MHz with scaled L2). The fork's static-L2 forces a NOM floor one corner ABOVE
  Android's known-good idle vote. Corner level is not the sufficient cause.
- The fork's entire msm8974 CPU-DVFS path is fork-only (never ran on other HW). Android's
  is a completely different, *coordinated* architecture (mach-msm/krait-regulator.c).

## OVERTURNED / SHAKY — stop relying on these
- **"CX per-transition vote = root cause"** — not established: the decisive control arms
  (same-corner hi vs lo, same dF) were never finished; the run arms had unequal exposure
  (survivor caps 36k vs 30k → removes 2 of 4 "resets"); governor confound; contradicted by
  boot 51 AND today's 27-min cxrevert reset.
- **super_turbo pin "fix B"** — crude power-wasting proxy. It (a) does NOT reproduce the
  last-good dc92e6 config (dc92e6 = super_turbo + PLAIN domain; the branch = super_turbo +
  active-only domain d2c884f, which the revert left in), (b) contradicts mimic-Android
  (Android does not pin super_turbo), (c) if it helps at all, does so by *removing corner
  crossings + adding blanket margin* — masking, not fixing.
- **"dc92e6 idle-stable because CX pinned"** — confounded with frequency-pinning (729.6 MHz
  floor → no transitions) and with ed13642 (dc92e6 never idled to 300 MHz at all).
- **"Bug #2 cross-core gang race"** — partly tool-death misread as reset (boots 36->36 = no
  reset). Mechanism unproven; serialize (A), rail-pin, and per-core LDO (B) all failed.
- **Flips-to-death numbers pre-2026-07-27** — invalidated by the SSH-poll confound
  (true clean baseline ~80k, not ~4400); six "refutations" declared unreliable.
- **Android differential** — SOLID at ~33/s (fork dies ~5 min, Android survives 30 min).
  BUT the *conclusive* root-cause A/Bs ran at ~1000/s where Android was **never tested**.
  So "real fork bug at field-ish rate" stands; "CX is the cause (1000/s)" is not Android-validated.
- **"Serialization (Option A) insufficient"** — rests on shaky pre-clean-baseline numbers;
  a proper *mutex-bracketed whole-transition* serialization was never cleanly built/tested
  (only a refuted Linux spinlock and a cpufreq-dt set_rate mutex).

## REAL GAPS vs downstream — one theme: STRUCTURAL DE-COUPLING
Vendor runs ONE atomic, ordered, mutex-bracketed sequence per OPP change:
gang rail (max of cores) -> per-core clk/mux; rail-before-clk on rise / clk-before-rail on
fall; retention-wake before the rail drops; phase count; L2-rate; CX-by-L2-rate; DDR-BW —
all under a single global lock. The fork issues each from a different subsystem
(cpufreq-dt, rpmpd, regulator core) with NO mutual ordering. Ranked:
1. (HIGH) No whole-transition cross-core serialization+ordering. Spinlock refuted; a proper
   mutex version never cleanly built. Matches "async 4-core resets fork, survives on vendor."
2. (HIGH-idle) No rail-drop vs idle/retention gating, with the L2 SAW left armed.
3. (MED) CX corner voted into an uncoordinated transition (mechanism, not values).
4. (MED) Static L2 (no scaling, no L2->CX coupling); CPU:L2 hits 3.1x, a regime vendor never runs.
5. (LOW-MED) SMPS phase mgmt — only the wrong direction (fewer phases/PFM at n=1) was tested.
COMBINATION POINT (Marc's own steer): every mechanism was tested ALONE and failed; the vendor's
point is COORDINATION (L2+DDR together via update_l2_bw; phase at n=4/PWM). The coordinated set
was never tested as a unit.

## WHAT WE MISSED (the highest-value part)
1. **We never READ the reset cause.** Get an EDL/SDI ramdump + RPM SMEM error log +
   `restart_reason` IMEM word (bench task, not a build). TZ records every reset; never dumped.
   This alone could collapse the entire hypothesis tree.
2. **"Clean PS_HOLD" doesn't rule out a secure/TZ/RPM watchdog** — the whole "it's DVFS"
   framing may be a category error.
3. **NoC/BIMC bus-error-fatal during a DVFS relock window** — fits the signature exactly
   (silent, per-transition, load-scaled), never examined. The XPU-fix's benefit might be a
   NoC/XPU effect, not a DVFS effect.
4. **No reliable field reproducer** — only the hammer reproduces, possibly torture; never
   rate-matched Android at the decisive ~1000/s.
5. **No clean 60-min soak on the fix-B+XPU branch alone** (converge-one-variable violated by
   the stacked LDO/phase/serialize experiments). Today's cxrevert 27-min reset is the closest
   and it FAILED the gate.
6. **Remoteproc / "idle isn't idle"** — a death during mpss firmware load; never A/B'd on the
   current tree with remoteprocs disabled.
7. **Thermal** ruled out on thin, un-recalibrated tsens data.

## SHORTEST PATH TO AN ANSWER (get ground truth, stop guessing)
1. Read the actual reset reason (EDL ramdump / RPM SMEM error log / restart_reason) after a
   forced reset, then after a spontaneous one.
2. Rate-matched Android hammer at ~1000/s — is there even a fork bug at that rate, or torture?
3. One clean 60-min idle soak on the current fix-B+XPU branch ONLY (partial answer already:
   cxrevert reset at 27 min -> likely fails the gate).
4. ONLY if confirmed a DVFS-transition bug: build the *coordinated* vendor transition
   (mutex-bracketed whole-transition + L2+DDR together + phase at n=4) as ONE unit.

## Android CX-per-freq reference (SOURCED, ANDROID-DVFS-SPEC.md §a.4.4 — do not invent)
- 300-883.2 MHz  -> SVS_SOC     (opp-level 3)   [reachable only if L2 scales]
- 960-1190.4 MHz -> NORMAL/nom  (opp-level 4)
- 1267.2-2265.6  -> SUPER_TURBO (opp-level 6)
Android votes active-set-only and does NOT pin super_turbo. With the fork's static L2=729.6
the faithful set is exactly 11c9727 (nom<=1190.4 / super_turbo>=1267.2). Either proper per-freq
set REINTRODUCES corner-crossing votes -> won't fix the reset without transition coordination.
