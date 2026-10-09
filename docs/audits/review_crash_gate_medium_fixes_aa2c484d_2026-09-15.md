# Adversarial review of commit aa2c484d (crash-gate MEDIUM 1/2 + LOW 1/3/4 + INFO fixes)

Reviewer: Opus 5, 2026-09-15. Read-only review of production code; the one
negative-test edit was restored by hand and re-proved by a forced full rebuild
into a fresh output directory (see "Verification performed").

Commit under review: `aa2c484d` "Fix MEDIUM/LOW findings from
review_crash_gate_low_fixes_c534a0df". Source findings:
`docs/audits/review_crash_gate_low_fixes_c534a0df_2026-09-15.md` (commit
`3d24acd3`).

## Verdict

**The commit is CLOSED for every finding it claims to fix.** All five claimed
fixes (MEDIUM 1, MEDIUM 2, LOW 1, LOW 3, LOW 4) plus the INFO test are genuinely
implemented, not documented-in-place. LOW 2 is correctly left open — no
resurrection scenario exists (searched, see below).

One **new MEDIUM defect was introduced by the LOW 3 fix**, in the same commit
whose headline is "stop blocking lvgl_task": `show_page()`'s new unconditional
`refresh_cb(NULL)` adds a cross-task blocking I/O read to *every* diagnostics
page switch. Details below.

| Finding | Claim | Verdict |
|---|---|---|
| MEDIUM 1 — unbounded LVGL stall | bounded-wait sibling + "Busy" | **CLOSED** (residual LOW 1/LOW 2 below) |
| MEDIUM 2 — comment cites nonexistent symbol | comment rewritten | **CLOSED** |
| LOW 1 — dispatch failure silent | `crash_ack_finish()` | **CLOSED** |
| LOW 3 — stale crash summary on page-in | `refresh_cb(NULL)` in `show_page()` | **CLOSED**, but see new MEDIUM 1 |
| LOW 4 — stale PSRAM-stack claim | comment corrected | **CLOSED** |
| INFO — test is structural only | `fake_kv_get_call_count()` + new test | **CLOSED** (forward guard, see INFO 3) |
| LOW 2 — `crash_report_clear()` two dispatches | left as-is per owner | **agree, do not reopen** |

## Safety direction: the gate fails CLOSED

This is the direction that matters, and it holds on every path.

