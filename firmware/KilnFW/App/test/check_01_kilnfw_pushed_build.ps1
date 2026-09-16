# check_01_kilnfw_pushed_build.ps1 -- builds KilnFW against the ACTUAL
# content of origin/main, not the local working tree.
#
# WHY THIS EXISTS, DISTINCT FROM check_00_kilnfw_target_build.ps1.
# check_00 (this directory, sorts before this file) deliberately mirrors the
# local working tree -- including uncommitted edits -- via robocopy, so it
# can catch a break before it is even committed. That is exactly right for
# "does my current tree build", but it answers nothing about what is
# actually pushed: a session's local checkout can be behind, ahead of, or
# flatly divergent from origin/main. On 2026-09-10, origin/main broke at
# least four times the same way -- something got pushed whose dependency
# closure was not fully committed (a header without its consumer, a struct
# field used before the struct change landed) -- and every one of those
# breaks would have sailed straight through check_00 on the pushing
# session's own machine, because that session's local tree (with the
# not-yet-committed other half of the change still sitting in the working
# copy) built just fine. Nobody built bare origin/main before the next
# session pulled it and got a blocked build -- once on the critical path
# for diagnosing a live hardware panic.
#
# WHAT THIS CHECKS: `git fetch origin main`, then a real `idf.py build`
# against a worktree checked out EXACTLY at that fetched ref -- no
# uncommitted edits, no local-tree mirroring of tracked files.
#
# ONE DELIBERATE EXCEPTION, and the claim above is overstated without saying
# so (2026-09-10, opus review round 2): `sdkconfig` is gitignored (see
# docs/CONFIG_FILESYSTEM.md and the "gitignored config hides mismatch"
# lesson elsewhere in this repo's history) and is NOT part of what a fresh
# `git pull` on origin/main hands another session at all -- ESP-IDF cannot
# build without one, so this check copies a real board-tuned sdkconfig into
# the worktree below (hash-verified after the copy) rather than fabricating
# a default one: the invoking tree's own if it has one, otherwise the main
# worktree's (see the seed-resolution block further down -- this paragraph
# claimed "the MAIN TREE's" while the code in fact read only the invoking
# tree's and skipped outright when it was absent). That means this check does NOT actually prove
# "a fresh clone of origin/main builds" -- it proves "origin/main's SOURCE
# builds against whatever board config happens to be sitting in the machine
# running this check". A push that changes a Kconfig default, or that
# depends on a config symbol this machine's sdkconfig happens to set but a
# fresh `idf.py menuconfig` would not, still passes here even though it
# would not build cleanly for another session starting from scratch.
# Seeding from somewhere is unavoidable (a bare origin/main checkout has no
# sdkconfig at all, and idf.py cannot proceed without one) and the main
# tree's own committed-adjacent, board-tuned config is the least-wrong
# available source -- but the exception is real, not merely theoretical:
# see feedback_gitignored_config_hides_mismatch.md's FT6336U incident. A
# stronger version of this check would build against a MINIMAL/default
# sdkconfig (`idf.py set-target` + whatever `sdkconfig.defaults` this repo
# commits, if any) as a SECOND, separate run, specifically to catch a
# Kconfig-default drift this seeded-config run cannot see by construction --
# not attempted here today; flagged as a real gap, not "not applicable".
#
# FAILURE MESSAGE CONTRACT: this check's FAILED text always says
# "origin/main (commit <sha>) does not build" and points at investigating
# what got pushed -- never "your tree does not build". check_00's failure
# text says the opposite ("uncommitted/local tree does not build"). Anyone
# reading a red run_all_checks.ps1 must be able to tell which situation
# they are in from the message alone: a red check_00 means "fix your
# working copy before you push"; a red check_01 means "origin/main itself
# is broken right now, stop pulling it until it is fixed".
#
# COST DECISION: run every time, unconditionally, not gated on "local HEAD
# differs from origin/main". A dedicated, persistent worktree checked out
# at origin/main is used (separate from check_00's C:\wt\checkbuild, which
# tracks the local tree) so this is a real `git fetch` + `git reset --hard`
# to the fetched ref each run, then an INCREMENTAL idf.py build -- ninja's
# own dependency tracking handles the "nothing changed since last run"
# case in a few seconds, same as check_00's steady-state cost. Only the
# very first run on a machine (or after a long gap with many commits
# landed) pays anything close to the ~138 s cold-build number; the common
# case measured here is close to check_00's own 27-29 s incremental figure.
# Gating on "HEAD != origin/main" was considered and rejected: it would
# only catch a break the FIRST time a session's HEAD lags origin/main by
# exactly the broken commit, and would go silent again the moment that
# session (or any other) updates its local HEAD to match -- exactly the
# gap that let today's breaks through in the first place, since the whole
# point is to catch a bad push independent of what any one session's HEAD
# happens to be.
#
# WHAT THIS DOES NOT CATCH: a break introduced and then fixed by a second
# push before this check next runs (this only ever sees the current tip of
# origin/main, not history in between); anything check_00 already doesn't
# catch (a from-scratch-configure-only bug, since this worktree's build/
# is also persistent/incremental, not wiped every run).
#
# SKIP CONTRACT: exit 3 with a line containing "SKIP" if there is no
# network path to `git fetch origin`, no `origin` remote configured, or the
# ESP-IDF toolchain profile script is not present. Any other failure to
# actually invoke the compiler (worktree setup, sdkconfig mismatch,
# submodule init) is a FAIL, never a silent PASS -- same contract as
# check_00.
#
# A MISSING sdkconfig IS NOT ON THAT LIST, deliberately (2026-09-16). It
# used to be, in the code but never in this paragraph -- the code exited 3
# for it while this contract described only the three prerequisites above.
# Since sdkconfig is gitignored that skip fired in every clean worktree,
# which is where this project verifies every commit, and a SKIP fails the
# whole run_all_checks.ps1 run by default. It is now resolved against the
# main worktree (see the seed-resolution block below) and, if genuinely
# absent everywhere on the machine, is a FAIL: an absent board config is a
# provisioning defect somebody must fix, not an absent toolchain to shrug
# at.

