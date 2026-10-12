# Flash-worker lock-inversion audit (2026-10-09)

Read-only audit of KilnFW on origin/dev `a7433500`. No code changed.

## The deadlock class

`uart_bridge_ext_run_on_flash_worker()` (`drivers/bridge/uart_bridge_ext.c`,
`bx_run_on_internal_stack()` around line 512) runs the job inline when the
caller is already on `bx_flash_worker`. Otherwise it takes `s_bx_lock`, posts
the job on the depth-1 queue `s_bx_jobs`, and waits on `s_bx_done`. Both waits
are `portMAX_DELAY`.

A deadlock needs three things:

1. Task A (not the worker) holds lock L.
2. While holding L, task A calls something that reaches the dispatch.
3. The job the worker is running at that moment takes L.

The worker is then blocked on L, and A is blocked on `s_bx_lock` or
`s_bx_done`. Neither lock nor the worker is ever released. Every caller of the
worker blocks next. That includes the PC UART bridge, because every UART
command handler runs on the worker.

UART command handlers that run entirely on the worker:

- `control_handle_message`, dispatched at `uart_bridge_ext_control.c:220`.
- `profiles_handle_message`, dispatched at `uart_bridge_ext_control.c:651`.
- `autotune_handle_message`, dispatched at `uart_bridge_ext_autotune.c:222`.

Any lock those handlers take is therefore a lock "acquired on the worker".

**Excluded.** These known instances are being fixed by another agent and are
not repeated here:

- `zcfg_save_lock` (`zones_config_store.c` `nvs_save`, `relay_names_save`, `zone_normals_save`).
- `profiles_save_lock` (the `nvs_save_slot` call sites in `profiles_http.c` and `profiles_edit_http.c`).
- `cfg_save_lock.h` users (`unit_pref.c`, `profiles_favorites`).

All paths below are relative to `firmware/KilnFW/App/`.

## Method

The call graph comes from grep, built by throwaway scripts that are not
committed:

1. Parse every non-test `.c` under `App/` into function bodies and call sites.
2. Compute the backward closure from `uart_bridge_ext_run_on_flash_worker` and
   `bx_run_on_internal_stack`: 269 functions that can reach a dispatch.
3. Compute the forward closure from the worker entry points (the three UART
   handlers, the posted `safety_link_poll.c:438` job, and every
   `*_job` function passed to `uart_bridge_ext_run_on_flash_worker`): 391
   functions that can run on the worker.
4. Linear-scan each function for a `xSemaphoreTake` or a lock-wrapper call
   that is held across a call into set 2.
5. Intersect the result with the locks taken by functions in set 3.
6. Verify every candidate by reading the code, and record the file:line of
   each edge.

Limits of the method:

- The graph is name-based. Same-named `static` functions in different files
  are merged, which produced some false positives (listed below).
- Calls through function pointers (hooks, job function pointers) were
  resolved by hand.
- The linear held-lock scan ignores control flow. Early-return gives were
  checked by hand.

The reverse direction was checked separately: a worker job that blocks on a
queue or semaphore given only by a task that may itself be waiting on the
worker.

## Findings, ranked

### F1 HIGH: `s_exec.lock` held across `relay_cycles` persist dispatch

**Fixed** in 4271767d (save mutex vs flash worker), which moved the call; the regression test came with the F2 commit:
`relay_cycles_maybe_persist()` now runs right after the RUNNING tick's
`xSemaphoreGive(s_exec.lock)`. It takes no argument and reads only `s_rc`
state, so nothing needed snapshotting. Host test
`test_relay_cycles_persist_runs_after_lock_give` (test_profile_executor_prestart.c)
drives one real RUNNING tick and fails if the persist is called with a lock held.

**Holder path (executor task):**

- `executor_task_entry` takes `s_exec.lock` at `drivers/control/profile_executor.c:737`
  on the RUNNING-tick path and gives it at `profile_executor.c:2053`.
- While holding it, it calls `relay_cycles_maybe_persist()` at `profile_executor.c:1970`.
- `relay_cycles_maybe_persist` calls `persist_snapshot_now(0)` (`drivers/persist/relay_cycles.c`, around line 1196).
- `persist_snapshot_now` dispatches `uart_bridge_ext_run_on_flash_worker(reset_persist_job)`
  at `relay_cycles.c:1162`.
- This path runs at most once every `RELAY_CYCLES_PERSIST_INTERVAL_S` (10 min),
  and only when the cycle counts are dirty. During a firing they are always
  dirty, so this dispatch happens every 10 minutes of every firing.

