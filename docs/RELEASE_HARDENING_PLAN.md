# Release Hardening Plan — what has to be true before this controls a real kiln

> **Status:** plan · **Opened:** 2026-09-16. §1 closed by owner decision, §8/§9 done, §2 items 1-2 done; the rest not implemented. No
> board was flashed, no heating run was performed, and no `.kicad_*` file was
> touched while writing it.
>
> This plan starts once `docs/KILN_PROFILES_PLAN.md` is finished. It does not
> re-plan anything that plan, `docs/SETUP_WIZARD.md`,
> `firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md`, `docs/ON_OFF_ZONE_PLAN.md`
> or `docs/ITER_TUNE_REDESIGN_PLAN.md` already owns; where release depends on
> one of those, it is named as a dependency rather than duplicated.

## 0. What "release" means here, and why that changes the bar

Everything this repository has been verified against so far is a roughly 4 W
bench fixture at 120 V, run attended, for minutes to a few hours, with a
developer on the keyboard and a second session usually watching. Release means
the opposite of every one of those: kilowatts of real element, an enclosure
that stores enough heat to keep climbing after every relay opens, twelve-hour
firings, and nobody in the room. Two consequences follow, and they set the
shape of this whole plan.

First, the failure that matters is no longer "the feature did the wrong
thing". It is "the board stopped, or lied, or drifted, at hour seven, and the
elements stayed on". Nearly all of this repository's mechanical coverage is
static analysis and host tests that execute in milliseconds. That coverage is
genuinely good — see section 6 for what it already closes — but time is the
axis it does not cover at all, and time is the axis release is defined on.

Second, the safety argument has to stand on its own without the bench. The
bench fixture physically cannot exercise several of the guards the safety
argument leans on, and no amount of bench time will change that. A release
plan that quietly counts bench hours as safety evidence is making the exact
mistake `docs/SAFETY_CASE.md` §4 was written to prevent. Section 4 below
separates what the bench can close from what it structurally cannot, and says
what the second category costs.

The items below are ordered by risk. Each says what the gap is, why it blocks
*release* specifically rather than merely being desirable, how you would know
it is closed, and a rough size on this repository's existing S/M/L/XL scale.
**Release blockers are marked BLOCKER.** Everything else is explicitly not.

---

## 1. CLOSED by owner decision — the `profile_executor` panic no longer gates
## release

> **Status, corrected 2026-09-22:** the owner closed this line. It is not
> tracked as a release BLOCKER any more, and no session should re-dispatch
> work against it — **only a new crash report reopens it.** The decision
> itself has no separate dated audit entry or ROADMAP.md row of its own; a
> repo-wide grep for "profile_executor", "profiles_stop" and "owner" near
> this date found nothing beyond the coordinating session's own project
> memory (`project_profile_executor_panic_at_stop`), which carries no
> timestamp of its own beyond "recent" — so this section cites that memory as
> the closure's source rather than inventing a commit or date the repo does
> not otherwise record. If a future pass finds a dated audit or ROADMAP entry
> for this decision, update this note to cite it directly instead. The
> "release-defining defect" framing and the numbered "how to know it is
> closed" list below are kept as history — they describe what was true
> before the owner's decision, not a live gate any more.

**The gap (historical).** `profile_executor` had panicked four times with the same
signature: `IllegalInstruction`, `exc_pc = 0xfffffffd` (a saved PC of exactly
zero, i.e. a return through a smashed `a0`), `backtrace_corrupted = true`,
crash task `profile_executo`. The history is in
`docs/audits/cplval75_aborted_executor_panic_2026-09-09.md`,
`docs/audits/profile_executor_panic_2026-09-10_root_cause.md`,
`docs/audits/profile_executor_panic_recurrence_2026-09-14.md` and
`docs/audits/profile_executor_coredump_2026-09-15.md`. The 2026-09-10 root
cause was real and its fix held; the 2026-09-14 and 2026-09-15 events are
*different events wearing the same signature*, and the last of these could not
be symbolized at all because no coredump could be read off the board. A
deferred-release fix has since landed and been adversarially reviewed
(`docs/audits/review_executor_panic_fix_1c8d7f6e_2026-09-15.md`), which found
further ordering defects in the fix itself. The honest state is: **the task
that runs every firing crashes, on the real board, under exactly the
conditions release consists of, and nobody can currently read the evidence.**

**Why it blocks release.** Every other item in this plan is conditional on
this one. A multi-hour unattended firing is precisely the workload that
produced all four crashes. The crash takes down the task that owns setpoint
progression and relay commands; what the elements do afterwards depends on
paths that have themselves never been exercised from this particular entry
state. Shipping with a known, recurring, unsymbolizable crash in the firing
task is not a risk trade — it is an unbounded one.

**How to know it is closed.** In order, and none of these substitutes for the
next:

1. ~~Coredump readback has to work first~~ — **done, `4af518ca`.** The prior
   failure was two real bugs: wrong `espcoredump` CLI args (positional path
   instead of `--core`/`--core-format`, no `--chip`) and the wrong
   interpreter (defaulted to the MCP server's own venv, which lacks
   `esp_coredump`), whose `ModuleNotFoundError` was misreported as "very
   likely an ELF/coredump mismatch" — an environment failure dressed as a
   substantive verdict. Fixed and confirmed live: `espcoredump` now runs far
   enough to report its own genuine verdict. Host-tested:
   `tools/PcTools/tests/test_coredump_symbolize_and_archive.py`. Also adds a
   durable, content-addressed coredump archive
   (`firmware/KilnFW/coredump_archive/`) so a fetched dump is never
   overwritten. The existing 748032-byte on-board dump is still permanently
   unsymbolizable — its matching ELF (build `Sep 14 2026 23:55:17Z`) was lost
   before the ELF-archive durability fix (`a347e726`) landed, and nothing
   recovers lost ELFs after the fact. **Residual gap, closed 2026-09-16:**
   `find_crash_elf()` (`mcp_server_flash.py:970`) still resolves against the
   board's *currently running* `fw_build`, still wrong for a stored dump's
   origin build — but callers no longer have to trust it. `read_esp_coredump()`
   now falls through to `coredump_fetch.find_matching_archived_elf()` on a
   mismatch instead of stopping at the first candidate, and a new tool,
   `find_crash_elf_for_coredump(coredump_path)`, verifies directly against
   the coredump's own embedded SHA256 across every archived ELF
   (`elf_archive.list_all_kiln_elf_paths()`) rather than any externally
   reported build identity. Two distinct, loud outcomes: an environment
   problem (wrong interpreter, no `esp_coredump`) aborts immediately naming
   it; every archived ELF failing the SHA256 check is reported as
   PERMANENTLY UNSYMBOLIZABLE — a legitimate outcome, not a mismatch or a
   tooling failure. The 748032-byte on-board dump above remains exactly that
   legitimate outcome: its ELF is gone, so nothing will ever match it.
   Negative tests: `tools/PcTools/tests/test_coredump_fetch.py`'s
   `FindMatchingArchivedElfTests` (reproduces the wrong-ELF selection via the
   unchanged `elf_archive.find_kiln_elf_for_build()` lookup, then proves the
   content-based search corrects it). Full detail:
   `docs/audits/release_hardening_plan_verify_1_2_5_7_8_2026-09-16.md`.
2. **A stack-margin measurement taken during a firing, not at idle.** The idle
   baseline is a floor, not a worst case — that is already recorded as a
   standing caveat in `tools/PcTools/scripts/stability_soak.py`'s own
   docstring. `check_executor_task_stack_budget.ps1` computes a static deepest
   path; the last review of it reported honest headroom around 27%, classified
   LOW. Static analysis of the deepest *known* path and a runtime high-water
   mark under a real firing are different measurements and this defect lives
   in the gap between them. **Size: M.**
3. **Two consecutive clean firings of at least the intended unattended
   duration, on the release candidate build, with the crash report read and
   confirmed absent at the end of each.** Not "no crash observed" — an
   explicit `GET /api/crash_report` read showing `present=false`. Note that a
   panic-and-clean-reboot reads as healthy to everything except that endpoint,
   which is why `get_heap_status` was fixed to surface it. **Size: L, and
   mostly wall-clock.**

**Trap to avoid.** Do not declare this closed on the basis of a build that
fixed *a* stack path. The 2026-09-14 pass established that the static stack
story which explained the first two events does not explain the third. A fix
without a symbolized backtrace is a hypothesis, and this defect has already
survived one confident fix.

---

## 2. BLOCKER — long-duration behaviour has never been measured over the
## duration release is defined by

**The gap.** `tools/PcTools/scripts/stability_soak.py` exists and is good: it
samples heap free and min-free, per-task stack margin, safety link counters,
guard warn/trip masks, reset reason and unacknowledged crash reports on a
cycle, appends to CSV, and uses *flat counters* rather than absolute values as
its pass criterion — which is the right choice for a link whose counters never
reset. What does not exist is a run of it that covers the release duration
**with a firing active**, and a defined verdict for what the resulting series
must look like.

Four specific quantities have no long-duration evidence:

- **Internal DRAM.** The documented failure threshold is real and measured:
  below roughly 11.9 kB free the board starts resetting HTTP sockets. Recent
  readings sit near 32–34 kB min-free. That is comfortable but it is a
  *snapshot*; nothing establishes the slope over twelve hours with a web
  client polling, LVGL rendering, and a profile logging.
- **Stack headroom under load**, per item 1 above.
- **Flash wear and config churn.** The endurance review concluded there is no
  wear problem, and that conclusion is believed — but the dual-write to the
  `cfg` LittleFS partition alongside NVS changed the write pattern after that
  review, and `docs/CONFIG_FILESYSTEM.md` carries its own open items.
- **Link stability.** `bench_link_health.py` measures per-command latency and
  catches both the "stopped answering" and "answers but slowly" failure
  shapes. It is a spot check, not a duration measurement.

**Why it blocks release.** Every one of these is a slow leak, and a slow leak
is exactly the defect class that passes an attended bench session and fails an
unattended firing. The board being demonstrably healthy at minute ten is not
evidence about hour eight; this is the same "a snapshot is not a trend" error
the idle-stack-baseline caveat already names.

**How to know it is closed.** ~~Extend `stability_soak.py` with an explicit
verdict rather than only a CSV~~ — **partly done already.** `main()` already
returns a real PASS/FAIL verdict with a non-zero exit (1 on any accumulated
problem, 2 on a precondition failure), and already flags a heap-floor breach,
a DOWN trend on heap-free or stack-min-headroom%, and a positive delta on
`crc_errors`/`timeouts`/`broadcast_dropped` (landed across `95ca7e6e`,
`e84a2db5`, `3780030f`). Of the three items named next, two are now done
(slope test, negative test); one remains open (the actual release-duration
run):

1. ~~**The trend test is not a slope test.**~~ **DONE.** `_trend_direction()`
   no longer compares only the first and last sample. It now fits a real
   ordinary-least-squares line over every sample against `t_s`, expresses
   the result as a slope in units per hour, and classifies DOWN/UP only once
   the fitted slope's magnitude exceeds a per-metric floor
   (`HEAP_TREND_FLOOR_BYTES_PER_HOUR`, `STACK_PCT_TREND_FLOOR_PP_PER_HOUR`) —
   immune to the failure mode named here, where a monotone drift that
   happens to return near its starting value was invisible to a two-point
   comparison. Verdict semantics (DOWN still fails the run, non-zero exit)
   are unchanged.
2. ~~**The harness itself has never been negative-tested.**~~ **DONE.**
   `tools/PcTools/tests/test_stability_soak_trend.py` feeds the OLD
   first-vs-last `_trend_direction()` (loaded verbatim from the pre-fix
   commit via `git show`, not a hand-transcribed stand-in) a series that
   drifts strongly up or down but returns near its starting value at the
   last sample, and confirms the old code reports "flat" on that series
   while the new least-squares classifier correctly reports UP/DOWN with the
   real slope. Also covers noise-only series (must stay flat) and a
   realistic noisy monotone decline (must be flagged DOWN).
3. **No run of the intended release duration exists**, with or without a
   firing active — no `stability_soak_*.csv` artifact or audit record exists
   anywhere in the tree. **Still open — not attempted as part of this pass.**

