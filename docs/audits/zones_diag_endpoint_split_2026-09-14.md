# GET /api/zones_diag endpoint split (2026-09-14)

Implements the primary recommendation of `docs/audits/zones_json_headroom_plan_2026-09-14.md`
sec 3a: `GET /api/zones` (`firmware/KilnFW/App/drivers/http/zones_http_get.c`) had only 161
bytes of headroom left in its 7360-byte `json_cap`. This task re-verified the plan's field
list and byte figures against the real code before moving anything, found one factual error in
the plan (tuning_* is NOT diagnostics-only -- see sec 1), and implemented a narrower split than
originally proposed.

## 1. Re-verifying the plan's premise (the actual work of this task)

The plan's sec 3a recommendation named four field groups as move candidates: `tuning_*` (11
keys/zone, 927 bytes), `model_fit_temp_c`/`model_fit_ambient_c` (177 bytes), `coupling_tau_c%u`/
`coupling_dead_time_c%u` (558 bytes), and `autotune_baseline_k_dc` (not moved here -- not
requested, and not on the original headroom-driving list). Total claimed: ~1662 bytes.

**Re-checking `zones_page.html` directly (not trusting the plan's grep) found the plan's
`tuning_*` claim is wrong.** `zones_page.html`'s `renderTuningQuality()` (around line 3006-3038)
renders `tuning_valid`, `tuning_method`, `tuning_rule`, `tuning_settled`,
`tuning_extrapolation_converged`, `tuning_tau_consistent`, `tuning_baseline_c`,
`tuning_step_ambient_c`, `tuning_raw_rise_c`, `tuning_rise_inf_c`, and `tuning_seq` into a live
table (`#tuningQuality`), called from `loadCurrent()`'s single `/api/zones` fetch handler
(`renderTuningQuality(data.zones)`, line 2037) -- the SAME fetch that populates the rest of the
page. Moving `tuning_*` to a second endpoint would have either broken this table or forced the
operator page into a second fetch on every load, which the task's own instructions rule out
("A field the operator page turns out to use is not movable ... the web UI must keep working
without a second fetch on the main zones page"). **`tuning_*` was NOT moved.**

Re-checking the other two groups against `zones_page.html` (grepped directly for
`model_fit_temp_c`, `model_fit_ambient_c`, `coupling_tau_c`, `coupling_dead_time_c`) found zero
hits for any of the six keys -- confirming the plan's claim for these two groups. Checked against
`tools/PcTools/src/kilnctrl/zones_http_client.py`: `coupling_tau_c%u`/`coupling_dead_time_c%u`
are matched only by `_ZONE_COUPLING_TAU_DEAD_TIME_CELL_RE` and excluded from POST bodies outright
(no z%u_ POST field exists for either -- `autotune_engine.c`'s `finalize_fit()` is the sole
writer); `model_fit_temp_c`/`model_fit_ambient_c` are listed in `_ZONE_MODEL_FIT_READONLY_KEYS`,
same treatment. Checked against `tools/PcTools/src/kilnctrl/mcp_server_control.py`:
`_describe_model_fields()` reads `model_fit_temp_c`/`model_fit_ambient_c` directly (added
2026-09-13, `docs/audits/mcp_zone_model_fields_2026-09-13.md`); `coupling_tau_c%u`/
`coupling_dead_time_c%u` are not read anywhere in that file. **Both groups were moved.**

**Byte re-measurement** (host build, `build_host_tests.ps1 -OutDir C:\wt\kfw_host_test_zonesdiag`,
then `kilnctl_host_tests_zones.exe`), confirmed independently rather than trusting the plan's
hand-derived numbers:

- Before this change: `GET /api/zones max-width render: 7199 bytes, against json_cap=7360 --
  measured headroom = 161 bytes` -- matches the plan's figure exactly.
- After moving only `coupling_tau_c%u`/`coupling_dead_time_c%u`/`model_fit_temp_c`/
  `model_fit_ambient_c` (NOT `tuning_*`): `GET /api/zones max-width render: 6506 bytes, against
  json_cap=7360 -- measured headroom = 854 bytes`. **Recovered 693 bytes (161 -> 854 headroom),
  not the plan's claimed ~1662-byte/1800-headroom figure -- the difference is entirely the
  927-byte `tuning_*` group this task found could not move.**

## 2. New route: `GET /api/zones_diag`

Added `zones_diag_get_handler()` to `firmware/KilnFW/App/drivers/http/zones_http_get.c` (same
file as `zones_get_handler()`, reusing its `APPEND()` macro pattern, heap-allocation discipline,
and truncation-guard shape -- no parallel serializer). Declared in
`firmware/KilnFW/App/drivers/persist/zones_http_internal.h`; registered in
`firmware/KilnFW/App/drivers/http/zones_http.c` (`get_diag_uri`, right after `get_uri`).

Response shape:
```json
{"zones": [
  {"index": 0, "coupling_tau_c0": 0.0, "coupling_dead_time_c0": 0.0, "coupling_tau_c1": ...,
   "coupling_dead_time_c1": ..., "coupling_tau_c2": ..., "coupling_dead_time_c2": ...,
   "model_fit_temp_c": -273.15, "model_fit_ambient_c": -273.15},
  ...
]}
```

Dense over `MAX31856_CHANNEL_COUNT`, indexed the same way `GET /api/zones`' own `zones` array is,
so a consumer that fetches both can line them up by `index`. Same sentinel conventions as before
the move: `-273.15` (`ZONE_MODEL_FIT_TEMP_UNKNOWN`) still means "no operating point recorded",
emitted verbatim; `coupling_tau_c%u`/`coupling_dead_time_c%u` are still emitted for every cell
including the always-zero diagonal, same [affected][stepped] orientation as before.

**Buffer discipline, same class as `GET /api/zones`'s own hazard:** a NEW heap buffer (`heap_
caps_malloc`, `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` -- not an httpd-worker-stack local), sized at
1024 bytes and pinned by `test_zones_diag_get_handler_max_width_response_fits_json_cap()`
(`firmware/KilnFW/App/test/test_zones_http.c`), which drives the real handler with every
coupling/model-fit field at its documented `ZONE_*_MAX` bound (same constants the `/api/zones`
sibling test uses). Measured: **740 bytes worst case, 1024-byte cap, 284 bytes headroom.** This
is a new, small buffer, not an enlargement of the existing 7360-byte `json_cap` in
`zones_get_handler()`, which is untouched and still must never simply be grown (two documented
httpd-worker-stack-local panics, standing rule -- see that function's own comment).

## 3. Consumers updated

- **`tools/PcTools/src/kilnctrl/zones_http_client.py`**: added `get_zones_diag(host, timeout)`
  (parallel to `get_zones()`) and `merge_zones_diag(zones_json, diag_json)` (a pure, non-
  mutating helper that merges a diag response's per-zone fields back into a `/api/zones`
  response's zone dicts by `index`, for any caller that wants the pre-split combined shape).
  `_ZONE_MODEL_FIT_READONLY_KEYS` and `_ZONE_COUPLING_TAU_DEAD_TIME_CELL_RE` are UNCHANGED --
  they still correctly describe "never repost these", now regardless of which endpoint a caller
  fetched them from. New `_ZONES_DIAG_MODEL_FIT_KEYS` constant names exactly the two literal
  keys the diag endpoint owns (used by both `merge_zones_diag()` and the drift check below).
  New tests: `GetZonesDiagTest`, `MergeZonesDiagTest` in `tools/PcTools/tests/
  test_zones_http_client.py`. `get_zones()` itself is UNCHANGED (still one HTTP request) -- the
  40+ existing tests that mock exactly one `urlopen` response for it were deliberately left
  alone rather than widened to expect two calls.

- **`tools/PcTools/src/kilnctrl/mcp_server_control.py`**: `control_get_zones()` now also calls
  `get_zones_diag()` and merges the result onto `zones_json` via `merge_zones_diag()` before
  calling `_describe_model_fields()`/`_describe_coupling_matrix()` -- neither of those functions
  needed to change, since they still just read `zones_json["zones"]`. A diag-fetch failure is
  caught and swallowed (`except zones_http_client.ZonesHttpError: pass`): the model-fit fields
  degrade to their existing "missing" rendering path rather than failing the whole tool call.
  Docstrings updated to say a second request now happens. Existing tests
  (`test_mcp_server_control_model_fields.py`, `test_mcp_server_control_coupling.py`) updated to
  mock `get_zones_diag` too (previously an unmocked real network call with an 8s timeout would
  have silently slowed those tests down ~8s each while still passing via the graceful-
  degradation path -- now mocked to fail/return fast, and `test_mcp_server_control_model_
  fields.py`'s mock exercises the real `merge_zones_diag()` code path rather than bypassing it).

- **`tools/PcTools/check_zones_per_zone_field_drift.ps1`** /
  `tools/PcTools/selfcheck_zones_fields.py`: `_extract_get_per_zone_keys()` now takes a
  `handler_name` parameter (default `zones_get_handler`) so the same extractor covers
  `zones_diag_get_handler()` too (both functions emit their `"zones":[` array via the identical
  `for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {` marker shape, by construction).
  `zones_per_zone_field_table_checks()` now runs TWO comparisons: `zones_get_handler()`'s real
  keys against `client_get_keys` (with `_ZONES_DIAG_MODEL_FIT_KEYS` explicitly subtracted out,
  since those two keys correctly still live in `_ZONE_READONLY_KEYS` -- a caller that
  `merge_zones_diag()`'d them back onto a zone dict must still never repost them -- but are no
  longer expected to be found on the `/api/zones` side of the wire), and
  `zones_diag_get_handler()`'s real keys against `_ZONES_DIAG_MODEL_FIT_KEYS | {"index"}`. Ran
  directly (`python tools/PcTools/zones_per_zone_field_drift_check.py`): both comparisons report
  `missing from client: [], extra in client: []`.

## 4. Negative tests (required before/after this change)

All four broken-then-restored-by-hand, full clean rebuild each time (`Remove-Item -Recurse
-Force` on the host-test output dir, per the repo's standing negative-test rule):

1. **`/api/zones` max-width test still catches an overflow**: re-added a 230-byte oversized
   field (`"NEGATIVE_TEST_OVERFLOW_FIELD_DELETE_ME"`) to `zones_get_handler()` right after
   `coil_power_w`, same injection point the original headroom plan used. Result: FAIL, with the
   updated message correctly reporting the post-split 854-byte headroom baseline and pointing at
   this doc. Removed by hand; host-test dir deleted and rebuilt from clean; re-measured 6506
   bytes / 854 bytes headroom exactly, confirming the restore was clean.
2. **`/api/zones_diag` max-width test catches an overflow**: added an equivalent oversized field
   to `zones_diag_get_handler()`. Result: FAIL (`"...did not fit...raise the buffer"` /
   `strstr` for `"zones":[{"` fails once truncation fires). Removed by hand; rebuilt from clean;
   re-measured 740 bytes / 284 bytes headroom exactly.
3. **Drift check catches a genuine firmware/client mismatch**: temporarily added an extra literal
   key (`"coupling_tau_c%u_v2":%.1f` is not representable without a real code change, so instead
   a real field addition was simulated by adding `"\"diag_test_extra_field\":%u,"` with a
   throwaway arg to `zones_diag_get_handler()`'s per-zone loop, with no matching client-side
   entry). Result: `python tools/PcTools/zones_per_zone_field_drift_check.py` FAILED --
   `zones_diag per-zone GET keys: ... (missing from client: ['diag_test_extra_field'], ...)`.
   Removed by hand; re-ran clean; check passes again (`missing from client: [], extra in
   client: []` on both comparisons).
4. **Drift check catches a stale client-side entry**: temporarily removed `model_fit_ambient_c`
   from `_ZONES_DIAG_MODEL_FIT_KEYS` in `zones_http_client.py`. Result: FAILED --
   `zones_diag per-zone GET keys: ... (missing from client: ['model_fit_ambient_c'], ...)`.
   Restored by hand; re-ran clean; passes.

## 5. Full check tally

- KilnFW host tests (`kilnctl_host_tests_zones.exe`): all pass, including the two new max-width
  tests and the two new round-trip tests (`test_zones_diag_get_handler_round_trips_moved_
  fields()`, and `test_post_then_get_round_trips_new_fields()`'s updated assertions that
  `GET /api/zones` no longer emits any of the four moved keys).
- Full KilnFW host-test build (`build_host_tests.ps1`, 38/38 executables built): one unrelated
  pre-existing failure in `kilnctl_host_tests.exe` (`test_iter_tune.c`, report-only-axis voting
  behaviour) -- outside this task's owned files (`pid_fuzzy.c`, `firing_score.{c,h}`,
  `firing_compare.{c,h}` are explicitly another session's territory per this task's own
  concurrency notice), not touched by this change, and reproducible before any edit here.
- PC-side suite (`pytest tools/PcTools/tests`): **2308 passed, 12 skipped** (up from the prior
  2301 passed / 13 skipped baseline -- the 7-test increase is this task's new
  `GetZonesDiagTest`/`MergeZonesDiagTest` cases; the skip-count delta is unrelated pre-existing
  variance, not caused by this change).
- Target build (`build_kilnfw` via the kilnctrl MCP tool, from the live working tree): OK,
  76.7s.
- `tools/run_all_checks.ps1`: **92 of 94 checks passed.** The two failures
  (`check_00_kilnfw_target_build.ps1`, `check_wire_protocol_fingerprint.ps1`) are BOTH caused by
  an untracked file left in the working tree by a concurrent session --
  `firmware/CommonFW/src/kilnlink_get_stack_margin.c` (confirmed untracked via `git status
  --porcelain`, no commit history) referenced by `components/kilnlink/CMakeLists.txt`, which a
  clean-checkout build (`check_00`) cannot see, and a `KILNLINK_PROTOCOL_VERSION` 12->13 bump
  whose manifest hasn't been refreshed yet (`check_wire_protocol_fingerprint`). Neither check
  failure mentions `zones_http`, `zones_diag`, or any file this task touched -- confirmed by
  reading each check's full failure output. Not fixed here: outside this task's owned files, and
  the underlying work (a `kilnlink` stack-margin protocol addition) belongs to whichever session
  is mid-edit on it.

## 6. Headroom summary

| Endpoint | Cap | Worst case | Headroom |
|---|---:|---:|---:|
| `GET /api/zones` (before) | 7360 | 7199 | 161 |
| `GET /api/zones` (after) | 7360 | 6506 | **854** |
| `GET /api/zones_diag` (new) | 1024 | 740 | 284 |

## 7. Status update to the headroom plan

`docs/audits/zones_json_headroom_plan_2026-09-14.md` sec 6 ("Implemented savings: None") is now
superseded by this document for the 3a recommendation -- see that file's own updated closing
note.
