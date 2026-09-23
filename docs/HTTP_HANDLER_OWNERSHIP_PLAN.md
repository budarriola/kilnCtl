# HTTP handler ownership migration plan (TODO.md Phase 5)

Pending work only. Premise re-verified at `origin/main` `6ea9294e`, not stale:
`firmware/KilnFW/TODO.md`'s "Phase 5: HTTP handler migration" line is still
unchecked, and this pass found real, live bypasses (below), not just an
unstruck TODO line.

## Scope

Phase 5 (`firmware/KilnFW/TODO.md` section 10.14) is "HTTP handler migration,
per-domain, alongside whichever owner (1/2/4) each handler calls into" --
i.e. `kiln_io_owner` (Phase 1), `thermo_owner` (Phase 2), `wifi_prov`'s own
task (Phase 4). `zones_config_accessors` and `safety_link` are **not** in
scope: `zones_config_accessors.h`'s own header explains it is a config-store
API, not a task-owned queue guarding a racing hardware writer, and
`safety_link_*` functions called from HTTP (`safety_link_get_status`,
`safety_link_send_clear_trip`, etc.) are already `SafetyLinkClass`'s own
public accessor API with no lower-level bypass available underneath them
(same conclusion the 2026-09-07 relay audit reached for relay writes:
"every relay-write call site... was already either owner-routed or one of
the two documented fail-safe exceptions"). `profile_executor`/
`relay_authority` gaining an owner-task queue is TODO.md's own "not urgent"
item -- out of scope here.

`docs/audits/relay_write_paths_2026-09-07.md` already swept every
**relay-write** path and found zero genuine offenders. It did not sweep
thermocouple or GPIO **reads**, or `kiln_io`/`MAX31856` calls outside
`dashboard_http.c`/`ota_http.c`/`diagnostics_http.c` specifically. This pass
covers that gap.

## Method

Grepped `firmware/KilnFW/App/drivers/http/*.c` for direct calls into
`MAX31856_*`/`kiln_io_*`/`SX1509_*` (the primitives one layer under
`thermo_owner`/`kiln_io_owner`) and cross-checked each hit against the
owner's public API (`thermo_owner.h`, `kiln_io_owner.h`) for an existing
equivalent accessor. `wifi_provision_http.c` was checked the same way
against `esp_wifi_*`/`wifi_prov_*` and found clean -- every call already
goes through `wifi_prov_*()`, confirming Phase 4's own claim that no caller
needed editing.

## Table

| Handler file:function | Direct call | Owning module | Accessor available |
|---|---|---|---|
| `drivers/http/dashboard_http.c:201` (`dashboard_get_status` readings block) | `MAX31856_read_all(s_dash.thermo_bus, ...)` | `thermo_owner` | YES -- `thermo_owner_command_read_all()` |
| `drivers/http/dashboard_http.c:618` (autotune/status refresh path) | `MAX31856_read_all(s_dash.thermo_bus, ...)` | `thermo_owner` | YES -- `thermo_owner_command_read_all()` |
| `drivers/http/ota_http.c:814` (pre-OTA snapshot) | `MAX31856_read_all(s_thermo_bus, ...)` | `thermo_owner` | YES -- `thermo_owner_command_read_all()` |
| `drivers/http/ota_http.c:821` (pre-OTA snapshot) | `kiln_io_read(s_io, &io_state)` | `kiln_io_owner` | YES -- `kiln_io_owner_command_read()` |
| `drivers/http/diagnostics_http.c:104`-area (per its own header comment: "calls `MAX31856_read_all()` directly, ONE call covering every channel") | `MAX31856_read_all(...)` | `thermo_owner` | YES -- `thermo_owner_command_read_all()` |

No genuine offenders found for `kiln_io` relay/GPIO **writes** (already
covered, still true) or for `wifi_prov` (clean). All five findings above are
**reads**, all have a ready-made 1:1 accessor already published by the
owner, and none require a new accessor to be designed.

Everything else grepped (`profile_executor.h`/`_internal.h`/`_state.h`
inclusion by `dashboard_exec_http.c`, `s_exec`/`s_at` mentions in
`profiles_live_http.c`/`zones_http.c`) turned out to be comment-only
references, not actual direct struct/lock access -- not counted as
offenders.

## Counts by owner

| Owner | Offending call sites | Files touched |
|---|---|---|
| `thermo_owner` | 4 | `dashboard_http.c` (x2), `ota_http.c`, `diagnostics_http.c` |
| `kiln_io_owner` | 1 | `ota_http.c` |
| `wifi_prov` | 0 | none |

Total: **5 call sites in 3 files**, one batch, no new accessor needed.

## Batch (one implementer task, 3 files)

**Batch A -- read-path bypasses (`thermo_owner` + `kiln_io_owner`), 3 files:**
`dashboard_http.c`, `ota_http.c`, `diagnostics_http.c`.

- Replace each `MAX31856_read_all(bus, readings, MAX31856_CHANNEL_COUNT, &count)`
  with `thermo_owner_command_read_all(readings, MAX31856_CHANNEL_COUNT, &count)`
  (signature-compatible per `thermo_owner.h:124`; drop the now-unused `bus`
  handle only if nothing else in the same file still needs it -- check before
  deleting the field).
- Replace `kiln_io_read(s_io, &io_state)` in `ota_http.c:821` with
  `kiln_io_owner_command_read(&io_state)` (`kiln_io_owner.h:220`).
- These are reads on the `esp_http_server` worker task, not writes -- no lock
  ordering or race is fixed here (per the Phase 2 TODO note, `thermo_owner`
  exists for architectural consistency, not because `MAX31856.c`'s own
  per-channel lock was unsafe). The value of closing this is uniformity: once
  closed, `check_relay_authority_paths.ps1`'s style of check (below) can be
  widened to a zero-tolerance grep with no exceptions left to allowlist.
- One batch because all three files change in the same mechanical way and
  the diff per file is 1-2 lines.

No accessor needs to be added first -- `thermo_owner_command_read_all()` and
`kiln_io_owner_command_read()` already exist and are already used elsewhere
(e.g. `dashboard_http.c`'s own header comment at line 144 already documents
routing a *different* read through `kiln_io_owner`, so the pattern is
established in the same file).

## Constraints

- Never enlarge an `esp_http_server` handler's on-stack JSON/response buffer
  to make this change fit (`httpd stack blob class`, `docs/audits/` -- big
  locals on the 8 KB httpd worker stack are a known hazard class).
- URI handler cap: `config.max_uri_handlers = 165`
  (`wifi_provision_http.c:1118`); this migration adds zero routes, so
  `check_uri_handler_cap.ps1` is unaffected. Any *future* Phase 5 batch that
  needs a new route must bump the cap in the same change.
- Host tests are not a target build: `firmware/KilnFW/App/drivers/http/*.c`
  links only into the real ESP-IDF target build (per
  `project_http_handlers_are_target_build_only.md` -- roughly 8 of 50 HTTP
  files ever link into host tests). Verification for this batch is
  `check_00_kilnfw_target_build.ps1` plus hardware behavior, not a host-test
  suite.
- Lock order `s_exec.lock` then `s_at.lock` (`profile_executor`/
  `autotune_engine`) is not touched by this batch -- no `profile_executor`
  handler is in scope here.
- No task in this batch is a PSRAM-stack task, so the "PSRAM-stack tasks
  must not write NVS" constraint does not apply to Batch A; carry it forward
  unchanged if a future batch ever does touch `profile_executor`/zones-config
  HTTP handlers running on such a task.

## Verification

- `check_00_kilnfw_target_build.ps1` -- must still build clean under
  `-Werror` after the substitution (same as every prior owner-migration
  phase; Phases 1/2/4 were all "build-verified only" per TODO.md's own
  verification note, and this batch is smaller and lower-risk than any of
  them).
- Manual/bench: `get_heap_status`/`dashboard_get_status` (`GET /api/status`)
  and `GET /api/partitions`'s pre-OTA snapshot should read identical
  temperature/IO values before and after the swap on a live board -- these
  are read-only substitutions, so a negative test (temporarily reintroduce
  one direct `MAX31856_read_all()` call, confirm the mechanical check below
  catches it, then restore and rebuild per the negative-test discipline in
  `CLAUDE.md`) is the right shape, not a behavior diff.
- A mechanical `check_no_handler_direct_driver_calls.ps1` is **feasible** for
  the narrow case actually found here, following
  `check_relay_authority_paths.ps1`'s existing shape (strip comments the same
  way that script already does, to avoid the line-number-drift bug it fixed
  2026-09-07; then grep): flag any
  `\bMAX31856_(read_all|read|start_all|configure)\s*\(` or
  `\bkiln_io_(read|set_relay|set_relay_mask|set_io|all_relays_off)\s*\(`
  call inside `firmware/KilnFW/App/drivers/http/*.c`, with a zero-entry
  allowlist once Batch A lands (no legitimate-bypass exception exists for
  HTTP handlers the way `main.c`'s panic path and `profile_executor.c`'s
  watchdog are legitimate for relay-off writes). Register it the same way
  `check_relay_authority_paths.ps1` is discovered, via `run_all_checks.ps1`'s
  `check_*.ps1` glob -- no separate wiring needed.
- Do not widen this check to also flag `zones_config_*` or `safety_link_*`
  calls from HTTP handlers -- both are already the sanctioned owning-module
  API for their domain (see Scope above), not a bypass.

## Non-findings, for the record

- `zones_http.c`/`zones_http_get.c`/`zones_http_post.c`/
  `zones_http_post_parse.c` all reference `MAX31856_CHANNEL_COUNT` (a
  constant) and `MAX31856_config_default()` (a pure struct initializer, no
  bus I/O) heavily, but make no live `MAX31856_read_all()`/`MAX31856_start_all()`
  hardware call themselves -- not offenders.
- `wifi_provision_http.c` makes zero direct `esp_wifi_*` calls; every status
  field and mutation goes through `wifi_prov_get_*()`/`wifi_prov_set_*()`/
  `wifi_prov_scan()`/`wifi_prov_forget_network()`. Phase 4 is fully closed on
  the HTTP side.
- `dashboard_exec_http.c` mentions `profile_executor_internal.h` only in a
  comment (line 224), does not include it, and makes no direct `s_exec`/
  `s_at` access -- not an offender. `profiles_live_http.c`'s `s_exec`
  reference at line 6 is likewise comment-only.
