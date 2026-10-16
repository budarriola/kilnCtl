# Audit: KilnFW locks, blocking waits and task watchdog (aulocks)

Date: 2026-10-14. Tree: origin/dev `d5bbf13fc`. Code-only review; no board access.

Scope: every mutex, semaphore and critical section in `firmware/KilnFW/App/`
(control, owners, safety, bridge, persist, update, http, net, ui, hw, main*.c);
lock ordering across modules; locks held across blocking calls (SPI, I2C, NVS,
link exchanges, queue waits); `portMAX_DELAY` waits; Task WDT subscription; task
priorities and core pinning.

Out of scope (not re-reported): the flash-worker / `s_bx_lock` inversion class
covered by `FLASH_WORKER_LOCK_INVERSION_AUDIT_2026-10-09.md` (F1-F5 and its
latent dispatch-holder list, including `zcfg_save_lock`, `profiles_save_lock`,
`cfg_save_lock`). Its F1 fix was confirmed in the code:
`relay_cycles_maybe_persist()` now runs after `xSemaphoreGive(s_exec.lock)`
(`profile_executor.c:2102-2119`).

**Key question: can any reachable sequence deadlock or starve heat control or
the safety-link task?** No deadlock was found: no ABBA cycle exists between
module locks, and every `portMAX_DELAY` take outside the excluded flash-worker
class guards a short section whose holder never blocks. Stalls are possible but
bounded and fail safe. The executor tick can wait on a busy flash worker (LOW-1);
guard 9 aborts the firing after 10 s. `safety_poll` waits on `s_exec.lock`
for as long as the executor holds it, which is bounded by 200 ms owner waits
(LOW-3). The Pico's S6b thresholds are 10 s soft and 120 s hard
(`SaftyFW/src/safety_guards.c:26-27`). The one way to silence the heartbeat
outright is a diagnostic PC command (LOW-2).

HIGH 0, MED 0, LOW 5, INFO 9

## Task priorities and pinning (reference)

Every task is unpinned (`tskNO_AFFINITY` or plain `xTaskCreate`), so on the
dual-core S3 the two highest ready tasks always run.

| Priority | Tasks |
|---|---|
| 6 | `link_watchdog` (`bridge/uart_bridge.c:552`) |
| 5 | `safety_poll` (`safety/safety_link.c:174-175`, 594; 8192 B PSRAM stack), `profile_executor` and `profile_exec_wdt` (`control/profile_executor_start.c:100`, 206), `autotune_engine`, `thermo_owner` (`owners/thermo_owner.c:245`), `kiln_io_owner` (`owners/kiln_io_owner.c:664`), `wifi_prov_owner`, `bx_flash_worker`, UART bridges, httpd (IDF default, `net/wifi_provision_http.c:1260-1270`), UI Wi-Fi workers (`ui/ui_page_network*.c`) |
| 4 | `lvgl` (`ui/lvgl_port.c:1356`), `dns_hijack`, `ota_pico_relay` |
| 3 | `screen_idle`, `gpio_probe`, `pico_auto_update` |
| idle+1/+2 | `zone_sweep`, `danger_mode`, `cfg_autofmt`, `kiln_cfg_swap`, `factory_reset_reboot` |

No priority-5 peer busy-loops: each one blocks on a queue, a semaphore or a
driver call, and LVGL (priority 4) calls `vTaskDelay` every loop. Many tasks
share priority 5 with the control and safety tasks, but none of them can starve
those tasks.

## Findings

### LOW-1: executor tick waits without bound on a busy flash worker

- Evidence: `control/profile_executor.c:2119` calls `relay_cycles_maybe_persist()`
  on the executor task every tick. When the persist is due, it reaches
  `persist_snapshot_now()` (`persist/relay_cycles.c` about line 1151), which
  calls `uart_bridge_ext_run_on_flash_worker()`. That call waits on
  `s_bx_lock`, the job queue and `s_bx_done`, all with `portMAX_DELAY`
  (`bridge/uart_bridge_ext.c:657-663`). The `_timeout` sibling
  (`uart_bridge_ext.c:689-720`) bounds only the `s_bx_lock` acquire.
- Scenario:
  1. A firing is running and the relay-cycle counts are dirty, so the
     10-minute persist becomes due.
  2. At that moment the worker is occupied. It might be running a worker job,
     or an httpd profiles save (not mode-gated) might be inside a
     `cfg_save_lock` section, which reserves `s_bx_lock` for the whole section.
  3. The executor blocks until the worker frees. The PID does not tick and the
     relays hold their last state.
- Impact: no deadlock, because nothing the worker runs takes a lock the executor
  holds at that point. If the occupancy exceeds `WATCHDOG_TICK_DEAD_MS` (10 s,
  `profile_executor.c:83`), guard 9 faults the firing. That fails safe, but the
  firing is lost.
