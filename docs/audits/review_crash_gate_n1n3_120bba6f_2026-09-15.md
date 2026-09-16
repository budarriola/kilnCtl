# Adversarial review of 120bba6f (N1-N3 fixes from the f07ad24d crash-gate follow-up review)

Date: 2026-09-15. Reviewed at `120bba6f`, in a clean throwaway worktree of `origin/main`.
Predecessor review: `docs/audits/review_crash_gate_followups_f07ad24d_2026-09-15.md`.

Owner constraint this was judged against (not a cleaner design): the crash-report manual
relay gate is option B -- an unacknowledged crash report blocks manual relay-ON, danger
mode bypasses it, `recovery_mode` and `estop_verified` are not gated.

## Verdicts

| Finding | Verdict |
|---|---|
| N1 -- success log lost the pre-reset count | **CLOSED** |
| N2 -- `(void)ok` discarded the result, two missing `ESP_LOGW` | **CLOSED** (one residual, LOW) |
| N3 -- duplicated snapshot-under-lock head | **CLOSED for the named pair** (partial in spirit, LOW) |

### N1 -- closed

`relay_cycles_reset_snapshot()` (`relay_cycles.c:750`) reads `s_rc.counts[relay]` into
`old_count` **before** zeroing it and **inside** the same `s_rc.lock` critical section, so
the logged value is genuinely the pre-reset count, not a post-reset re-read. It is logged
only on the success branch (`relay_cycles.c:788`), which is correct -- the failure branches
leave the count zeroed in RAM only and say so separately.

Caller audit: `relay_cycles_reset_finish()` gained a parameter and has exactly three call
sites, all in `relay_cycles.c` (818/883's inline path/883's dispatch path), all updated. The
function is `static`, so there are no callers in tests or tools -- confirmed by grep across
`firmware/`, `tools/` and `docs/`. Nothing anywhere parses the old `"count reset to 0"` log
string; the only other hits are an unrelated comment in `safety_cfg_store.c` and the review
doc quoting it.

### N2 -- closed, with one residual

The `(void)ok;` cast is gone and both `ESP_LOGW` calls are present
(`ui_page_diagnostics.c`, timeout branch and non-timeout-failure branch). The success label
is set only in the non-timeout branch. This matches `crash_ack_btn_clicked_cb()`'s precedent
line for line, which is what the finding asked for.

**Residual (LOW, inherited from the precedent):** on a non-timeout persist failure
(`ok == false`, `timed_out == false`) the label is still set to `"Reset"` -- textually
identical to the success outcome. Only the serial log distinguishes them. The finding's own
wording ("leaves the label reading 'Reset', i.e. indistinguishable from success") is
therefore only half addressed: the silence is fixed, the visual ambiguity is not. The
crash-report precedent has the same weakness (it restores `"Acknowledge"` on both), so this
is a deliberate consistency choice rather than an oversight, and a third distinct label
state would need an owner decision. Not blocking.

Side effect that is an improvement, and is not called out in the commit message: the
pre-call `lv_label_set_text(..., "Reset")` was deleted, so the button now reads `"Confirm?"`
for the duration of the blocking bounded wait instead of flipping to `"Reset"` before the
work is done. Checked that no armed path now leaves the label unset -- both branches write
it, and `refresh_cb()` only rewrites that label when the confirm deadline is non-zero, which
is cleared before the call, so a `"Busy"` result survives until the next arm tap as intended.

### N3 -- closed for the named pair, partial in spirit

`relay_cycles_reset_snapshot()` is genuinely used by **both** call sites. Neither retains a
private copy and neither has a bypass path: `relay_cycles_reset()` (818) and
`relay_cycles_reset_timeout()` (883) each call it exactly once, unconditionally, before any
branching on `uart_bridge_ext_is_on_flash_worker()`. The inline (already-on-worker) path in
`reset_timeout()` shares the same snapshot. There is no "shared helper one caller skips on
some path".

