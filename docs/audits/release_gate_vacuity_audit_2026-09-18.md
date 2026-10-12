# Release-gate vacuity audit, 2026-09-18 (tenth pass)

Continues the series started in
`docs/audits/release_gate_vacuity_audit_2026-09-16.md`; BLOCKER 3 of
`docs/RELEASE_HARDENING.md`.

Work was done in a dedicated worktree, `C:\wt\gateaudit_m2wz4r`, minted at
`origin/main` `f7f0d0c1` via `tools/worktree_mint.ps1 -Label gateaudit
-RunSetup`. No hardware was touched, no board flashed, no MCP tool called,
no `.kicad_*` file opened.

**Baseline, before any change:** `113 passed, 0 skipped, 0 failed.` (exit 0).

## Method, and what "examined" means here

This pass deliberately led with *mechanical screens over the whole check
population* rather than a hand-picked slice, because the prior nine passes
had already negative-tested most individually-interesting gates and a tenth
hand-picked slice risked re-proving known results. Four screens were run
across all 119 tracked `check_*`/`test_check_*` scripts (`tools/`,
`firmware/*/tools/`, `firmware/*/test/`); the guard's own census counts 124
check/test files under `tools/` and `firmware/*/test/`.

Each screen targets one of the failure shapes section 3 enumerates. A screen
hit is not a finding -- every hit was resolved by hand, and the resolution is
recorded below so this pass is itself falsifiable.

### Screen A -- a check with no failure path at all cannot fail

Flagged every script containing no `throw`, `exit 1`, `sys.exit(1)`,
`raise`, `Write-Error` or `assert`. **7 flagged, 0 findings.** All seven are
Python checks that signal failure as `return 1` from `main()` with
`sys.exit(main())` at the bottom -- a real, propagating failure path the
screen's regex simply did not spell:
`check_httpd_task_stack_budget.py`, `check_readiness_gate_display_agreement.py`,
`check_saftyfw_task_count.py`, `check_cfgfs_never_gates_nvs.py`,
`check_recovery_image_size.py`, `check_relay_authority_paths.py`,
`check_stack_margin_baseline.py`. Verdict: honest, all seven.

### Screen B -- a guard that skips instead of failing when its input is missing

Flagged every `Test-Path` guard whose body is `continue`/`return`/`exit 0`.
**27 hits, 0 findings.** Resolutions:

- `tools/check_test_has_assertions.ps1` -- the two `if (-not (Test-Path
  $dir)) { continue }` lines are now **dead code**: the 2026-09-17 pass added
  an upstream loop that `throw`s on any missing fixed directory, and a
  `$scannedFiles -eq 0 -or $scannedFns -eq 0` discovery floor backs it.
  Honest.
- `tools/check_hal_include_boundary.ps1` -- `Get-ScanFiles`/`Get-HalScanFiles`
  return `@()` on a missing directory, but every call site is guarded by a
  hard `throw` on the directory's absence (lines 539/540/569), a **200-file
  discovery floor** ("this check has gone blind, not found a shrunk tree"),
  and a `throw` if *any* enumerated file could not be read
  (`SkippedFiles.Count -gt 0`). This is the strongest anti-vacuity
  construction in the repo. Honest.
- `tools/check_c_files_in_cmakelists.ps1:175` -- returns an empty
  *referenced-names* set on a missing script, which yields **more**
  violations, not fewer. Fails safe. Honest.
- The remainder iterate over files they themselves just enumerated, where a
  `continue` covers a file vanishing mid-scan (the documented
  WRITER-vs-SCANNER race), not a missing prerequisite.

### Screen C -- does each Python check's wrapper actually invoke it?

For all 14 `check_*.py` files having a sibling `check_*.ps1`, confirmed the
wrapper references the `.py` **in non-comment code**. 14/14 do. 0 findings
from the screen itself -- but see the finding below, which this screen's
construction exposed.

### Screen D -- is a file "wired" only because a comment mentions it?

**2 hits, 1 finding.** This is the pass's real result; see below.

## Finding -- `tools/check_no_orphaned_checks.ps1` could absolve a genuinely orphaned check

This guard exists to catch the exact defect `check_saftyfw_task_count.py`
once shipped: a check file that exists on disk, that a sibling's comment
claims "fails loud", and that **nothing ever executes**. Two layered
weaknesses meant the guard could report such a file as covered.

**Weakness 1 (wrapper rule).** For a `check_*.py`, the guard accepted the
mere *existence* of a same-named `.ps1` as proof of coverage:

```powershell
$wrapper = Join-Path $f.DirectoryName ($f.BaseName + ".ps1")
if (Test-Path $wrapper) { continue }
```

A wrapper that never invokes its `.py` leaves that `.py` orphaned while the
guard reports it covered.

**Weakness 2 (the decisive one -- the fallback matched prose).** Files not
absolved by the wrapper rule fall through to:

```powershell
if ($runAllChecksText -notmatch [regex]::Escape($f.Name)) { $orphans += ... }
```

`$runAllChecksText` is the **raw text** of `run_all_checks.ps1`, comments
included. That file's header narrates past orphan incidents *by filename*.
Live, confirmed: `check_saftyfw_task_count.py` appears in
`run_all_checks.ps1` exactly once, at **line 311**, inside a comment:

```
# same "orphaned negative test" shape check_saftyfw_task_count.py was found
```

