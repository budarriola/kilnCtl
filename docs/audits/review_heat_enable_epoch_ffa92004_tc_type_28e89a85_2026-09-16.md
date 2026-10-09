# Adversarial review: ffa92004 (heat-enable executor follow-ups) and 28e89a85 (TC type F2)

Date: 2026-09-16
Commits reviewed: `ffa92004`, `28e89a85`
Reviewer worktrees: `C:\wt\herev_k7n3qp` (28e89a85), `_p1` (ffa92004), `_p0` (52f046ff),
`_pA`/`_pD` (throwaway mutation trees, all removed on completion).

Everything below was measured, not read. Per-test counts were taken against both the
commit and its true parent; three mutations were injected and hand-restored (no
`git checkout --`/`git restore`/`git stash`), each verified by an empty `git status`
and a line-ending-normalised md5 against the committed blob, and each measurement was
taken from a forced full rebuild into a fresh `-OutDir`.

---

## Headline: BLOCKING defect in ffa92004 -- the MEDIUM-5 "fix" is inert in the scenario it exists to close

`heat_enable_release()` bumps the per-claimant release epoch only when the claim was
actually held:

```c
/* heat_enable.c:448-457 */
bool was_held = (s_he.held_mask & bit) != 0u;
s_he.held_mask &= ~bit;
if (was_held) {
    ...
    s_he.release_epoch[who]++;
}
```

But at **every one of the four acquire call sites the claimant's bit is not yet set
when the epoch is sampled**:

| Call site | epoch sampled | spent | claim bit at sample time |
|---|---|---|---|
| `profile_executor_run.c` | 952 | 954 | clear -- fresh firing start |
| `profile_executor_status.c` | 239 | 241 | clear -- the pause at `:153` already released it |
| `autotune_engine.c` | 1411 | 1419 | clear -- autotune entry |
| `autotune_engine.c` | 1523 | 1530 | clear -- autotune entry |

So the interleaving the fix was written against still resurrects the claim:

1. Task A samples `he_epoch = heat_enable_claim_epoch(PROFILE)` under `s_exec.lock`
   (`profile_executor_run.c:952`). The PROFILE bit is **clear**. Epoch reads `N`.
2. `s_exec.lock` is dropped (`:953`).
3. The operator stop lands here. It calls `heat_enable_release(PROFILE)`. Because the
   bit is clear, `was_held == false`, so `release_epoch[PROFILE]` **stays at `N`**.
   The run is gone.
4. Task A spends the sample at `:954`. `now_epoch == N == epoch`, so `stale == false`,
   `held_mask |= PROFILE_BIT`, and `send_enable("firing")` puts `REQUEST_ENABLE(true)`
   on the wire for a run that no longer exists -- and
   `heat_owner_active_decide()` now reports a heat owner to the Pico off that same mask.

This is the pre-fix behaviour, unchanged. The window is not narrow in practice:
`executor_task_entry`'s per-tick backstop `heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE)`
(`profile_executor.c:353`) runs inside exactly this window on every tick where the state
is not `PROFILE_EXEC_RUNNING`, and is likewise a no-op bump-wise.

### Demonstration (my test, run against the committed code at `ffa92004`)

Added to `test_heat_enable.c` and registered in `run_test_heat_enable()`:

```c
reset_all(true);
uint32_t e = heat_enable_claim_epoch(HEAT_ENABLE_CLAIMANT_PROFILE);
heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);   /* the stop, mid-window */
bool got = heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_PROFILE, e);
TEST_CHECK(got == false, "DEMO: the stale claim is refused");
TEST_CHECK(enable_sends() == 0, "DEMO: no REQUEST_ENABLE(true) for a run that no longer exists");
```

Result on unmodified `ffa92004` (`main` executable, 7924/7926):

```
FAIL test_heat_enable.c: DEMO: the stale claim is refused
FAIL test_heat_enable.c: DEMO: no REQUEST_ENABLE(true) for a run that no longer exists
```

