# firing_score sub-score enrolment: measurement separated from adjudication (2026-09-14)

Acts on the adversarial review appended to
`docs/audits/firing_score_four_objective_scorecard_2026-09-13.md` by `9a9afb25`,
which found that `d41da85f` — a commit whose stated purpose was closing
accept-permissive gaps — silently changed the iter_tune decision core.

Everything marked **[measured]** was produced by running code in this session.
No board was flashed and no firing was run: all work is host-side
(`firing_score.{c,h}`, `firing_compare.{c,h}`, `test_iter_tune.c`).

## 1. The review's claims, independently verified

**[measured] R1 reproduces exactly.** Two runs of
`firmware/KilnFW/App/test/check_sim_iter_tune_bars.ps1` at the canonical n=220
configuration — one from a disposable worktree at `e78fbc5b` (`d41da85f`'s
parent), one at working-tree HEAD `5d3bc854` (`firing_score.c`/`firing_compare.c`
byte-identical to `d41da85f`):

| | ACCEPT | REJECT_DEGRADED | INSUFFICIENT | NO_PAIRS | A2 `better` |
|---|---|---|---|---|---|
| `e78fbc5b` (pre-scorecard) | 24 | 21 | 615 | 0 | 6 |
| HEAD `5d3bc854` (== `d41da85f`) | 24 | **26** | **610** | 0 | **3** |

Five null comparisons flipped INSUFFICIENT -> REJECT_DEGRADED and A2's
improvement count halved, on an experiment that compares identical gains
against themselves. The 24/660 pin held by cancellation, not by inertness.

**[read] The mechanism is confirmed:** `firing_compare.c` looped
`for (int s = 0; s < FIRING_SUBSCORE_COUNT; s++)` in BOTH its accumulation and
its verdict loop, so raising the enum's count from 3 to 6 enrolled all three
new axes into Bar 1, the no-degradation veto, the low-n `degraded_untrusted`
gate and the human composite with no edit to `firing_compare.c` at all.
**The size of an enum was the decision rule.**

**[measured] R4's band finding reproduces and is sharpened.** Every `dwelling`
tick of all six `logs/coupling/noise_floor_p7*_run*.jsonl` captures, 33
dwell-zone instances, fraction of ticks inside a band of the commanded target:

| band | first dwells (21 instances) | long second dwells (12 instances) |
|---|---|---|
| +-0.5 degC | 0.02 – 0.31 | 0.00 or 1.00 (bimodal on TC quantisation) |
| +-1.0 degC | 0.40 – 0.70 | 0.00 – 1.00 |
| +-1.5 degC | 0.60 – 0.94 | 1.00 (all twelve) |
| +-2.0 degC | **0.79 – 1.00 (all 33)** | **1.00 (all twelve)** |

p90 |err| on first dwells is 1.45 – 3.13 degC. A 0.5 degC band is touched
constantly and never held; 2.0 degC is held.

R2 (signed axis in a lower-is-better comparator), R3 (feeding the axis above
the infeasibility exclusion gives it a vote on heater-limited ticks) and R5
(never-settles is an in-band sentinel that arithmetic consumes) are read-level
findings and were re-read against the source; all stand as written.

## 2. What changed — measurement separated from adjudication

### 2.1 Enrolment is now an explicit, visible act (`firing_compare.h`)

`FIRING_COMPARE_VOTING_MASK` names the four axes that may decide:
`LAG_S`, `ENTRY_PEAK_C`, `STEADY_RMS_C`, `ENTRY_UNDERSHOOT_C`.
`FIRING_COMPARE_REPORT_ONLY_MASK` names the two that may not: `SETTLE_S`,
`LAG_SIGNED_S`. `firing_compare_subscore_votes()` is the single accessor, and
`firing_compare_subscore_t` gained a `votes` field so a result carries its own
classification.

`firing_compare()` still computes `n`, `improved`, `median_raw` and
`median_normalised` for EVERY axis — report-only is not exclusion, the numbers
are all there — and then does nothing else with a non-voting axis: no Bar 1, no
Bar 2, no veto, no `degraded_untrusted`, and no contribution to
`composite_normalised` (so composites are once again comparable with those
recorded before any axis was added). `have_any_sample`, which distinguishes
`NO_MATCHED_PAIRS` from `INSUFFICIENT`, was moved BELOW the votes gate: "this
pair says nothing" is a statement about the rule, and a report-only axis having
samples does not change it.