$ErrorActionPreference = "Stop"
$ErrorActionPreference = "Continue"

. (Join-Path $PSScriptRoot "..\..\..\..\tools\build_lock.ps1")

# -LiteralPath: Resolve-Path glob-expands otherwise, so a tree path containing
# [ or ] would fail to resolve here. This matters more than cosmetically now
# that $repoRoot.Path is compared against the main worktree's path below.
$repoRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..\..")
$IdfProfile = "C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"

function Fail([string]$msg) {
    Write-Host ""
    Write-Host "FAILED: origin/main does not build -- $msg" -ForegroundColor Red
    Write-Host "        This is a PUSHED-REF failure, not a local-tree failure: what is" -ForegroundColor Red
    Write-Host "        actually on origin/main right now fails to build KilnFW. Do not" -ForegroundColor Red
    Write-Host "        pull/base new work on origin/main until this is fixed there." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $IdfProfile)) {
    Write-Host "SKIP: ESP-IDF profile script not found at $IdfProfile -- toolchain not installed on this machine" -ForegroundColor Yellow
    exit 3
}

# SDKCONFIG SEED RESOLUTION (2026-09-16).
# ---------------------------------------
# Until this revision this block looked ONLY at the INVOKING tree's
# firmware\KilnFW\sdkconfig and exited 3 (SKIP) when it was absent -- while
# saying "in the main tree", which is not the path it tested. Two problems,
# both real:
#
#   * sdkconfig is gitignored, so a fresh worktree never has one. A clean
#     worktree at origin/main is where every commit in this repo is actually
#     verified, so this check SKIPped in exactly the situation it matters
#     most -- and run_all_checks.ps1 fails the whole run on a SKIP by
#     default, so an otherwise-green verification run exited non-zero and
#     had to be explained away by hand every time.
#   * The message sent the reader to the main tree to look for a file whose
#     absence had been detected somewhere else entirely.
#
# Fix, the same one check_00_kilnfw_target_build.ps1 already makes for its
# own identical need: prefer the invoking tree's own sdkconfig, fall back to
# the MAIN worktree's board-tuned config and SAY SO, and if neither exists
# FAIL rather than SKIP.
#
# WHY THE FALLBACK IS HONEST HERE SPECIFICALLY, not merely precedented.
# This check's build worktree is reset --hard to the fetched origin/main
# every run, so the SOURCE it compiles is byte-for-byte the pushed ref no
# matter which tree invoked the check -- the invoking tree contributes
# nothing to it. The seed config is the single exception, and the header
# above already documents at length that this check proves "origin/main's
# source builds against whatever board config is sitting on the machine
# running this check", not "a fresh clone builds". Reaching that same
# machine's same board-tuned file by a second path when the invoking tree
# is a fresh worktree does not widen that exception by one bit: it is the
# identical file, and the alternative on offer is not a stricter check but
# no check at all. The genuine gap -- a Kconfig-default drift a seeded
# config cannot see -- is unchanged by this, and is still flagged unfixed
# in the header's ONE DELIBERATE EXCEPTION paragraph.
#
# Regenerating a config from Kconfig defaults instead is NOT an option, and
# this is the reason a pinned-defaults answer was rejected rather than
# merely not chosen: sdkconfig.defaults pins no CONFIG_IDF_TARGET, so a
# regenerated config silently targets plain esp32 rather than esp32s3 and
# dies on esp32s3-only code -- it would convert this check from a skip into
# a red herring.
$repoRootFull = ([System.IO.Path]::GetFullPath($repoRoot.Path)).TrimEnd('\')
$mainWorktreeRaw = (((& git -C $repoRoot worktree list --porcelain) | Select-Object -First 1) -replace '^worktree\s+', '').Trim()
if (-not $mainWorktreeRaw) {
    Fail "could not determine the main worktree path ('git worktree list --porcelain' produced nothing for $repoRoot) -- refusing to guess where the board-tuned sdkconfig seed lives."
}
$mainWorktreeFull = ([System.IO.Path]::GetFullPath($mainWorktreeRaw.Replace('/', '\'))).TrimEnd('\')

$MainSdkconfig = Join-Path $repoRoot "firmware\KilnFW\sdkconfig"
if (-not (Test-Path -LiteralPath $MainSdkconfig)) {
    $fallbackSdkconfig = Join-Path $mainWorktreeFull "firmware\KilnFW\sdkconfig"
    if (Test-Path -LiteralPath $fallbackSdkconfig) {
        Write-Host "NOTE: $repoRootFull has no firmware\KilnFW\sdkconfig (gitignored; absent in a fresh worktree) -- seeding this build from the main worktree's board-tuned config at $fallbackSdkconfig" -ForegroundColor Yellow
        $MainSdkconfig = $fallbackSdkconfig
    } else {
        Fail "no firmware\KilnFW\sdkconfig in the invoking tree ($repoRootFull) and none in the main worktree ($mainWorktreeFull) either. This is the board-tuned config this check must not regenerate from Kconfig defaults (sdkconfig.defaults pins no CONFIG_IDF_TARGET, so a regenerated config would silently target esp32, not esp32s3). Run the IDE workspace setup, or copy a known-good sdkconfig into the main tree, before this check can say anything truthful about whether origin/main builds."
    }
}

# Fetch origin/main. A network-absent machine or a repo with no `origin`
# remote is a missing prerequisite -- SKIP, not FAIL.
$fetchOutput = & git -C $repoRoot fetch origin main 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "SKIP: 'git fetch origin main' failed (exit $LASTEXITCODE) -- no network path to origin, or no such remote/branch. Output:" -ForegroundColor Yellow
    $fetchOutput | Write-Host
    exit 3
}

$originSha = (& git -C $repoRoot rev-parse origin/main).Trim()
if (-not $originSha) {
    Write-Host "SKIP: could not resolve origin/main after fetch -- no such ref" -ForegroundColor Yellow
    exit 3
}

$WorktreePath = "C:\wt\checkbuild_origin_kilnfw"
$WorktreeSdkconfig = Join-Path $WorktreePath "firmware\KilnFW\sdkconfig"

if (-not (Test-Path $WorktreePath)) {
    Write-Host "Setting up persistent origin/main build worktree at $WorktreePath (first run) ..."
    & git -C $repoRoot worktree add --detach $WorktreePath $originSha 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Fail "git worktree add failed (exit $LASTEXITCODE)"
    }
}

$lvglGitFile = Join-Path $WorktreePath "firmware\KilnFW\components\lvgl\.git"
if (-not (Test-Path $lvglGitFile)) {
    Write-Host "Initializing lvgl submodule in worktree ..."
    & git -C $WorktreePath submodule update --init firmware/KilnFW/components/lvgl 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Fail "git submodule update --init for lvgl failed (exit $LASTEXITCODE)"
    }
}

foreach ($v in @("MSYSTEM", "MSYSTEM_PREFIX", "MSYSTEM_CARCH", "MSYSTEM_CHOST", "MSYS", "MSYS2_PATH_TYPE")) {
    if (Test-Path "Env:$v") { Remove-Item "Env:$v" }
}

$lock = Enter-BuildLock -Name "kilnfw_checkbuild_origin_worktree"
try {
    Write-Host "Checking out origin/main ($originSha) in $WorktreePath, discarding any prior state there ..."
    # This worktree is dedicated to this check alone -- nothing else ever
    # writes into it deliberately -- so a hard reset to the fetched ref is
    # safe and is the whole point: this must be EXACTLY origin/main, no
    # local-tree contamination of any kind, unlike check_00's mirror step.
    & git -C $WorktreePath fetch origin main 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Fail "git fetch inside worktree failed (exit $LASTEXITCODE)"
    }
    & git -C $WorktreePath checkout --detach $originSha 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Fail "git checkout $originSha in worktree failed (exit $LASTEXITCODE)"
    }
    & git -C $WorktreePath reset --hard $originSha 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Fail "git reset --hard $originSha in worktree failed (exit $LASTEXITCODE)"
    }
    & git -C $WorktreePath clean -fdx -e build -e "components/lvgl" -e sdkconfig 2>&1 | Write-Host

    Copy-Item -Path $MainSdkconfig -Destination $WorktreeSdkconfig -Force
    $mainHash = (Get-FileHash $MainSdkconfig -Algorithm SHA256).Hash
    $worktreeHash = (Get-FileHash $WorktreeSdkconfig -Algorithm SHA256).Hash
    if ($mainHash -ne $worktreeHash) {
        Fail "sdkconfig copy did not verify (hash mismatch) -- refusing to build against an unconfirmed config"
    }

    # Same freshness signal as check_00: newest tracked-source mtime versus
    # produced-artifact mtime, not wall-clock -- a legitimate incremental
    # no-op (nothing changed since the last time this worktree was built at
    # this same sha) must PASS; a silent no-relink after a real change must
    # FAIL. git checkout/reset only touches the mtime of files that actually
    # changed content, so this is a genuine content-derived signal here too.
    function Get-NewestSourceTime([string[]] $roots) {
        $newest = $null
        foreach ($root in $roots) {
            if (-not (Test-Path $root)) { continue }
            $candidate = Get-ChildItem -Path $root -Recurse -File -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -notmatch '\\build\\' -and $_.FullName -notmatch '\\components\\lvgl\\' } |
                Measure-Object -Property LastWriteTime -Maximum
            if ($candidate.Maximum -and (-not $newest -or $candidate.Maximum -gt $newest)) {
                $newest = $candidate.Maximum
            }
        }
        return $newest
    }

    $newestSourceTime = Get-NewestSourceTime @(
        (Join-Path $WorktreePath "firmware\KilnFW"),
        (Join-Path $WorktreePath "firmware\hwAbstraction")
    )
    if (-not $newestSourceTime) {
        Fail "could not determine a newest source mtime under $WorktreePath after checkout -- refusing to grade artifact freshness with no signal to grade it against"
    }

    & $IdfProfile *>&1 | Out-Null

    # Unlike check_00, ccache is left ENABLED here on purpose: this worktree
    # only ever holds exact, reproducible git content (never uncommitted
    # edits), so a ccache hit here is keyed on real, committed source bytes
    # and cannot mask an uncommitted-vs-committed mismatch the way check_00's
    # mixed mirror could.
    Remove-Item Env:CCACHE_DISABLE -ErrorAction SilentlyContinue

    $binPath = Join-Path $WorktreePath "firmware\KilnFW\build\KilnCtrl.bin"
    $elfPath = Join-Path $WorktreePath "firmware\KilnFW\build\KilnCtrl.elf"

    Write-Host "Building KilnFW target against origin/main ($originSha) in $WorktreePath ..."
    $buildOutput = & idf.py -C (Join-Path $WorktreePath "firmware\KilnFW") build 2>&1
    $buildExit = $LASTEXITCODE

    $buildOutput | Write-Host

    if ($buildExit -ne 0) {
        Fail "idf.py build failed (exit $buildExit) against origin/main commit $originSha -- see output above."
    }

    if (-not (Test-Path $binPath) -or -not (Test-Path $elfPath)) {
        Fail "idf.py build reported success (exit 0) but $binPath / $elfPath does not exist -- refusing to report PASS without a real build artifact."
    }

    $binTime = (Get-Item $binPath).LastWriteTime
    $elfTime = (Get-Item $elfPath).LastWriteTime
    $tolerance = [TimeSpan]::FromSeconds(2)
    if (($binTime -lt $newestSourceTime.Subtract($tolerance)) -or ($elfTime -lt $newestSourceTime.Subtract($tolerance))) {
        Fail "idf.py build reported success (exit 0) but $binPath (mtime $binTime) / $elfPath (mtime $elfTime) predate the newest tracked source file's mtime ($newestSourceTime) -- the build silently did not relink against current origin/main source. Refusing to treat a stale artifact as proof origin/main builds."
    }
} finally {
    Exit-BuildLock -Lock $lock
}

Write-Host ""
Write-Host "PASS: origin/main (commit $originSha) builds KilnFW target cleanly." -ForegroundColor Green
exit 0
