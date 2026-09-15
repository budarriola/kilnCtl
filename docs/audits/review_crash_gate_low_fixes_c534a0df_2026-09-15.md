# Adversarial review of commit c534a0df (crash-gate LOW 1-5 fixes)

Reviewer: Opus 5, 2026-09-15. Read-only review; no production file was changed by
this pass (the one negative-test edit was restored by hand and re-proved by a
forced full rebuild, see "Verification performed").

Commit under review: `c534a0df` "Address five LOW findings from Opus review of the
crash-gate follow-ups (62e95bbd)". Source findings:
`docs/audits/review_crash_gate_followups_62e95bbd_2026-09-15.md` LOW-1..LOW-5.

## Verdict per finding

| Finding | Status |
|---|---|
| LOW-1 test asserts flash-worker dispatch | **CLOSED** — negative-tested, fails when the write is made directly |
| LOW-2 unbounded wait under the LVGL task | **NOT CLOSED** — comment only; behaviour unchanged (see MEDIUM 1/2) |
| LOW-3 ack/clear race serialized | **CLOSED** for the load-modify-write; residual gaps below (LOW 1/2) |
| LOW-4 two-tap debounce (both buttons) | **CLOSED** — arithmetic correct for crash-ack and Relay Life Reset |
| LOW-5 label text / hidden-page flash read | **CLOSED** with a residual staleness gap (LOW 3) |

## Findings, severity-ranked

### MEDIUM 1 — LOW-2 is documented, not fixed: an unbounded LVGL-task stall remains
`firmware/KilnFW/App/drivers/bridge/uart_bridge_ext.c:414-441`

The only change is a 21-line comment. `bx_run_on_internal_stack()` still does
`xSemaphoreTake(s_bx_lock, portMAX_DELAY)`, `xQueueSend(..., portMAX_DELAY)` and
`xSemaphoreTake(s_bx_done, portMAX_DELAY)`. Scenario: operator taps Acknowledge on
the diagnostics page while the httpd task is mid-profile-package import
(`kiln_cfg_store` writes NVS + cfg_fs through this same worker); `lvgl_task` blocks
inside the click callback for the entire duration of that job plus any job queued
ahead of it, so the whole LCD — including the topbar and any other page — is frozen
with no watchdog bound and no operator feedback. The commit message calls this
"accepted"; the audit finding asked for a bound. The comment's own proposed fix
(deferred job + result flag polled by the refresh timer) is sound and remains
unimplemented. Reviewer's position: accepting it is defensible for today's job set,
but the finding should be recorded as open/deferred rather than closed, because
nothing mechanical prevents a future long job (a cfg_fs format, a large package
import) from turning this into a multi-second freeze.

### MEDIUM 2 — the no-deadlock argument cites a symbol that does not exist
`firmware/KilnFW/App/drivers/bridge/uart_bridge_ext.c:421-428`

The comment's safety argument is "nothing under drivers/bridge, control/profile*,
or control/autotune* calls `lvgl_port_lock`". `lvgl_port_lock` does not exist
anywhere in the tree — a grep of `firmware/KilnFW/App` finds the identifier only
inside this comment; `drivers/ui/lvgl_port.h` exposes no lock API, and
`lvgl_port.c`'s only mutex is `s_inject.lock` (touch injection, taken with a
timeout). So the stated invariant is unfalsifiable as written, and the real
invariant it stands in for is broader and weaker: *no flash-worker job may block on
anything `lvgl_task` must produce* (an LVGL-serviced queue, a UI-thread callback, a
future `lv_*` mutex). That broader invariant is likewise unenforced. Readers who
later grep for `lvgl_port_lock` to check the claim will find nothing and may
conclude the hazard is gone. Fix: restate the invariant in terms that can actually
be checked, or add it to `flash_worker_lint.py` as a pattern.

Related nit (same block, line 430): "crash_report.c's refresh_cb is already polling
every 2 s" — `refresh_cb()` lives in `drivers/ui/ui_page_diagnostics.c`, not
`crash_report.c`.

### LOW 1 — a failed dispatch is now silent and indistinguishable from "no record"
`firmware/KilnFW/App/drivers/safety/crash_report.c:505-528`