- Fix: either move the persist off the control task (set a flag on the tick and
  persist from a low-priority task), or use a dispatch bounded on both the lock
  acquire and the completion wait, leaving the counts dirty and retrying on
  timeout.
- Host test: `test_relay_cycles_persist_runs_after_lock_give`
  (`test_profile_executor_prestart.c`) proves only that no lock is held at the
  call. No test covers a busy worker.

### LOW-2: poll period 0 drains the inbox without `xact_lock` and stops the heartbeat

- Evidence: `safety/safety_link_poll.c:597-602` runs
  `if (period == 0u) { (void)safety_drain_inbox(link, SAFETY_LINK_IDLE_TICK_MS); continue; }`.
  The normal idle drain first try-takes `xact_lock` (timeout 0, about line 749).
  `safety_exchange()` holds `xact_lock` while it waits for its reply. Period 0
  is reachable from the PC bridge command `SAFETY_CMD_SET_POLL_PERIOD`
  (`bridge/uart_bridge_safety.c:148`, which calls `safety_link_set_poll_period()`
  at `safety/safety_link.c:822-833`); that function accepts 0 unconditionally.
- Scenario: a PC tool sets period 0 while the kiln is running.
  - (a) A concurrent command exchange (heat enable, cfg fetch, ceiling
    `guard_raise`) can lose its reply to the unlocked drain. It then times out
    after 5 s and fails.
  - (b) `GET_STATUS`, `safety_update_health()` (link-loss policy, cfg/ceiling
    reconcile) and `safety_build_and_send_context()` all stop. The Pico sees
    silence and trips S6b (after 10 s with current present, or 120 s hard),
    and the ESP-side link-loss policy also stops running.
- Severity is LOW because only a diagnostic PC command reaches it; firmware
  never sets 0.
- Fix:
  - Wrap the period-0 drain in the same `xSemaphoreTake(xact_lock, 0)` try-take
    the idle path uses.
  - Refuse period 0 while ARMED or while heat enable is held, or keep the
    health and context push running in that branch.
- Host test: none. No test in `App/test` sets `poll_period_ms == 0`.

### LOW-3: `safety_poll` waits on `s_exec.lock` with `portMAX_DELAY`

- Evidence: every `safety_poll` iteration calls `safety_build_and_send_context()`
  (`safety/safety_link_poll.c:682`). That calls `profile_executor_get_status(pstat)`
  (`safety/safety_link_frames.c:548`), which takes `s_exec.lock` with
  `portMAX_DELAY` (`control/profile_executor_status.c:473`). The ceiling
  reconcile's halt hook also takes `s_exec.lock`: `profile_executor_fault_halt`,
  installed at `main_control_bringup.c:181` and called from
  `safety_ceiling_sync.c` `enforce_*` under `s_reconcile_lock`. The executor
  tick holds `s_exec.lock` from `profile_executor.c:748` to 2102, and under it
  calls:
  - kiln_io relay writes through `post_and_wait` (each bounded by
    `KILN_IO_OWNER_WAIT_MS` = 200 ms, `owners/kiln_io_owner.c:57`, 709)
  - `adaptive_tune_zone_tick` (`adaptive_tune_lock`, short)
  - `relay_cycles_add` (`s_rc.lock`, short)
  - `thermal_guard_tick`
  - `safety_link_get_status` (`state_lock`, leaf)
  - `sweep_unowned_relays`
- Scenario: the SX1509 is wedged (its own lock timeout is about 6 s), so every
  relay write in a tick runs out its 200 ms owner wait while the executor holds
  `s_exec.lock`. Meanwhile `safety_poll` blocks in `get_status` for the sum of
  those waits, which is several hundred ms to low seconds per tick. Heartbeats
  slow down but do not stop.
- The chain is one step longer through autotune. Under `s_exec.lock` the
  tick calls `exec_mode_state_check()` (`profile_executor.c:2072`), which calls
  `autotune_engine_get_status()` (`profile_executor.c:137`). That takes
  `s_at.lock` with `portMAX_DELAY` (`autotune_engine.c:1725`). The autotune tick
  holds `s_at.lock` (`autotune_engine.c:892`) across its own owner relay write
  (`autotune_engine_guard.c:122`, 200 ms) and, on abort, across
  `force_relays_off()`. With the SX1509 wedged, the stall therefore runs
  autotune → executor → `safety_poll`. Each step is bounded, and nothing under
  `s_at.lock` takes `s_exec.lock`.
- Lock order: `s_reconcile_lock` before `s_exec.lock`, and `xact_lock` before
  `state_lock`. No reverse order exists: the executor never takes
  `s_reconcile_lock`, `xact_lock` or `s_bx_lock` while holding `s_exec.lock`.
  So this is a coupling, not a deadlock. Its bound comes from the owner timeouts,
  well under S6b's 10 s.
