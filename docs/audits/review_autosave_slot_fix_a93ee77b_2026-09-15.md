# Adversarial review of a93ee77b (kiln-config autosave slot override)

Date: 2026-09-15. Read-only review; no production code changed. Context:
`docs/audits/kiln_profiles_feature_review_2026-09-15.md` Defect 1.

## Summary

The override is mechanically correct on the two call sites it touches, but
those call sites have no production caller. The same defect is still live on
the path that does have one (`POST` apply -> `kiln_cfg_store_apply()`). It
also comes back for any autosave that fires later in the swap window, while
the override is cleared but `active_id` has not moved yet. The new tests only
check a fake. The production reader of the override has no test.

| # | Severity | Finding |
|---|----------|---------|
| 1 | HIGH | Defect 1 still live in `kiln_cfg_store_apply()` (production HTTP path) |
| 2 | MEDIUM | `kiln_cfg_swap_apply()` has no production caller; the fix protects dead code today |
| 3 | MEDIUM | Override window is narrower than the `active_id`/live-config divergence window (steps 8-12, and indefinitely on the diverged exit) |
| 4 | MEDIUM | Tests exercise a fake setter only; `kiln_cfg_store_autosave_from_live()`'s override branch is untested |
| 5 | LOW | Cross-task race: a concurrent `nvs_save()` from another task can have its autosave run under the override |
| 6 | LOW | Rollback override premise is wrong (harmless) |
| 7 | INFO | Exit paths, dispatch semantics, deadlock: OK |
| 8 | INFO | `msg[1024]` stack question: httpd task, heap-bundled function, not at risk |

## 1. HIGH: the unfixed production path

`kiln_cfg_store.c` `kiln_cfg_store_apply()` (~line 1236) calls
`zones_config_import_blob()` and only then sets `s_store.active_id = id`.
The import's `nvs_save()` dispatches `zones_autosave_job` onto the flash
worker synchronously (`bx_run_on_internal_stack()` blocks on `s_bx_done`).
So the autosave runs *before* `active_id` moves and writes the incoming live
config over the **outgoing** slot. That is Defect 1 exactly. This function is
reachable from `kiln_cfg_http.c:262` (the apply handler). The commit
guarded only the swap module. Fix: set the override to `id` around this
import too, or move `active_id` first and restore it on failure.

The boot-restore import (`kiln_cfg_store.c` ~903) is fine: `active_id`
already names the slot being imported.

## 2. MEDIUM: swap path is unwired

`grep kiln_cfg_swap_apply` finds only `kiln_cfg_swap.c/.h` and tests. No HTTP,
LCD, or bridge caller exists. The commit message's framing ("wrote incoming
config into outgoing kiln's slot") matches the live `kiln_cfg_store_apply`
path, not the one that was changed.

## 3. MEDIUM: divergence outlives the override (reset-one-side class)

`active_id` and the live zones config form a pair that the autosave assumes
agree. The override covers only the `zones_config_import_blob()` call. After
it is cleared, the live config is the incoming kiln's, but `active_id` still
names the outgoing kiln through ESP_DONE persist, readback,
`safety_ceiling_sync_reconcile_on_link_up()` and the divergence check (Pico
round trips, seconds). Any `nvs_save()` in that window reverts to
`s_store.active_id` and overwrites the outgoing slot. Examples: a zones POST
on httpd, or an adaptive-tune/executor write. Worse, the `diverged` exit
("config left pending for retry") returns with that mismatch still in place,
and it stays until the retry or reboot. Every ordinary setter's autosave
until then corrupts the outgoing slot. Swap is refused while firing, which
lowers the odds but does not remove the risk (httpd setters are not gated).
Suggested fix: keep the override set from the import until
`set_active_id_raw()`/rollback completes, clearing it on every exit. Or
suppress autosave for the whole transaction.

## 4. MEDIUM: tests do not reach production