**Why a mask rather than a per-subscore flag struct or a switch.** Three
compile-time assertions fall out of it that a flag field cannot give:

```c
_Static_assert(FIRING_COMPARE_CLASSIFIED_MASK == FIRING_COMPARE_ALL_SUBSCORES_MASK, ...);
_Static_assert((FIRING_COMPARE_VOTING_MASK & FIRING_COMPARE_REPORT_ONLY_MASK) == 0u, ...);
_Static_assert((FIRING_COMPARE_VOTING_MASK & FIRING_SUBSCORE_SIGNED_MASK) == 0u, ...);
```

The first is the structural fix: **adding a sub-score without classifying it no
longer compiles.** The default is "does not vote", so the accident that
happened cannot recur, and the compile error makes ignoring the decision
impossible. The third makes the R2 sign defect
structurally unrepresentable rather than merely currently-absent —
`FIRING_SUBSCORE_SIGNED_MASK` lives in `firing_score.h`, with the measurement
that knows its own sign convention, not with the adjudicator.

### 2.2 The three axes

**`SETTLE_S` — band re-sized from the plant, never-settles re-encoded,
withdrawn from voting.**

- Band `FIRING_SCORE_SETTLE_BAND_C` 0.5 -> **2.0 degC**, justified from the 33
  dwell-zone instances in section 1, not from a constant. The 0.5 degC
  materiality line answers "is this difference worth chasing"; a settle band
  asks "what can this plant hold", which is a different question, and the
  measurement says 2.0 degC. 1.0 and 1.5 degC were evaluated and rejected on
  the same table (both leave first-dwell z0 short of holding).
- Never-settles no longer reports a number. `settle_outside_at_end` latches
  when the last scored tick is outside the band, and `seg_finish()` then leaves
  `has[SETTLE_S] = false` — the comparator's existing, first-class "nothing to
  say" outcome — instead of the segment duration. The fact is still reported
  out of band, for humans, in a new `firing_segment_score_t.dwell_unsettled`
  counter that no arithmetic touches.
- `firing_score_set_add()` merges `SETTLE_S` only when BOTH same-class dwells
  settled; otherwise the class reports nothing. Letting the settled one's
  number stand in for the class would resurrect the sentinel one level up
  (R5's own observation that merging destroys the `scored_ticks`
  correspondence).
- It does not vote. The band and the encoding are new; whether the axis carries
  signal on matched real captures is an open measurement, and an axis earns a
  vote by being measured to carry signal, not by existing. Re-enrolling it is
  now a one-line, reviewable, test-breaking act — which is the point.
- **[measured]** The new definition is not inert on real data:
  `firing_score_from_capture` over the `noise_floor_p7` vs `p7d` pair now
  reports `settle_s n=6 median_raw=0.0000` — real settle times on both arms,
  where the 0.5 degC band gave ten-of-twelve saturated readings.

**`ENTRY_UNDERSHOOT_C` — kept, and it votes.** It is a correctly-signed
non-negative magnitude on the same units and floor as `ENTRY_PEAK_C`, it closes
a gap demonstrated through the real comparator by `d41da85f`'s own test
(a 2 degC undershoot that previously scored byte-identically to a clean entry
now returns `REJECT_DEGRADED`), and R1's instrumented attribution measured it
never firing in the A1 null experiment. **This is the one deliberate departure
from `e78fbc5b`'s behaviour** — see section 3.

**`LAG_SIGNED_S` — kept as a diagnostic, withdrawn from voting.** Its own
header comment called it a "diagnostic companion"; it is one again. The sign
is preserved in the reported value (that is the whole reason it exists), and
the static assert above guarantees no signed axis can be adjudicated by a
comparator whose universal contract is "lower is better". Scoring `|lag_signed|`
for adjudication was considered and rejected: that would silently create a
THIRD lag axis differing from `LAG_S` only by including the
saturated-and-short ticks, i.e. R3's "it scores the kiln, not the gains" defect
with a magnitude wrapper on it. The honest position is that the axis has no
validated decision semantics yet, and a report-only axis says so.

## 3. A1: full breakdown, before and after

All four rows are **[measured]** in this session, same script, same n=220
canonical configuration, deterministic across repeats.

