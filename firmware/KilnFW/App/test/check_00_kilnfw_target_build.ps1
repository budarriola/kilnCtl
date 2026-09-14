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

# HOSTILE ENVIRONMENT: idf.py refuses to configure correctly (or silently
# no-ops the build, leaving a STALE elf/bin in place from a previous run) when
# MSYSTEM/MSYS-flavoured environment is inherited into the process -- e.g.
# when this script is launched from a git-bash/MSYS2 shell (the Bash tool)
# rather than native PowerShell. That no-op looks exactly like "produced no
# .bin/.elf" (or worse: reports exit 0 against artifacts that were not
# actually rebuilt). Strip it unconditionally so this check behaves the same
# no matter which shell launched it, instead of relying on every caller
# remembering to use PowerShell.
foreach ($v in @("MSYSTEM", "MSYSTEM_PREFIX", "MSYSTEM_CARCH", "MSYSTEM_CHOST", "MSYS", "MSYS2_PATH_TYPE")) {
    if (Test-Path "Env:$v") { Remove-Item "Env:$v" }
}

# The whole mirror -> build -> verify -> publish sequence operates on the ONE
# shared, persistent worktree/build dir (C:\wt\checkbuild and the main tree's
# firmware\KilnFW\build\), so the lock must span all of it, not just the
# final publish copy -- two concurrent runs building into the same worktree
# at once would race ninja/cmake exactly like the two check_bootloader_builds.ps1
# runs documented in build_lock.ps1's header, and a lock that only wrapped the
# publish step would not have prevented that.
$lock = Enter-BuildLock -Name "kilnfw_checkbuild_worktree"
try {
    Write-Host "Mirroring current firmware/KilnFW, firmware/hwAbstraction and firmware/CommonFW into $WorktreePath ..."
    Mirror-Tree (Join-Path $repoRoot "firmware\KilnFW") (Join-Path $WorktreePath "firmware\KilnFW") `
        @("build", "components\lvgl", ".git") @("sdkconfig")
    Mirror-Tree (Join-Path $repoRoot "firmware\hwAbstraction") (Join-Path $WorktreePath "firmware\hwAbstraction") `
        @(".git") @()
    # firmware/CommonFW (kilnlink) -- added 2026-09-14. The worktree's OWN git
    # checkout is pinned to whatever $headCommit existed the FIRST time this
    # persistent worktree was created (git worktree add above only runs once,
    # guarded by Test-Path $WorktreePath) and is never advanced afterward, so
    # anything reached only through the worktree's git history -- rather than
    # mirrored like KilnFW/hwAbstraction above -- silently goes stale the
    # moment CommonFW changes on a later run. This was invisible until now
    # because CommonFW rarely changed; it broke the FIRST time it did
    # (KILNLINK_PROTOCOL_VERSION 13's new kilnlink_stack_margin.c/kilnlink_
    # get_stack_margin.c, docs/audits/saftyfw_live_stack_reporting_impl_2026-
    # 09-14.md): "Cannot find source file ... CommonFW/src/kilnlink_get_
    # stack_margin.c" -- a CMake configure failure, not a compile error, so
    # it looked like a broken build rather than a stale mirror. KILNLINK_DIR
    # (firmware/KilnFW/components/kilnlink/CMakeLists.txt) resolves to
    # "$WorktreePath/firmware/CommonFW" via a relative path, which is exactly
    # this worktree's own (stale) checkout unless mirrored the same way the
    # other two trees are.
    Mirror-Tree (Join-Path $repoRoot "firmware\CommonFW") (Join-Path $WorktreePath "firmware\CommonFW") `
        @(".git") @()

    Copy-Item -Path $MainSdkconfig -Destination $WorktreeSdkconfig -Force
    $mainHash = (Get-FileHash $MainSdkconfig -Algorithm SHA256).Hash
    $worktreeHash = (Get-FileHash $WorktreeSdkconfig -Algorithm SHA256).Hash
    if ($mainHash -ne $worktreeHash) {
        Fail "sdkconfig copy did not verify (hash mismatch) -- refusing to build against an unconfirmed config"
    }

    # FRESHNESS SIGNAL, FIXED 2026-09-09 (opus review of 16f0563f).
    # ------------------------------------------------------------
    # The original check took $buildStart = Get-Date AFTER the mirror above
    # and required bin/elf mtime >= $buildStart. That is wrong on its own
    # terms: Mirror-Tree's robocopy /MIR only touches files that actually
    # changed, so a second consecutive run on an UNCHANGED tree correctly
    # updates nothing, ninja correctly relinks nothing, and the (perfectly
    # valid, current) artifacts from the PREVIOUS run predate this run's
    # $buildStart -- a wall-clock gate cannot tell that apart from the real
    # hazard (a broken toolchain silently no-op'ing despite a genuine source
    # change) because both look identical by "predates the moment this run
    # started". Comparing against wall-clock time is answering the wrong
    # question; what actually matters is whether the artifact is at least as
    # new as the newest INPUT that could affect it.
    #
    # Fix: snapshot the newest LastWriteTime among the tracked source trees
    # (Mirror-Tree already made these current relative to the main tree,
    # including any uncommitted edit) BEFORE the build runs, then after the
    # build require bin/elf mtime >= that snapshot, not >= $buildStart.
    # robocopy preserves source mtimes by default (it does not touch a
    # file's timestamp merely by copying it unchanged), so this is a genuine
    # content-derived signal, not a copy-time artifact:
    #   * Unchanged tree, second run: newest source mtime is whatever it was
    #     the last time a file actually changed (before this run even
    #     started); the existing artifacts from the prior successful run are
    #     already >= that -- PASS, correctly, no matter how long ago they
    #     were built.
    #   * A real edit landed (committed or not) and the toolchain silently
    #     no-ops anyway: the edited file's mtime is now newer than the
    #     existing (stale, unrelinked) artifacts -- FAIL, correctly, because
    #     the artifact provably does not reflect current source.
    # components\lvgl is excluded from this scan for the same reason it is
    # excluded from Mirror-Tree: it is a submodule, effectively static here,
    # and not part of "did today's edit get built".
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
        (Join-Path $WorktreePath "firmware\hwAbstraction"),
        (Join-Path $WorktreePath "firmware\CommonFW")
    )
    if (-not $newestSourceTime) {
        Fail "could not determine a newest source mtime under $WorktreePath after mirroring -- refusing to grade artifact freshness with no signal to grade it against"
    }

    & $IdfProfile *>&1 | Out-Null

    $env:CCACHE_DISABLE = "1"

    $binPath = Join-Path $WorktreePath "firmware\KilnFW\build\KilnCtrl.bin"
    $elfPath = Join-Path $WorktreePath "firmware\KilnFW\build\KilnCtrl.elf"

    Write-Host "Building KilnFW target (CCACHE_DISABLE=1) in $WorktreePath ..."
    $buildOutput = & idf.py -C (Join-Path $WorktreePath "firmware\KilnFW") build 2>&1
    $buildExit = $LASTEXITCODE

    $buildOutput | Write-Host

    if ($buildExit -ne 0) {
        Fail "idf.py build failed (exit $buildExit) -- see output above. This is exactly the class of break check_00_kilnfw_target_build.ps1 exists to catch (e.g. commit 9bc155ea's -Werror=format-truncation in readiness_http.c)."
    }

    if (-not (Test-Path $binPath) -or -not (Test-Path $elfPath)) {
        Fail "idf.py build reported success (exit 0) but $binPath / $elfPath does not exist -- refusing to report PASS without a real build artifact."
    }

    # Positive freshness check, not just existence: both artifacts' last-write
    # time must be AT OR AFTER the newest tracked source mtime captured above
    # -- see the "FRESHNESS SIGNAL" comment. A stale elf/bin that predates the
    # newest source edit (the MSYSTEM no-op, or any other silent short-
    # circuit) must FAIL loudly here rather than be mistaken for a fresh
    # build; an elf/bin that is merely older than "now" but already reflects
    # every current source file (the legitimate no-op case) must PASS.
    $binTime = (Get-Item $binPath).LastWriteTime
    $elfTime = (Get-Item $elfPath).LastWriteTime
    # Small negative tolerance for filesystem timestamp granularity/clock skew.
    $tolerance = [TimeSpan]::FromSeconds(2)
    if (($binTime -lt $newestSourceTime.Subtract($tolerance)) -or ($elfTime -lt $newestSourceTime.Subtract($tolerance))) {
        Fail "idf.py build reported success (exit 0) but $binPath (mtime $binTime) / $elfPath (mtime $elfTime) predate the newest tracked source file's mtime ($newestSourceTime) -- the build silently did not relink against current source (known cause: MSYSTEM/MSYS environment inherited from a git-bash launcher confusing idf.py, or any other silent no-op). Refusing to publish a stale artifact as current."
    }

    # Publish into the shared main-tree build/ directory so the ELF-reading
    # checks that sort after this one (check_httpd_task_stack_budget.ps1 and
    # siblings, see header) see a build that is current with the source they
    # just ran against -- not whatever was last built by hand, hours or days
    # ago.
    #
    # PUBLISH (2026-09-10, objdump-subprocess-error investigation; corrected
    # 2026-09-10 after an Opus review found the original fix's central claim
    # false):
    # the ELF-reading checks (check_uart_log_bridge_stack_budget.py,
    # check_all_task_stack_budgets.py, check_httpd/main/system_uart_bridge's
    # own checkers) do NOT take "kilnfw_checkbuild_worktree" or any other
    # lock while reading firmware\KilnFW\build\KilnCtrl.elf -- only this
    # script and check_01_kilnfw_pushed_build.ps1 do. A plain `Copy-Item
    # -Force` writes into the destination file IN PLACE, so a concurrent
    # `run_all_checks.ps1` (or a manually-launched checker) that opens
    # KilnCtrl.elf for objdump while this copy is mid-flight gets a
    # partially-written file -- objdump then fails with a subprocess error
    # that has nothing to do with the actual object code, and looks
    # indistinguishable from a real toolchain problem unless you already
    # know to suspect the race.
    #
    # CORRECTED CLAIM: copying to a sibling temp file and then calling
    # `Move-Item -Force` onto the final name is NOT atomic on Windows.
    # Windows PowerShell 5.1's `Move-Item -Force` is implemented as
    # delete-destination-then-rename, not `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`.
    # Demonstrated by reproduction: with the destination held open under
    # ordinary `FILE_SHARE_READ` (what `objdump` does), `Move-Item -Force`
    # throws `System.IO.IOException: Cannot create a file when that file
    # already exists.`, leaving the destination file UNTOUCHED and the temp
    # file orphaned -- i.e. there is a real window in which the delete
    # succeeded conceptually but didn't, and, worse, an orphaned .tmp_<pid>
    # file was left in build/ with no cleanup, which is exactly what was
    # found sitting in this tree (KilnCtrl.elf.tmp_6288, an orphan from an
    # earlier failed publish under this exact code path).
    #
    # A further reproduction shows switching to the real Win32 primitive
    # (`MoveFileEx` with `MOVEFILE_REPLACE_EXISTING`, which is what .NET's
    # `[System.IO.File]::Move(src, dst, $true)` calls on runtimes that have
    # that overload -- Windows PowerShell 5.1's .NET Framework 4.8 does NOT)
    # does not by itself make the publish atomic "from every reader's point
    # of view" either: when the destination is held open by a reader, even
    # with FILE_SHARE_READ|FILE_SHARE_DELETE granted, MoveFileEx itself
    # fails with ERROR_ACCESS_DENIED (5). True atomic replace-of-an-open-file,
    # the way POSIX rename(2) behaves, is not available on NTFS through this
    # API. So there is no implementation that makes an in-place publish safe
    # against a reader that is mid-read RIGHT NOW; the realistic goal is:
    # (a) never leave a half-written file at the destination path, (b) never
    # leave an orphaned temp file behind on failure, and (c) fail LOUDLY
    # instead of silently leaving a stale artifact in place when the publish
    # cannot complete.
    #
    # Implementation: MoveFileEx(MOVEFILE_REPLACE_EXISTING) is used instead
    # of `Move-Item -Force` because when the destination is NOT concurrently
    # held open (the overwhelmingly common case -- nothing should have the
    # published ELF open outside of the brief window an ELF-reading check is
    # actually running), it performs the replace as a single kernel call
    # rather than Move-Item's delete-then-create, so there is no window
    # where the destination path does not exist at all. If the destination
    # IS held open, a short bounded retry gives a transient reader a chance
    # to close before giving up; on final failure the temp file is removed
    # (never left orphaned) and the check FAILS naming the destination path,
    # rather than silently leaving the previous (stale) artifact in place
    # with no signal that publish did not happen this run.
    $mainBuildDir = Join-Path $repoRoot "firmware\KilnFW\build"
    New-Item -ItemType Directory -Force -Path $mainBuildDir | Out-Null

    if (-not ([System.Management.Automation.PSTypeName]"KilnFWCheck00.NativeMove").Type) {
        Add-Type -Namespace KilnFWCheck00 -Name NativeMove -MemberDefinition @"
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool MoveFileEx(string lpExistingFileName, string lpNewFileName, uint dwFlags);
"@
    }
    $MOVEFILE_REPLACE_EXISTING = 0x1
    $MOVEFILE_WRITE_THROUGH = 0x8

    function Publish-BuildArtifact {
        param([string]$SourcePath, [string]$TempPath, [string]$FinalPath)
        Copy-Item -Path $SourcePath -Destination $TempPath -Force
        $attempts = 0
        $maxAttempts = 5
        $lastErr = 0
        while ($attempts -lt $maxAttempts) {
            $ok = [KilnFWCheck00.NativeMove]::MoveFileEx($TempPath, $FinalPath, $MOVEFILE_REPLACE_EXISTING -bor $MOVEFILE_WRITE_THROUGH)
            if ($ok) { return }
            $lastErr = [System.Runtime.InteropServices.Marshal]::GetLastWin32Error()
            $attempts++
            Start-Sleep -Milliseconds 200
        }
        # Publish did not complete -- clean up the temp file so it is never
        # left as an orphan, and fail loudly instead of silently leaving the
        # previous (stale) $FinalPath in place with no signal.
        Remove-Item -Path $TempPath -Force -ErrorAction SilentlyContinue
        Fail "Could not publish $FinalPath -- MoveFileEx failed after $maxAttempts attempts with Win32 error $lastErr (5=ERROR_ACCESS_DENIED usually means another process has $FinalPath open; find and close it, e.g. a stuck objdump/nm/readelf). The stale existing $FinalPath was left untouched; refusing to report PASS with an unpublished build."
    }

    $elfTmp = Join-Path $mainBuildDir "KilnCtrl.elf.tmp_$PID"
    $binTmp = Join-Path $mainBuildDir "KilnCtrl.bin.tmp_$PID"
    Publish-BuildArtifact -SourcePath $elfPath -TempPath $elfTmp -FinalPath (Join-Path $mainBuildDir "KilnCtrl.elf")
    Publish-BuildArtifact -SourcePath $binPath -TempPath $binTmp -FinalPath (Join-Path $mainBuildDir "KilnCtrl.bin")
    Write-Host "Published fresh KilnCtrl.elf/.bin to $mainBuildDir"
} finally {
    # Belt-and-suspenders: Publish-BuildArtifact already removes its own temp
    # file on a MoveFileEx failure, but an unexpected exception elsewhere in
    # the try block (e.g. Copy-Item itself throwing) could still leave a
    # KilnCtrl.{elf,bin}.tmp_$PID orphan in build/ -- exactly the class of
    # leftover found in this tree (KilnCtrl.elf.tmp_6288) before this fix.
    # $PID is this script's own process id, so this only ever removes a temp
    # file this run itself could have created, never another process's.
    $mainBuildDirCleanup = Join-Path $repoRoot "firmware\KilnFW\build"
    Remove-Item -Path (Join-Path $mainBuildDirCleanup "KilnCtrl.elf.tmp_$PID") -Force -ErrorAction SilentlyContinue
    Remove-Item -Path (Join-Path $mainBuildDirCleanup "KilnCtrl.bin.tmp_$PID") -Force -ErrorAction SilentlyContinue
    Exit-BuildLock -Lock $lock
}

Write-Host ""
Write-Host "PASS: KilnFW target build succeeded, $binPath produced." -ForegroundColor Green
exit 0
