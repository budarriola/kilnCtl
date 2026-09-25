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

**Status (2026-09-25):** implemented in worktree `wifiw1`. Both parts of this
slice landed together:

- `owner_task()` (`wifi_prov.c`) now replies via a reply-slot pool (see
  below) BEFORE calling `start_sta_join()`. `do_add_network()`/
  `do_set_mode()` set an `out_join_after_reply` flag instead of calling
  `start_sta_join()` themselves; the owner calls it after replying, still on
  the same task (single writer preserved). The producers'
  (`wifi_prov_add_network()`/`wifi_prov_set_mode()`) wait dropped from
  `WIFI_OWNER_SCAN_WAIT_MS` (15000) to `WIFI_OWNER_WAIT_MS`.
- The dead-stack-frame hazard is fixed with a fixed pool of module-owned
  reply slots (`s_reply_slots[WIFI_REPLY_SLOT_COUNT]`, sized to
  `WIFI_OWNER_QUEUE_LEN`), each with its own semaphore, generation counter,
  and `abandoned` flag, guarded by one pool mutex. Producer:
  `claim_reply_slot()` before posting, `free_reply_slot()` on a normal
  round-trip or a queue-send failure, `abandon_reply_slot()` on timeout
  (marks abandoned, does NOT free or bump generation -- the owner may still
  be mid-flight). Owner: `owner_reply()` holds the lock for its whole
  decision -- a generation mismatch or an abandoned slot means it recycles
  the slot itself (bump generation, clear in_use/abandoned) and never
  writes into `.result` or signals the semaphore; otherwise it writes the
  result and signals outside the lock. This is the "reset one side of a
  pair" class from CLAUDE.md: freeing/generation-bumping authority is split
  cleanly so neither side reuses state the other still depends on.
- Post-reply join failure observability is unchanged: `start_sta_join()`'s
  own `ESP_LOGE`/state-transition side effects were never contingent on
  running before vs. after the reply, only the order relative to the
  producer's wait changed.
- Client-visible contract unchanged: no new routes, no 202, same
  `{"ok":...}` bodies, `route_tier_table.h` untouched.
- Host tests: `test_wifi_prov.c` gained
  `test_reply_slot_normal_roundtrip()`,
  `test_reply_slot_abandon_then_owner_recycles()`, and
  `test_reply_slot_stale_generation_never_matches_after_reuse()`, run against
  the real `s_reply_slots`/`claim_reply_slot()`/`owner_reply()` internals
  (this file `#include`s `wifi_prov.c` directly, the repo's usual pattern for
  reaching `static` internals). Negative-tested by disabling the
  generation-mismatch guard in `owner_reply()`: the
  stale-generation test failed as expected (only that test), the guard was
  restored by hand, and a forced full host-test rebuild via
  `run_all_checks.ps1` confirmed a clean pass with no poisoned binaries.
- **Pending bench verification** (not done from this worktree, per task
  scope): provision over the AP, a `mode=home` round-trip, and confirming
  `GET /api/status` stays responsive during the join.

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

### A1 -- Shared async-job helper, first user `ct_auto_zero` (shape B). HIGH value

- **Handler:** `ct_auto_zero_post_handler()` (`safety_cfg_http.c:1405`). It
  polls the Pico every 200 ms for up to `SAFETY_CT_AUTO_ZERO_TIMEOUT_MS` =
  15000 (`safety_cfg_http.c:1302-1303,1523-1546`). A normal run takes about
  10-12 s. During that time, httpd serves nothing else.
- **New module:** `drivers/http/http_async_job.c/.h`, exposing
  `http_async_job_submit(req, fn, ctx, ctx_size)`. The helper allows one
  job at a time across all users. Each job runs on a one-shot task created
  with plain `xTaskCreate` (an **internal-RAM stack**, because the job
  commits calibration). The task is named `http_async_job`, registered with
  `stack_margin_register()` and tagged `# liveness: on-demand` in
  `tools/check_stack_margin_registration.ps1`'s `$requiredNames`, following
  the `ota_pico_rollback` pattern (`ota_http_pico.c:586,608`). Start at 4096
  B. Measure with `get_stack_margin` after a real run, and raise it if
  needed (stack bumps are pre-authorized). The job always ends with
  `httpd_req_async_handler_complete()`, including on every error path.
- **Why a one-shot task, not a permanent owner:** every user of this helper
  is a rare commissioning or maintenance action. A permanent task would
  keep 4-8 KB of internal heap allocated at all times for nothing. The stack
  comes from the heap, so `.dram0.bss` grows only by the few static handles.
- **Why not reuse `bx_flash_worker`:** it serializes every cfg_fs write, and
  a 15 s job on it would stall those writes. The job task may still
  *dispatch* cfg_fs writes onto `bx_flash_worker`. That is not a re-entry,
  because the job runs on a different task (`check_flash_worker_lint.ps1`).
- **Response contract:** unchanged. Every current `{"ok":...}` body and
  status is sent from the job instead. When the helper is busy, the handler
  replies synchronously, before `async_handler_begin`, with the refusal
  shape this route already uses: `200 {"ok":false,"reason":"another
  commissioning operation is running"}`. This needs no new client code.
  Keep reading the body, checking auth and checking preconditions on
  `httpd_worker` before the handoff. Put everything the job needs into its
  ctx. The job must not call `http_auth_*`, cookie or client-IP functions.
- **What changes:** today, httpd itself stops any other HTTP handler from
  running while a measurement is in progress. That implicit serialization
  goes away. The handler's re-check after the measurement
  (`safety_cfg_http.c:1552`) already handles a relay, profile or autotune
  starting mid-measurement from the LCD or UART, so it covers HTTP too. The
  implementer must confirm this by reading the code, not assume it.
- **Lock order:** unchanged. The job calls `profile_executor_get_status()`
  and `autotune_engine_is_active()` one after the other, each taking its own
  lock briefly. It must never hold one lock while calling the other
  (`s_exec.lock` before `s_at.lock`).
- **Host test:** put the helper's admission logic (idle/busy/refuse, and the
  rule that every submit is paired with exactly one complete) in a pure
  function. Add `httpd_req_async_handler_begin/complete` to
  `App/test/stubs/esp_http_server.h` and write a host test that checks
  begin/complete stay paired on every path, including the busy path and a
  failure to create the task. Include a negative test: drop one `complete`
  and confirm the test fails.
- **Risk:** medium. A job that never completes leaks a socket
  (`httpd_accept_conn: error in accept (23)`), so the job's bounded timeout
  is also the bound on how long the socket is held.

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
