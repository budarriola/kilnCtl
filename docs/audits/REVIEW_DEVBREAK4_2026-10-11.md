# Review: devbreak4 fixes for REVIEW_DEVBREAK3 (2026-10-11)

Range reviewed on origin/dev (two commits):

| SHA | Subject |
|---|---|
| f4e5b4b8a | REVIEW_DEVBREAK3 fixes: stale doc citations, build-gate exclusion by relative path, reset-fence check hardened, 512 B bridge headroom floor |
| 84130e19f | bridge stack checker: enforce 512 B honest-headroom floor (DEVBREAK3 LOW-4) |

Method: fresh worktree `C:\wt\rvdb4_41ecz0` at dev tip 2d191c82f (minted with
submodules; both commits are ancestors). Every touched check was run there.
Suspected gaps were negative-tested with `tools\negtest.ps1` (every baseline
PASS, every copy removed, real tree unchanged). The bridge stack checker was
run against three real ELFs. No code was changed. The bench board was not
touched.

Summary: no HIGH. One MED (the host test the reset-fence check relies on for
semantics never covers the real reset window). Four LOW. Several INFO.

## Tests run

| Test | Result |
|---|---|
| check_reset_fence_hooks.ps1 | PASS |
| check_build_gate_usage.ps1 | PASS (21 callers) |
| check_doc_hash_citations.ps1 | PASS (5606 citations, 2291 unique hashes, 7 blob citations) |
| check_gate_negative_test_table.ps1 | PASS (198 checks, 199 rows) |
| check_system_uart_bridge_stack_budget.ps1 (worktree, no build) | SKIP exit 3, no ELF (expected) |
| check_system_uart_bridge_stack_budget.py --elf, ELFs from `C:\wt\checkbuild_159bd283b2` and `C:\wt\checkbuild_1380f4af5e` (both linked 2026-10-11 22:55-22:56, after these commits) and `elf_archive\KilnCtrl-latest.elf` (2026-10-10) | all OK: 3216 B deepest, ceiling 3216 B, honest headroom 580 B, above the 512 B floor |
| check_build_gate_usage.ps1 -Root (shared main tree) | RED, but only on untracked archive copies under `logs/wt_archive_2026-10-0{9,10}/` (see INFO-1) |

No new KilnFW target build was run. The two checkbuild ELFs were built by other
sessions from nearby dev commits plus their own WIP. One of them was deleted by
its owner mid-review, so the negative test used a scratchpad copy of
`KilnCtrl-latest.elf`.

## Negative tests (all `tools\negtest.ps1`, baseline PASS, copies removed, real tree unchanged)

check_reset_fence_hooks.ps1 (`-Preset check`):

| Mutation | Verdict |
|---|---|
| R0 control: relay_authority.c `refuse = false;` before `return refuse;` | CAUGHT |
| R1 main.c: `#if 0` / `#endif` around the `hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write);` install | MISSED |
| R2 main.c: `hal_kv_set_write_refuse_hook(NULL);` added after the install | MISSED |
| R3 main.c: `if (0) hal_kv_set_write_refuse_hook(...)` | MISSED |
| R4 relay_authority.c kiln_nvs predicate: `void *me = s_reset_job_task;` | MISSED |
| R5 relay_authority.c: locals `bool s_reset_erases_kiln_nvs = true; uint8_t s_reset_in_flight_depth = 0;` shadow the statics before the assignment | MISSED |
| R6 main.c: `#define \` + newline + `relay_authority_reset_refuses_kiln_nvs_writer() false` (macro name on a continuation line) | MISSED |
| R7 relay_authority.c: faithful copy of the predicate inside `#if 0` placed before the real one, real one starts with `return false;` | MISSED |
| R8 main.c: same `#if 0` decoy for `kiln_nvs_reset_refuses_write`, real one returns false | MISSED |
| R9 relay_authority.c: generic `relay_authority_reset_refuses_writer()` body `bool refuse = 0 && ...` | MISSED (not pinned by this check at all; see INFO-3) |

Same semantic mutations against the host test (`build_host_tests.ps1 -Only test_link_watchdog`, `-ExpectPattern` per the rules file):

| Mutation | Verdict |
|---|---|
| R4 kiln_nvs predicate `me = s_reset_job_task` | MISSED |
| R5 local shadows | CAUGHT (test_link_watchdog.c:243, 259, 261) |
| R9 generic writer `0 &&` | CAUGHT (test_link_watchdog.c:225, 229, 238) |
| R10 generic writer `void *me = s_reset_job_task;` | MISSED |

check_build_gate_usage.ps1 (`-Preset check`, each a new file holding `Enter-KilnBuildGate`; `Start-Sleep`; `Exit-KilnBuildGate`):

| New file | gitignored? | Verdict |
|---|---|---|
| tools/build/gate_misuse.ps1 | no | MISSED |
| tools/buildx/gate_misuse.ps1 (control) | no | CAUGHT |
| docs/build/gate_misuse.ps1 | no | MISSED |
| tools/checkbuild_abc123/gate_misuse.ps1 | no | MISSED |

