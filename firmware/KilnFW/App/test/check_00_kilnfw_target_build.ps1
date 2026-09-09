# check_00_kilnfw_target_build.ps1 -- the one check that actually builds
# KilnFW for the real target, not the host-test fakes.
#
# NAME/LOCATION, DELIBERATE. run_all_checks.ps1 runs every discovered
# check_*.ps1 in plain FullName sort order, all in one pass, with no other
# ordering guarantee. Four other checks in this same directory
# (check_httpd_task_stack_budget.ps1, check_main_task_stack_budget.ps1,
# check_system_uart_bridge_stack_budget.ps1,
# check_uart_log_bridge_stack_budget.ps1) read firmware/KilnFW/build/KilnCtrl.elf
# by default and only SKIP if it is missing -- none of them check whether it
# is FRESH relative to current source. 2026-09-09: a heating run aborted on
# a panic in a build that had already passed the full check suite, and
# separately check_httpd_task_stack_budget's red partly traced back to a
# STALE ELF -- it went green once KilnFW was actually rebuilt. Checks that
# read build artifacts are only as honest as the freshness of those
# artifacts. This check's "check_00_" prefix is what makes it sort before
# all four of those siblings in this same directory, and its last step
# (below) publishes its own freshly-built KilnCtrl.elf/.bin into the shared
# firmware/KilnFW/build/ directory those checks default to reading -- so a
# clean run_all_checks.ps1 pass now guarantees a current build precedes
# every ELF-reading check in the suite, not just this one.
#
# WHY THIS EXISTS. Commit 9bc155ea introduced a -Werror=format-truncation
# error in readiness_http.c that sat undetected on main until an agent tried
# to flash from a clean worktree. run_all_checks.ps1 discovers check_*.ps1 by
# glob and runs firmware/hwAbstraction/test/compile_esp_backends.ps1 /
# compile_pico_backends.ps1, but both of those CONSUME an existing
# firmware/KilnFW/build/compile_commands.json -- they never produce one, and
# neither does anything else in the check suite. So there was no clean-tree
# KilnFW target build anywhere in the automated checks, and the shared main
# tree's build/ directory (ccache-backed, built by whichever agent last
# touched it) hid the breakage: the object for the broken TU was already
# ccache-cached from before the -Werror was tripped, or simply never
# rebuilt, so nobody watching run_all_checks.ps1 ever saw it fail.
#
# WHAT "CLEAN" MEANS HERE, AND WHY IT IS NOT A FULL REBUILD EVERY RUN.
# A genuine `idf.py fullclean` + rebuild was measured (2026-09-09, this
# machine, ninja -j auto) at:
#   cold build (empty build/, fresh worktree)   ~138 s
#   incremental, one real TU edited             ~27-29 s
# 138 s is affordable for a suite that is invoked deliberately, but the
# actual hazard this check exists to catch is narrower than "any stale
# object": it is specifically CCACHE MASKING a broken translation unit
# (a hit from before the break was introduced, or from a shared tree another
# agent already "fixed" locally without committing). Ninja's own dependency
# tracking already forces a real recompile of any TU whose content or
# transitively-included headers changed -- that part of "staleness" is not
# ccache's problem and a full wipe buys nothing extra for it that
# CCACHE_DISABLE=1 does not already buy for the TUs that actually changed.
# So: this check keeps ONE persistent worktree/build dir across runs
# (avoiding the ~138 s cold-configure cost every time) and forces
# CCACHE_DISABLE=1 so ccache cannot serve a stale hit for whatever changed
# since the last run. Every run it MIRRORS (robocopy /MIR) the main tree's
# actual firmware/KilnFW and firmware/hwAbstraction directories -- including
# uncommitted edits, deliberately, not just HEAD -- into the worktree. This
# is what lets the check catch a break before it is even committed (this
# script's own negative test relies on that: a plain uncommitted edit in the
# main tree, no commit required, must turn this check red), and it is also
# a closer match to "what run_all_checks.ps1 sees right now" than a
# git-checkout-of-HEAD would be for a shared, often-dirty tree.
#
# WHAT THIS DOES NOT CATCH, STATED PLAINLY: a break that depends on a truly
# from-scratch configure (a CMakeLists/Kconfig ordering bug that only shows
# up when build/ does not exist yet), or a break in a TU that has not
# changed since the last time this check happened to run clean here. Those
# gaps are accepted in exchange for a check that finishes in well under a
# minute on the common case instead of ~2.5 minutes every single invocation
# of run_all_checks.ps1. If that trade stops being worth it, the fix is to
# force a full `idf.py fullclean` here (already measured safe at ~138 s),
# not to make this check exit 0 without having actually compiled anything.
#
# SDKCONFIG. sdkconfig is gitignored (board-specific, hand-tuned) so a fresh
# worktree has none. Regenerating one from Kconfig defaults is KNOWN to
# diverge from what the board actually runs (CLAUDE.md's "gitignored config
# hides mismatch" note -- the FT6336U/NS2009 touch panel mismatch was found
# exactly this way). So this check COPIES the main tree's sdkconfig into the
# worktree and diffs them byte-for-byte every run, refusing to build (a real
# FAIL, not a skip) if they disagree after the copy -- which would only
# happen if something raced the copy itself.
#
# SUBMODULE. firmware/KilnFW/components/lvgl is a submodule; a fresh
# worktree needs `git submodule update --init` for it before CMake can see
# lv_conf.h and friends.
#
# SKIP CONTRACT: exit 3 with a line containing "SKIP" if the ESP-IDF
# toolchain profile script is not present on this machine -- that is a
# missing prerequisite, not a defect. Anything else that stops this check
# from actually invoking the compiler (worktree setup failure, sdkconfig
# mismatch, submodule init failure) is a FAIL (non-zero, non-3), never a
# silent exit 0 -- a check that could not compile anything and still says
# PASS is the exact defect class this file exists to close.

