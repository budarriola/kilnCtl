# Adversarial review: heat-enable release/enable race fix (commit 059a896e)

Read-only review, 2026-09-15. No flash, no board access. `059a896e` claims to
close the findings of `docs/audits/review_executor_panic_fix_1c8d7f6e_2026-09-15.md`
against the earlier deferred-release commit `1c8d7f6e`.

Files changed by `059a896e`:

* `firmware/KilnFW/App/drivers/control/heat_enable.c`
* `firmware/KilnFW/App/drivers/control/heat_enable.h`
* `firmware/KilnFW/App/drivers/control/profile_executor_run.c`
* `firmware/KilnFW/App/drivers/control/profile_executor_status.c`
* `firmware/KilnFW/App/test/test_heat_enable.c`

## Verdict

**Not closed.** HIGH-1's original interleaving is genuinely fixed, and LOW-5 is
genuinely fixed for the release direction. But the mechanism chosen to fix
HIGH-1 introduces a new HIGH: an unbounded-in-practice blocking wait on httpd
and UART-bridge stacks, whose "bounded 200 ms" justification in the source
comment is arithmetically wrong. MEDIUM-3 is only half closed (the autotune
caller was not touched), the fix moves the deep UART release chain onto the
4096 B watchdog task — reintroducing the stack hazard `1c8d7f6e` existed to
remove — and the safety-critical stuck-ON direction retains exactly the LOW-5
defect that was fixed in the stuck-OFF direction.

## Verification performed

All measurements are from a clean worktree, never from the main tree and never
from a prebuilt binary.

* Worktree `C:\wt\rvexec`, detached at `b40c894b` (then origin/main).
  `git diff 059a896e b40c894b --` over all five reviewed files is **empty**, so
  measurements at that worktree are valid for the commit. Submodules
  initialised; `idf.py set-target esp32s3` run explicitly, since the gitignored
  sdkconfig is not provisioned and otherwise targets plain `esp32`.
* **Host tests at HEAD:** all 46 executables built and passed (fresh `-OutDir`).
  Two harness SKIPs (`firing_score_from_capture`, `sim_credibility_gate`) from
  absent gitignored `logs/coupling/*.jsonl` captures — expected in a fresh
  worktree.
* **Are the two new tests load-bearing?** Yes. Building the two new tests
  against the **parent** commit `b2e7017f`'s `heat_enable.c` produces
  **7 assertion failures**: 4 in
  `test_reenable_cannot_overtake_an_inflight_release` (lines 439, 442, 446, 447)
  and 3 in `test_failed_release_is_retried_not_dropped` (lines 478, 483, 485).
  Neither test passes against the old code.
* **Negative test (hand-poisoned production source, fresh directory each
  build):** HEAD clean, 0 FAILs -> poisoned `heat_enable.c` by hand, FULL
  rebuild into a fresh directory, exactly 7 FAILs at the same line numbers ->
  restored **by hand** (no `git checkout --`, no `git restore`, no `git stash`),
  `git diff --exit-code` returns 0 -> forced another FULL rebuild into a second
  fresh directory, 0 FAILs. (`git status --porcelain` briefly showed ` M` on the
  restored file while `git diff` was empty: a CRLF/stat-cache artifact, not a
  content difference.)
* **Target build:** `idf.py build` in the clean worktree, exit 0, 27% of the app
  partition free.
* **`check_executor_task_stack_budget`:** OK. Deepest static path from
  `executor_task_entry` is 1776 B (ceiling 1936 B of a 4096 B stack); honest
  free 1100 B (26.9%), classified LOW. Identical to the prior review's figure,
  so MEDIUM-3's acquire move did not change executor depth.
* **`check_all_task_stack_budgets`:** OK on a settled ELF — 0 of 28 tasks fully
  measured, all 28 INDETERMINATE (unresolved indirect calls). An earlier FAIL
  in this worktree (`gpio_probe: could not resolve root symbol
  'gpio_probe_task'`) did not reproduce against the finished ELF and is an
  in-flight/partial-ELF artifact, not caused by `059a896e`, which touches no
  bridge or task-table file. `profile_exec_wdt` reports a 1696 B **lower bound**
  of its 4096 B stack — a lower bound, not a measurement, which matters for
  finding 2 below.
* **`tools/run_all_checks.ps1`** (run with `-ExecutionPolicy Bypass`; the
  worktree's missing `tools/PcTools/.venv` was provisioned with `uv sync`,
  exit 0): **2 of 94 checks FAILED**, neither attributable to `059a896e`:
  * `tools/check_mykicad_golden_suite_runs.ps1` — "venv python not found at
    `C:\wt\rvexec\tools\mykicadMcp\.venv\Scripts\python.exe`". Environmental:
    that submodule venv is not provisioned in a fresh worktree.
  * `tools/PcTools/selfcheck.py` — `[FAIL] zones GET top-level keys ... missing
    from client: ['safety_tc_type_known']`. Stale worktree, not a defect: main
    has since advanced to `2cc69ebb`, "PcTools: mirror N5's new
    safety_tc_type_known GET field", which is precisely this fix.
  * Note that `check_00_kilnfw_target_build.ps1` mirrors the **main** tree's
    dirty WIP by design and is never evidence about HEAD.