When `uart_bridge_ext_run_on_flash_worker()` returns non-OK (worker not started,
queue send failed) the job never runs, so `ctx.had_record` stays `false`. The new
`if (!ctx.had_record) return false;` fires *before* the `ESP_LOGW("could not
persist crash-record acknowledgement...")` branch, so this path now returns `false`
with no log at all — whereas pre-c534a0df the caller had already `load()`ed a real
record and reached the warning. The LCD callback logs its own generic "failed --
try again" line, but the HTTP path (`diagnostics_http.c`) and any future caller
lose the distinction between "there was nothing to acknowledge" and "the flash
worker refused the job". Fix: carry `submit_err` through and log it explicitly.
Fails closed (gate stays blocking), so this is a diagnosability defect, not a
safety one.

### LOW 2 — `crash_report_clear()` is still two dispatches, not one serialized job
`firmware/KilnFW/App/drivers/safety/crash_report.c:563-586`

LOW-3's claim is "the whole read-modify-write is serialized". True for
`crash_ack_job()`. But `crash_report_clear()` calls `crash_report_acknowledge()`
(dispatch #1) and then dispatches `crash_clear_job()` (dispatch #2); another task's
job can run between them. The benign orderings dominate today (an ack landing after
an erase now correctly finds no record — that is exactly what the new test proves),
and a new record can only be captured at boot, so no resurrection scenario was
found. Noted because the serialization guarantee is per-job, not per-operation, and
a future caller that assumes otherwise would be wrong.

### LOW 3 — the crash summary label is stale for up to one refresh tick after navigating to the page
`firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c:493-517`, `:1211-1240`, `:1755`

The LOW-5 fix correctly gates the blocking `crash_report_get()` on
`s_page_index == UI_PAGE_DIAGNOSTICS_PAGE_CRASH_REPORT`, but `show_page()` does not
call `refresh_cb()`; only the 2 s timer repaints. So the first frames after the
operator pages onto Crash Report show whatever the label last held. On the *first*
visit that is the label's creation text (the sole unconditional `refresh_cb(NULL)`
at `:1755` runs with `s_page_index == 0`, so the summary label has never been
painted), i.e. an empty/placeholder summary under a correctly shown or hidden
Acknowledge row. The commit's own comment claims the row is handled "the instant the
operator turns to this page" — that is true of the row's visibility (the flag check
is unconditional) but not of the text beside it. One-line fix: call `refresh_cb(NULL)`
at the end of `show_page()`, or repaint the summary when the page becomes visible.

### LOW 4 — a stale "LVGL task stack is PSRAM-backed" claim survives, now contradicting the LOW-2 correction
`firmware/KilnFW/App/drivers/ui/ui_page_diagnostics.c:1417-1427`

`crash_report.c`'s header comment was corrected to say `lvgl_task`'s stack is static
internal SRAM. The Relay Life Reset callback in the same commit's other file still
reads "per DRAM_PSRAM_PLAN.md section 7.2 (the LVGL task's stack is PSRAM-backed)".
Two files in one commit now assert opposite facts about the same stack. The
dispatch remains correct either way (serialization is the real reason), but the
next reader gets the wrong model.

### INFO 1 — the LOW-3 test is structural, not concurrent
`firmware/KilnFW/App/test/test_crash_report.c:534-588`

`test_ack_job_does_not_resurrect_after_concurrent_clear()` calls `crash_clear_job()`
then `crash_ack_job()` directly and asserts the ack sees no record. That is the
right unit test given the synchronous stub (`bx_worker_stub.h` runs jobs inline, so
no real interleaving is expressible), and it genuinely pins the load-inside-the-job
shape — but it cannot fail if a future change moves `load()` back out to the caller
while leaving `crash_ack_job()` itself intact, because the test drives the job, not
`crash_report_acknowledge()`. A complementary assertion that
`crash_report_acknowledge()` performs no `load()` before dispatch (e.g. a fake-kv
read counter snapshot around the dispatch) would close that.

### INFO 2 — the debounce reconstructs the arm time from the deadline
`ui_page_diagnostics.c:1401-1412` and `:1509-1517`

