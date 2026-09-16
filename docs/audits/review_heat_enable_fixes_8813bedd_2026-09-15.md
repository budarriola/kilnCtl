# Adversarial review: deferred heat-enable release + autotune lock ordering (commit 8813bedd)

Read-only review, 2026-09-15. No flash, no board access, no source change. The
commit under review is the tip of `origin/main` at review time and claims to
close the HIGH findings of
`docs/audits/review_executor_race_fix_059a896e_2026-09-15.md`.

Files changed by `8813bedd`:

* `firmware/KilnFW/App/drivers/control/heat_enable.c`
* `firmware/KilnFW/App/drivers/control/autotune_engine.c`
* `firmware/KilnFW/App/test/test_heat_enable.c`
* `firmware/KilnFW/App/test/test_autotune_engine_prestart.c`

## Verdict

**Partially closed.** HIGH-1's real defect (the blocking exchange driven from
inside the retry loop) is genuinely fixed: the exchange is now driven at most
once and the loop that follows is a passive poll. HIGH-3 is genuinely fixed.
MEDIUM-4 is genuinely fixed for the acquire path and, as a bonus the commit
message does not claim, for the autotune per-tick release backstop too.

But three things do not hold up:

* the new "roughly 5.5 s" bound is **mis-derived**. The ~345 ms reply-wait term
  it cites is on a branch this call never takes. The commit that exists to
  replace a wrong bound comment installs a differently wrong one.
* HIGH-2 is **partially closed only**. The deep UART chain no longer runs
  unconditionally on the 4096 B `profile_exec_wdt` stack, but it is still fully
  reachable there, on the conditional retry path, in precisely the link-down
  scenario that matters. And the same task can now block for ~5 s on that path,
  ahead of every guard-9 staleness check in its own loop body.
* MEDIUM-5's accepted risk is **worse than the comment implies**: the existing
  orphan-compensation branch provably cannot fire in that window, and the
  condition is not merely unalarmed, it is actively invisible to the Pico.

Nothing in this commit bears on the open `profiles_stop` panic. See "Root cause"
below.

## Verification performed

All measurements from a clean worktree at `origin/main`, never from the main
tree and never from a prebuilt binary.

* Worktree `C:\wt\execrv_q7m3k9`, `git fetch` first, HEAD `8813bedd`,
  `git status --porcelain` empty throughout. Submodules initialised.
* **Target build: PASSES.** `idf.py set-target esp32s3` then `idf.py build`,
  exit 0, "Successfully created ESP32-S3 image", `KilnCtrl.bin` 2,289,760 bytes.
  `check_00_kilnfw_target_build.ps1` was deliberately not used as evidence — it
  mirrors the main tree into a shared directory and says nothing about HEAD.
* **A note on the reported "pre-existing target build break".** A first build
  attempt in this worktree did fail with
  `TEMPERATURE_SENSOR_CLK_SRC_DEFAULT undeclared` inside
  `hal_sysinfo_esp.c`. That is **not** a break in that file and not a break on
  `origin/main`. It is a wrong-target artifact: the first `set-target esp32s3`
  was refused ("Directory ... doesn't seem to be a CMake build directory"), so
  the build ran against the default `esp32` target, whose `esp_driver_tsens`
  header lacks that enumerator. Deleting `build/`, re-running `set-target
  esp32s3` and rebuilding succeeds with no source change. Anyone chasing that
  error should check `build/config` for the target before concluding anything
  about the source. `8813bedd` touches no hardware-abstraction file either way.
* **`tools/run_all_checks.ps1`** (`-ExecutionPolicy Bypass`, foreground, in the
  worktree, after `uv sync` provisioned `tools/PcTools/.venv`):
  **92 passed, 0 skipped, 2 failed of 94.** Neither failure is attributable to
  this commit:
  * `check_all_task_stack_budgets.ps1` — "could not resolve root symbol
    `gpio_probe_task`". Known: `CONFIG_KILNCTL_ENABLE_GPIO_PROBE` defaults to
    `n` in a regenerated `sdkconfig`, so the symbol is compiled out. Owned
    elsewhere; not touched here.
  * `check_flash_worker_lint.ps1` — an unguarded
    `uart_bridge_ext_run_on_flash_worker()` dispatch at
    `drivers/ui/ui_page_home_refresh.c:259`. A UI file this commit does not
    touch. Pre-existing, and a genuine instance of the standing "never dispatch
    to the flash worker from code already on it" invariant, so it deserves an
    owner even though it is out of scope here.
  * `check_mykicad_golden_suite_runs.ps1`, listed in the brief as a known
    environmental failure, **passed** in this worktree.