(Citation correction, 2026-09-16: this transcript's original `:803`/`:804` line
numbers named lines in the reviewer's own local, uncommitted addition to
`test_heat_enable.c` -- the two `TEST_CHECK`s shown just above this block, added
only to demonstrate the defect and never landed on `main`. `git log -S` against
the full history finds no commit ever containing this text, so no `path:line`
citation for it is possible; the test names above are the only stable
reference. The failure counts and messages themselves are an accurate
transcript of a real run against unmodified `ffa92004`.)

### Why the committed test does not catch it

`test_stale_claim_is_not_resurrected()`'s third case asserts the hole as intended
behaviour:

```c
TEST_CHECK(heat_enable_acquire_since(HEAT_ENABLE_CLAIMANT_PROFILE, e2) == true,
           "a no-op release does not invalidate a legitimate acquire");
```

Its first case (acquire, release **while held**, stale acquire refused) exercises a
shape that does not occur at any call site, because no call site samples the epoch
while already holding the claim.

### The design tension, measured

Removing the `if (was_held)` guard so the epoch always bumps produces **exactly one
failure in the whole suite** -- that third case:

```
FAIL test_heat_enable.c:669: a no-op release does not invalidate a legitimate acquire
main: 7923/7924
```

(Citation correction, 2026-09-16: `:669` is where this assertion lived in the
`ffa92004` tree this section measures against. `9b9ef3ef` re-pointed the same
property at `heat_enable_release_backstop()` and reworded the message to "an
idle-backstop tick with nothing held does not invalidate a legitimate
acquire" -- same requirement, not deleted or weakened -- now at
`test_heat_enable.c:686`.)

That is the honest statement of the problem: the unconditional bump closes the
resurrection hole, and the only thing standing against it is the per-tick backstop at
`profile_executor.c:353`, which would then invalidate every in-flight acquire. The
epoch cannot be both "advanced by any stop" and "not advanced by the idle backstop"
while the backstop is an unconditional release. A correct fix has to distinguish an
operator stop from the idle backstop -- e.g. a separate stop-generation counter bumped
only on a real stop transition, or hoisting the backstop so it cannot fire during a
start. Adding `was_held` to the bump condition is what makes the epoch inert.

**Rank: BLOCKING.** MEDIUM-5 is reported FIXED and is not fixed. The claimed fix
adds an API surface (`heat_enable_claim_epoch`/`heat_enable_acquire_since`), a
tripwire widening and a test that pins the hole open, with no behavioural change in
the motivating scenario.

---

## Per-finding verdicts, ffa92004

### Guard 9 latency (reconcile moved last) -- FIXED, test partly weak

Both implementer claims hold. `heat_enable_reconcile()` is the last statement of
`watchdog_task_entry`'s loop body (`profile_executor.c:1836`); nothing else in that
body touches heat-enable state, and the two `heat_enable_release()` calls at
`profile_executor.c:349/353` are in `executor_task_entry` (a different function,
beginning at 326). The retry is level-triggered: `heat_enable_reconcile()` re-derives
from `held_mask`/`granted` each tick rather than consuming an edge.

The ordering test is **partly vacuous but not wholly so**. `last_occurrence(text,
"WATCHDOG_TICK_DEAD_MS")` resolves to line 1818 -- inside the explanatory comment the
commit itself added above the call -- not to the real in-loop check at 1675. I mutated
the production file (moved the call back to just after the watchdog `vTaskDelay()` at
1813, leaving the comment where it was) and the anchor still fired:

```
FAIL test_heat_enable.c:720: the stale-tick (guard 9) check runs BEFORE heat_enable_reconcile()
main: 7923/7924
```

(Citation correction, 2026-09-16: `:720` is where this check lived in the
`ffa92004` tree this section measures against; unchanged wording, now at
`test_heat_enable.c:844` after `9b9ef3ef`'s unrelated insertions earlier in the
file shifted it down.)

So it catches today's regression, but only because the comment stayed behind. A future
move that carries the comment with the call moves the anchor too and the check passes
silently. The second anchor (`run_state_note(RUN_STATE_PHASE_FAULTED`, last at 1799)
did **not** fire on that mutation. **Rank: LOW.** Anchor on the real check, not on a
token that also appears in the commit's own prose.