`test_kiln_cfg_swap.c` replaces `kiln_cfg_store_set_autosave_target_override()`
with a local fake and asserts only the value latched at import time. Nothing
links the real `kiln_cfg_store.c` setter together with
`kiln_cfg_store_autosave_from_live()`. `test_kiln_cfg_store.c` has no override
test, and `test_zones_http.c` stubs the autosave entirely. Removing the
ternary in `autosave_from_live()` (the actual behavioral fix) would leave
every test green. By inspection, no experiment was run. The negative test in
the commit message only removed the swap-side calls, so it proves the fake
was called, not that a slot was protected. Needed: a store-level test that sets
active=A and override=B, calls `autosave_from_live()`, and asserts B's slot
changed and A's did not.

## 5. LOW: cross-task observation

`s_autosave_target_override` is a plain static read on the flash-worker task.
`kiln_cfg_store_lock()` (`s_swap_lock`) is not taken by other `nvs_save()`
callers. If task B is already queued on `s_bx_lock` with its own autosave job
when the swap sets the override, B's job runs with override=target. It reads
the live config at that moment, which may still be the pre-import config if
B's job runs before the import's RAM commit. That writes the outgoing config
into the incoming slot. It needs tight interleaving, and the unguarded code
had the mirror-image race. The actual read/write across tasks is ordered by
the queue handoff, so there is no torn read.

## 6. LOW: rollback premise

The rollback comment and `test_rollback_autosave_targets_previous_slot_not_target`
assume `active_id == target_id` when rollback re-imports. In every real call
site, `active_id` is still `previous_active_id`: apply's rollbacks all precede
step 12, and boot recovery never moves it before its fallback rollback. The
override therefore equals `active_id` and is redundant, not wrong. The test
fakes `s_active_id = 3` and asserts 3, so it does not model its own stated
scenario. `previous_active_id == KILN_CFG_NO_ACTIVE_ID` makes the autosave a
no-op, which is correct.

## 7. INFO: verified OK

- Every exit path is covered. Each set is followed by the clear on the very
  next statement with no early return between them.
  `zones_config_import_blob()` returns normally on failure.
- Dispatch is synchronous (`bx_run_on_internal_stack()`), or inline when the
  caller is already the worker. The override is therefore still set when the
  autosave job reads it. An asynchronous queue would have made the fix inert.
- No deadlock: `kiln_cfg_store_save_current()` does not take `s_swap_lock`, so
  holding `kiln_cfg_store_lock()` across the blocking dispatch is safe.
- The sentinel `INT32_MIN` cannot collide with a slot id or `KILN_CFG_NO_ACTIVE_ID`.

## 8. INFO: swept-in WIP and msg[1024]

Both buffers are in `kiln_cfg_store_import_package_json()`, called only from
`kiln_cfg_http.c` `import_post_handler`, on the shared 8192 B **httpd_worker**
stack (ceiling 4832 B, `check_httpd_task_stack_budget.py`). The function's
large locals are already one heap `malloc` (`kiln_cfg_import_scratch_t`). The
two 1024 B arrays sit in sibling scopes, so worst case is +~1.8 KB, or ~1 KB
with slot sharing. Measured against `build/elf_archive/KilnCtrl-latest.elf`
(linked 10:13, after the commit; `build/KilnCtrl.elf` was 0 B, mid-build
elsewhere): check OK, `import_post_handler` is not in the top five (below
3456 B). The worst path is `cfgfs_status_get_handler` at 4304 B, with honest
headroom LOW (2088 B). I could not confirm whether that ELF carries the
`[1024]` version, because the working tree no longer has `msg[1024]`
(presumably the owning agent has already shrunk it). Not at risk per the
static check; the live `get_stack_margin` reading is the authority. `%.1f`
of a float bounded by the checks needs < 16 chars, so an explicit precision
or width cap on a 192 B buffer is the proper fix, not 1024.

The sweep also brought in unrelated WIP (Defect 2
`kiln_cfg_store_capture_expected_pico_fields`, Defect 4 `nvs_save_store`
refusal) under a commit message that does not mention either. It needs its
own review. Defect 4's `memset(e, 0, ...)` rollback also does not undo any
`count`/next-id bookkeeping done at slot allocation. Not verified here.