$ErrorActionPreference = "Stop"

# Native git/idf.py output on stderr (informational lines, e.g. "From
# <path>" on a fetch) is promoted to a terminating NativeCommandError under
# "Stop" -- the same trap run_all_checks.ps1's own header documents. Every
# external-process call in this script runs under "Continue" and checks
# $LASTEXITCODE explicitly instead.
$ErrorActionPreference = "Continue"

# Shared with check_bootloader_builds.ps1: a global named mutex so that
# publishing into the shared firmware/KilnFW/build/ directory below never
# races another concurrent build/check touching that same tree.
. (Join-Path $PSScriptRoot "..\..\..\..\tools\build_lock.ps1")

# This file lives at firmware/KilnFW/App/test/ -- four levels below repo root.
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")
$IdfProfile = "C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"

if (-not (Test-Path $IdfProfile)) {
    Write-Host "SKIP: ESP-IDF profile script not found at $IdfProfile -- toolchain not installed on this machine" -ForegroundColor Yellow
    exit 3
}

# Short path is required: the default .claude\worktrees\... path overflows
# the MSVC/xtensa command line on this project (see CLAUDE.md). Persistent
# across runs on purpose -- see the cost discussion above.
$WorktreePath = "C:\wt\checkbuild"
$MainSdkconfig = Join-Path $repoRoot "firmware\KilnFW\sdkconfig"
$WorktreeSdkconfig = Join-Path $WorktreePath "firmware\KilnFW\sdkconfig"

function Fail([string]$msg) {
    Write-Host ""
    Write-Host "FAILED: $msg" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $MainSdkconfig)) {
    Fail "main tree has no firmware\KilnFW\sdkconfig -- this is the board-tuned config this check must not regenerate from Kconfig defaults; run the IDE workspace setup or copy a known-good sdkconfig into the main tree first."
}

$headCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
if (-not $headCommit) {
    Fail "could not resolve HEAD in $repoRoot"
}

