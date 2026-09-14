# Two observability gaps closed: `dwell_unsettled` and fuzzy-inactive-no-model (2026-09-14)

Closes the two gaps left open by `560cffe0`/its review (appended to
`docs/audits/firing_score_subscore_enrolment_2026-09-14.md`) and by `233ded79`
(`docs/audits/fuzzy_no_model_no_fuzzy_2026-09-14.md`). Scope: `firing_score.{c,h}`,
`firing_compare.{c,h}` (unchanged), `zones_http_get.c`, and their tests. No
verdict/voting behaviour in `firing_compare` was touched.

## GAP 1: `dwell_unsettled` -- removed, not surfaced

`dwell_unsettled` (`firing_segment_score_t`) was added at `560cffe0` to report
"this dwell never settled" out of band, for humans, once `has[SETTLE_S]`
switched to reporting `false` instead of a sentinel duration. A same-day
review (section 3 of `docs/audits/firing_score_subscore_enrolment_2026-09-14.md`)
found it had **no consumer anywhere outside three assertions in
`test_iter_tune.c`** -- not `iter_tune.c`, no HTTP route, no log line.

**Decision: remove it.** Reasoning:

1. **No real reader exists to surface it to.** `firing_score`/`firing_compare`
   have no live production caller at all yet (`iter_tune.h` documents the
   intended wiring; `firing_score_seg_tick()` is called only from tests and
   simulators today). `iter_tune.c` is outside this pass's ownership and this
   pass's concurrency lane, so inventing a caller there — the only place a
   "for humans" surface could currently mean anything — would be scope creep
   into a file another lane may be mid-edit on, not a fix to the field itself.
2. **The anti-sentinel fix does not depend on the counter.** The actual
   defect `560cffe0` fixed was a *never-settling* dwell reporting a fabricated
   duration indistinguishable from a *slow-but-settled* one. That is fixed by
   `has[SETTLE_S] == false` alone — a bool, not a magnitude a difference can
   be taken of — which is what `firing_compare`'s own "nothing to say about
   this sub-score" first-class outcome already consumes. `dwell_unsettled`
   added a count on top of that fact; it never carried the fix itself.
3. **The task's own instruction: do not add a consumer just to justify a
   field's existence.** The honest options were surface-to-something-real or
   remove; with no real reader reachable from this pass's scope, remove is
   the non-fabricated choice.

Removed: the `dwell_unsettled` field (`firing_score.h`), the two write sites
in `firing_score_seg_finish()`/`firing_score_set_add()` (`firing_score.c`),
and the three assertions in `test_iter_tune.c` that were its only readers.
Left in place, unchanged: `has[SETTLE_S] == false` on a never-settled dwell,
the merge rule that keeps a merged class `has == false` unless *every*
same-class dwell settled, and the tests covering both
(`test_settle_time_never_settling_reports_nothing_not_a_sentinel`,
`test_settle_merge_requires_both_dwells_to_have_settled`) — with their
`dwell_unsettled` assertions dropped, their `has[SETTLE_S]` assertions intact.

`firing_score.c` carries a comment at the removed call site pointing future
work at re-adding a statistic like this only once an actual caller needs it,
shaped to what that caller consumes — not resurrecting the same
write-only shape.

## GAP 2: fuzzy-inactive-because-no-model -- new field, on `/api/zones`

`233ded79` made a zone with no identified plant model run plain PID instead
of fuzzy on invented membership bands (`docs/audits/fuzzy_no_model_no_fuzzy_
2026-09-14.md`). Until now an operator could only infer "fuzzy is configured
but not actually running" by noticing `model_k_dc` reads 0 (or non-finite,
or `model_tau_s` non-positive) alongside a non-zero `fuzzy_strength_pct` --
two fields, an implicit AND, and a sign convention the operator has to know.

**New field: `fuzzy_model_valid` (bool), on `GET /api/zones`, per zone.**

- **Endpoint choice: `/api/zones`, not `/api/zones_diag`.** This is
  operator-facing state describing what the control loop is doing right now
  (same footing as `fuzzy_strength_pct` itself, which already lives here),
  not an engineering diagnostic. `/api/zones_diag` exists to hold fields with
  no rendering consumer (`docs/audits/zones_diag_endpoint_split_2026-09-14.md`);
  this field's whole purpose is to be shown to an operator.