### MEDIUM-5 -- NOT FIXED. See the headline above. BLOCKING.

Per-claimant scoping, the other half of the brief, **is** correct in both directions.
A second demo test (4 checks) passed unmodified on `ffa92004`: an autotune release does
not move the profile epoch and cannot refuse a firing start, and a profile release does
not move the autotune epoch and cannot refuse an autotune start. `release_epoch[]` is
indexed by `who` throughout and `acquire_since()` compares only `release_epoch[who]`.

### Stack numbers (4096 declared / 2496 budgeted / 1696 INDETERMINATE) -- ACCEPT, honestly

Documented at three sites as "UNMEASURED AND UNBOUNDED BY ANALYSIS". The residual is
INDETERMINATE because of unresolved indirect calls, so no static number would close it;
labelling an unmeasured quantity as unmeasured is the correct outcome, and this commit
did not grow the stack. Closing the gap needs a runtime high-water measurement, which
is separate work.

### Timing comment (~5.2 s, all `SAFETY_XACT_LOCK_TIMEOUT_MS`) -- CORRECT, and the dependency is real

Verified independently: **zero** production `esp_task_wdt_add()` call sites in the tree,
and `REQUEST_ENABLE` passes `expect_status=false`, so the ~345 ms reply term is
genuinely unreachable. The comment is accurate.

But a comment is thin protection: the first task that ever subscribes to the Task WDT
turns this 5.2 s block into a panic reset (`CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`,
`CONFIG_ESP_TASK_WDT_PANIC=y`), and it learns that from the panic, not from the comment.
`test_flush_bound_comment_is_correctly_derived()` scans the comment text; nothing scans
the tree for `esp_task_wdt_add()`. **Rank: MEDIUM (defence-in-depth).** A four-line
`check_*.ps1` asserting zero `esp_task_wdt_add()` call sites would make the dependency
mechanical instead of advisory.

### The widened tripwire regex -- has NOT gone blind