**Size: L for the remaining run.** The slope test and negative test (item
1/2 above) are closed. Full verification detail:
`docs/audits/release_hardening_plan_verify_1_2_5_7_8_2026-09-16.md`.

**Already partly covered, and worth saying:** the metric *selection* problem
is solved. `stability_soak.py` already samples the right things, already
knows why flat counters beat absolute ones, and already carries the
idle-baseline caveat in its own docstring. This item is about duration and a
verdict, not about instrumentation.

---

## 3. BLOCKER — test coverage that survives this repository's own failure modes

**The gap.** This repository has a documented, repeated history of tests that
pass for the wrong reason. The known instances are worth restating as a
checklist because they are the acceptance criteria for any coverage this plan
adds:

- A check that passes against the *unfixed* code (vacuous), caught only by
  negative-testing — and eight shipped that way before negative-testing became
  standing practice.
- A check that read a sibling process's output file instead of producing its
  own.
- A harness printing a PASS-shaped verdict while exiting 0 on a regression.
- A verdict measured from a prebuilt binary that an earlier negative test had
  poisoned, where an empty `git diff` was taken as proof and the stale `.exe`
  in the build directory was not.
- A guard that skipped its own body (`if not path.is_file(): skipTest(...)`)
  and reported green with zero coverage — the reason `run_all_checks.ps1` now
  treats SKIP as a failure by default.
- A mirror-drift test written against a test-local copy of the production
  table, which proves nothing about production.

**Why it blocks release.** Not because the existing suite is weak — it is
unusually strong for a project this size — but because the *release decision*
will be made by reading a green suite. If any gate in that suite is green for
a reason unrelated to the code being correct, the release decision is made on
a forgery. The cost of this class is already measured: one poisoned-binary
verdict reached the project owner before it was root-caused.

**How to know it is closed.** A release-gate audit, executed once, over every
check that the release decision will cite:

1. **Negative-test every gate that has not been negative-tested.** Break the
   thing it claims to guard, confirm RED, restore **by hand** (never
   `git checkout --`, which discards other sessions' WIP), and then **force a
   full rebuild** before measuring anything — an empty `git diff` proves the
   source is restored and says nothing about build artifacts.
2. **Confirm each gate produces its own inputs.** For every check that reads a
   file, establish who wrote that file and when. A check reading an artifact it
   did not cause to be built is the poisoned-binary shape.
3. **Confirm each gate's failure exit actually propagates.** Non-zero on
   failure, exit 3 with a stated reason on a genuine missing prerequisite, and
   nothing else — the contract already written in `tools/run_all_checks.ps1`'s
   header. Verify by injection, not by reading.
4. **Record the result as a table of gate → negative-test evidence**, so the
   next session does not have to re-derive which gates are trustworthy.

**Size: L.** It is mechanical but there are roughly ninety-four checks plus
the host-test suites, and the ones that matter for release are a subset —
scope it to the safety, config, stack-budget and build gates first.

**Progress.** Five passes of negative-testing have run so far:
`docs/audits/release_gate_vacuity_audit_2026-09-16.md` (first slice),
`docs/audits/release_gate_vacuity_audit_2026-09-16b.md` (six mirror-drift
checks plus the KilnFW stack-budget extraction-fidelity question),
`docs/audits/release_gate_vacuity_audit_2026-09-16c.md` (KilnFW's
`check_httpd_task_stack_budget`/`check_system_uart_bridge_stack_budget`/
`check_uart_log_bridge_stack_budget`, SaftyFW's ARM stack-budget checker and
its isolation/guard-producer family — nine gates total, plus a behavioral
negative test of the new `-RunSetup` worktree-mint switch below), and
`docs/audits/release_gate_vacuity_audit_2026-09-16d.md` (`check_uart_version_
independence`, `check_uri_handler_cap`, `check_heartbeat_contract`,
`check_heat_enable_wiring`, `check_c_files_in_cmakelists`,
`check_no_duplicate_crc`, `check_safety_baud_sync`, and the PC-side half of
`check_relay_authority_paths` — eight gates), and
`docs/audits/release_gate_vacuity_audit_2026-09-16e.md` (the firmware-side
half of `check_relay_authority_paths` plus a materially widened PC-side rule,
`check_mcp_facade_coverage`, `check_mcp_tool_count_doc`,
`check_test_has_assertions`, `check_test_c_files_wired`,
`check_stack_margin_registration`'s create-vs-register sub-check,
`check_safety_trip_mask_docs`, `check_mykicad_golden_suite_runs` — eight
gates, plus `check_duplicate_symbols` examined at a fresh-build baseline
only, FAIL path not yet exercised). Every gate negative-tested across the
five passes was found load-bearing; none was vacuous, though one stale
documented negative-test example was found in `check_mcp_facade_coverage.py`
(`plant_sim_compare`, now independently covered by a later `GROUP_PREFIXES`
entry — reported, not silently rewritten) and one real, live production bug
was found and fixed: `actions.py`'s `"IO: All Relays Off"` GUI action
bypassed `IoClient`'s refusal-aware wrapper via a bare `_send()` call,
caught by the fifth pass's widened `check_relay_authority_paths.py` rule;
fixing it surfaced a second issue — routing relay actions through
`_client_query()` drops the protocol-compatibility gate `_send()` applies —
fixed by adding `_gated_client_query()` and using it for all three relay
actions, which also closed a pre-existing, never-tested gap in
`"IO: Set Relay"`/`"IO: Set Relay Mask"`. The fifth pass also found
`check_01_kilnfw_pushed_build.ps1` genuinely, currently failing against live
`origin/main` (a `-Werror=format-truncation` defect in `backup_import.c`
from concurrent, unrelated backup/config-migration work) — out of scope to
fix here, but strong unplanned evidence the check is load-bearing.
`tools/worktree_mint.ps1 -RunSetup` (added in the third pass) lets a minted
worktree actually run `tools/run_all_checks.ps1` to completion, which the
first two passes could not do; the fourth, fifth and sixth passes used it
directly. The fourth pass also fixed 11 pre-existing `check_doc_hash_
citations.ps1` false positives in the third pass's own document
(git-hash-object blob hashes misread as commit citations — rewritten as
`` `blob:<hash>` `` rather than deleted).

A sixth pass, `docs/audits/release_gate_vacuity_audit_2026-09-16f.md`, closed
the fifth pass's remaining four `check_stack_margin_registration.ps1`
sub-checks (required-name-missing, duplicate-name, cap-vs-count, hwAbstraction
accessor/boundary), exercised `check_duplicate_symbols.ps1`'s FAIL path for
the first time (requiring a real ESP-IDF build; merged an upstream fix for
the `backup_import.c` `-Werror=format-truncation` defect the fifth pass had
found to get a clean baseline), and negative-tested two of the seven
UI/layout checks (`check_ui_budget_asserts`, `check_ui_status_color`) — eight
gates total, all load-bearing. It also replaced `check_mcp_facade_coverage
.py`'s stale documented negative-test example (`plant_sim_compare`, flagged
but not fixed by the fifth pass) with a fresh one (`ramp_assist_set_enabled`)
and verified it actually fails today, and found — but deliberately left
unfixed, as a scoped follow-up — a real, currently-live vacuity gap in
`check_duplicate_symbols.ps1` itself: its `$componentSourceRoots` mapping for
the `hwabstraction_esp` component omits `firmware/hwAbstraction/common`, even
though that component's own `CMakeLists.txt` compiles `hal_status.c` from
there, so `hal_status.c.obj` is silently misclassified as stale build output
and excluded from duplicate-symbol scanning on every run. **Fixed by the seventh pass** (below); confirmed at HEAD: `tools/check_duplicate_symbols.ps1:143` now lists `@("firmware\hwAbstraction\esp", "firmware\hwAbstraction\common")` for `hwabstraction_esp`. The sixth pass's
own document lists what remains unexamined — five of the seven UI/layout
checks, and both `check_00_*_target_build.ps1`/`check_01_*_pushed_build.ps1`
pairs' FAIL paths (still only exercised at a clean baseline, never
deliberately sabotaged).

A seventh pass, `docs/audits/release_gate_vacuity_audit_2026-09-16g.md`,
fixed the `check_duplicate_symbols.ps1` `hwabstraction_esp` source-root gap
the sixth pass had found but deliberately left unfixed (`hal_status.c.obj`
was silently classified as stale and dropped from every scan; now correctly
attributed, 264 to 265 objects scanned), verified by planting a genuine
duplicate symbol spanning exactly `hal_status.c` and `hal_esp_common.c` and
confirming the fix is what makes it visible at all. It also negative-tested
eight more gates -- `check_c_files_in_cmakelists`, `check_no_duplicate_crc`,
`check_relay_authority_paths`, `check_heat_enable_wiring`,
`check_uri_handler_cap`, `check_test_c_files_wired`,
`check_uart_version_independence`, and `check_heartbeat_contract` (the
cross-language PC/firmware heartbeat producer-consumer pair) -- all found
load-bearing.