if (-not (Test-Path $WorktreePath)) {
    Write-Host "Setting up persistent build worktree at $WorktreePath (first run) ..."
    & git -C $repoRoot worktree add $WorktreePath $headCommit 2>&1 | Write-Host
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

# Mirror the main tree's actual firmware/KilnFW and firmware/hwAbstraction
# directories into the worktree -- this is what actually makes the build
# "current" every run, not the worktree's own git history. lvgl (a
# submodule, static, cloned once above) and build/ (gitignored, persistent,
# the whole point of keeping it) are excluded so this never touches them;
# sdkconfig is excluded here because it is copied and hash-verified
# separately below, not mirrored blindly.
function Mirror-Tree([string]$src, [string]$dst, [string[]]$excludeDirs, [string[]]$excludeFiles) {
    $args = @($src, $dst, "/MIR", "/NFL", "/NDL", "/NJH", "/NJS", "/NP")
    if ($excludeDirs.Count -gt 0) { $args += "/XD"; $args += $excludeDirs }
    if ($excludeFiles.Count -gt 0) { $args += "/XF"; $args += $excludeFiles }
    & robocopy @args | Out-Null
    # robocopy exit codes 0-7 are all success variants; 8+ is a real error.
    if ($LASTEXITCODE -ge 8) {
        Fail "robocopy mirror of $src failed (exit $LASTEXITCODE)"
    }
}

Write-Host "Mirroring current firmware/KilnFW and firmware/hwAbstraction into $WorktreePath ..."
Mirror-Tree (Join-Path $repoRoot "firmware\KilnFW") (Join-Path $WorktreePath "firmware\KilnFW") `
    @("build", "components\lvgl", ".git") @("sdkconfig")
Mirror-Tree (Join-Path $repoRoot "firmware\hwAbstraction") (Join-Path $WorktreePath "firmware\hwAbstraction") `
    @(".git") @()

Copy-Item -Path $MainSdkconfig -Destination $WorktreeSdkconfig -Force
$mainHash = (Get-FileHash $MainSdkconfig -Algorithm SHA256).Hash
$worktreeHash = (Get-FileHash $WorktreeSdkconfig -Algorithm SHA256).Hash
if ($mainHash -ne $worktreeHash) {
    Fail "sdkconfig copy did not verify (hash mismatch) -- refusing to build against an unconfirmed config"
}

& $IdfProfile *>&1 | Out-Null

$env:CCACHE_DISABLE = "1"

Write-Host "Building KilnFW target (CCACHE_DISABLE=1) in $WorktreePath ..."
$buildOutput = & idf.py -C (Join-Path $WorktreePath "firmware\KilnFW") build 2>&1
$buildExit = $LASTEXITCODE

$buildOutput | Write-Host

if ($buildExit -ne 0) {
    Fail "idf.py build failed (exit $buildExit) -- see output above. This is exactly the class of break check_00_kilnfw_target_build.ps1 exists to catch (e.g. commit 9bc155ea's -Werror=format-truncation in readiness_http.c)."
}

$binPath = Join-Path $WorktreePath "firmware\KilnFW\build\KilnCtrl.bin"
$elfPath = Join-Path $WorktreePath "firmware\KilnFW\build\KilnCtrl.elf"
if (-not (Test-Path $binPath) -or -not (Test-Path $elfPath)) {
    Fail "idf.py build reported success (exit 0) but $binPath / $elfPath does not exist -- refusing to report PASS without a real build artifact."
}

# Publish into the shared main-tree build/ directory so the ELF-reading
# checks that sort after this one (check_httpd_task_stack_budget.ps1 and
# siblings, see header) see a build that is current with the source they
# just ran against -- not whatever was last built by hand, hours or days
# ago. Locked because the shared tree may have another build/check running
# against the same directory concurrently.
$mainBuildDir = Join-Path $repoRoot "firmware\KilnFW\build"
$lock = Enter-BuildLock -Name "kilnfw_main_build_dir_publish"
try {
    New-Item -ItemType Directory -Force -Path $mainBuildDir | Out-Null
    Copy-Item -Path $elfPath -Destination (Join-Path $mainBuildDir "KilnCtrl.elf") -Force
    Copy-Item -Path $binPath -Destination (Join-Path $mainBuildDir "KilnCtrl.bin") -Force
    Write-Host "Published fresh KilnCtrl.elf/.bin to $mainBuildDir"
} finally {
    Exit-BuildLock -Lock $lock
}

Write-Host ""
Write-Host "PASS: KilnFW target build succeeded, $binPath produced." -ForegroundColor Green
exit 0
