# drivers/ reorg dry run (2026-09-05, revised)

Output of `tools/drivers_reorg/plan_moves.ps1 -DryRun` against the
coordinator-revised `mapping.csv` (decisions A-E below). Nothing was applied;
this whole pass is preparation only.

## 0. Coordinator decisions applied this pass

- **A -- layout deviation from the original plan.** Target directories are
  subdirectories of the existing `drivers` ESP-IDF component --
  `firmware/KilnFW/App/drivers/<layer>/` -- NOT sibling `App/<layer>/`
  components as earlier drafts of the plan implied. One component remains;
  `CMakeLists.txt`, `Kconfig`, `README.md` and `gen_build_info.cmake` all
  **STAY** at `firmware/KilnFW/App/drivers/` (mapping.csv rows have
  `old_path == new_path`, rationale prefixed `STAY:`). Reason: this needs
  zero `REQUIRES`/component-boundary churn, `Kconfig` options already span
  net/ui/safety and have no natural single new home, and `README.md`
  describes the whole former tree rather than one layer. `INCLUDE_DIRS`
  becomes the eleven subdirs (`hw, owners, control, safety, persist, net,
  http, ui, bridge, sim, common`) instead of ten sibling directories -- see
  section 3.
- **B -- new bottom-most tier `common`**, below `hw`, for pure leaf
  headers/utilities: `uart_task_ids.h`, `settings.h`, `stack_margin.{c,h}`,
  `stack_margin_calc.h`, `http_form.h`, `web_encoding.{c,h}`,
  `httpd_socket_budget.h`, `dram_margin.h`, `bx_worker_reentrancy.h`. Each
  was checked by hand before placement: every one includes only ESP-IDF/libc
  headers (or each other) and nothing from `control/safety/persist/net/
  owners/hw`, so **all nine passed the leaf check** -- none were left in
  place. `stack_margin.c` was included in the tier move alongside its header
  since it has the same include profile.
- **C -- every `*_http.c/.h` / `*_http_*.c` file goes to `http`** regardless
  of subject matter: `ota_http*`, `wifi_provision_http*`, `safety_cfg_http*`,
  `adaptive_tune_http*`, plus `backup_export.c`/`backup_import.c` (paired
  with `backup_http.c`, so grouped with it rather than left in `persist`).
  16 files moved layer under this rule (see mapping.csv rows whose rationale
  starts "Decision C"). Exception: `zones_http_internal.h` -- read and
  confirmed it is the shared internals of the `zones_http.c` split, consumed
  by `zones_config_store.c`/`zones_config_accessors.c` (persist) as well as
  the `zones_current_sweep_*`/`zones_http_*` files -- placed in `persist`
  per the exception clause, since a `zones_config_*.c` file is a consumer.
- **D -- `sim` moves to the top tier** in the verifier (with `ui/http/
  bridge`): the sim backend drives the system from above. This is a genuine
  tier reclassification, not just a mapping.csv edit -- it turns four
  existing `control -> sim_backend.h` includes into new upward-include
  findings (see section 5).
- **E -- all remaining `AMBIGUOUS:`-prefixed rows accepted** as the agent
  originally proposed: `heat_enable`/`heat_interlock` -> control,
  `kiln_io`/`kiln_io_owner`/`relay_authority` -> owners, `relay_cycles` ->
  persist, `thermo_combine` -> control,
  `tuning_recommendations_fallback.json` -> http, `wifi_status_ui` -> ui,
  `zone_settings_source_chain.h` -> persist. The `AMBIGUOUS:` prefix is
  stripped in mapping.csv; each row keeps a short "Coordinator-accepted
  placement: ..." rationale.

## 1. git mv plan

354 files map into the eleven layer subdirs (`mapping.csv`, 358 rows minus
the 4 STAY rows from decision A). No basename collisions across the eleven
target dirs. Full list of `git mv` commands: run the script, or read
`mapping.csv` directly (one row per file).

## 2. CMakeLists literal SRCS rewrite