**Status, 2026-09-17 (eighth pass, partial).** Closed three of the sixth
pass's remaining five untested UI/layout gates: `check_kv_narrow_stack.ps1`
(sabotaged `ota_page.html`'s `.kv dd { min-width: 0 }`, confirmed RED),
`check_label_column_overflow_wrap.ps1` (sabotaged `diagnostics_page.html`'s
`.row .label` rule to `overflow-wrap: anywhere`, confirmed RED), and
`check_stop_bar_body_padding.ps1` (sabotaged `app.js`'s `ResizeObserver`
reference, confirmed RED) -- each restored by hand, confirmed by an empty
`git diff` AND a matching `git hash-object` against the pre-sabotage blob,
then re-run and confirmed PASS again through `run_all_checks.ps1` itself
(not just the standalone script). No production/build artifact involved for
any of the three (pure static-file greps), so no rebuild step was needed.
All three found load-bearing. This adds no new checks, so the suite total is
unchanged at 107. Remaining in this item, not attempted this pass:
`check_ui_responsive_sweep.ps1` (the one UI/layout gate left untested -- a
headless-Chrome sweep, expensive to sabotage safely) and both
`check_00_*_target_build.ps1`/`check_01_*_pushed_build.ps1` pairs' FAIL
paths. Also unrevisited: item 1's gated-out-test sweep and the
zero-production-caller sweep called for by this item's own acceptance
criteria have not been re-run as a fresh scripted pass in this session --
the seven prior passes' findings stand but were not re-verified here.

**Status, 2026-09-17 (ninth pass).** Closed `check_ui_responsive_sweep.ps1`
and both `check_00_*_target_build.ps1` FAIL paths.

Root cause of the three 2026-09-17 misfires (twice SKIP, once a hard FAIL on
`Page.loadEventFired`, all passing clean on an immediate standalone rerun):
`ui_responsive_sweep.mjs`'s `isTransientHarnessError()` classifier -- which
decides whether a CDP/network exception during the sweep is a retriable
harness hiccup or a genuine layout-regression FAIL -- did not match the exact
string `CdpSession.waitForEvent()`'s own timeout produces (`` `timed out
waiting for ${method}` ``), even though the sibling `CdpSession.send()`
per-call timeout was already covered by an existing regex. A contention-
caused slow page load under `run_all_checks.ps1`'s default 8-way parallel
phase therefore fell through to the "real exception" branch and was reported
as a hard, unretried FAIL instead of being retried. Corroborated live:
`tasklist` showed a dozen-plus concurrent `chrome.exe` processes on the
shared machine at the time, matching the file's own prior-incident comments;
a standalone rerun of the check with that same ambient load passed clean
(129/129) in ~71s, matching the reported "always passes standalone" pattern.

Fix: added the missing regex clause to `isTransientHarnessError()`, exported
it (it was previously unexported and therefore untestable in isolation), and
added `firmware/KilnFW/App/test/ui_responsive_sweep_classify.test.mjs` -- a
pure unit-level negative test asserting 13 classifications (the two
2026-09-17-gap cases, six pre-existing transient classifications that must
not regress, and five messages, including a 2026-09-09 regression guard,
that must stay hard FAILs). Also fixed a side issue found while making the
function testable: the module's `main()` was called unconditionally at
import time, so importing it for a unit test launched a full live Chrome
sweep as a side effect; guarded behind an entry-point check
(`if (path.resolve(process.argv[1] || '') === __filename)`).

Sabotage-and-restore proof (in a minted worktree, `git hash-object` matching
the pre-sabotage blob both before and after): removing the new regex line
reproduced RED (2 of 13 assertions failed, exactly the two gap cases);
restoring by hand and re-running confirmed GREEN again with a hash match.
No production build artifact involved (pure `.mjs`/string-classifier logic),
so no rebuild step was required for this fix.

Determinism under parallel execution: fixing the classifier alone does not
eliminate contention entirely (the sweep's own internal wall-clock timeouts
can still stretch under load even when every result is classified
correctly), so `check_ui_responsive_sweep.ps1` was also moved into a new,
third execution phase in `tools/run_all_checks.ps1` (`-MaxParallel 1`),
run after the existing build-check and throttled-parallel phases, so it
never shares a scheduling slot with up to 7 concurrently-running sibling
checks. Two consecutive full foreground `run_all_checks.ps1` runs after this
change both returned a clean verdict with the sweep check passing in its own
phase both times -- see the verbatim verdict line below.

Added `firmware/KilnFW/App/test/test_check_ui_responsive_sweep.ps1`, a thin
wrapper running the new `.mjs` unit test, wired in by name in
`tools/run_all_checks.ps1` following the existing `test_check_*.ps1`
convention (same block shape as the hal-boundary/stop-path negative tests).
Negative-tested the wrapper itself the same way: sabotage reproduced RED
(wrapper throws with the node exit code), hand-restore + hash match restored
GREEN.

Both `check_00_*_target_build.ps1` FAIL paths, never previously exercised,
were negative-tested in a separate minted worktree: appending `#error
SABOTAGE_NEGATIVE_TEST_<NAME>_BUILD` to `firmware/KilnFW/App/main.c` (resp.
`firmware/SaftyFW/src/boot_reason.c`) reproduced a genuine compiler error and
a FAILED verdict naming that exact line for each; restored by hand, confirmed
an empty `git diff` and a matching `git hash-object` against the
pre-sabotage blob for each, then deleted the build directory and forced a
full rebuild before re-measuring -- both came back PASS. Both checks are
proven able to fail on a genuinely broken build.

Suite total: this pass adds one new file wired by name
(`test_check_ui_responsive_sweep.ps1`; `check_ui_responsive_sweep.ps1`
itself already existed and was already glob-discovered, so it does not add
a second count). Verbatim verdict from a full foreground
`tools/run_all_checks.ps1` run after all changes: `109 passed, 0 skipped, 0
failed.` -- sums correctly (109 = 109 + 0 + 0), no `-AllowSkips` needed,
`check_recovery_image_size.ps1` passed (did not need to SKIP this run).

Not reached this pass: the Python-side zero-production-caller sweep for
`tools/PcTools/tests` (explicitly out of scope of the eighth pass's C-only
sweep) remains unstarted.

**Status, 2026-09-17 (separate ninth-pass thread): the gated-out-test and zero-production-
caller sweeps, run as fresh scripted passes for the first time since this
item's acceptance criteria called for them.** Both done in a clean
`tools/worktree_mint.ps1`-minted worktree, provisioned via `git submodule
update --init --recursive` + `tools/setup.ps1`.

*Gated-out-test sweep.* A two-stage regex scan (broad shape match, then
narrowed to prerequisite-check-immediately-followed-by-skip/pass) covered
492 files under `firmware/*/test/`, `firmware/*/tools/`, `tools/`, and
`tools/PcTools/tests/`, producing 153 narrowed hits. Every hit resolved to
one of: (a) a `Get-Command python`/`python3`/`node`/`ffmpeg` fallback
picking whichever interpreter/tool is on PATH -- not a skip; (b) a
documented, gitignored-by-design optional-data skip (`test_fuzzy_band_
probe.py`, `test_noise_floor.py` x2, `test_plant_sim.py`'s hardware-capture
`self.skipTest`/`pytest.skip` guards -- their `logs/coupling/*.jsonl`
inputs are covered by `.gitignore`'s `logs/**/*.jsonl` line, confirmed
absent from `origin/main`'s tree; a pytest skip is counted and printed by
the runner, so this is visible, not silent); (c) `tools/PcTools/selfcheck.
py`'s `tools/mykicadMcp` submodule-not-checked-out fallback and `selfcheck_
commonfw.py`'s CommonFW-vectors-manifest fallback -- both LIVE on a
provisioned checkout (all referenced files present and confirmed on
`origin/main`), by design INERT-with-a-printed-message on an unprovisioned
one; (d) `tools/mykicadMcp/tests/conftest.py`'s `scratch_board` fixture
skip, unreachable today because its own upstream `kiln_project_path`
fixture already hard-fails (`pytest.fail`, not skip) if the board is
missing at all; (e) `test_kicad_cli_acceptance.py`'s documented
machine-capability skip, LIVE on this bench (KiCad 10 installed). No
instance of the confirmed defect shape (`if not path.is_file():
skipTest(...)` masking a renamed/moved prerequisite) was found live today.

One real, narrower gap was found and fixed: `tools/check_test_has_
assertions.ps1` scans a hardcoded list of three known-good test
directories (`firmware/KilnFW/App/test`, `firmware/SaftyFW/test`,
`tools/PcTools/tests`) but only guarded each with `if (-not (Test-Path
$dir)) { continue }` -- silently scanning fewer directories rather than
failing, with only a "zero directories total" floor check to catch total
loss. Negative-tested by renaming `firmware/SaftyFW/test` aside: the
pre-fix script printed "Test-has-assertions check passed" and exited 0
with an entire suite's worth of coverage silently dropped (confirmed by
`git stash`-ing the fix and re-running against the same sabotage). Fixed by
failing loud (`throw`) if any of the three fixed paths is missing, since
none of them is an optional/machine-dependent prerequisite in this repo.
Restored the sabotaged directory by hand; `git hash-object` on the fixed
script matched pre- and post-verification. This is the only source change
this pass made.

*Zero-production-caller sweep.* A scripted pass paired each `test_<subject>
.c` under `firmware/KilnFW/App/test` and `firmware/SaftyFW/test` with its
matching production `<subject>.c`, extracted that file's function
definitions, ranked them by reference frequency in the test file, and
`git grep`'d the top candidates (321 rows) for call sites outside the
defining file and outside any `test`/`tests` directory. 7 rows came back
zero. All 7 checked by hand and found NOT genuinely unwired: 5
(`log_store.c`'s `kind_prefix`, `partition_info_http.c`'s `send_chunk`,
`ramp_assist_cfg.c`'s `ramp_assist_validate`, `safety_cfg_http.c`'s
`rate_guard_auto_compute`, `zones_http.c`'s `tuning_rec_body_len`) are
`static` helpers called only from elsewhere in their own defining file --
the script's exclude-defining-file rule produced a false zero, not a real
one; 2 are genuinely indirect-dispatch: `time_sync.c`'s `tz_file_validate`
is passed by name as a callback to `pref_cfg_fs_load_raw(...)`, and `zones_
http.c`'s `sweep_status_get_handler` is registered in a static
`httpd_uri_t` table (`.handler = sweep_status_get_handler`) -- both reached
in production, just not via a textually-adjacent call. No genuinely
unwired subject (the `web_auth_table_create_session` shape) was found in
this pass's scope. Python suites under `tools/PcTools/tests` were not run
through the same script this pass (time-boxed); this is scoped-out, not
verified-clean.

Suite: 108 guard scripts discovered by `run_all_checks.ps1` in this
worktree (one more than the eighth pass's stated 107 -- not reconciled
here, no check was added or removed this pass, so the discrepancy predates
this session's change). Full run after the fix: **108 passed, 0 skipped, 0
failed.**

Remaining, unattempted this pass: the eighth pass's leftover items
(`check_ui_responsive_sweep.ps1` negative test, both `check_00_*_target_
build.ps1`/`check_01_*_pushed_build.ps1` pairs' FAIL paths — note these were
in fact closed by the *other* ninth-pass thread above, which ran
concurrently and was not cross-referenced here at the time of writing), the
107-vs-108 discovered-count discrepancy, and full Python-side coverage for
the zero-production-caller sweep.

*Reconciled:* both items closed by the tenth pass below (count drift explained there as expected; Python sweep run, no unwired production function). The suite is now 157 checks at `a1bcfb0f`.

**Status, 2026-09-17 (tenth pass, host-only, no board touched).** Closed the
two items this section's own text left open above.

*Discovered-count discrepancy.* Re-run in a freshly minted worktree at
current `origin/main`: **110 passed, 0 skipped, 0 failed** — neither 107 nor
108 nor the 109 this plan's own instructions had been quoting. Traced this
to the actual cause rather than reconciling a single snapshot: the suite is
discovered by an unpinned `Get-ChildItem -Recurse -Filter "check_*.ps1"` glob
(`tools/run_all_checks.ps1:157`) plus a handful of explicitly wired
`test_*.ps1` files, and `origin/main` has had checks added by other,
concurrently-running sessions between every one of these passes (the eighth
pass's `test_check_ui_responsive_sweep.ps1`, the ninth pass's own additions,
and others outside this plan's own edit history). **This is not a defect to
reconcile** — the count is expected to keep moving under a shared, actively
developed tree, exactly as this session's own operating instructions warn
("expect `origin/main` to move under you"). There is nothing here that a
fixed constant or a reconciliation table would keep correct for more than a
few commits; the actionable invariant is what every pass has already been
checking — `passed + skipped + failed` sums to the discovered total and
`failed` is 0 — which this run satisfies (110 = 110 + 0 + 0).

*Python-side zero-production-caller sweep.* Ran the same shape of sweep the
ninth pass ran for C, applied for the first time to
`tools/PcTools/tests` (137 `test_*.py` files): paired each with its
production `<subject>.py` under `tools/PcTools/src/kilnctrl`,
`tools/PcTools/src/mcpkit`, `tools/PcTools/scripts` and
`tools/PcTools/ui_scripts`, ranked that file's top-level (non-underscore)
functions by reference count in the test file, and `git grep`'d the 8
most-referenced names per pairing (264 rows total) for call sites outside
the defining file and outside any `tests` directory. 62 rows came back
zero. 59 of the 62 were false zeros of the same shape the C sweep already
named: a helper called only elsewhere in its own defining file (the
grep intentionally excludes the defining file to look for *outside*
callers, which undercounts these). The remaining 3 —
`fuzzy_load_sweep.find_best_strength_per_load`,
`http_capture_log.starting_temps_c`, `load_mass_sweep.run_profile7_loaded`
— have no caller anywhere in the repository outside their own test files,
confirmed by an unrestricted repo-wide `git grep` on each name (not just
the ranked-candidate methodology). Checked each by hand against the
`web_auth_table_create_session` shape this item's acceptance criteria is
watching for (a live, wired-in production entry point nothing actually
calls) and found a different, benign shape instead: all three live in
one-off research/analysis modules (`fuzzy_load_sweep.py`, `load_mass_sweep.py`,
`http_capture_log.py`) whose own module docstrings describe them as ad hoc,
interactively-invoked sweep tooling for a specific owner request — none of
the three files has a `__main__` block, a CLI entry point, or any documented
invocation other than importing the module by hand. There is no dashboard,
HTTP route, or scheduled job that should be calling these and silently
isn't; they are unwired in the same sense every other function in these
files not already covered above is unwired — by design, pending a human
running the sweep again. No genuinely unwired *production* (safety- or
runtime-relevant) function was found in the Python-side sweep. Suite
verdict unaffected: no source change was needed, since every finding
resolved to a false zero or a non-production research module — 110 passed,
0 skipped, 0 failed after this pass (same run as above).

**Status, 2026-09-18 (tenth pass).**
`docs/audits/release_gate_vacuity_audit_2026-09-18.md`. This pass led with
*mechanical screens over the whole check population* rather than a
hand-picked slice, on the reasoning that nine prior passes had already
negative-tested most individually-interesting gates. Four screens ran across
all 119 tracked `check_*`/`test_check_*` scripts, and all 34 screen hits were
resolved by hand: (A) no-failure-path -- 7 flagged, all false positives
(Python checks signalling failure as `return 1` + `sys.exit(main())`);
(B) skip-instead-of-fail `Test-Path` guards -- 27 hits, none vacuous
(`check_test_has_assertions.ps1`'s two are now dead code behind the
2026-09-17 `throw`; `check_hal_include_boundary.ps1`'s are backed by a hard
`throw`, a 200-file discovery floor and a `throw` on any unreadable file;
`check_c_files_in_cmakelists.ps1`'s fails safe by producing *more*
violations); (C) wrapper-invokes-`.py` -- 14/14 clean.

Screen (D) found the pass's one real defect, in
`tools/check_no_orphaned_checks.ps1` -- the guard whose entire job is
detecting lost coverage. It matched filenames against the **raw text** of
`run_all_checks.ps1`, comments included. That file's header narrates past
orphan incidents by filename, so `check_saftyfw_task_count.py` was treated as
wired on the strength of **line 311 -- a comment describing the time it went
unwired**. The note written to record the bug was what hid it. A second,
weaker layer accepted a sibling `.ps1` wrapper's mere existence as proof it
invoked its `.py`. Both closed: the wrapper must now reference the `.py` in
non-comment code, and the fallback matches a comment-stripped
`$runAllChecksCode`. Measured before adopting -- all 14 wrapper/`.py` pairs
reference their `.py` in code, and the only two comment-only-mentioned files
are absolved by the strengthened wrapper rule instead -- so it flags nothing
correct; it tightens and loosens nothing.

Negative-tested three ways, which is what separated the two weaknesses:
against one hand-made sabotage the pre-fix guard **PASSED** (vacuous), the
wrapper fix alone **still PASSED** (the prose match absolved it), and only
both fixes together **FAILED, exit 1**, naming the orphan. Had the pass
stopped at the first fix it would have shipped a change that fixed nothing
observable. Restored by hand, verified by an empty `git diff` *and* a
`git hash-object` match against the pre-sabotage blob. No build artifact is
involved (pure text scans), so no rebuild was required.

Now recorded as structurally un-negative-testable rather than merely
outstanding: both `check_01_*_pushed_build.ps1` FAIL paths. They build
`origin/main`'s *own* content, so a local sabotage cannot reach them and the
only way to drive the FAIL path is to push a knowingly-broken commit to the
shared branch. The fifth pass's unplanned real failure
(`-Werror=format-truncation`) stands as the direct evidence that path
propagates. Suite unchanged at `113 passed, 0 skipped, 0 failed.` -- this
pass modified an existing glob-discovered check rather than adding one.

**Closed, 2026-09-20: full Python-side coverage of the zero-production-caller
sweep.** The prior one-off passes (2026-09-17, twice) ran the sweep by hand
and were never made a standing check, so a new zero-caller function could
slip back in unnoticed. `tools/check_python_zero_caller_sweep.py` +
`tools/check_python_zero_caller_sweep.ps1` (glob-discovered) make it
permanent: every top-level, non-underscore function under
`tools/PcTools/src/kilnctrl` and `tools/PcTools/src/mcpkit` is checked for a
caller anywhere in the non-test tree (methodology fix vs. the one-off
scripts: the caller search now INCLUDES each function's own defining file,
so a `static`-shaped helper called only elsewhere in its own file is no
longer a false positive; a decorator-registered MCP tool -- `@_srv._tool()`
-- is also correctly treated as wired, the Python equivalent of the C
sweep's `httpd_uri_t`-table carve-out). First full-coverage run (909
functions scanned) found 8 genuinely zero-caller functions beyond the 3
already-reviewed benign ones: `devices_control.control_get_unit_pref`/
`control_set_unit_pref` (the shipped unit-preference feature went over HTTP
instead, leaving this CONTROL-task wire-protocol pair unused),
`safety_cfg_http_client.params_by_id` (superseded by name-based lookup per
its own sibling's docstring), `run_queue.entry_to_dict` (asymmetric pair --
`entry_from_dict` is called, this serializing half is not), `run_queue.
load_preset_json`, `pico_image_freshness.read_file_identity`, `cone_table.
cone_for_temp_c` (sibling functions `band_bottom_c`/`heat_work_weight` are
both used by `ramp_assist.py`; this one is not), and `log_analysis.
parse_history_csv` (the module's own CLI/report functions never call it,
unlike its CSV-parsing sibling `parse_trace_csv`). None matches the
`web_auth_table_create_session` shape (a live, wired-in production entry
point); all are unused helpers/leftovers. Recorded in the script's
`PENDING_OWNER_REVIEW` set (distinct from the benign `ZERO_CALLER_ALLOWLIST`)
with a one-line reason each, so the check stays green but the finding is not
silently dropped -- an owner call on wire-up-vs-delete is left open per item.
Negative-tested: planted a real zero-caller function in `cone_table.py`,
confirmed the check FAILs naming it; removed by hand (not `git checkout`)
and confirmed both an empty `git diff` and a `git hash-object` match against
the pre-sabotage blob, then re-ran and confirmed PASS. Full foreground
`run_all_checks.ps1 -AllowFewerChecks` run after adding the check:
`125 passed, 0 skipped, 0 failed`. `tools/PcTools/tests` itself (test
files, not production source) was a deliberate scope decision, not a gap
closed by this pass -- there is no "caller" concept for a test file to
sweep, and nothing below revisits that decision.

**Status, 2026-09-20b: caller matching corrected from text match to
token-based.** The 2026-09-20 pass above overclaimed coverage: its caller
search was a plain `\bname\b` regex over whole file contents, so a mention
in a comment, a docstring, an `__all__ = [...]` string-list entry, or a
function's own recursive self-call all counted as "used" -- the exact false-
negative shape a name-only text match always has. Fixed by switching the
search to `tokenize`-based NAME-token matching (`build_mention_index()`):
only identifiers that are real code -- never inside a string or a comment --
count as a mention, and each function's own signature line plus its own
indented body (computed via `_function_body_end()`) are excluded from its
own caller search, so a recursive self-call is no longer mistaken for an
outside caller. `__all__` membership specifically stops counting as a
caller not through special-case logic but as a consequence of the token
switch: an `__all__` entry is a STRING token, never a NAME token. The
decorator-wiring carve-out for MCP-registered tools (`@_srv._tool()` etc.)
is unaffected. Separately, allowlist/pending keys were `(path.name, name)`
basenames despite the comment's claim that they name the defining file --
`registry.py` exists at both `tools/PcTools/src/kilnctrl/bench_test/registry.py`
and `tools/PcTools/src/mcpkit/registry.py`, so a same-named entry could have
silently covered the wrong file's function. Keys are now the full
repo-relative POSIX path.

Re-running the corrected check (909 functions scanned, same two roots, ~1 s)
surfaced 25 previously-hidden zero-caller functions, on top of the 8 already
in `PENDING_OWNER_REVIEW` from the prior pass (which remained genuinely
zero-caller and needed no changes). All 25 were reviewed by hand. Ten are
NOT dead code -- they are `kilnlink_codec.py`'s `encode_context`/
`encode_status`/`encode_announce`/`encode_diag`/`encode_trip`/`encode_power`/
`encode_ceiling`/`encode_clear_trip`/`encode_get_fw_version`/
`encode_set_clock`, each reached only via
`getattr(kilnlink_codec, fn_name)` dispatch from a string-keyed manifest
table in `tools/PcTools/selfcheck_commonfw.py`. This is the token matcher's
own documented blind spot (dispatch by string name), confirmed by hand
against that manifest table and added to `ZERO_CALLER_ALLOWLIST` with a note
that they are real production callees, not benign-unused ones. The other 15
had no caller anywhere the check or a by-hand `git grep` could find,
including `tools/PcTools/scripts`/`ui_scripts` (outside the check's scan
roots but inside its caller search): `devices_io.digital_io_label`,
`http_capture_log.write_no_heat_diagnostic_tsv`,
`load_estimator.estimate_from_capture_path`, `noise_floor.load_artifact`,
`noise_floor.floor_lookup`, `log_analysis.parse_profile_exec_uart_capture`,
`mcp_server_flash.fixture_flash` (unlike its sibling `flash_firmware()`, it
carries no `@_srv._tool()` registration at all), `partition_table.
check_chip_partition_table` (its own docstring calls it deprecated; the CLI
script named after it calls the newer `_via_http()` variant instead),
`pico_image_freshness.check_slot_bins_fresh`, `plant_sim.
actuator_weight_sensitivity_sweep`, `plant_sim.per_zone_gain_holdout_report`,
`ramp_assist.compare_heat_work`, `ramp_assist.dwell_credit_parity`,
`stack_margin_baseline.load_pico_records`, and `mcpkit.registry.
collapse_table` (genuinely called, but only from the separate `mykicadMcp`
submodule this check deliberately excludes from its scan -- kilnctrl's own
`mcp_server.py` calls the sibling `collapse()` instead). None matches the
`web_auth_table_create_session` shape; all 15 went into
`PENDING_OWNER_REVIEW`, never deleted.

**Corrected totals.** Across both passes the sweep has now found **49**
zero-caller functions in total: **26** in `ZERO_CALLER_ALLOWLIST` (13
bulk-allowlisted 2026-09-20 as the same one-off-research-module shape as 3
originally reviewed 2026-09-17, plus the 10 `kilnlink_codec` getattr-dispatch
entries added this pass), and **23** in `PENDING_OWNER_REVIEW` (8 from
2026-09-20 plus the 15 above) awaiting an explicit owner wire-up-or-delete
call. The prior status text's "8 genuinely zero-caller functions" undercounted
because its own matching missed the 15 above.

**Remaining known blind spots**, stated plainly rather than implied by
omission: this is token-based NAME-token matching over each file's own text,
not full AST/type-aware call-graph analysis. It still cannot see (a)
dispatch-by-string -- `getattr(module, name_from_a_table)()`, a dict keyed by
function name, or any other call reached only through a string that happens
to equal the identifier (the `kilnlink_codec` case above, and the reason
that allowlist exists); (b) methods -- only top-level, non-underscore `def`
statements are scanned as *candidates*, and a bare method call
(`obj.some_method()`) is indistinguishable from an unrelated top-level
function that happens to share `some_method`'s name, so a same-named
method and top-level function could each hide the other's true caller
count; and (c) genuine dead code shadowed by a same-named method elsewhere
in the tree, for the same reason. These are accepted limits of a
lightweight per-repo text/token scan, not defects introduced by this pass;
negative-testing below still proves the check reliably catches the shapes it
is designed to catch.

Negative-tested three ways: (1) planted a genuinely dead function with no
reference anywhere -- FAILed, naming it; (2) planted a function referenced
ONLY inside a `#` comment and ONLY inside a string literal (both previously
enough to hide a zero-caller under the old text match) -- confirmed the
check still FAILs, naming it, proving the token switch actually changed
behavior rather than coincidentally leaving the old regex's blind spot
intact; restored both by hand (not `git checkout`) and confirmed an empty
`git diff` plus a `git hash-object` match against each file's pre-sabotage
blob. Full foreground `run_all_checks.ps1 -Fast -AllowFewerChecks`:
`122 passed, 0 skipped, 0 failed` (SaftyFW's slot bins needed a fresh
`check_00_saftyfw_target_build.ps1` run first -- unrelated to this change,
a shared-tree build artifact left stale by another session).