| | ACCEPT | REJECT_DEGRADED | INSUFFICIENT | NO_PAIRS | A2 `better` | A2 `WORSE` |
|---|---|---|---|---|---|---|
| `e78fbc5b` (pre-scorecard baseline) | 24 | 21 | 615 | 0 | 6 | 0 |
| HEAD `5d3bc854` before this change | 24 | 26 | 610 | 0 | 3 | 0 |
| **after this change** | **24** | **21** | **615** | **0** | **7** | **0** |
| after change, re-run post full rebuild | 24 | 21 | 615 | 0 | 7 | 0 |

**A1's verdict distribution is restored exactly to `e78fbc5b`'s 24/21/615.**
The five wrongly-flipped null comparisons are back to INSUFFICIENT. The pin
was not loosened — `check_sim_iter_tune_bars.ps1` still passes against the same
24/660 ceiling, and no literal in `sim_iter_tune.c` was touched.

**One deliberate deviation, stated:** A2's improvement count is **7**, against
`e78fbc5b`'s 6 and `d41da85f`'s 3. That is `ENTRY_UNDERSHOOT_C` voting — a
genuinely worse-tracking arm is now visible to the comparator on mismatched
plants. `WORSE` stays 0, so the A2 bar itself (worse-by-more-than-one-floor
<= 1%) is unaffected. I intend this: it is the one axis of the three whose
definition survived review intact, and the whole point of the four-objective
work was that objective 4b had no instrument.

## 4. The regression guard

Two tests in `firmware/KilnFW/App/test/test_iter_tune.c`, plus the compile-time
assertions. This is the part worth more than any individual axis.

- `test_subscore_vote_enrolment_is_explicit` — pins the voting set BY NAME
  (each of the six axes asserted voting or not), pins the voter COUNT at four,
  and re-asserts the classification and no-signed-voter invariants at runtime.
  A silently-enrolled axis changes the count and goes red.
- `test_report_only_subscores_cannot_change_a_verdict` — the behavioural half.
  Three matched ramp classes with voting axes IDENTICAL in both arms and the two
  report-only axes swung by ~10 Bar-1 floors, run in BOTH directions: the
  report-only axes must keep their full statistics (`n == 3`, medians) while
  the verdict stays `INSUFFICIENT` — neither manufacturing an ACCEPT nor firing
  the veto — and the composite must stay at the voting-axes-only value.

Two existing tests were rewritten for the new definitions (both are mine to
own): the ring test's excursion amplitude is now sized against the 2.0 degC
band, and `test_settle_time_never_settling_reads_as_worst_not_zero` became
`test_settle_time_never_settling_reports_nothing_not_a_sentinel` — it now
asserts `has[SETTLE_S] == false` plus `dwell_unsettled == 1`, and proves a
slow-but-settled dwell of the SAME length is distinguishable from it (under
the old encoding both read ~400 s). A new
`test_settle_merge_requires_both_dwells_to_have_settled` covers the merge path.

## 5. Negative tests (production broken, confirmed red, restored by hand, full rebuild)

Three, each breaking PRODUCTION code, not a test-local copy.

1. **Deleted `if (!ss->votes) continue;`** from `firing_compare.c` (the whole
   separation). `build_host_tests.ps1`: **RED**, 8 failures, all in
   `test_report_only_subscores_cannot_change_a_verdict` and in both
   directions — Bar 1 cleared on a report-only axis, the veto fired on one, the
   verdict moved off INSUFFICIENT, and the composite read -6.6667 / +6.6667
   instead of 0.
2. **Removed `SETTLE_S` from `FIRING_COMPARE_REPORT_ONLY_MASK`** (an axis left
   unclassified — the exact `d41da85f` accident). Result: **COMPILE ERROR**,
   `firing_compare.h(109): error C2338: static assertion failed: 'every
   firing_subscore_t must be classified voting or report-only'`. The structural
   guard is real, not decorative.
3. **Moved `SETTLE_S` into `FIRING_COMPARE_VOTING_MASK`** (a deliberate but
   unreviewed enrolment). Result: **RED**, 10 failures across BOTH guard tests —
   the name pin, the voter count, the `votes` flag on the result, Bar 1, the
   veto, the verdict and the composite.

Each break was restored **by hand** (retyped, no `git checkout`/`restore`/
`stash`); `git diff --stat` on `firing_compare.h` reads 67 insertions and 0
deletions, i.e. exactly the intended addition. The `App/test/build/` directory
was then **deleted** to force a full rebuild before anything was re-measured.
Post-rebuild: **38/38 executables built and passed**, 7521/7521 checks in the
main host-test binary, and A1 re-measured at 24/21/615 (section 3, row 4).

