# Zones field sourcing and generation, 2026-09-14

Fixes the two defects the opus adversarial review appended to
`docs/audits/zones_diag_endpoint_split_2026-09-14.md` (its sections 2 and 6, on top
of the `f59b21c8` GET-endpoint split and `0dae6a3c`'s `fuzzy_model_valid`).
Scope: `firmware/KilnFW/App/drivers/http/zones_http_get.c`,
`firmware/KilnFW/App/drivers/persist/zones_config_accessors.{c,h}` (read-only, no change
needed there), their host tests, `tools/PcTools/src/kilnctrl/zones_http_client.py`, and
`tools/PcTools/selfcheck_zones_fields.py`'s drift check.

## Defect 1 -- `fuzzy_model_valid` was incidental, not structural

`0dae6a3c` (`5d3bc854`, `zones_http_get.c`) computed `fuzzy_model_valid` by reading
`z->model_k_dc`/`z->model_tau_s` directly and calling `pid_fuzzy_derive_bands()` itself.
That agreed with `profile_executor_pid_tick.c`'s `resolve_fuzzy_bands()` (the real control
tick) only because `zone_model_at()` -- the seam `5d3bc854` added specifically so a future
gain-scheduling design can make `T_c` load-bearing -- was, and still is, `{ (void)T_c;
return zones_config_get_model(...); }`. The equivalence held today and would have silently
broken the day `T_c` became load-bearing, with the HTTP field then answering the
unscheduled question while the tick answered the scheduled one -- this repo's "reset one
side of a pair" class (a semantic contract between two pieces of state, expressed nowhere
as one function).

**Fix**: `zones_get_handler()` now calls `zone_model_at()` itself -- the SAME function
`resolve_fuzzy_bands()` calls -- instead of re-deriving the predicate from a direct
`model_k_dc`/`model_tau_s` read. `NAN` is passed for `T_c`: this call site has no live
`actual_c` available (that lives in `profile_executor`'s `zone_runtime_t`, another area's
owned code, and is only meaningful while a run is active -- outside a run there is no live
tick for this field to match anyway). `zone_model_at()` ignores `T_c` today so `NAN` is
inert now; it is deliberately not `0.0` or a fit temperature so that `grep 'zone_model_at(i,
NAN'` finds this exact call site the day `T_c` stops being ignored, at which point it needs
a real live temperature threaded in, not a placeholder.

A useful side effect closes the review's smaller, second finding in the same section: the
HTTP loop runs `MAX31856_CHANNEL_COUNT` times but `zone_model_at()` -> `zones_config_get_model()`
refuses any `zone_index >= thermo_count`. A zone past `thermo_count` carrying a stale
non-zero model now reads `fuzzy_model_valid:false`, exactly as the tick would, instead of a
direct read's `true`. Inert on the bench board (`thermo_count == 3 == MAX31856_CHANNEL_COUNT`),
but no longer a live discrepancy waiting for `thermo_count` to be lowered.

**Negative-tested**: reverted the call site to the pre-fix direct read
(`pid_fuzzy_derive_bands(z->model_k_dc, z->model_tau_s, NULL, NULL)`), rebuilt from a clean
`build/` dir -- the new
`test_get_fuzzy_model_valid_single_sourced_past_thermo_count()` test failed (zone 2, past
`thermo_count`, read `true` instead of the expected `false`). Restored by hand, deleted
`build/`, rebuilt clean: 40/40 executables, 1875/1875 checks in
`kilnctl_host_tests_zones.exe`.

## Defect 2 -- the two-fetch pair was not atomic, and the detector already existed unused

`/api/zones` and `/api/zones_diag` are separate HTTP requests with no shared snapshot.
`autotune_engine.c`'s `finalize_fit()` writes both sides of the pair --
`model_k_dc`/`model_tau_s` on `/api/zones`, `model_fit_temp_c`/`model_fit_ambient_c` plus
`coupling_tau_c%u`/`coupling_dead_time_c%u` on `/api/zones_diag` -- so a finalize landing
between the two GETs can pair a NEW gain with an OLD fit operating point, exactly the
misattribution `model_fit_temp_c` exists to prevent.

