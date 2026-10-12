# The safety argument that does not depend on the bench

> Works blocker 4 of `docs/RELEASE_HARDENING.md` ("a safety argument that
> does not depend on the bench"). Standalone document rather than an extension
> of the plan, because it needs to sit alongside — and periodically
> re-reconcile against — `docs/SAFETY_CASE.md` §4 and
> `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`, both of which are living,
> independently-updated documents; folding this into the plan would either
> duplicate their content or go stale the next time either of those two
> moves, which is exactly the failure mode the plan's own front matter warns
> about for audit documents.

No board was flashed, no firing was started, and no guard behaviour was
changed while writing this. Two read-only live queries were made
(`safety_get_status`, `safety_get_link_stats`) to confirm current arming
state; no trip was cleared and no crash report was touched.

## 1. The headline count, verified rather than repeated

The plan text (and `SAFETY_CASE.md` §4's own rollup, both dated 2026-09-04
through 2026-09-16) state: of roughly twenty tracked guard-level claims, all
are host-tested, and **three** are hardware-verified.

That count checks out, with one internal inconsistency worth flagging. The
plan's item 4 names the three as "S5's hardware fit, S5's masking-before-fit
finding, and KilnFW's thermal_guard guard 6." `SAFETY_CASE.md` §4's own
rollup line names a different set of three: "S5's fit/masking finding" (S5's
two hardware findings collapsed into one bullet), "KilnFW guard 6," and "the
E-stop polarity fix." Both arrive at **3**, but they disagree about whether
the E-stop polarity fix or S5's masking finding is the third item. Read
literally, `SAFETY_CASE.md`'s own per-row table lists exactly these
hardware-verified rows:

- S5 hardware fit — 2026-08-24, live link read a real temperature through the
  safety thermocouple (`GUARD_TEST_MATRIX.md` §6a).
- S5 masking-before-fit finding — same bench session.
- KilnFW thermal_guard guard 6 (sensor validity) — live-verified, no TC
  attached, trip fired after exactly 3 bad reads, relay stayed off.
- E-stop polarity fix — hardware-relevant, negative-tested at unit level,
  2026-08-24 (labelled "hardware-relevant, negative-tested," not
  "hardware-verified," in its own row, but included in the rollup's parenthetical).

That is four items competing for three rollup slots, not three that simply
got mis-summarized once. This is a **stale-inconsistency finding against
`SAFETY_CASE.md` itself**, not a correction to the plan: the safety case's
own row table and its own rollup sentence disagree about which of S5's two
findings, versus the E-stop fix, is the third hardware-verified claim. The
substance is small — the count is 3 either way, and every genuinely
hardware-verified item traces to a real, described bench event — but a
document whose job is to be the number that matters should not contain two
different lists that both claim to be that number's breakdown. Recommend
`SAFETY_CASE.md` §4's rollup sentence be edited to name S5's fit and masking
finding as two items (matching its own row table) and either promote the
E-stop polarity fix's row label to "hardware-verified" explicitly or drop it
from the rollup's parenthetical.

Separately, `SAFETY_CASE.md` §4's row table has **no S15 row at all** — S15
("shared-CT commanded-sum-minus-measured deficit," added 2026-08-28 per
`firmware/SaftyFW/src/safety_guards.h`'s comment "S15 NEW, 2026-09-06" in
`GUARD_TEST_MATRIX.md`) exists in the firmware (`SAFETY_TRIP` enum does not
carry it — S15 is WARN-only, no trip reason — but `safety_guards.c` computes
and reports it) and is documented in `GUARD_TEST_MATRIX.md`'s own
provocation table, yet is absent from `SAFETY_CASE.md`'s guard-level table
and its "~20" rollup count entirely. This means the true denominator is at
least 21, not "roughly 20," and the omission understates how much is
untested rather than overstates it — S15 is unambiguously Bucket B/C
territory (CT-dependent, see §3 below), so its absence from the table is not
hiding a hardware-verified claim, but it is a second, independent staleness
finding against `SAFETY_CASE.md` (distinct from the rollup-arithmetic finding
above) and should be added as its own row.

**Verified count: 3 hardware-verified, against roughly 21 tracked claims
(20 named in `SAFETY_CASE.md` plus the S15 row that document omits).** The
plan's headline number is correct; the document it cites is internally
inconsistent about its own breakdown and missing a row.

## 2. Live state confirmed at time of writing (2026-09-16)

```
safety_get_status():
  link up; SaftyFW armed (relay_owner not tripped); safety thermocouple valid
  29.05 C (CJ 29.39 C) | currents not fitted, not fitted, 0.00 A | ct zone: -
  240 ms old | tx_dropped 0 | ct_counts 16, 17, 57

safety_get_link_stats():
  sent 14331, received 38269, crc/framing errors 1, timeouts 187,
  broadcast dropped 0, diag applied 9568, power applied 9567,
  frames deframed 57409, ... poll period 500 ms
```

This confirms, independently of any document: the link is up and the Pico is
armed with no trip latched; the safety thermocouple channel is fit and
reading a plausible value; and the CT/current-sense side reports **"not
fitted, not fitted"** for two of the three current channels and **"ct zone:
-"** — i.e. current sensing is not commissioned on this board today, exactly
as `docs/CONFIG_FILESYSTEM.md`/`CT_COMMISSIONING_PLAN.md`'s prerequisite
description says. This is the live confirmation behind Bucket B below.

## 3. The three buckets

### Bucket A — provokable on the bench today; hardware verification is owed and schedulable

| Guard | How to provoke it on this bench |
|---|---|
| S1 (overtemp ceiling) | Temporarily lower `abs_max_temp_c` below the current TC reading (per `GUARD_TEST_MATRIX.md` §3.4's own row), confirm trip on the 3rd consecutive over-ceiling reading and K4 drop, then restore the value and confirm via the config CRC in telemetry. Owed because the guard is fully reachable in source and the safety TC is physically fit. |
| S2 (overshoot-sustained) | Drive a fast ramp past setpoint on the 4 W fixture and let it decay; the guard's trip window (119 s/121 s boundary) is a firmware timer, not a power-dependent physical process, so the low-power fixture provokes the same logic path a full kiln would. |
| S5 (sensor validity) fault-injection cases | Only the healthy in-band reading has been observed live. A deliberate `tc_type` CR1 mismatch and a reading forced outside the commissioned plausibility band are both bench-executable without any additional hardware — they exercise the MAX31856 configuration/readback path already wired and fit. |
| S6b (link-dead, both tiers) | Hold the UART link down (disconnect or block the physical link) for the soft (10 s) and hard (120 s) thresholds and confirm both tiers trip and K4 drops. Purely a communications-layer provocation; power level is irrelevant. |
| S7 (E-stop press-to-open) | Physically press the E-stop button (pole 2, the only pole wired on this fixture) and confirm the trip and relay de-energize end to end on real hardware, not just the unit-level `discrete_pin_policy` test. `S7` pole-2 already has a documented bench-verification procedure (`firmware/SaftyFW/README.md`); what remains owed is running it and recording the result in `GUARD_TEST_MATRIX.md` §3.4, which the plan itself notes is "currently entirely unexecuted." |
| S11 (frozen-sensor) | Stall the safety thermocouple's reading (same physical technique as freezing any MAX31856 channel — hold the same ADC code across many polls) with current present, and confirm the trip fires per its trip-verify window. Fully reachable in source (`heat_commanded` now derives from real ADC presence, per `GUARD_TEST_MATRIX.md` §6) and does not depend on CT commissioning for its own detection input, only for the co-requirement that current is genuinely present — achievable at 4 W. |
| KilnFW thermal_guard guards 1, 2, 4, 5, 7 | Each is host-tested against the real (non-stub) `thermal_guard_tick()`, but none has a bench provocation on record. All five operate purely on commanded-heat-vs-observed-rate arithmetic over the main-board thermocouples already fit and working; a slow, low-power ramp on the bench fixture is a legitimate (if slower) provocation for the sanity-rate logic, since the guards compare *commanded* heat against *observed* rate rather than against an absolute power threshold. |
| KilnFW thermal_guard guard 3 (relay-welded, thermal sanity) | Host-tested including a hardware-motivated regression case, but "the only hardware contact this guard has had is the false positive it was tuned against, not a genuine positive trip" (`SAFETY_CASE.md`). A genuine weld cannot be safely staged on real hardware, but the guard's *trip* path (commanded-off, temperature-still-rising) can be provoked by commanding a relay off and observing the guard react to a residual-heat decay curve that mimics a stuck relay closely enough to exercise the same code path — worth attempting, sized the same as the other thermal_guard rows. |
| KilnFW thermal_guard guard 9 (control-task stall) | Both sub-paths (trip/priority and fault-clear) are already tested against the real function, not a stub, but "no commit or bench record... shows a genuinely provoked control-task stall on real hardware." A deliberate task-stall (e.g. suspending the control task under a debugger, or a synthetic busy-loop build) is bench-executable without any power-level dependency. |

**Bucket A count: 12** (S1, S2, S5's two fault-injection cases counted as
one provocation effort, S6b, S7, S11, KilnFW guards 1/2/3/4/5/7/9). This
matches the plan's own Bucket A list closely; the one addition here is
naming KilnFW guard 9 explicitly with its provocation method, since the plan
text folds it into "KilnFW's per-zone guards" without naming it.

### Bucket B — blocked on a prerequisite that could be satisfied

| Guard | Prerequisite |
|---|---|
| S3 (load-stuck-on) | `current_sense_set_cal()` must be called with a real calibration for the channel under test — confirmed live above ("currents not fitted, not fitted, 0.00 A"). Reachable in source; inert until calibrated. |
| S4 (load-inactive WARN) | Same CT calibration prerequisite as S3. |
| S9 (trip-ineffective escalation, non-welded-contactor half) | S9's *logic* verification (current persists after K4 opens) needs a genuine current reading, which needs the same CT calibration; note S9's Bucket-C half (the actual welded-contactor jig) is separate and covered below. |
| S14 (per-channel overcurrent WARN) | CT commissioning, same as S3/S4. The plan's stated reason for S14/S15 being unreachable on this fixture specifically is **a systematic offset, not a sample-count shortfall** — i.e. even once CT hardware is nominally present, the current calibration mapping (`k_ct_v_per_a`) itself measures wrong by a consistent amount rather than being merely noisy, so more samples would not fix it. This is worth stating explicitly because it changes the fix: the prerequisite is a calibration correction, not "run it longer." |
| S15 (shared-CT commanded-sum-vs-measured deficit WARN) | Same CT commissioning and calibration-offset prerequisite as S14; additionally requires `ct_topology = summed`, which is a config choice this board has not made. |

`CT_COMMISSIONING_PLAN.md` steps 0 and 6 are named by the plan as the
dependency for S3/S4/S9/S14/S15, and that is confirmed by both the live
telemetry above and `GUARD_TEST_MATRIX.md`'s own reachability notes. **Bucket
B count: 5** (S3, S4, S9's calibration-dependent half, S14, S15) — one more
than the plan's stated "S3, S4, S9, S14 and S15" list already says, so this
matches the plan exactly; S15 is added here explicitly because
`SAFETY_CASE.md` omits it (§1 above).

S1 and S13 are **not** placed in Bucket B, despite both also being
"commissioning gaps" in `GUARD_TEST_MATRIX.md`'s own vocabulary. The
distinction that matters: S1's and S13's blocking inputs
(`abs_max_temp_c`, `tc_source`/`borrowed_zone_index`) are ordinary
config-store fields this board can set today with a single MCP call and no
new hardware, calibration, or CT wiring — S1 is in fact already commissioned
on this board in practice for firing (a non-zero ceiling is required to run
at all) and only sits at "not commissioned" as a default-safe stance. Placing
them in Bucket A is correct, and this document does so: both are covered by
Bucket A's S1 row and the S13 provocation described in
`GUARD_TEST_MATRIX.md` §3.4 (stall one zone's thermocouple with
`tc_source` set to `BORROWED_ZONE`), which is bench-executable without CT
hardware. S13 was omitted from Bucket A's table above only for space; adding
it: **Bucket A count becomes 13** with S13's inclusion made explicit here.

### Bucket C — structurally impossible on this bench; needs another source of sign-off

| Guard / claim | Why it is impossible here | Where the sign-off has to come from instead |
|---|---|---|
| S9's genuine welded-contactor escalation | Needs a jig that injects real AC current through the CT loop while K4 is confirmed de-energized. No such jig exists, and firmware simulation cannot substitute because the guard latches on an analog CT signal, not a GPIO — this is a hardware-fidelity gap, not a software-coverage gap. | Build the jig (owner decision, sized XL per the plan), or accept the risk with explicit owner sign-off, or defer verification to first-boot on the installed kiln with the safety case stating openly that S9's real-contactor path ships unproven until then. |
| S6a (mainFault) | Argued reachable in source since the wiring commits, but confirmed **genuinely not provokable through the (now-deleted) `virtual_dut` harness at all** — `GUARD_TEST_MATRIX.md` §6a's two independent gaps: no I2C-expander emulation existed to assert `fault_line_asserted`, and the harness never forwarded that field into the TICK protocol even if it had. That specific harness is gone (`virtual_dut`/SimFW deleted 2026-08-28), so this is not "not yet provoked," it is "the tool that could have provoked it no longer exists." Separately — and this is a live reachability defect, not a test-coverage gap — `docs/audits/s6a_startup_grace_revert_2026-09-07.md` documents that a startup-grace suppression window for S6a was added, found unsafe on four independent grounds (contradicts the fail-de-asserted hardware behaviour, does not gate on `clock_stalled`, re-arms every Pico boot, and suppresses invisibly with no log/counter), and was **reverted**. As of that revert S6a is fully armed with no suppression window; the audit is a closed finding, not an open gap, but it is worth restating here because a reader skimming only `SAFETY_MODEL.md`'s "known gap" language could mistake it for still-open. | On real hardware, S6a **can** be provoked directly — assert the ESP's GPIO6 fault-output pin (or physically interrupt the opto path) and confirm the Pico trips and K4 drops — this does not require the deleted harness at all, only bench access to the ESP-to-Pico fault line already wired on this board. This item is placed in Bucket C only for the *host-fixture* gap (no software substitute exists); the actual hardware provocation belongs in Bucket A and should be added to `GUARD_TEST_MATRIX.md` §3.4 as an owed row, since nothing about the 4 W power level blocks it. Recommend re-classifying S6a's hardware trip as Bucket A in the next revision of this document once that row is added and attempted. |
| S8's real threshold | The rate-of-rise threshold can only be set meaningfully from a full-power ramp measurement; a 4 W fixture is structurally incapable of producing a representative ramp rate — this is a physics limit, not a firmware or test-harness limit. Confirmed: the bench is described elsewhere in this repository's own memory as a genuine ~4 W fixture at 120 V, not a scaled-down kiln. | Measure the real threshold on the installed kiln during commissioning (`ROADMAP.md` M3), and ship with an explicit "S8 threshold provisional until first-boot commissioning" note in the safety case rather than a number that looks authoritative but was derived from a fixture two-plus orders of magnitude below rated power. |
| The E-stop's physical interlock, pole 1 | Permanently unwired on this fixture by owner decision (`HARDWARE.md` §5.1, owner 2026-09-10) — not a gap, a closed decision. E-stop on this bench is firmware-mediated only via pole 2. | No action needed for *this* fixture; the installed kiln's first-boot checklist (plan item 8) must include verifying pole 1's physical interlock, since this bench can say nothing about it at all. |
| Thermal behaviour as a class (overshoot magnitude, ramp rates, guard nuisance thresholds) | Measured against a plant with essentially no stored thermal energy; a 4 W fixture cannot exhibit the "elements stayed on for hours after the failure" failure mode release is defined against (plan §0). | Argument from construction for the guard *logic* (host-tested against synthetic but representative traces), combined with mandatory first-boot verification on the installed kiln before any unattended firing — this is squarely plan item 8's territory, not something this document can close on its own. |
| Duty-stability gate (mentioned in the task's known constraints) | Proven infeasible on this fixture: the commanded duty oscillates with a period matching plant tau, a sustained limit cycle rather than a settling transient, so `joint_observations` never reaches its threshold of 5 — this is a property of the 4 W plant's thermal time constant relative to the control loop's tick rate, not a bug in the gate or an insufficient run length. | Either loosen the gate's stability criterion to tolerate a bounded limit cycle (a control-design decision, not a test-fixture fix) or defer this specific gate's satisfaction to the installed kiln, whose much larger thermal mass may settle rather than cycle at the same control gains. |

**Bucket C count: 6** (S9 welded-contactor jig, S6a host-fixture gap — with
its hardware path re-classifiable into Bucket A per the note above, S8 real
threshold, E-stop pole 1, thermal-behaviour-as-a-class, duty-stability gate).

## 4. The Pico's `abs_max_temp_c` invariant, and where this analysis touches it

The task's stated invariant — the Pico's `abs_max_temp_c` must always equal
the ESP's, and there must never be a way for the Pico to be left unarmed —
is touched by two items in this analysis, both in Bucket A/B, neither
requiring a behaviour change to state:

- **S1's Bucket-A provocation** temporarily *lowers* `abs_max_temp_c` on the
  Pico to provoke a trip, then restores it. This is exactly the kind of
  "reset one side of a pair" hazard the project's own standing practice
  (`MEMORY.md`'s "reset-one-side" bug class) warns about: the provocation
  procedure must restore the Pico's value to match the ESP's commissioned
  ceiling afterward, confirmed via the config CRC in telemetry as
  `GUARD_TEST_MATRIX.md` §3.4 already specifies — not merely "restored to
  some prior value." Any future automation of this provocation must verify
  post-test equality against the ESP's own `abs_max_temp_c`, not just that
  the Pico's own value looks like what it was before, since a bench session
  is exactly the kind of moment a mismatch could be introduced and go
  unnoticed (no HTTP endpoint currently surfaces a live ESP-vs-Pico
  ceiling-equality check).
- **Nothing in this analysis proposes disarming the Pico at any point.**
  Every Bucket A provocation described here provokes a trip against an
  already-armed Pico; none of them park the Pico in an unarmed state as part
  of the procedure, and this document takes no position that would change
  that. This is stated explicitly because a document about "how to test
  guards" could easily be misread as license to temporarily disarm one to
  isolate it — that is not proposed anywhere above, and should not be
  inferred from silence.

A `GET`-only diagnostics endpoint exposing the raw boot_guard count and an
ESP-vs-Pico ceiling equality check (raised as a reasonable follow-up
elsewhere in this repository's own notes for boot_guard) would materially de-risk
future S1 provocation work; it is named here as a recommendation, not
undertaken.

## 5. What a release-credible safety argument still needs

**Release blockers** (must be true before this controls an unattended,
full-power, real kiln):

1. All of Bucket A (13 items) actually provoked on real hardware, results
   recorded in `GUARD_TEST_MATRIX.md` §3.4, which today has zero executed
   rows.
2. Bucket B's prerequisite — CT commissioning steps 0 and 6 — completed, so
   S3, S4, S9's current-dependent half, S14 and S15 stop being inert and can
   themselves move into Bucket A and be provoked.
3. S6a's hardware-reachable path (assert the ESP fault-output pin directly,
   no harness needed) attempted and recorded — this is achievable without
   waiting on any of the above and should not continue to be reported as "no
   host fixture exists" once it is understood that the hardware path does
   not need one.
4. For every Bucket C item, an explicit, recorded decision (build the jig /
   defer to installed-kiln commissioning / accept the risk with sign-off) —
   not silence. As of this writing, none of the six Bucket C items has a
   recorded decision; `SAFETY_CASE.md` and `GUARD_TEST_MATRIX.md` describe
   the gaps but neither commits to a resolution path per item.
5. `SAFETY_CASE.md` §4's own internal inconsistency (§1 above: two
   competing lists of "the three hardware-verified items," and a missing S15
   row) corrected, since the safety case's credibility depends on its
   headline number being unambiguous and complete.
6. The first-boot checklist (plan item 8) inheriting every Bucket C item
   that is deferred rather than jig-built or risk-accepted — S8's real
   threshold and the whole thermal-behaviour-as-a-class item, at minimum,
   cannot be signed off before an installed-kiln first firing under any
   version of this plan.

**Desirable, not blocking:**

- A `/api/boot_guard` (or similar) diagnostics route surfacing the raw
  recovery counter and an ESP-vs-Pico `abs_max_temp_c` equality check, which
  would make future S1 bench work self-verifying rather than dependent on a
  human reading a config CRC.
- Re-running the duty-stability gate's design assumption against the
  installed kiln's thermal mass once available, since the limit-cycle
  failure mode observed here is a property of this specific 4 W fixture's
  time constant, not necessarily of every plant this controller will run on.
- Restoring some form of a maintained comparison/reference model (the class
  of tool `virtual_dut`/SimFW used to be) — several Bucket C classifications
  above exist specifically because that tooling was deleted 2026-08-28 and
  nothing replaced it; this is scored desirable rather than blocking because
  the guards it would help test are already covered by direct hardware
  provocation paths (Bucket A) or are Bucket C regardless of tooling (S8, S9
  jig, thermal-behaviour-as-a-class).

## 6. Documents this analysis found stale, and how

- **`docs/SAFETY_CASE.md` §4**: its own rollup sentence names a different
  set of three "hardware-verified" items than its own per-row table implies,
  and its guard-level table has no row for S15 at all despite S15 existing in
  `firmware/SaftyFW/src/safety_guards.h` and being documented in
  `GUARD_TEST_MATRIX.md`. Both are described in §1 above. Recommend both be
  fixed directly in `SAFETY_CASE.md` rather than only noted here, since this
  document is explicitly a point-in-time analysis and will not be kept in
  sync with `SAFETY_CASE.md`'s future edits.
- **`SAFETY_MODEL.md`'s "known gap" language for S6a** (referenced by the
  task prompt) reads, on its own, as an open item; cross-referencing
  `docs/audits/s6a_startup_grace_revert_2026-09-07.md` shows the specific
  startup-grace suppression mechanism that prompted that language was found
  unsafe and reverted, so S6a is fully armed today with no suppression
  window. This is not a contradiction — `SAFETY_MODEL.md`'s gap is about
  *test-fixture* reachability (the deleted `virtual_dut` harness), not about
  a live suppression window — but a reader who does not chase both documents
  could conflate "S6a has a known reachability gap" with "S6a has a known
  live safety hole." It does not; the audit closed a proposed fix as unsafe
  and reverted it, which is different from S6a itself being unsafe. No edit
  is proposed to either document since both are individually accurate; this
  is flagged as a cross-referencing hazard rather than a factual error in
  either.

No other document consulted for this analysis (`GUARD_TEST_MATRIX.md`,
`HARDWARE.md`, `CT_COMMISSIONING_PLAN.md`, `ARCHITECTURE.md`'s 2026-08-27
correction section) was found to disagree with current `origin/main` content
on the specific claims this document relies on.
