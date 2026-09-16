# Adversarial review of 94377112 (check_00 D1/D3/D4/D5/D6 closure)

Reviewed 2026-09-16 from a clean worktree of `origin/main` at `94377112`
(`C:\wt\chk00rev_q7x3m2`, removed afterward). Parent for all comparisons is
`6d8c7194`. No production source was changed; the two poison tests below were
applied to this tree's own files and restored by hand, verified by MD5 against
the pre-poison bytes and by an empty `git diff`.

## Verdict per defect

| | verdict |
| --- | --- |
| D1 glob half | CLOSED for the bracket case, but it **introduces a new deletion path** — see N1 |
| D1 lock half | HOLDS. Derivation matches `build_lock.ps1` exactly; zero timeout genuinely non-blocking; abandoned mutex does not wedge |
| D2 | HOLDS. Already closed by `69441590`; verified independently |
| D3 | PARTIAL. The witness is real and load-bearing, but the "by construction" claim is FALSE — see N7 |
| D4 | PARTIAL. What it names is fixed; it misses a larger instance — see N2 |
| D5 | PARTIAL. Window shortened as claimed, but "contains no failure path" is FALSE — see N4 |
| D6 | HOLDS. Verified by survey |

## Negative tests, per-test counts

Each lifts the real code verbatim out of each version of the file by anchor
search (prune loop, sentinel block, `Mirror-Tree` plus each call site's parsed
exclude arrays). The only edit is retargeting the prune's hardcoded `C:\wt`
literal at a fixture root, so no fixture can reach a real directory.

**D1, prune loop (5 cases).** Parent **3 passed / 2 failed**; commit
**4 passed / 1 failed**.

| case | parent | commit |
| --- | --- | --- |
| bracketed LIVE owner | FAIL (deleted) | PASS |
| genuinely absent owner | PASS (pruned) | PASS (pruned) |
| plain LIVE owner | PASS | PASS |
| non-ASCII LIVE owner | PASS (kept) | **FAIL (deleted)** |
| absent owner, victim lock HELD | FAIL (deleted mid-build) | PASS (deferred) |

The implementer ran three cases and reported 3/1 to 4/0. The bracket and
lock cases reproduce. The non-ASCII case was not tested and is a regression.

**D3, sentinel block (3 scenarios).** Parent **1 passed / 2 failed**; commit
**2 passed / 1 failed**. Scenario 2 (mirror fully no-op'd, edited `.c` is the
newest file) reproduces the implementer's direction: parent reports success,
commit fails naming the stale file. Scenario 3 is mine and is not detected —
see N7.

**D4, Mirror-Tree with each version's parsed exclude list (6 cases).** Parent
**3 passed / 3 failed**; commit **5 passed / 1 failed**. Top-level `build`,
nested `App\test\build`, nested CommonFW `test\build`, destination `lvgl`
survival and real-source delivery all pass on the commit. The surviving
failure is N2.

**D5** is a line-order property, confirmed by inspection: parent creates the
directory at line 256 and marks it at line 400; the commit at 313 and 328.

## Poison tests — which assertions are load-bearing

**Poison 1 (real script, real build worktree).** Neutered `Mirror-Tree`'s
`robocopy` invocation and appended a line to `firmware/KilnFW/App/main.c`.
`check_00` exited **1** at the newest-source witness hash comparison
(`check_00_kilnfw_target_build.ps1:519-520`), naming `main.c` and its mtime,
before `idf.py` ran at all. Restored by hand; MD5 of both files matches the
pre-poison bytes and `git diff` is empty. A subsequent clean run exits 0.

**Poison 2 (extracted prune loop).** Changed the victim-tag derivation by one
character (`Substring("checkbuild".Length)`, leaving the separator in the tag).
Result **3 passed / 2 failed** — the held-lock case flips straight back to
deleting a directory mid-build, silently. The lock-name derivation is
therefore load-bearing to exactly one character, and nothing in the repo
enforces it. See N3.

## Victim's-lock design — judgement

Sound, and the deviation from the brief was the right call. Verified:

* `Enter-BuildLock` builds `Global\kilnCtl_buildlock_<Name>` and `$LockName`
  is `kilnfw_checkbuild_worktree_<tag>`; the prune builds
  `Global\kilnCtl_buildlock_kilnfw_checkbuild_worktree_<tag>` from the victim
  directory's own suffix. For every tag this script can generate the two are
  identical strings.
