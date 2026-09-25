# HTTP POST handlers off the httpd worker -- plan

**Status: PLANNED, 2026-09-25. Owner approved the migration the same day.**
Covers `firmware/KilnFW/TODO.md` section 10.14's open "Web side" item
("action-taking POST handlers should post commands instead of running inline
on `esp_http_server`'s one shared worker task"). Phase 5
(`docs/HTTP_HANDLER_OWNERSHIP.md`) is a different job and already closed: it
moved direct driver **reads** onto `thermo_owner`/`kiln_io_owner`. Nothing
below repeats that work.

Surveyed at `origin/main` `3543f530`: 78 `HTTP_POST` registrations (77 under
`drivers/http/`, plus `sim/sim_backend.c`). The TODO line names
`rules_http.c`, which no longer exists. It was removed with the rules engine
in `56dfa07f`.

## Why only a few handlers are worth moving

`esp_http_server` runs every handler on one task (`httpd_worker`, 8192 B
internal stack, `wifi_provision_http.c:1151`). While a handler blocks, every
other request waits, including the dashboard's `GET /api/status` poll and
the Stop button. Moving a handler is only worth it if it blocks for about a
second or more. A 50-300 ms NVS write is not, because the migration would
add more risk than it removes.

Moving the work onto an owner task and then **waiting** for it
synchronously does not help, because `httpd_worker` stays blocked for the
whole wait. There are two shapes that do help:

- **(A) Reply before the slow tail.** The owner finishes the part that
  decides the result, replies, and then does the slow part. The HTTP handler
  does not change. This fits only when the result does not depend on the
  slow part.
- **(B) Hand the request itself to a job task.** Use
  `httpd_req_async_handler_begin()`/`httpd_req_async_handler_complete()`
  (ESP-IDF 6.0.2, declared in the IDF component's `esp_http_server.h`; example in
  `examples/protocols/http_server/async_handlers`). The handler validates and
  parses on `httpd_worker`, copies the request, and returns. A job task then
  does the slow work and sends **the same status and body the handler sends
  today**, so clients see no difference. The copy duplicates `httpd_req_t`,
  including `user_ctx`, the header scratch buffer and the response headers
  (`httpd_txrx.c:657-708`), using ordinary `malloc`, not `.bss`. The
  session is flagged `for_async_req`, and LRU purge skips it
  (`httpd_sess.c:91`). So every job holds one of the 13 sockets
  (`max_open_sockets`) until it completes, and the job's time bound matters.

A 202-plus-status-poll contract is **not** proposed anywhere. It would change
the API that PcTools and the web pages use. `kiln_configs/apply` already
uses 202 and poll, and it stays that way.

## Already done or already deferred (do not migrate again)

| Route(s) | Existing mechanism |
|---|---|
| `POST /api/kiln_configs/apply` | Dispatched to `kiln_cfg_swap_apply()`'s worker. Status comes from `GET /api/kiln_configs/apply_status` (`kiln_cfg_http.c:380`) |
| `POST /api/ota/esp/rollback` | One-shot `ota_rollback_reboot` task (`ota_http_esp.c:750`) |
| `POST /api/ota/pico/rollback` | One-shot `ota_pico_rollback` task plus async status (`ota_http_pico.c:586`) |
| `POST /api/ota/esp/recovery_exit` | One-shot `recovery_exit_reboot` task (`ota_http_recovery.c:170`) |
| `POST /api/sw_reset`, `POST /api/factory_reset` | One-shot reboot tasks (`sw_reset_http.c:438`, `factory_reset.c:356`). The board reboots, so a free worker gains nothing |
| `POST /api/ota/esp`, `POST /api/ota/pico` (transfers) | **Out of scope** by TODO.md's own text. They stream the body for the whole transfer |
| `/api/profile_exec/*`, `/api/autotune/*`, `/api/zones/current_sweep/*` | The handler only starts or stops work that runs on the executor, autotune or sweep task. They return quickly. Phase 3 (an executor command queue) was **deliberately skipped** (TODO.md 10.14) |
| `POST /api/diagnostics/danger/relay` | Already goes through `kiln_io_owner_command_set_relay()` (`dashboard_http.c:716`) |
| `POST /api/cfgfs/format_confirm`, all cfg_fs writes | Already dispatched to `bx_flash_worker` (`cfg_fs_mount.c:576-591`). For a flash partition, `lfs_format` erases only the superblock, so this is fast |
| Every `wifi_provision_http.c` POST | Already goes through the `wifi_prov_owner` queue (Phase 4). But the handler still **waits** up to 15 s (see slice W1) |

## Slices, in value order

Each slice is one reviewable commit. Everything in "Constraints" below
applies to every slice.

### W1 -- Wi-Fi: reply before the scan-and-join (shape A). HIGHEST value -- IMPLEMENTED, pending bench verification

**Status (2026-09-25):** landed; bench verification pending. Review-fixed same
day (Opus review of `8f9ae877`). `owner_task()` replies via a reply-slot pool
BEFORE calling `start_sta_join()`; `do_add_network()`/`do_set_mode()` set
`out_join_after_reply` instead of joining themselves, and now also set
`s_wifi.state = WIFI_PROV_STATE_CONNECTING` before returning, so a client
polling `GET /api/status` right after `"ok"` never observes a pre-join state.
The reply-slot pool's `owner_reply()` writes the result, sets a `replied`
flag, and gives the semaphore all under one pool-mutex acquisition (giving a
FreeRTOS semaphore never blocks, so this is legal); the producer's timeout
path (`abandon_or_free_reply_slot()`) makes one locked decision: if the reply
already landed, copy the result out and free the slot (late success); else
mark abandoned for the owner to recycle. `s_reply_results[]` (the actual
`wifi_result_t` payloads) lives in `EXT_RAM_BSS_ATTR` (PSRAM), not inline in
the slot struct, to keep `.dram0.bss` under budget -- measured
`.dram0.bss = 99592 B` against the 101000 B ceiling (1408 B headroom),
`check_kilnfw_dram_bss_budget.ps1` on a fresh, non `-Fast` target build.
`ensure_reply_pool_init()`
is called once, synchronously, from `wifi_prov_start()` before the queue/task
exist, removing a cross-core lazy-init race.

**Pending:** bench verification (not done from this worktree, per task
scope) -- provision over the AP, a `mode=home` round-trip, and confirming
`GET /api/status` stays responsive and reports `CONNECTING` during the join.

- **Handlers:** `provision_post_handler()` for station credentials and for
  `mode=home` (`wifi_provision_http.c:655`). The same fix also frees
  `uart_bridge_ext_wifi.c` and `ui_page_network*.c`, because they call the
  same producers.
- **Owner:** the existing `wifi_prov_owner` (`wifi_prov.c:426`, 4096 B
  internal stack). No new task is needed.
- **What blocks today:** `wifi_prov_add_network()` and
  `wifi_prov_set_mode(HOME)` wait `WIFI_OWNER_SCAN_WAIT_MS` = 15000
  (`wifi_prov_api.c:36,135,291`). The owner runs `do_add_network()` →
  `start_sta_join()` → `select_and_apply_join_candidate()` → `do_scan()`,
  which is a real blocking scan (`wifi_prov_link.c:204-207`), before it
  replies.
- **Change:** in `owner_task()` (`wifi_prov.c:507-585`), `do_add_network()`
  and `do_set_mode()` stop calling `start_sta_join()` themselves. Each one
  instead sets a local `join_after_reply` flag. The owner then gives
  `cmd.done`, and only after that calls `start_sta_join()`. This stays on
  the same task, so there is still a single writer. The result (`ESP_OK`,
  or `ESP_ERR_NO_MEM` when the list is full) is already decided before the
  join starts, so the HTTP contract (`"ok"` or a 400 with the same text) does
  not change. The producers' wait can drop to `WIFI_OWNER_WAIT_MS`.
- **Latent bug to fix in the same slice (inferred from reading the code, not
  seen on hardware):** `wifi_prov_post_and_wait()` keeps `done` and
  `result` on the producer's stack (`wifi_prov.c:141-161`). On timeout it
  returns, but the owner still writes `*r` and gives `cmd.done` later
  (`wifi_prov.c:585`). That is a write into a dead stack frame. W1 makes a
  timeout much less likely. It does not remove the hazard. Before choosing a
  fix, compare with `uart_owner_transfer()`/`i2c_owner_transfer()`, which
  the code comment cites. One option is a per-command sequence number that
  the owner checks against an "abandoned" slot before it writes.
- **Side benefit:** when a phone submits credentials over the fallback AP,
  it now gets `"ok"` before the radio changes mode or channel.
- **Host test:** none today (`wifi_prov*` links only into the target build).
  Verify with a target build plus bench checks: provision over the AP, a
  `mode=home` round-trip, and `GET /api/status` staying responsive during
  the join.
- **Risk:** low to medium. Event ordering: a Wi-Fi event queued before the
  reply is now handled after `start_sta_join()`, which matches what happens
  today.

### A1 -- Shared async-job helper, first user `ct_auto_zero` (shape B). LANDED 2026-09-25

`drivers/http/http_async_job.c/.h` (`http_async_job_try_start()`/
`http_async_job_busy()`) wraps `httpd_req_async_handler_begin()`/
`_complete()` behind a one-job-at-a-time admission gate (a `portMUX_TYPE`
critical section, not a mutex -- the guarded window is a handful of
instructions). `ct_auto_zero_post_handler()` (`safety_cfg_http.c`) now
validates/checks preconditions on `httpd_worker`, then hands off to a new
`ct_auto_zero_job()` running on a one-shot `xTaskCreate` task
(`http_async_job`, internal-RAM stack, raised 4096 -> 6144 B in the
2026-09-25 fix-then-push review once the real resolved depth was measured
(see below), `stack_margin_register()`ed unconditionally and tagged
`# liveness: on-demand` in `tools/check_stack_margin_registration.ps1`).
Every existing body and status is unchanged from the old inline handler,
plus one new synchronous refusal for a concurrent second POST: the
`{"ok":false,"reason":"another commissioning operation is running"}` busy
reply is NEW as of A1, not something this route already sent -- pre-A1 a
second concurrent POST simply ran inline behind the first on `httpd_worker`,
since no busy state existed yet to refuse it with. It is sent on the
original `req` (the helper never touches `req` on any refusal path).
`http_async_job_try_start()` also distinguishes this ordinary busy
contention from a resource failure (`begin()` or `xTaskCreate()` failing) --
the caller replies busy only for real contention, and its own existing 500
"out of memory" reply for a resource failure, never folding the two into one
boolean (2026-09-25 fix-then-push review). Host-test stubs
(`App/test/stubs/esp_http_server.h`, `stubs/freertos/task.h`) gained
injectable `begin`/task-create failure and a `complete()` call counter;
`test_http_async_job.c` covers admission, busy refusal, begin failure,
task-create failure (undoes `begin` via one `complete()`), begin/complete
pairing through `run_job()` directly, and (2026-09-25 review) that
`run_job()` clears `s_task_handle` atomically with `s_busy` rather than
leaving it for a separate trampoline step -- negative-tested by dropping the
`complete()` call, confirming the new test failed, and restoring by hand
with a forced full rebuild (61/61 executables). A post-rebase target build
(2026-09-25 fix-then-push re-review) produced a fresh ELF:
`check_kilnfw_dram_bss_budget.py` measures `.dram0.bss` at 99624 B against
the 101000 B ceiling (1376 B headroom) -- this figure moves with every pass
that touches file-scope state anywhere in the image, so re-measure rather
than trusting either this number or the prior pass's 98952 B going forward.
This helper's
added file-scope state (a `portMUX_TYPE`, a `bool`, one `TaskHandle_t`, one
small run-context struct) is a few dozen bytes of that total, not measured
in isolation. `http_async_job.c` was also missing from
`drivers/CMakeLists.txt`'s SRCS list until this pass (caught by the full
`run_all_checks.ps1` run, not by host tests, since the host-test harness
links it separately -- the real target build failed at link time until this
was added).
2026-09-25 fix-then-push review: the original landing's stack accounting was
wrong on two counts. `check_all_task_stack_budgets.py`'s static ELF walk
reported this task INDETERMINATE at a 32 B lower bound only, because
`run_job()` dispatches through a function pointer (`http_async_job_fn_t`)
the walk cannot follow on its own -- and the `CEILING_BYTES` entry that
landed (2736 B) was borrowed from `ota_pico_rollback` on the assumption the
two tasks were a comparable shape. Both were wrong: `ota_pico_rollback`'s
own resolved depth is only 1888 B, not a valid stand-in either way, and the
real chain rooted at `ct_auto_zero_job` (`ct_auto_zero_job` ->
`safety_cfg_write_apply_pairs` -> `apply_pairs_ex` ->
`safety_cfg_store_refetch_locked` -> `safety_link_get_config_page`, a
1024 B frame, -> `uart_protocol_send_broadcast` -> `frame_and_send`) is
3280 B, not ~2700 B. `ct_auto_zero_job` is now wired in as an `extra_roots`
entry (same technique as `bx_flash_worker`'s enumerated dispatch-target
list) so the checker measures and grades this task's real depth: 3312 B
total (32 B trampoline + 3280 B), against the ceiling raised to match --
negative-tested by setting the ceiling one byte below 3312, confirming the
checker FAILed, then restoring 3312 by hand. The 6144 B stack (up from
4096 B) accounts for that 3312 B plus headroom for an `ESP_LOG` call through
`uart_log_vprintf` (~1344 B: `esp_log_write` ~128 B + `uart_log_vprintf`
~736 B + `vsnprintf` ~480 B) that the earlier accounting had not included.
`stack_margin_register()` is called with a fixed literal `"http_async_job"`,
not the caller-supplied task name, since this helper is single-flight (one
shared handle slot regardless of which future caller's job is running) and
`check_stack_margin_registration.ps1`'s static scan requires a literal name
argument to see a call site at all.

### A2 -- `bench_preset` onto A1's helper. MEDIUM-HIGH value

- **Handler:** `bench_preset_post_handler()` (`safety_cfg_http.c:1755`). It
  makes 32 `safety_link_send_set_param()` calls and then one
  `safety_link_send_commit_config()` (`:1719-1776`). Each call takes
  `xact_lock` with a 5000 ms timeout (`SAFETY_XACT_LOCK_TIMEOUT_MS`), so
  the worst case with a slow link is tens of seconds.
- **Interleaving audit, required:** today httpd guarantees that no other
  handler writes safety config between the 32 SET_PARAMs and the COMMIT. The
  POST handlers for `commissioning`, `relay_type`, `ct_cal`, `ct_trim` and
  `rate_guard/auto` (same file) must refuse while
  `http_async_job_busy()` is true, using the same `ok:false` shape. Without
  that refusal, a mixed parameter set could be committed. The LCD and UART
  paths already interleave today and are not made worse. Check whether they
  commit mid-sequence, and record the answer in the commit.
- Everything else is as in A1. The body is small, and the job's stack needs
  are similar.
- **Risk:** medium, because of the interleaving audit.

### A3 -- `crash_report/clear` onto A1's helper. CONDITIONAL, measure first

- `crash_report_clear()` erases the whole 1 MiB `coredump` partition
  (`partitions.csv`) through `hal_sysinfo_coredump_erase`
  (`crash_report.c:754`). It also writes NVS, so an internal stack is
  required. Before writing any code, time one clear on the bench from the
  log timestamps. If it takes under about 1 s, **drop this slice**.
- The contract is the `{"ok":...}` body and 500 status as they are today.
  `crash_report_clear_http_client.py` reads the state back afterwards and
  does not change.

### A4 -- `backup/import` onto A1's helper. CONDITIONAL, measure first

- `backup_import_post_handler()` receives the body and then applies up to
  100 profile slots, zones and kiln_configs through the stores' public save
  functions (`backup_import.c`). This could plausibly take several seconds.
  The job task would also receive the body (`httpd_req_recv` on the async
  copy is supported). The body buffer is already on the heap
  (`backup_import.c:77`). Time a full 100-slot import on the bench first. If
  it takes under about 2 s, **drop this slice**. This is the largest
  handler and the riskiest to move, so do it last.

## Not worth doing, with reasons

| Routes | Reason |
|---|---|
| `/api/auth/login`, `logout`, `bootstrap_password`, `security`, `forgot`, `reset`, `session/extend` | The KDF is 2000 rounds, about 0.4 s (`web_auth_store.h:85`). The auth path is security-sensitive, and the gain is small |
| `/api/zones`, `/api/zones/pid`, `/api/profile*` (save, delete, favorite, builtin hide/restore, import), `/api/profile/live*`, `/api/kiln_configs/{save,clone,delete,rename,import,quarantine_clear}` | Each is one short NVS or cfg_fs write on an internal stack, which is correct today (`zones_http_pid.c:69-87`). Moving them adds risk for well under 1 s saved |
| `/api/unit_pref`, `settings/tz`, `settings/display_power`, `setup/progress`, `watchdog_cfg`, `ramp_assist`, `relay_cycles/*`, `estop/verify`, `adaptive_tune/*`, `iter_tune/restore_commissioned`, `dualwrite_window/restore_verified`, `cfgfs/file`, `crash_report/ack`, `safety/commissioning*` except the two in A1/A2 | Short local writes |
| `/api/safety/clear_trip`, `/api/safety/log_level` | One link transaction each, a few hundred ms |
| `/api/ota/esp/boot_guard_reset` | One verified NVS clear. Short |
| `/forget`, `/ip_config` | Already go through the owner. No scan, only an NVS write and netif changes |
| `/api/sim` | Sim build only |

`GET /scan` and `GET /networks` also block for a scan
(`wifi_provision_http.c:378,426`). They are GETs, so they are outside this
TODO item. If the owner wants them handled, they could use A1's helper
later.

## Constraints (every slice)

- **Auth gate untouched.** `kiln_http_prehandler()` (`http_auth_http.c:273`)
  runs before the inner handler, so the gate has already passed when the
  job starts. `route_tier_table.h` gets no edits. This plan adds no routes.
- **URI cap unaffected.** There are no new routes, so
  `check_uri_handler_cap.ps1` stays at 160/170.
- **Never hold a module lock across producer calls.** A job calls the same
  public accessors the handler calls today, in the same order.
- **PSRAM-stacked tasks must not write NVS.** Every task in this plan has an
  internal stack: `wifi_prov_owner` already does, and `http_async_job` uses
  plain `xTaskCreate`. Never run a job on `profiles_task`, `control_task`,
  `autotune_task` or `wifi_uart_bridge`.
- **DRAM:** `.dram0.bss` ceiling is 101000 B
  (`App/test/check_kilnfw_dram_bss_budget.py:75`). The helper adds only a
  few static handles. Allocate job stacks and ctx from the heap.
- **Stack margin:** register every new task. `check_stack_margin_registration.ps1`
  and `check_task_liveness` must pass.
- **Host tests are not a target build.** HTTP handlers link only into the
  target build. Each slice needs `check_00_kilnfw_target_build.ps1` plus a
  bench check that `GET /api/status` keeps answering while the moved
  operation runs. That check is the actual goal of the work.
- **Negative test:** after each slice, temporarily move the work back inline
  and confirm the bench responsiveness check fails. Restore by hand, then do
  a **forced full rebuild** (CLAUDE.md's negative-test rule).

## When this is done

W1 and A1 close the TODO item. A2 is strongly recommended. A3 and A4 depend
on their bench measurements. When the chosen slices land, tick the TODO.md
10.14 "Web side" box, citing this plan, and rename this file without the
`_PLAN` suffix.
