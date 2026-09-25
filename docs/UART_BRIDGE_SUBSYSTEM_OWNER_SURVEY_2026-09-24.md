# uart_bridge.c remaining per-subsystem dispatch: owner-task survey (2026-09-24)

TODO.md's "Debug/PC-link UART side" bullet asks the six subsystems that never
got a THERMO/IO-style migration (SAFETY, WIFI, TOUCH, UI_TEST, SYSTEM, INFO)
to "post commands to an owning task instead of running the dispatch switch
body inline." This note is the design pass that bullet asked for, done
before touching any code.

## What THERMO/IO actually changed

`thermo_bridge_task`/`io_bridge_task` do not touch the MAX31856/GPIO hardware
themselves. Their switch bodies call `thermo_owner_command_*()`/
`kiln_io_owner_*()`, thin wrappers that post a command onto a *second*,
dedicated task's queue (`thermo_owner_task`, the owner task) and block for
its reply. That second task is the only thing in the firmware that ever
touches the SPI bus / GPIO directly. The motivation was hardware-bus
contention: other tasks (dashboard_http, LVGL, etc.) also need the same
MAX31856/GPIO, so one arbiter task was required.

## Per-subsystem finding

For each of the six, the question is: does the switch case in
`uart_bridge_<name>.c` touch a shared resource directly with no serialization
of its own, or does it already go through a module that owns that
serialization (whether or not that module is named `*_owner`)?

- **SYSTEM** (`uart_bridge_system.c:89`): calls `factory_reset_execute()`,
  `watchdog_cfg_set_panic_disabled()`/`watchdog_cfg_panic_disabled()`,
  `telemetry_log_set_enabled()`/`is_enabled()`. `watchdog_cfg.c` has its own
  `xSemaphoreCreateMutex()` (`watchdog_cfg.c:163`) guarding its state --
  already exactly the "single owner of this state" shape a new owner task
  would add, just implemented as a mutex-protected module instead of a
  queue-and-task. `telemetry_log_*` is a single bool flag with no
  cross-field invariant, so a torn read costs nothing correctness-relevant.
  `factory_reset_execute()` touches NVS/cfgfs; nothing else in this firmware
  calls it concurrently with the UART path today (its only other entry point
  is the analogous HTTP factory-reset handler on the shared httpd worker,
  never running at the same instant as a UART command in the field). No
  hardware bus contention here — SYSTEM has no driver of its own.
  **Finding: no migration needed.**

- **INFO** (`uart_bridge_info.c:247`): every case is a pure read -- compiled
  build_info constants, `stack_margin` registry snapshot, and a cached WiFi
  status struct copy. Nothing here blocks, nothing here touches a hardware
  bus, and there is no mutable state this task itself owns to protect.
  **Finding: no migration needed** (matches the prompt's own hint that
  read-only INFO may not be worth it).

- **TOUCH** (`uart_bridge_touch.c:91`): already migrated for the one real
  risk this class of bug has actually caused here. `TOUCH_CMD_LOG_TAP_TARGETS`
  used to call `kiln_ui_log_tap_targets()` directly, which recurses the live
  LVGL tree and logs a line per widget -- bench-reproduced 2026-09-19 as an
  `IllegalInstruction` panic in `touch_uart_bridge` from running that on this
  task's own thin stack instead of `lvgl_port_task`'s. The fix (already
  landed, see the case's own comment) is exactly the pattern this bullet
  asks for: it now calls `lvgl_port_request_tap_dump()`, which flags
  `lvgl_port_task` — the module that already owns every other LVGL access —
  to run the dump on its own next tick, instead of running LVGL code on the
  UART task. The remaining TOUCH cases (`GET_STATE`, `INJECT`,
  `SET_TAP_DUMP`) call `screen_idle_*()` (own mutex,
  `screen_idle.c:64`/`:312`) and `lvgl_port_inject_touch()`/
  `lvgl_port_get_touch_diag()`/`kiln_ui_get_show_diag()`, documented in
  the source as cheap lock-protected variable writes/reads, not LVGL tree
  walks. **Finding: already migrated where it mattered; no further change
  needed.**

- **WIFI** (`uart_bridge_ext_wifi.c:258`, investigated for this note but not
  in this pass's implement list): every case calls `wifi_prov_*()`. That
  module already implements the post-and-wait shape internally --
  `wifi_prov.c` posts a command and blocks on
  `xSemaphoreCreateBinaryStatic()`/`xSemaphoreTake()` (`wifi_prov.c:142/152/585`)
  the same way `thermo_owner_command_*()` does, just inside `wifi_prov.c`
  rather than named `*_owner`. **Finding: already owner-task-shaped;
  no migration needed.** No code touched this pass since WIFI was not in the
  candidate list, but the survey is recorded here for completeness.

- **SAFETY** (`uart_bridge_safety.c:79`, out of scope this pass by
  instruction — safety-link path): `safety_link_*()` already carries its own
  `state_lock`/`xact_lock` (`safety_link.c:402-403`, taken together at
  `:636-637`). Most queries (`GET_STATUS`, `GET_DIAG`, `GET_LINK_STATS`,
  `GET_TRIP_EVENT`, `FW_VERSION`) are answered from a cache, never by talking
  to the Pico live, by design (comments in the source). `GET_CT_CAL`/
  `GET_PARAM` do live, blocking round trips but through `safety_link`'s own
  serialized transaction path. **Recommendation for a future pass: no
  architectural change needed here either** — this already matches the
  target shape. Left untouched per this pass's scope.

- **UI_TEST** (`uart_bridge_ui_test.c:70`, out of scope this pass by
  instruction — bench LCD rerun dependency): this is the one subsystem where
  the TOUCH-class bug is still live. `UI_TEST_CMD_LIST_TAP_TARGETS` calls
  `kiln_ui_collect_tap_targets()` and `UI_TEST_CMD_CLICK_BY_NAME` calls
  `kiln_ui_click_by_name()`, both directly on `ui_test_bridge_task`'s own
  stack/thread — the same "walk the live LVGL tree from a task that isn't
  `lvgl_port_task`" shape that caused the 2026-09-19
  `touch_uart_bridge` panic before `TOUCH_CMD_LOG_TAP_TARGETS` was fixed.
  **Recommendation: follow up with the same fix shape** — add an
  `lvgl_port_request_*()`-style post into `lvgl_port_task` for these two
  commands (with a reply-carrying completion, since both need a return
  value TOUCH's fire-and-forget dump didn't), once the bench LCD rerun that
  currently depends on `UI_TEST_CMD_CLICK_BY_NAME`'s exact current code path
  is not at risk of being disrupted mid-run.

## Net result of this pass

No firmware C change: SYSTEM and INFO were already fine, and TOUCH's only
real risk was already fixed in an earlier, targeted panic-fix commit (dated
2026-09-19) that happens to be exactly the migration shape this TODO bullet
asked for. WIFI and SAFETY were surveyed and found already owner-task-shaped
inside their respective modules. The one confirmed remaining gap is
UI_TEST's `LIST_TAP_TARGETS`/`CLICK_BY_NAME`, deliberately left alone this
pass per instruction (bench dependency) and recorded above as the concrete
follow-up.