## 6. Check tally

`tools/run_all_checks.ps1`, PowerShell `-ExecutionPolicy Bypass`, foreground:
**92 of 94 passed**, including `check_00_kilnfw_target_build.ps1` (full ESP-IDF
target build), `check_sim_iter_tune_bars.ps1` (A1/A2/A5/A6),
`check_doc_hash_citations.ps1`, `check_test_c_files_wired.ps1` and
`check_test_has_assertions.ps1`.

The 2 failures belong to other agents' in-flight work, are attributed and were
not touched: `tools/PcTools/check_zones_per_zone_field_drift.ps1` (failing on a
literal `NEGATIVE_TEST_OVERFLOW_FIELD_DELETE_ME` stub left live in the tree) and
`tools/PcTools/selfcheck.py`. Two others named in the handoff as known-failing —
`tools/check_coil_power_w_sentinel_guard.ps1` and the `sim_fuzzy_overshoot`
build — were GREEN in this run; their owners appear to have landed fixes.

## 7. Known drift left deliberately untouched

`firmware/KilnFW/App/test/sim_fuzzy_overshoot.c` is another agent's in-flight
file. It carries a hand-copied MIRROR of `SETTLE_S`'s algorithm with the old
0.5 degC band hardcoded in its own comments and code
("`==FIRING_SUBSCORE_SETTLE_S`'s own algorithm, 0.5C band"). It is now out of
step with production's 2.0 degC band. It was not edited here under this
session's concurrency rules; whoever owns that file should re-derive its band
from `FIRING_SCORE_SETTLE_BAND_C` rather than restating a literal — this repo's
own mirror-drift class.

## 8. Correction to `firing_score_four_objective_scorecard_2026-09-13.md`

Section 8 of that document says *"The new subscores did not flip any of the 660
null comparisons"*. That is refuted, by its own review and independently again
here: five flipped INSUFFICIENT -> REJECT_DEGRADED and A2's `better` fell 6 -> 3.
Its section 8 is left in place as the record of what was believed at the time;
this document and the review appended to it are the correction.

---

# Review, 2026-09-14 — the structural fix stands; the SETTLE_S band table and the composite-comparability claim are REFUTED, and ENTRY_UNDERSHOOT_C's vote is justified on the opposite evidence to the one cited

Adversarial review of `560cffe0` (`git cat-file -t`: commit) by a separate
session. Everything marked **[exec]** was produced by running code here;
**[read]** is source inspection only. No board was flashed and no firing was
run. Production files were broken for the negative tests below, restored **by
hand** (no `git checkout`/`restore`/`stash`), and `firmware/KilnFW/App/test/build/`
was deleted and fully rebuilt before the final measurement.

## Summary

| claim | outcome |
|---|---|
| Separation of measurement from adjudication (`votes` gate) | **stands** |
| Three `_Static_assert`s fail at COMPILE time, incl. signed-cannot-vote | **verified [exec]** |
| A1 restored to 24 / 21 / 615, A2 `better` 7, `WORSE` 0 | **verified [exec]** |
| A2's 7-vs-6 is `ENTRY_UNDERSHOOT_C` voting | **verified [exec]** — de-enrolling it gives exactly 6 |
| Negative tests: gate deleted -> 8 failures; `SETTLE_S` enrolled -> 10 | **both reproduce exactly [exec]** |
| `ENTRY_UNDERSHOOT_C` "measured never to fire" | true of the A1 *simulator* null only. On REAL captures it is the single most decision-moving axis and flips two of three A/B pairs to `REJECT_DEGRADED` **[exec]** |
| SETTLE_S band table (+-2.0 degC "0.79-1.00 on all 33"; second dwells "1.00, all twelve") | **REFUTED [exec]** — 36 instances, and z0's second dwell holds +-2.0 degC on only 0.59-0.71 of ticks in all six runs |
| "composites are once again comparable with those recorded before any axis was added" | **REFUTED [read]** — the composite now averages FOUR axes; before `d41da85f` it averaged three |
| `dwell_unsettled` "reported out of band, for humans" | **no consumer exists [exec]** — write-only outside tests |

## 1. `ENTRY_UNDERSHOOT_C` CAN fire on real data, and it is decisive there