check_system_uart_bridge_stack_budget.py (`-Command python ... --elf <copy of KilnCtrl-latest.elf>`, expect `below the 512 B floor`):

| Mutation | Verdict |
|---|---|
| B1 `CONFIGURED_STACK_BYTES = 3700` (honest 184 B, deepest still under the ceiling, so only the floor can fire) | CAUGHT |
| B2 `CONFIGURED_STACK_BYTES = 4027` (honest 511 B) | CAUGHT |
| B3 `CONFIGURED_STACK_BYTES = 4028` (honest 512 B, boundary, expected to pass) | MISSED, as intended (`<` not `<=`) |
| B4 floor set to 0, ceiling 3700, stack 3700 together | MISSED, as expected: editing the check itself is out of scope |

## Findings

### MED-1: the reset-fence host test never checks the real reset window, so the check's "semantics: test_link_watchdog.c" leaves a gap

check_reset_fence_hooks.ps1 says the semantics of the relay_authority
predicates are pinned by test_link_watchdog.c, so the regex only keeps the
terms. The host test cannot tell "the reset job's own task" from "another task".
Every host stub of `xTaskGetCurrentTaskHandle()` returns the constant
`(TaskHandle_t)1` (`test/stubs/freertos/task.h:52`). Between
`relay_authority_reset_job_enter()` and `_exit()` the test therefore only asserts
that the caller is exempt. It never asserts that a different task is refused.

On hardware the job registers itself as soon as the reset job task starts. So
the "job registered" window is nearly the whole erase, and it is the window the
fence exists for. A mutation that exempts every task while a job is registered
passes both guards:

- R4: `void *me = s_reset_job_task;` in
  `relay_authority_reset_refuses_kiln_nvs_writer()`. This leaves
  `refuse = depth && erases && job == NULL`, so no kiln_nvs writer is fenced
  while the job runs. The check passes (the body regex never pins `me`) and the
  host test passes.
- R10: the same edit in `relay_authority_reset_refuses_writer()` turns off the
  cfg/pref/cfg_fs fence for the job's duration. Neither guard catches it. The
  check does not look at this function (INFO-3).

This is a gap in the guards, not a live bug: the shipped code is correct.

