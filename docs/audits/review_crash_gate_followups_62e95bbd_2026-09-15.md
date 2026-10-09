# Adversarial review: crash-report relay-gate follow-ups (commit 62e95bbd)

Date: 2026-09-15. Read-only review of the follow-up commit to
`review_crash_report_relay_gate_61765de7_2026-09-15.md`. No firmware was flashed.

## Completeness (clean worktree at origin/main = 62e95bbd)

- A clean `git worktree` at origin/main (under `C:\wt\`) needed no untracked files.
  The commit is complete.
- KilnFW host tests (`build_host_tests.ps1 -OutDir` private): **46/46 executables
  built and passed.** That includes `test_heat_enable`, `test_profile_executor_prestart`
  and `test_autotune_engine_prestart`. The implementer's reported failures in those
  three come from the other session's uncommitted `heat_enable.c` work, not from HEAD.
- KilnFW target build (PowerShell, MSYS env stripped, CCACHE_DISABLE=1, main tree's
  sdkconfig copied in): **exit 0, all 2187 steps.** `KilnCtrl.bin` was freshly linked,
  and the app partition has 27% free space. `-Werror` did not trip in any touched TU.
- `check_ui_budget_asserts.ps1`: PASS. The new `_Static_assert` is present.
- `check_all_task_stack_budgets.ps1` on the worktree ELF: exit 0. Every task is still
  INDETERMINATE (a lower bound only), the same as before this commit.
- The worktree was removed after the review.

## Findings, severity-ranked

### LOW-1: the test claims flash-worker routing but never checks it (negative test survived)
`firmware/KilnFW/App/test/test_crash_report.c:224-257`
(`test_acknowledge_write_failure_path`). The section title says the ack "still routes
through the flash worker". Nothing in the test checks that:

- `reset_all()` sets `fake_kv_set_write_safe_here(true)` for the whole file.
- No check reads the stub's `s_stub_on_flash_worker` or a dispatch count.

**Negative test:** in `crash_report.c` the dispatch
`uart_bridge_ext_run_on_flash_worker(crash_ack_persist_job, &ctx)` was replaced with
an inline `crash_ack_persist_job(&ctx)`. The test exe was rebuilt from scratch into a
fresh directory and still reported **72/72 passed**.

A future edit that drops the dispatch would go unnoticed. The fix is to call
`fake_kv_set_write_safe_here(false)` outside the worker, so an inline write fails, or
to assert a dispatch counter in `bx_worker_stub.h`.

As a control, removing `refresh_unacked_cache()` from `crash_report_clear()` made the
suite fail as intended: 71/72, failing at `test_crash_report.c:516`. The clear-flag
test is real.

The source was restored by hand (byte-identical hash to a backup, and `git status`
clean). The restored source was rebuilt fresh and passed 72/72.

### LOW-2: the commit's PSRAM premise is false, and the dispatch adds an unbounded UI stall
`firmware/KilnFW/App/drivers/safety/crash_report.c:169-187` (rationale comment) and
`firmware/KilnFW/App/drivers/bridge/uart_bridge_ext.c:417-421`.

**The premise:** `lvgl_task`'s stack is **not** PSRAM. It became static internal SRAM
in 2026-08-21 (`lvgl_port.c:1016`, `s_lvgl_task_stack[8192/...]` via
`xTaskCreateStaticPinnedToCore`; see the "REVERTED TO INTERNAL SRAM" comment above
it). So the stated hazard does not apply to the LCD caller. The dispatch is harmless,
but the comment block and the commit message will mislead the next reader.

**The new cost:** the LVGL click callback runs under the LVGL port lock. It now waits
with `portMAX_DELAY`, first on `s_bx_lock` and then on `s_bx_done`. If the flash worker
is busy with a long job (a profile or autotune save, or a CONTROL message), the whole
LCD freezes for that long.

No flash-worker job was found that takes the LVGL lock, so there is no deadlock today:
nothing under `drivers/bridge`, `control/profile*` or `control/autotune*` calls
`lvgl_port_lock`. Nothing enforces that, though. Recommended: fix the comment, and
either accept the stall knowingly or move the ack off the LVGL task with a deferred
job and a result flag polled by `refresh_cb`. `relay_cycles_reset()` from the Relay
Life page already has the same pattern.

### LOW-3: a clear and an ack running at the same time can bring back an acknowledged record
`crash_report.c`, `crash_report_acknowledge()` (load at ~463, persist dispatch ~488)
against `crash_report_clear()` (erase job ~517-526, refresh ~565).

**Failure scenario:**
1. The LCD Acknowledge runs on `lvgl` and calls `load()`, which succeeds.
2. At the same moment, web `POST /api/crash_report/clear` on httpd runs its own ack,
   then the erase job, then `refresh_unacked_cache()`, which reads false.
3. The LCD's ack persist job then runs on the worker. It writes the record back to NVS
   with `acknowledged=1`, after the erase.

**Result:** the coredump is gone but a record reappears. `/api/crash_report` and the
readiness item show "last crash acknowledged" for a crash the operator cleared. The
relay gate is not affected: the cache ends false either way.

`s_have_unacked_crash` is also a plain `bool` written from httpd and lvgl with no lock.
It is benign on its own, but it is the same "reset one side" shape: the cache and the
disk can disagree in this window.

Fix: do the load, modify and persist inside the worker job, so the worker serialises
the whole read-modify-write.

### LOW-4: one touch bounce can do the two-tap confirm
`firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c` `crash_ack_btn_clicked_cb`
(~1468-1485). The second `LV_EVENT_CLICKED` counts as confirmation if it arrives any
time before the 5 s deadline. There is no minimum gap after arming.

A touch bounce, or a release/press glitch from the FT6336U, produces two CLICKED events
milliseconds apart. That arms and confirms the ack from one physical tap.

The impact is limited. Acknowledging does not energise anything; it re-enables manual
relay-ON and stops the crash from blocking readiness. The Relay Life Reset has the same
pre-existing weakness. Fix: ignore a confirm less than about 300-500 ms after arming,
in both callbacks.

### LOW-5: the static label "Unacknowledged crash" is shown when no unacknowledged crash exists
`ui_page_diagnostics.c` `build_crash_report_ack_row` (~1508-1510). The row's name label
is fixed text. `refresh_cb` hides only the button (~1441-1449), so a board with no
record, or an acknowledged one, still shows a red-accented card reading "Unacknowledged
crash". Hide the whole row, or change the label text in `refresh_cb`.

### INFO
- **Blocking NVS read every 2 s:** `refresh_cb` (~1419-1421) calls `crash_report_get()`
  every `UI_PAGE_DIAGNOSTICS_REFRESH_MS` (2 s), whichever page is shown. That is a
  blocking NVS read under the LVGL lock. The stack is internal SRAM, so it is not a
  cache-disable hazard, but it is avoidable I/O. Gate it on the Crash Report page
  being visible.
- **Stack:** the diagnostics `refresh_cb` frame is `entry a1, 0x350` (848 B), and
  `crash_ack_btn_clicked_cb` plus `crash_report_acknowledge` add 32 + 352 B. Both are
  well under the `ui_home_refresh_cb` dispatch depth (2656 B frame) that the budget
  checker already charges to `lvgl`. No new task was added, so no stack-margin
  registration was needed.
- **No-scroll check:** the page is in `page_names[]` and `PAGE_COUNT` (8 pages),
  prev/next are bounded by `PAGE_COUNT`, and there is a budget `_Static_assert`. The
  worst-case summary is about 152 chars, within the 4-line allowance at either panel
  orientation. No new colours: only `UI_THEME_ACCENT_5`, `UI_THEME_ACCENT_1`,
  `UI_THEME_COLOR_CARD` and `UI_THEME_COLOR_TEXT_*`.
- **Gate logic unchanged:** `relay_on_blocked()` is untouched (`kiln_io_owner.c:203-236`).
  Danger mode still bypasses first, and `recovery_mode`/`estop_verified` are still not
  gated. The new precedence and danger-mode tests drive the real static function
  through mutable stubs, which is meaningful.
- **Other readers of the unacked state:** the readiness gate (`readiness_http.c:780`)
  and web `/api/crash_report` read disk, not the cache. `relay_on_blocked()` and the
  LCD read the cache. The new re-derive in `clear()` keeps these consistent apart from
  the LOW-3 race. `dashboard_http.c:646` only logs.
- **Web ack status codes:** `diagnostics_http.c` checks presence before acknowledging
  and returns 409 or 500 correctly. The presence-then-ack gap is harmless. The 500 path
  has no host test.
- **Lost coverage in `test_dualwrite_window.c`:** it now stubs `crash_report_get()` to
  always return false. The `crash_pending` branch of `dualwrite_window.c` has no
  coverage anywhere. The commit says it had none before either; this is noted, not a
  regression.
- **Message lengths:** the CT-sweep reason strings are 52 bytes or less, within
  `reason[64]`. The LCD refusal message is about 53 chars, within `msg[64]`. The target
  build accepted both under `-Wformat-truncation`.

## Verdict
The commit is complete, builds for the target, and passes all 46 host-test executables
at a clean HEAD. There are no blocking defects. Fix LOW-1 (the vacuous routing test)
and the false PSRAM rationale in LOW-2 before anyone relies on either.