## Findings

### Finding 1 (HIGH-1): closed in substance, but the new bound comment is wrong

The mechanism is right now. `he_flush_release_blocking()` reads the outstanding
flags under the module lock, releases it, drives
`heat_enable_service_pending_release()` **exactly once** (and only when no other
caller already has a release in flight), then polls 20 times at 10 ms with the
lock dropped between reads. The pathology the prior review found — the blocking
exchange re-driven on every one of 20 iterations — is gone.

**The stated bound is mis-derived.** The new header comment says the wait is
bounded by `SAFETY_XACT_LOCK_TIMEOUT_MS` (5000 ms) "plus a reply wait of about
345 ms (`SAFETY_LINK_REPLY_TIMEOUT_MS`) -- plus this 200 ms passive poll ...
i.e. roughly 5.5 s". Derived independently from the code:

* `safety_link_request_enable()` (`safety_link.c:652`) ends with
  `return safety_exchange(link, request, sizeof(request), false);` — note the
  final argument, `expect_status == false`.
* `safety_exchange()` (`safety_link_inbox.c:745`) takes `xact_lock` with a
  `SAFETY_XACT_LOCK_TIMEOUT_MS` timeout, does one opportunistic
  `safety_drain_inbox(link, 0)` (zero wait), sends one broadcast frame, and
  enters the `SAFETY_LINK_REPLY_TIMEOUT_MS` wait **only inside
  `if (expect_status)`**.

REQUEST_ENABLE is a fire-and-forget broadcast with no ACK and no reply, so the
~345 ms term is on a branch this path never reaches. The true worst case is
5000 ms of lock contention, plus one local UART write, plus the 200 ms passive
poll: **about 5.2 s, not 5.5 s**. The magnitude is close enough that the safety
conclusion does not change — which is exactly why this matters: a comment that
lands on approximately the right number by summing the wrong terms will be
trusted the next time it is read, and the next reader will conclude that
shortening `SAFETY_LINK_REPLY_TIMEOUT_MS` shortens this bound. It does not.

The 5000 ms term is the whole bound. It is a *lock* timeout, so the flush's
worst case is governed entirely by how long some other task can hold
`xact_lock` — and the longest holder in the tree is not an ordinary exchange at
all but the rollback path in `safety_link_commands.c`, which holds `xact_lock`
across a multi-repeat send burst with drains between repeats.

**Who calls it, and on what task.** `he_flush_release_blocking()` is reached
only via `heat_enable_acquire()` -> `send_enable()`. The acquire call sites are:

| Call site | Task |
|---|---|
| `profile_executor_run.c:949` (run start) | httpd, UART bridge, or LVGL/UI |
| `profile_executor_status.c:237` (`profile_executor_resume()`) | httpd, UART bridge |
| `autotune_engine.c:1416`, `:1525` | httpd, UART bridge |
| `heat_enable_reconcile()` -> `send_enable("retry")` | **`profile_exec_wdt`** |

Start/resume is reachable from `ui_page_home_actions.c:44`, so the LVGL/UI tick
is in scope for the multi-second block as well as httpd (`recv/send_wait_timeout
= 3` s, so a 5 s block can outlast an HTTP socket timeout) and the bridge.

**It is not on the flash worker, and it does not trip the ESP-IDF Task WDT.**
`CONFIG_ESP_TASK_WDT_PANIC=y` with a 5 s period would be a live hazard against a
5.2 s bound, but a grep finds **zero `esp_task_wdt_add()` call sites** — only
the two idle tasks subscribe, and a task blocked in `xSemaphoreTake` yields the
CPU, so the idle tasks still run and still feed. This is safe today by the
accident of nobody having subscribed a task. The moment any task in this chain
is subscribed, a 5.2 s bound against a 5 s panic-on-expiry watchdog becomes a
reset.

