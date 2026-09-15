# Adversarial review of 5bc9afb5 and the 561efa3b autosave hunks

Date: 2026-09-15. Scope: follow-up to
`docs/audits/review_autosave_slot_fix_a93ee77b_2026-09-15.md` (the "prior review").
It covers `5bc9afb5`, which moves the `active_id` finalize to right after
readback in `kiln_cfg_swap_apply()`. It also covers the `kiln_cfg_store_apply()`
override and the real-path test that were swept into `561efa3b`.

Production code was not changed. One experiment was run in a clean worktree
(`C:\wt\arv5`, detached at `5bc9afb5`). It was restored by hand, `git diff` came
back empty, and a fresh-directory full rebuild passed.

## Summary

| # | Prior finding | Status at HEAD | Severity now |
|---|---------------|----------------|--------------|
| 1 | HIGH: `kiln_cfg_store_apply()` unguarded | **Closed** | - |
| 3 | MEDIUM: override window narrower than the `active_id` gap | **Closed** for the long window; a short residual remains | LOW |
| 4 | MEDIUM: tests only reach a fake | **Closed**, and proved by a negative test | - |
| 5 | LOW: cross-task autosave under the override | **Open**, unchanged, not claimed fixed | LOW |
| A | NEW: early finalize moves divergence-recapture corruption onto the slot boot recovery verifies against | Interaction with the 561efa3b rework | MEDIUM |
| B | NEW: test name contradicts its assertion | - | LOW |

## HIGH 1: closed

At `kiln_cfg_store.c` ~1246, `kiln_cfg_store_apply()` now sets the override to `id`
around `zones_config_import_blob()`, under `kiln_cfg_store_lock()`. The override is
cleared on the next statement, with no early return between the set and the clear.
The import's synchronous autosave therefore targets the incoming slot.

## MEDIUM 4: closed, proven

`test_apply_autosave_targets_incoming_slot_via_real_override()`
(`test_kiln_cfg_store.c:2143`) drives the real setter and the real
`kiln_cfg_store_autosave_from_live()`.

**Negative test.** I replaced the ternary in `autosave_from_live()` with
`int32_t active = s_store.active_id;`, then ran a fresh full build
(`C:\wt\arv5_neg3`). 45/45 executables built, and exactly one run failure:
`test_kiln_cfg_store.c:2203: slot A (outgoing) was NOT touched by the autosave that
fired mid-apply`. After restoring by hand, `git diff` was empty. A fresh full
rebuild (`C:\wt\arv5_pos`) passed: "all 45 host test executables built and passed".

Not run: removing only the `set_autosave_target_override(id)` call in
`kiln_cfg_store_apply()`. By inspection, the same assertion should catch it.

## MEDIUM 3: closed; short residual (LOW)

`5bc9afb5` moves `kiln_cfg_store_set_active_id_raw(target_id)` ahead of
`safety_ceiling_sync_reconcile_on_link_up()` and the divergence decision. The
multi-second window that included Pico round trips is gone, and so is the
indefinite stale `active_id` on the diverged exit.

A residual gap remains in two places:

- **Swap path.** Between the override clear (after step 8's import) and the new
  finalize there are still `persist_marker(ESP_DONE)` and the readback export.
  That is local flash I/O with no Pico traffic. A cross-task `nvs_save()` landing
  there still autosaves the incoming live config into the outgoing slot.
- **Store path.** `kiln_cfg_store_apply()` has the same gap between
  `kiln_cfg_store_unlock()` and `s_store.active_id = id`, but it is only a few
  statements long.

Both are the prior review's LOW 5 cross-task class, not a same-task path.

## Is finalizing before the divergence check safe?

**For the slot/`active_id` pair: yes.** `active_id` now names the kiln whose ESP
zones config is live, and readback proved it. Checking each exit:

- **Rollback exits** (push refused, write race, ESP import refused, readback
  mismatch) all occur *before* the finalize, so `active_id` never moved.
- **After the finalize,** `kiln_cfg_swap_apply()` has no rollback. The only
  non-success exit is the diverged one. It returns with `active_id = target`, the
  ESP on target content, and the marker left at `ESP_DONE`.
- **Boot `ESP_DONE` recovery** (`kiln_cfg_swap.c:780`) has three outcomes:
  1. Target slot unreadable: it calls `rollback()`, which re-imports the previous
     blob under override `previous_active_id` and then calls
     `set_active_id_raw(previous_active_id)` (`kiln_cfg_swap.c:384`). Both sides of
     the pair are restored, so this is not a reset-one-side defect.
  2. Both sides match: it sets `target`, which is idempotent.
  3. Mismatch: it stays alarmed with `active_id = target` and does not roll back.
     That is consistent, because the ESP really is on target.

On the diverged exit, `active_id` does name a kiln whose Pico half the Pico may not
hold. That state is alarmed, with heat forced off by the latch. The early finalize
also fixes a latent mis-comparison. Under the old order, `reconcile_on_link_up()`
read the expected Pico fields from the active slot. That was still the **outgoing**
kiln, while the Pico had just been given the target's. Any swap between kilns with
different Pico halves would have reported a spurious divergence. The new order
compares against the correct slot.

## A (MEDIUM, NEW): interaction with the divergence-check rework

This builds on `docs/audits/review_divergence_check_561efa3b_2026-09-15.md` HIGH 1.
`autosave_from_live()` -> `save_current()` -> `populate_pico_half_and_hash()`
recaptures the active slot's Pico half from the **live Pico cache**.

1. **Before `5bc9afb5`.** A zones save during the diverged window corrupted the
   **outgoing** slot. The target slot stayed pristine, so boot `ESP_DONE` recovery
   still compared the Pico against the true target half.
2. **After `5bc9afb5`.** The same save rewrites the **target** slot's Pico half
   with whatever the Pico really holds, for example flash-fallback values after a
   reboot. That self-clears the latch. At the next boot, `ESP_DONE` recovery then
   finds `pico_matches == true` against the corrupted slot and "finishes" the swap.
   It calls `persist_pico_flash_fallback()` with the corrupted half and clears the
   pending record. The evidence is gone, and the wrong Pico config is now enshrined
   in both the slot and Pico flash.

Neither ordering is safe while the recapture exists. The early finalize moves the
damage from a bystander slot onto the one reference that recovery trusts.

Requirements for the concurrent suppression rework:

- Suppress the Pico-half recapture, and ideally the whole autosave, both while
  `safety_ceiling_sync_is_diverged()` and while a swap marker is not `NONE`.
- Suppression must not be gated on the latch alone. The latch is exactly what the
  recapture clears, and `ESP_DONE` persists across reboot before the latch re-arms.
- Consider the plan's rule: record the expected Pico set only on a confirmed push.

**Impact today:** `kiln_cfg_swap_apply()` still has no production caller (prior
review finding 2), so this is latent. It becomes live the moment the swap is wired.

## B (LOW, NEW): misleading test name

`test_diverged_ceiling_does_not_finalize()` now asserts `s_active_id == 7`, which
means it *does* finalize. The body comments explain this, but the name states the
opposite of the contract. Rename it (e.g. `..._finalizes_before_divergence_check`).

## LOW 5: still open

`s_autosave_target_override` is still a plain static that other tasks' `nvs_save()`
callers read without taking `s_swap_lock`. Nothing in these commits addresses it,
and neither commit claims to.

## Not re-verified

- Prior review finding 2 (swap unwired): a grep still finds no production caller.
- Findings 6, 7 and 8 were outside this pass.