**But the finding's stated rationale is only partly delivered (LOW, pre-existing, not
introduced here).** The hazard N3 names is "any future field added to
`reset_persist_job_arg_t` must be copied into both, and the compiler will not say so". The
same verbatim snapshot head (three `memcpy`s, `rev + 1`, `dirty = false`) still exists in two
*other* functions in the same file -- `relay_cycles_restore_all()` (~943) and
`persist_snapshot_now()` (~1053). The count of sites a new field must be threaded through
went from 4 to 3, not to 1. Those two were outside the finding's wording and outside this
commit's claimed scope, so N3 is closed as written; the class is not retired.

## Independent verification of the poison test

Reproduced from scratch. Flipped `relay_cycles.c:803`'s `submit_err == ESP_ERR_TIMEOUT` to
`!=`, built into a fresh directory, and got:

```
  FAIL .../test_relay_cycles.c:369: the caller must be told this was specifically a
       timeout, not some other persist failure
170/171 checks passed        Built: 47/47 executables
```

**Exactly one check failed, at exactly the claimed line, and the failure is behavioural, not
a compile or API-existence error** -- all 47 executables still built and linked; the failure
is a runtime `TEST_CHECK` on `timed_out == true`. The claim in the commit message is accurate.

Restored **by hand** (exact-string replace, no `git checkout --`/`restore`/`stash`), confirmed
`git diff --exit-code` == 0 and md5-identical to `120bba6f`'s blob, then forced a **full
rebuild into a fresh directory**: 0 FAIL lines, `171/171`, `47/47`.

### Is the second test load-bearing?

**No -- it is a coverage test being described as a regression test.** The commit message
calls `test_reset_timeout_idle_worker_succeeds()` a pin "against N1-style drift". It caught
nothing in either mutation run:

- Mutation 1 (the `ESP_ERR_TIMEOUT` flip): 1 failure, busy test only. Idle test passed --
  admitted in the commit message.
- Mutation 2, mine (**bounded dispatch swapped for the unbounded sibling**, a realistic
  bounded/unbounded drift of exactly the pair this file is built around): 2 failures,
  `test_relay_cycles.c:369` and `:378`, **both in the busy test**. The idle test passed again.

Every assertion the idle test makes (`ok`, `!timed_out`, count zeroed, `dirty` cleared,
dispatch +1, persisted blob) is already made by the pre-existing
`test_reset_zeroes_count_and_persists()` against the unbounded entry point through the same
shared tail. The one behaviour unique to the bounded entry point -- which dispatch function it
calls -- is caught by the busy test, not the idle one. The idle test is worth keeping as
cheap coverage; the commit message overstates it.

Credit where due: the busy test's `s_stub_dispatch_count == dispatch_before` check looked
vacuous (the bounded stub returns `ESP_ERR_TIMEOUT` *before* incrementing, so it is partly a
property of the stub), but mutation 2 made it fail on a production-only change, so it is
discriminating rather than decorative.

## Numbers (per-test, not summary)

Clean full rebuild of `120bba6f` in a fresh output directory:

- **47/47 executables** built and passed; **0** `FAIL` lines across the whole run.
- **39 suites** reporting their own counts; aggregate **19752/19752 checks passed**.
- The relay-cycles suite specifically: **171/171**.
- Poisoned run for comparison: **170/171** in that suite, 47/47 executables still built.

Both of the commit message's reported numbers (`171/171`, `47/47`) reproduce exactly.

## New findings

**LOW-1 -- `relay_cycles_reset_snapshot()` has no bounds check.** It writes
`s_rc.counts[relay]` relying on a comment ("relay is already validated by both callers").
Both current callers do validate against `RELAY_CYCLES_COUNT` before calling, so there is no
live defect. The two functions it was extracted from each had that guard in the same
function as the write; the extraction moved the write away from its guard. A third caller
added later gets an out-of-bounds write with nothing to stop it.