`s_have_unacked_crash` (the flag `kiln_io_owner.c`'s `relay_on_blocked()` reads)
is written `false` in exactly one place after this commit:
`crash_ack_finish()` (`crash_report.c`), and only after either
`ctx->already_acked` or a real `ctx->had_record` with `ctx->err == ESP_OK`.
Trace of every new exit:

- **Bounded wait times out.** `crash_report_acknowledge_timeout()` returns
  `false` *before* `crash_ack_finish()` is ever called (the
  `submit_err == ESP_ERR_TIMEOUT` branch returns early). The cache is untouched,
  `fn` was never enqueued, nothing was read or written. Relay-ON stays blocked.
- **Dispatch fails for any other reason** (worker not started, `ESP_FAIL`).
  `crash_ack_finish()`'s first branch logs and returns `false` with the cache
  untouched.
- **Job ran but `persist()` failed.** `ctx->err != ESP_OK` branch logs and
  returns `false`, cache untouched.
- **Re-entrant caller already on the worker.** Runs `crash_ack_job()` inline and
  passes `ESP_OK` as `submit_err`, which is correct: the job genuinely ran.

No path grants relay-ON and no path silently marks a crash acknowledged. The LCD
"Busy" outcome specifically leaves the record unacknowledged and the row visible.

**Danger mode / recovery_mode / estop_verified — unchanged and still correct.**
`kiln_io_owner.c:~205-235`: `danger_mode_active()` is checked first and bypasses
every gate including the crash gate, logging when the bypass actually changed the
outcome. `recovery_mode` and `estop_verified` are deliberately *not* gated —
the doc comment still says so and cites
`docs/audits/manual_relay_readiness_gating_options_2026-09-15.md` sections 5 and
8. aa2c484d touches none of this.

## MEDIUM 1 (the original) — closed; the unwound-stack hazard is genuinely avoided

`uart_bridge_ext.c:455-487`, `flash_worker.h:38-65`.

I attacked this from the three angles asked for. The invariant holds in all of
them:

1. **A job that is enqueued and then times out cannot happen.** The bounded wait
   is `xSemaphoreTake(s_bx_lock, wait_ticks)` and it is strictly *before*
   `xQueueSend`. There is no second timeout anywhere. A caller that times out has
   provably not touched the queue, so `&ctx` on its stack is never published.
2. **A job whose argument lives on the caller's stack is safe.** After the
   bounded acquire succeeds, the caller holds the non-recursive mutex across
   `xQueueSend` *and* `xSemaphoreTake(s_bx_done, portMAX_DELAY)`, exactly like the
   unbounded sibling. The caller cannot return — and so cannot unwind `ctx` —
   until the worker has signalled completion.
3. **Two concurrent callers.** `s_bx_lock` is `xSemaphoreCreateMutex()`
   (`uart_bridge_ext.c:314`) and the job queue is `xQueueCreate(1, ...)`
   (`:312`). Because the previous holder releases the lock only *after* taking
   `s_bx_done`, the queue is always empty at the moment a new holder sends. So
   `xQueueSend(..., portMAX_DELAY)` cannot itself block, and there is never more
   than one job in flight. A second bounded caller simply waits on the mutex and
   gets `ESP_ERR_TIMEOUT` if it expires — the correct, fail-closed outcome.
4. **Re-entrant caller** (`bx_caller_is_worker_task`) runs `fn` inline with no
   queue interaction at all.

The stated design ("bound the acquire, never the completion") is the right call
and is implemented as described. `flash_worker_lint.py` at clean HEAD: **clean,
213 driver files scanned** — no dispatch from code already on the worker.

**Is lvgl_task genuinely bounded now?** Only on the acquire half, which is what
the commit message says. See LOW 1 below for the honest residual.

## MEDIUM 2 — closed

The rewritten comment (`uart_bridge_ext.c:414-443`) no longer names
`lvgl_port_lock`; it states the checkable invariant instead ("no flash-worker job
may ever block on anything lvgl_task itself must produce"), explicitly flags that
the previous wording named a symbol that exists nowhere in the tree, admits the
invariant is not mechanically enforced, and names `flash_worker_lint.py` as the
candidate enforcement point. The `refresh_cb()` misattribution sentence is gone
entirely (the comment no longer attributes it to any file). Both halves of
MEDIUM 2 are addressed.

## LOW 1 — closed

`crash_ack_finish()` (`crash_report.c:502-535`) checks `submit_err != ESP_OK`
*first*, with its own distinct `ESP_LOGW` naming `esp_err_to_name(submit_err)`
and saying "nothing was read or written", before the `!ctx->had_record` early
return can swallow it. Both `crash_report_acknowledge()` and
`crash_report_acknowledge_timeout()` funnel through it, so the HTTP path
(`diagnostics_http.c`) gets the distinction too. Diagnosability defect closed.

## LOW 3 — closed, and it introduced a new MEDIUM

The fix itself is correct and safe against the obvious hazard: I checked the
build order because `show_page()` is called from inside
`ui_page_diagnostics_build()` (`:1808`) and now dereferences page widgets.
`s_cr_summary_label` is assigned at `:1797` and `build_crash_report_ack_row()`
runs at `:1798`, both **before** `show_page(0)`. Every other label `refresh_cb()`
touches is created earlier still. No NULL deref at build time. The forward
declaration at `:391` is needed and correct. The summary is now painted on
page-in rather than up to one 2 s tick later.

But see NEW MEDIUM 1.

## LOW 4 — closed

`ui_page_diagnostics.c:1436-1450` now says lvgl_task's stack is static internal
SRAM (`lvgl_port.c`'s `s_lvgl_task_stack`, per the 2026-08-21 "REVERTED TO
INTERNAL SRAM" fix), explicitly retracts the `DRAM_PSRAM_PLAN.md` 7.2 claim, and
— correctly — keeps the flash-worker dispatch while restating its *real*
justification as serialization rather than stack placement. The two files in the
previous commit no longer contradict each other.

## LOW 2 — agree it stays closed; no resurrection scenario found

I looked for one specifically. A crash record is written in exactly two places:
`crash_report.c:422` (inside the boot-time capture path, reachable only from
`crash_report_init()`, called once from `main_boot_early.c:263`) and
`crash_ack_job()`'s `persist()` at `:499`. There is **no runtime capture path**.
So the only thing that can run between `crash_report_clear()`'s two dispatches is
another `crash_ack_job()` or another `crash_clear_job()`, and an ack against an
already-erased record correctly finds nothing (that is what
`test_ack_job_does_not_resurrect_after_concurrent_clear()` pins). Nothing can
re-create a record within a boot. **Not reopened.**

---

# New findings

## MEDIUM 1 (new) — the LOW 3 fix puts a blocking cross-task read on lvgl_task at every page switch

`ui_page_diagnostics.c:521-535` (the new `refresh_cb(NULL)` in `show_page()`),
against `refresh_cb()` at `:712-722`.

`refresh_cb()` opens with an **unconditional** `dashboard_get_status(&ds)`,
before any `s_page_index` gating. `dashboard_get_status()`
(`dashboard_http.c:134-164`) is not a cheap accessor: it calls
`kiln_io_owner_command_read()`, a cross-task `post_and_wait()` onto
`owner_task`'s queue bounded by `KILN_IO_OWNER_WAIT_MS`, plus
`relay_cycles_get()` and the rest of the status assembly. CLAUDE.md itself
characterises this getter as "three MAX31856 SPI reads, a 200 ms-capable queue
wait and four interrupts-disabled heap walks".

Before this commit that cost was paid once per 2 s refresh tick. Now it is *also*
paid synchronously inside every `prev_cb`/`next_cb` tap — seven extra invocations
to walk the diagnostics pages end to end, each able to stall lvgl_task for up to
`KILN_IO_OWNER_WAIT_MS` if `owner_task` is busy. Additionally, paging **to** the
Crash Report page now performs the blocking NVS `crash_report_get()` read
synchronously during the page switch rather than on the next tick.

The fix's own comment asserts the opposite: *"refresh_cb() is cheap when the page
it cares most about (Crash Report) isn't the one just switched to — the per-page
work above already gates its own expensive part (the blocking
crash_report_get() read) on s_page_index"*. That is true of the
`crash_report_get()` half only. It is **not** true of `dashboard_get_status()`,
which is ungated and is the larger of the two costs. The comment's claim that
this is "not a new per-page-switch I/O cost" is factually wrong as written.

Severity MEDIUM, not HIGH: it is bounded (`KILN_IO_OWNER_WAIT_MS`, and
`kiln_io_owner_command_read()` fails closed on timeout), it cannot deadlock, and
it does not touch the safety gate. But it is a regression of exactly the property
this commit set out to protect, shipped in the same change, and justified by a
comment that does not match the code.

Suggested fix: paint only the page being switched to — e.g. hoist the
per-page blocks into small helpers and have `show_page()` call just the one for
`index`, or gate the `dashboard_get_status()` call on `timer == NULL` being a
page-switch that actually needs it. A one-line alternative is to pass the new
index through and skip `dashboard_get_status()` for pages that do not consume
`ds`.

## LOW 1 (new) — lvgl_task is bounded only on the acquire half, with no mechanical guard on the other

`uart_bridge_ext.c:481-484`, `flash_worker.h:47-52`.

Answering the question directly: **if a job IS accepted and then runs long,
lvgl_task still blocks unbounded.** `xSemaphoreTake(s_bx_done, portMAX_DELAY)` is
unchanged in the bounded path. The header comment is honest about this — it
argues the caller's own job is "a short, bounded-in-practice write" — and that
argument is correct today (`crash_ack_job()` is one `load()` plus one
`persist()`). But nothing enforces it. This is the same *shape* as the invariant
MEDIUM 2 just got corrected for: a true-today claim with no mechanical guard. If
`crash_ack_job()` ever grows (a coredump summary re-fetch, a cfg_fs mirror
write), the 300 ms bound buys nothing.

Not a defect in this commit — it is the deliberate, documented design, and the
alternative (bounding `s_bx_done`) is genuinely unsafe for the stated
stack-lifetime reason. Recorded so the assumption is visible.

## LOW 2 (new) — the sibling unbounded lvgl_task caller was not converted

`ui_page_diagnostics.c:1436-1450`, `relay_reset_btn_clicked_cb()`.

The Relay Life Reset button on the adjacent page still calls
`relay_cycles_reset()`, which uses the **unbounded**
`uart_bridge_ext_run_on_flash_worker()`. The original MEDIUM 1 named this path
explicitly ("the same shape relay_cycles_reset() ... has already accepted"), and
the new comment in this very function still describes it as accepted rather than
fixed. So the identical unbounded-LCD-freeze remains reachable from one page
away, now that the bounded sibling exists and costs ~6 lines to adopt. Owner call
whether to convert it; flagged because "the LCD can no longer freeze
indefinitely" would be an incorrect reading of this commit.

## INFO 1 — the "Busy" label can outlive its meaning

`ui_page_diagnostics.c:1563-1578` vs `refresh_cb()`'s `:1285-1292`.

The timeout branch sets the button label to "Busy" and zeroes
`s_cr_ack_deadline_us`. `refresh_cb()` only ever rewrites that label when
`s_cr_ack_deadline_us != 0` — which the timeout branch has just ruled out. The
in-code comment states this correctly and calls it deliberate. The residual: if
the record is acknowledged from the web while "Busy" is on screen, the row hides
still reading "Busy", and would re-appear reading "Busy" rather than
"Acknowledge" if it were ever re-shown. Within a boot that cannot happen (records
are captured only at `crash_report_init()`), so this is cosmetic today. A
one-line `lv_label_set_text(s_cr_ack_label, "Acknowledge")` in the
`else` branch of the row-hiding block would remove the latent state.

## INFO 2 — `refresh_cb()` now runs twice during build

`show_page(0)` at `:1808` now calls it, and `:1814` calls it again immediately.
Harmless (idempotent), but it doubles the boot-time `dashboard_get_status()` and
the `:1814` call is now redundant.

## INFO 3 — the new test is a forward guard, not load-bearing against the parent

This was asked about directly, so stating it plainly: **the new test does not
fail at the parent commit.** I checked out `aa2c484d^`'s
`crash_report.c`/`crash_report.h` into a clean worktree while keeping aa2c484d's
`test_crash_report.c`, `fake_kv.c`/`.h` and `bx_worker_stub.h`, and built all 46
host-test executables from scratch: **46/46 built and passed, crash_report 85/85**.

That is expected and is exactly what the commit message claims — the
load-inside-the-job shape was already correct as of `c534a0df`, and the INFO
finding asked for a guard against a *future* regression that the existing
dispatch-count assertion could not catch. The test is therefore not evidence that
aa2c484d fixed a live bug; it is evidence that a specific future regression is now
caught. It is genuinely non-vacuous — proved by negative test below.

## LCD constraints — compliant

- **No new colour.** The diff adds no `lv_obj_set_style_*_color` call and no new
  `UI_THEME_*` constant. "Busy" reuses the existing button label object.
- **No scrolling / no layout growth.** The change is a text swap on an existing
  label; "Busy" (5 chars) is shorter than "Acknowledge" (11), so the Crash Report
  page's row arithmetic is unchanged and cannot overflow the 480x320 no-scroll
  budget.
- **No new task**, so no stack-margin registration is owed;
  `check_stack_margin_registration.ps1` passed in the suite run below.

---

## Verification performed

Clean worktree `C:\wt\rvcrash` at `origin/main` (`841a3a80`, which contains
`aa2c484d`), `git submodule update --init --recursive`, board-tuned gitignored
`sdkconfig` copied in, `uv sync` in `tools/PcTools`. A second clean worktree
`C:\wt\rvneg` (at `da6667f3`, origin/main having advanced mid-review; also
contains `aa2c484d`) carried the poison/parent experiments so nothing was built
from an edited tree that also ran the check suite. Neither experiment touched the
main tree. Both worktrees removed afterwards.

**1. `run_all_checks.ps1` — RAN**, `powershell -ExecutionPolicy Bypass`, in
`C:\wt\rvcrash`. Result: **91 passed, 1 skipped, 2 failed** of 94.
`check_00_kilnfw_target_build.ps1`: **PASS** (re-run individually to confirm).
All three non-passes are fresh-worktree provisioning, **none traceable to
aa2c484d**, and each is explained rather than waved past:

- `compile_esp_backends.ps1` **FAIL** — "Missing
  `firmware/KilnFW/build/compile_commands.json`". Root cause: nothing in the
  check suite produces that file. `check_00_kilnfw_target_build.ps1` builds in
  its own persistent worktree `C:\wt\checkbuild` (`:119`) and publishes only
  `KilnCtrl.elf`/`KilnCtrl.bin` into `firmware/KilnFW/build/` — I confirmed that
  directory in the worktree contains exactly those two files and no
  `compile_commands.json`. Confirmed a provisioning artifact by running the same
  check in the provisioned main tree: **PASS**.
- `check_duplicate_symbols.ps1` **SKIP** — same root cause ("`build/` exists but
  none of this project's own component object directories were found"), because
  the published `build/` holds only the two artifacts. Per CLAUDE.md a SKIP is
  not a pass; the prerequisite is genuinely absent in a fresh worktree and
  present in a provisioned one.
- `check_mykicad_golden_suite_runs.ps1` **FAIL** — `tools/mykicadMcp/.venv` is
  not provisioned by `git submodule update`; the check says so itself.

**2. KilnFW host tests at clean HEAD** (`C:\wt\rvcrash`): **46/46 built and
passed**, `crash_report` **85/85**.

**3. `flash_worker_lint.py` at clean HEAD: clean** (213 driver files scanned, 30
write-allowlisted, 7 cfg_fs-allowlisted).

**4. Parent-source test (INFO 3):** `aa2c484d^`'s `crash_report.c`/`.h` copied
over aa2c484d's in `C:\wt\rvneg`, new test kept, full build into a fresh
`ht_parent` directory: **46/46 passed, crash_report 85/85**. The new test does
not fail at the parent. Sources restored by hand (`git show HEAD:... > file`
redirect — no `git checkout`/`restore`/`stash`), `git diff` empty.

**5. My own negative test (poison → fresh full rebuild → hand restore → fresh
full rebuild):** in `C:\wt\rvneg`, a single line was inserted by hand into
`crash_report_acknowledge()` immediately before `esp_err_t submit_err = ESP_OK;`:

```c
crash_report_record_t negtest_rec; (void)load(&negtest_rec); /* NEGTEST */
```

This is exactly the regression shape the INFO test claims to catch — a
caller-side `load()` before dispatch, which the dispatch-count assertion alone
cannot see. All 46 executables rebuilt from scratch into a **fresh** `ht_neg`
directory (never measured from a prebuilt binary). Result: **`crash_report`
84/85, one FAIL**, at

```
test_crash_report.c:630: exactly one hal_kv read total -- the job's own load(),
and nothing read by the caller before dispatch
```

and **nothing else in the entire suite failed** (45 other executables green).
The assertion is precise and load-bearing.

Restored **by hand** (targeted Python string removal of that one inserted line —
no `git checkout --`, no `git restore`, no `git stash`). `grep -rn NEGTEST
firmware/`: no residue. `git diff`: **0 lines**. Then a forced full rebuild into
a second **fresh** directory `ht_clean`: **46/46 built and passed,
crash_report 85/85**.

## Main-tree WIP

Nothing in this review was measured from the main dirty tree except the single
`compile_esp_backends.ps1` confirmation run in step 1, which passed. No failure
anywhere in this review traced to another session's uncommitted work, and no file
named as concurrently-edited (`safety_ceiling_sync.c`, `safety_cfg_http.c`,
`kiln_cfg_store.c`, `elf_archive.py`, the heat_enable/profile_executor files) was
read for a verdict or modified.

## Recommendation

Accept `aa2c484d` — all claimed findings are genuinely closed and the
safety-critical direction (fail closed) holds on every new path. Open one
follow-up for **NEW MEDIUM 1**: `show_page()`'s `refresh_cb(NULL)` should not
drag `dashboard_get_status()` onto every page switch, and its justifying comment
should be corrected either way, since as written it states something the code
does not do.