**Expiry behaviour is fail-safe.** A flush returning false makes `send_enable()`
set `s_he.pending = true`, log once (throttled by `warned_pending`) and send
**nothing**. No enable=true reaches the wire, so K4 is not granted; the ESP's own
zone relays are gated separately and are not energised by this path. Heat-enable
state stays internally consistent: the claim is recorded in `held_mask`, the
send is owed, and `heat_enable_reconcile()` retries it later. Correct direction.

**Verdict: closed for the defect, not closed for the documentation.**

### Finding 2 (HIGH-2): partially closed — the chain is off the *unconditional* path only

`heat_enable_reconcile()` no longer begins with an unconditional
`heat_enable_service_pending_release()`. It now takes the lock, computes
`want_retry = s_he.pending && s_he.held_mask != 0u && !s_he.granted`, drops the
lock, and returns early unless a retry is genuinely owed. Good, and materially
better than before: the deep chain is off the 2 s tick in the normal case.

But the retry path calls `send_enable("retry")`, which calls
`he_flush_release_blocking()`, which can call
`heat_enable_service_pending_release()` -> `safety_link_request_enable()` ->
`safety_exchange()` -> `uart_protocol_send_broadcast()`. That is the full UART
chain, on `profile_exec_wdt`'s **4096 B** stack, and `want_retry` is true in
exactly one situation: a run was started while the safety link was down, so the
enable was refused and stayed pending. That is not an exotic corner — it is the
scenario the retry exists for.

Two consequences the commit does not address:

1. **Stack.** `check_all_task_stack_budgets.py` reports `profile_exec_wdt` as
   INDETERMINATE with a 1696 B **lower bound** against a `CEILING_BYTES` entry
   of 2496 B on a 4096 B stack. The checker says in its own source that
   INDETERMINATE "is not a pass claim". See finding 6.
2. **Latency.** In `watchdog_task_entry()` the loop body is
   `vTaskDelay(2000 ms)` and then, at `profile_executor.c:1614`,
   `heat_enable_reconcile()` — **first**, ahead of every guard-9 staleness and
   trip check in the same iteration. A 5.2 s block there does not merely delay
   the retry; it delays the tick-dead check (`WATCHDOG_TICK_DEAD_MS` 10000 ms)
   and the 30 s safety-link silence abort behind it, pushing guard 9's effective
   detection latency from roughly 10-12 s toward roughly 15 s. The guard task
   is supposed to be the thing that still works when the executor does not;
   putting a five-second blocking call at the top of its loop is the wrong shape
   even when the stack turns out to fit. Moving the `heat_enable_reconcile()`
   call to the **end** of the loop body would cost nothing and remove the
   latency coupling entirely.

**Verdict: partially closed.**

### Finding 3 (HIGH-3): closed

The orphan branch in `send_enable()` now sets `s_he.release_pending = true` when
the compensating `enable=false` fails, and logs at ERROR with "queued for
retry". Checked against the four failure modes asked for:

* **Cannot loop forever.** The retry is not a loop. It is a flag drained once
  per `safety_poll_task` iteration at `safety_link_poll.c:358`, at poll cadence.
* **Cannot lose a release permanently.** `heat_enable_service_pending_release()`
  clears `release_pending` only on `ESP_OK` and always clears
  `release_inflight`, so a failure leaves the flag set and the next poll retries.
* **Cannot double-release harmfully.** `enable=false` is idempotent and is the
  fail-safe direction; `safety_link_request_enable()` attempts it
  unconditionally even with the link down, unlike `enable=true`.
* **"Reset one side of a pair" check: clean.** REQUEST_ENABLE carries no
  sequence number, no dedup ring and no ACK — it is a fire-and-forget broadcast
  and the Pico keeps no per-request counter for it. There is therefore no
  counterpart state to clear alongside `release_pending`. (Contrast
  `trip_seq`/`trip_last_seq`, which is a real instance of that class.) The only
  paired state is `release_pending`/`release_inflight` inside this module, and
  the orphan branch sets `release_pending` without ever touching
  `release_inflight` — benign here, and only because the send has already
  returned by that point, so no drain is in flight to be confused. Worth a
  comment; not a defect.

One cosmetic wart: the orphan branch increments `s_he.release_sends` before it
knows the outcome, so that counter counts attempts, not successes, on this path
while counting something closer to successes elsewhere. Diagnostics only.

