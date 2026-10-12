# Review: baselinefx1 (17a0bfe32) and gatetblfx1 (b7412bec7)

Date: 2026-10-12. Reviewer: Claude (Opus). Read-only review plus negative tests through
`tools\negtest.ps1`, all run against origin/dev `648e97e6d`. No board access.

Commits reviewed (both on origin/dev):

- `17a0bfe32` "check_main_baseline: scratch repo copies every lib_*.ps1". Changes
  `tools/check_main_baseline.ps1` only.
- `b7412bec7` "Gate evidence table: rows for check_lcd_sink_callgraph and its test".
  Changes `docs/audits/GATE_NEGATIVE_TEST_EVIDENCE.md` only.

Verdict: **0 HIGH, 1 MED, 1 LOW, 2 INFO.** MED-1 is still open at the dev tip after
`d8d6713ac` (see the re-run section). Both commits are correct. Every claim in the
new evidence rows was reproduced: M1 to M3 CAUGHT on the same FAIL lines, M4 MISSED with
exit 0, and the counts match. The MED is not an error in the row. The row honestly says
M4 is MISSED, but it gives the wrong reason: it calls the loop "redundant" in the
synthetic dumps. A two-hop case shows the loop is needed, and nothing tests it, so the
call-graph check can lose that loop and stay green.

## Questions asked, answered

| Question | Answer |
|---|---|
| Does `17a0bfe32` fix the real cause? | Yes. `run_all_checks.ps1:200` now dot-sources `lib_check_discovery.ps1`, which the scratch repo in case 5 did not have. The scratch `run_all_checks.ps1` then died at load (`...\scratch\tools\lib_check_discovery.ps1' is not recognized`), and the 9 scratch-run assertions failed. That matches the commit message. The other dot-sourced files (`lib_pctools_python.ps1`, `checkcache_lib.ps1`, `main_baseline_lib.ps1`) are still copied: the first by the `lib_*.ps1` glob, the other two by name. |
| Can the baseline test still fail? | Yes. `negtest -Preset check -PresetArg tools\check_main_baseline.ps1`: the baseline passes (69 s), and all 5 mutations are CAUGHT on Assert `FAIL:` lines, not on a crash of the test itself (table below). B1 and B1b (the fix reverted) reproduce the original 9 failures exactly. |
| Is the glob safe? | Yes. A glob with no matches passes `$null` to `Copy-Item`, which throws inside the test's `try` block, so the test fails loudly. It does not pass silently. Copying the extra libraries (`lib_safe_remove.ps1`, `lib_push_verify_walk.ps1`) does no harm: discovery only picks up `check_*` files, so they are never run as checks. |
| Are the gatetblfx1 counts right? | Yes. `check_gate_negative_test_table.ps1` passes (`OK: 201 discovered checks all have a row (202 rows)`). Recounting the rows by hand gives NEGATIVE-TESTED 189, PARTIAL 1 and REVIEWED, NOT MUTATED 12, total 202, the same as the header. The one row that is not discovered is `check_submodule_pins_pushed.ps1`, which existed before this commit and is excluded on purpose. Both new rows name discovered checks. |
| Is the cited base still accurate? | Yes. `b4c819c80` is an ancestor of dev, and neither `check_lcd_sink_callgraph.py` nor its test has changed since then (`git diff b4c819c80 HEAD` is empty for both). |
| Do M1 to M4 do what the row says? | Yes, all four reproduced (table below). M1 gives `FAIL: D ...`. M2 gives `FAIL: E ...`. M3 gives `FAIL: B ...` and `FAIL: C ...`. M4 exits 0 with A to F all PASS. Three extra mutations (R5 to R7) are also CAUGHT. |
| Is M4 really "redundant" in the synthetic dumps? | Only in the sense that none of the dumps exercises it. The loop is not redundant in the check itself. See MED-1. |

## Findings

### MED-1: the node-keyed reverse walk (M4) has no test, and the row calls it "redundant"

STATUS: FIXED (case N in `tools/test_check_lcd_sink_callgraph.ps1`; M4 now CAUGHT by `FAIL: N`; evidence row corrected).