Fix: give the host stubs a settable current-task handle (a `stub_set_current_task()`
in the test's task.h). Then in `test_reset_refuses_writer_exempts_reset_job_task`
and `test_kiln_nvs_fence_only_when_scope_erases_it`, assert after
`_job_enter()` that switching to task 2 is refused and switching back to task 1
is exempt. Optionally pin `void *me = (void *)xTaskGetCurrentTaskHandle();` in
the check's `pre` group.

### LOW-1: check_build_gate_usage.ps1 skips any directory named `build` (and the other names) even where it is not gitignored

The new exclusion matches path segments, not ignore rules. `build/` is
gitignored only under `firmware/KilnFW/` and `tools/PcTools/`. So under App/
(the case asked about) a nested `build/` really is ignored output, and a
script there can only be committed with `git add -f`. Elsewhere it is not
ignored: `tools/build/`, `docs/build/` and `tools/checkbuild_<hex>/` are all
ordinary tracked locations. A gate-misusing script committed there is never
linted (negtests above: MISSED, MISSED, MISSED; the `tools/buildx/` control is
CAUGHT). `checkbuild_<hex>` dirs live under `C:\wt\`, never inside a tree, so
that entry only adds a blind spot.

False RED: none in a worktree. No tracked `.ps1` sits under any excluded
segment today (`git ls-files` finds none), so the narrowing changes no current
verdict. A repo cloned under a `build*` parent no longer false-REDs, as
intended.

Fix: enumerate candidates with `git -C $Root ls-files -co --exclude-standard -- '*.ps1'`
instead of `Get-ChildItem` plus a name list. Ignored output then drops out by
definition, every tracked or untracked-but-not-ignored script is linted, and
the `logs/` false RED (INFO-1) goes away too, if `logs/` copies are ignored or
the archive dirs are added to `.gitignore`.

### LOW-2: check_reset_fence_hooks.ps1 main.c guards are textual and the preprocessor defeats them

main.c is not compiled into any host test, so this check is its only guard (the
check says so). All of these pass:

- `#if 0` around an install line (R1)
- a later `hal_kv_set_write_refuse_hook(NULL);` (R2)
- `if (0)` in front of the install (R3)
- a `#define` whose macro name is on a backslash-continuation line (R6; the
  macro regex needs the name on the `#define` line)
- an `#if 0` decoy copy of the predicate ahead of a gutted real one (R8; the
  first regex match wins)

R1-R3 are pre-existing (the install-line part did not change here). R6 and R8
get past the hardening this commit added.

Fix:
- join `\`-newline continuations before the macro scan;
- delete `#if 0 ... #endif` regions, or refuse any `#if`/`#ifdef` around the
  install block and the predicate;
- require each hook setter to appear exactly once in main.c, at statement start
  (`^\s*name\s*\(`), with exactly the expected argument and no other call to
  that setter;
- require exactly one definition match of `kiln_nvs_reset_refuses_write`.

### LOW-3: the same textual gaps in the relay_authority.c body pin

R5 (local shadows declared in `pre`) and R7 (`#if 0` decoy, first match wins)
pass the check. R5 is caught by the host test, R7 is not. The `pre` group
forbids only `return`/`goto`. The same fixes apply: one definition match, no
`#if 0` region, and `pre` limited to exactly
`void *me = (void *)xTaskGetCurrentTaskHandle(); portENTER_CRITICAL(&s_heat_claim_mux);`
(which also closes R4 at the check level).

### LOW-4: the 512 B floor trusts a hand-mirrored stack size, and the live registration for the same task has already drifted

The floor logic is right. `honest = CONFIGURED_STACK_BYTES - total - 300`, and
it fails at `< 512`, so 512 B passes. It runs before the ceiling test, which is
correct because it must fire even when the ceiling is raised. B1/B2 CAUGHT,
B3 passes as intended. Today the floor can only fire if CEILING_BYTES goes above
3284 B, so it is a backstop exactly as intended. The CRLF file is consistent:
153 CRLF, 153 LF, no bare LF in the new lines.

`CONFIGURED_STACK_BYTES = 4096` is a literal ("must match
uart_bridge_start_system_task()'s xTaskCreatePinnedToCore() literal"). No check
compares it with `uart_bridge_system.c:211`. If the C stack is ever reduced, the
floor keeps computing against 4096 and goes vacuous.

The drift risk is real, because it has already happened next door:
`uart_bridge_system.c:220` still registers the task with
`stack_margin_register("system_uart_bridge", ..., 3072)` while line 211 creates
it with 4096. That has been so since the 3072 -> 4096 raise in 0a1cdc7dc.
`get_stack_margin` therefore grades this task against a 3072 B denominator. This
is pre-existing and outside the reviewed range, but it is the same mirror.

Fix: have the .py (or check_stack_task_table_consistency.ps1) read the
`xTaskCreatePinnedToCore(system_bridge_task, "system_uart_bridge", <N>` literal
and fail on a mismatch. Fix the 3072 registration to 4096 (or use a shared
`#define`).

## INFO

- INFO-1: check_build_gate_usage.ps1 run with `-Root` on the shared main tree
  goes RED on untracked copies of `tools/check_build_gate_reentrant.ps1` under
  `logs/wt_archive_2026-10-09/` and `logs/wt_archive_2026-10-10/`, because the
  deliberate-misuse allowlist is keyed by repo-relative path. This is
  pre-existing and only matters for a run_all_checks in the main tree. The
  LOW-1 fix (git ls-files) plus ignoring `logs/wt_archive_*/` would end it.
- INFO-2: doc citations (question 1).
  - `0a08000511` and `0a08000011` (REVIEW_TRIPWP1 :122, :134) are kilnlink frame
    bytes, not objects (`git cat-file` fails on both). Un-backticking them is
    correct, not dodging.
  - `60c6b410` (REVIEW_DEVBREAK2 :163) is a real blob, the superseded
    profile_executor_relay_io.c blob. Leaving it un-backticked follows the
    repo's superseded-citation convention. The new sentence "The blob id
    (60c6b410, a superseded value) is right" contradicts itself; "was right when
    written" would read better.
  - `e33e0f44` is the current blob of `firmware/SaftyFW/src/tasks/link_frame.c`
    at the dev tip (`git rev-parse origin/dev:...` = e33e0f446039...), and the
    last commit to touch the file is a364d1703, as the doc says. Like every blob
    pin it goes stale on the next edit of that file (the churn DEVBREAK2 noted).
  - The 09-16g symbol statement holds:
    `heat_enable_release(HEAT_ENABLE_CLAIMANT_PROFILE);` is at
    profile_executor_relay_io.c:1106. The parenthetical that follows ("citation
    refreshed again 2026-09-24 ...") now describes a citation that is gone, so
    it can be cut.
- INFO-3: check_reset_fence_hooks.ps1 pins only the kiln_nvs predicate body. The
  generic `relay_authority_reset_refuses_writer()`, which backs three of the four
  hooks, is left to the host test alone (R9 caught there, R10 not; see MED-1).
- INFO-4: REVIEW_DEVBREAK3's "Fix status" LOW-2 bullet holds a literal newline
  inside `[^` ... `]` (the `\n` was expanded when written), which splits the
  bullet into a stray `].` line.
- INFO-5: the GATE_NEGATIVE_TEST_EVIDENCE row for check_reset_fence_hooks.ps1
  lists the DEVBREAK3 mutations as CAUGHT, which is accurate. It should not be
  read as covering the preprocessor and job-window classes above.