* `WaitOne(0)` against a mutex held by a *different process* returns `False`
  in 0 ms — genuinely non-blocking, confirmed by measurement.
* `AbandonedMutexException` is caught and treated as acquired, so a crashed
  holder does not wedge pruning forever. Confirmed by reading the same
  handling in `build_lock.ps1`.

Two residual gaps, neither fatal: N3 (the mutex prefix is a duplicated
literal) and N6 (TOCTOU).

**Can any path still delete a live owner's directory? Yes — N1.**

## New defects, ranked

**N1 (HIGH, regression introduced by this commit). A live owner whose tree
path contains a non-ASCII character now has its build directory deleted.**
The marker is written `-Encoding ascii`, so such a path is stored with `?`
substituted. Measured against a fixture whose owner directory genuinely
exists:

| predicate | result |
| --- | --- |
| `Test-Path $owner` (parent) | `True` → skipped, directory kept |
| `[System.IO.Directory]::Exists($owner)` (commit) | `False` → **pruned** |

This is D1's own failure shape — force-removing a live session's build
directory — reached through a different trigger, and the commit message
argues the opposite ("a marker that does not name a path that literally
exists now fails the test honestly rather than by luck"). For the ASCII
mangling case the path *does* exist; it is the marker that is lossy, and the
wildcard was load-bearing safety rather than luck. The direction is **not**
"leaks disk": it is "deletes the wrong thing", the same as before. The
victim's-lock deferral narrows but does not close it — an owner that is live
but not mid-check holds no lock. Blast radius is a regenerable ~425 MB build
directory, not source. Fix: write the marker `-Encoding utf8` (and read it
back the same way); the literal predicate is then correct and safe.

**N2 (MEDIUM). D4 is incomplete and misses a larger instance than the one it
fixed.** `firmware/CommonFW/build` — **427 files, 67.9 MB** in the main tree —
is excluded by neither version: the CommonFW call site lists `test\build` and
`.git` only. That is roughly three times the 418 files / 21.9 MB the commit
measures and claims to have recovered (both figures confirmed here). Its own
reasoning covers it; the list does not. Also uncovered, but negligible:
`firmware/KilnFW/.venv/Lib/site-packages/pip/_internal/operations/build`
(12 files).

**N3 (MEDIUM). The prune hard-codes `build_lock.ps1`'s internal mutex name
prefix.** `Global\kilnCtl_buildlock_` is now written out in two files with no
shared constant and no check tying them together. This is the "reset one side
of a pair" class CLAUDE.md documents: the two sides are joined by a naming
contract expressed nowhere, and a drift makes the prune take a lock nobody
holds and delete a live directory — silently, with health output that looks
clean. Poison 2 shows a one-character divergence is enough. Fix: export the
name-building from `build_lock.ps1` (e.g. `Get-BuildLockMutexName -Name`) and
call it from both sites.

**N4 (LOW). D5's "a 15-line window that contains no failure path" is false.**
Lines 314-316 are `if ($LASTEXITCODE -ne 0) { Fail "git worktree add failed" }`,
inside the window, before the marker write at 328. Empirically the reachable
failure modes are benign — a bogus commit-ish exits 128 and leaves no
directory, and the non-empty-destination case (which does leave one) is
excluded by the `Test-Path $WorktreePath` guard above it — so the defect is an
overclaim in the comment rather than a live orphan source. Separately,
`Set-Content` at 328 raises a non-terminating error under
`$ErrorActionPreference = "Continue"`, so a failed marker write would produce
exactly the unreclaimable directory D5 set out to eliminate, with no signal.

**N5 (LOW). The directory regex is case-insensitive; Win32 mutex names are
case-sensitive.** `-match '^checkbuild_[0-9a-f]{10}$'` accepts uppercase hex
(PowerShell `-match` is case-insensitive by default), and a mutex named with
uppercase hex was confirmed here to be acquirable independently of its
lowercase twin. This script only ever generates lowercase, so it is
unreachable today; `-cmatch` would close it.

**N6 (LOW). The owner-exists test is never re-checked after the victim's lock
is taken.** More to the point, the victim itself runs `git worktree add` and
writes its marker (lines 311-328) *outside* its own lock, which it does not
take until line 407. A tree recreated at a previously-pruned path can
therefore be starting up, unlocked, while a pruner that already decided "owner
gone" holds the lock and deletes. Narrow, but the lock does not cover what the
comment implies it covers.

**N7 (LOW-MEDIUM, D3). The "by construction" claim does not hold.** The
witness is *whatever file has the newest mtime*, which is only the edited file
when nothing else in three trees was touched more recently. Demonstrated:
with a stale `.c` whose source copy is byte-different but the same size and
the same mtime as the destination's, `robocopy /MIR` **skips it** (measured:
destination content unchanged, exit 1), while an unrelated newer file becomes
the witness and hash-matches — the block reports success and the stale `.c`
rides into the build. Not hypothetical in shape: this review's own clean
`check_00` run selected `check_00_kilnfw_target_build.ps1` itself as the
witness, i.e. a file that is not firmware source at all. The assertion is
still a real strengthening over three `CMakeLists.txt` pairs, and it is
load-bearing (poison 1); it is just weaker than "by construction the edit a
no-op mirror failed to deliver". A content-based witness (the newest file by
`git status`/`git diff --name-only`, or hashing every mirrored file) would
match the claim.