So a file that nothing executes is reported as wired **precisely because
this repository documented that it once went unwired**. The comment written
to record the bug became the thing that hides it. `check_saftyfw_task_
stack_budgets.py` is named in `run_all_checks.ps1` only from comment lines
too; those are the only two such files in the tree.

Both files are genuinely reached today, via their code-referencing wrappers,
so this was a latent false-absolution rather than live lost coverage -- but
it sat in the one guard whose entire job is detecting lost coverage.

**Fix.** Both weaknesses closed in `tools/check_no_orphaned_checks.ps1`: the
wrapper rule now requires the wrapper to reference the `.py` in non-comment
code, and the fallback now matches against a comment-stripped
`$runAllChecksCode` instead of the raw text (the now-unused
`$runAllChecksText` was removed). Comment lines are stripped in both places,
so naming a file in prose never counts as wiring it.

**Blast radius, measured before adopting the change:** all 14 wrapper/`.py`
pairs reference their `.py` in code, and the only two comment-only-mentioned
files are absolved by the strengthened wrapper rule instead. The change
therefore flags nothing correct today. It tightens; it loosens nothing.

### Negative test

The demonstration is three-way, which is what makes it decisive -- a single
RED would not have distinguished the two weaknesses:

1. **Sabotage** (by hand, in `firmware/SaftyFW/test/check_saftyfw_task_count.ps1`):
   changed the one non-comment reference
   `$script = Join-Path $PSScriptRoot "check_saftyfw_task_count.py"` to a
   placeholder filename, leaving the `.py` referenced only from that
   wrapper's comments. This models exactly the historical defect.
2. **Pre-fix guard** (pristine `HEAD` copy, run against the same sabotage):
   **PASS, exit 0** -- vacuous, the orphan invisible.
3. **Wrapper fix alone**, same sabotage: still **PASS, exit 0** -- because
   the fallback's prose match absolved the file. This is what proved
   weakness 2 was the load-bearing one; had the audit stopped at the first
   fix it would have shipped a change that fixed nothing observable.
4. **Both fixes**, same sabotage: **FAILED, exit 1**, naming
   `firmware\SaftyFW\test\check_saftyfw_task_count.py` as never mentioned by
   `run_all_checks.ps1`.

**Restore.** The sabotaged line was restored **by hand** (never
`git checkout --`/`restore`/`stash`/`reset`). Verified by an empty
`git diff --stat` for that file *and* a `git hash-object` match against the
pre-sabotage blob,
blob:firmware/SaftyFW/test/check_saftyfw_task_count.ps1`f259df1b2e4b104c6ece4a2327a75007831d7ab1`,
identical before and after. `git status --porcelain` then showed exactly one
modified file, the guard itself. Post-restore standalone re-run: **PASS**,
124 files.

No production build artifact is involved anywhere in this fix (the guard and
the wrapper are pure PowerShell text scans), so there was no prebuilt binary
to distrust and no rebuild step was required -- the poisoned-binary shape
does not apply here.

## Gate that genuinely cannot be negative-tested in this context

`firmware/KilnFW/App/test/check_01_kilnfw_pushed_build.ps1` and
`firmware/SaftyFW/test/check_01_saftyfw_pushed_build.ps1` remain the only
carried-forward gates this pass did not exercise, and the reason is
structural rather than a time-box. By design they fetch and build
**`origin/main`'s own content** in a throwaway worktree, deliberately
ignoring the local tree. A local sabotage therefore cannot reach them: the
only way to drive their FAIL path is to push a knowingly-broken commit to
the shared `origin/main`, which would break every other session on this
machine. Saying so plainly is the honest outcome; papering over it with a
local edit that the check does not even read would be a fake negative test
of exactly the kind this item exists to prevent.

Partial evidence does exist and is worth naming: the fifth pass recorded
`check_01_kilnfw_pushed_build.ps1` genuinely failing against live
`origin/main` on a real `-Werror=format-truncation` defect -- unplanned, but
direct evidence the FAIL path propagates.

## Result

- **Checks examined:** 119 tracked `check_*`/`test_check_*` scripts through
  four mechanical screens, plus by-hand resolution of all 34 screen hits
  (7 + 27) and targeted inspection of the 14 wrapper/`.py` pairs.
- **Findings:** 1 (`check_no_orphaned_checks.ps1`, two layered weaknesses),
  fixed and negative-tested.
- **Honest-and-narrow, not findings:** everything in screens A, B and C.
  `check_hal_include_boundary.ps1` and `check_test_has_assertions.ps1` are
  worth citing as positive models -- both pair a hard `throw` on a missing
  input with a numeric discovery floor.
- **Left alone as settled:** `check_sim_iter_tune_bars.ps1`'s A8
  cross-profile bar (deliberately outside its exit code, genuinely failing
  at 6.82%) and `firmware/SaftyFW/tools/check_link_impl_isolation.ps1`'s
  path allowlist (documented intended remedy).

**Full-suite verdict after the fix, verbatim:** `113 passed, 0 skipped, 0
failed.` -- exit 0, no `-AllowSkips`, unchanged from the baseline as
expected, since this pass modified an existing glob-discovered check rather
than adding a new one.

## Generalizable lesson

A guard that decides coverage by searching a build/runner script for a
filename must search that script's **code**, not its prose. This repository
documents its own defects in comments unusually thoroughly, which is a
strength -- but any `-match` over raw script text turns that documentation
into false evidence of wiring, and the better a defect is documented, the
more convincingly it hides. Worth checking the next time a guard asks "is
this file mentioned anywhere?".