* The main tree carries other sessions' uncommitted WIP throughout. No reviewed
  file was modified there by this review; `git status --porcelain` over all five
  is empty.

## Findings, by severity

### HIGH 1 (new): the flush can block a UI or bridge task for seconds, and the comment's bound is wrong

`send_enable()` calls `he_flush_release_blocking()` before its own exchange.
That helper loops up to `HE_FLUSH_MAX_ATTEMPTS` (20) times with
`HE_FLUSH_RETRY_MS` (10 ms) of `vTaskDelay` between attempts, which is where the
"bounded 200 ms" claim comes from. But inside the loop it calls
`heat_enable_service_pending_release()`, which performs the **blocking link
exchange itself** on the caller's stack. The 200 ms is the delay budget only; it
is not a bound on the loop's wall time.

Worst case is therefore roughly one full release exchange —
`SAFETY_XACT_LOCK_TIMEOUT_MS` is 5000 ms
(`firmware/KilnFW/App/drivers/safety/safety_link_internal.h:46`) plus a reply
wait of about 345 ms (`safety_link.h:619`) — plus the 200 ms of delays, on the
acquiring caller's own stack.

Who that caller is:

* the **httpd** task, via `dashboard_exec_http.c:750/770/780`;
* the **UART bridge** task, via `uart_bridge_ext_control.c:502/512/518`.