**Worker takers of `s_exec.lock`:**

| Worker path | Takes `s_exec.lock` at |
|---|---|
| `profiles_handle_message` -> `profile_executor_halt` (`uart_bridge_ext_control.c:568`) | `drivers/control/profile_executor_status.c:41` |
| `profile_executor_pause` (`uart_bridge_ext_control.c:573`) | `profile_executor_status.c:278` |
| `profile_executor_resume` | `profile_executor_status.c:330` |
| `profile_executor_run` (UART run command) | `drivers/control/profile_executor_run.c:433` |
| `autotune_handle_message` START -> `autotune_engine_run_relay` -> `autotune_begin_run_locked` -> `profile_executor_zone_is_active` (`autotune_engine.c:1175`) | `profile_executor_status.c:608` |
| `profile_executor_get_active_id` (UART status) | `profile_executor_status.c:644` |
| `fault_halt_run_halt_job` (dispatched at `profile_executor_status.c:259`) -> `profile_executor_halt` | `profile_executor_status.c:41` |

**Trigger.** A PC sends halt, pause, resume or status, or a fault-halt job is
running on the worker, at the moment the executor tick reaches the 10-minute
persist.

**Consequence:**

- The executor task is wedged forever, so guard 9 (control-tick liveness) fires.
- The PC UART is dead.
- Every other user of the worker blocks behind it, including the profile and
  zones saves.

The comment at `relay_cycles.c:1088-1135` says the executor tick "never
waits". That covers only the `persist_lock` try-take, not the worker dispatch
that follows it.

**Suggested fix (text only).** Set a flag under the lock and call
`relay_cycles_maybe_persist()` after the give at `profile_executor.c:2053`.
Alternatively, make the tick-path persist use the posted, fire-and-forget slot.

### F2 HIGH: `s_at.lock` held across the coupling persist dispatch

**Fixed** (same commit as F1): `autotune_finalize_fit()` parks the by-value
`coupling_persist_job_t` in `s_at.pending_coupling` instead of dispatching.
The tick helper `autotune_engine_tick_under_lock()` takes it under
`s_at.lock` and dispatches it after the give. One ordering change: the run's
DONE state is now visible to readers a moment before the coupling cells are
written (the write still happens in the same tick). Host tests
`test_finalize_fit_under_lock_does_not_dispatch_coupling_persist` and
`test_tick_under_lock_dispatches_coupling_persist_after_give`
(test_autotune_engine_prestart.c) use a worker stub that refuses to run when
a lock is held.

**Holder path (autotune task):**

- `task_entry` takes `s_at.lock` at `drivers/control/autotune_engine.c:854` and
  gives it at `autotune_engine.c:860`.
- Between the two it calls `autotune_engine_tick_locked_impl` (`autotune_engine.c:858`).
- That calls `autotune_finalize_fit()` at `autotune_engine.c:698` and `autotune_engine.c:726`.
- `autotune_finalize_fit` dispatches `uart_bridge_ext_run_on_flash_worker(coupling_persist_job)`
  at `drivers/control/autotune_engine_step_identify.c:525`. There is no give in
  `autotune_engine_step_identify.c`, so the lock is held across the dispatch.

**Worker takers of `s_at.lock`:**

| Worker path (`autotune_handle_message`) | Takes `s_at.lock` at |
|---|---|
| `AUTOTUNE_CMD_GET_STATUS` -> `autotune_build_status` (`uart_bridge_ext_autotune.c:126`, `:32`) -> `autotune_engine_get_status` | `autotune_engine.c:1668` |
| ABORT (`uart_bridge_ext_autotune.c:157`) | `drivers/control/autotune_engine_guard.c:274` |
| ACCEPT (`uart_bridge_ext_autotune.c:182`) | `autotune_engine_guard.c:324` |
| START -> `autotune_begin_run_locked` | `autotune_engine.c:1184` |
| `autotune_engine_is_active_on_zone` | `autotune_engine.c:1798` |
| `autotune_engine_release_zone_for_external_write` | `autotune_engine.c:1836` |

**Trigger.** A PC polling autotune status (the normal autotune workflow) while
a step-test fit finishes. The odds are high, because the PC polls throughout a
run.

**Consequence.** The autotune task and the worker are both wedged. Autotune
never finishes, and the UART is dead.

`coupling_persist_job` itself (which calls `zones_config_set_coupling_cell`)
does not take `s_at.lock`, so the job cannot deadlock on its own. The comment
at `autotune_engine_step_identify.c:521` ("Not reachable on-worker today")
covers only re-entrancy, not this inversion.

