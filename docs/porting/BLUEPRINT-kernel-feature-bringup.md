# Blueprint: methodical bring-up of a kernel feature on a silent-failure SoC

A reusable thinking model for taking any kernel feature from "not present" to
"release-grade" on hardware that can fail without saying anything. **Target-agnostic and
feature-agnostic** — it applies to any phone/SoC and any capability being mainlined.

The premise it is designed around: **the expensive failure mode is not a bug, it is an
unattributable bug.** Every rule below exists to keep failures attributable.

Companion documents:
- `LAB-OPERATIONS.md` — the devices, the reference corpus, the target profile, the
  buildroot external tree and the local loop. Read it first.
- One feature plan per campaign, instantiating §3 for a specific capability and target.

---

## 1. Ten principles

1. **Oracle before implementation.** A working reference implementation (vendor kernel,
   downstream tree, another OS on the same silicon) defines *target behaviour
   quantitatively* before you write code. Never derive hardware limits from a datasheet
   guess when a running oracle can be measured.
2. **Observability is a hard gate.** On a platform that resets silently, instrumentation
   is work package zero. No feature work starts until a failure produces evidence.
3. **One variable per experiment.** Every run differs from a known-good baseline by
   exactly one deliberate change, recorded in a manifest.
4. **Atomic capability units (ACUs).** Decompose the feature into the smallest units that
   can be independently enabled, disabled and judged. If it cannot be toggled, it cannot
   be bisected.
5. **Switchability is engineering work.** Build *one* kernel where every ACU is gateable
   at runtime (sysfs / cmdline / module param) or by a small set of DT variants. Combination
   testing must be cheap or it will not happen.
6. **Interactions are planned, not discovered.** Enumerate shared resources (rails,
   clocks, domains, locks, buses, firmware) and pre-declare which ACU pairs *must* be
   tested together. This is the antidote to "A is only unstable unless B is done".
7. **Pre-register acceptance criteria.** Write the pass/fail threshold before the run.
   Criteria invented after seeing the result are rationalisation.
8. **Duration is a calculated quantity, not a habit.** Soak length is derived from the
   MTBF you need to exclude (§7). "It looked fine" is not a duration.
9. **Falsify, don't confirm.** Each candidate root cause ships with the observation that
   would *disprove* it. Prefer the experiment that can kill a hypothesis fastest over the
   one that would look good if it passed.
10. **The ledger is the deliverable.** Sessions end, context is lost. An append-only
    evidence ledger in the repo is the only thing that compounds.

---

## 2. Assets and their roles

Assign every piece of hardware exactly one role and do not blur them.

| Role | Definition | Rules |
|---|---|---|
| **Oracle** | Device running the *working* reference stack (e.g. rooted vendor Android) | Read-only source of ground truth. Never reflash it; its value is that it works. |
| **DUT** | Device under test, runs the kernel being developed | Always reflashable to a known-good anchor. All experiments happen here. |
| **Reference sources** | Downstream/vendor source trees, upstream history, other SoC ports | Cited by commit/file, never paraphrased from memory. |
| **Anchor image** | Last configuration that met an acceptance gate | One-command restore. Distinguishes regression from environment drift. |

Oracle caveat: a second unit is **different silicon**. It yields the *algorithm and the
tables*; per-die values (fuses, bins, calibration) must always be read on the DUT.

---

## 3. Phases

### P0a — Reference reconnaissance *(before writing any code)*
Someone has probably already mainlined part of this SoC. Find the best existing port and
stand on it. Survey **multiple** sources, rank them, and record provenance
(`LAB-OPERATIONS.md` §2 has the concrete procedure and trust ranking):
- **postmarketOS / pmaports** is the highest-value starting point: it names the canonical
  kernel fork and commit for the SoC, ships a **known-good kernel config**, and its
  `deviceinfo` files are effectively pre-made target profiles. Clone it locally to grep.