`check_heat_enable_wiring.ps1`'s `'heat_enable_acquire(_since)?\s*\('` still fires.
Negative test: renaming only the two real `heat_enable_acquire_since(` calls in
`profile_executor_run.c` and `profile_executor_status.c` produced
"1 heat-enable wiring violation(s)" and a throw. The only remaining textual matches
were the comment lines at `profile_executor_status.c:149` and `:193`, correctly excluded
by the `^\s*(\*|//|/\*)` filter. The unmodified tree reports "1 enable / 2 release wire
call(s)".

---

## Per-test counts, ffa92004

Host tests, MSVC harness, fresh `-OutDir` per measurement, 47/47 executables every run.
`firing_score_from_capture` and `sim_credibility_gate` SKIP on gitignored
`logs/coupling/*.jsonl` captures in every run, including the baselines.

| Tree | `main` executable | Note |
|---|---|---|
| `ffa92004` (commit) | **7924/7924** | matches the claim |
| `52f046ff` (**true** parent) | **7902/7902** | +22 checks in the commit |
| `52f046ff` + the commit's `test_heat_enable.c`, minus `test_stale_claim_is_not_resurrected` | **7908/7914, 6 FAILs** | matches the claimed parent figure exactly |

The commit message cites parent `120bba6f`; `git log --format='%H %P'` gives the actual
parent as `52f046ff`. `120bba6f` is an unrelated commit. The claimed "7908/7914 with
6 FAILs" is the **backport** measurement, which I reproduced exactly -- the citation is
wrong, the number is right.

The 6 backported FAILs, all in `test_heat_enable.c`, run by `run_test_heat_enable()`:

```
:657  the stale-tick (guard 9) check runs BEFORE heat_enable_reconcile()
:670  heat_enable_reconcile() comes after the loop body's final breadcrumb write
:701  the bound must not be derived from SAFETY_LINK_REPLY_TIMEOUT_MS
:703  the discredited ~5.5 s figure is gone
:707  and quotes the corrected ~5.2 s worst case
:715  the blocking site names its dependency on ZERO esp_task_wdt_add() call sites
```

2 from `test_reconcile_runs_last_in_the_watchdog_loop`, 4 from
`test_flush_bound_comment_is_correctly_derived`, as claimed. Both are behavioural
(source-scan) failures, not compile errors.

`test_stale_claim_is_not_resurrected` has no parent evidence, as the implementer
disclosed -- its API does not exist at the parent. Mutation supplies it instead:
disabling the staleness refusal (`bool stale = false;`) fails 4 of its checks:

```
FAIL test_heat_enable.c:640: the stale-epoch acquire is REFUSED
FAIL test_heat_enable.c:643: and it sent no REQUEST_ENABLE(true) -- K4 was not asked for on behalf of a run that had already stopped
FAIL test_heat_enable.c:646: and the claim was NOT resurrected -- held_mask stays clear
FAIL test_heat_enable.c:647: and nothing reads as granted
main: 7920/7924
```

(Citation correction, 2026-09-16: these four line numbers are where the
respective `TEST_CHECK` calls lived in the `ffa92004` tree this section
measures against. `9b9ef3ef` inserted new cases and comments earlier in
`test_stale_claim_is_not_resurrected()`, shifting all four down by 7 with no
other change to this quartet's own code or messages; they are now at
`test_heat_enable.c:647`, `:649`, `:652` and `:654` respectively.)

```
```

Load-bearing -- for the shape it tests. That shape is not the shape that occurs.

---

## Commit 28e89a85 (TC type F2): verdicts

### F2 cached-vs-persisted -- FIXED, and the evidence is real

`config_store_write_ex()` compares `s_persisted_record.tc_type != rec->tc_type`
(`config_store_flash.c:1092-1096`), and the new `config_store_get_persisted_tc_type()`
returns that same `s_persisted_record.tc_type`, so the wire reason is now built from
the exact byte the refusal decision was made on.

### The anti-vacuity assertion works

Poisoned `link_task_commit_reject.c:25` (`!=` to `==`), full rebuild:

```
FAIL test_link_task_commit_reject.c:38: ARMED refusal whose candidate ALSO changes tc_type is MIXED
FAIL test_link_task_commit_reject.c:43: ARMED refusal that does NOT touch tc_type stays the plain ARMED reason
  main:              2566/2568
  hal_spi_pico:      56/56
FAIL test_config_store_flash.c:428: the wire reason built from the PERSISTED tc_type is ARMED_MIXED
FAIL test_config_store_flash.c:438: the wire reason built from the CACHED tc_type is the plain ARMED reason
  config_store_flash: 268/270
```

Exactly the 4 failures at exactly the 4 file:line positions the implementer claimed.
Restored by hand, `git status` clean, md5 match, fresh rebuild back to 2568/2568.

### F3 declared untestable -- the corrected reason holds

`s_call_in_progress` is function-local and everything between its set and clear is
same-translation-unit or libc; there is no seam to re-enter through without adding
one. Genuinely untestable as written.

### Per-test counts, 28e89a85

| Executable | parent `ffa92004` | commit `28e89a85` |
|---|---|---|
| `main` | 2560/2560 | **2568/2568** (+8) |
| `config_store_flash` | 259/259 | **270/270** (+11) |
| `hal_spi_pico` | 56/56 | 56/56 |

All three match the claims exactly.

---

## The two Pico invariants

**1. Nothing may make it possible for the Pico to end up not armed.** Holds for both
commits. `28e89a85` changes only which copy of `tc_type` a *refusal reason string* is
built from -- a diagnostic, downstream of a decision already taken -- and touches no
arming path. `ffa92004` touches the K4 heat-enable interlock, whose failure direction
is heat being *requested* when it should not be, never the Pico disarming; the headline
defect resurrects a heat request, it does not drop arming. The Pico stays armed
throughout in both.

**2. The Pico's `abs_max_temp_c` must always equal the ESP's.** Holds, structurally and
untouched. `abs_max_temp_c` sits at offset 17 of the v2 packed record;
`config_store_only_tc_type_differs()` neutralises only format_version, seq, tc_type and
the SET_TC_TYPE bit, then does `memcmp(a, b, REC_OFF_CRC)` (`config_store.c:1238`),
whose window starts at 0 and so covers offset 17. A candidate that changes
`abs_max_temp_c` while ARMED therefore still fails the equality and is still refused as
plain ARMED. `28e89a85` did not touch that function, that window, or the layout.

Neither invariant is weakened by either commit.

---

## Suite state

`tools/run_all_checks.ps1 -ExecutionPolicy Bypass`, foreground, clean worktree at
`28e89a85`: **87 passed, 1 skipped, 6 failed.** Four failures are the documented
clean-worktree set (`check_flash_worker_lint`, `check_mcp_facade_coverage`,
`check_mykicad_golden_suite_runs`, `check_doc_hash_citations` -- the last on
`docs/audits/esp_bring_up_to_head_2026-09-10.md:13`, `sub:lvgl`). The other two,
`check_main_task_stack_budget` and `check_all_task_stack_budgets`, fail on
"could not read CONFIG_ESP_MAIN_TASK_STACK_SIZE from sdkconfig" -- a clean worktree has
no `sdkconfig`; neither is attributable to either commit. Both `check_00_*` target
builds passed; no claim here rests on them.

---

## Ranked new defects

1. **BLOCKING** -- `heat_enable.c:450`'s `if (was_held)` makes the release epoch inert
   at all four call sites, so the stale-claim resurrection MEDIUM-5 describes is still
   live. Demonstrated by a failing test on unmodified `ffa92004`.
2. **MEDIUM** -- the 5.2 s blocking site's safety depends on there being zero
   `esp_task_wdt_add()` call sites, enforced only by a comment. Make it a check.
3. **LOW** -- `test_reconcile_runs_last_in_the_watchdog_loop`'s `WATCHDOG_TICK_DEAD_MS`
   anchor resolves into the commit's own comment at 1818, not the real check at 1675.
   It catches today's regression by accident of comment placement.
4. **LOW** -- `ffa92004`'s commit message cites the wrong parent (`120bba6f`; actual
   `52f046ff`). The figures are right, the provenance line is not.