**[exec]** `firing_score_from_capture` (production `firing_score.c` +
`firing_compare.c` linked as-is, built at working-tree HEAD) over the three
real matched A/B pairs in `logs/coupling/` (`ab_old_N` vs `ab_new_N`, the
coupling-matrix campaign of `logs/coupling/ab_campaign_report.md`):

| pair | `lag_s` med.norm | `entry_peak_c` med.norm | `entry_undershoot_c` med.raw / med.norm | verdict |
|---|---|---|---|---|
| 1 | -0.87 | **-1.29 (Bar 1)** | +0.66 / **+1.32 -> `degraded`** | **REJECT_DEGRADED** |
| 2 | -0.80 | **-1.17 (Bar 1)** | +0.415 / +0.83 | ACCEPT |
| 3 | **-1.20 (Bar 1)** | **-1.30 (Bar 1)** | +0.585 / **+1.17 -> `degraded`** | **REJECT_DEGRADED** |

Per-class values reach 1.27, 1.38, 1.09 and 1.00 degC of entry undershoot on the
`ab_new_*` arm against 0.00-0.43 degC on `ab_old_*`; `improved` is 0 of 6 in
all three pairs. So the axis is emphatically not inert on hardware: in two of
three pairs **it alone** fires the veto and overrides two axes that cleared or
nearly cleared Bar 1. On the null pair the commit's own script uses
(`noise_floor_p7_run1` vs `noise_floor_p7d_run3`) it is quiet — n=6,
median_raw 0.0000, worst single class 0.27 degC, i.e. inside the 0.5 degC floor.