**Suggested fix (text only).** Capture the coupling-cell arguments under
`s_at.lock`, return them to `task_entry`, and dispatch after the give at
`autotune_engine.c:860`.

### F3 MEDIUM: `adaptive_tune_lock` held across zones saves -- FIXED (see "Fix" below)

**Holder path (executor task, after a run ends):**

- `adaptive_tune_run_end` takes `adaptive_tune_lock` at `drivers/control/adaptive_tune.c:634`
  and gives it at `adaptive_tune.c:785`.
- While holding it, it calls `adaptive_tune_refine_zone_locked` (`adaptive_tune.c:713`).
  That reaches `zones_config_set_autotune_baseline_k_dc` (`drivers/control/adaptive_tune_model.c:116`),
  `zones_config_set_model` (`adaptive_tune_model.c:225`) and `zones_config_set_pid` (`adaptive_tune_model.c:232`).
- It also calls `adaptive_tune_refine_coupled_locked` (`adaptive_tune.c:714`),
  which reaches `zones_config_set_coupling_cell` (`adaptive_tune_model.c:626`).
- Each of those setters ends in the zones `nvs_save`. That takes `zcfg_save_lock`
  (`drivers/persist/zones_config_store.c:829`) and dispatches at
  `zones_config_store.c:852` and `zones_config_store.c:895`.
- Off-worker caller: the executor task at `profile_executor.c:795`, on the
  DONE/FAULTED path with `clean=true`. `s_exec.lock` has already been given at
  `profile_executor.c:786`, so this finding does not involve F1.

**Worker taker.** The UART `AUTOTUNE_CMD_ACCEPT` handler calls
`autotune_engine_accept`, which calls `adaptive_tune_clear_ki_baseline`
(`autotune_engine_guard.c:506`). That takes `adaptive_tune_lock` at `adaptive_tune.c:1015`.

**Why the gate does not prevent it.** The system mode gate refuses zones and
config writes only while `profile_running` or `autotune_running`
(`drivers/safety/system_mode_gate.c:82-101`). A profile that has just reached
DONE does not count as running, so an autotune accept can coincide with
`run_end`. The window is narrow (one run end), hence MEDIUM.

The worker-side halt path (`profile_executor_status.c:126`) reaches `run_end`
only when `fs_need_persist` is set. That case runs inline on the worker, so it
is not an inversion.

**Interaction with the excluded `zcfg_save_lock` fix.** Fixing the zones save
mutex alone does not fix F3: the worker would still block on
`adaptive_tune_lock`.

**Suggested fix (text only).** Compute the refined values under
`adaptive_tune_lock`, release the lock, and then call the `zones_config_set_*`
setters.

**Fix (FIXED, SHA below).** `adaptive_tune_run_end` now runs three passes:
plan under the lock (`adaptive_tune_plan_zone_locked` /
`adaptive_tune_plan_coupled_locked`, no setter), release, apply the zones setters
(`adaptive_tune_apply_zone_plan` / `_apply_coupled_plan`), retake the lock and
commit the outcome (`_commit_*_locked`, ki refine, confidence, kibase snapshot).
`adaptive_tune_ki_clear_gen` (bumped by `adaptive_tune_clear_ki_baseline`) stops
the commit re-latching a SIMC Ki baseline that an Accept in the unlocked window
made stale. Host test `test_run_end_holds_no_lock_across_zones_setters`
(plus a check in the coupled-cell test) fails if a setter sees a held lock;
negtest (removing the unlock) CAUGHT.

**Follow-up review of the fix (FIXED in the commit that added this paragraph).**
Dropping the lock opened a window that the revert path could enter. The executor
calls `run_end` with its state already DONE/FAULTED, so `adaptive_tune_revert`
(HTTP) is not refused. The plan pass captures the revert snapshot
(`revert_available=true`) before the setters run, which made two bad outcomes
possible:

- A revert landing between plan and apply, or between `set_model` and `set_pid`,
  reported OK. Then `run_end`'s own setters overwrote the restore, and the commit
  marked the change applied and latched its SIMC Ki baseline.
- A `run_end` landing inside a revert's own unlocked write left its model behind.

The fix adds a per-zone `write_in_flight` flag (`adaptive_tune_internal.h`):

- `run_end` sets it from plan to commit, and `adaptive_tune_revert` sets it for its write.
- While `run_end` holds it, a revert refuses with the new `ADAPTIVE_TUNE_REVERT_BUSY`.
- `run_end` skips a zone that a revert is writing.