`zones_config_generation()` (`zones_config_accessors.c:112`, backed by
`s_config_generation`, bumped by every config setter and already consumed by
`profile_executor.c` for this exact "did config change under me" purpose) was on neither
response.

**Fix**:
- `zones_get_handler()` now emits a top-level `"generation":<n>` on `/api/zones`
  (`zones_http_get.c`).
- `zones_diag_get_handler()` now emits the same top-level `"generation":<n>` on
  `/api/zones_diag`.
- `zones_http_client.py`: `"generation"` added to `_TOP_READONLY_OR_STRUCTURAL_KEYS` (read-only
  telemetry, no POST field). `merge_zones_diag()` now compares the two responses'
  `generation` values and raises a new `ZonesHttpGenerationMismatchError` (a
  `ZonesHttpError` subclass) if they disagree, instead of silently returning an impossible
  merged view. A missing `generation` on either side (older, pre-2026-09-14 firmware) is
  NOT treated as a mismatch -- nothing to compare, and refusing every older board would
  regress every existing caller for no safety gain.
- `mcp_server_control.py`'s `_describe_model_fields()` call site needed **no change**:
  `ZonesHttpGenerationMismatchError` IS a `ZonesHttpError`, so the existing
  `except zones_http_client.ZonesHttpError as exc: diag_error = ...` (added by the prior
  review pass for defect-1-of-the-earlier-review) already catches it, and a generation
  mismatch now degrades exactly like any other diag-fetch failure -- the model-fit fields
  render `UNAVAILABLE(...)` with the reason named, rather than a fabricated merged view.

**Headroom, re-measured from a clean build** (`build_host_tests.ps1`, `build/` deleted
first, 40/40 built and passed; figures read from the max-width tests' own printed
headroom, `test_zones_get_handler_max_width_response_fits_json_cap()` /
`test_zones_diag_get_handler_max_width_response_fits_json_cap()` in `test_zones_http.c`):

| Endpoint | Cap | Worst case (before `generation`) | Worst case (after) | Headroom (after) |
|---|---:|---:|---:|---:|
| `GET /api/zones` | 7360 | 6581 (779 headroom) | **6597** | **763** |
| `GET /api/zones_diag` | 1024 | 740 (284 headroom) | **756** | **268** |

`"generation":4294967295,"` (a `uint32_t` at its own max width) costs 16 bytes on each
endpoint, matching the observed 779->763 and 284->268 deltas. `/api/zones`' `json_cap` was
**not** enlarged (its two-panics history stands); `/api/zones_diag`'s 1024-byte cap is
heap, not stack, and was not touched either -- 268 bytes remains ample for a `uint32_t`
scalar, and the review's own note that this cap is freely enlargeable (unlike its sibling)
still applies to any future addition.

**Negative-tested**: removed the `"generation"` APPEND fragment from
`zones_diag_get_handler()`, clean-rebuilt -- the new
`test_get_and_diag_emit_matching_generation()` test failed (2 checks: the diag response no
longer carried the expected `"generation":<n>,` substring). Restored by hand, deleted
`build/`, rebuilt clean: 40/40 executables, 1875/1875 checks pass. Separately negative-tested
the client-side mismatch detector by disabling the comparison in `merge_zones_diag()`
(`if False and ...`) -- `test_generation_mismatch_is_refused_not_silently_merged` failed
(`ZonesHttpGenerationMismatchError` not raised). Restored by hand; `pytest
tools/PcTools/tests/test_zones_http_client.py -q` -- 71 passed.

## Coupling-cell drift hole -- closed

The review noted the 12 `coupling_tau_c%u`/`coupling_dead_time_c%u` cells (558 of the 693
bytes the `zones_diag` split moved) matched no identifier regex in
`selfcheck_zones_fields.py`'s extractor (`_IDENT_RE` requires a clean identifier, no `%`)
and were checked on neither endpoint -- the same blind spot `coupling_c%u` already had on
the `/api/zones` side.

