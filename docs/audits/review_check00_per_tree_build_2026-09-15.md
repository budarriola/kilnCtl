# Adversarial review of 827dd887 (per-tree check_00 build dir; ESP32-S3 target pin)

Reviewed 2026-09-15 from a clean worktree of origin/main at `827dd887`
(`C:\wt\chkrev_f14ee9`, removed afterward). No production source changed.

## Verdict per claim

1. **Per-tree build directories — HOLDS, with one live-deletion hazard.**
   Main worktree keeps `C:\wt\checkbuild` and lock `kilnfw_checkbuild_worktree`
   byte-for-byte (old line 119/193 vs new line 183/184 — verified against the
   parent revision). Non-main trees get `checkbuild_<10 hex>`; my run created
   `C:\wt\checkbuild_df4732f81a` (597 MB) with a correct marker, and
   `C:\wt\checkbuild`, `checkbuild_origin_kilnfw`, `checkbuild_origin_saftyfw`
   were untouched. See **D1** for the prune hazard.

2. **Premise correction — CORRECT.** The parent revision's line 108 is
   `$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")` and line
   196 mirrors `(Join-Path $repoRoot "firmware\KilnFW")`. The mirror SOURCE was
   always the invoking tree. The original report ("validates the main tree") was
   wrong; only the DESTINATION was shared. This commit is built on a correct
   reading.

3. **The robocopy `/XD` fix — REAL BUG, REAL FIX, both reproduced.** Fixture
   test with an *empty* source `components\lvgl` (the uninitialized-submodule
   case the bug required) and a populated destination:
   - old relative form (`/XD build components\lvgl`): destination's
     `components\lvgl\CMakeLists.txt` **deleted**.
   - new both-roots absolute form: **survives**.
   `build` survived under both, as the commit says (bare name, no separator).

4. **Mirror kept + sentinels — floor is enforced but the assertion is weak.**
   `if ($verifiedPairs -lt 2) { Fail ... }` is real code on a real path, and
   every failure exit is `Fail` (exit 1), never 3. But see **D3**.