**INFO-1 -- N2 sibling scan result.** Scanned both production files for the repo's standing
"discarded return plus a success log" class. One discarded return remains:
`ui_page_diagnostics.c:1119`, `(void)thermo_owner_command_read_all(...)` (returns
`esp_err_t`). It is **not** an instance of the class -- `reading_count` is initialised to 0
and `readings` is `memset`, so a failed read renders every channel as "no data" rather than
as stale or OK, and nothing logs success. The residual is only that a total owner/SPI failure
is indistinguishable from "no channels present" and produces no log line at all. Untouched by
this commit and outside its scope. `relay_cycles_get_type()` and `relay_cycles_budget()`, the
other two bare-statement calls in that file, return `void` -- not findings.

**INFO-2 -- scope honesty.** The commit does exactly what it says across exactly the three
files it claims, and its commit-message correction about `f07ad24d`'s "refresh_cb() runs
twice" item is accurate. It adds no closure record to
`review_crash_gate_followups_f07ad24d_2026-09-15.md`, so that document still reads as if N1-N3
are open.

**Survival check:** current `origin/main` (`ec682a7d`, "Restore 16 files wrongly reverted by
the previous commit") carries all three files **byte-identical** to `120bba6f`. The fixes were
not caught by that revert/restore churn.

## Methodology cautions (both are the repo's own poisoned-binary class)

1. My **first** poison run was invalid and would have produced a false verdict. Piping the
   build script into `Select-String ... | Select-Object -First 40` terminated the PowerShell
   pipeline early and killed the script at 983 of ~3100 lines -- it never reached
   `test_relay_cycles` at all, while still exiting non-zero and looking like a real failure.
   **Never filter a build/test script through an early-terminating pipeline element.** Tee to
   a file, then grep the file.
2. My mutation-2 restore-by-string corrupted two *unrelated* call sites: the replacement text
   `uart_bridge_ext_run_on_flash_worker(reset_persist_job, &ctx)` also matched the legitimate
   unbounded calls in `relay_cycles_reset()` and `relay_cycles_restore_all()`/
   `persist_snapshot_now()`. Caught by the md5-vs-`git show` check and a compile error
   (`'timeout_ms': undeclared identifier` at three lines), then repaired line-targeted by hand
   and re-verified to md5 identity before the final rebuild. A hand restore needs an identity
   check against the committed blob, not just a successful-looking edit.

## What I tried that did NOT find anything

- Looked for a call site that bypasses the shared N3 helper, including the inline
  already-on-worker path. None.
- Grepped `firmware/`, `tools/` and `docs/` for other callers of the changed
  `relay_cycles_reset_finish()` signature, in tests and tools included. It is `static`; there
  are none.
- Grepped for any consumer (script, doc, test, parser) of the old `"count reset to 0"` log
  string that the N1 wording change would break. Nothing consumes it.
- Checked whether `old_count` could be a post-reset read or read outside the lock. It is
  neither.
- Checked whether deleting the pre-call label write leaves any armed path without a label
  update, including the `refresh_cb()` deadline-expiry path that also writes that label. It
  does not.
- Scanned both production files for remaining unchecked-success/discarded-result siblings.
  Only INFO-1, which is not the class.
- Ran a second, independent mutation (bounded->unbounded dispatch) specifically to try to make
  the second new test fail on its own. Could not.
- Checked whether a later `origin/main` commit had reverted any of this. It had not.

## Verdict

**Safe to stop iterating on this area.** All three findings are closed against their own
wording, the reported evidence reproduces exactly, and the one claim that does not hold up
(the second test being a regression pin) is a commit-message overstatement, not a code defect.
The residuals -- N2's label ambiguity, N3's two remaining snapshot heads, LOW-1's missing
bounds check -- are all LOW and none of them can produce a wrong safety outcome: every failure
path still zeroes only RAM, re-arms `dirty`, and logs.