- `firmware/KilnFW/App/drivers/CMakeLists.txt`: **201** literal quoted-filename
  SRCS occurrences found (unchanged from the prior pass -- decision A keeps
  this file in place, so the count doesn't shift).
- `firmware/KilnFW/App/CMakeLists.txt`: **0** literal `drivers/<file>` paths
  (`REQUIRES drivers` only).
- **Consequence for -Apply, now resolved by decision A**: since the single
  `drivers` component is retained, the fix is a straight in-place rewrite of
  each SRCS literal from `"foo.c"` to `"<layer>/foo.c"` inside the
  unchanged `drivers/CMakeLists.txt` -- no new per-layer `CMakeLists.txt`
  fragments, and no `App/CMakeLists.txt` restructuring. `plan_moves.ps1
  -Apply` already does this rewrite (section 2 of the script).

## 3. Bare-include feasibility

**Preferred: keep bare `#include "x.h"`.** List all **eleven** new
subdirectories in the `drivers` component's own `INCLUDE_DIRS`
(`idf_component_register` in `firmware/KilnFW/App/drivers/CMakeLists.txt`,
which stays put per decision A) -- the same mechanism that makes `drivers/`
itself a single include-path entry today, just with the entry list grown
from 1 to 11 and nested one level deeper. `mapping.csv`'s 358 target paths
have **zero basename collisions** across the eleven directories, so an
unqualified include can never resolve ambiguously post-move. No `#include`
line needs to become path-qualified.

## 4. Path-keyed check/test sites

Scanned `tools/**/*.ps1`, `tools/**/*.py`, and
`firmware/KilnFW/App/test/**/*.ps1|*.py` (excluding `.venv`/`node_modules`/
`site-packages`/`.git`) for the literal `drivers/` or any of the 358 mapped
filenames: **1423 matching lines across 341 files**. As before, the large
majority are documentation/docstring path citations in
`tools/PcTools/src/kilnctrl/*.py` -- stale after the move but **not a
runtime break**. Below are the two truly actionable subsets, refreshed for
the decision-A subdirectory layout (a rewrite now becomes
`.../App/drivers/<layer>/<file>`, not `.../App/<layer>/<file>`).

### 4a. Runtime code that resolves a `drivers/` filesystem path (WILL break)

```
firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py:151:  uart_ids = root / "firmware/KilnFW/App/drivers/uart_task_ids.h"
firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py:153:  max_payload_hdr = root / "firmware/KilnFW/App/drivers/espInterfaces/uart_protocol.h"
```
Rewrite to `firmware/KilnFW/App/drivers/common/uart_task_ids.h` (decision B
moved it to `common`) and `firmware/KilnFW/App/drivers/owners/uart_protocol.h`
(the `espInterfaces/` subfolder is flattened into `owners/` by the move, so
the `espInterfaces/` path segment disappears entirely, not just `drivers/`).

### 4b. check_*.ps1 / check_*.py / selfcheck*.py scripts hardcoding `App/drivers/...`

Same 18 scripts as the prior pass -- decisions A-E only change *where*
inside `drivers/` each literal now points, not which scripts need editing.
Each needs its literal path(s) rewritten to `App/drivers/<layer>/...` in the
same commit as the move, and re-run afterward to confirm it can still go
red:

```
firmware/KilnFW/App/test/check_flash_worker_lint.ps1                 -> App/drivers/bridge/... (uart_bridge_ext.c family)
firmware/KilnFW/App/test/check_thermal_guard_input_producers.ps1     -> App/drivers/control/thermal_guard.c
tools/PcTools/selfcheck.py                                            -> spans several new layer subdirs
tools/PcTools/selfcheck_zones_fields.py                               -> App/drivers/persist/zones_config_*.*
tools/check_bridge_reject_reason.ps1                                  -> App/drivers/bridge/uart_bridge*.c
tools/check_c_files_in_cmakelists.ps1                                 -> reads App/drivers/CMakeLists.txt (STAYS put, decision A) against files now under App/drivers/<layer>/
tools/check_doc_citations.ps1                                         -> spans several new layer subdirs
tools/check_duplicate_symbols.ps1                                     -> spans several new layer subdirs
tools/check_hal_include_boundary.ps1                                  -> App/drivers/http/{dashboard_http.c,ota_http.c,ota_http_esp.c,ota_http_pico.c,ota_http_recovery.c,partition_info_http.c}, App/drivers/ui/ui_page_diagnostics.c, App/drivers/net/wifi_prov*.c/.h (note: ota_http*.c moved net->http under decision C, wifi_prov*.c/.h stayed net -- only wifi_provision_http*.c/.h moved to http)
tools/check_heat_enable_wiring.ps1                                    -> App/drivers/control/heat_enable.c, App/drivers/safety/danger_mode.c, App/drivers/bridge/uart_bridge.c
tools/check_host_embed_symbols_defined.ps1                            -> parses "#include \"../drivers/X.c\"" convention -- becomes "../drivers/<layer>/X.c"
tools/check_no_duplicate_crc.ps1                                      -> also has a firmware/UnitTestFw/... drivers/ literal -- UnitTestFw is explicitly OUT of scope, do not touch that one
tools/check_relay_writes_through_owner.ps1                            -> RelPath = ".../drivers/owners/kiln_io.c", ".../drivers/control/profile_executor.c"
tools/check_safety_baud_sync.ps1                                      -> $kconfigPath = '.../App/drivers/Kconfig' (unchanged -- decision A keeps Kconfig at the component root)
tools/check_safety_call_results_checked.ps1                           -> RelPath = ".../drivers/control/profile_executor.c"
tools/check_stack_margin_baseline.py                                  -> App/drivers/common/stack_margin.* (decision B moved it out of safety/)
tools/check_uart_version_independence.ps1                             -> App/drivers/bridge/... / App/drivers/common/uart_task_ids.h
tools/check_uri_handler_cap.ps1                                       -> spans App/drivers/http/*
```

### 4c. Host-test "mirror" scripts under App/test that #include a drivers/*.c file directly

Same 12 scripts as the prior pass; each `#include "../drivers/X.c"` or path
literal becomes `"../drivers/<layer>/X.c"`:

```
firmware/KilnFW/App/test/approach_rate_cap_mirror_drift_check.py      -> App/drivers/control/... (profile_executor family)
firmware/KilnFW/App/test/attribute_str_pool.py                        -> check target layer at edit time
firmware/KilnFW/App/test/flash_worker_lint.py                         -> App/drivers/bridge/uart_bridge_ext*.c
firmware/KilnFW/App/test/frame_a_offset_drift_check.py                -> App/drivers/safety/safety_link_frame*.c
firmware/KilnFW/App/test/fuzzy_gain_mirror_drift_check.py             -> App/drivers/control/pid_fuzzy.c
firmware/KilnFW/App/test/heater_output_pwm_drift_check.py             -> App/drivers/control/heater_output.c
firmware/KilnFW/App/test/pid_fuzzy_drift_check.py                     -> App/drivers/control/pid_fuzzy.c
firmware/KilnFW/App/test/power_diag_flag_mirror_drift_check.py        -> check target layer at edit time
firmware/KilnFW/App/test/ramp_lock_decision_mirror_drift_check.py     -> App/drivers/control/profile_executor*.c
firmware/KilnFW/App/test/ramp_stepping_gate_mirror_drift_check.py     -> App/drivers/control/profile_executor*.c
firmware/KilnFW/App/test/source_path_drift_check.py                   -> spans several new layer subdirs
firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py           -> App/drivers/common/uart_task_ids.h, App/drivers/owners/uart_protocol.h
```

`firmware/KilnFW/App/test/build_host_tests.ps1` and
`firmware/KilnFW/App/test/stubs/*` still contain **no** hardcoded `drivers/`
literal.

## 5. Include-direction verifier

Tiers (decision A keeps the same grouping, decision D moves `sim` up):
`ui/http/bridge/sim` (top) -> `control/safety/persist/net` (mid) ->
`owners/hw` (lower) -> `common` (bottom-most, decision B).

**UPDATE (2026-09-05, coordinator pass on Fix 4/5/6):** applied. Decision D
was reverted -- `sim` moved back to the bottom tier (`owners/hw/sim`) in
`plan_moves.ps1`'s `$tierOf`, since it is a hardware substitute, not a
top-tier orchestrator; this resolved the 4 Fix-4 `control -> sim_backend.h`
findings for free (`autotune_engine_internal.h`, `profile_executor.c`,
`profile_executor_relay_io.c`, `profile_executor_run.c` all sit at the same
tier as `sim` now). Fix 5's five owner-interlock pairs are now a named
allowlist in the verifier (`$allowedUpwardIncludes` in `plan_moves.ps1`,
keyed `"<includer basename>|<included basename>"`) rather than silently
tolerated -- any *other* upward include from those same files still reports.
Fix 6 shipped: a new `firmware/KilnFW/App/drivers/flash_worker.h` declares
only `uart_bridge_ext_run_on_flash_worker()` (with the re-entrancy hazard
documented on it directly); `log_store_mount.c` now includes it instead of
`uart_bridge.h`, and `uart_bridge.h` itself `#include`s it back so the
declaration has one source. Mapped to `common` in `mapping.csv` (it includes
only `esp_err.h`, nothing above `common`). Grepped the rest of `drivers/`
for other `uart_bridge.h` includes that exist solely for this call: none --
every other hit is either the bridge family itself (`uart_bridge*.c`,
`uart_bridge_ext*.c`, which need the full API) or a comment/prose match
(`safety_cfg_store.c`, `autotune_engine_internal.h` already hand-declare the
function and don't include the header at all -- the same false-positive
shape the anchored regex above already accounts for).

Sim reclassification surfaced two genuine (not false-positive) upward
includes directly on `sim_backend.c` itself, now tier 2 (`sim`) same as
`owners/hw`: `sim_backend.c:16 #include "wifi_provision_http.h"` [http] and
`sim_backend.c:17 #include "zones_config_accessors.h"` [persist]. Checked
what it actually calls from each -- one accessor apiece:
`wifi_provision_http_get_server()` (line 332) and
`zones_config_get_thermo_count()` (line 45). Both are narrow; per the
group-4 instruction this is a **propose, don't implement** finding:
- `wifi_provision_http.h`: already covered by Fix 3 below (a narrow
  `wifi_provision_state.h` exposing `wifi_provision_http_get_server()` would
  serve `sim_backend.c` too, alongside `factory_reset.c` and `wifi_prov.c`
  -- no separate header needed, just add `sim_backend.c` to that fix's
  consumer list once it lands). Confirmed via `git log -1 --format='%H %ci
  %s' -- firmware/KilnFW/App/drivers/wifi_provision_http.h` that no other
  session has narrowed it since the 2026-08-16 tree reorg -- it is still
  the full httpd-handler header, so this is real, not stale.
- `zones_config_accessors.h`: propose pulling
  `zones_config_get_thermo_count()` (and any other single-purpose read-only
  accessors `sim`/`hw`-tier files need) into a narrow
  `zones_config_query.h` (home: `persist`, alongside
  `zones_config_accessors.h`), with `zones_config_accessors.h` including it
  back for its own use -- same split pattern as Fix 1/2/3. Not implemented
  this pass; `sim_backend.c` is explicitly read-only for this task.

**22 upward includes remain (now 5 after the above -- confirmed via a
`plan_moves.ps1 -DryRun` re-run 2026-09-05)** (down from 73 in the prior pass -- decisions B
and C's reclassifications resolved the other 51 without any code change,
since they were all narrow shared-header cases exactly as flagged before).
One entry from the prior pass, `safety_cfg_store.c [safety]:23 includes
"uart_bridge.h" [bridge]`, and one new candidate, `backlight_pwm.h [hw]
includes "screen_idle.h" [ui]`, turned out to be **false positives** in the
verifier itself: both are comments describing what the file *deliberately
does not* include (`safety_cfg_store.c` declares the one function it needs
by hand instead of pulling in all of `uart_bridge.h`; `backlight_pwm.h`
explicitly documents "Deliberately NOT #include ... here"). The verifier's
regex matched the string `#include "x.h"` inside prose. Fixed in
`plan_moves.ps1` by anchoring the match to `^\s*#include\s*"..."` (an actual
directive, optional leading whitespace only) instead of matching anywhere in
the line -- confirmed neither file has a real matching `#include` line.

Residuals, grouped by proposed fix:

**Fix 1 -- split `profiles_http.h` into a types header (same shape as plan
item 4).** 5 sites: `profiles_builtin.h [persist]:28`,
`profile_executor.h [control]:80`, `profile_executor_state.h [control]:23`,
`profile_feasibility.h [control]:28`, `run_state.h [control]:46`. All five
only need shared profile types/constants, not the httpd handler
declarations. Proposed fix: pull the type/constant declarations these five
actually use out of `profiles_http.h` into a new `profiles_types.h` (home:
`persist`, alongside `profiles_builtin.h`), and have `profiles_http.h`
`#include` it back for its own use -- same pattern as the plan's existing
item 4 split.

**Fix 2 -- split `ota_http.h`'s interlock/status query API out of the httpd
handler header.** 5 sites: `autotune_engine_internal.h [control]:74`
(`ota_http_heat_blocked_by_update()`), `factory_reset.c [persist]:14`
(`ota_http_authenticate_request()`/interlock checks),
`kiln_cfg_store.c [persist]:12` (`ota_http_check_interlocks()`),
`ota_pico_relay.c [net]:69` (progress/fail-reason accessors),
`zones_current_sweep_task.c [control]:15`. None of these five touch httpd
route registration -- they all want a handful of query functions
(`ota_http_check_interlocks`, `ota_http_authenticate_request`,
`ota_http_heat_blocked_by_update`, progress/fail-reason getters). Proposed
fix: move those declarations into the existing `ota_state.h` (already
`net`-tier, already a shared status header) or a new `ota_interlock.h`
companion, and have `ota_http.c` implement them there instead of in
`ota_http.h`.

**Fix 3 -- same pattern for `wifi_provision_http.h`.** 2 sites:
`factory_reset.c [persist]:16` (`wifi_provision_http_get_server()`),
`wifi_prov.c [net]:93`. Proposed fix: expose the one or two accessors these
callers need from a narrow `wifi_provision_state.h` (net-tier, alongside
`wifi_prov_internal.h`) instead of the full httpd-handler header.

**Fix 4 -- RESOLVED 2026-09-05 (decision D reverted, see update above).**
`sim_backend.h` consumers in `control` are a direct consequence
of decision D, not a pre-existing gap.** 4 sites:
`autotune_engine_internal.h [control]:80`, `profile_executor.c
[control]:34`, `profile_executor_relay_io.c [control]:23`,
`profile_executor_run.c [control]:24` -- all call
`sim_backend_enabled()`/`sim_backend_read_all()` to get simulated
thermocouple readings when sim mode is on, the same role a real hw driver
plays for `profile_executor`. Decision D puts `sim` at the top tier because
the sim *backend* drives the system from above (http/bridge-style
injection), but this specific direction -- control *reading* from sim as a
data source -- is the opposite relationship. Proposed fix: split a narrow
`sim_reader.h` (home: `hw`, next to the real thermocouple drivers it
substitutes for) exposing only `sim_backend_enabled()`/
`sim_backend_read_all()`, and keep the rest of `sim_backend.h` (the
orchestration/injection API used from the top tier) where decision D put
it. Flag for the coordinator: this may instead be judged an intentional
exception to keep in place as-is, since sim-as-hw-substitute is a
well-established pattern in this codebase (compare `kiln_io_owner.c`'s hw
abstraction) -- either the split or an explicit exception comment resolves
the finding.

**Fix 5 -- RESOLVED 2026-09-05 (verifier allowlist added, see update
above).** `kiln_io_owner`/`relay_authority`'s interlock includes are very
likely a deliberate exception, not a bug.** 4 sites:
`kiln_io_owner.c [owners]:16` (`danger_mode.h` [safety]),
`kiln_io_owner.c [owners]:17` (`heat_interlock.h` [control]),
`kiln_io_owner.c [owners]:18` (`ota_state.h` [net]),
`kiln_io_owner.h [owners]:116` (`safety_link.h` [safety]),
`relay_authority.h [owners]:23` (`safety_link.h` [safety]). These are the
owner modules that arbitrate direct relay/GPIO access reaching *up* to
consult the safety/control state that gates whether a write is allowed at
all -- CLAUDE.md's "Bypassed owner module bug class" note is explicit that
every relay write must route through these owners with the interlocks
consulted, so an owner checking `danger_mode`/`heat_interlock`/
`safety_link` state before acting is the safety property, not an
architecture violation. Proposed resolution: leave these five in place and
record the exception explicitly (a one-line comment at each `#include`, or
a documented carve-out in the tier table) rather than attempting a header
split that would only relocate the same coupling.

**Fix 6 -- RESOLVED 2026-09-05 (`flash_worker.h` shipped, see update
above).** `log_store_mount.c [persist]:9` includes `uart_bridge.h`
[bridge].** Real, single-purpose include:
`uart_bridge_ext_run_on_flash_worker()`, the flash-safe-executor dispatch
CLAUDE.md's "Flash worker re-entrancy"/"PSRAM stack + NVS = panic" notes
describe. Proposed fix: declare that one function by hand the same way
`safety_cfg_store.c` already does for a different `uart_bridge_ext.c`
function (see the false-positive note above for the precedent), or split it
into a narrow `flash_worker.h` at a shared tier (candidate: `common`, since
it is a single function pointer dispatch with no other bridge-layer
dependency) so both callers stop pulling in all of `uart_bridge.h`.
