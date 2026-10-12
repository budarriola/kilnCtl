# On/off device: bind to a spare relay instead of a zone slot — 2026-09-14

> **Superseded, 2026-10-04:** the owner approved spare-relay binding anyway. `docs/SPARE_RELAY_ONOFF_PLAN.md` takes the alternative this audit did not cost: a small parallel aux-outputs store, so `zones[]` is not widened and `ZONES_CFG_VERSION` is not bumped. The cost analysis below still holds for widening the zone array.

**Owner request, verbatim:** *"We should allow support for up to 3 zones like
my test kiln. All unused relays may be used for on off control test kiln"*

**Interpretation carried into this pass:** heating zones stay capped at 3
(matching the thermocouple count, unchanged), and an on/off device should use
a **spare relay** rather than consuming one of those 3 heating-zone slots. The
board has 3 thermocouples and 4 relays; today an on/off device is a
`ZONE_TYPE_ON_OFF` zone occupying one of the 3 zone-array slots, so a 3-zone
kiln that also wants a vent has no slot left for it.

**Verdict: the code does not support this reading as a small change. It is
genuinely large (M–L), for the reason `docs/audits/on_off_zone_decisions_
2026-09-14.md` D1 already gave when this exact option was evaluated and
rejected on 2026-09-14 (same day, earlier decision). Not implemented.**

---

## 1. Where the "consumes a zone slot" coupling actually lives

An on/off device today is not a separate entity — it **is** a zone, typed
`ZONE_TYPE_ON_OFF` (`docs/ON_OFF_ZONE.md` sec 1), and every zone lives in
one fixed-size array:

- **`zone_cfg_t zones[MAX31856_CHANNEL_COUNT]`** —
  `firmware/KilnFW/App/drivers/persist/zones_config_json.h:2238`, the
  persisted config struct. `MAX31856_CHANNEL_COUNT` is `THERMO_CHANNEL_COUNT`,
  `#define THERMO_CHANNEL_COUNT 3u`
  (`firmware/KilnFW/App/drivers/common/uart_task_ids.h:289`).
- The relay a zone drives is `zone_cfg_t::relay_mask`
  (`zones_config_json.h:463`, one bit per relay, bounded to the board's
  configured relay count) — **a property of a zone, not a standalone
  binding.** There is no relay-to-device binding independent of a zone index
  anywhere in the persisted schema.
- The same `MAX31856_CHANNEL_COUNT` bound sizes every per-zone runtime array
  the executor, the thermal guards, the coupling matrix (`zone_coupling_
  solve.c`, an N×N matrix over zone index), the firing-stats record
  (`profile_firing_run_record_t::zones[MAX31856_CHANNEL_COUNT]`), and the
  `/api/zones` HTTP surface (`GET`/`POST` iterate `zone_index` 0..2).

So "bind an on/off device to a spare relay without costing a heating zone
slot" requires a **fourth (or more) addressable on/off slot that is not one
of the 3 `zone_cfg_t` entries** — either widening the `zones[]` array past 3,
or adding a second, parallel array of on/off-only entries. Both are schema
changes: the array widens past the size everything downstream was written
against, or a second array needs its own persisted type, its own migration,
and its own threading through every one of the guard/coupling/firing-stats/
HTTP consumers listed above, none of which currently accept an index outside
`0..MAX31856_CHANNEL_COUNT-1`.