`adaptive_tune_ki_clear_gen` is now per zone, so an Accept on one zone no longer
suppresses another zone's re-latch. The gen guard previously had no test.

Host tests (`test_adaptive_tune_status.c`) land the competing call inside the
unlocked window through one-shot setter hooks:

- `test_revert_during_run_end_apply_is_refused_busy`
- `test_run_end_during_revert_write_skips_the_zone`
- `test_ki_clear_gen_is_per_zone`

negtest CAUGHT four mutations: no BUSY refusal, no `run_end` skip, global gen,
and gen guard removed.

Still open (pre-existing, narrower):

- A zones POST or an Accept that writes PID for the same zone inside the `run_end`
  apply window is a last-writer-wins race with the `run_end` setters. This existed
  before F3, as an ordering race outside the lock, and F3 only widens it.
- The revert snapshot survives an Accept, so a later revert restores the
  pre-adaptive gains over the accepted ones.

### F4 LOW (bounded): `s_rc.persist_lock` across the dispatch -- NOT CHANGED (by design)

**Disposition.** Re-checked on dev after F1: the executor no longer holds
`s_exec.lock` at the persist, so the only remaining cross-wait is
`persist_lock` (executor, held across its dispatch) against the worker's
`relay_cycles_flush` (bounded 3 s take). It is a bounded stall, not a
deadlock, and cannot be removed without breaking the ordering invariant:
`persist_lock` must bracket snapshot-through-dispatch-completion so an older
snapshot cannot be written after a newer one (see the comment above
`persist_snapshot_now`). A worker-side flush that skipped the wait and ran
inline would overtake the executor's already-queued older snapshot. The
failed flush leaves `dirty` set, so no counts are lost. Accepted; no code
change.

- `persist_snapshot_now` takes `persist_lock` (`relay_cycles.c:1140`) and
  holds it across the dispatch at `relay_cycles.c:1162`.
- On the worker, `profile_executor_halt` calls `relay_cycles_flush`
  (`profile_executor_status.c:151`), which calls `persist_snapshot_now` with
  `RELAY_CYCLES_FLUSH_LOCK_WAIT_MS` = 3000.
- The take is bounded, so the worst case is a 3 s worker stall and an
  `ESP_ERR_TIMEOUT` flush (counts persisted later), not a deadlock.
- This finding goes away if F1 is fixed by moving the persist off `s_exec.lock`.

### F5 LOW (boot only): `s_rc.lock` in `relay_cycles_init` -- FIXED

`relay_cycles_init` now loads (migrate, NVS read, `pref_cfg_fs_resolve`) into
locals and takes `s_rc.lock` only to publish counts/types/overrides/rev and
`initialized`. Host test `test_init_holds_no_lock_across_cfg_write`
(test_relay_cycles.c) fails if the cfg write runs with a lock held; negtest
(taking the lock before the load) CAUGHT. Original finding follows.

- `relay_cycles_init` holds `s_rc.lock` from `relay_cycles.c:343` to
  `relay_cycles.c:496`.
- During that time it calls `migrate` (`relay_cycles.c:351`) and
  `pref_cfg_fs_resolve` (`relay_cycles.c:477`). Both can write back through
  the cfg_fs write function, which dispatches at `drivers/persist/cfg_fs_mount.c:525`.

Worker takers of `s_rc.lock`:

- `relay_cycles_note_safety_edge` (`relay_cycles.c:532`), reached through
  `safety_drain_inbox` -> `safety_apply_status` (`drivers/safety/safety_link_frames.c:766`).
- `relay_cycles_set_type` (`relay_cycles.c:583`), reached through
  `zones_config_push_all_relay_types` (`zones_config_store.c:943`).

Boot order makes contention unlikely: `main.c:218` `main_control_bringup`
(`relay_cycles_init` at `main_control_bringup.c:336`) runs before
`main_network_http_bringup` and `main_bridges_bringup`, which start the UART
bridges. Recorded so a
reordering of boot does not silently turn this into a deadlock.

## Held across a dispatch but not taken on the worker (safe today, latent)

These locks are held across a dispatch but are never taken by worker code
today. A future change that takes any of them on the worker creates a
deadlock.