- Fix: optional hardening. Take `s_exec.lock` with a bounded wait in
  `profile_executor_get_status()` when called from `safety_poll`, or publish an
  executor status snapshot outside the lock (the `screen_idle` pattern), so the
  heartbeat no longer depends on the control tick.
- Host test: none for the cross-task timing.

### LOW-4: `ILI9488_reset` and `ILI9488_blit_abort` skip the async-blit refusal

- Evidence: every other panel entry point first calls
  `panel_spi_reject_if_blitting()` (`hw/panel_spi_blit.c:49-140`,
  `hw/panel_spi.c:467-486`). `ILI9488_reset` (`hw/panel_spi_bringup.c:531`) and
  `ILI9488_blit_abort` (`hw/panel_spi_blit.c:492`) take the panel lock directly.
- Scenario: while the SPI owner's async trampoline is in flight, a caller of
  either one holds the panel lock and waits on the SPI owner. That is the ABBA
  case the other entry points prevent. It is bounded by the 5000 ms panel lock
  timeout.
- Not reachable today: neither function has a caller outside `hw/panel_spi*`.
- Fix: call `panel_spi_reject_if_blitting()` first in `ILI9488_reset`, and
  document that `blit_abort` is for the blit owner only.
- Host test: none.

### LOW-5: a failed take in `ota_http_update_end()` leaves the OTA claim held until reboot

- Evidence: `http/ota_http.c:290-304` takes `s_ota_lock` with a 50 ms timeout.
  If that times out, the function only logs `"claim may remain held"`.
  `ota_http_update_in_progress()` then keeps returning true. Callers use it to
  refuse both a further update and the start of heating (comment at
  `ota_http.c:319-321`).
- Scenario: lock contention at the moment an update ends leaves the board
  refusing all OTA and heat starts until reboot. This fails closed and needs a
  50 ms stall on a lock whose holders are only field copies.
- Fix: retry the release in a bounded loop, as `danger_mode.c:109` does for its
  rollback.
- Host test: none.

### INFO-1: the Task WDT watches only the idle tasks

`sdkconfig.defaults:470-494` sets `CONFIG_ESP_TASK_WDT_PANIC=y` with a 5 s
timeout and idle-task checking on CPU0 and CPU1. The tree has no
`esp_task_wdt_add()` call sites. `heat_enable.c:40-70` documents this and relies
on it, since its 5.2 s worst-case block would exceed the TWDT period, and
`test_heat_enable.c:972` pins it. As a result, a priority-5 task that hangs
while blocked (`profile_executor`, `safety_poll`, `kiln_io_owner`) is never seen
by the TWDT. Only a busy loop that starves an idle task panics. Liveness for the
control path comes from guard 9 (`profile_exec_wdt`, bounded lock take,
`profile_executor.c:2282`, 10 s stale tick), and for the link from the Pico's
S6b trip. That coverage is adequate for heat safety. A hang in `safety_poll`
while idle (no firing) is caught only by S6b. Anyone who later subscribes a task
must first bound `heat_enable`'s rollback burst, as `heat_enable.c:53` states.

### INFO-2: `safety_poll` reaches the flash worker synchronously

The chain is `safety_update_health` → `safety_cfg_store_maybe_refetch` →
`flush_if_dirty` (`safety/safety_cfg_store.c:710`) →
`uart_bridge_ext_run_on_flash_worker` (`portMAX`). This is the one unbounded
wait on the heartbeat task. It belongs to the excluded flash-worker class and is
listed here only so the safety_poll picture is complete.

### INFO-3: ceiling `guard_raise` under `s_reconcile_lock` can hold `safety_poll` about 5 s

`safety_ceiling_sync_reconcile_on_link_up_nonblocking` only try-takes
`s_reconcile_lock` (timeout 0, `safety_ceiling_sync.c:777-781`). Under the lock
it can run `guard_raise` set-and-confirm, which waits up to 5000 ms in 100 ms
polls. Exchanges continue during the wait, so this is no S6b risk. The blocking
path (`kiln_cfg_swap_worker`, `portMAX`) does not contend with `safety_poll`,
because `safety_poll` skips the tick when the lock is busy.

### INFO-4: `s_store_lock` held across page exchanges and the flush

The httpd refetch (`safety_cfg_store.c:1737-1750`) takes `s_store_lock` with
`portMAX` and holds it across page fetches, the `vTaskDelay` gaps (line 1594)
and the flash-worker flush. `safety_poll` only try-takes it (line 1784), so it
never waits on httpd. Readers do not take the lock; this is a documented data
race, not a deadlock.

### INFO-5: two mutexes are created lazily with no claim