`now < deadline - CONFIRM_US + DEBOUNCE_US` is arithmetically correct for both
buttons as written (both deadlines are set as `now + CONFIRM_US` with the matching
constant, both `int64_t`, no overflow). It is nonetheless the "two pieces of state
joined by an unexpressed contract" shape CLAUDE.md warns about: if either
`*_CONFIRM_US` is ever changed in one place only, or a deadline is ever set by some
other expression, the debounce silently becomes wrong rather than failing. Storing
the arm timestamp alongside the deadline would make it self-evident.

## Explicit checks requested

- **uart_bridge_ext.c change is comment-only.** The diff adds no statement; other
  flash-worker callers (`safety_cfg_store`, control/profiles/autotune bridges,
  `relay_cycles_reset`, `cfg_fs`) are semantically unchanged.
- **`relay_on_blocked` behaviour unchanged.** The crash gate reads
  `crash_report_has_unacknowledged()`, whose cache is still set false only by a
  genuine ack, a clear, or `refresh_unacked_cache()`. The only behavioural delta is
  LOW 1's silent dispatch failure, which leaves the flag `true` — fails closed.
- **No new deadlock introduced by the LOW-3 restructuring.** `crash_ack_job()` takes
  no lock; it does `load()` + `persist()`, both plain `hal_kv_*` calls already made
  by `crash_clear_job()` on the same worker. No flash-worker job takes a UI lock (no
  UI lock exists — see MEDIUM 2).
- **No dispatch from code already on the worker.** `crash_report_acknowledge()` and
  `crash_report_clear()` both guard with `uart_bridge_ext_is_on_flash_worker()` and
  call the job inline when true; `bx_run_on_internal_stack()` additionally detects
  the re-entrant case by task identity. `python firmware/KilnFW/App/test/flash_worker_lint.py`
  at clean HEAD: **clean** (213 driver files scanned).
- **Two-tap debounce covers Relay Life Reset**: yes, `relay_reset_btn_clicked_cb()`
  got the same guard, per relay.

## Verification performed (clean worktree, origin/main = c534a0df)

Worktree `C:\wt\rv534` at `origin/main`, `git submodule update --init
firmware/KilnFW/components/lvgl`, board-tuned `sdkconfig` copied in (gitignored).
Worktree removed afterwards.

1. **KilnFW target build**: `check_00_kilnfw_target_build.ps1` — **PASS**.
2. **KilnFW host tests**: `build_host_tests.ps1` — **46/46 built and passed**.
3. **flash_worker_lint** — clean.
4. **LOW-1 negative test**: replaced the dispatch in `crash_report_acknowledge()`
   with a direct inline `crash_ack_job(&ctx)` call, rebuilt all 46 executables —
   `crash_report` **FAILED** at `test_crash_report.c:251` and `:271`, exactly the two
   new dispatch-count assertions, and nothing else. Source restored by hand (sed
   substitution back to the original text — no `git checkout`/`restore`/`stash`),
   `git diff` empty (0 lines, no `NEGTEST` residue), build output directories
   deleted and all 46 executables rebuilt from scratch: **46/46 pass**. The LOW-1
   assertion is load-bearing and not vacuous.

## `safety_ceiling_sync_divergence` at clean HEAD

**It passes at clean HEAD** (`35/35 checks passed`, part of the 46/46 run above).
Built from the dirty main tree it also **passes**, with `38/38 checks passed` — the
extra three checks come from another session's uncommitted edits. The dirty main
tree's host-test run does fail, but on a *different* executable:
`kilnctl_host_tests_safety_link` fails to link —
`error LNK2019: unresolved external symbol heat_enable_is_held` and
`danger_mode_active`, referenced from `safety_build_and_send_context` — caused by
the uncommitted `firmware/KilnFW/App/drivers/safety/safety_link_frames.c` edit in
the shared tree. Not a defect in c534a0df and not a defect in
`safety_ceiling_sync_divergence`; it is another session's work in progress.

## Recommendation

The three genuinely mechanical fixes (LOW-1, LOW-3, LOW-4) are correctly
implemented and, for LOW-1, proven by negative test. LOW-5 is substantially fixed
with one residual staleness gap (LOW 3 above). LOW-2 should be re-opened: it was
answered with documentation, and the documentation's central claim references a
symbol that does not exist.
