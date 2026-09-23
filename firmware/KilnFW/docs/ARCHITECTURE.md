# Firmware Architecture

> **Status:** in progress · **Last reviewed:** 2026-08-19
> **Keep this file current.** Task priorities and ownership are safety
> properties, not implementation detail — if they change, this file changes
> in the same commit. If it disagrees with the code, **the code wins.**

FreeRTOS on the ESP32-S3, structured the way `firmware/SaftyFW` structures
the RP2040 (`SaftyFW/docs/ARCHITECTURE.md` is the model this file follows):
**every hardware interface and every piece of shared mutable state is owned
by exactly one task**, and everything else talks to that owner through a
bounded command queue rather than touching the peripheral or the state
directly. Unlike `SaftyFW`, `KilnFW` runs on a single logical core for
scheduling purposes (`tskNO_AFFINITY` everywhere — see §1) and gets its
isolation from priority and queues, not core affinity.

This file was written after `TODO.md` section 10.14's owner-task pass
(Phases 1, 2, 4 done; Phase 3 skipped with a documented reason; Phase 5
partially absorbed into Phases 1/2; Phase 6 designed, not built) gave
`KilnFW` a genuinely nontrivial task architecture. Everything below is
verified against the code that creates these tasks, cited by file and line.

---

## 1. Task inventory