So a start/resume request over HTTP or the PC link can block its task for
multiple seconds on a sick link. The source comment justifying the constant
("200 ms comfortably under `SAFETY_XACT_LOCK_TIMEOUT_MS` ... so a healthy
link's own release exchange always finishes inside this window") has the
reasoning inverted: being *under* the transaction timeout is exactly what makes
the flush unable to outwait a slow exchange, and the exchange is run by this
very loop rather than by some other task it is waiting on.

`safety_poll_task` itself is not newly blocked — its drain call at
`safety_link_poll.c:358` is the non-blocking `heat_enable_service_pending_release()`
path it already had.

**Answer to "what is the worst-case release latency, and is the drain bound
stated anywhere?"** — About 5.5 s per stuck exchange for an acquire-side flush;
the release itself still drains at poll cadence (about 0.5-1 s healthy, about
5.5 s in backoff, per MEDIUM-4 of the prior review, unchanged). The only bound
stated anywhere is the 200 ms figure, and it is wrong.

### HIGH 2: the deep release chain now runs on `profile_exec_wdt`'s 4096 B stack

`heat_enable_reconcile()` now begins with `heat_enable_service_pending_release()`,
i.e. the full blocking UART release chain. `heat_enable_reconcile()` is called
every 2 s (`WATCHDOG_CHECK_PERIOD_MS`) from `watchdog_task_entry`, which runs on
the 4096 B `profile_exec_wdt` task (`profile_executor_start.c`). That is the
same stack size as `profile_executor`, the task whose suspected overflow
`1c8d7f6e` existed to prevent — so the deep chain has been moved from one 4096 B
task onto another 4096 B task, and specifically onto the guard task that is
supposed to survive the executor misbehaving.

The static checker cannot rule this out: `profile_exec_wdt` is INDETERMINATE
(1696 B is a lower bound only, unresolved indirect calls). This is not proof of
an overflow; it is the absence of the very headroom argument the original fix
relied on, applied to a task whose failure mode is worse.

### HIGH 3: the stuck-ON direction keeps the LOW-5 defect that was fixed for stuck-OFF

Asked directly: *can heat be left ENABLED when the executor believes it
released?*

`send_enable()` has no `enable_inflight` counterpart to `release_inflight`, so a
release can still overtake an in-flight enable=true. This is largely mitigated
by the existing `orphaned` check — after a successful enable, if `held_mask` has
gone to 0 meanwhile, `send_enable()` immediately sends enable=false. But that
compensating release is **not queued for retry**: on the orphan path the code
increments `release_sends` and does not set `release_pending = true` when the
send fails. That is precisely the LOW-5 defect the commit fixed in
`heat_enable_service_pending_release()` — left unfixed in the direction where
the failure mode is a heater the ESP believes it released and the Pico still
permits. Given the stated priority ("a stuck-on heater is far worse than a
stuck-off one"), this is the most consequential gap in the commit.

The per-tick backstop in `executor_task_entry` (a `heat_enable_release()` call
every `PROFILE_EXECUTOR_TICK_MS`, 1000 ms, when not RUNNING) does eventually
re-queue a release, so this is a window rather than a permanent state — but only
while the executor task is alive, which is the task under suspicion.

### MEDIUM 4: MEDIUM-3 is half closed — the autotune caller still acquires under its lock

The commit moved `heat_enable_acquire()` after `xSemaphoreGive(s_exec.lock)` in
both `profile_executor_run.c` (~line 949) and `profile_executor_status.c`
(`profile_executor_resume()`, ~line 237). Good.

But `autotune_engine.c:1348` still calls
`heat_enable_acquire(HEAT_ENABLE_CLAIMANT_AUTOTUNE)` inside
`autotune_begin_run_locked()` (which is documented "Returns with `s_at.lock`
HELD"), and `task_entry()`'s not-running branch calls `heat_enable_release()`
under `s_at.lock`. With finding 1, this is now *worse* than before the commit:
the lock is held across a call that can take seconds.

**Lock order is not inverted.** The invariant `s_exec.lock` then `s_at.lock`
holds on every path examined; `heat_enable`'s own `s_he.lock` is always released
before any send, and no path takes `s_exec.lock` while holding `s_at.lock`. No
AB-BA was found.

### MEDIUM 5: a new TOCTOU window from the MEDIUM-3 move

Releasing `s_exec.lock` before acquiring heat-enable opens a window the old
ordering did not have. A halt interleaving after `xSemaphoreGive(s_exec.lock)`
leaves state IDLE while `heat_enable_acquire()` then re-adds its bit to
`held_mask`, so K4 is granted with no run in progress. `heat_enable_acquire()`
sets `held_mask |= bit` unconditionally before any send, so nothing rejects the
stale acquire. It is bounded to about 1 s by the per-tick release backstop, and
the relays are separately de-energised, so this is a loss of the second pole's
interlock semantics rather than heat — but it is a regression introduced by this
commit's own fix.

### LOW 6: the tests do not exercise the paths they are named for

Both new tests are single-threaded, and the host stub's `vTaskDelay` is a no-op
(`App/test/stubs/freertos/task.h:60`), with `pdMS_TO_TICKS` an identity macro.
So `he_flush_release_blocking()`'s retry loop spins 20 times instantly and the
deferral/timeout branch is never exercised with real time or real concurrency.
The tests are still load-bearing (7 assertions fail against the parent), but
they pin the mutual-exclusion bookkeeping, not the blocking behaviour that
findings 1 and 2 are about.

### LOW 7: original HIGH-1 is genuinely closed for the interleaving it described

The `release_pending` / `release_inflight` pair plus the flush does close the
scenario in the prior review: an enable cannot be sent while a release is
pending or in flight, and the "clear on success" choice means a failed release
stays pending rather than being silently dropped. A re-enable arriving mid-drain
now blocks on `release_inflight` instead of overtaking it; a drain that times
out leaves `pending` set and the enable **refused** (returns false) rather than
sent, which is the fail-safe direction; concurrent callers serialise on
`s_he.lock` around the `go` test-and-set. The generation/latest-wins redesign
the prior review suggested was not required to close that specific race. The
cost is findings 1-3.

### LOW 8: root cause, and the safety invariants

* **Root cause (prior HIGH-2) is untouched.** `059a896e` adds no coredump
  symbolisation, no backtrace, no new evidence about the original
  `profile_executor` panic. The memory entry
  `project_profile_executor_panic_at_stop` **stays OPEN**, and finding 2 above
  arguably widens the suspect set rather than narrowing it.
* **`abs_max_temp_c` parity and Pico arming are unaffected.** The commit touches
  no safety-config, abs-max or arming file; the ESP/Pico ceiling contract and
  the Pico's armed state are untouched by this diff.
* **No lock is held across a producer or blocking call** on the two paths this
  commit fixed; one such hold remains on the autotune path (finding 4).
* **No flash-worker dispatch** is added from code already on the flash worker.

## Summary

| # | Sev | Issue | Prior finding |
|---|-----|-------|---------------|
| 1 | HIGH | `he_flush_release_blocking()` runs the blocking exchange inside its own retry loop: worst case about 5.5 s on the httpd or UART-bridge stack. The "bounded 200 ms" comment is wrong and its reasoning inverted. | new, from HIGH-1's fix |
| 2 | HIGH | `heat_enable_reconcile()` now runs the deep release chain on `profile_exec_wdt`'s 4096 B stack — the hazard `1c8d7f6e` existed to remove, relocated onto the guard task. | new |
| 3 | HIGH | Orphan-path release failure is not queued for retry — LOW-5's defect left unfixed in the safety-critical stuck-ON direction. | LOW-5, partial |
| 4 | MED | Autotune still acquires heat-enable under `s_at.lock`; now worse given finding 1. Lock order itself is correct, no AB-BA. | MEDIUM-3, half closed |
| 5 | MED | New TOCTOU: halt between unlock and acquire grants K4 with state IDLE, bounded to about 1 s by the per-tick backstop. | new, from MEDIUM-3's fix |
| 6 | LOW | New tests are single-threaded and `vTaskDelay` is a no-op stub; the blocking path is not exercised. | LOW-7 |
| 7 | LOW | Original HIGH-1 interleaving genuinely closed, including mid-drain re-enable, drain timeout and concurrent callers. | HIGH-1, closed |
| 8 | LOW | Root cause still unproven; memory entry stays OPEN. `abs_max_temp_c` parity and Pico arming untouched. | HIGH-2, untouched |

Both new tests are load-bearing (7 assertions fail against parent `b2e7017f`).
HEAD builds and passes 46/46 host tests in a clean worktree; the two
`run_all_checks` failures are environmental and are not attributable to this
commit.