- **`s_reconcile_lock`** (`drivers/safety/safety_ceiling_sync.c:767`, gives at
  `:844`/`:873`/`:904`/`:917`). It is held around the whole reconcile body,
  including `enforce_ceiling_divergence`. That function calls the
  `s_disable_halt_run` hook (`safety_ceiling_sync.c:581`), and the hook reaches
  `profile_executor_fault_halt`, which dispatches `fault_halt_run_halt_job` at
  `profile_executor_status.c:259`.
  - The divergence-state lock is correctly released around the hooks
    (`safety_ceiling_sync.c:573-582`).
  - `s_reconcile_lock` is taken only by `safety_poll_task` and
    `kiln_cfg_swap_worker`. Neither is the worker, and no worker-reachable
    function takes it: only `safety_ceiling_sync_is_diverged` and the
    divergence-state lock wrappers are in the worker set.
- **`safety_cfg_store` `s_store_lock`**, at refetch (`drivers/safety/safety_cfg_store.c:1743`)
  and `refetch_nonblocking` (`safety_cfg_store.c:1783`, try-take 0). It is held
  across the flush dispatch at `safety_cfg_store.c:709`, which is reached from
  `safety_poll_task` -> `safety_update_health` (`safety_link_poll.c:676`) ->
  `safety_sync_cfg_cache` (`:218`) -> maybe_refetch (`:170`).
  - Separately: `safety_poll_task` is the ESP side of the link heartbeat, and
    it blocks synchronously on the worker here. If the worker is wedged by F1
    or F2, the heartbeat stops and the Pico trips S6b. That fails safe, but it
    turns F1/F2 into a field-visible safety trip.
- **`profiles_live_http` `s_decide_lock`** (`drivers/http/profiles_live_http.c:667`/`:673`).
- **`update_settings` `s_write_lock`** (`drivers/update/update_settings.c:232`/`:233`).
- **`aux_outputs_cfg` `s_set_lock`** (`drivers/persist/aux_outputs_cfg.c:315`) is held
  across `pref_cfg_fs_commit` (`aux_outputs_cfg.c:338`).
  - The worker takes only `s_lock` (via `aux_outputs_cfg_get` at `:233` and the
    enabled mask at `:279`).
  - `s_lock` is released at `aux_outputs_cfg.c:327`, before the commit.

## Candidates rejected after reading the code

| Candidate | Why it is not a finding |
|---|---|
| `adaptive_tune_revert` | Lock given at `adaptive_tune.c:1197`, before the writes at `:1205`/`:1206` |
| `autotune_engine_accept` | `s_at.lock` given at `autotune_engine_guard.c:435`, before `set_pid` at `:466`; the re-take at `:521` only flips state |
| executor `run_end` at `profile_executor.c:795` | Runs after the give at `:786` (F3 is about `adaptive_tune_lock`, not `s_exec.lock`) |
| `relay_cycles` `restore_all` / persist `s_rc.lock` | `s_rc.lock` given at `relay_cycles.c:1041` and `:1151`, before the dispatch |
| `zones_http_post.c:85-93` | `zones_cfg_lock` (portMUX) exits at `:88`, before `nvs_save` at `:93` |
| `run_state` `s_rs.lock` in migrate | `run_state.c`'s `migrate_from_default_partition` does an inline `hal_kv_commit` with no dispatch; the scanner merged it by name with `relay_cycles`' `migrate` |

## Reverse direction: worker waits on a task that waits on the worker

| Worker wait | Verdict |
|---|---|
| `kiln_io_owner` post-and-wait (`drivers/owners/kiln_io_owner.c:683-715`) | No owner function reaches a dispatch, so there is no cycle |
| `thermo_owner` post-and-wait (`drivers/owners/thermo_owner.c:275-304`) | Same: no cycle |
| `safety_link` `xact_lock` in `safety_exchange` on the worker (`drivers/safety/safety_link_inbox.c:796`) | Bounded by `SAFETY_XACT_LOCK_TIMEOUT_MS`. `safety_poll_task`'s own exchange (`safety_link_poll.c:605`) releases the lock before its dispatch path (`:676`), and the drain at `:749` is a try-take |
| `heat_enable` `he_flush_release_blocking` (`drivers/control/heat_enable.c:210-242`) | Bounded polling |
| Posted `safety_poll` job (`safety_link_poll.c:438`) | Fire-and-forget post; the poster never waits |

No function in `wifi_prov`, `update_fetch`, LVGL, `heat_enable` or
`uart_protocol` reaches a dispatch. No reverse-direction deadlock was found.

## Summary