**Verdict: closed.**

### Finding 4 (MEDIUM-4): closed, and slightly more than claimed

`autotune_begin_run_locked()` no longer acquires heat-enable. Both callers
(`autotune_engine.c:1416` in `autotune_engine_run()`, `:1525` in
`autotune_engine_run_to_target()`) now call
`heat_enable_acquire(HEAT_ENABLE_CLAIMANT_AUTOTUNE)` after
`xSemaphoreGive(s_at.lock)`, mirroring the pattern `059a896e` established for
the executor. The standing "never hold a module lock across a blocking call"
invariant now holds on the autotune start path too.

The commit message does not mention it, but `task_entry()` was also
restructured so the per-tick not-running backstop
(`heat_enable_release(HEAT_ENABLE_CLAIMANT_AUTOTUNE)`, `autotune_engine.c:861`)
runs **after** the unlock rather than under `s_at.lock`. That closes the second
half of the prior review's MEDIUM-4, which had called out both the acquire and
that release. The teardown funnel in `autotune_engine_guard.c:165` is unchanged
and was already outside the hazard.

Lock order is unchanged and correct: nothing takes `s_exec.lock` while holding
`s_at.lock`, and `s_he.lock` is released before every send.

**Verdict: closed.**

### Finding 5 (MEDIUM-5): the accepted risk is understated, and a comment is not enough

The commit explicitly accepts, rather than fixes, the window between
`xSemaphoreGive(s_at.lock)` / `xSemaphoreGive(s_exec.lock)` and
`heat_enable_acquire()`. Three things need saying plainly.

**The existing orphan compensation provably cannot save this case.** The
sequence is: halt runs in the window, clears `held_mask` and queues the release;
the interrupted starter then resumes and calls `heat_enable_acquire()`, which
sets `held_mask |= bit` **before** `send_enable()` runs. `send_enable()` then
evaluates `orphaned = (err == ESP_OK) && (s_he.held_mask == 0)` — and
`held_mask` is non-zero, because the acquire just re-set it. So the orphan
branch does not fire, and K4 is granted with the executor (or autotune) in
IDLE. The only thing that ends the condition is the per-tick not-running
backstop, one tick later (1000 ms for both modules).

**The concrete hardware consequence.** K4 is the Pico's heat-permission relay:
granting it does not by itself energise a heater. The ESP's zone relays go
through `kiln_io_owner` and `relay_authority`, both of which were torn down by
the halt, so no element is driven during the window. What is lost is the second
pole of the interlock: for up to ~1 s the system is one erroneous relay write
away from heat instead of two, having explicitly told the safety processor that
heat is permitted while no run exists.

**It is not an alarmable divergence — it is an invisible one.** The brief asks
whether the ESP's and the Pico's views disagree. They do not, and that is the
problem. `KILNLINK_CONTEXT_FLAG_HEAT_OWNER_ACTIVE` is computed at
`safety_link_frames.c:505-520` from, among other terms,
`heat_enable_is_held(PROFILE) || heat_enable_is_held(AUTOTUNE)` — i.e. from
`held_mask`, the very field the stale acquire just re-set. So during the window
the Pico is told "heat owner active" and its own gates (including the tc-type
change gate at `link_task.c:1621`) behave exactly as if a run were in progress.
The standing rule that config divergence between processors must alarm rather
than silently reconcile is not violated, because there is no divergence to
detect: both processors agree on a state that is false. Nothing anywhere logs
this, and the recovery is a silent self-heal one tick later.

That is a materially worse characterisation than "K4 is granted with state IDLE
for about 1 s, documented in a comment". A comment is not an adequate control
for a state that (a) defeats the module's own compensating branch by
construction, (b) is undetectable from either processor, and (c) is recovered
only by a backstop on the same task class that is already under suspicion for
the open panic. The cheap fix is not a redesign: have `heat_enable_acquire()`
take a caller-supplied "still starting" predicate, or have the halt path set a
generation/epoch counter that the deferred acquire compares against and refuses
on mismatch. Either turns a silent window into a refused acquire plus a log line.

**Verdict: accepted risk, but the acceptance rests on an understated premise.**