**Judgement.** The axis is *not* spurious: the `ab_new` arm genuinely trades
overshoot for undershoot, and flagging that trade is exactly the
non-dominance rule's purpose, and the null-pair figure above is real evidence
its own noise sits inside the floor. But the commit's stated justification —
"measured never to fire in the A1 null experiment" — is an argument from
**inertness**, and it is the exact inverse of the principle the same document
uses to hold `SETTLE_S` back ("an axis earns a vote by being measured to carry
signal, not by existing"). Applied consistently, the evidence that should have
enrolled `ENTRY_UNDERSHOOT_C` is the hardware measurement above, which was not
made. Note also the historical consequence, offered as data and not as a
re-decision: the campaign that adopted the new coupling matrix would have had
two of its three pairs come out `REJECT_DEGRADED` under this comparator. (That
campaign was decided by `pid_ab_compare.py`, not by `firing_compare`, so this
is not a retro-reversal — but it is the size of the axis's influence.)

`ENTRY_UNDERSHOOT_C` is also partly a second view of `LAG_S`: a dwell entered
from a lagging ramp reads as undershoot in the dwell class and as lag in the
ramp class. The infeasibility exclusion removes the heater-limited part of
that, which is what keeps it from being R3's defect, but the correlation is
worth measuring before the axis is relied on further.

## 2. The 2.0 degC settle band: independently re-derived, and the doc's table does not reproduce

**[exec]** Independent re-derivation from the same six captures
(`logs/coupling/noise_floor_p7{,b,c}_run1.jsonl`,
`noise_floor_p7d_run{1,2,3}.jsonl`), per zone per dwell, replicating the
production exclusions (capture latch at `band_c` 5.0 degC, infeasibility drop
at duty >= 0.98 with err < 0 — which removed **zero** ticks here, so raw and
excluded figures are identical):

| band | first dwells (18) | second dwells (18) |
|---|---|---|
| +-0.5 degC | 0.00 - 0.48 | 0.00 - 0.41 |
| +-1.0 degC | 0.39 - 0.86 | 0.00 - 0.71 |
| +-1.5 degC | 0.68 - 1.00 | 0.20 - 1.00 |
| +-2.0 degC | 0.91 - 1.00 | **0.59 - 1.00** |

**36** dwell-zone instances, not 33; I could not reconstruct the 21/12 split the
document quotes. The load-bearing disagreement is the second dwells: the
document says +-2.0 degC is held "1.00 (all twelve)" there and 0.79-1.00 across
all instances. Measured here, **z0's second dwell holds +-2.0 degC on only
0.59-0.71 of its ticks in every one of the six runs** (p90 |err| 3.36-3.47 degC),
and at +-1.5 degC on 0.20-0.48. That is the same "saturated at its worst
reading" condition the document uses to reject 0.5 degC, one zone over.

The band's *direction* still survives: 2.0 degC dominates 1.5 and 1.0 on every
instance, and 1.0/1.5 are genuinely rejected on this evidence (1.5 degC leaves
z0's second dwell at 0.20-0.48, 1.0 degC leaves it at 0.00-0.22), so choosing
2.0 over them is evidence-backed and the rejection of the smaller bands was not
merely asserted. What is not supported is the stronger claim that 2.0 degC is a
band this plant holds everywhere.

**Does 2.0 degC discriminate?** Partly, and in the other direction from the
worry. Across the eight capture-derived firings scored above, roughly half of
all dwell classes report `settle_s == 0.0000` exactly (settled at the first
scored tick) — 8 of 12 classes in the null pair — while the rest spread over
47-277 s. The axis is bimodal: informative on slow dwells, pinned at the floor
on fast ones. Keeping it report-only is the right call on this evidence, and
the document's "open measurement" framing is honest.

## 3. `dwell_unsettled` is write-only outside tests; and the merge rule hides the worst case

**[exec]** A repo-wide grep for `dwell_unsettled` finds readers only in three
assertions in `firmware/KilnFW/App/test/test_iter_tune.c` (lines 227, 240,
275). Nothing in `iter_tune.c`, no HTTP route, no log line, no UI. The
document's "still reported out of band, for humans" is aspirational — it is a
**write-only diagnostic** today, this repo's documented
consumer-without-producer class with the arrows reversed. (Mitigating context:
`firing_score.c`/`firing_compare.c` have no live production caller at all yet —
`iter_tune.h` documents the intended wiring but `firing_score_seg_tick()` is
called only from tests and simulators — so no *existing* reporting path was
skipped. It still needs a consumer before the fact is "reported".)

**[read] The merge rule's real hazard is not bias, it is invisibility.**
`firing_score_set_add()`'s `else if (s == FIRING_SUBSCORE_SETTLE_S) e->has[s] = false;`
applies identically and independently to the baseline set and the trial set, so
it cannot favour one arm; the comparator then simply finds no matched pair for
that class. But note the shape: **a dwell that settles badly produces a
number, and a dwell that never settles at all produces silence.** Under
`FIRING_COMPARE_VOTING_MASK` today that is harmless. The moment `SETTLE_S` is
re-enrolled — which this document plans — a trial that stops settling entirely
will read as "nothing to say about settling" rather than as a degradation, i.e.
the axis will be blind at exactly its worst case. Whoever re-enrols it must
pair the enrolment with a rule that consumes `dwell_unsettled` (an unsettled
dwell count that *rises* is a degradation), not just flip the mask bit.

## 4. The three `_Static_assert`s all fail at compile time — verified

**[exec]** Byte-identical scratch copies of `firing_compare.h` /
`firing_score.h` (production untouched), each compiled as a one-line TU with
`cl /std:c11 /c`:

| variant | result |
|---|---|
| unmodified control | compiles, exit 0 |
| `LAG_SIGNED_S` moved report-only -> voting (isolates assert 3) | `firing_compare.h(117): error C2338: ... 'a signed sub-score must never vote in a lower-is-better comparator'` |
| `SETTLE_S` removed from report-only, added nowhere | `firing_compare.h(109): error C2338: ... 'every firing_subscore_t must be classified voting or report-only'` |
| `SETTLE_S` in BOTH masks | `firing_compare.h(112): error C2338: ... 'a sub-score cannot be both voting and report-only'` |

Hard errors, not warnings, in all three. The commit's negative test 2 is
therefore confirmed, and assert 3 — the one the commit did not negative-test
in isolation — is confirmed too.

**[read] Residual gap, not a defect in this commit:** nothing forces a *newly
added* signed axis to declare itself in `FIRING_SUBSCORE_SIGNED_MASK`. Assert 1
forces the author to classify the axis; assert 3 then only bites if they also
remembered to mark it signed. The dependency direction is sound and
non-circular (`firing_compare.h` includes `firing_score.h`; `firing_score.h`
includes neither), and putting the mask with the measurement is the right
choice — it is the completeness of that mask, not its location, that is
unenforced.

## 5. `have_any_sample` below the votes gate: NO_MATCHED_PAIRS is narrower than before

**[read]** The move is correct for its stated purpose — a report-only axis with
samples no longer makes the comparator claim the rule had evidence. One
newly-reachable state is worth recording: if every matched class contributes
samples on report-only axes ONLY, the function now returns `NO_MATCHED_PAIRS`
even when `matched_classes > 0` and even when `in_band_median_delta` is outside
tolerance — the `!have_any_sample` branch precedes the veto branch, so an
in-band veto computed in the same call is discarded. Before this commit that
case produced `REJECT_DEGRADED`. The change is in the accept-conservative
direction (no accept is manufactured) and no current caller can reach it, so it
is a note, not a defect. `matched_classes` remaining nonzero alongside a
`NO_MATCHED_PAIRS` verdict is also now normal; the field name refers to
classes, the verdict to adjudicable pairs.

## 6. A1 / A2 independently re-measured

**[exec]** `firmware/KilnFW/App/test/check_sim_iter_tune_bars.ps1`, canonical
n=220, PowerShell `-ExecutionPolicy Bypass`, foreground, at working-tree HEAD:

```
660 null comparisons: ACCEPT 24 (3.64%)  REJECT 21  INSUFFICIENT 615  NO_PAIRS 0
660 zone-runs over 220 mismatched plants: better 7, unchanged(<0.5C) 653, WORSE 0 (0.00%)
```

Exactly the document's section 3, rows 3 and 4. **The 7-vs-6 attribution is
confirmed by execution**, not by reading: with `ENTRY_UNDERSHOOT_C` moved from
`FIRING_COMPARE_VOTING_MASK` to `FIRING_COMPARE_REPORT_ONLY_MASK` (production
edit, restored by hand afterwards) the same script reports
`better 6, unchanged 654, WORSE 0` with A1 unchanged at 24/21/615 — that one
axis accounts for the entire deviation from `e78fbc5b`, and for nothing else.

## 7. The negative tests reproduce

**[exec]** Both behavioural negative tests were re-run on PRODUCTION code:

- **`if (!ss->votes) continue;` deleted** from `firing_compare.c`:
  `build_host_tests.ps1` exit 1, **8** `FAIL` lines, all in
  `test_report_only_subscores_cannot_change_a_verdict`, both directions,
  composite reading **-6.6667 / +6.6667** instead of 0. Matches the claim
  exactly, including the composite figures.
- **`SETTLE_S` moved into `FIRING_COMPARE_VOTING_MASK`**: exit 1, **10** `FAIL`
  lines across BOTH guard tests — the name pin (`test_iter_tune.c:474`), the
  voter count (`:483`), the `votes` flag on the result (`:534`, twice), Bar 1
  (`:539`), the veto (`:541`), the verdict (`:546`, twice) and the composite
  (`:549`, -5.0000 / +5.0000). Matches the claim exactly.
- The compile-error test was performed on byte-identical scratch copies
  (section 4).

Both production files were then restored by hand and confirmed byte-identical
(`git status --porcelain` on `firing_compare.c`/`.h`: empty),
`firmware/KilnFW/App/test/build/` was deleted, and a full rebuild gave
**38/38 executables built and passed**, exit 0.

## 8. Refutation: the composite is NOT comparable with pre-`d41da85f` records

**[read]** Section 2.1 states the report-only axes stay out of
`composite_normalised` "so composites are once again comparable with those
recorded before any axis was added". They are not. `e78fbc5b`'s
`firing_score.h` has `FIRING_SUBSCORE_COUNT = 3`, so its composite is the mean
of `LAG_S`, `ENTRY_PEAK_C`, `STEADY_RMS_C`. Today's composite averages those
three **plus `ENTRY_UNDERSHOOT_C`** — four axes. The composite is printed in
iter_tune's own per-firing decision line ("composite -0.007 floors"), so this is
a reported number whose basis changed. The correct statement is that the
composite excludes the two report-only axes; comparability with pre-`d41da85f`
records is a separate claim, and it is false.

## Verdict

The structural change — explicit enrolment, a compile error for an unclassified
axis, statistics for every axis and adjudication for four — is sound, is the
right fix for `d41da85f`'s defect, and its guards genuinely fail when broken.
Three corrections are owed to the record: the `SETTLE_S` band table does not
reproduce (z0's second dwell does not hold +-2.0 degC), the composite is not
pre-`d41da85f`-comparable, and `dwell_unsettled` has no consumer. One judgement
is owed on `ENTRY_UNDERSHOOT_C`: the vote is defensible on evidence that now
exists (quiet on the null pair, real signal on hardware), but it was taken on
the opposite argument, and the axis is live enough on real captures to have
flipped two of three historical A/B pairs — precisely the "inert in the guard
test, live on hardware" shape this commit set out to eliminate.