**Closed, 2026-09-21: owner review of the 23-function `PENDING_OWNER_REVIEW`
batch.** Per-function keep/delete review (worktree `zerocall`): 11 kept
(moved to `ZERO_CALLER_ALLOWLIST` with individual reason comments --
`cone_table.cone_for_temp_c`, `devices_io.digital_io_label`,
`log_analysis.parse_profile_exec_uart_capture`,
`mcp_server_flash.fixture_flash`, `pico_image_freshness.
check_slot_bins_fresh` (a genuinely new sweep blind spot found this pass:
its only real caller is Python embedded as a heredoc inside
`check_embedded_pico_image_fresh.ps1`, invisible to the sweep's
`.py`-file-glob corpus), `plant_sim.actuator_weight_sensitivity_sweep`,
`plant_sim.per_zone_gain_holdout_report`, `ramp_assist.compare_heat_work`,
`ramp_assist.dwell_credit_parity`, `stack_margin_baseline.
load_pico_records`, and `mcpkit.registry.collapse_table`); 12 deleted from
source along with their tests and doc mentions in the same commit --
`devices_control.control_get_unit_pref`/`control_set_unit_pref`,
`safety_cfg_http_client.params_by_id`, `run_queue.entry_to_dict`,
`run_queue.load_preset_json`, `pico_image_freshness.read_file_identity`,
`log_analysis.parse_history_csv`, `http_capture_log.
write_no_heat_diagnostic_tsv`, `load_estimator.estimate_from_capture_path`,
`noise_floor.floor_lookup`, `noise_floor.load_artifact`, and
`partition_table.check_chip_partition_table`. Deleting
`check_chip_partition_table` and `estimate_from_capture_path` orphaned
their own remaining in-repo callees (`partition_table.
parse_partition_table_binary`/`read_chip_partition_table_bytes`,
`load_estimator.estimate_all_zones`) as collateral new zero-callers; all
three were reviewed the same way and allowlisted (kept, not deleted --
each is genuine retained/deprecated or actively-tested logic, not itself
superseded). `PENDING_OWNER_REVIEW` is now empty. **Corrected totals:**
**40** in `ZERO_CALLER_ALLOWLIST` (26 prior + 11 kept + 3 collateral), **0**
in `PENDING_OWNER_REVIEW`. Full keep/delete table with one-line reasons: see
the commit message and `tools/check_python_zero_caller_sweep.py`'s own
`ZERO_CALLER_ALLOWLIST` comments.