### Finding 6 (MEDIUM): the stack argument is honest but is not a pass

The commit's reasoning — that `profile_exec_wdt` stays at a 1696 B INDETERMINATE
lower bound because the static walk still sees the chain through the conditional
`send_enable("retry")` path — is *correct as a description of the checker's
output* and the commit is right not to claim an improvement it cannot measure.
But three separate numbers are being conflated in that argument:

* **4096 B** — the declared stack, `profile_executor_start.c:129/:139`.
* **2496 B** — `CEILING_BYTES["profile_exec_wdt"]`, the budget the checker
  enforces.
* **1696 B** — a static **lower bound**, produced by a walk that gave up at
  unresolved indirect calls (`callx4/8/12`). The checker's own source says
  INDETERMINATE "is not a pass claim for those tasks".

So the true worst-case depth on that task is **not established** by anything in
this commit or in the tooling today. The observational evidence is a floor, not
a bound, and was captured under conditions that exclude the path in question:
the `2bcdc2d` idle baseline records 3004 B free of 4096, `DRAM_PSRAM_STATUS.md`
records 1840 B free (44.9%) — both with no firing, therefore with `want_retry`
false, therefore with the UART chain never entered. The one scenario that
exercises the deep path is the one nobody has measured.

What the commit genuinely achieves is a large reduction in *exposure*: the chain
went from "every 2 s tick, always" to "only while an enable is owed and
ungranted". That is worth having. It is not the same as showing the stack is
safe, and the review should not be read as saying it is.

### Finding 7 (LOW): the rebase/cherry-pick premise is false, so the claim is safe for a different reason

The commit is described as rebased over `60d6552f`, which allegedly touched the
same two files, with a clean merge and no semantic collision. Verified
independently:

* `git show --stat 60d6552f` lists 15 files across http, persist, safety and
  test. **Neither `heat_enable.c` nor `autotune_engine.c` is among them.**
* Ancestry is strictly linear — `60d6552f` then `f64315c1` then `d2d2097e` then
  `8813bedd` — and `8813bedd` has the single parent `d2d2097e`. No merge commit,
  no cherry-pick, nothing to collide.
* `git log -- heat_enable.c` lists only `8813bedd`, `059a896e`, `1c8d7f6e` and
  `9f18ca5c`.

So the conclusion ("no semantic collision") is true, but the stated reason is
not: there was never a concurrent edit to those files to collide with. Diffing
both parents against the result is vacuous when there is only one parent. This
matters only as a reliability signal about the commit message's other claims.

### Finding 8 (LOW): the two new tests are load-bearing but pin text, not behaviour

Both additions to `test_heat_enable.c` and the addition to
`test_autotune_engine_prestart.c` are `test_read_source_anchored` source scans —
the established idiom in this repo for "function X must not call Y", and a
legitimate one. They would catch a regression that re-introduced the call, which
is the failure mode that actually happened twice here.

What they do **not** do is exercise the behaviour:
`test_flush_drives_the_exchange_at_most_once()` counts the string
`heat_enable_service_pending_release()` in the extracted function body and
asserts the count is 1 and that it precedes `for (int attempt`. A refactor that
moved the drive into a correctly-guarded helper would fail this test while being
correct; conversely a drive placed inside the loop *through a helper* would pass
it. `test_reconcile_never_drives_the_blocking_exchange_unconditionally()` skips
the leading comment with `strstr(body, "*/") + 2`, so it is defeated by any
second block comment later in the function. Adequate as a tripwire; not
evidence about timing or stack depth, which is what findings 1, 2 and 6 are
about.

### Finding 9 (LOW, out of scope but reportable): `check_flash_worker_lint` fails on clean origin/main

`drivers/ui/ui_page_home_refresh.c:259` dispatches to the flash worker with
neither an `is_on_flash_worker()` guard nor the sanctioned "not reachable
on-worker" justification. This is a live instance of a standing invariant and
the check that enforces it is red on a clean worktree. Not this commit's, not
fixed here, but it needs an owner.

### The missing host test, and the panic