**Fix**: `selfcheck_zones_fields.py` gained a narrow `_DYNAMIC_KEY_RE` (`^([A-Za-z_]\w*)%u$`)
that recovers `<ident>%u`-shaped template tokens into a separate `dynamic_keys` set
alongside the existing `keys` set, plumbed through `_scan_template_keys()` and
`_extract_get_per_zone_keys()` (now returns `(keys, dynamic_keys)`). Four new checks in
`zones_per_zone_field_table_checks()` assert:
1. `/api/zones`'s dynamic keys are exactly `{"coupling_c%u"}`.
2. the client's `_ZONE_COUPLING_CELL_RE` matches a concrete instance of each.
3. `/api/zones_diag`'s dynamic keys are exactly `{"coupling_tau_c%u", "coupling_dead_time_c%u"}`.
4. the client's `_ZONE_COUPLING_TAU_DEAD_TIME_CELL_RE` matches a concrete instance of each.

Deliberately narrow (one placeholder shape, checked by name against the client's own
matching regexes rather than a second hand-typed list) rather than a general dynamic-key
parser, consistent with this repo's standing rejection of over-general mechanical checks
for this class of bug (see `CLAUDE.md`'s "reset one side of a pair" section).

**Negative-tested**: added a bogus expected dynamic key
(`"coupling_tau_c%u", "coupling_dead_time_c%u", "neg_test_bogus_cell%u"`) to the diag side's
expectation set -- `python zones_per_zone_field_drift_check.py` failed
(`missing: ['neg_test_bogus_cell%u']`). Restored by hand; re-ran clean, all checks pass.

## What was NOT changed

- `mcp_server_control.py` -- outside this pass's owned-file list, and (as noted above) the
  existing `except ZonesHttpError` already covers the new
  `ZonesHttpGenerationMismatchError` correctly with no code change.
- `profile_executor*`/`adaptive_tune*`/`pid_fuzzy*`/`sim_*` -- another session's owned
  files; not touched. The single-sourcing fix reads `zone_model_at()` (owned by this pass's
  `zones_config_accessors.c`), not anything in those files.
- `/api/zones`' 7360-byte `json_cap` -- untouched, per its own two-panics history.

## Verification

- KilnFW host tests (`build_host_tests.ps1`, clean `build/`): **40/40 executables built and
  passed**, including `kilnctl_host_tests_zones.exe` at **1875/1875 checks**.
- `kilnctl_sim_iter_tune.exe`'s A1 bar: **PASS**, pinned ceiling `24/660` (3.6364%),
  unchanged verdict from a clean rebuild -- this pass did not touch
  `sim_*`/`adaptive_tune*`.
- `python tools/PcTools/zones_per_zone_field_drift_check.py`: clean, all checks pass
  (including the four new dynamic-key checks).
- `pytest tools/PcTools/tests/test_zones_http_client.py
  tools/PcTools/tests/test_mcp_server_control_model_fields.py -q`: **78 passed**.
- `pytest tools/PcTools/tests -q` (full PC-side suite): see the check-tally note in the
  final report -- run separately due to the suite's own runtime.
- Target build: `idf.py build` (KilnFW, via `export.ps1` + `idf.py build`, ESP-IDF 6.0.2)
  -- **Project build complete**, `KilnCtrl.bin` 0x2282f0 bytes, 28% partition free.
- `tools/run_all_checks.ps1` (`-ExecutionPolicy Bypass`, foreground): see the final report.

## MCP server note

The running `kilnctrl` MCP server was last observed stale (`fresh:false`, commit
`fd8d7b93`) by the prior review pass. This pass changed PC-side source
(`zones_http_client.py`, `selfcheck_zones_fields.py`) but verified behavior by driving
`zones_http_client`/`selfcheck_zones_fields` directly (pytest, the standalone drift-check
script) rather than through the MCP tool layer, per `profiles_get_exec_status`-first
coordination guidance -- no board interaction, read-only or otherwise, was performed in
this pass, and the MCP server was not restarted.