**Already covered, name the evidence:** the two `check_01_*_pushed_build.ps1`
scripts are the strongest single piece of process coverage in the repo. They
build `origin/main`'s actual content in a clean worktree rather than the local
tree, which is the only thing that catches a push whose dependency closure was
not fully committed — a failure that happened at least four times in one day.
Their own header is honest about the one hole (`sdkconfig` is gitignored and
copied in from the main tree, so they prove "origin/main's source builds
against this machine's board config", not "a fresh clone builds"). Closing that
hole is **desirable, not a blocker**, and is item 11.

**Status, 2026-10-02 (eleventh pass).** `docs/audits/release_gate_vacuity_audit_2026-10-02.md`: 13 checks added or changed since 2026-09-18 negative-tested, all load-bearing; two discovery vacuities fixed (`check_no_exec_status_stack_locals.py`, `check_no_handler_direct_driver_calls.py` now fail on a missing scan directory or too few files).

---

## 4. BLOCKER — a safety argument that does not depend on the bench

**The gap.** The rollup in `docs/SAFETY_CASE.md` §4 is the number that matters:
of roughly twenty tracked guard-level claims, twenty are host-tested and
**three** are hardware-verified. The three are S5's hardware fit, S5's
masking-before-fit finding, and KilnFW's thermal_guard guard 6. Everything
else — all of S1–S4 and S6–S14's trip logic, and KilnFW guards 1, 2, 3, 4, 5,
7 and 9 — has never been provoked on real silicon. Separately, several guards
are commissioned off or structurally inert on this fixture: S9 and S14 depend
on current sensing whose commissioning is incomplete, S11's gate runs through
the same current path, and S6a cannot be provoked by any host fixture at all
now that `virtual_dut` and SimFW are gone.

**Why it blocks release.** The dual-processor design *is* the safety argument.
If the safety processor's guards have only ever been proven by host tests, the
argument reduces to "the logic is right", which was never the claim — the
claim is that the wiring, the sensors, the trip path and the relay actuation
all work together on this board. As `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`
puts it in its own header: the host tests prove the logic, the hardware tests
prove the wiring, and most of the failures worth catching are wiring.

**How to know it is closed.** Split the guards into three buckets and treat
each differently. This split is the actual deliverable of this item.

**Bucket A — provokable on the bench today, and must be provoked.** S1's real
ceiling trip, S2's overshoot-sustained path, S5's fault-injection cases (a
reading outside the commissioned band, a deliberate `tc_type` mismatch — only
the healthy in-band reading has ever been observed live), S6b's link-dead
tiers, S7's press-to-open path, S11's frozen-sensor trip once its gate is
reachable, and KilnFW's per-zone guards. Each needs a provocation, an observed
trip, and a confirmed relay state afterwards. Record results in
`GUARD_TEST_MATRIX.md` §3.4, which is currently entirely unexecuted.
**Size: L.**

**Bucket B — needs commissioning the bench has not completed.** S3, S4, S9,
S14 and S15 all sit behind current sensing. `CT_COMMISSIONING_PLAN.md` steps 0
and 6 are the dependency, and both need the owner. Until those land, five
guards are inert and the safety case must say so in plain language rather than
listing them as coverage. **Size: M, and owner-blocked.**

**Bucket C — cannot be closed on this fixture at all, ever.** S9's genuine
welded-contactor escalation needs a jig that injects real AC current through
the CT loop while the K4 drive line is confirmed de-energized; no such jig
exists and firmware simulation cannot substitute, because the guard latches on
an analog current-transformer signal, not on a GPIO. S8's real threshold
cannot be set until a full-power ramp is measured, and a 4 W fixture cannot
produce one. The E-stop's pole 1 is permanently unwired here by owner
decision, so the E-stop on this fixture is firmware-mediated only. And the
whole class of thermal behaviour — overshoot magnitude, ramp rates, guard
nuisance thresholds — is measured against a plant with essentially no stored
energy.

**What bucket C means for release, stated plainly.** These are not items to
schedule; they are the boundary of what the bench can sign off. A
release-credible position on them is one of three, chosen deliberately: build
the jig, defer the guard to commissioning-on-the-installed-kiln with the
safety case saying openly that it ships unproven, or accept the risk with the
owner's explicit sign-off. Picking silently is the failure mode. **Size: S to
decide, XL if the answer is "build the jig".**

**Status, 2026-09-17 (host-only pass; no board touched, no flash/reset).**
Re-derived `SAFETY_CASE.md` section 4's per-guard classification directly
against `firmware/SaftyFW/src/` rather than trusting the doc: 21 tracked
guard-level claims, **21 host-tested, 3 hardware-verified, 0 in the (b)
"provable by host test but not yet done" bucket** — every guard's pure trip
logic already has a real-function host test (spot-checked S1/S3/S6a/S9/S12/S13
against current line numbers; also confirmed S14/S15 have dedicated host
tests in `test_safety_guards.c`). No new host test was added this pass
because none of the remaining gaps are closeable that way: what's left is
exclusively hardware-verification (buckets A/B/C above), which this pass's
hard safety constraints forbid touching (no `flash_firmware`/`debug_reset`/
`ota_rollback_esp`). **Found and fixed in `SAFETY_CASE.md` while verifying**:
three stale source-line citations that no longer pointed at the claimed
check (S3 reachability `current_task.c:197`->`:243`; S6a trip condition
`safety_core.c:1073`->`safety_guards.c:459-462`, the prior line was a
different backstop entirely; S9 reachability `safety_core.c:1109`->`:1278`,
same mix-up with the unconfigured-armed backstop). None of the three changed
the classification, only the citation.

Owner/hardware boundary, restated concretely from buckets A/B/C so it can be
acted on without re-deriving anything:
- **Bucket A (do on this bench, no new equipment):** S1 ceiling, S2
  overshoot-sustained, S5 fault-injection (out-of-band reading, `tc_type`
  mismatch), S6b both link-dead tiers, S7 press-to-open, S11 frozen-sensor
  (once its gate is reachable), KilnFW per-zone guards. Each needs: provoke
  it, observe the trip, confirm relay state after, record in
  `GUARD_TEST_MATRIX.md` section 3.4.
- **Bucket B (owner must finish CT commissioning first):** S3, S4, S9, S14,
  S15 — blocked on `CT_COMMISSIONING_PLAN.md` steps 0 and 6, both owner
  actions, before any bench provocation of these five means anything.
- **Bucket C (cannot close on this fixture, ever — owner must choose a
  posture, not schedule work):** S9's welded-contactor escalation (needs an
  AC-injection jig that does not exist), S8's real rate threshold (needs a
  full-power ramp a 4 W fixture cannot produce), E-stop pole 1 (permanently
  unwired here by owner decision), and thermal-behaviour magnitudes generally
  (measured against a plant with no stored energy). For each, the owner picks
  one of: build the jig, ship with the safety case saying openly it's
  unproven until commissioned on the installed kiln, or sign off on the risk
  explicitly. This pass does not pick for them.

**Status, 2026-09-20: re-verified the (b) "provable by host test but not yet
done" bucket independently.** Re-read `SAFETY_CASE.md` §4's evidence table
row by row against its own **not done** / hardware-only markers rather than
trusting the 2026-09-17 rollup at face value: every row not labeled
hardware-verified is either **host-tested** (a real host test already pins
the pure trip logic) or explicitly **not done, hardware-only** (S1/S3/S6b/
S9/S11 hardware trips, S6a's "cannot be provoked by any current host
fixture, permanently," S8's real threshold, both link-loss/OTA-interlock
real-bench rows, and the three KilnFW thermal_guard rows marked
"host-tested, not hardware-verified"). None of those hardware-only rows is
closeable by a host test by construction -- they are precisely the claims
whose entire content is "does this work on real wiring/sensors/actuation,"
which a host test cannot exercise. Confirms the 2026-09-17 finding: **the
(b) bucket is empty.** No new host test was added this pass, because there
is nothing left in it to add one for -- what remains is exclusively buckets
A/B/C above (hardware-verification-only), consistent with this item's own
instruction to say so per-condition rather than skip silently.

---

## 5. BLOCKER — failure injection and recovery, exercised rather than argued

**The gap.** The recovery paths are designed, largely implemented, and in
several cases host-tested. Almost none has been made to happen on real
hardware. Specifically:

- **Power loss mid-write.** The RP2040 `config_store` A/B sector scheme is
  implemented and flashed, and its in-RAM cache race is fixed. ~~Its
  `next_write_slot` torn-slot reprogramming defect is flagged and untouched~~
  — **the defect is fixed, 2026-09-14** (`config_store_next_write_slot()` now
  verifies the target slot is actually erased before programming, and
  read-back-verifies after every program; full detail
  `docs/audits/rp2040_config_store_write_atomicity_2026-09-14.md`), host-tested
  (`test_config_store_flash.c`, torn-slot-reuse cases) and **partially
  bench-verified** (`88bb4333`, 2026-09-14: a real field round-tripped
  correctly across two live Pico reboots, all safety-relevant config read
  back unchanged). What remains genuinely open, by deliberate safety choice
  rather than oversight: that bench pass explicitly did not tear a real flash
  program mid-write (judged unsafe/irreversible on the only bench Pico), so
  the mismatch-detected and slot-skip-and-switch branches are proven only by
  the host-test power-cut-injection harness, never on real hardware. **The
  plan's original ask — cut power during a write, repeatedly, on real
  hardware, confirming intact read-back every time — is still open.** Verify
  detail: `docs/audits/release_hardening_plan_verify_1_2_5_7_8_2026-09-16.md`.
- **A Pico reboot mid-firing.** The reset-one-side bug class has already
  produced four confirmed silent instances in this codebase, one of them
  exactly this scenario (`trip_seq` restarting on the Pico while the ESP's
  dedup cursor did not). That one is fixed. The general exercise — reboot the
  Pico during a live firing and confirm the ESP blocks heat, recovers the
  link, and re-establishes dedup state correctly — has not been run.
- **A dead link.** The 1.5 s staleness ceiling is bench-verified. The 30 s
  firing-abort is host-test-pinned only; `bench_firing_abort_stopwatch.py`
  exists and is written specifically to close it, and the only missing
  ingredient is a running firing. **This one is cheap and should be taken the
  next time a firing runs.** Size: S.
- **A thermocouple fault.** Guard 6 is hardware-verified on the KilnFW side —
  genuinely, with the trip firing after exactly three bad reads and the relay
  staying off. The safety processor's equivalent injection cases are not.
