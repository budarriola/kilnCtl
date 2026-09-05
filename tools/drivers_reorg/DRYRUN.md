# drivers/ reorg dry run (2026-09-05)

Output of `tools/drivers_reorg/plan_moves.ps1 -DryRun`, trimmed to the
actionable lists. Full raw output kept locally as
`tools/drivers_reorg/_dryrun_raw.txt` (gitignored-by-convention scratch file,
not committed -- regenerate with the command above). Nothing was applied;
this whole pass is preparation only.

## 1. git mv plan

358 files map into the ten target dirs from `mapping.csv` (354 layer files +
4 BUILD_META files that are excluded from git mv pending a coordinator
decision -- see mapping.csv's `AMBIGUOUS:` rows). No basename collisions
across the ten target dirs. Full list of `git mv` commands: run the script,
or read `mapping.csv` directly (one row per file).

## 2. CMakeLists literal SRCS rewrite

- `firmware/KilnFW/App/drivers/CMakeLists.txt`: **201** literal quoted-filename
  SRCS occurrences found (plan estimated 185; the extra ~16 are files added to
  `drivers/` since the plan was written -- e.g. the `board_temps_http.*` /
  `zones_config_accessors.*` splits).
- `firmware/KilnFW/App/CMakeLists.txt`: **0** literal `drivers/<file>` paths.
  It only has `REQUIRES drivers` (component-level dependency, not a file
  list) -- so the 185+ literal SRCS the plan refers to live entirely inside
  `drivers/CMakeLists.txt`, not split between the two files as the plan's
  wording could be read to imply.
- **Consequence for -Apply**: since the SRCS list is single-file, the move
  requires either (a) writing new per-layer `CMakeLists.txt` fragments,
  pulled in from a new `App/CMakeLists.txt` layout, or (b) one consolidated
  SRCS list that walks all ten new directories. Not decided here --
  flagged as a BUILD_META row in mapping.csv for the coordinator.

## 3. Bare-include feasibility

**Preferred: keep bare `#include "x.h"`.** List all ten new directories in
`idf_component_register`'s `INCLUDE_DIRS` (the same mechanism that makes
`drivers/` itself a single include-path entry today). `mapping.csv`'s
358 target paths have **zero basename collisions** across the ten
directories, so an unqualified include can never resolve ambiguously
post-move. No `#include` line needs to become path-qualified.

## 4. Path-keyed check/test sites

Scanned `tools/**/*.ps1`, `tools/**/*.py`, and
`firmware/KilnFW/App/test/**/*.ps1|*.py` (excluding `.venv`/`node_modules`/
`site-packages`/`.git`) for the literal `drivers/` or any of the 358
mapped filenames: **1418 matching lines across 341 files**. The large
majority are documentation/docstring path citations in `tools/PcTools/src/kilnctrl/*.py`
(e.g. `ota_http_client.py`, `pid_validation.py`, `protocol.py` -- comments
citing a `firmware/KilnFW/App/drivers/foo.c` path or line range for a mirrored
constant) -- stale after the move but **not a runtime break**. Below are the
two truly actionable subsets.

### 4a. Runtime code that resolves a `drivers/` filesystem path (WILL break)

```
firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py:151:  uart_ids = root / "firmware/KilnFW/App/drivers/uart_task_ids.h"
firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py:153:  max_payload_hdr = root / "firmware/KilnFW/App/drivers/espInterfaces/uart_protocol.h"
```

### 4b. check_*.ps1 / check_*.py / selfcheck*.py scripts hardcoding `App/drivers/...`

18 scripts -- exactly the "splits break filename-keyed checks" class CLAUDE.md
warns about (9 of 12 past splits broke a check silently). Each needs its
literal path(s) rewritten to the new layer directory in the same commit as
the move, and re-run afterward to confirm it can still go red:

```
firmware/KilnFW/App/test/check_flash_worker_lint.ps1
firmware/KilnFW/App/test/check_thermal_guard_input_producers.ps1
firmware/KilnFW/App/test/test_check_hal_include_boundary.ps1
tools/PcTools/selfcheck.py
tools/PcTools/selfcheck_zones_fields.py
tools/check_bridge_reject_reason.ps1
tools/check_c_files_in_cmakelists.ps1
tools/check_doc_citations.ps1
tools/check_duplicate_symbols.ps1
tools/check_hal_include_boundary.ps1          (7 RelPath literals: dashboard_http.c, ota_http.c, ota_http_esp.c, ota_http_pico.c, ota_http_recovery.c, partition_info_http.c, ui_page_diagnostics.c, wifi_prov*.c/.h)
tools/check_heat_enable_wiring.ps1
tools/check_host_embed_symbols_defined.ps1    (parses "#include \"../drivers/X.c\"" convention -- becomes "../<layer>/X.c")
tools/check_no_duplicate_crc.ps1              (also has a firmware/UnitTestFw/... drivers/ literal -- UnitTestFw is explicitly OUT of scope, do not touch that one)
tools/check_relay_writes_through_owner.ps1    (RelPath = ".../drivers/kiln_io.c", ".../drivers/profile_executor.c")
tools/check_safety_baud_sync.ps1              ($kconfigPath = '.../App/drivers/Kconfig' -- see BUILD_META Kconfig row)
tools/check_safety_call_results_checked.ps1   (RelPath = ".../drivers/profile_executor.c")
tools/check_stack_margin_baseline.py
tools/check_uart_version_independence.ps1
tools/check_uri_handler_cap.ps1
```