Where: `tools/check_lcd_sink_callgraph.py:93-94`
(`for c in rev.get(node, ()): work.append((c, node))`), and
`tools/test_check_lcd_sink_callgraph.ps1` cases A to F. The row text in
`GATE_NEGATIVE_TEST_EVIDENCE.md` says: "the bare-name `rev.get(fn, ())` loop after it
reaches the same callers in the synthetic dumps, so it is redundant there".

Why it is not redundant: `parse()` names a reference to a static function's section
(`.text.s_do`) by its object-qualified key (`<obj>::s_do`). Only the node-keyed loop
follows those keys. The bare-name loop only follows references to global symbols by
name. Inside a UI object this does not matter, because the first UI node the walk
reaches already fails. Outside the UI, it does matter whenever the path to the sink goes
through a static helper in an object that is on `NON_UI_ALLOW`, which is the normal case
for HTTP and link plumbing.

Reproduced with synthetic dumps, without touching the repo. In
`drivers/http/dashboard_exec_http.c.obj`, a static `s_do` calls
`dashboard_safety_clear_trip`, and a global `w_clear` references `.text.s_do`.
`drivers/ui/ui_page_home.c.obj::home_cb` references `w_clear`. The check at HEAD fails
with `LCD UI code reaches a trip-clear sink: drivers/ui/ui_page_home.c.obj::home_cb (via
drivers/http/dashboard_exec_http.c.obj::w_clear)`. With M4 applied it prints `LCD sink
call-graph check passed` and exits 0. If `NON_UI_ALLOW` does not cover the helper's
object (for example `drivers/misc/helper.c.obj`), M4 is still partly caught by the
unreviewed-caller rule. So the gap is specific to allowlisted objects.

Impact: this check is the structural guard (REVIEW_LCDFX9 HIGH-2) that keeps an
LCD-to-trip-clear path from bypassing the ADMIN gate. A refactor that drops or breaks the
node-keyed walk would pass the standing suite. The PARTIAL status is honest; the
reasoning in the row is what is wrong.

Fix: add a case G to `test_check_lcd_sink_callgraph.ps1` that uses exactly the dump
above (helper chain inside `dashboard_exec_http.c.obj`, expect exit 1 matching
`home_cb`). Re-run M4, which should now be CAUGHT, and move both rows to
NEGATIVE-TESTED. Until that is done, change the row text from "redundant there" to "not
exercised by any synthetic case (needed for static-helper chains in allowlisted
objects)".

### LOW-1: the test row says NEGATIVE-TESTED, but it records the same MISSED mutation

STATUS: FIXED (M4 CAUGHT; both rows NEGATIVE-TESTED, header counts 190 / 0 PARTIAL).

Where: the `tools/test_check_lcd_sink_callgraph.ps1` row. It cites "the same four
mutations" with "M4 MISSED", yet its status is NEGATIVE-TESTED, while the check row with
the same evidence is PARTIAL. M4 is a gap in the test: the test cannot detect that
mutation. So by the table's own standard the test row should also be PARTIAL, or the
counts should be redone after MED-1 is fixed. The counts are currently consistent with
the statuses as written, so `check_gate_negative_test_table.ps1` does not catch this.

### INFO-1: the scratch copy depends on a naming convention

`17a0bfe32` copies `lib_*.ps1` plus two files by name. If `run_all_checks.ps1` later
dot-sources a file named `*_lib.ps1` (like `checkcache_lib.ps1`) or anything else, case
5 fails again, loudly, as it did this time. A sturdier option is to derive the list from
the `. (Join-Path $PSScriptRoot "<name>")` lines in `run_all_checks.ps1`. Not required:
the failure mode is loud, not silent.

### INFO-2: the test header lists cases A to E, but the test also has case F

`test_check_lcd_sink_callgraph.ps1` lines 2 to 6 document A to E. Case F (no build
returns exit 3 SKIP) exists at line 59, and R7 shows that it catches a SKIP-returns-0
mutation. The header was simply not updated.

## Mutation table

All runs used `tools\negtest.ps1 -Preset check` from worktree `C:\wt\rvbaseline1_44yemj`
at `648e97e6d`, with mutation JSON kept in a private scratch directory. Both runs ended
with the baseline PASS, `real_tree_unchanged=true` and `copies_removed=true`. The verdict
pattern was the test's own Assert line (`FAIL: [A-F] ` for the LCD test, `^\s+FAIL: ` or
the assertion-count guard for the baseline test). No CAUGHT result came from a crash.