- **A welded contactor.** Bucket C above.
- **Corrupted or downgraded config.** Section 7.

**Why it blocks release.** Unattended operation is defined by what happens
when something goes wrong and nobody intervenes. Every item above is a
scenario a real installation will eventually produce. An argued recovery path
is a design intent; a hardware-exercised one is a property.

**How to know it is closed.** One scripted injection session per scenario,
each with a defined expected end state (relays open, fault surfaced, firing
aborted or resumed as designed), each recorded. Build them as scripts in
`tools/PcTools/scripts/` next to the two bench scripts that already exist, so
they are repeatable against the next build rather than being one-off session
logs. **Size: L overall; the link-abort stopwatch alone is S.**

**Status, 2026-09-17 (host-only pass, no board access).** Failure classes
named by this section plus one this pass found undocumented (a quarantined/
downgraded config never actually verified against its real producer): power
loss mid-write (Pico A/B sectors, argued+host-tested, partially bench-verified
per the bullet above), a Pico reboot mid-firing (argued, ESP-side dedup-reset
logic host-tested via real state transitions in `test_safety_link.c`/
`test_safety_link_compile.c`, never run against a real rebooting Pico), a dead
link (1.5s bench-verified, 30s abort host-test-pinned only, needs a running
firing), a thermocouple fault (Guard 6 hardware-verified on KilnFW, Pico-side
injection not), a welded contactor (bucket C, no jig), and corrupted/
downgraded config (see below). Rough count: 7 named classes, 1 exercised on
real hardware (Guard 6), 1 partially exercised (config-store power loss), 5
argued/host-tested only.

