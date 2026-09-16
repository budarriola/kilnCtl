# Adversarial review: crash-gate follow-up fixes (commit hash in this file's name)

Date: 2026-09-15. Read-only review of the follow-up commit to
`review_crash_gate_medium_fixes_aa2c484d_2026-09-15.md`, which closed one MEDIUM
and two LOW findings against the crash-report manual-relay gate. Nothing was
flashed, no source was changed, and the review worked from a private clean
worktree of origin/main under `C:\wt\` (removed afterwards). The gate policy
itself (option B) is an owner decision and was not re-examined; only the
implementation was reviewed.

Scope: 5 files, 194 insertions / 53 deletions, in `uart_bridge_ext.c`,
`flash_worker.h`, `relay_cycles.c`, `relay_cycles.h`, `ui_page_diagnostics.c`.

## Verdicts

| Claimed fix | Verdict |
|---|---|
| MEDIUM — gate `refresh_cb(NULL)` in `show_page()` on the crash-report page | **Closed** |
| LOW 1 — bounded `relay_cycles_reset_timeout()` + shared tail | **Partially closed** |
| LOW 2 — document the "unbounded once accepted" invariant | **Closed** |

### MEDIUM — `show_page()` gate: closed, and not too tight

`firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c`, `show_page()`.

- `s_page_index = index;` is assigned *before* the new gate, so when the gate does
  fire, `refresh_cb()`'s own internal `s_page_index == UI_PAGE_DIAGNOSTICS_PAGE_CRASH_REPORT`
  check sees the new page. Ordering is correct; a gate written the other way round
  would have silently refreshed nothing.
- Every path into `show_page()` was enumerated: `prev_cb`, `next_cb`, and the single
  `show_page(0)` in `build()`. Only the first two can land on the crash-report page,
  and both go through the gate. `build()` lands on page 0 and then calls
  `refresh_cb(NULL)` separately on the line after, so page 0 still gets its initial
  paint.
- The crash-report page therefore still refreshes on every page-in, and on the
  periodic timer path, unchanged. The gate is not too tight.
- The comment above the call no longer claims `refresh_cb()` is unconditionally
  cheap. Verified against the body, which opens with `dashboard_get_status(&ds)` —
  three MAX31856 SPI reads and a queue wait. The old comment was genuinely wrong.

**Unclaimed side effect (documentation defect, not a code defect).** The commit
message says the "`refresh_cb()` runs twice at build time" cosmetic item was
deliberately left untouched. It was not: `build()` calls `show_page(0)` and then
`refresh_cb(NULL)`, and since `show_page(0)` no longer calls `refresh_cb()`, this
commit *closed* that item as a side effect. The behaviour is an improvement; the
commit message is stale about it.

### LOW 1 — `relay_cycles_reset_timeout()`: correct, but with three unclaimed regressions

`firmware/KilnFW/App/drivers/persist/relay_cycles.c` / `.h`.

Correct as far as it goes:

- The bounded-acquire pattern **genuinely matches** `crash_report_acknowledge_timeout()`
  rather than merely resembling it. Both snapshot under the module lock, release the
  lock, check `uart_bridge_ext_is_on_flash_worker()` and run the job inline if already
  on the worker, otherwise dispatch via `uart_bridge_ext_run_on_flash_worker_timeout()`,
  and funnel both outcomes into a shared tail. The only structural difference is
  cosmetic: `crash_report.c` tests `submit_err == ESP_ERR_TIMEOUT` in the caller, while
  `relay_cycles.c` pushed that branch into the shared tail. Same semantics.
- **Re-entrancy is safe.** The only caller is `relay_reset_btn_clicked_cb()` in
  `ui_page_diagnostics.c`, which runs on `lvgl_task`. The flash worker never calls into
  the UI, so the function is unreachable from the worker in practice, and the explicit
  `uart_bridge_ext_is_on_flash_worker()` check makes it safe even if it ever were —
  double-guarded, matching the documented hazard in this repo's flash-worker notes.
- **The shared tail did not change behaviour for the pre-existing caller** in any way
  that affects persistence: `relay_cycles_reset()` passes `out_timed_out = NULL`, the
  tail null-checks it, and the ESP_OK/failure/dirty handling is byte-for-byte the
  previous logic. See the one diagnostic exception below.
- **Lock discipline is clean.** `s_rc.lock` is released before the dispatch, and the
  tail takes it only around scalar field assignments (`s_rc.rev`, `s_rc.dirty`) with no
  producer or blocking call inside. The `show_page()` change holds no lock at all and
  strictly reduces work on `lvgl_task`. Both satisfy the standing "never hold locks
  across producer or blocking calls" rule.

Three unclaimed regressions, all introduced by this commit:

**N1 (LOW) — the success log lost the old count.** The refactor into
`relay_cycles_reset_finish()` dropped the `old_count` local, and the success message
went from `"count reset from %lu to 0"` to `"count reset to 0"`. Relay-cycle counts are
wear data with no other record of the pre-reset value once it is zeroed, so the log line
was the only place an accidental reset could be reconstructed from. This also regresses
the **pre-existing HTTP caller**, which was not part of the claimed change.

**N2 (LOW) — the UI path silently swallows a persist failure.** Contrast the two
handlers. `crash_ack_btn_clicked_cb()` logs `ESP_LOGW` on *both* the timeout branch and
the `!ok` branch, and explicitly restores its "Acknowledge" label in the non-timeout
branch. The new `relay_reset_btn_clicked_cb()` does neither:

```c
bool ok = relay_cycles_reset_timeout(relay, UI_PAGE_DIAGNOSTICS_RELAY_RESET_WAIT_MS, &timed_out);
if (timed_out) {
    lv_label_set_text(s_rl_reset_label[relay], "Busy");
}
(void)ok;
```

A non-timeout persist failure (`ok == false`, `timed_out == false`) produces no log line
and leaves the label reading "Reset", i.e. indistinguishable from success, while the RAM
count is zeroed and `s_rc.dirty` is set. This is the "logging unchecked success" class
already called out for this repo — here it is worse than unchecked, it is explicitly
discarded with a cast to void.

**N3 (LOW) — only the tail was shared; the snapshot head is duplicated.** The
snapshot-under-lock block (zero the count, three `memcpy`s, `rev + 1`, clear `dirty`) is
now written out verbatim in both `relay_cycles_reset()` and `relay_cycles_reset_timeout()`.
Any future field added to `reset_persist_job_arg_t` must be copied into both, and the
compiler will not say so. This is the "reset one side of a pair" hazard documented for
this repo, created by a refactor whose stated purpose was to remove exactly this kind of
duplication. Extracting the head as well (e.g. a `relay_cycles_reset_snapshot()`) would
have made the two functions differ only by their dispatch call.

### LOW 2 — documentation of the unbounded-once-accepted invariant: closed

`uart_bridge_ext.c`'s new comment sits directly above the deliberate
`xSemaphoreTake(s_bx_done, portMAX_DELAY)` and correctly states why a bounded wait
*there* would be a defect (a queued job would be left pointing at an unwound stack
frame), as distinct from the bounded acquire on `s_bx_lock`. `flash_worker.h`'s doc
comment for `uart_bridge_ext_run_on_flash_worker_timeout()` names relay-cycle reset as a
second known caller and states plainly that nothing mechanically enforces short jobs.
Both are accurate against the code. Documenting rather than enforcing was the right call
here: the invariant is about job duration, which has no syntactic form to check.

## The untested new function

`relay_cycles_reset_timeout()` ships with no test, and this is a real gap rather than a
formality:

- **The code is reachable.** It is wired to the diagnostics page's relay reset button.
- **The timeout path is reachable.** `uart_bridge_ext_run_on_flash_worker_timeout()`
  returns `ESP_ERR_TIMEOUT` whenever the worker is busy, which is routine during a
  profile save or a config write.
- **Nothing that exists today would catch a defect in it.** `test_relay_cycles.c`
  includes `stubs/bx_worker_stub.h` and then the driver `.c` directly, and has three
  reset tests — none of which call the new entry point. There is no test for
  `crash_report_acknowledge_timeout()` either, so the precedent it was modelled on is
  equally uncovered. The stub's bounded sibling
  (`uart_bridge_ext_run_on_flash_worker_timeout()`, which returns `ESP_ERR_TIMEOUT`
  when `s_stub_bx_busy`) is dead infrastructure: it is the exact hook such a test
  needs and nothing currently reaches it.

The harness is already in place, so the missing test is cheap. **The test that should
exist**, in `firmware/KilnFW/App/test/test_relay_cycles.c`:

`test_reset_timeout_busy_worker_reports_timeout()` — seed a non-zero count, set
`s_stub_bx_busy = true`, call `relay_cycles_reset_timeout(0, 300, &timed_out)`, and
assert: it returns `false`; `timed_out` is `true`; the in-RAM count is 0 (the snapshot
already committed it); `s_rc.dirty` is `true` so a later flush still persists it; and
`s_stub_dispatch_count` is unchanged (the busy path must not have run the job). A
companion `test_reset_timeout_idle_worker_succeeds()` with `s_stub_bx_busy = false`
should assert `true` / `timed_out == false` / `dirty == false` / dispatch count
incremented by exactly one, which is what pins the shared tail against N1-style drift.

## Check-suite status

`tools/run_all_checks.ps1` was run with `-ExecutionPolicy Bypass` in the foreground.
The two reported failures are both pre-existing and unrelated to this commit:

- **`check_mykicad_golden_suite_runs`** — the real error, which PowerShell's
  `NativeCommandError` rendering had been truncating, is
  `ModuleNotFoundError: No module named 'execnet'` raised from `xdist/newhooks.py`
  during pytest plugin load. The submodule's `.venv` has `pytest-xdist` installed
  without its `execnet` dependency. A broken local venv, not a code failure, and this
  commit touches no Python.
- **`check_all_task_stack_budgets`** — a separate investigation, now closed, root-caused
  this: `idf.py set-target` regenerates `sdkconfig` from `sdkconfig.defaults`, where
  `CONFIG_KILNCTL_ENABLE_GPIO_PROBE` defaults to `n`. With the probe compiled out,
  `gpio_probe_task` does not exist in the ELF, but the checker's root-symbol table lists
  it unconditionally, so it hard-fails before measuring any other task. The shared main
  tree's gitignored `sdkconfig` has the option set to `y` from bench work, which is why
  the same check passes there — confirmed here: run against the main tree's ELF it
  reports `OK -- 28 registered tasks`, `gpio_probe` included. A fix to the checker is
  owned elsewhere and was deliberately not attempted. The separately reported `lvgl`
  stack overrun did not reproduce either: from a clean worktree `lvgl` measures 4544 B
  against an 8192 B declaration and a 4880 B ceiling, status INDETERMINATE, not FAIL.

Neither failure can be caused by this commit: the checker is ELF- and table-driven and
the commit registers no task and changes no stack declaration. The `lvgl` task's
`extra_roots` does list `refresh_cb` from `ui_page_diagnostics.c`, but gating a *call
to* `refresh_cb()` inside `show_page()` changes neither `refresh_cb()`'s own frame nor
its callees, so the measured depth is unchanged by construction.

A methodology note for future reviews of this area:
`firmware/KilnFW/App/test/check_00_kilnfw_target_build.ps1` robocopy-mirrors the **main
tree's** firmware into a fixed shared worktree at `C:\wt\checkbuild`, not the worktree it
is invoked from. It therefore cannot validate a private clean worktree, and re-running it
both measures the (shared, dirty) main tree and perturbs a path other live sessions use.
It was deliberately skipped here in favour of `-Fast` plus a private build.

## Summary

The MEDIUM fix is correct and complete, and the LOW 2 documentation fix is accurate. The
LOW 1 fix is functionally correct and its re-entrancy and lock discipline hold up under
inspection, but it arrives with a lost diagnostic (N1), a silently swallowed failure in
the UI path (N2), a duplicated snapshot head that re-creates the hazard the refactor was
meant to remove (N3), and no test coverage at all for the new entry point or the
precedent it copies. None of these is a correctness bug on the happy path; all three
would make a real field failure harder to see.
