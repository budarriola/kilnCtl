# Host test coverage gaps, KilnFW round 3 (2026-10-10)

Follows `HOST_TEST_COVERAGE_GAPS_ROUND2_2026-10-10.md`. Method: concatenated
every `test/*.c` and grepped for each `drivers/**/*.c` basename and its public
symbols; name-only matches were checked (many safety_cfg_*, safety_link_* files
are exercised through other tests and were excluded). Picked the three
data-relevant files with no host test driving their real logic. No coverage
instrumentation was run. All three `#include` the real `.c`.

| # | Module | Role | Test | Checks |
|---|--------|------|------|--------|
| R3-A | `drivers/http/cfg_fs_format_http.c` | `POST /api/cfgfs/format_confirm` is the data-loss gate for the cfg LittleFS partition (sole up-to-date config copy); `GET format_pending` | `test_cfg_fs_format_http.c` | 49 |
| R3-B | `drivers/http/setup_progress_http.c` | `POST/GET /api/setup/progress` commissioning-progress persistence (config write path) | `test_setup_progress_http.c` | 58 |
| R3-C | `drivers/http/dashboard_settings_http.c` | `POST /api/unit_pref` (persisted pref) and `POST /api/safety/log_level` (wire send to the Pico vs local relay knob) | `test_dashboard_settings_http.c` | 81 |

`$totalExpected` 95 -> 98 in `test/build_host_tests.ps1`. `stubs/esp_http_server.h`
gained `char uri[256]` in `httpd_req_t` (matches the real struct) so the
`?force_healthy=` parser can be driven.

## What is pinned

- R3-A: system mode gate (firing or autotune) refuses 409 before cfg_fs is
  consulted; recovery mode refuses 409 even with `force_healthy=1`; a mounted
  cfg refuses 409 unless `force_healthy` is exactly `1` (`=0`, `=11`, empty,
  bare key and `xforce_healthy=1` all refuse); unavailable cfg formats exactly
  once; a format failure answers 500 and never claims success; pending GET
  JSON, stale reason suppressed when not pending, overlong reason is an error;
  route registration (GET pending, POST confirm) and failure propagation.
- R3-B: unmounted cfg 503 before the store is touched; empty/oversize/recv
  failure body 400; step range 0..12 (`12`, `-1`, `1x` refused), state
  validation, note rejection; exact args passed to the store (absent note ->
  NULL); store failure 500 (503 if cfg dropped mid-request), never ok; GET
  JSON shape, escaping, worst-case document, allocation failure 500.
- R3-C: unit_pref unmounted 503 before body read, length bounds, only
  `C`/`F` exact and `celsius`/`fahrenheit` case-insensitive accepted, save
  failure 500 with `adopted` reported and never ok; log_level unwired link
  500, level 0..4 only, default peer goes to the wire, `peer=relay` is local
  only (no wire send), empty/overlong/unknown peer fails closed (400, nothing
  sent anywhere), wire send failure answers `ok:false`.

## Negtests (`tools/negtest.ps1 -Preset kilnfw-host`, `-ExpectPattern`)

| Test | Mutation | Result |
|------|----------|--------|
| R3-A | `cfg_fs_format_http.c`: recovery refusal branch `&& 0` | CAUGHT |
| R3-B | `setup_progress_http.c`: `step_num >= COUNT` -> `>` | CAUGHT |
| R3-C | `dashboard_settings_http.c`: `peer_len != -1` -> `peer_len > 0` | CAUGHT |

## Defects / observations

No firmware defect found. Observation (not a defect): `safety_log_level_post_handler`
uses `strtol`, so a leading space (`level= 2`) is accepted as level 2; harmless,
not pinned either way.