Every `xTaskCreate`/`xTaskCreatePinnedToCore` call in `firmware/KilnFW/App`,
as of 2026-08-19, **file:line citations corrected 2026-09-04**. The original
2026-08-19 pass cited `uart_bridge.c` and `uart_bridge_ext.c` for most of the
bridge tasks and `profile_executor.c` for both executor tasks; both files
have since been split (per this project's own "After splitting an oversized
firmware file" gotcha in the root `CLAUDE.md`), so nearly every citation in
this table pointed past that file's current end-of-file. Re-verified against
`xTaskCreate*`/`stack_margin_register` call sites directly. **All of them
pass `tskNO_AFFINITY`** — core placement is left to the scheduler; only
priority orders them. Priority numbers are FreeRTOS convention: higher = more
urgent.

| Task | Prio | Stack | Owns / does | Created in |
|---|---|---|---|---|
| `link_watchdog` | **6** | 3072 | Highest-priority task in the system: drops all relays and asserts the isolated fault line when the PC UART link goes silent. Calls `kiln_io_all_relays_off()` **directly**, bypassing `kiln_io_owner` — see §2's exceptions | `App/drivers/bridge/uart_bridge.c:549` |
| `uart_log_bridge` (sender) | 7* | 4096 | *Highest number in the file, but it's the log drain, not a control path — see the file's own comment; treat `link_watchdog`(6)/owner tasks(5) as the real safety-priority ceiling* | `App/drivers/bridge/uart_log_bridge.c:326` |
| `kiln_io_owner` | 5 | 4096 | **Single writer** of the SX1509 relay/IO expander — all relay, digital-IO, and raw-register writes | `App/drivers/owners/kiln_io_owner.c:533` |
| `thermo_owner` | 5 | 4096 | **Single writer/caller** of the MAX31856 thermocouple SPI API (config, thresholds, one-shot/read, faults) | `App/drivers/owners/thermo_owner.c:245` |
| `profile_executor` | 5 | 4096 | The fire-profile control task: PID/bang-bang per zone, ramp/dwell stepping, relay drive via `kiln_io_owner`'s AUTHORIZED path | `App/drivers/control/profile_executor_start.c:85` (moved out of `profile_executor.c`, which split into several `profile_executor_*.c` files) |
| `profile_exec_wdt` | 5 | **4096** (STALE — was 2560, corrected 2026-09-04) | Guard 9 — profile-executor's own watchdog; forces relays off if the control task's tick goes stale or the safety link is silent ≥30 s. Deliberately independent of the task it watches | `App/drivers/control/profile_executor_start.c:129` |
| `autotune_engine` | 5 | 4096 | PID autotune step-test task (relay-feedback autotune not yet built) | `App/drivers/control/autotune_engine.c:813` |
| `wifi_prov_owner` | 5 | 4096 | **Single writer** of `s_wifi` — all `esp_wifi_*`/NVS Wi-Fi calls, including the driver's own `on_wifi_event`/`on_ip_event` handlers, routed through the same queue as external callers | `App/drivers/net/wifi_prov.c:399` |
| `thermo_uart_bridge` | 5 | 4096 | UART bridge subsystem task, THERMO command family — dispatches into `thermo_owner` | `App/drivers/bridge/uart_bridge_thermo.c:475` (split out of `uart_bridge.c`) |
| `io_uart_bridge` | 5 | 4096 | UART bridge subsystem task, IO command family — dispatches into `kiln_io_owner` | `App/drivers/bridge/uart_bridge_io.c:652` (split out of `uart_bridge.c`) |
| `touch_uart_bridge` | 5 | 3072 | UART bridge subsystem task, TOUCH command family | `App/drivers/bridge/uart_bridge_touch.c:274` (split out of `uart_bridge.c`) |
| `safety_uart_bridge` | 5 | 4096 | UART bridge subsystem task, SAFETY command family (isolated-link cache reads, mostly fire-and-forget) | `App/drivers/bridge/uart_bridge_safety.c:294` (split out of `uart_bridge.c`) |
| `system_uart_bridge` | 5 | 3072 | UART bridge subsystem task, SYSTEM command family (`FACTORY_RESET` reboots immediately after, so blocking briefly is accepted) | `App/drivers/bridge/uart_bridge_system.c:162` (split out of `uart_bridge.c`) |
| `info_uart_bridge` | 5 | 3072 | UART bridge subsystem task, INFO/version command family | `App/drivers/bridge/uart_bridge_info.c:337` (split out of `uart_bridge.c`) |
| `info_boot_push` | 5 | 3072 | One-shot: pushes firmware version/boot info to the PC link right after boot | `App/drivers/bridge/uart_bridge_info.c:359` (split out of `uart_bridge.c`) |
| `control_uart_bridge` | 5 | 4096 | UART bridge extension task, CONTROL command family (calibration offset, etc.) | `App/drivers/bridge/uart_bridge_ext_control.c:199` (split out of `uart_bridge_ext.c`) |
| `profiles_uart_bridge` | 5 | 4096 | UART bridge extension task, PROFILES command family — dispatches into `profile_executor`'s public API | `App/drivers/bridge/uart_bridge_ext_control.c:593` (split out of `uart_bridge_ext.c`, landed alongside CONTROL rather than its own file) |
| `autotune_uart_bridge` | 5 | 4096 | UART bridge extension task, AUTOTUNE command family | `App/drivers/bridge/uart_bridge_ext_autotune.c:239` (split out of `uart_bridge_ext.c`) |
| `wifi_uart_bridge` | 5 | **8192** (STALE — was 4096, corrected 2026-09-04) | UART bridge extension task, WIFI command family — dispatches into `wifi_prov`'s public API | `App/drivers/bridge/uart_bridge_ext_wifi.c:404` (split out of `uart_bridge_ext.c`) |
| `bx_flash_worker` | 5 | configurable (`BX_WORKER_STACK`) | **Missing from this inventory until 2026-09-04.** Single worker for flash-touching UART-bridge-extension jobs (dispatched serially off a queue, plain `xTaskCreatePinnedToCore`, not the `*WithCaps` retry helper the other `uart_bridge_ext_*` tasks use — see the file's own comment) | `App/drivers/bridge/uart_bridge_ext.c:324` |
| `hal_uart_esp_event_task` (started via `uart_owner_init`) | configurable | configurable | `espInterfaces/uart_owner.c` — event-handling task underneath the PC-link UART port (`uart_owner_task`/`uart_owner_evt_task` request-queue tasks were deleted; a second instance runs for the safety link). 2026-09-06 uart collapse: `uart_owner_init` no longer duplicates driver-install/task-creation itself — it delegates to `hal_uart_init`, whose backend spawns this task (same FIFO_OVF/BUFFER_FULL/line-error behavior as the old `uart_owner_event_task`, which was deleted) | `firmware/hwAbstraction/esp/uart/uart_owner.c:75` (delegating call), `firmware/hwAbstraction/esp/uart/hal_uart_esp.c:196` (task creation) |
| `uart_protocol_rx_task` | configurable | configurable | `espInterfaces/uart_protocol.c` — frame parser reading off `uart_owner`'s RX path | `firmware/hwAbstraction/esp/uart/uart_protocol.c:309` |
| `esp_spi_owner` (`spi_owner_task`) | configurable | configurable | `espInterfaces/esp_spi_owner.c` — single owner of the shared SPI bus request queue, underneath `thermo_owner`/MAX31856 | `firmware/hwAbstraction/esp/spi/esp_spi_owner.c:81` |
| `i2c_owner_task` | configurable | configurable | `espInterfaces/i2c_owner.c` — single owner of the shared I2C bus request queue, underneath `kiln_io_owner`/SX1509 | `firmware/hwAbstraction/esp/i2c/i2c_owner.c:136` |
| `safety_poll` | `SAFETY_POLL_TASK_PRIORITY` | `SAFETY_POLL_TASK_STACK` | `safety_link.c` — polls/exchanges frames with the RP2040 safety processor over the isolated link, builds the periodic context broadcast | `App/drivers/safety/safety_link.c:533` |
| `lvgl` (`lvgl_port_task`) | 4 | 8192 | **The only task allowed to call any `lv_*` function.** Owns the on-device LCD UI entirely | `App/drivers/ui/lvgl_port.c` (line unverified this pass; not spot-checked) |
| `monitor_task` | 4 | 3072 | Heartbeat/liveness monitor | `App/monitor_task.c:130` |
| `gpio_probe` | 3 | **6144** (STALE — was 3072, corrected 2026-09-04) | Debug GPIO probing task (bench/dev tool) | `App/drivers/bridge/gpio_probe.c:305` |
| `screen_idle` | 3 | **6144** (STALE — was 3072, corrected 2026-09-04) | LCD idle/screensaver timer | `App/drivers/ui/screen_idle.c:326` |
| `wifi_mode_ui` / `wifi_connect_ui` / `wifi_scan_ui` / `wifi_ap_id_ui` | 5 | 4096 | **STALE, corrected 2026-09-04**: `ui_page_network.c` split — `wifi_connect_ui`/`wifi_scan_ui` moved to `ui_page_network_manage.c`, `wifi_mode_ui` stayed, and a FOURTH task, `wifi_ap_id_ui` (AP-identity worker), was added and was missing from this inventory entirely. All four are still ad-hoc job-struct worker tasks predating and **not yet migrated onto** `wifi_prov_owner`'s queue — see §3 | `App/drivers/ui/ui_page_network.c:271` (`wifi_mode_ui`), `:585` (`wifi_ap_id_ui`), `App/drivers/ui/ui_page_network_manage.c:296` (`wifi_connect_ui`), `:378` (`wifi_scan_ui`) |
| `ota_pico_relay` | task-supplied | `OTA_PICO_RELAY_TASK_STACK` | Relays an OTA image to the Pico safety processor over the link | `App/drivers/net/ota_pico_relay.c:757` |
| `ota_confirm` | task-supplied | 3072 | One-shot: confirms an ESP-side OTA rollback candidate | **STALE, corrected 2026-09-04**: `App/main.c` was itself split by boot phase the same day as the table-wide correction above; the task creation moved to `App/main_network_http.c:472` (`main_ota_rollback_confirm_task()`, defined `:131`) |
| `factory_reset_reboot` | `tskIDLE_PRIORITY+1` | 2048 | One-shot: reboots after a factory-reset request | `App/drivers/http/factory_reset.c:104` |

**`esp_http_server`'s single worker task** is not created by this codebase
(it's `esp_http_server`'s own internal task, started once by
`wifi_provision_http_start()`'s `httpd_start()` call —
`App/drivers/http/wifi_provision_http.c:556`). Every `*_http.c` module
(`dashboard_http.c`, `zones_http.c`, `rules_http.c`, `profiles_http.c`,
`ota_http.c`, `wifi_provision_http.c`) registers URI handlers against that
one server; none of them start their own httpd instance. This means every
HTTP handler across every page shares one worker — a long handler (OTA
transfer, a Wi-Fi mode switch) blocks every other HTTP client, including the
dashboard's own polling, for its duration. See §3.

---

## 2. The single-writer ownership doctrine

Traced from `TODO.md` §10.14 and the owner modules' own header comments.

### What has exactly one writing task

| State | Owner task | Producers post to it via |
|---|---|---|
| SX1509 relay/IO expander (relays, digital IO, raw registers) | `kiln_io_owner` | `kiln_io_owner_command_*()` |
| MAX31856 thermocouple channels (config, thresholds, reads, faults) | `thermo_owner` | `thermo_owner_command_*()` |
| `wifi_prov.c`'s `s_wifi` module state (Wi-Fi mode, credentials, AP identity) | `wifi_prov_owner` | `wifi_prov_*()` public API (unchanged signatures — every caller became a producer with zero call-site edits) |
| Shared SPI bus | `esp_spi_owner` | request-queue posts from `thermo_owner`/`MAX31856.c` |
| Shared I2C bus | `i2c_owner` | request-queue posts from `kiln_io_owner`/`SX1509.c` |
| PC-link UART port | `uart_owner` | `uart_protocol`/`uart_bridge*` |
| Fire-profile run state | `profile_executor`'s tasks | its own public API (`_run()`/`_halt()`/`_pause()`/`_resume()`), guarded by `s_exec.lock` — **not** a queue (see §3, Phase 3) |

### Two producer families (`kiln_io_owner`/`thermo_owner` pattern)

Mirrors `firmware/SaftyFW/src/tasks/relay_owner.c`'s shape exactly (an
enum + tagged command struct, a small bounded `xQueueCreate`d queue, one
`<owner>_command_<verb>()` post function per command):

- **MANUAL** (`kiln_io_owner_command_set_relay[_mask]()`): applies the
  ownership check (is this relay owned by a running profile?) and the
  safety-fault check before the write. Used by the UART bridge and
  `dashboard_http.c`'s relay handler (HTTP + LCD).
- **AUTHORIZED** (`kiln_io_owner_command_set_relay_mask_authorized()`): no
  ownership or safety-fault check — the caller (`profile_executor.c`,
  `autotune_engine.c`) already *is* the owner of the relays it names, via
  `relay_authority_zone_blocked()`'s zone-level gate, checked before this is
  ever called. This function exists only to serialize the actual I2C write
  against the MANUAL writers through the same queue; it does not change who
  is allowed to command what.

Every producer is a bounded, non-blocking `xQueueSend(..., 0)` — never
blocks the caller if the queue is full — followed by a bounded wait
(200 ms, `KILN_IO_OWNER_WAIT_MS`/`THERMO_OWNER_WAIT_MS`) on a per-call
result. Queue depth is 8 for both `kiln_io_owner` and `thermo_owner`
(`KILN_IO_OWNER_QUEUE_LEN`/`THERMO_OWNER_QUEUE_LEN`,
`kiln_io_owner.c:15`/`thermo_owner.c:14`). A timeout is treated exactly
like an I/O failure — fail closed, never "assume it worked"
(`kiln_io_owner_relay_result_t`'s `ERR_TIMEOUT` case).

**Result/semaphore storage — module-owned pool, never the caller's stack
(read this before writing the next owner task).** The result struct and the
"done" semaphore a producer waits on used to be locals in the producer's own
stack frame (this is `relay_owner.c`'s original shape, and Phase 1/2 copied
it faithfully). That is a use-after-free waiting to happen: the producer's
bounded wait (200 ms) is a client-patience budget, **not** a bound on how
long the owner task can actually take — `SX1509_LOCK_TIMEOUT_MS` alone is
6000 ms, thirty times the wait — so a stalled bus let a timed-out producer
return (freeing/reusing its stack frame) while the owner task was still
going to write the result and give the semaphore through pointers into that
now-dead frame: a cross-task write into whatever the frame held next.
Found and fixed 2026-08-24 in both `kiln_io_owner.c` and `thermo_owner.c`
(`i2c_owner.c`/`uart_owner.c` did **not** share this bug — see their own
`_transfer()` functions, which wait `portMAX_DELAY` on the semaphore instead
of a client-side timeout, so their producer never gives up while the owner
side might still be working; that is a valid alternative fix but was
rejected here because it would let a stalled bus block an HTTP handler or
`rules_task`'s watchdog-fed control loop for up to 6 s).

The fix, and the pattern any **new** owner task in this codebase must
follow: a small, fixed pool of result slots (`s_slots[]`, sized to the
command queue depth) owned by the module itself — static storage, never
freed — with each slot's lifecycle tracked by a two-sided reference count
(`firmware/hwAbstraction/esp/spi/owner_slot_pool.h`/`.c`, host-tested by
`App/test/test_owner_slot_pool.c`). A slot handed out by
`owner_slot_pool_alloc()` is held by **both** the producer and the owner
task; each releases its own half exactly once (the producer after it stops
waiting, success or timeout; the owner task after it writes the result and
gives the semaphore), and only the release that arrives **second** actually
returns the slot to the free pool. This makes the order irrelevant — a
producer timing out before the owner finishes, and the owner finishing
before the producer's timeout fires, both end in the same state — and
guarantees a slot can never be reused (silently corrupting a *different*,
newer command's result) while either side might still touch it. See
`owner_slot_pool.h`'s top comment for the full invariant and
`kiln_io_owner.c`'s `post_and_wait()`/`owner_task()` for the reference
wiring.

`wifi_prov_owner` additionally routes the Wi-Fi **driver's own event
handlers** (`on_wifi_event`, `on_ip_event`, `ap_fallback_timer_cb`,
`rescan_timer_cb`) through the same queue as fire-and-forget posts — the
one thing that made Phase 4 riskier than Phases 1/2, because it required
touching code that isn't a producer call site but an interrupt-adjacent
callback.

### Deliberate exceptions

**Direct `kiln_io_all_relays_off()` call sites bypass `kiln_io_owner` on
purpose.** Three call sites, all fail-safe/last-resort paths that must keep
working even if the owner task itself is wedged:

1. `App/drivers/bridge/uart_bridge.c`'s `link_watchdog_task` (**STALE citation
   corrected 2026-09-04**: function at `uart_bridge.c:311`, task creation
   already correctly cited in §1's table at `:549`) — drops relays when the
   PC link goes silent. "Must still run when every bridge task is blocked."
2. `App/drivers/control/profile_executor.c`'s `watchdog_task_entry` (guard 9,
   **STALE citation corrected 2026-09-04**: function at
   `profile_executor.c:1282`, its `kiln_io_all_relays_off()` calls at
   `:1397`, `:1448`, `:1469`) — drops relays on a stale control tick or a
   30 s-silent safety link. "Must still run if the main control task is
   stuck."
3. `App/main.c`'s boot-failure/panic shutdown path (**STALE, corrected
   2026-09-04**: this was cited as `kiln_enter_safe_state()` at `main.c:79`;
   the function was renamed `main_kiln_enter_safe_state()` and moved to
   `App/main.c:155` when `main.c` was split by boot phase the same day — see
   `docs/SYSTEM_ARCHITECTURE.md` §6's boot-order section for that split).

Routing any of these through `kiln_io_owner`'s queue would make them depend
on the owner task *not* being the thing that's wedged — backwards for code
whose entire purpose is acting when something else already is.
`kiln_io_all_relays_off()` is unconditional and only ever turns things
**off**, so a race between one of these and the owner task mid-write is
benign in the failure direction: worst case is a redundant I2C transaction,
never an unsafe state (`kiln_io_owner.h:62-76`).

**LVGL single-task rule.** Only `lvgl_port_task` may call any `lv_*`
function (`lvgl_port.h`'s header comment, `lvgl_port.c:67,80`). This is
what the whole Phase 4/kiln_io_owner effort was triggered by: `scan_btn_cb()`
in `ui_page_network.c` called a blocking `wifi_prov_scan()` straight from
this task and froze the entire display, not just that page.

**`kiln_io_lcd_dc()`/`kiln_io_lcd_reset()` are explicitly NOT routed
through `kiln_io_owner`.** These are `ILI9488.c`'s own hot path, called
once per display command from `lvgl_port_task`; `SX1509.c`'s internal
mutex already makes them safe to interleave with everything else at the
I2C-transaction level, and adding a task hop here would cost latency on the
single most latency-sensitive call in the display driver for no
correctness benefit (`kiln_io_owner.h:55-60`).

**SPI/I2C bus owner serialization underneath everything.** `esp_spi_owner`
and `i2c_owner` are the actual bus owners; `kiln_io_owner`/`thermo_owner`
sit on top of them, not in place of them. `thermo_owner.h`'s own header
comment is explicit that it is **not** fixing a live race the way Phase 1
was: `MAX31856.c`'s `ch->lock` already serializes each channel's whole
multi-transfer sequence end to end, so two callers racing on the same
channel were already correctly serialized before `thermo_owner` existed.
Phase 2 buys architectural consistency (one owner task per hardware
subsystem, matching `kiln_io_owner` and `SaftyFW`'s `relay_owner`) and a
choke point for Phase 6 (§3) — not a bug fix.

---

## 3. What's still planned, not built

### Phase 6 — system-mode command gate

Not started; design sketch only, recorded in `TODO.md` §10.14 and the
approved plan. Distinct question from single-writer ownership: not "can two
writers race on this state" (which the owner tasks already answer) but "is
this *class* of command allowed at all, given what the system is doing
right now" — e.g. starting a *different* profile while one is already
firing should be refused outright, not merely raced-and-serialized.

Can't live inside `kiln_io_owner`/`thermo_owner` — needs to see
`profile_executor`'s and `autotune_engine`'s run state, which those owners
have no business knowing about. Sketched as a policy layer (`system_mode.c`/
`.h`, name TBD) consulted by every producer-facing entry point (HTTP
handlers, LCD callbacks, UART bridge dispatch) *before* a command is even
built:

| System mode | Stop/pause/resume current run | Start a different profile | Run PID autotune | Debug GPIO/SX1509 write via UART |
|---|---|---|---|---|
| Idle | n/a | allowed | allowed | allowed |
| Profile running | allowed | refused | refused | refused (reads still allowed) |
| Autotune running | allowed (abort) | refused | n/a (already running) | refused (reads still allowed) |

Open design questions per the plan: where in the call chain it's checked
(once, at the HTTP/LCD/UART producer boundary, so the policy lives in one
place), and how "reads still allowed" is drawn precisely for SX1509 raw
commands. Deliberately not built ahead of a real caller needing it.

### `ui_page_network.c`'s three ad-hoc job structs

`scan_job_t`, `mode_job_t`, `connect_job_t` (**STALE citation corrected
2026-09-04**, consistent with §1's table note on the same split:
`mode_job_t` is `App/drivers/ui/ui_page_network.c:244`; `scan_job_t`/
`connect_job_t` moved to `App/drivers/ui/ui_page_network_manage.c:89`/`:179` —
`wifi_mode_ui`/`wifi_scan_ui`/`wifi_connect_ui` tasks in §1's table) were
each added as a same-session emergency fix for the LVGL-freeze bug, each
its own worker-task-plus-mutex pair, polled from `refresh_cb()`. Now that
`wifi_prov_owner` exists with a real queue and request/response producer
shape (§2), these three should migrate onto it and the page-local
duplication should be deleted — named as remaining work in Phase 4's
`TODO.md` entry, not done yet. `forget_confirm_yes_cb()`'s
`wifi_prov_forget_network()` call was deliberately left synchronous
(NVS-only, no radio, lowest risk of the four call sites on that page).

### Remaining Phase 5 (HTTP handlers) notes

Phase 5 was "move each HTTP handler onto its owner's queue as that owner
lands," not a separate phase — it landed piecemeal with Phases 1/2/4:

- `dashboard_http.c`'s relay handler and `profile_exec_*_post_handler`s
  already call into `kiln_io_owner`/`profile_executor`'s existing
  synchronous API — no separate queue-migration work outstanding there.
- `wifi_provision_http.c` still needs to be confirmed as fully migrated
  onto `wifi_prov_owner`'s queue now that Phase 4 is done (not re-verified
  as part of this document).
- **`display_uart_bridge`** (`uart_bridge.c`'s `display_bridge_task`, DISPLAY
  command family) was confirmed dead code — `main.c` never called
  `uart_bridge_start_display_task()` — and removed 2026-08-27, along with the
  twelve-plus stale `display_*` MCP tools on the PC side. Replaced by the
  on-device LVGL UI. `DISPLAY_CMD_*`/`UART_TASK_ID_DISPLAY` stay defined in
  `uart_task_ids.h` as wire-protocol constants: kilnctrl's `gui.py`/
  `actions.py` Display panel still speaks them (a separate, still-live front
  end left out of that cleanup), even though nothing on the firmware side
  answers. See TODO.md section 10.1.
- **OTA handlers** (`ota_http.c`) deliberately stay on the shared HTTP
  worker — they hold a streaming request body open across a whole
  transfer, which is a reason to keep them there by design, not a gap.
- **SAFETY/SYSTEM UART bridge tasks** deliberately not queued — SAFETY
  commands are cache reads or already fire-and-forget to the isolated
  link; SYSTEM's `FACTORY_RESET` reboots immediately after, so blocking
  briefly doesn't matter.

### Phase 3 — deliberately skipped, not deferred

`profile_executor_run()`/`_halt()`/`_pause()`/`_resume()` were read in full
during the Phase 3 research pass and already wrap their entire bodies in
`s_exec.lock` (**STALE citation corrected 2026-09-04**: `profile_executor.c`
has since split — `profile_executor_run()` is now
`App/drivers/control/profile_executor_run.c:55`, `_halt()`/`_pause()`/`_resume()` are
`App/drivers/control/profile_executor_status.c:21`/`:119`/`:164`; all four still take
`s_exec.lock` around their body per those files' own `xSemaphoreTake`s) — a
correct mutex-guarded API with no uncoordinated-writer bug of the kind
Phase 1 fixed. Converting a safety-critical state machine to a queue buys
no safety and adds regression risk, and trades a lock that makes callers
wait for a queue that can drop. **Revisit only if Phase 6 needs a choke
point the lock can't provide.**

---

## 4. Bench and verification reality

Same honesty convention as the rest of this project (`ROADMAP.md`'s "what
'done' means", `SaftyFW/docs/ARCHITECTURE.md`'s completion checklist):
distinguish *built* from *boot-smoke-tested* from *hardware-verified*.

- **`kiln_io_owner` (Phase 1) — build-verified only.** `idf.py build` clean
  under `-Werror`. No relay/expander hardware attached to observe the
  ownership/safety-fault gate actually refuse a live command.
- **`thermo_owner` (Phase 2) — build-verified only, and cannot be
  bench-verified even once other hardware is attached**, because the
  thermocouple daughterboard itself is not connected on this dev machine's
  board — a narrower gap than "no board at all."
- **`wifi_prov_owner` (Phase 4) — build-verified AND partially
  boot-smoke-tested.** Flashed over JTAG with OpenOCD: programmed and
  verified OK, board reset, core confirmed running (not panic-halted), no
  reboot loop. **Wi-Fi behavior itself was NOT observed** — the bench
  board's PC UART link is physically broken, and that link carries both
  the console log and the Wi-Fi status query, so only one boot log line
  was recoverable. AP fallback, join, rescan cadence, and multi-client
  contention on the owner all remain unverified.
- **PC↔ESP UART link — physically dead** on this bench board. Affects
  every UART-bridge task in §1's table and both owner-task verification
  notes above.
- **Pi↔ESP UART link — physically dead**, separately.
- **Thermocouple ICs — absent** (no daughterboard attached).
- **Display — attached and working.** `lvgl_port_task` and the on-device
  UI have real hardware to run against; this is the one piece of the stack
  actually exercised live this session (the LVGL-freeze bug that started
  §10.14 was found and fixed on real hardware).
- **Relay/SX1509 expander — not confirmed attached this pass**; treat
  `kiln_io_owner`'s verification note above as the current truth.

Only the bare ESP32-S3 board plus display is wired up in this environment
as of 2026-08-19 — no thermocouple daughterboard, no confirmed relay
expander, no safety RP2040, both UART links dead. Every claim above that
says "hardware-verified" means verified against *that* subset, not the
full system.

---

## 5. How to add a new owner

Distilled from the `kiln_io_owner`/`thermo_owner` precedents
(`App/drivers/owners/kiln_io_owner.c`, `App/drivers/owners/thermo_owner.c`) and
`firmware/SaftyFW/src/tasks/relay_owner.c`, the pattern both were built to
match.

1. **New file**, `App/drivers/<name>_owner.{c,h}`. Header comment states:
   what this owns, which callers raced on it before (or, if this is a
   Phase-2-style consistency pass rather than a race fix, say so plainly —
   don't invent a race-fix motivation the code doesn't have).
2. **Command struct + enum**: `<name>_owner_cmd_type_t` +
   `<name>_owner_cmd_t`, one variant per verb, fields commented
   `/* CMD_X only */` where they don't apply to every variant.
3. **Bounded queue**, `xQueueCreate(<N>, sizeof(cmd_t))` with `N` small (8
   is this codebase's current choice for both existing owners) — "a
   backlog here means something is wrong upstream," not a buffer to size
   generously.
4. **Owner task**: `xTaskCreatePinnedToCore(owner_task, "<name>_owner",
   4096, NULL, 5, NULL, tskNO_AFFINITY)` — priority 5 matches every other
   owner/control task in §1's table; `tskNO_AFFINITY` matches every task in
   this codebase (core pinning is a `SaftyFW`/RP2040-SMP idea, not used
   here). `xQueueReceive` with a bounded timeout if the task also has
   periodic housekeeping duties, `portMAX_DELAY` if it's purely
   command-driven (`kiln_io_owner`/`thermo_owner` both use
   `portMAX_DELAY` today — neither has periodic work of its own).
5. **`post_and_wait()` helper**: `xQueueSend(queue, &cmd, 0)` (never blocks
   the caller — 0 ticks), then `xSemaphoreTake(slot->sem, pdMS_TO_TICKS(200))`
   on a semaphore from the module's own **static, module-owned slot pool** —
   see §3's "Result/semaphore storage" note and `App/drivers/
   owner_slot_pool.h` for why it must NOT be a semaphore (or result struct)
   the caller allocates on its own stack: a producer that gives up after
   the bounded wait can return, freeing that stack frame, while the owner
   task is still going to write through pointers into it — exactly the bug
   fixed 2026-08-24 in `kiln_io_owner.c`/`thermo_owner.c`. Every new owner
   task must use `owner_slot_pool_alloc()`/`_release()` (or an equivalent
   two-sided release protocol) the same way. A timeout is a real failure,
   reported exactly like an I/O error — fail closed, never silently assume
   success.
6. **Producer functions**, one `<name>_owner_command_<verb>()` per verb,
   with the caller's existing signature preserved wherever possible so
   call sites don't change shape (both existing owners achieved zero or
   near-zero caller-file edits this way). If the module needs both a gated
   (MANUAL) and an already-authorized (AUTHORIZED) producer family
   because some caller already proved ownership/safety upstream, say so
   explicitly in the header, matching `kiln_io_owner.h`'s split.
7. **Fail closed if `kiln_io_owner_start()`-equivalent wasn't called or
   failed**: every producer checks for a NULL queue and returns a
   TIMEOUT/ERR_IO_FAIL-flavored result, never silently no-ops.
8. **`main.c` boot ordering**: start the owner task *before* anything that
   can issue a command through it (mirrors `kiln_io_owner_start()`'s
   placement in `main.c`, right after the underlying hardware handle comes
   up). Don't let a failure to start clear an unrelated `*_ready` flag that
   a fail-safe direct-call path (§2's exceptions) depends on.
9. **`App/drivers/CMakeLists.txt`**: add `"<name>_owner.c"` to the
   `idf_component_register(SRCS ...)` list (explicit file list in this
   project, not a glob — see the existing `"kiln_io_owner.c"` entry).
10. **Verify**: `idf.py -C firmware/KilnFW build` clean under `-Werror`.
    State plainly in `TODO.md` whether this was also flashed/boot-tested,
    and never claim hardware verification of behavior this environment
    can't observe (§4) — match the phrasing convention there exactly.