**`test_heat_owner_active_decide.c`.** Established from the tree, then confirmed
by the coordinator: the file was **never committed and never deleted**.
`git log --oneline --all -- "*test_heat_owner_active_decide*"` returns nothing,
and no `heat_owner_active_decide.c` production source exists either — the
HEAT_OWNER_ACTIVE decision is still inline at `safety_link_frames.c:514`. The
dangling reference (the `Invoke-HostTestExe` block in `build_host_tests.ps1`
around line 1415, plus the `$totalExpected = 47` bump) was introduced by
`60d6552f`, which added the build recipe without the test or the extracted pure
function. Because `Invoke-HostTestExe` records a build failure into
`$script:buildFailures` and the script `exit 1`s, the host-test suite genuinely
failed on a clean `origin/main` worktree. So the three agents who called it
pre-existing and not theirs were right, but it was never "harmless": it was a
red gate attributable to a specific commit. It has since been resolved by a
separate commit that supplies the missing test. Reported, not fixed, as
instructed.

**Bearing on `project_profile_executor_panic_at_stop`: none.** This commit adds
no coredump symbolisation, no backtrace and no new evidence about the panic at
`profiles_stop`, and the implementing agent correctly made no claim that it
did. **This commit must not be read as closing that panic; the memory note
stays OPEN.** The one adjacent effect is finding 2: taking the deep UART chain
off the *unconditional* watchdog-tick path narrows the stack-overflow hypothesis
slightly, while leaving the conditional path reachable on the same 4096 B stack
and adding a ~5.2 s blocking window at the top of the guard task's loop. On
balance that is a small improvement to one hypothesis and a small enlargement of
another. It is not closure of either.

## Summary

| # | Sev | Issue | Claimed finding |
|---|-----|-------|-----------------|
| 1 | HIGH | Mechanism fixed (drive-once then passive poll), but the new bound comment is mis-derived: the ~345 ms reply wait is on a branch `expect_status == false` never takes. Real bound is about 5.2 s and is entirely `xact_lock` contention. Reachable on httpd, UART bridge, LVGL/UI and `profile_exec_wdt`. Expiry is fail-safe. | HIGH-1, closed in substance, doc wrong |
| 2 | HIGH | Deep UART chain is off the unconditional tick but still fully reachable on `profile_exec_wdt`'s 4096 B stack via `send_enable("retry")`, in the link-down case. `heat_enable_reconcile()` is the FIRST call in the watchdog loop body, so a ~5.2 s block delays guard 9 from about 10-12 s toward about 15 s. | HIGH-2, partially closed |
| 3 | - | Orphan-release failure is requeued; cannot loop, cannot be lost, cannot double-release harmfully; no paired counterpart state needs clearing (REQUEST_ENABLE has no sequence/dedup). | HIGH-3, closed |
| 4 | - | `heat_enable_acquire()` out from under `s_at.lock` at both callers; the per-tick release backstop was moved out too, which the message does not claim. Lock order unchanged and correct. | MEDIUM-4, closed |
| 5 | MED | The accepted K4-with-IDLE window defeats the orphan branch **by construction** (`held_mask` is re-set before the `orphaned` test) and is invisible to the Pico, because HEAT_OWNER_ACTIVE derives from that same `held_mask`. Not a divergence that alarms — an agreed-upon falsehood, recovered silently one tick later. A comment is not an adequate control. | MEDIUM-5, understated |
| 6 | MED | 4096 B declared, 2496 B budgeted, 1696 B INDETERMINATE lower bound: three different numbers. True worst-case depth on the deep path is unmeasured, and every observed HWM figure was captured with that path not entered. Exposure reduced, safety not shown. | stack claim |
| 7 | LOW | The rebase-over-`60d6552f` premise is false: that commit touches neither file, ancestry is linear, single parent. Conclusion safe, stated reason wrong. | cherry-pick |
| 8 | LOW | New tests are source-text scans: real tripwires, defeatable by refactor, and silent about timing and stack. | tests |
| 9 | LOW | `check_flash_worker_lint` FAILS on clean origin/main at `ui_page_home_refresh.c:259`. Pre-existing, out of scope, needs an owner. | new |

Target build of `8813bedd` **passes** in a clean worktree (ESP32-S3 image,
exit 0); the `hal_sysinfo_esp.c` error seen elsewhere is a wrong-target build
directory, not a source break. `run_all_checks.ps1`: 92 passed, 0 skipped,
2 failed of 94, neither attributable to this commit.