- **Computed, not stored**, in `zones_http_get.c`'s `zones_get_handler()`,
  by calling `pid_fuzzy_derive_bands(z->model_k_dc, z->model_tau_s, NULL,
  NULL)` -- the exact same predicate `profile_executor_pid_tick.c`'s
  `resolve_fuzzy_bands()` calls at the live control tick. `zone_model_at()`'s
  `T_c` parameter is an unused passthrough today
  (`zones_config_accessors.c`'s `zone_model_at()` calls straight through to
  `zones_config_get_model()`, ignoring `T_c`), so the persisted
  `model_k_dc`/`model_tau_s` fields this handler already has in hand are
  bit-for-bit what the live tick consults -- not an approximation of it, and
  not dependent on a live temperature reading this GET handler doesn't have.
- **Raw fact, not a verdict** -- house convention throughout this file. A
  client combines it with the already-emitted `fuzzy_strength_pct` itself
  (`fuzzy_strength_pct > 0 && !fuzzy_model_valid` == "configured but
  inactive: no identified plant model") rather than the field encoding that
  combination itself.
- **Always emitted**, same discipline as every other field in the zone
  object: an absent key and a "valid" client default must never mean the
  same thing.
- **PC client**: added to `_ZONE_READONLY_KEYS`
  (`tools/PcTools/src/kilnctrl/zones_http_client.py`) as a new
  `_ZONE_FUZZY_MODEL_VALID_READONLY_KEYS` set -- there is no `z%u_` POST key
  for it and there should never be one; it is derived fresh on every GET
  from fields (`model_k_dc`/`model_tau_s`) that already round-trip.
  `check_zones_per_zone_field_drift.ps1` re-run clean (below).

### Headroom

Adding this field cost **75 of the 854 bytes** of headroom
`docs/audits/zones_diag_endpoint_split_2026-09-14.md` recovered
(`"fuzzy_model_valid":true,` = 25 bytes x `MAX31856_CHANNEL_COUNT` (3) zones).

`test_zones_get_handler_max_width_response_fits_json_cap()`
(`firmware/KilnFW/App/test/test_zones_http.c`), re-run against the real
handler with every field pinned at its documented max (including
`model_k_dc`/`model_tau_s` at `ZONE_MODEL_K_MAX`/`ZONE_MODEL_TIME_MAX_S`,
which necessarily makes `fuzzy_model_valid` render `"true"`, the narrower of
the two boolean literals -- see the test's own comment on why that is the
true joint worst case rather than an under-measurement: forcing `"false"`
requires shrinking those two fields well below their own documented max,
losing far more than the one byte `"false"` vs `"true"` would gain):

```
GET /api/zones max-width render: 6581 bytes, against json_cap=7360 -- measured headroom = 779 bytes
```

**779 bytes of headroom remain.** `json_cap` (7360) was not touched.
`GET /api/zones_diag`'s own `json_cap` (1024) and headroom (284 bytes,
740-byte render) are unaffected -- this field never went there.

### Test coverage

- `test_get_emits_fuzzy_model_valid()` (`test_zones_http.c`): a zone with
  `model_k_dc`/`model_tau_s` both 0 (the "never autotuned" sentinel) and
  `fuzzy_strength_pct = 75` reports `fuzzy_model_valid:false`; a sibling
  zone with a real fit and the same `fuzzy_strength_pct` reports
  `fuzzy_model_valid:true`. Locates each zone's own JSON object first (by
  its `"index":N,` marker) so a false/true swap between the two zones cannot
  be masked by a substring match landing in the wrong zone.
- `test_zones_get_handler_max_width_response_fits_json_cap()` extended with
  `fuzzy_model_valid`'s own worst-case reasoning (see Headroom above).
- `check_zones_per_zone_field_drift.ps1` extended implicitly: it now expects
  `fuzzy_model_valid` in `_ZONE_READONLY_KEYS`, so a firmware-side rename or
  removal without a matching client update fails it (verified clean run
  below).

## Negative tests (production broken, confirmed red, restored by hand, full rebuild)

Three, all against production code, all restored by hand (no `git checkout`/
`restore`/`stash`), `firmware/KilnFW/App/test/build/` deleted and fully
rebuilt before every subsequent measurement.

1. **`zones_http_get.c`: `fuzzy_model_valid` hardcoded to `true`** (deleting
   the `pid_fuzzy_derive_bands()` call). `build_host_tests.ps1`: **RED** --
   `zones_http` executable run-failed with
   `test_zones_http.c:4356: zone 0 (no identified model, but
   fuzzy_strength_pct=75) reports fuzzy_model_valid:false -- configured but
   inactive`. Confirms the new field's own test actually discriminates.