- Mainline itself, including **sibling devices on the same SoC family** — a feature may
  already be upstream for a cousin device.
- The SoC's community mainline fork, the vendor/downstream kernel (authoritative for
  register-level semantics), the bootloader project, and the relevant mailing list.
If no kernel tree was supplied, **resolve two of them rather than asking**: a *mainline
base* to develop on, and the *downstream vendor tree* as the register-level reference. Both
are derivable from the oracle probe's search keys; the vendor tree is confirmed by resolving
the `-g<sha>` suffix of the device's own kernel version string inside the candidate
repository (`LAB-OPERATIONS.md` §2.4).

Output: the chosen base fork *and the reason*, the resolved vendor tree *and how it was
validated*, the siblings worth mining, the known-good config to diff against, and a
realistic per-feature support picture.

### P0b — Ground truth and target definition
Derive the target profile from the oracle (`LAB-OPERATIONS.md` §4 — discovered, not
hand-entered), then extract: the state variables the feature drives, their legal ranges,
the mapping the vendor implements, and the vendor's own limits. Output: a **target
specification** with numbers, plus a list of *derived invariants* (e.g. "rail X must be ≥
rail Y", "clock Z must be voted before frequency F"). Mark every value `model`-scoped or
`die`-scoped; only the former may come from the oracle.

### P1 — Observability and safety net  *(gate: no feature work before this passes)*
Minimum viable evidence chain for a silent failure:
- persistent, fsync'd device-side telemetry sampling every state variable the feature
  touches, at an interval far shorter than the shortest failure MTBF;
- failure-reason capture that survives the reset (firmware/PMIC reset-reason register,
  bootloader banner, persistent ram console if available);
- kernel-side detectors that turn a silent hang into a printed one (soft/hard lockup, RCU
  stall, atomic-sleep debug in a debug build);
- an anchor image plus a build manifest per test (source SHA, config hash, DTB hash,
  userspace version) — pinned to commit SHAs, never branch refs.

### P2 — ACU decomposition
Break the feature into ACUs using the template in §4. Order them by dependency depth, not
by importance. A layer of ACUs that *plumbs* a mechanism always precedes the layer that
*drives* it (e.g. "can I set X reliably at all" before "does X get set correctly by
policy").

### P3 — Interaction matrix
For every ACU pair, mark:
- **I** — independent (no shared resource): do not test the pair.
- **E** — expected interaction, with the shared resource named: **must** test.
- **U** — unknown: test if budget allows, after all E cells.

Add rows for *environmental* interactions (peripherals, remote processors, userspace load)
— they are ACUs too, just ones you don't control.

### P4 — Switchability engineering
Implement the gates from P2 so a single build can express the matrix. Enumerate the
build variants you genuinely cannot avoid (usually DT-level). Output: a **configuration
matrix** mapping each planned run to `(build variant, switch settings)`.

### P5 — Staged execution ladder
- **S0 Baseline** — anchor image, feature fully gated off. Proves the harness, not the feature.
- **S1 Isolation** — one ACU on at a time, shortest test that can fail.
- **S2 Pairs** — every **E** cell from P3.
- **S3 Full stack** — all ACUs on, default policy.
- **S4 Duration & adversarial** — idle soak, sustained-load soak, transition-storm soak,
  real workload with peripherals.

Never skip forward. A pass at S3 with S1 unproven tells you the stack works *today* and
nothing about which part to blame tomorrow.

### P6 — Gates and verdicts
Each stage ends at a checkpoint with pre-registered criteria and a verdict (§6).

### P7 — Ledger and handoff
Every experiment appends one entry: id, hypothesis, single variable, manifest, criteria,
raw result, verdict, artefacts. Plus a maintained **"ruled out" list with the evidence** —
this is what stops the next session re-litigating settled questions.

### P8 — Escalation: the reassessment council
Triggered only by a `FAIL-UNKNOWN` verdict (§6). Convene independent analyses with
*deliberately different lenses* (e.g. power/rails, clock/PLL, idle/PM, thermal,
upstream-diff, vendor-diff, harness-artefact). Requirements:
- each lens works from the ledger, not from the last session's conclusion;
- every proposed cause must state its disproof test;
- a "completeness critic" asks what modality was never tried;
- adversarial verification: try to *refute* each surviving candidate before acting.
Output: a ranked hypothesis list with cheapest-decisive-first ordering. Then re-enter at
the lowest stage the new hypothesis touches.

### P9 — Promotion and upstreaming
Only after S4. Separate the *fork-shippable* result from the *upstreamable* subset; keep
genuine upstream bug fixes as standalone commits with `Fixes:` provenance.

---

## 4. ACU template

```
ID:            <layer>.<n>        e.g. V3
Name:
Enables:       what capability exists once this works
Depends on:    ACU ids (hard) / (soft)
Shared state:  rails, clocks, domains, locks, buses it touches   <- drives the matrix
Kill switch:   exact runtime/DT/config mechanism to disable it
Instrument:    what telemetry proves it is active and behaving
Test:          shortest procedure that can fail; expected observable
Accept:        pre-registered pass criterion (quantitative)
Disproof:      observation that would show this ACU is NOT the problem
Risk:          what it could break elsewhere
```

---

## 5. Experiment record (pre-registered, then completed)

```
EXP-id / date / operator
Hypothesis:        falsifiable statement
Single variable:   what changed vs which baseline
Manifest:          src SHA | config hash | DTB hash | userspace | switches
Criteria (before): pass = ... ; fail = ...
Duration rationale: derived per §7
Result:            raw observations + artefact paths
Verdict:           PASS | PASS-CAVEAT | FAIL-BOUNDED | FAIL-UNKNOWN
Ledger updates:    hypotheses killed / confirmed / added
```

---

## 6. Verdict taxonomy and routing

| Verdict | Meaning | Route |
|---|---|---|
| **PASS** | Criteria met for the pre-registered duration | Advance one stage; update anchor |
| **PASS-CAVEAT** | Met, but with a known deviation or reduced scope | Advance, record the caveat as an open item with an owner |
| **FAIL-BOUNDED** | Failed, **and** the mechanism is identified with evidence | Fix at the responsible ACU, re-run the same stage |
| **FAIL-UNKNOWN** | Failed with no attributable mechanism | **Stop. Do not iterate blindly.** Improve observability (back to P1) or convene the council (P8) |

The distinction between the two failure classes is the heart of this method. Historically,
time is lost by treating `FAIL-UNKNOWN` as `FAIL-BOUNDED` — patching the most plausible
suspect, observing that the symptom moved, and concluding causation.

**Rule:** a fix may only be credited with resolving a failure if the failure was
reproducible *on demand* before the fix, or if the post-fix soak exceeds the §7 duration
for the mode being claimed.

### 6.1 The wall protocol — escalation for *interactive* debugging

The council (P8) triggers on formal soak verdicts, but most time is lost at a
different kind of wall: interactive bring-up where a symptom survives fix
attempts. That wall has its own protocol, and it fires on **symptom
persistence, not on verdicts**:

> **Trigger: the same symptom has survived two fix attempts, or one debugging
> hour has produced no mechanism. Stop touching the DUT and run the checklist.**

1. **Say out loud which tree you have been reading.** If the answer is "the
   tree I am porting *to*", you have been consulting the one source that is
   known not to contain the answer — if it did, the port would work.
2. **Walk the authority order, in order, and record what each says:**
   1. the **vendor/downstream source** — how does the working driver program
      this exact block? (registers, formats, init order, magic values);
   2. the **live oracle** — what is the working stack's *runtime* state for
      this subsystem (debugfs, sysfs, /proc, interrupts, clocks, regulators)?
   3. **sibling ports** — did another device/SoC-family port solve this, and
      how?
   4. **this fork's own history** — grep branches, reverted commits and
      abandoned attempts; someone may have hit and documented this exact wall;
   5. **upstream history** — `git log` the subsystem between the last-working
      and current base; behaviour that "worked on N-2" may be an upstream
      regression or removal.
3. **Diff the working implementation against yours at register level.** Every
   divergence is a ranked hypothesis; the highest rank goes to divergences in
   *format, order or ownership* (who programs what), not in values.
4. **Parallelize the reading.** When more than one authority might hold the
   answer, dispatch independent analyses concurrently (subagents, one lens per
   authority) instead of serially poking the device between theories. A
   completeness critic closes the loop: *"which authority have we not
   consulted yet?"*
5. **Only then instrument the DUT.** Device-side experiments are for deciding
   between hypotheses the authorities produced — not for generating hypotheses
   from scratch.

Case studies from the FP2 campaign (why every step above exists — each cost
real hours to learn):
- a command-mode panel stayed black because the vendor's `set_tear_on` is
  injected by the downstream *framework* from a DT property, so it appears in
  no panel command blob and no generated driver (authority 1 held the answer;
  hours went to mainline theories first);
- scanout "worked" fault-free into a black screen because the vendor programs
  every SMMU on the platform with **V7S short-descriptor** tables while the
  ported driver used LPAE — a 30-line read of the vendor's
  `__program_context()` (authority 1) that was postponed behind three
  mainline-side theories;
- an "already solved" hazard (the fatal SMMU+0x2000 write) was documented in
  the fork's own abandoned branch (authority 4) before it was rediscovered
  the hard way.

---

## 7. How long to soak (do the arithmetic, don't guess)

Model failures as a Poisson process with mean time between failures *m*. Observing no
failure for time *T* bounds the rate at 95% confidence when `T ≈ 3m`.

| Failure mode you must exclude | Required clean runtime |
|---|---|
| MTBF ~20 min | ~1 hour |
| MTBF ~90 min | ~5 hours |
| MTBF ~7 hours | ~21 hours |
| MTBF ~1 week (release claim) | ~3 weeks cumulative |

Consequences to accept up front:
- with a single DUT, **cumulative clean runtime is the currency** and it cannot be
  parallelised — plan calendar time accordingly;
- a claim is always bounded: "no failure in 20 h ⇒ MTBF > ~7 h at 95%", never "fixed";
- once a longer-lived mode is discovered, **all shorter soaks are retroactively
  uninformative** for it. Re-baseline rather than reinterpret old passes.

---

## 8. Forbidden moves (each one has already cost this project time)

- Treating a **green CI check as a build** — verify the artefact, not the status icon.
- Concluding stability from a short observation window.
- Changing two things at once "to save a flash cycle".
- Accepting a **clean cherry-pick as a correct port** — surrounding code drifts.
- Judging device state from a userspace success return instead of kernel logs.
- Modifying files on the running rootfs instead of flashing a full image.
- Building test images from a **branch ref** (caches serve stale trees) rather than a SHA.
- Reasoning from a *different unit's* per-die values.
- Reporting a mechanism as established when only its plausibility was established.
- **Debugging the port against the port's own source tree** while a working
  implementation (vendor source, live oracle, sibling port) sits unconsulted —
  the tree being ported *to* is the one source known not to contain the answer.
- Testing a hardware-behaviour hypothesis on the DUT before reading how the
  downstream driver programs the same block (§6.1 authority order).
- Serial trial-and-error on the device when independent authorities could be
  read in parallel (spawn one analysis per authority; then decide).

---

## 9. Session handoff protocol

Because sessions lose context, each one ends by writing to the repo:
1. ledger entries for every experiment run;
2. the updated **ruled-out list with evidence**;
3. current stage, current anchor, and the single next action;
4. any new invariant discovered, promoted into the project's contributor doc — invariants
   belong in durable project rules, not in a point-in-time report.

A session that produced no ledger entry produced nothing, regardless of what it learned.