### `-PresetArg tools\test_check_lcd_sink_callgraph.ps1` (target `tools/check_lcd_sink_callgraph.py`)

| # | Mutation | Result | Matched |
|---|---|---|---|
| M1 | gate-edge filter `if c.partition("::")[2] != GATE_CALLER]` -> `if False]` | CAUGHT | `FAIL: D table referencing gated action fails` |
| M2 | `if not is_ui(c):` (non-UI allowlist) -> `if False:` | CAUGHT | `FAIL: E unreviewed non-UI caller fails` |
| M3 | UI-reach `fails.append(...)` -> `pass` | CAUGHT | `FAIL: B ...`, `FAIL: C ...` |
| M4 | node-keyed loop `for c in rev.get(node, ()):` -> `for c in ():` | **MISSED** (exit 0) | none; reproduces the row. Gap demonstrated in MED-1 |
| R5 | bare-name loop `for c in rev.get(fn, ()):` -> `for c in ():` | CAUGHT | `FAIL: C ui reaches sink via non-ui helper fails` |
| R6 | gate caller `!=` -> `==` | CAUGHT | `FAIL: A ...`, `FAIL: D ...` |
| R7 | `skip()` returns 0 instead of 3 | CAUGHT | `FAIL: F no build skips` |

### `-PresetArg tools\check_main_baseline.ps1`

| # | Mutation | Result | Matched |
|---|---|---|---|
| B1 | the fix's `lib_*.ps1` `Copy-Item` line deleted | CAUGHT | the 9 scratch-run asserts, e.g. `FAIL: clean HEAD == origin/main run records a baseline`; the log shows `lib_check_discovery.ps1' is not recognized` |
| B1b | glob replaced by `lib_pctools_python.ps1` only (the list before the fix) | CAUGHT | the same 9 asserts |
| B2 | `run_all_checks.ps1`: `$rep.Cmp.New.Count -eq 0` -> `-ge 0` | CAUGHT | `FAIL: -FailOnlyOnNew still fails on a NEW failure`, `FAIL: ... non-merge-base baseline keeps the failing exit code` |
| B3 | `main_baseline_lib.ps1`: `$known += $p` -> `$new += $p` | CAUGHT | 12 asserts, e.g. `FAIL: KNOWN = same non-pass status on main` |
| B4 | `main_baseline_lib.ps1`: `elseif (-not $Exact)` -> `elseif ($false)` | CAUGHT | `FAIL: without -Exact nothing is KNOWN ...`, `FAIL: B2a: the failure is not KNOWN` |

### Re-run at the dev tip (after `d8d6713ac`, REVIEW_LCDFX10 fixes)

`d8d6713ac` landed while this review was running. It rewrote parts of `analyse()` and
added test cases G to M, so the M1 and M3 find-strings above no longer apply. The
node-keyed loop is unchanged (now line 146), and none of the new cases is a static-helper
chain in an allowlisted object. Re-run at `8c763167a` (this doc's commit, rebased on
`ee9847b53`), baseline PASS:

| # | Mutation | Result | Matched |
|---|---|---|---|
| M4 | `for c in rev.get(node, ()):` -> `for c in ():` | **MISSED** (exit 0) | none: MED-1 still open |
| R5 | `for c in rev.get(fn, ()):` -> `for c in ():` | CAUGHT | `FAIL: C ui reaches sink via non-ui helper fails` |

The MED-1 two-hop dump also still separates the two at the tip: the real script fails on
`home_cb (via drivers/http/dashboard_exec_http.c.obj::w_clear)`, and with M4 it prints
`passed (6 referenced symbols)`. The check row's text after `d8d6713ac` still says
"redundant there", and INFO-2 still applies: the header now lists A to E and G to M,
but still not F.

Separate demonstration for MED-1 (not a negtest run): a scratch copy of
`check_lcd_sink_callgraph.py` with M4 applied, run against the two-hop dump described in
MED-1. The real script exits 1 and the mutant exits 0.