2. **`firing_score.c`: restored the pre-2026-09-14 SETTLE_S sentinel**
   (commented out the `&& !seg->settle_outside_at_end` guard, so a
   never-settled dwell reports `has[SETTLE_S] = true` with a fabricated
   duration again). `build_host_tests.ps1`: **RED**, 2 failures --
   `test_iter_tune.c:226` ("never-settled reports has == false...") and
   `test_iter_tune.c:271` ("the merged class reports no settle time..."),
   the two SETTLE_S-sentinel guard tests that remain after `dwell_unsettled`'s
   own assertions were removed from them. Confirms removing `dwell_unsettled`
   did not weaken the actual anti-sentinel guard -- it was never load-bearing
   for that guard, exactly as GAP 1's reasoning above claims.
3. Removal completeness for GAP 1 was verified by construction rather than a
   third break-and-restore: `grep -rn dwell_unsettled firmware/` after the
   edit finds zero references outside this document and the (unchanged)
   prior audit doc's own historical record -- there is no leftover call site
   a stray edit could have silently kept alive.

Each break was restored by retyping the exact original line, confirmed by
`git diff --stat` showing the intended net change only (see below), then
`firmware/KilnFW/App/test/build/` was deleted and rebuilt from scratch before
the final, clean 40/40 measurement.

## A1/A2 re-confirmed unchanged

Neither gap touches `firing_compare.c`'s voting logic, floors, or masks.
`check_sim_iter_tune_bars.ps1`, canonical n=220, re-run after the final clean
rebuild:

```
660 null comparisons: ACCEPT 24 (3.64%)  REJECT 21  INSUFFICIENT 615  NO_PAIRS 0
660 zone-runs over 220 mismatched plants: better 7, unchanged(<0.5C) 653, WORSE 0 (0.00%)
```

**Exactly 24/21/615, A2 `better` 7, `WORSE` 0** -- unchanged from
`docs/audits/firing_score_subscore_enrolment_2026-09-14.md`'s own
post-review figure.

## Check tally

- `firmware/KilnFW/App/test/build_host_tests.ps1`: **40/40 executables
  built and passed**, 0 build failures, 0 run failures (full rebuild from a
  deleted `build/` directory).
- `pytest tools/PcTools/tests`: **2307 passed, 13 skipped** in 937s (prior
  baseline handed off as 2308/12; net total unchanged at 2320 -- one test's
  pass/skip split moved, attributable to the three concurrent agents'
  in-flight work elsewhere in the tree, not to anything touched here).
- `check_00_kilnfw_target_build.ps1` (full ESP-IDF target build, clean
  worktree mirror): **PASS**.
- `tools/run_all_checks.ps1`: **94/94 passed, 0 skipped, 0 failed**
  (includes `check_sim_iter_tune_bars.ps1`, `check_zones_per_zone_field_
  drift.ps1`, `check_pid_fuzzy_drift.ps1`, `check_doc_hash_citations.ps1`,
  `check_test_c_files_wired.ps1`, `check_test_has_assertions.ps1`).
  The two failures reported as pre-existing at handoff time
  (`check_zones_per_zone_field_drift.ps1` on a stray negative-test stub,
  `selfcheck.py`) were both green in this run -- their owners appear to have
  already landed fixes; not investigated further here as they are outside
  this pass's ownership.

## Files touched

- `firmware/KilnFW/App/drivers/control/firing_score.h` -- removed
  `dwell_unsettled` field.
- `firmware/KilnFW/App/drivers/control/firing_score.c` -- removed the two
  `dwell_unsettled` write sites; comment left at the removed call site.
- `firmware/KilnFW/App/test/test_iter_tune.c` -- removed the three
  `dwell_unsettled` assertions; `has[SETTLE_S]` assertions in the same tests
  unchanged.
- `firmware/KilnFW/App/drivers/http/zones_http_get.c` -- new
  `fuzzy_model_valid` field on `GET /api/zones`'s per-zone object; `pid_
  fuzzy.h` included; `json_cap` comment chain updated with the new headroom
  figure (779 bytes).
- `firmware/KilnFW/App/test/test_zones_http.c` -- `pid_fuzzy.c` included (for
  `pid_fuzzy_derive_bands()`, needed to link this field's call site into the
  `#include`-based test executable); new
  `test_get_emits_fuzzy_model_valid()`; max-width test extended with
  `fuzzy_model_valid`'s own worst-case reasoning and updated headroom
  literal (854 -> 779 bytes).
- `tools/PcTools/src/kilnctrl/zones_http_client.py` -- `fuzzy_model_valid`
  added to `_ZONE_READONLY_KEYS`.

Hashes cited above (`git cat-file -t <hash>`, all confirmed `commit`):
`560cffe0`, `9067c1f4`, `d41da85f`, `e78fbc5b`, `233ded79`, `f59b21c8`,
`992f3954`.