### 4c. Host-test "mirror" scripts under App/test that #include a drivers/*.c file directly

These bind a Python (or C) mirror to the real source file per the "Binding a
Python mirror to C" pattern -- their `#include "../drivers/X.c"` or path
literal must be updated to `"../<layer>/X.c"` or the mirror silently stops
tracking the real function:

```
firmware/KilnFW/App/test/approach_rate_cap_mirror_drift_check.py
firmware/KilnFW/App/test/attribute_str_pool.py
firmware/KilnFW/App/test/flash_worker_lint.py
firmware/KilnFW/App/test/frame_a_offset_drift_check.py
firmware/KilnFW/App/test/fuzzy_gain_mirror_drift_check.py
firmware/KilnFW/App/test/heater_output_pwm_drift_check.py
firmware/KilnFW/App/test/pid_fuzzy_drift_check.py
firmware/KilnFW/App/test/power_diag_flag_mirror_drift_check.py
firmware/KilnFW/App/test/ramp_lock_decision_mirror_drift_check.py
firmware/KilnFW/App/test/ramp_stepping_gate_mirror_drift_check.py
firmware/KilnFW/App/test/source_path_drift_check.py
firmware/KilnFW/App/test/wire_protocol_fingerprint_check.py
```

`firmware/KilnFW/App/test/build_host_tests.ps1` and `firmware/KilnFW/App/test/stubs/*`
were scanned and contain **no** hardcoded `drivers/` literal -- host-test
sources are pulled in per-file by the mirror/drift-check scripts above, not
by a directory glob in the build script itself.

## 5. Include-direction verifier

Tiers: `ui/http/bridge` (top) -> `control/safety/persist/net` (mid) ->
`owners/hw/sim` (bottom). A violation is a lower-tier file `#include`-ing a
header whose file maps to a higher tier.

**73 upward includes found** -- well above the "near zero" the plan expects
after items 1-6 landed. Most are a narrow shared header (`settings.h`,
`stack_margin.h`, `uart_task_ids.h`, `http_form.h`, `web_encoding.h`,
`zones_http_internal.h`, `profiles_http.h`) reached from a lower tier for one
constant/type rather than a real behavioral dependency -- the same shape as
the six patterns the plan already untangled (see plan items 1-6), just not
yet done for these. Full list:

```
esp_spi_owner.c [owners]:10 includes "stack_margin.h" [safety]
adaptive_tune_http.c [control]:11 includes "http_form.h" [http]
autotune_engine_internal.h [control]:91 includes "uart_bridge.h" [bridge]
backlight_pwm.c [hw]:10 includes "stack_margin.h" [safety]
backlight_pwm.h [hw]:42 includes "screen_idle.h" [ui]
backup_export.c [persist]:18 includes "backup_http.h" [http]
backup_export.c [persist]:19 includes "backup_http_internal.h" [http]
backup_export.c [persist]:32 includes "profiles_http.h" [http]
backup_export.c [persist]:33 includes "web_encoding.h" [http]
backup_import.c [persist]:33 includes "backup_http.h" [http]
backup_import.c [persist]:34 includes "backup_http_internal.h" [http]
backup_import.c [persist]:49 includes "profiles_http.h" [http]
danger_mode.c [safety]:14 includes "uart_task_ids.h" [bridge]
factory_reset.c [persist]:13 includes "http_form.h" [http]
FT6336U.c [hw]:23 includes "settings.h" [persist]
kiln_io.c [owners]:11 includes "settings.h" [persist]
kiln_io_owner.c [owners]:16 includes "danger_mode.h" [safety]
kiln_io_owner.c [owners]:17 includes "heat_interlock.h" [control]
kiln_io_owner.c [owners]:18 includes "ota_state.h" [net]
kiln_io_owner.c [owners]:21 includes "stack_margin.h" [safety]
kiln_io_owner.h [owners]:116 includes "safety_link.h" [safety]
log_store_mount.c [persist]:9 includes "uart_bridge.h" [bridge]
MAX31856.c [hw]:9 includes "settings.h" [persist]
MAX31856.h [hw]:52 includes "uart_task_ids.h" [bridge]
NS2009.c [hw]:15 includes "settings.h" [persist]
NS2009.c [hw]:16 includes "stack_margin.h" [safety]
ota_http.c [net]:32 includes "boot_button.h" [bridge]
ota_http.c [net]:43 includes "web_encoding.h" [http]
ota_http.c [net]:79 includes "boot_button.h" [bridge]
ota_http.c [net]:90 includes "web_encoding.h" [http]
ota_http_esp.c [net]:32 includes "boot_button.h" [bridge]
ota_http_esp.c [net]:43 includes "web_encoding.h" [http]
ota_http_pico.c [net]:32 includes "boot_button.h" [bridge]
ota_http_pico.c [net]:43 includes "web_encoding.h" [http]
ota_http_recovery.c [net]:32 includes "boot_button.h" [bridge]
ota_http_recovery.c [net]:43 includes "web_encoding.h" [http]
ota_pico_relay.c [net]:71 includes "uart_task_ids.h" [bridge]
panel_spi.c [hw]:40 includes "settings.h" [persist]
panel_spi_blit.c [hw]:22 includes "settings.h" [persist]
panel_spi_bringup.c [hw]:30 includes "settings.h" [persist]
profiles_builtin.h [persist]:28 includes "profiles_http.h" [http]
profile_executor.h [control]:80 includes "profiles_http.h" [http]
profile_executor_state.h [control]:23 includes "profiles_http.h" [http]
profile_feasibility.h [control]:28 includes "profiles_http.h" [http]
relay_authority.h [owners]:23 includes "safety_link.h" [safety]
run_state.h [control]:46 includes "profiles_http.h" [http]
safety_cfg_http.c [safety]:12 includes "http_form.h" [http]
safety_cfg_http.c [safety]:15 includes "uart_task_ids.h" [bridge]
safety_cfg_http.c [safety]:16 includes "web_encoding.h" [http]
safety_cfg_store.c [safety]:23 includes "uart_bridge.h" [bridge]
safety_link.c [safety]:47 includes "uart_task_ids.h" [bridge]
safety_link_commands.c [safety]:36 includes "uart_task_ids.h" [bridge]
safety_link_frames.c [safety]:45 includes "uart_task_ids.h" [bridge]
safety_link_inbox.c [safety]:42 includes "uart_task_ids.h" [bridge]
safety_link_payload.c [safety]:36 includes "uart_task_ids.h" [bridge]
safety_link_poll.c [safety]:35 includes "uart_task_ids.h" [bridge]
sim_backend.c [sim]:14 includes "http_form.h" [http]
sim_backend.c [sim]:15 includes "uart_task_ids.h" [bridge]
sim_backend.c [sim]:16 includes "wifi_provision_http.h" [net]
sim_backend.c [sim]:17 includes "zones_config_accessors.h" [persist]
SX1509.c [hw]:14 includes "settings.h" [persist]
SX1509.c [hw]:15 includes "stack_margin.h" [safety]
thermo_owner.c [owners]:13 includes "stack_margin.h" [safety]
wifi_provision_http.c [net]:13 includes "http_form.h" [http]
wifi_provision_http.c [net]:14 includes "web_encoding.h" [http]
wifi_provision_http.c [net]:17 includes "httpd_socket_budget.h" [http]
zones_config_accessors.c [persist]:1 includes "zones_http_internal.h" [http]
zones_config_json.c [persist]:23 includes "http_form.h" [http]
zones_config_json.h [persist]:47 includes "uart_task_ids.h" [bridge]
zones_config_store.c [persist]:1 includes "zones_http_internal.h" [http]
zones_current_sweep_engine.c [control]:1 includes "zones_http_internal.h" [http]
zones_current_sweep_task.c [control]:1 includes "zones_http_internal.h" [http]
zones_current_sweep_task.c [control]:20 includes "uart_task_ids.h" [bridge]
```

Note `uart_task_ids.h` is bridge-tier per the plan's census table (grouped
with `uart_bridge_*`), but it is a pure task-id/version-constant header with
no bridge behavior of its own -- 15 of the 73 hits are this one header. If
the coordinator reclassifies `uart_task_ids.h` (and similarly narrow leaf
headers `http_form.h`, `web_encoding.h`, `settings.h`) as a shared
lower-tier header instead of leaving them keyed to their current directory,
the violation count drops sharply without any code change -- purely a
mapping.csv placement decision. Flagged here rather than resolved, per the
task's ambiguous-placement handling.