5. **Target pin — VERIFIED.** In a fresh worktree with no sdkconfig and no
   `set-target`, `idf.py reconfigure` exits 0 and writes
   `CONFIG_IDF_TARGET="esp32s3"`. (It additionally requires the lvgl submodule
   to be initialized first — without it reconfigure dies on "component 'lvgl'
   could not be found", which the commit message does not mention.) No
   non-esp32s3 IDF project in the repo is affected: the only other
   `sdkconfig.defaults` files are inside `managed_components/` (mdns, littlefs
   host tests), and SaftyFW is pico-sdk. For an existing tree with a generated
   `sdkconfig`, the pin is inert — ESP-IDF keeps the existing value, which is
   the entire premise of `check_sdkconfig_defaults_applied.ps1`.

6. **91 / 0 / 3 — reproducible only after three hand-provisioning steps, and
   the stack-budget credit is WRONG.** See **D2** and the numbers below.

## Independent suite numbers

| worktree state | result |
| --- | --- |
| straight out of `git worktree add` | runner aborts **exit 2**, zero checks run (`tools/PcTools/.venv` absent) |
| + `uv sync` in tools/PcTools | **87 passed, 1 skipped, 6 failed** of 94 |
| + lvgl submodule init + `idf.py reconfigure` (generated sdkconfig) | `check_all_task_stack_budgets` **FAILS** on a gpio_probe ELF/sdkconfig disagreement |
| + main tree's `sdkconfig` copied in | **91 passed, 0 skipped, 3 failed** — matches the commit |

The 6 failures in the unprovisioned run: `check_flash_worker_lint`,
`check_mcp_facade_coverage`, `check_mykicad_golden_suite_runs` (all declared
pre-existing), plus `check_main_task_stack_budget` and
`check_all_task_stack_budgets` (both "no sdkconfig") and
`check_doc_hash_citations` ("unknown sub: name 'lvgl'", submodule uninit). The
skip was `check_01_kilnfw_pushed_build` ("no sdkconfig ... to seed this build").

## New defects, ranked

**D1 (high, silent, cross-session). The prune deletes a LIVE tree's build
directory when the owning tree's path contains `[` or `]`.** Line 245 is
`if (Test-Path $owner) { continue }`. `Test-Path` glob-expands: for an existing
directory `C:\wt\tree[1]`, `Test-Path` returns **False** while
`[System.IO.Directory]::Exists()` returns True (verified both in isolation and
by running the exact prune predicate against fixtures). The directory is then
`git worktree remove --force`d and `Remove-Item -Recurse -Force`d. The prune
loop runs at line 237, **outside** the build lock taken at line 339, so this can
land on another session's build directory while its build is running. `-LiteralPath`
is the fix. No bracketed worktree exists under `C:\wt\` today, so this is latent,
not currently firing. The blast radius is a regenerable build directory, not
source — but the failure is silent and lands in another session's tree.
Related, same line: the marker is written `-Encoding ascii`, so a non-ASCII tree
path is stored with `?` substituted; that happens to still match today (`?` is a
single-char wildcard) which is luck, not correctness.

**D2 (high). `check_00` publishes an ELF next to an sdkconfig that did not build
it, and the commit mis-credits itself with fixing `check_all_task_stack_budgets`.**
The new sdkconfig fallback (lines 219-228) builds the invoking tree's source
against the MAIN tree's board-tuned sdkconfig, then publishes the resulting
`KilnCtrl.elf` into the invoking tree's `firmware/KilnFW/build/`. The ELF-reading
checks resolve their sdkconfig as `--elf/../sdkconfig` — the invoking tree's own.
Those two are never the same file. Consequences in a clean worktree:
  - no sdkconfig at all: `check_all_task_stack_budgets` and
    `check_main_task_stack_budget` fail "no sdkconfig", `check_01` skips (and a
    skip fails the run by default).
  - sdkconfig generated the way this commit's own target pin advertises:
    `CONFIG_KILNCTL_ENABLE_GPIO_PROBE` is `n` by default, but the published ELF
    was built with the main tree's `=y`, so the check fails with *"root symbol
    'gpio_probe_task' must not exist in this build -- but it resolves ... Either
    the ELF is stale ... or the #if no longer matches"*. That message points at
    two causes, neither of which is the actual one.
This is the "corrupts another agent's evidence" shape: a confusing red in a
check this commit does not touch, caused by this commit's own provisioning
fallback. **Which commit fixed `check_all_task_stack_budgets`? Neither.**
`3ec08793` added the `kconfig=` adjudication and is what produces the message
above; the target pin in `827dd887` is irrelevant to it (the check reads a
generated/copied `sdkconfig`, never `sdkconfig.defaults`). The check passes only
when the main tree's sdkconfig is hand-copied in, which is what the author's
worktree must have had. The two fixes do not conflict, but neither closes this.
Suggested direction (not applied): publish the sdkconfig actually used
alongside the ELF.

**D3 (medium). The sentinel floor is enforced but cannot detect the failure it
names.** All three sentinels are `CMakeLists.txt` files that change rarely, and
they are compared source-vs-destination. If the mirror silently no-ops, the
destination still holds an identical copy from the previous run and all three
pairs match — the assertion passes while every `.c` file is stale. It proves
"these three files agree", not "this tree's source got mirrored". Not vacuous;
just much weaker than "refusing to grade a build of source that is not this
tree's" claims.

**D4 (medium). `/XD` narrowing silently changed which directories are
excluded.** The old bare name `build` excluded *any* directory named `build` at
any depth; the absolute form excludes only the top-level one. Verified by
fixture: a nested `App\test\build` is now mirrored in. Real instances exist —
`firmware/KilnFW/App/test/build` and `firmware/CommonFW/test/build` (host-test
output, `.obj`/`.exe`) are now copied into the build worktree every run. Correct
but unintended, uncommented, and it grows the 425 MB figure.

**D5 (low). Markerless orphans are never reclaimed.** The worktree is created at
line 254 but the marker is written at line 400, after mirror and sentinels. Any
run that fails in between leaves a `checkbuild_<hex>` with no marker, which the
prune skips forever (verified in the fixture run). 425 MB each.

**D6 (low, pre-existing, now misleading).** The header at lines 102-104 says the
mutex is "shared with check_bootloader_builds.ps1". That script uses
`saftyfw_bootloader_build`; nothing else in the repo takes
`kilnfw_checkbuild_worktree`. The per-tree lock name is therefore safe (I
checked this specifically, expecting a regression, and found none), but the
comment justifying the lock is false.

## What I tried that found nothing

- **Hash collisions / recreate-at-same-path.** 40 bits over ~190 trees;
  recreating a tree at the same path reuses its directory and `/MIR` makes it
  current. No defect.
- **Main-tree name/lock drift.** Byte-identical; no in-flight run is affected.
- **Other relative paths handed to a tool that resolves against CWD.** Grepped
  every `.ps1` in the repo for `robocopy`, `/XD`, `/XF`: `check_00` is the only
  caller, and its remaining relative argument is `/XF sdkconfig`, where a bare
  filename is the intended semantics. The bug class did not travel here.
- **Prune reaching non-generated directories.** The `^checkbuild_[0-9a-f]{10}$`
  regex plus the marker requirement genuinely excludes `checkbuild`,
  `checkbuild_origin_kilnfw`, `checkbuild_origin_saftyfw` and all ~187 hand-made
  worktrees; confirmed by running the predicate over fixtures.
- **Empty / whitespace / truncated markers.** Empty is skipped; a truncated path
  deletes only that tree's own directory. No path to deleting source.
- **FAIL-reachable-as-SKIP.** Every new failure path is `Fail` (exit 1). The only
  `exit 3` is the pre-existing missing-toolchain one.
- **Mis-targeting some other board.** None exists in this repo.
- **Destroying the main `C:\wt\checkbuild`.** Ran the real suite from a linked
  worktree and confirmed afterward that its lvgl checkout and `KilnCtrl.elf`
  were intact.

## Is this area safe to stop iterating on?

Not yet. D1 and D2 both want a fix: D1 is a one-word change (`-LiteralPath`) for
a latent cross-session deletion, and D2 means a clean worktree still cannot get
an honest green without a hand-copied sdkconfig — which is the exact workflow
this commit set out to make work. The directory-per-tree change itself, the
robocopy fix and the target pin are sound and worth keeping.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