**This is not a small, isolated coupling** (e.g. "an on/off zone's relay_mask
happens to also appear in a heating zone's relay_mask, refuse that overlap")
— it is the zone *array itself* being the only mechanism that gives an on/off
device an identity, a config record, and a place in every per-zone loop in
the firmware.

## 2. This was already evaluated today, and rejected, for the same reason

`docs/audits/on_off_zone_decisions_2026-09-14.md` D1 considered exactly this
shape of change — TC-less on/off slots appended past the 3 thermocouple-backed
ones — as the "what would flip it" case if the owner's kiln needed all 3
zones heating plus a spare-relay device. It rated that path **M–L**, "still
bounded and worth doing on a real requirement," but explicitly recommended
**against** widening the array as the default answer, given:

- `MAX31856_CHANNEL_COUNT` sizes the persisted config array and every
  consumer of it (quoted above).
- `ZONES_CFG_VERSION` is at 26 — a widening needs a frozen `zone_cfg_v26_t`,
  a converter, and a CRC check on the migration path, the same discipline
  every prior version bump in `zones_config_json.h` already carries (see the
  `_Static_assert(offsetof(...))` pattern at e.g. line 1211, 1321, 1423,
  1523 for past bumps).
- **A migration defect is under active investigation right now** (a separate
  agent, concurrently, diagnosing possible plant-model loss potentially tied
  to config migration). This task's own instructions singled out schema
  changes for extra caution for exactly this reason, and to "prefer a design
  that needs no schema change if one exists."

No design that satisfies the owner's literal request ("use unused relays
without costing a zone slot") avoids widening the zone-identified array or
adding a parallel one — both are schema changes on the same struct family
under active scrutiny. Per this task's own instruction — *"If the code does
not support that reading, say so and stop rather than forcing it"* — this
pass did not implement the decoupling.

## 3. What this pass did instead

Per the task's remaining items, independent of the D1 question:

1. **Closed the `adaptive_tune` gap** (`docs/audits/on_off_zone_decisions_
   2026-09-14.md` sec 2, item 1): `adaptive_tune_run_end()`
   (`firmware/KilnFW/App/drivers/control/adaptive_tune.c:573`) now refuses to
   train on a `zone_is_on_off()` zone with its own named skip reason, ahead of
   the pre-existing `!zr->active` check. Negative-tested by hand: disabled the
   new branch (`false && zone_is_on_off(zi)`), confirmed the new
   `test_run_end_skips_on_off_zone_even_with_ring_data_present()` (`test_
   adaptive_tune_status.c`) goes RED, restored by hand, deleted `firmware/
   KilnFW/App/test/build` and rebuilt clean, confirmed 40/40 host-test
   executables build and run green again.
2. **`firing_score`/`firing_compare` were checked for contention first**
   (per this task's instruction) — `git status --porcelain` showed no
   modification or untracked state on either file, so they were not
   contended. On inspection, `firing_score.c`/`firing_compare.c` operate on
   opaque per-segment data supplied by a caller and have **no live production
   caller yet** (`firing_score.c:246`'s own comment: "firing_score has no
   live production caller yet at all") — there is no zone-index-aware loop in
   either file for an on/off exclusion to attach to; the exclusion belongs at
   the (not yet built) call site. The real production gap in this family is
   `profile_executor_firing_stats.c:200`'s `firing_stats_build_record()`,
   which snapshotted `zr->active = z->active` with no zone-type check,
   feeding `adaptive_tune` (and any future firing-history consumer)
   unfiltered on/off-zone data. Fixed: `zr->active = z->active &&
   !zone_is_on_off(zi)`, so an on/off zone is now reported as inactive in
   every firing record, matching `docs/ON_OFF_ZONE.md` sec 1's "Firing
   stats / IAE: Skip" row.
3. **`docs/SAFETY_CASE.md`** now carries the two mandated on/off entries
   (items 12 and 13 in sec 3): the guard-3 (welded-contactor) coverage gap
   accepted for this zone type, and the `max_temp_c == 0` relaxation for a
   TC-less on/off zone, cross-referenced to the actual `zone_needs_ceiling()`
   test (`test_zones_http.c:7174`).
4. **`ROADMAP.md`'s on/off row** updated: no longer "design only, 4 questions
   open" (stale since before 2026-09-07's shipped steps); now reflects steps
   1-8 shipped, this pass's two gap closures, and the still-open step 9 bench
   session plus the unresolved D1/spare-relay question.

## 4. Verification

- `firmware/KilnFW/App/test/build_host_tests.ps1`: 40/40 executables built
  and run green (one transient `zones` float-fit test failure on the first
  run, `test_zones_http.c:4358` "zone 1 (a real fit) reports
  fuzzy_model_valid:true", unrelated to any file this pass touched — no diff
  on `zones_config_*`/`test_zones_http.c`; a clean rebuild after the
  negative-test restore passed 40/40 with no failures).
- `tools/run_all_checks.ps1`: **94/94 passed, 0 skipped, 0 failed** (both
  before and after the negative-test restore/rebuild).
- Negative test: see sec 3 item 1 above.

## 5. Files touched

- `firmware/KilnFW/App/drivers/control/adaptive_tune.c` — on/off skip branch.
- `firmware/KilnFW/App/drivers/control/profile_executor_firing_stats.c` —
  on/off exclusion in `firing_stats_build_record()`.
- `firmware/KilnFW/App/test/test_adaptive_tune.c` — `zone_is_on_off()` fake
  fixture, reset wiring.
- `firmware/KilnFW/App/test/test_adaptive_tune_status.c` — new negative-tested
  regression test.
- `docs/SAFETY_CASE.md` — items 12, 13.
- `ROADMAP.md` — on/off row rewritten.
- This file.

## 6. Not done, and why

- **Spare-relay binding for on/off devices** — sec 1-2 above: genuinely
  M–L, requires a `ZONES_CFG_VERSION` schema change on a struct family under
  active migration-defect investigation elsewhere. Not implemented.
  Recommend the owner weigh this against `docs/audits/on_off_zone_decisions_
  2026-09-14.md` D1's original recommendation (accept the 3-slot cost) before
  requesting the M–L work, and that it happen only once the concurrent
  migration investigation has concluded.
- **Step 9 bench session** — explicitly out of scope for this pass (needs
  the board, currently under investigation).