Moved argued -> exercised-by-real-fault-injection this pass, host-only (no
hardware): the config-quarantine signal chain. Every existing test around
`zones_config_store.c`'s NEWER/UNREADABLE decode outcomes drove the real
decode path with real staged bytes, but none of them ever read back the real
`zones_config_get_load_fault()` accessor that `profile_executor_run()`,
`dashboard_http.c` and `ui_page_home_refresh.c` actually gate on — the
consumer side was only ever exercised against a hand-built fault struct
(`test_profile_executor_prestart.c`'s fake), never the real producer. Added
`test_nvs_load_from_newer_than_firmware_latches_real_load_fault()` and
`test_nvs_load_from_bad_crc_latches_real_load_fault_as_unreadable()` to
`firmware/KilnFW/App/test/test_zones_http.c` (drives real bytes through the
real `nvs_load_from()`/`zones_config_json_decode_blob()`, reads back the real
`zones_config_get_load_fault()`). Both negative-tested: each latch call was
independently removed/altered in `zones_config_store.c` (that file's current blob,
blob:firmware/KilnFW/App/drivers/persist/zones_config_store.c`1b485e17e0659ff95a03aab0f856b94ef3a8679f`),
confirmed to turn the
`zones_http` host-test executable red, restored by hand with an empty
`git diff` and a matching `git hash-object`, then the build directory was
deleted and a full rebuild confirmed green again. This does not reach
`profile_executor_run()` itself; that gap is closed (2026-10-04) by
`firmware/KilnFW/App/test/test_profile_executor_store_link.c`, which saves through the real
`profiles_http.c` store and runs the executor from it (slot 3, and slot 99 with 12 segments).

Owner/hardware boundary (cannot be closed without the board): cut power
mid-flash-write on the Pico repeatedly with intact read-back each time
(bench-unsafe per the bullet above, tear only via host fault injection);
reboot the Pico during a live firing and confirm the ESP blocks heat/
recovers the link/re-establishes dedup; the 30s dead-link firing-abort via
`bench_firing_abort_stopwatch.py` (needs a running firing); thermocouple
fault injection on the safety processor's own MAX31856; a welded contactor
(bucket C, no jig exists, firmware simulation cannot substitute — analog
CT signal, not a GPIO); S6b's persistent OpenOCD halt session.

---

## 6. BLOCKER — OTA, rollback, recovery mode and the first-boot path

**The gap.** Field-update mechanics are the one area where a defect is
unrecoverable without physical access, and this system has three known, sharp
hazards in it:

1. **`flash_firmware()` writes the factory partition and never touches
   `otadata`.** After any OTA, the bootloader keeps booting `ota_0`/`ota_1`
   while every later flash reports success and the board runs old code. The
   automatic post-flash verification now catches this loudly, which is a real
   mitigation — but the hazard itself is structural and an operator doing a
   field update needs to not be able to hit it.
2. ~~**Rolling back past a `zones_cfg` schema bump silently runs on
   firmware-default PID gains.**~~ — **CLOSED, `bfa60679`, 2026-09-16** (see
   the matching bullet below). The older firmware refuses the
   newer-than-it-knows blob, flash is untouched so reflashing restores
   everything, and there is *no separate warning* — a firing started between
   the rollback and the reflash runs on defaults. On a 4 W fixture that is a
   bad graph. On a real kiln it is a ruined load or worse.
3. **Recovery mode has bricked this board into a permanent loop three times.**
   Both 2026-09-08 findings are fixed — `boot_guard_mark_healthy()` now
   verifies its own NVS write rather than trusting a return code, and
   `boot_guard_reset_counter()` exists for a tool that knows it just
   deliberately flashed. That second function is now wired into
   `flash_firmware()`'s verify step (`b09294fb`, 2026-09-09), and, per the
   **owner decision 2026-09-19, is DEFAULT ON**: `flash_firmware()` calls
   the new `POST /api/ota/esp/boot_guard_reset` route ONLY after post-flash
   verification confirms full, unambiguous success, and reports the
   counter's before/after values and whether the clear actually verified —
   see CLAUDE.md's `boot_guard_reset_counter()` paragraph for the full
   wiring. A caller with no explicit `ap_password` falls back to the
   `KILNCTL_AP_PASSWORD` environment variable; `reset_boot_guard=False`
   opts out unconditionally. This is now closed by default, not merely for
   a caller that opts in.

The Pico half of field updates has additionally never completed a transfer:
the 2026-09-06 attempt was refused by a genuine Pico-side interlock before any
bytes crossed the wire.

**Why it blocks release.** An OTA that leaves a kiln in an unknown state is
hazard H8 in `docs/SAFETY_CASE.md`, and every mitigation for it is currently
classed argued or host-tested. A field unit cannot be recovered by walking
over to it with a JTAG probe.

**How to know it is closed.**

- ~~Wire `boot_guard_reset_counter()` into `flash_firmware()`'s verify step and
  negative-test it~~ — **done, `b09294fb`** (see item 3 above and CLAUDE.md).
  Host-tested (`tools/PcTools/tests/test_flash_firmware_verify.py`'s
  `BootGuardResetWiringTest`, and `test_boot_guard.c`/`test_ota_http.c` on the
  firmware side) covering the central negative case (a hard verification
  failure must never clear the counter) and the lying-write/unreachable-
  endpoint paths. **Owner decision 2026-09-19 closed the residual**: the call
  is now default on rather than opt-in (`reset_boot_guard=True` by default,
  falling back to the `KILNCTL_AP_PASSWORD` environment variable when no
  explicit `ap_password` is given; `reset_boot_guard=False` opts out).
- ~~Make the schema-downgrade hazard impossible to hit silently: on boot, if
  the persisted config version is newer than this firmware understands,
  refuse to start a firing and say so on every surface, rather than running
  on defaults.~~ — **CLOSED, `bfa60679`, 2026-09-16 ("Refuse to start a
  firing on a quarantined zones config, surface it everywhere").** A new
  `zones_cfg_load_fault_t` (`zones_config_accessors.h`) is latched in
  `zones_config_store.c` whenever the on-disk blob is newer than
  `ZONES_CFG_VERSION` or older than the single-step migration chain can
  carry forward, and `profile_executor_run()` now refuses to start with a
  fault-kind-specific, version-naming message instead of silently adopting
  firmware-default gains — no override. Surfaced on every operator-facing
  path: the web dashboard status payload and `main_page.html` banner, and
  the LCD home page's trip-strip widget (`ui_page_home_refresh.c`), matching
  CLAUDE.md's no-scrolling/existing-layout constraint. Negative-tested in
  `test_profile_executor_prestart.c`
  (`test_run_refuses_with_named_reason_on_config_quarantine`): 5 genuine
  `TEST_CHECK` failures against the sabotaged gate, 0 at the fix, via a full
  rebuild of all 47 host-test executables in both states (per that commit's
  message).
- Complete a real Pico update over the wire, end to end, including whatever
  bootloader/metadata gap `firmware/SaftyFW/docs/BOOTLOADER.md` and the update
  protocol's completion checklist imply for a finished one. **Size: L.**
- Write and rehearse the **first-boot-on-a-real-kiln checklist** — section 8.

**Status, 2026-09-17 (source-level pass, no hardware touched).**

- **Item 1, `otadata` legibility — verified already closed, no code change
  needed.** Traced (not assumed) `_verify_flash_landed()`
  (`tools/PcTools/src/kilnctrl/mcp_server_flash.py:353-490`): the running-
  partition mismatch branch (line 450) raises with the actual `running`
  value, names `app_partition_name`, explains the `otadata` gap is a KNOWN
  GAP (`docs/OTA_SINGLE_SLOT_PLAN.md`), states `ota_rollback_esp()` does not
  fix it, and says what a from-scratch board needs. This exact raise path
  is exercised by `test_flash_firmware_verify.py` (asserts `"KNOWN GAP"` in
  the message). The docstring paragraph (lines 777-786) sits on
  `flash_firmware()` itself, so it surfaces through `kiln_help()`/
  `kiln_find()` at the point someone would actually invoke the tool, not
  only in this plan. No further legibility work identified.
- **Item 2, rollback-past-schema-bump — already closed, `bfa60679`,
  2026-09-16** (per this section's own strikethrough above; reconfirmed
  this pass by reading `zones_config_store.c`'s
  `zones_cfg_load_fault_t` latch and `profile_executor_run()`'s prestart
  refusal — a rollback to firmware that cannot parse the persisted schema
  now refuses to start a firing rather than running on default gains).
- **Item 3, `boot_guard_reset_counter()` default wiring — CLOSED, owner
  decision 2026-09-19.** The credential-handling/architecture question this
  status entry raised (where the password comes from by default, and
  whether a wrong guess is worse than "no attempt, no warning") was decided
  by the owner: `reset_boot_guard=True` is now the default
  (`mcp_server_flash.py:843`), falling back to the `KILNCTL_AP_PASSWORD`
  environment variable when no explicit `ap_password` is given
  (`_resolve_boot_guard_password()`), with `reset_boot_guard=False` as the
  opt-out. See CLAUDE.md's `boot_guard_reset_counter()` paragraph.
- **Mechanical coverage added this pass (host-verifiable, no board):**
  - `tools/PcTools/tests/test_flash_board_pinning.py`'s new
    `FlashFirmwareSizePreflightTest`: the pre-flight
    `app_bin_size > app_target.size` refusal in `flash_firmware()`
    (`mcp_server_flash.py:819-826`) existed but was never exercised by any
    test — every other test in that file deliberately fakes `getsize()`
    under the partition size specifically to make this check a no-op.
    Negative-tested by disabling the guard (`if False and ...`), confirming
    the new test goes red (result was `"flashed and verified OK"`, OpenOCD
    WAS called), restoring by hand with Edit, and confirming an empty
    `git diff` and a matching `git hash-object` before re-running.
  - `firmware/KilnFW/App/test/check_boot_guard_reset_reachability.ps1`
    (new): a static guard that `boot_guard_reset_counter()` is called from
    nowhere except `ota_http_recovery.c` (the authenticated route) and test
    files — the exact property whose absence the 2026-09-08 audit warned
    about ("wiring it into every `boot_guard_init()` call instead defeats
    the counter entirely"). Negative-tested against a throwaway copied tree
    (never the real one) with a fabricated `boot_guard_reset_counter()`
    call added to a fake `main_boot_early.c`; the real tree was never
    modified so no restore was needed.
- **Still requires the bench board and the owner, and cannot be closed from
  a source-level pass:** the Pico half of field updates (a real end-to-end
  transfer has never completed a single byte across the wire — Size L);
  writing/validating a correct `otadata` blob (deliberately not attempted —
  a wrong one risks a worse, silently-bricked boot than the current loud
  refusal, and it cannot be validated without the board); and the
  first-boot-on-a-real-kiln checklist rehearsal (section 8).

**Already covered, name the evidence:** post-flash verification is genuinely
solid. `flash_firmware()` polls the board's own API for the running partition
and compares the reported build timestamp against the binary's embedded
`esp_app_desc_t`, fails loudly on disagreement, tries an ordered candidate host
list rather than assuming the AP-fallback address, and treats "was reachable
before, unreachable after" as a hard failure. It also records git provenance
and refuses a dirty tree that touches config-schema, migration or safety code.
That is a better flash path than most projects of this size have, and it does
not need work.

---

## 7. Config, migration and the two-processor agreement — mostly owned
## elsewhere, one release-specific addition

**Already owned:** `docs/KILN_PROFILES_PLAN.md` is the in-progress plan and it
already covers the central defect (the stored blob is ESP-only; the Pico's
sixty-eight commissioning parameters do not travel with a kiln config), the
standing cross-processor agreement invariant with alarm-and-disable-heaters on
divergence, upload as untrusted input, the missing-field rule, and atomic
swap across two processors. Do not re-plan any of it here.

**What release adds on top.** Two things that plan does not have to solve but
release cannot ship without:

- **A migration test that runs every historical schema version forward —
  CLOSED, 2026-09-16, narrower than originally scoped, deliberately.**
  `convert_versioned_blob_to_current()` (`zones_config_migrate.c`) already
  performs a direct v1-through-v25-to-current conversion for every historical
  version — that part of the ask was already satisfied at the conversion
  layer before this pass. The real gap, per
  `docs/audits/release_hardening_plan_verify_1_2_5_7_8_2026-09-16.md`, was
  narrower: CRC-integrity verification in
  `zones_config_json_decode_blob()` only covered the current version and
  `ZONES_CFG_VERSION - 1` (the 2026-09-10 fix for Opus finding 10, deliberately
  scoped to N-1 "not every historical version"). This pass widens that gate to
  `ZONES_CFG_VERSION - 2` (v24) — the version a board goes through on its
  *second-most-recent* update, mechanically identical to the existing N-1
  check, with a matching negative test
  (`test_decode_zones_blob_refuses_a_v24_blob_with_a_corrupted_crc`,
  `test_zones_http.c`).
  **Deliberately left unimplemented:** widening the same CRC gate to v7-v23.
  These are structurally reachable (the switch/converter already handles
  them) but judged practically unreachable for the one board this project
  ships against, given this board's reflash cadence — by the time a second
  bump has happened, the board has already round-tripped through the N-1 gate
  once and the persisted blob is already current-version. v1-v6 are not just
  impractical but impossible to add: those structs predate the `crc32` field
  entirely (added later; enforced today only by `_Static_assert` on later
  structs), so there is no CRC to check. `ZONES_CFG_VERSION` was not bumped by
  this pass. **Size: S (down from M) — closed to the extent argued reachable;
  v7-v23 consciously declined, not missed.**
  **Update, 2026-09-17:** the CRC-narrowing above closed one specific gap, but
  `docs/audits/release_hardening_plan_verify_1_2_5_7_8_2026-09-16.md`
  separately confirmed the corpus test this bullet's title actually names —
  "a v1-through-current corpus of real persisted blobs, run each forward to
  current, assert the result is sane" — was still genuinely missing: every
  existing migration test proved one hop, or one specific historical starting
  point, never the full v1..v25 sweep. That gap is now closed by
  `test_zones_config_migration_corpus_every_historical_version_forward()`
  (`firmware/KilnFW/App/test/test_zones_http.c`): it constructs one real,
  minimally-populated `zones_cfg_vN_t` for every historical version 1..25,
  decodes each through the actual production
  `zones_config_json_decode_blob()`, and asserts the decode succeeds, the
  result is stamped with the current version, and the fields common to every
  historical layout survive unchanged. A `_Static_assert` on
  `ZONES_CFG_VERSION` trips the day a new version is added without extending
  this corpus. **Blocker fully closed.**
- **Import of a deliberately hostile config.** Truncated, wrong CRC, valid CRC
  with out-of-range values, a version number from the future, a file that is
  valid for a *different* kiln. The kiln-profiles plan covers untrusted upload
  for its own surface; the release gate is that no such input can produce a
  bootable state that will command heat. **Substantially more coverage
  already exists than this bullet implied**: `test_backup_import.c` already
  rejects a malformed/truncated body, wrong `kind`, a too-new version,
  out-of-range model values and overlong names; separately, at the NVS-blob
  level, bad-CRC and refused-newer-than-firmware blobs are covered and
  preserved un-overwritten (`test_nvs_load_from_bad_crc_is_rejected`,
  `test_zones_http_start_refused_newer_blob_not_overwritten`, among others).
  What is still missing is a single test stating the release-gate property
  directly rather than inferring it from several unit tests. **Size: S
  (down from M).** Verify detail:
  `docs/audits/release_hardening_plan_verify_1_2_5_7_8_2026-09-16.md`.
  **Update 2026-09-17: closed.** `test_backup_import.c` now has
  `test_no_hostile_backup_input_produces_a_bootable_heat_commanding_state()`,
  driving 8 distinct hostile bodies (truncated, wrong `kind`, future version,
  out-of-range model values, overlong name, transposed coupling index,
  self-referencing `settings_source`, bad-CRC-shaped field) through the real
  `backup_import_apply()` and asserting refusal with zero writes for every
  one, stating the release-gate property directly in one place
  (`f7233461b4f8069e7b6bdf508cd6535a2655bc13`).

**2026-09-17 verification pass.** Both release-specific bullets above were
re-checked mechanically against `origin/main` rather than assumed from this
doc's own prose: `bfa60679` and `f7233461b4f8069e7b6bdf508cd6535a2655bc13`
are both ancestors of `origin/main`, and `ZONES_CFG_VERSION` is still 26
(`firmware/KilnFW/App/drivers/persist/zones_config_json.h:63`) — neither
bullet needed further work. Section 7 is closed; nothing outstanding here.

Also traced mechanically (not from docs) for this pass, since the task
description raised it as the highest-value risk: whether the Pico's
`abs_max_temp_c` can ever end up looser than the ESP's, stale, or absent
while armed.
- `safety_ceiling_sync.c`'s `enforce_ceiling_divergence()` (called every
  `safety_poll_task` tick via `safety_ceiling_sync_reconcile_on_link_up()`,
  `safety_ceiling_sync.c:627-675`) actively forces all relays off and halts
  any run on every tick the ceilings disagree (`safety_ceiling_sync.c:501-536`)
  — not a one-shot check, and it runs before the raise-retry backoff so it
  cannot go quiet during the most common divergent case (Pico ARMED,
  refusing a raise).
- The Pico only ever raises its ceiling after a confirmed stage+commit+
  read-back write (`pico_ceiling_writer()` -> `safety_cfg_write_set_and_
  confirm_f32()`, `safety_ceiling_sync.c:218-224`); `check_volatile_ceiling_
  write_callers.ps1` mechanically forbids the RAM-only variant from being
  substituted anywhere except the one allowlisted two-processor-transaction
  caller (verified passing 2026-09-17: "4 occurrence(s) total").
  `safety_ceiling_sync_guard_raise()`/`_apply_lower()` (`safety_ceiling_sync.c:
  247-283`) are also the only writers, both routed through the same
  confirmed primitive.
- On the Pico side, an unset/never-commissioned `abs_max_temp_c` reads back
  as the sentinel `0.0f` (`config_store.c:820`), which
  `commissioning_gate_is_commissioned()` (`commissioning_gate.h:85-88`)
  treats as NOT commissioned, and `link_task.c` refuses to grant
  `request_enable` for an uncommissioned board rather than arming with no
  ceiling — the fail direction on a fresh/rebooted/never-heard-from-ESP
  Pico is refuse-to-enable, never armed-with-a-loose-or-absent-ceiling.
- No path was found where the Pico's stored ceiling is looser than the
  ESP's committed one: every write path is raise-with-confirm or
  lower-with-confirm through the same guarded primitive, and any observed
  disagreement (including a stale post-reboot value) forces heat off on the
  very next tick rather than being silently tolerated.

Also confirmed already mechanically covered, both re-run green this pass:
`tools/check_nvs_key_length.ps1` (458 files scanned, 0 violations — covers
every `NVS_KEY`/`NVS_NAMESPACE`/`NVS_PARTITION` `#define` literal in
`firmware/KilnFW/App`, added `ce129d3b`; the historical 16-char
`zone_normals_cfg` trap was already renamed to the 13-char `zone_norm_cfg`
on 2026-09-06; SaftyFW carries no NVS keys at all, it is not ESP-IDF) and
`tools/check_kiln_auth_config_isolation.ps1` (237 files scanned under
`firmware/KilnFW/App/drivers`, 0 references to `kiln_auth`/`web_auth`/
`lcd_auth`/`auth_policy` outside the dedicated auth module and its 2
allowlisted files) — the namespace literal (`WEB_AUTH_NAMESPACE
"kiln_auth"`, `web_auth_store.c:19`) is confined exactly as CLAUDE.md
requires. Both checks already existed and already ran green; nothing new
needed adding or negative-testing this pass.

---

## 8. ~~BLOCKER~~ DONE — what must be verified on the installed kiln, and the
## first-boot checklist

**Closed.** `docs/FIRST_FIRING_CHECKLIST.md` now exists: an ordered,
owner-followable sequence covering pre-power wiring/continuity, E-stop
function (both poles, including the durable `estop_verification` attestation
for pole 1, which no software check can see), safety-link-up and trip
clearing, per-zone thermocouple identity and type on both processors
(including the zone-2-is-bottom/zone-0-is-top swap check and the
`s_tc_type_verified` distinction from a bad reading), Pico/ESP
`abs_max_temp_c` equality, a low-temperature dry run proving the contactor
before any real load, CT calibration (with the import-forces-uncalibrated
caveat), and an explicit per-guard accounting of what the bench already
proved versus what still needs the installed kiln or a supervised firing.
It ends by handing off to the low-temperature/full-temperature/unattended
firing sequence below rather than claiming to close that sequence itself.
The remaining bullets below (S8's real threshold, autotune/coupling matrix,
thermal overshoot/cool-down, relay wiring under full load) still require an
actual firing on the installed kiln to measure — the checklist gets the
kiln to the point those are safely attemptable, it does not substitute for
running them.

**The gap (as originally written).** There is no document that says, in order, what an operator does
between "the controller is bolted to a kiln that has never run under it" and
"it is safe to leave this firing unattended". `docs/SETUP_WIZARD.md` covers
configuration thoroughly and is the right backbone, but configuration is not
commissioning: the wizard gets the numbers in, and this checklist proves the
kiln behaves the way those numbers claim.

**Why it blocks release.** Several things are *only* knowable on the installed
kiln, and every one of them is a safety input:

- **`abs_max_temp_c` must be raised Pico-first, then ESP**, and the Pico's
  ceiling must never end up tighter than the ESP's — that ordering is a
  standing rule, and the bench's 80 °C value is meaningless on a real kiln.
- **S8's rate-of-rise threshold** cannot be set until a full-power ramp has
  been measured on the actual kiln. The bench's hand-set 20 °C/min is a bench
  number, tighter than the documented rule, and would nuisance-trip a fast
  kiln.
- **Current-sensing commissioning under real load**, which is what unlocks
  S3/S4/S9/S14/S15 from bucket B.
- **Autotune per zone and the coupling matrix**, which are multi-hour, need a
  rested kiln, and must never ship values tuned against a different one. The
  standing requirement that nothing ship guessed for, or tuned to, a kiln
  other than the installed one applies to the fuzzy layer's bands too.
- **Thermal overshoot and cool-down behaviour** — a real kiln keeps climbing
  after the elements open, and every guard threshold tuned against a 4 W
  fixture with no stored energy is untested against that.
- **Relay and contactor wiring actually proven**, including that dropping K4
  actually opens the contactor. Today that claim is schematic-derived, not
  bench-proven.

**How to know it is closed.** Write `docs/FIRST_FIRING_CHECKLIST.md` (or fold
it into the setup wizard as a final gated stage, which is better because the
wizard already persists progress in NVS where a filesystem problem cannot lose
it): an ordered, signed-off sequence ending in a low-temperature attended
firing, then a full-temperature attended firing, then the first unattended
one. Each step names its refusal condition. The deliverable is closed when an
operator who is not the author can follow it. **Size: M to write, L including
the rehearsal.**

---

## 9. ~~Desirable, not blocking~~ DONE — a shared-state review pass

Swept 2026-09-17: `docs/audits/shared_state_review_2026-09-17.md`. All four
originally-confirmed instances are fixed-and-tested or moot (SimFW/kilnsim
and `fault_sched.c` no longer exist on `origin/main`); the two web-auth
instances found later are already fixed (`1179e2d3`). Seven further
cross-processor/seqlock/cfg-vs-NVS candidates surfaced by the sweep's own
enumeration all resolved to correct one-sided designs. No new pair found, so
no code change and no narrow check — the standing rejection of a general
check for this class stands.

## 10. Desirable, not blocking — the `cfg` partition is inert and should
## either be finished or explicitly parked

**Superseded 2026-09-21:** the bench board (`8ab3b81a`) now has `cfg`
mounted and populated with 7 files, confirmed via `GET /api/cfgfs` on
hardware; NVS remains authoritative. The section below describes the
prior, now-stale state.

Zones config, profiles and several preferences dual-write to the `cfg`
LittleFS partition, but that partition is unformatted on the bench board and
not mounted at boot, so the whole path is inert today. NVS remains
authoritative and unconditional, so this is not a hazard. It is, however, a
significant amount of live code that has never run. Before release, either
finish it (format, mount, exercise the tie-break and fallback paths) or gate it
off behind something that makes its inertness explicit. Shipping a dormant
write path that will wake up on the first field unit whose partition happens to
be formatted is the bad third option. **Size: M. Desirable.**

**Decision, 2026-09-17: PARKED, not finished.** Verified against the code and
the live bench board rather than assumed:

- The mount call (`cfg_fs_mount_device()`, wired into `main_boot_early.c`) and
  the auto-format-or-ask gate both already exist in source (landed
  2026-09-07, `docs/CONFIG_FILESYSTEM.md`). This is further along than
  "nothing written" — the code is real and host-tested.
- It has never run on the actual bench board. `get_fw_version()` against the
  live board (2026-09-17) reports commit `3b0c82e`, built 2026-09-05, 1057
  commits behind HEAD — i.e. the board's running firmware predates the mount
  wiring entirely. `GET /api/cfgfs` on that board answers `no such endpoint`.
  Every claim in `docs/CONFIG_FILESYSTEM.md` that the partition "is live" or
  "mounts cleanly" describes what the code does, not anything observed on
  hardware.
- "Finish it" in the sense this section originally meant — exercise the
  tie-break and fallback paths for real — requires reflashing the bench board
  and then meeting `docs/FILESYSTEM_PLAN.md`'s own closing criterion (20
  consecutive clean boots, one complete file-backed firing, one verified
  backup/restore round trip). That is bench time and an owner-visible,
  by-hand step (`docs/CONFIG_FILESYSTEM.md`'s "Dual-write window" section is
  explicit that nothing may act on `window_may_close` automatically), not
  something a coding pass can close. **Rejected** as out of scope for this
  pass specifically because it requires flashing the board, which this pass
  was not authorized to do.
- What this pass instead confirmed and hardened is the property that makes
  parking safe: NVS is authoritative and unconditional. Every one of the 11
  production `*_cfg_fs_save()`/`*_cfg_fs_resolve()` call sites (across
  `zones_config_store.c`, `kiln_cfg_store.c`, `profiles_http.c`,
  `relay_cycles.c`, `unit_pref.c`, `display_power_cfg.c`,
  `ramp_assist_cfg.c`, `time_sync.c`, `adaptive_tune.c`,
  `profile_executor_firing_stats.c`) treats the file write as best-effort:
  logged on failure, never gating the NVS write that follows. That is a
  claim worth a mechanical guard rather than a one-time read, since it is
  exactly the kind of cross-module contract this codebase has broken
  silently before (see CLAUDE.md's "reset one side of a pair" bug class).
  `tools/check_cfgfs_never_gates_nvs.py` (wired via
  `tools/check_cfgfs_never_gates_nvs.ps1`) fails the build if any
  `*_cfg_fs_save()` call site's captured return value ever gates a `return`
  in the same function — negative-tested by inserting exactly that `return`
  into `unit_pref.c` and confirming the check catches it, then restoring by
  hand and confirming an unchanged `git hash-object`.
- **To un-park:** reflash the bench board with current HEAD (or a clean
  worktree build), confirm `GET /api/cfgfs` reports `mounted: true`, and run
  the three-condition closing criterion above before removing any NVS
  writer. None of that has started. **Size: M, and now dependent on bench
  time. Desirable, not blocking.**

## 11. Desirable, not blocking — prove a fresh clone builds

**Status, 2026-09-17: the sdkconfig-seeding gap this item was written about is
already closed** (`check_01_kilnfw_pushed_build.ps1` stopped seeding a
board-tuned `sdkconfig` on 2026-09-16 — `sdkconfig.defaults` pins
`CONFIG_IDF_TARGET="esp32s3"` in committed content, so a from-scratch build
targets esp32s3 correctly with no seed). Verified empirically today in an
independent from-scratch worktree, distinct from that check's own persistent
one: `git worktree add` + `git submodule update --init --recursive` (all
three submodules resolve cleanly) + `tools/setup.ps1` (which now also runs
`uv sync --project tools/PcTools` itself) + `tools\run_all_checks.ps1`, no
other manual step, reached **106 passed, 1 skipped, 0 failed** — the skip was
`check_ui_responsive_sweep.ps1`'s known transient CDP/fetch flake, unrelated
to clone freshness. No generated-and-gitignored file was found without a
template or a self-provisioning step: `check_00_saftyfw_target_build.ps1`
configures SaftyFW's `build.ninja` on first use, and
`check_mykicad_golden_suite_runs.ps1` creates its own `.venv`. `docs/SETUP.md`'s
clean-worktree recipe (which recommended a now-unnecessary manual
`idf.py set-target` and a now-redundant manual `uv sync`) was corrected in the
same change.

**What remains pending:** nothing found. `check_01_kilnfw_pushed_build.ps1`
and `check_01_saftyfw_pushed_build.ps1` already run this proof, against a real
fetched `origin/main`, on every `run_all_checks.ps1` invocation — a dedicated
from-scratch-clone check (fresh submodule init and venv creation on every run,
not just the first) was considered and rejected as the wrong trade: it would
add real minutes to every run to re-prove something the self-provisioning
steps above already establish once per worktree and the two `check_01`
scripts keep proving on every commit. No further work is planned here unless
that judgment changes. **Size: done. Desirable.**

## 12. Desirable, not blocking — adaptive tuning should not ship enabled and
## unproven

The fuzzy layer has no demonstrated benefit under matched conditions on this
bench — that scope qualifier is load-bearing and travels with the finding.
Fixed-gain fuzzy at strength 50 measured as net harmful; the adaptive variant,
which is the one the owner has repeatedly said is intended, has not been
evaluated at all, and `docs/ADAPTIVE_FUZZY_EVALUATION_PLAN.md` specifies the
run that would decide it. `fuzzy_strength_pct` is 0.0 on the live board, so it
is inert today. The release position writes itself: **it ships at zero strength
unless and until the adaptive evaluation shows benefit on the installed kiln**,
and the band derivation from each zone's own autotune model is what makes that
defensible rather than fixture-trained. No work, one recorded decision.
**Size: S. Desirable.**

**Status, 2026-09-17: verified closed, no code change needed.** Checked the
actual shipping default rather than assuming it, per this section's own
concern about sentinel traps (`project_zone_band_zero_is_default_sentinel`)
and field-upgrade regressions:

- **Fresh board:** `zones_config_store.c:225`'s `memset(out_cfg, 0,
  sizeof(*out_cfg))` zero-fills the whole config, and `zone_cfg_t`'s own field
  comment (`zones_config_json.h:391-392`) documents `fuzzy_strength_pct` as
  "0 = no fuzzy adjustment, i.e. behaves exactly like classic PID" — 0 lands
  on the OFF side here, not the sentinel-trap shape.
- **Upgraded board (pre-v10 record, no `fuzzy_strength_pct` field at all):**
  `zones_config_convert.c:300`'s `convert_zone_v9()` also starts with
  `memset(d, 0, sizeof(*d))` and deliberately does not touch
  `fuzzy_strength_pct` (its own comment at line 331), so an absent record
  upconverts to 0/OFF, not enabled — the same class of bug the OTA-status and
  web-auth-enable incidents were, but not present here.
- **The separate adaptive-tune opt-in flag** (`adaptive_tune.h:60`,
  `zone_cfg_t::adaptive_tune_enabled`) is likewise struct-zero-default false;
  `adaptive_tune.c:1331`'s own comment calls this out explicitly: "its
  struct-zero default: enabled = false. DEFAULT OFF, as required".
- **Disable path / state hygiene:** `adaptive_tune_set_enabled(zone, false)`
  (`adaptive_tune.c:842`) only clears the live/persisted opt-in bit; it
  deliberately leaves any already-committed learned gains in place, because
  those are written through the same `zones_config_set_pid()`/`set_model()`
  path autotune's Accept uses (`adaptive_tune.c:20-27`) — "a reader cannot
  tell a learned gain from a hand-tuned or autotuned one, which is the
  point." That is not a stranded-state bug: the dedicated restore path is
  `adaptive_tune_revert()` (`adaptive_tune.c:1156`, "one-click revert",
  `adaptive_tune.h:19`), a separate, explicit action from the enable toggle,
  by design. The one real reset-one-side instance in this module
  (`ki_baseline` never being cleared on re-autotune) was already fixed
  before this pass, at `adaptive_tune_clear_ki_baseline()`
  (`adaptive_tune.c:1014`), called from `autotune_engine.c`'s accept path.
- **Mechanical guard:** `test_zones_http.c:3119` (`nvs_load_from`'s v9→v10
  migration test) and `test_adaptive_tune.c:462-468` ("opt-in default off")
  already assert these defaults with real, non-vacuous checks. Negative-tested
  this pass: temporarily forced `convert_zone_v9()` to write
  `d->fuzzy_strength_pct = 50.0f`, rebuilt `firmware/KilnFW/App/test/
  build_host_tests.ps1`'s `zones_http` executable, and confirmed it failed
  loudly (`test_zones_http.c:3119: zones[0].fuzzy_strength_pct defaults to 0
  ... got 50.0000, want 0.0000`) before the sabotage was hand-reverted
  (`git diff` empty, `git hash-object` matched pre-sabotage). No new check
  was added — the existing assertions already catch this condition; the
  suite total is unchanged at 107 passed / 0 skipped / 0 failed.

Nothing here touches control-loop math, gains, or the schedule table; no
`ZONES_CFG_VERSION` bump. **Size: done. Desirable.**

---

## 13. Order of work

The dependency structure is simple enough to state in a paragraph. Coredump
readback (item 1.1) comes first because it changes the information content of
every subsequent hardware run. The gate audit (item 3) comes second, because
everything after it is measured by gates whose trustworthiness is currently
assumed. The soak harness with a real verdict (item 2) is third and then runs
continuously alongside everything else. Guard provocation (item 4, buckets A
and B) and failure injection (item 5) are the bulk of the hardware work and can
interleave. OTA and recovery (item 6) is independent and can be done by a
different session in parallel. Config migration (item 7) depends on the
kiln-profiles plan landing. The first-firing checklist (item 8) is written last
because it is the summary of everything the other items established, and it is
the document the owner actually uses.

The three bucket-C decisions in item 4 should be put to the owner early rather
than late, because one possible answer ("build the current-injection jig") is
an XL that would otherwise be discovered at the end.