`persist/kiln_cfg_store.c:2029-2033` (`s_swap_lock`) and `:2634-2640`
(`s_pico_half_dirty_lock`) use `if (!h) h = xSemaphoreCreateMutex();`. If two
tasks hit first use at the same time, one mutex leaks and that one overlap goes
unprotected. The possible racers are an httpd zones save and `safety_poll`'s
Pico-half recapture. Concurrent first use is not proven. Fix: create both in
`kiln_cfg_store_init()`. `control/run_state.c:236-246` (`ensure_lock()`) has the
same pattern, but `run_state_init()` runs at `profile_executor_start.c:45`,
before the executor tasks start. Only the HTTP readers (`dashboard_exec_http.c:82`,
`ota_http.c:462`) could race it.

### INFO-6: `s_swap_lock` is held across a save section (latent)

`kiln_cfg_store_apply` holds `kiln_cfg_store_lock()` (`kiln_cfg_store.c:1676`)
across `zones_config_import_blob`, which enters a save section and dispatches to
the worker. No worker job takes `s_swap_lock` today, so no cycle exists. Add it
to the flash-worker audit's latent-holder list, and keep `s_swap_lock` off the
worker.

### INFO-7: leaf locks held across NVS writes

These module locks are held across NVS writes:

- `run_state` `s_rs.lock` (`control/run_state.c:430-436`), on the executor
  after `s_exec.lock` is released, rate-limited
- `dualwrite_window` `s_dw.lock` (`persist/dualwrite_window.c:229-248`), once
  per firing end
- `boot_guard` `s_bg.lock` (`persist/boot_guard.c:509-544`, 677-687, 774-781)

All three are leaves: nothing else is taken under them and no heat or safety
path waits on them while another task holds them. `persist_locked` refuses PSRAM
stacks. No change is needed.

### INFO-8: `danger_mode` rollback retry has no upper bound

`safety/danger_mode.c:109` loops `while (xSemaphoreTake(s_dm.lock, 50 ms) != pdTRUE)`.
This is deliberate (K7 review F1): every holder is short and the loop runs in
the danger-mode caller, not on a control or safety task.

### INFO-9: remaining `portMAX_DELAY` takes guard short sections

The UI job locks (`ui/ui_page_network.c:257-670`,
`ui/ui_page_network_manage.c:127-528`) are taken with `portMAX_DELAY`. Their
workers run the blocking `wifi_prov` call with no lock held, and take the lock
only to copy results. The same applies to:

- `wifi_prov` `s_reply_pool_mutex` (`net/wifi_prov.c:251-347`)
- the Pico rollback status lock (`http/ota_http_pico.c:435-450`)
- `watchdog_cfg` (`safety/watchdog_cfg.c:190-244`)
- `safety_link` `state_lock` (`safety_link_internal.h:52`)
- `heat_enable` `he_lock` (`heat_enable.c:163-169`; sends happen outside it)
- `owner_slot_pool` `s_slot_lock`

None of these can hang. Bounded takes would only be hardening.

### Checked and clean

- There is no LVGL mutex. Only `lvgl_port_task` touches LVGL, and
  cross-task walks use bounded takes (`ui/lvgl_port.c:1037-1168`).
- `zones_cfg_lock` is a portMUX held only for copies and compares; NVS and
  relay-type pushes run outside it.
- `screen_idle` snapshots its producers without holding its lock.
- Locks taken with short timeouts that fail closed:
  - `s_ota_lock`, `s_login_lock` and `s_totp_lock` (50 ms)
  - `s_decide_lock`
  - `http_async_job` (try-claim)
- `update_fetch`:
  - `st_lock` sections are copies only.
  - The `wr_req`/`wr_done` handshake is bounded at `FETCH_WR_TIMEOUT_MS`
    plus 1 s.
  - `update_http` `io_lock` covers only the phase claim; erase, write and hash
    run outside it.
- `uart_protocol` `tx_lock` is bounded by a 1000 ms TX timeout.
- Bridge tasks block on inbox and queue-set receives. The log hook sends with
  timeout 0 and drops the oldest entry.
- `kiln_cfg_swap_worker` queue: send 0, receive `portMAX` (correct for a
  worker).
- `main*.c`, `main_boot_early.c` and `monitor_task.c` take no locks.

## Test gaps

- No host test exercises any cross-module lock ordering. An order checker (debug
  build: record held-lock sets per task, assert a fixed order) would cover the
  whole class.
- No test covers a busy flash worker during the executor's relay-cycle persist
  (LOW-1).
- No test covers `poll_period_ms == 0` (LOW-2).
- No timing test covers how long `safety_poll` waits on `s_exec.lock` with
  wedged owners (LOW-3).
- No test covers the panel async-blit refusal in `ILI9488_reset` (LOW-4) or a
  failed release in `ota_http_update_end` (LOW-5).
- `test_heat_enable.c:972` pins the "no `esp_task_wdt_add()`" premise (INFO-1);
  keep it.
