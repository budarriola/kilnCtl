# Adversarial review: c1d2c526 (divergence rework: HIGH1-3 + swap-pending MEDIUM)

Answers `review_divergence_check_561efa3b_2026-09-15.md` and finding A of
`review_autosave_rework_5bc9afb5_2026-09-15.md`. Read-only review; no flashing.

## Verdict

HIGH1 and the swap-pending gate do what they claim, and the tests hit production
code. But HIGH1's gate keys on the new warning-only standing flag. HIGH2's own
rationale says ordinary applies produce that flag. Together these create a
state where autosave is off indefinitely, silently, with no operator-visible
signal. HIGH3's fix is probably defeated by the same gate.

| # | Finding | Severity |
|---|---------|----------|
| 1 | Standing warning suppresses autosave indefinitely and silently; zone/autotune/coupling edits never reach the active slot | HIGH |
| 2 | Standing warning has no consumer: log line only, not in readiness, web or LCD | HIGH (consumer without producer) |
| 3 | HIGH3 autosave races the poll tick that its own commit makes diverge, so it likely suppresses itself | MEDIUM (reasoned, not measured) |
| 4 | HIGH3 has no test; the `test_safety_cfg_http.c` hunk only adds link stubs | MEDIUM |
| 5 | Explicit "save over slot" (`kiln_cfg_http.c:165` -> `save_current`) is ungated and still launders a divergence | LOW (operator-initiated) |
| 6 | Swap-pending seam fails open (NULL means "not pending") | LOW |
| - | MEDIUM4 (expected-record provenance), MEDIUM5 (stale cache after failed refetch) | still OPEN, unchanged |

## 1 (HIGH): autosave off indefinitely, silently

`safety_ceiling_sync.c` recomputes `s_standing_warning_active` every tick by
level, not as a latch. It clears only when the slot's captured Pico half
matches the live cache again. The slot's Pico half is rewritten only by
`populate_pico_half_and_hash()`, which is reached via `save_current()`. The
automatic caller of `save_current()` is `kiln_cfg_store_autosave_from_live()`,
and the gate now blocks it while standing is set. So the flag cannot clear
itself. Only these clear it:

- the Pico's values change back (for example, an operator re-commissions them
  to the slot's values);
- an explicit operator save or apply of a slot;
- `target_known == false`.

The commit's own HIGH2 comment says standing mismatches happen "on every
ordinary apply whose captured Pico half merely predates the live Pico value".
After such an apply, every later zones POST, autotune accept and coupling write
reaches `zones_autosave_job()`. That job gets `true` back (suppressed counts as
success) and logs nothing (`zones_config_store.c:343` logs only on `false`). The
comment's "dirty flag persists until the next opportunity" does not hold: there
is no dirty flag, and nothing retries.

The active slot silently falls behind live. The next apply of that slot, or a
swap rollback to it, restores stale gains.

Fix direction: record the expected Pico half only on a confirmed push (the
plan's rule). Then recapture of the ESP half does not need suppressing at all.
At minimum, keep the ESP-blob autosave going and suppress only
`populate_pico_half_and_hash()`, and report "suppressed" to callers distinctly
from "saved".

## 2 (HIGH): the warning is invisible

`safety_ceiling_sync_is_standing_diverged()` has exactly one non-test caller,
the autosave gate. `readiness_http.c:683` and `readiness_gate.c:68` read only
`is_diverged()`. No JSON, JS or LCD code mentions it. The only signal is an
`ESP_LOGW`, rate-limited. An operator can have a diverged Pico config plus
disabled autosave (finding 1) and see nothing.

## 3 (MEDIUM): HIGH3 likely suppresses itself

`commissioning_post_handler` -> `apply_pairs()` commits, then blocking-refetches
the Pico cache. With `nonblocking_refetch=false`, the cache is updated before
the call returns. After that, the handler does an NVS write for 0x0204, and only
then autosaves.

`safety_poll_task` runs `enforce_ceiling_divergence()` unconditionally every
poll iteration. Any tick that runs in that window sees the new live value
against the stale slot value, sets standing, and the HIGH3 autosave returns
"suppressed", which is finding 1. Whether this wins depends on the poll period
against the NVS write latency. Not measured on hardware. The result is discarded
with `(void)`, so it would be invisible either way.

Fix: autosave (or a Pico-half-only recapture) before the cache becomes
observable, or bypass the gate for this confirmed-push path. This is exactly
the "confirmed push" recording MEDIUM4 asks for.

## 4 (MEDIUM): HIGH3 untested

No test asserts that a committed commissioning POST updates the active slot, or
that a stage-only POST does not.

## 5 (LOW): explicit save still launders

`kiln_cfg_http.c` "save over existing" calls `save_current()` directly, not
through the gate, and recaptures the live Pico half. It is operator-initiated,
but the operator gets no divergence warning first (finding 2).

## 6 (LOW): seam fails open

`autosave_blocked_by_swap_pending()` returns false when unregistered. It is
registered in `main_control_bringup.c`. Autosave callers are HTTP, autotune and
coupling paths that start later, so it is fine today. `kiln_cfg_swap_apply()`
still has no production caller, so finding A was latent anyway.

## Checked and clean

- **HIGH1 gate on production code.** `test_kiln_cfg_store` links the real
  `kiln_cfg_store.c`; only the sync predicates are stubbed. Negative test in a
  clean worktree (`C:\wt\rv_c1d2` at c1d2c526): replacing the standing term with
  `false` makes `test_kiln_cfg_store.c:2219` FAIL (7885/7886). Restored by hand
  (reverse edit), `git diff` empty, fresh out dir rebuilt 45/45 PASS.
- **HIGH2 heat-off scope.** Enforcement is `config_divergence_check(..., 1, ...)`
  on field 0 only, the pre-561efa3b behaviour. The ceiling writer is untouched.
  No new write to the Pico, no disarm path, and `abs_max_temp_c` still has to
  equal the ESP's target.
- **httpd safety of the HIGH3 call.**
  - It runs on the httpd task, whose stack is internal, not PSRAM. Same as the
    existing `kiln_cfg_http.c` `save_current()` call.
  - It does not dispatch to the flash worker, so there is no re-entrancy.
  - `kiln_cfg_swap_is_pending()` costs one NVS read per autosave.
  - `check_httpd_task_stack_budget.py` puts `commissioning_post_handler` at
    4192 B, under the 4832 B ceiling (deepest overall: `cfgfs_status_get_handler`,
    4304 B). Caveat: that ran against the main-tree ELF built at 11:35 in a dirty
    tree, so its provenance is not established.
- **Swap-pending gate.** Real `kiln_cfg_swap_get_marker()` wrapper, and the test
  flips it with both divergence flags false.
