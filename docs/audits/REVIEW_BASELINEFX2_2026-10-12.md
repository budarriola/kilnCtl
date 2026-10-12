# Review: baselinefx2 (042909f0e, 1bfdf6f75)

Date: 2026-10-12. Reviewer: Claude (Opus). Read-only review plus negative tests through
`tools\negtest.ps1`, run against origin/dev `1bfdf6f75`. No board access.

Commits reviewed (both on origin/dev). Together they fix REVIEW_BASELINEFX1_GATETBLFX1
MED-1 and LOW-1:

- `042909f0e` adds case N to `tools/test_check_lcd_sink_callgraph.ps1`: a static helper
  chain inside an allowlisted object, which only the node-keyed reverse walk follows.
- `1bfdf6f75` updates the two `GATE_NEGATIVE_TEST_EVIDENCE.md` rows, sets the header
  counts to NEGATIVE-TESTED 190 and PARTIAL 0, and marks MED-1 and LOW-1 FIXED.

Verdict: **0 HIGH, 0 MED, 1 LOW, 2 INFO.** The fix is correct. Case N isolates M4: with M4
applied, N is the only case that fails. The header counts are right. The one gap is in
labelling. The row does not say that the re-run M1 was reconstructed, and it still
describes M1 and the base by the old text and the old SHA.

## Questions asked, answered

| Question | Answer |
|---|---|
| Does case N truly isolate M4? | Yes. In N, `dashboard_exec_http.c.obj::s_do` calls the sink, `w_clear` references `.text.s_do` (object-qualified key), and `ui_page_home.c.obj::home_cb` references `w_clear` by bare name. The walk must use the node-keyed loop (`rev.get(node)`, line 146) for `s_do -> w_clear`, then the bare-name loop for `w_clear -> home_cb`. With M4, the test log shows exactly one FAIL, `N`, and every other case PASS. Without M4, N fails on `home_cb` for the intended reason: `dashboard_exec_http.c` is on `NON_UI_ALLOW`, so the unreviewed-caller rule cannot be what fails it. |
| Do the rows' claims match reality? | Yes for every result claimed. M4 is CAUGHT with "only N fails" (reproduced). With the bare-name loop emptied, it is CAUGHT with `FAIL: C` and `FAIL: N` (reproduced as R5). M1 at the current code is CAUGHT with `FAIL: D` (reproduced, reconstructed text below). For M2, M3 and M1b I did not re-run them; their claims are consistent with the code. |
| Are the header counts correct? | Yes. A hand recount of the table's status column gives 190 NEGATIVE-TESTED, 12 REVIEWED, NOT MUTATED and 0 of every other status: 202 rows, matching `Gate rows: 202`. `check_gate_negative_test_table.ps1` reports `OK: 201 discovered checks all have a row (202 rows)`. |
| Is the reconstructed M1 honestly labelled? | No, see LOW-1. Nothing in the row or the commit says that M1 was reconstructed. |

## Findings

### LOW-1 (FIXED): the reconstructed M1 is not labelled, and the row's M1 text and base are stale

Where: the `tools/check_lcd_sink_callgraph.ps1` row in `GATE_NEGATIVE_TEST_EVIDENCE.md`.

The row still defines M1 as "gate-edge referencer filter -> `if False`" and cites
base `b4c819c80`. That find-string (`if c.partition("::")[2] != GATE_CALLER]`) no longer
exists. `d8d6713ac` rewrote the filter as
`if not (os.path.basename(c.partition("::")[0]) == GATE_OBJ and c.partition("::")[2] == GATE_CALLER)]`.
The row then says "rerun with N: M1-M3 CAUGHT" as if the same M1 were re-applied. The
re-run M1 had to be a new mutation, and the fixer says the original text could not be
recovered, but neither the row nor the commit message says so. The re-run's base (after
`d8d6713ac`, around `042909f0e`) is not recorded either. The Audit column still shows only
`b4c819c80`.

Impact: the claimed result is true, because I reproduced it (M1r below, `FAIL: D`). The
risk is to the record: a later reader would assume the 10-12 re-run used the M1 text the
row prints.

Fix: in the rerun clause, write "M1 reconstructed for the post-`d8d6713ac` filter
(`if not (...GATE_OBJ and ...GATE_CALLER)]` -> `if False]`; original text not
recoverable)". Add the re-run base SHA to the Audit column.

### INFO-1 (FIXED): the test header still does not list case F

`tools/test_check_lcd_sink_callgraph.ps1` lines 2 to 14 list A to E, G to N and K, but
not F (no build means exit 3 SKIP). This was REVIEW_BASELINEFX1 INFO-2. `042909f0e`
edited this header to add N and left F out.

### INFO-2 (FIXED): mutation names drift between documents

The row's "M1b" (bare-name append off, H and I2) is not the earlier review's R5 (bare-name
walk loop emptied). The row also calls M4 the "first" reverse-walk loop, but it is the
first of two loops inside the `while`, after the seeding loops. These are naming issues
only. Results are unaffected.

## Negtest table

One run: `tools\negtest.ps1 -Preset check -PresetArg tools\test_check_lcd_sink_callgraph.ps1
-Mutations <json>` from worktree `C:\wt\rvbaseline2_7q7i69` at `1bfdf6f75`. The mutation
JSON was kept in a private scratch directory. Baseline PASS (17.1 s). `ALL_CAUGHT`,
`real_tree_unchanged=true`, `copies_removed=true`. Each CAUGHT came from the test's own
`FAIL:` Assert line (exit 1, `N failure(s)`), not from a crash. The check is Python, so
there is no compile step.

| # | Mutation (`tools/check_lcd_sink_callgraph.py`) | Result | Failing cases (full list from the mutant log) |
|---|---|---|---|
| M4 | node-keyed loop `for c in rev.get(node, ()):` -> `for c in ():` (line 146) | CAUGHT | `FAIL: N` only (1 failure) |
| M1r | gate filter `if not (os.path.basename(c.partition("::")[0]) == GATE_OBJ and c.partition("::")[2] == GATE_CALLER)]` -> `if False]` (my own reconstruction for the current code) | CAUGHT | `FAIL: D` only (1 failure) |
| R5 | bare-name loop `for c in rev.get(fn, ()):` -> `for c in ():` | CAUGHT | `FAIL: C`, `FAIL: N` (2 failures) |

## Fix status

LOW-1, INFO-1 and INFO-2 fixed in the baselinefx3 commit: the row now labels M1 as reconstructed for the post-d8d6713ac filter, cites the 042909f0e re-run base next to b4c819c80, names M4 as the node-keyed loop and the bare-name loop as R5 (the row's M1b is a different mutation: bare-name append off); the test header lists case F.
