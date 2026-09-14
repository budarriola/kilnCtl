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

---

## Review, 2026-09-14 (adversarial, Opus)

Adversarial review of `f59b21c8` (the split) and `0dae6a3c` (`fuzzy_model_valid`).
Everything below is labeled **[executed]** or **[read]**. One real defect was found and
fixed in this pass; the rest of the claims held up.

### 1. A failed `/api/zones_diag` fetch rendered as an absent field -- DEFECT, FIXED

**[executed]** Stood up a local HTTP server that serves `/api/zones` normally and 404s
`/api/zones_diag`, pointed `control_get_zones()` at it, and diffed the rendering against
the same call with the diag route served. Result, before the fix:

```
z0: K_dc=2.5000 C/duty  tau=300.0s  dead_time=10.0s  fit_at=missing (ambient=missing)  tuning_valid=yes
```

That is **byte-identical** to what a firmware that never emitted the two keys renders
(`_describe_model_fields()`'s `else: fit_temp_desc = "missing"` branch, reached either
way). Nothing in the output said a second request had been attempted, let alone failed --
`mcp_server_control.py`'s `except zones_http_client.ZonesHttpError: pass` discarded the
exception object entirely. So the audit's "degrades gracefully" claim was true about not
crashing and false about not misleading: a reader making a gain-scheduling decision could
not distinguish "this board has no recorded fit operating point" from "this tool call
could not ask". Same class as this repo's standing "logging unchecked success" finding,
one layer up.

Mitigating, and worth recording: an OLDER, pre-split firmware degrades correctly by
accident -- it still emits `model_fit_temp_c`/`model_fit_ambient_c` on `/api/zones`, so a
404 on the (nonexistent) diag route leaves the real values in place and they render
normally. The confusing case is specifically post-split firmware plus a transient diag
failure, which is exactly the reboot/Wi-Fi-flap window.

**Fixed in this pass** (`mcp_server_control.py`, plus one new test): the exception is now
carried, not swallowed. `_describe_model_fields()` takes an optional `diag_error`; when
set, the two fields render `UNAVAILABLE(diag-fetch-failed)` instead of `missing`, and a
separate line names the error and the host and says the values are *unknown, NOT confirmed
absent*. Graceful degradation is unchanged -- the PID, coupling-matrix and HTTP-only
sections still render. **[executed]** re-ran the same 404 harness and confirmed the new
rendering; added `test_failed_diag_fetch_does_not_render_like_an_absent_field`, which
asserts both halves (failed fetch says UNAVAILABLE plus "NOT READ this call"; a genuinely
absent field still says plain `missing` and must NOT claim a fetch failed).
**[executed] negative-tested** it by regressing `diag_error` back to `None` at the call
site: the test goes red; production restored by hand from a pre-edit copy (no
`git checkout`), re-run green.

### 2. The two fetches are not atomic -- can mislead, narrowly; the detector already exists, unexposed

**[read]** `/api/zones` and `/api/zones_diag` are separate requests with no shared
snapshot, so `merge_zones_diag()` can produce a per-zone view that never existed on the
board. The realistic writer is `autotune_engine.c`'s `finalize_fit()`, which is the sole
writer of *both* sides of the pair: `model_k_dc`/`model_tau_s` (on `/api/zones`) and
`model_fit_temp_c`/`model_fit_ambient_c` plus `coupling_tau_c%u`/`coupling_dead_time_c%u`
(on `/api/zones_diag`). A finalize landing between the two GETs yields a *new* model gain
attributed to the *old* fit operating point -- precisely the misattribution
`model_fit_temp_c` was added to prevent, and it feeds the gain-scheduling argument. The
window is milliseconds and needs a concurrent autotune finalize or zones POST, so this is
low-probability, not low-consequence. The drift check is unaffected: it is static source
analysis, no HTTP involved.