## The two open items the implementer reported

**The markerless orphan.** It no longer exists as described: the directory the
commit names now carries a marker naming `C:\wt\divrevpar_k7q2m9`, which is
live, so it is claimed and prunable. On the general question — leaving
pre-existing markerless directories permanently unreclaimable is the **right
call**, and no reclamation path should have been offered. Ownership is exactly
what the marker encodes; without it the only available signals (name shape,
mtime, size) are the same ones that would let the prune reach a hand-made
worktree, and `C:\wt` holds ~195 directories belonging to live sessions. The
correct answer is the one taken: make new directories markered from their
first instant, report the orphan, and let a human delete it. The cost is
bounded and one-off.

**The `-Encoding ascii` marker.** The direction is **not** safe — see N1. This
is the one open item that should have been fixed rather than argued away.

## Suite result

`tools/run_all_checks.ps1` under `-ExecutionPolicy Bypass`, foreground, in
this clean worktree after `uv sync` in `tools/PcTools`: **89 passed, 1 skipped,
4 failed** of 94 (the implementer reported 88/2/4; the difference is
`check_ui_responsive_sweep`, which they saw skip transiently and which passed
here). `check_00_kilnfw_target_build` itself **passed**, building into a fresh
per-tree directory.

| failure | classification |
| --- | --- |
| `check_mcp_facade_coverage` | environmental, not theirs — `tools/mykicadMcp` submodule uninitialized (`kicad_facade.py` absent) |
| `check_mykicad_golden_suite_runs` | environmental, not theirs — same submodule (`requirements-mcp.txt` absent) |
| `check_doc_hash_citations` | environmental, not theirs — one citation needs `sub:lvgl`, submodule uninitialized |
| `check_main_task_stack_budget` | pre-existing, not theirs — reads `REPO_ROOT/firmware/KilnFW/sdkconfig` unconditionally, which a clean worktree lacks. Owned by another agent; not touched |
| SKIP `check_01_kilnfw_pushed_build` | same missing-sdkconfig prerequisite |

`check_all_task_stack_budgets` **passed**, which is the functional half of D2.

## D2, verified independently

`check_all_task_stack_budgets.py`'s `_sdkconfig_candidates()` resolution order
is: explicit `--sdkconfig`; then `<elf-stem>.sdkconfig` or `sdkconfig` sitting
**in the ELF's own directory**; then `<elf-dir>/../sdkconfig`; with no
repo-root fallback. The parent revision of `check_00` already publishes the
sdkconfig that built the ELF into that same directory, and `git show 69441590`
confirms that hunk is where it came from. So the fix predates this commit,
`94377112` correctly made no change, and the check passes in this clean
worktree. The claim holds.

## No collateral damage

One `checkbuild_<hex>` directory was pruned during my suite run. Verified
afterward that no directory under `C:\wt` and none of the 51 registered git
worktrees hashes to its tag, so its owner was genuinely gone and the prune was
correct. Every remaining `checkbuild_<hex>` directory has a marker naming a
live, ASCII-clean tree. The directory the implementer reported as a live
orphan was not touched.

## Safe to stop iterating?

**No.** N1 is a live cross-session deletion path that this commit created
while closing a different instance of the same bug, and it is a one-line fix
(`-Encoding utf8`). N2 leaves three times more wasted I/O than D4 recovered,
also one line. N3 is the repo's own documented bug class, sitting under the
new lock the whole D1 fix rests on. The lock design itself, the D4 fix as far
as it goes, D5's reordering and D6 are sound and worth keeping.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