| Rank | Lock | Holder task | Status |
|---|---|---|---|
| F1 HIGH | `s_exec.lock` | profile executor | Fixed |
| F2 HIGH | `s_at.lock` | autotune | Fixed |
| F3 MEDIUM | `adaptive_tune_lock` | profile executor (run end) | FIXED (plan/apply/commit split) |
| F4 LOW | `s_rc.persist_lock` | executor / worker | Bounded 3 s stall; accepted by design (ordering invariant) |
| F5 LOW | `s_rc.lock` | boot (`relay_cycles_init`) | FIXED (load into locals, publish under lock) |

## Follow-ups to the review of 4271767d / 48e1ba8a

**Finding 5 (save section entered before the worker starts reserves nothing): CLOSED.**
The review's premise was wrong about the trigger: LVGL does not start before the
worker. `main_control_bringup()` starts the worker, and LVGL starts later in
`main_bridges_bringup()`. The window was still real in principle, so it is closed
rather than argued. `s_bx_lock` now uses static storage. `uart_bridge_ext_save_reservation_init()`
creates it and installs the save-section hooks. `app_main` calls that function right after
`uart_log_bridge_early_init()`, before any other task exists. `bx_reserve_for_save_section()`
no longer requires `s_bx_started`, and `ensure_started()` neither creates nor deletes the
lock any more. A section entered before the worker starts now holds `s_bx_lock`, so a
worker created mid-section cannot run a queued or posted job until the section ends.

**Worker-side test seam: ADDED.** `bx_worker_iteration()` is the worker loop body, split out
with no behaviour change. `test/test_uart_bridge_ext_worker.c` includes `uart_bridge_ext.c`
with fake FreeRTOS primitives (a recursive mutex that tracks owner and depth, a one-slot queue,
a done semaphore that runs one worker iteration as the worker task). It has 5 tests, 159 checks:
- a reservation made before the worker starts;
- the worker not reserving for itself;
- a dispatch inside a section counting recursively;
- the timeout path giving nothing back;
- a posted job delayed, not dropped, while a section holds the lock.

Five mutations were run with `tools/negtest.ps1`, and every one was caught.

**Finding 4 (`profiles_http.c` `retarget_commit` holds the worker reservation for up to
about 2N cfg writes plus 2N reads): DOCUMENTED, not changed.** The only safety-relevant waiter
is `safety_poll`'s synchronous `safety_cfg_store_flush_if_dirty()`, and only when the safety
config is dirty. It would trip S6b only if the hold outlasted the link timeout. Retarget is
idle-only, and S6b fails safe. Chunking was rejected because it breaks the all-or-nothing
retarget. The comment is at the top of `retarget_commit()`.

**Executor stack budget regression from 4271767d: FIXED in 2fcd20c1.**
`check_executor_task_stack_budget` failed on dev at 2816 B against a 1936 B ceiling.
Two paths through `adaptive_tune_run_end` were over the ceiling:

1. 4271767d's on-worker branch in `nvs_save()` called `zones_autosave_job()` directly.
   The static analyser counts a direct call on every path, worker or not. That put the
   ~2.8 KB autosave frame into `executor_task_entry`'s worst case, through
   `adaptive_tune_apply_coupled_plan` -> `zones_config_set_coupling_cell` -> `nvs_save`.
   The on-worker branch now calls the job through a volatile function pointer. Behaviour is
   unchanged: the job still runs inline on the worker, and `flash_worker_lint`'s
   `is_on_flash_worker()` guard is kept. The volatile load stops GCC from folding the call
   back into a direct one.
2. With that path gone, the next one was 2080 B. `adaptive_tune_plan_coupled_locked` kept two
   288 B observation arrays on the stack, giving it a 704 B frame. They are now one malloc'd
   block, freed right after `adaptive_tune_coupled_fit()`. If the allocation fails, the
   function records a refusal reason and returns.

On a fresh target build of the fixed tree, `executor_task_entry` measures 1712 B, which passes.
**Correction, 2026-10-09:** the 1712 B figure was wrong. The static analyser followed only `call8`
and silently dropped every `l32r aN,<lit>` + `callx8 aN` long call (all IRAM/ROM and >512 KB flash calls).
With those resolved, `executor_task_entry` measures 3360 B, and 3936 B when the `nvs_save` ->
`zones_autosave_job` volatile-pointer edge is included. 3936 B + 1220 B unmodeled overhead = 5156 B,
which still fits the 6144 B stack, so the stack was not bumped. The executor ceiling is now 3360 B
(re-baselined, not a regression of this fix).

The finding-5 fix and the test seam are in 9cc5ed06.