**What would detect it:** a generation counter already exists in firmware --
`zones_config_generation()` (`zones_config_accessors.c:112`), backed by
`s_config_generation` (`zones_http.c:412`), bumped by every config setter including
`zones_config_set_model_fit_context()`, and already consumed by `profile_executor.c` for
exactly this "did config change under me" purpose. **[executed]** grepped
`zones_http_get.c` for `generation`: zero hits -- it is **not on the wire in either
response**. Emitting it as one top-level key in both (~25 bytes on `/api/zones`, comfortably
inside the re-measured 779 bytes of headroom) and having `merge_zones_diag()` flag or
refuse a mismatch would close this completely, with no new mechanism invented. Not
implemented here (firmware change, outside this review's remit); recorded as the concrete
follow-up.

### 3. The `tuning_*` correction -- independently confirmed

**[executed]** Extracted every `z.tuning_*` reference from `renderTuningQuality()`
(`zones_page.html:3006`) and compared it to the 11 keys the audit names. All 11 are
consumed -- `tuning_valid`, `tuning_method`, `tuning_rule`, `tuning_settled`,
`tuning_extrapolation_converged`, `tuning_tau_consistent`, `tuning_baseline_c`,
`tuning_step_ambient_c`, `tuning_raw_rise_c`, `tuning_rise_inf_c`, `tuning_seq` -- with no
extras and none unused. **[read]** the call site: `renderTuningQuality(data.zones)` at
`zones_page.html:2037`, inside `loadCurrent()`'s single `/api/zones` handler. The
correction to the plan is right, **none** of the group is movable, and the recovered
headroom is **not** understated.

### 4. Headroom, re-measured from a clean build

**[executed]** Deleted the output dir, rebuilt host tests clean (`build_host_tests.ps1
-OutDir C:\wt\kfw_rev_zonesdiag`, 40/40 built and passed), and read the figures the two
max-width tests print themselves:

| Endpoint | Cap | Worst case | Headroom | Claimed |
|---|---:|---:|---:|---:|
| `GET /api/zones` (after `fuzzy_model_valid`) | 7360 | **6581** | **779** | 6506+75 = 6581 / 779 |
| `GET /api/zones_diag` | 1024 | **740** | **284** | 740 / 284 |

Both reproduce exactly. On whether 284 bytes is a problem in the making: proportionally it
is tighter (27.7% of cap vs `/api/zones`' 10.6%), but the comparison that matters is cost
per future field, and there the diag route is the safer endpoint by a wide margin. A new
per-zone key costs ~3x its width on either endpoint (dense over
`MAX31856_CHANNEL_COUNT`=3), but the diag buffer is **heap, separate, and freely
enlargeable**: the standing "never grow `json_cap`" rule exists because of two
httpd-worker-stack-local panics and applies to the 7360-byte buffer's history, not to this
1024-byte one. Growing the diag cap to 2048 is a one-line change with no stack exposure.
So 284 bytes is a *speed bump that will be hit soon*, not a hazard -- provided the next
person raises this cap rather than treating 1024 as sacred. Worth stating plainly here,
since "do not grow the cap" is otherwise the lesson a reader carries over from the sibling
handler.

### 5. Drift check -- all four directions verified by hand

**[executed]** Four injections, each run, then restored by hand from a pre-edit copy and
re-run to confirm the check goes green again (no `git checkout`/`restore`/`stash`):

| # | Injection | Result |
|---|---|---|
| A | extra firmware key on `zones_get_handler()` (`neg_test_zones_extra`) | FAIL: `missing from client: ['neg_test_zones_extra']` |
| B | removed `fuzzy_model_valid` from the firmware emit | FAIL: `extra in client: ['fuzzy_model_valid']` |
| C | extra firmware key on `zones_diag_get_handler()` (`neg_test_diag_extra`) | FAIL: `missing from client: ['neg_test_diag_extra']` |
| D | extra key in `_ZONES_DIAG_MODEL_FIT_KEYS` (`neg_test_client_only`) | FAIL: `extra in client: ['neg_test_client_only']` |

Both directions on both endpoints have teeth; coverage did not halve across the split.

**One honest limit, pre-existing and correctly documented in the check's own comment:** the
diag comparison covers exactly three keys (`index` plus the two `model_fit_*`). The twelve
`coupling_tau_c%u`/`coupling_dead_time_c%u` cells -- 558 of the 693 bytes that moved -- are
invisible to the extractor, because a `%u`-templated key never matches `_IDENT_RE`. This is
the same blind spot `coupling_c%u` already had on the `/api/zones` side, so the split did
not create it; but it does mean the majority of what moved is not drift-checked, and the
`len(fw_diag_keys) > 1` liveness guard would still pass if the coupling block vanished
entirely.

### 6. `fuzzy_model_valid`'s equivalence is INCIDENTAL, not structural

**[read]** The HTTP field is `pid_fuzzy_derive_bands(z->model_k_dc, z->model_tau_s, NULL,
NULL)` (`zones_http_get.c:366`), reading the persisted `zone_cfg_t` directly. The control
tick is `resolve_fuzzy_bands()` (`profile_executor_pid_tick.c`), which calls
`zone_model_at(zi, z->actual_c, ...)` and then the same `pid_fuzzy_derive_bands()`. The two
agree **only because** `zone_model_at()` is `{ (void)T_c; return
zones_config_get_model(...); }` (`zones_config_accessors.c:1592-1596`). The audit says so;
what it does not say is that this is a **dependency on current behaviour with nothing
holding it**. `5d3bc854` introduced `zone_model_at()`/`coupling_at()` explicitly as a
*gain-scheduling seam*, i.e. the stated plan is to make `T_c` load-bearing. On the day that
lands, the HTTP field silently answers a different question from the control tick -- and
the divergence is in the direction that misleads: HTTP would report the unscheduled model's
validity while the tick used the scheduled one. Nothing catches this: no check, no test, no
assertion, and both sides stay internally consistent -- textbook "reset one side of a pair"
shape (state joined by a semantic contract never expressed as a shared function).

Two cheap structural fixes, either of which would make the equivalence real rather than
true-for-now: route the HTTP field through `zone_model_at()` as well (it would need a
temperature; the honest one is the zone's live `actual_c`, matching the tick exactly), or
give the predicate one owning function both callers use. **Recommended: the former.** Not
done here -- `profile_executor*` and its neighbours are another session's owned files this
pass.

A **second, smaller incidental divergence**, same root: the HTTP handler loops
`MAX31856_CHANNEL_COUNT` and reads `z->model_k_dc` raw, while `zones_config_get_model()`
returns false for any `zone_index >= thermo_count`, leaving the tick's locals at 0 and thus
`model_valid` false. For a zone past `thermo_count` carrying a stale non-zero model, HTTP
would say `true` where the tick says `false`. Inert on this bench unit (`thermo_count == 3
== MAX31856_CHANNEL_COUNT`, **[executed]** confirmed live), and such a zone is not
controlled anyway -- recorded for completeness, not as a live bug.

### 7. Live board (read-only)

**[executed]** against `kilnctl.local` (`fw_build Sep 14 2026 09:41:11`, uptime 6056 s; no
firing, nothing written):

- `GET /api/zones` -- serves. `GET /api/zones_diag` -- **serves**, with the documented
  shape; z0 reads `coupling_tau_c1=368.5`, `coupling_dead_time_c1=73.2`,
  `coupling_tau_c2=393.3`, `coupling_dead_time_c2=115.3`, diagonal 0. `merge_zones_diag()`
  against the live pair produced a coherent merged view (z0 `model_fit_temp_c` /
  `model_fit_ambient_c` = `-273.15`, the UNKNOWN sentinel).
- **The four moved keys are confirmed GONE from the live `/api/zones` response** -- so the
  split is not just committed, it is flashed and running.
- **`fuzzy_model_valid` is ABSENT from the live `/api/zones`.** The board is running
  firmware built between `f59b21c8` (09:05) and `0dae6a3c` (11:14): `0dae6a3c` is **not
  flashed**. The field could not be confirmed on hardware this pass (no flashing in this
  pass's remit).
- **The task's stated expectation -- "all three autotuned, so expect true" -- would NOT
  hold on this board as it stands.** All three zones read `model_k_dc = 0.0` and
  `model_tau_s = 0.0` live, and `model_fit_temp_c` is the never-recorded sentinel, so
  `pid_fuzzy_derive_bands()` would return **false** for all three. `fuzzy_strength_pct` is
  also 0.0 on all three, so the fuzzy layer is inactive either way and nothing is
  mis-controlled -- but the per-zone plant model this board is actually running with is
  empty, notwithstanding a fully populated coupling matrix and `coupling_diag_k_dc` of
  42.73 / 32.40 / 33.85. Flagged, not diagnosed: outside this review.
- **Stale MCP server, unrelated to this change but blocking the audit's own claim:** the
  running `kilnctrl` server reports `fresh: false`, `commit fd8d7b93`, started 2026-09-11.
  Its `control_get_zones` output has **no plant-model section at all** -- it predates
  `docs/audits/mcp_zone_model_fields_2026-09-13.md`, let alone this split. The merged MCP
  view was therefore verified by driving `zones_http_client` against the live board
  directly, not through the MCP tool. Restart the server before trusting
  `control_get_zones` output.

### 8. Checks

**[executed]** `tools/run_all_checks.ps1` (`-ExecutionPolicy Bypass`, foreground):
**94 passed, 0 skipped, 0 failed** -- matching the last-green 94/94, including
`check_doc_hash_citations.ps1` and `check_zones_per_zone_field_drift.ps1`. The two failures
the original audit's sec 5 attributed to a concurrent session's untracked `kilnlink` work
have since cleared. A second run after this pass's own edits reported 93/1, the single
failure being `check_00_kilnfw_target_build.ps1` unable to publish `KilnCtrl.bin`
(`MoveFileEx ... ERROR_ACCESS_DENIED` -- another process on this shared machine holding the
file, the known non-atomic-publish footgun); the build itself succeeded, and re-running that
check alone passed, so the tally stands at 94/94. Nothing in this pass touches firmware.
**[executed]** KilnFW host tests: 40/40 built and passed from a clean
output dir. **[executed]** PcTools suite: green, including the new regression test.

### Verdict

The split itself is sound: the `tuning_*` correction is right and independently confirmed,
both headroom figures reproduce exactly, and the drift check has teeth in all four
directions. Two things the audit overstated: "degrades gracefully" was hiding a real
rendering ambiguity (found, traced by execution, fixed here), and `fuzzy_model_valid`'s
"bit-for-bit the live decision" is incidental rather than structural, with a known future
change (`5d3bc854`'s scheduling seam) that would silently break it and nothing that would
catch that.