## Verdicts

- **`ffa92004`: NOT safe to stop iterating.** One of its four claimed closures is not
  closed, and the test suite pins the hole open as intended behaviour. The other three
  findings are genuinely addressed.
- **`28e89a85`: safe to stop iterating.** Every claim reproduced exactly, the
  anti-vacuity assertion is load-bearing, and both Pico invariants are intact.

## Follow-up, 2026-09-20

The BLOCKING finding above is now fixed, by `9b9ef3ef` ("Close the heat-enable
stale-claim window the release epoch missed"), the same day this review landed.
It does exactly what the "design tension" section above says a correct fix
needs: it distinguishes an operator stop from the idle backstop rather than
bumping `release_epoch[]` unconditionally or only on `was_held`. `heat_enable.c`
now threads a `stop_transition` flag through the shared release body so a real
stop/pause/abort transition advances the epoch (`stop_transition || was_held`,
`heat_enable.c:469`) even when the claimant's bit was already clear at that
call site -- closing exactly the window this review demonstrated -- while
`heat_enable_release_backstop()`'s unconditional per-tick calls do not, so a
legitimate in-flight acquire is not spuriously invalidated by an idle tick.
`test_stale_claim_is_not_resurrected()` (`test_heat_enable.c:625`) still carries
the anti-vacuity case this review flagged, reworded ("an idle-backstop tick
with nothing held does not invalidate a legitimate acquire",
`test_heat_enable.c:686`) rather than deleted or weakened. Verified on this
pass: a full host-test rebuild (`build_host_tests.ps1`) reports 56/56
executables built and passed at `HEAD` (`0b4524e2`), which includes
`test_heat_enable`. No code change was needed this pass; this note closes the
finding.
