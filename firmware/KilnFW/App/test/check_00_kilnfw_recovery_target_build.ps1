# check_00_kilnfw_recovery_target_build.ps1 -- builds the standalone
# firmware/KilnFW_recovery IDF project so tools/check_recovery_image_size.py
# always has a real recovery.bin to measure on any machine that has the
# ESP-IDF toolchain.
#
# WHY THIS EXISTS (gap found 2026-09-17). check_recovery_image_size.py has
# had SKIP-on-purpose baked into it since it was written (48945291): as of
# 2026-09-16 neither the `recovery` partition row nor the recovery IDF
# project existed, so there was genuinely nothing to measure. Both have
# since landed (partition row: e88bd9ee's plan section 1/step 3;
# firmware/KilnFW_recovery/ itself: 5e07eeca, which also built recovery.bin
# once by hand in that session's own build tree and confirmed the size gate
# PASS). But nothing in provisioning (tools/setup.ps1) or in the check suite
# ever built firmware/KilnFW_recovery/build/recovery.bin, so that PASS lived
# only in the session that produced it -- every fresh clone still has no
# recovery.bin, check_recovery_image_size.py still legitimately reports its
# second documented SKIP reason ("recovery row exists, no recovery.bin
# built yet"), and run_all_checks.ps1's "any SKIP fails the run" policy
# (2026-09-15, adopted after a silent SKIP once hid a real stack-budget
# overrun) means a fresh clone could never reach a green run. The recovery
# image is real, working, buildable source in this tree -- the honest fix is
# to build it, the same way check_00_kilnfw_target_build.ps1 builds the main
# application, not to carve out a softer verdict for the size gate.
#
# WHY THIS IS SIMPLER THAN check_00_kilnfw_target_build.ps1. That script's
# bulk is: (a) copying the MAIN board's gitignored, hand-tuned sdkconfig,
# because a Kconfig-default config for the main app is known to diverge from
# what the board runs (CLAUDE.md's "gitignored config hides mismatch"); (b)
# the lvgl submodule; (c) elaborate mirror-freshness proofs sized for a
# ~1063-file, frequently-hand-edited tree with a persistent incremental
# build cached across runs. None of that applies here:
#   * KilnFW_recovery's sdkconfig is NOT gitignored config drift risk -- its
#     sdkconfig.defaults IS the whole configuration (docs/
#     OTA_SINGLE_SLOT_PLAN.md section 3: deliberately minimal, no
#     board-specific tuning, same for every board). idf.py regenerates
#     sdkconfig from it on every configure; there is nothing to diverge from
#     in the sense of "which board" -- but this project's build\ directory
#     is still persistent/incremental like the main target's, so it shares
#     that check's stale-cached-build-config risk if sdkconfig.defaults
#     itself changes (a Kconfig-gated compile-time branch could still build
#     against an old cached configure). 2026-09-19 added the same
#     hash-tracked reconfigure guard here as check_00_kilnfw_target_build.ps1
#     uses for the main sdkconfig -- see "STALE CACHED BUILD CONFIG GUARD"
#     below.
#   * No submodule dependency (no LVGL, no touch driver). PSRAM is enabled
#     but optional (CONFIG_SPIRAM_IGNORE_NOTFOUND), so the build needs no board.
#   * The whole project is ~14 files. A full `idf.py build` here has been
#     observed to run well under the main target's ~138s cold build (no
#     LVGL/PSRAM bring-up), so this check does a plain always-fresh build
#     into its own per-tree external directory rather than chasing
#     incremental-cache correctness for a project this small.
#
# SKIP CONTRACT: exit 3 with a line containing "SKIP" only if the ESP-IDF
# toolchain profile script is absent -- a missing prerequisite, not a
# defect, same category check_00_kilnfw_target_build.ps1 already uses. Any
# other failure to produce a real recovery.bin (build error, missing
# artifact, stale artifact) is a FAIL, never a silent PASS.
#
# ISOLATION: builds in a per-tree external directory under C:\wt\ (a global
# named mutex from build_lock.ps1 serializes concurrent runs against the
# same directory), then publishes into this tree's own
# firmware/KilnFW_recovery/build/ -- so two concurrent worktrees on this
# shared machine never build into or read each other's in-progress output,
# matching the isolation pattern check_00_kilnfw_target_build.ps1 already
# established for the main application build.

$ErrorActionPreference = "Stop"
$ErrorActionPreference = "Continue"

. (Join-Path $PSScriptRoot "..\..\..\..\tools\build_lock.ps1")
. (Join-Path $PSScriptRoot "..\..\..\..\tools\build_gate.ps1")

$repoRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..\..")
$repoRootFull = ([System.IO.Path]::GetFullPath($repoRoot.Path)).TrimEnd('\')
$IdfProfile = "C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"

function Fail([string]$msg) {
    Write-Host ""
    Write-Host "FAILED: $msg" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path -LiteralPath $IdfProfile)) {
    Write-Host "SKIP: ESP-IDF profile script not found at $IdfProfile -- toolchain not installed on this machine" -ForegroundColor Yellow
    exit 3
}

$srcRoot = Join-Path $repoRoot "firmware\KilnFW_recovery"
if (-not (Test-Path -LiteralPath $srcRoot)) {
    Fail "firmware\KilnFW_recovery not found under $repoRoot -- has it moved? tools\check_recovery_image_size.py's DEFAULT_RECOVERY_BIN and this check both assume this path."
}

# Per-tree external build directory, same naming scheme
# check_00_kilnfw_target_build.ps1 uses for its own persistent worktree, so
# two different trees on this machine never collide.
$sha = [System.Security.Cryptography.SHA256]::Create()
try {
    $hashBytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($repoRootFull.ToLowerInvariant()))
} finally {
    $sha.Dispose()
}
$treeTag = (($hashBytes | ForEach-Object { $_.ToString("x2") }) -join "").Substring(0, 10)
$WorktreePath = "C:\wt\checkbuild_recovery_$treeTag"
$LockName = "kilnfw_recovery_checkbuild_worktree_$treeTag"
$MarkerFile = Join-Path $WorktreePath ".checkbuild_source"

Write-Host "Invoking tree:   $repoRootFull"
Write-Host "Build directory: $WorktreePath"

# PRUNE STALE PER-TREE DIRECTORIES (2026-09-19). Until now this directory was
# reused across runs from the same tree (good -- no thrash) but NEVER cleaned
# up once the owning tree (a throwaway agent worktree under C:\wt\) was
# removed, because nothing recorded which tree owned which directory. 29 such
# directories (~4.8 GB) were found accumulated before this fix. This mirrors
# check_00_kilnfw_target_build.ps1's own prune pass exactly: only a directory
# matching this script's own naming scheme AND carrying this script's own
# ownership marker is ever considered, and only once its named owner tree no
# longer exists on disk, and only after taking that owner's own per-tree
# build lock (zero timeout) so a run still in progress is never deleted out
# from under itself.
foreach ($stale in (Get-ChildItem -LiteralPath "C:\wt" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -cmatch '^checkbuild_recovery_[0-9a-f]{10}$' })) {
    if ([string]::Equals($stale.FullName, $WorktreePath, [System.StringComparison]::OrdinalIgnoreCase)) { continue }
    $staleMarker = Join-Path $stale.FullName ".checkbuild_source"
    if (-not (Test-Path -LiteralPath $staleMarker)) { continue }
    $owner = $null
    try { $owner = [System.IO.File]::ReadAllText($staleMarker, [System.Text.Encoding]::UTF8) } catch { $owner = $null }
    if (-not $owner) { continue }
    $owner = $owner.Trim()
    if ([System.IO.Directory]::Exists($owner) -or [System.IO.File]::Exists($owner)) { continue }

    $staleTag = $stale.Name.Substring("checkbuild_recovery_".Length)
    $staleMutex = New-Object System.Threading.Mutex($false, (Get-BuildLockMutexName -Name "kilnfw_recovery_checkbuild_worktree_$staleTag"))
    $staleHeld = $false
    try {
        try {
            $staleHeld = $staleMutex.WaitOne(0)
        } catch [System.Threading.AbandonedMutexException] {
            $staleHeld = $true
        }
        if (-not $staleHeld) {
            Write-Host "Not pruning $($stale.FullName): its owning tree '$owner' is gone, but its build lock is currently HELD -- a run is still using that directory. Leaving it for a later invocation." -ForegroundColor Yellow
            continue
        }
        if ([System.IO.Directory]::Exists($owner) -or [System.IO.File]::Exists($owner)) {
            Write-Host "Not pruning $($stale.FullName): its owning tree '$owner' reappeared between the first check and acquiring its build lock." -ForegroundColor Yellow
            continue
        }
        Write-Host "Pruning stale build directory $($stale.FullName) -- its tree '$owner' no longer exists"
        Remove-Item -LiteralPath $stale.FullName -Recurse -Force -ErrorAction SilentlyContinue
    } finally {
        if ($staleHeld) {
            try { $staleMutex.ReleaseMutex() } catch { }
        }
        $staleMutex.Dispose()
    }
}

$buildGate = Enter-KilnBuildGate -Label "kilnfw_recovery_target_build"
try {
# Enter-BuildLock is INSIDE the gate's try (opus review A5): if it throws
# before its own try block starts, the gate is still released by the outer
# finally below -- a flat gate/lock/try/finally chain would leak the gate
# slot forever in that case.
$lock = Enter-BuildLock -Name $LockName
try {
    New-Item -ItemType Directory -Force -Path $WorktreePath | Out-Null

    # WRITE THE OWNERSHIP MARKER IMMEDIATELY, under the lock, the moment the
    # directory is known to exist -- same rationale as
    # check_00_kilnfw_target_build.ps1's D5 fix: a run that dies between
    # directory creation and this write would otherwise leave an unmarked,
    # unreclaimable directory forever (the prune above skips markerless
    # directories by design). UTF-8, no BOM, so any path NTFS can name
    # round-trips through the marker exactly.
    try {
        [System.IO.File]::WriteAllText($MarkerFile, $repoRootFull, (New-Object System.Text.UTF8Encoding($false)))
    } catch {
        Fail "could not write the ownership marker $MarkerFile ($($_.Exception.Message)) -- refusing to continue, because an unmarked build directory is never reclaimed by the prune pass above and would leak disk permanently."
    }

    $dstRoot = Join-Path $WorktreePath "KilnFW_recovery"

    # Mirror current source (uncommitted edits included, same rationale as
    # check_00_kilnfw_target_build.ps1: this check exists to catch a break
    # before it is even committed). build\ is excluded so the destination's
    # own build output is never clobbered by the mirror.
    $roboArgs = @($srcRoot, $dstRoot, "/MIR", "/NFL", "/NDL", "/NJH", "/NJS", "/NP",
        "/XD", (Join-Path $srcRoot "build"), (Join-Path $dstRoot "build"))
    & robocopy @roboArgs | Out-Null
    if ($LASTEXITCODE -ge 8) {
        Fail "robocopy mirror of $srcRoot failed (exit $LASTEXITCODE)"
    }

    # components/kilnlink links firmware/CommonFW's framing sources by the
    # relative path ../../../CommonFW (sibling of KilnFW_recovery in the real
    # tree), so the build directory needs the same sibling. Only src/ and
    # include/ are read by that component.
    $commonSrc = Join-Path $repoRoot "firmware\CommonFW"
    $commonDst = Join-Path $WorktreePath "CommonFW"
    & robocopy $commonSrc $commonDst "/MIR" "/NFL" "/NDL" "/NJH" "/NJS" "/NP" "/XD" (Join-Path $commonSrc "test") (Join-Path $commonSrc "docs") | Out-Null
    if ($LASTEXITCODE -ge 8) {
        Fail "robocopy mirror of $commonSrc failed (exit $LASTEXITCODE)"
    }

    # Full-content verification: this project is small (~14 files today), so
    # a full hash comparison is cheap and leaves no sampling gap -- unlike
    # check_00_kilnfw_target_build.ps1's tree, there is no cost reason to
    # settle for a witness/sample here.
    $verified = 0
    $srcFiles = Get-ChildItem -LiteralPath $srcRoot -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\build\\' }
    foreach ($f in $srcFiles) {
        $rel = $f.FullName.Substring($srcRoot.ToString().Length).TrimStart('\')
        $dstFile = Join-Path $dstRoot $rel
        if (-not (Test-Path -LiteralPath $dstFile)) {
            Fail "the mirror step did not deliver $dstFile -- $rel exists in the invoking tree but not in the build directory."
        }
        if ((Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $dstFile -Algorithm SHA256).Hash) {
            Fail "after mirroring, $dstFile still differs from the invoking tree's $rel -- refusing to grade a build of source that is not this tree's."
        }
        $verified++
    }
    if ($verified -lt 5) {
        Fail "the mirror content assertion only compared $verified file(s) under $srcRoot -- firmware\KilnFW_recovery cannot really be that small, so the enumeration is broken and this assertion is vacuous."
    }
    Write-Host "Mirror content check: all $verified file(s) byte-identical in the build directory."

    $newestSourceTime = ($srcFiles | Measure-Object -Property LastWriteTime -Maximum).Maximum
    if (-not $newestSourceTime) {
        Fail "could not determine a newest source mtime under $srcRoot -- refusing to grade artifact freshness with no signal to grade it against"
    }

    # Strip any MSYS/MSYS2 environment inherited from a git-bash launcher --
    # idf.py silently misbehaves (or no-ops) under it. Same defensive strip
    # check_00_kilnfw_target_build.ps1 applies.
    foreach ($v in @("MSYSTEM", "MSYSTEM_PREFIX", "MSYSTEM_CARCH", "MSYSTEM_CHOST", "MSYS", "MSYS2_PATH_TYPE")) {
        if (Test-Path "Env:$v") { Remove-Item "Env:$v" }
    }

    & $IdfProfile *>&1 | Out-Null
    $env:CCACHE_DISABLE = "1"

    $binPath = Join-Path $dstRoot "build\KilnFW_recovery.bin"
    $elfPath = Join-Path $dstRoot "build\KilnFW_recovery.elf"

    # STALE CACHED BUILD CONFIG GUARD (2026-09-19), shared mechanism with
    # check_00_kilnfw_target_build.ps1's identically-named block. $dstRoot's
    # build\ directory is persistent/incremental across runs (see this
    # file's own header on why that is deliberate), and this project's
    # config comes from sdkconfig.defaults rather than a copied board
    # sdkconfig -- but the underlying risk is the same one a reviewer
    # observed on the main target: an ordinary incremental `idf.py build`
    # is not guaranteed to redo a full CMake configure just because a
    # tracked config input's mtime changed, so a Kconfig-gated compile-time
    # branch in this project's CMakeLists.txt/main could keep building
    # against whatever was cached from an earlier sdkconfig.defaults even
    # after the mirror step above delivers a changed one. Track the
    # defaults-file content this build directory was last reconfigured
    # against, and force `idf.py reconfigure` before building whenever it
    # disagrees (including "never recorded", which covers a brand-new build
    # directory and one left over from before this guard existed).
    #
    # sdkconfig.defaults really is the whole governing input here, and
    # that depends on the /MIR mirror above: this project has no tracked
    # sdkconfig (it is gitignored and absent from the invoking tree), and
    # the mirror excludes only build\, so any sdkconfig idf.py generated
    # at $dstRoot's root during an earlier run is deleted every run and
    # regenerated from the defaults. That matters, because ESP-IDF applies
    # sdkconfig.defaults only as defaults -- an existing sdkconfig wins, so
    # `idf.py reconfigure` alone would NOT pick up a changed defaults file.
    # If a tracked sdkconfig is ever added to this project, hash that
    # instead (as the main target's check does), or this guard becomes
    # cosmetic.
    $recoveryDefaultsHash = (Get-FileHash -LiteralPath (Join-Path $dstRoot "sdkconfig.defaults") -Algorithm SHA256).Hash
    $recoveryHashMarker = Join-Path $dstRoot "build\.sdkconfig_defaults_built.sha256"
    $recoveryCMakeCache = Join-Path $dstRoot "build\CMakeCache.txt"
    $recoveryPreviousHash = $null
    if (Test-Path -LiteralPath $recoveryHashMarker) {
        try { $recoveryPreviousHash = ([System.IO.File]::ReadAllText($recoveryHashMarker, [System.Text.Encoding]::UTF8)).Trim() } catch { $recoveryPreviousHash = $null }
    }
    if (($recoveryPreviousHash -ne $recoveryDefaultsHash) -and (Test-Path -LiteralPath $recoveryCMakeCache)) {
        $recoveryPrevDisplay = if ($recoveryPreviousHash) { $recoveryPreviousHash } else { "(none recorded)" }
        Write-Host "STALE CONFIG: $dstRoot\build was last reconfigured against sdkconfig.defaults hash $recoveryPrevDisplay; the mirrored sdkconfig.defaults now hashes $recoveryDefaultsHash. Running 'idf.py reconfigure' first." -ForegroundColor Yellow
        $reconfigureOutput = & idf.py -C $dstRoot reconfigure 2>&1
        $reconfigureExit = $LASTEXITCODE
        $reconfigureOutput | Write-Host
        if ($reconfigureExit -ne 0) {
            Fail "idf.py reconfigure failed (exit $reconfigureExit) in $dstRoot while correcting a stale cached build config (previous sdkconfig.defaults hash $recoveryPrevDisplay, current $recoveryDefaultsHash) -- see output above."
        }
    }

    Write-Host "Building KilnFW_recovery target in $dstRoot (sdkconfig.defaults hash $recoveryDefaultsHash) ..."
    $buildOutput = & idf.py -C $dstRoot build 2>&1
    $buildExit = $LASTEXITCODE
    $buildOutput | Write-Host

    if ($buildExit -ne 0) {
        Fail "idf.py build failed (exit $buildExit) in $dstRoot -- see output above."
    }

    # The IDF project's binary name comes from its top-level project() name
    # in CMakeLists.txt, not assumed here -- discover the actual .bin/.elf
    # produced rather than hardcoding a guess that could silently drift from
    # the CMakeLists.txt project name.
    if (-not (Test-Path -LiteralPath $binPath) -or -not (Test-Path -LiteralPath $elfPath)) {
        $foundBins = Get-ChildItem -LiteralPath (Join-Path $dstRoot "build") -Filter "*.bin" -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -ne "bootloader.bin" -and $_.Name -ne "partition-table.bin" -and $_.Name -ne "ota_data_initial.bin" }
        if ($foundBins.Count -eq 1) {
            $binPath = $foundBins[0].FullName
            $elfPath = [System.IO.Path]::ChangeExtension($binPath, "elf")
        }
        if (-not (Test-Path -LiteralPath $binPath) -or -not (Test-Path -LiteralPath $elfPath)) {
            Fail "idf.py build reported success (exit 0) but no single app .bin/.elf pair was found under $(Join-Path $dstRoot 'build') -- refusing to report PASS without a real build artifact."
        }
    }

    $binTime = (Get-Item -LiteralPath $binPath).LastWriteTime
    $elfTime = (Get-Item -LiteralPath $elfPath).LastWriteTime
    $tolerance = [TimeSpan]::FromSeconds(2)
    if (($binTime -lt $newestSourceTime.Subtract($tolerance)) -or ($elfTime -lt $newestSourceTime.Subtract($tolerance))) {
        Fail "idf.py build reported success (exit 0) but $binPath (mtime $binTime) / $elfPath (mtime $elfTime) predate the newest source file's mtime ($newestSourceTime) -- refusing to publish a stale artifact as current."
    }

    # Record the sdkconfig.defaults content this build directory is now
    # known-good against -- only after a build this script itself confirmed
    # succeeded and passed the freshness check above. See the "STALE CACHED
    # BUILD CONFIG GUARD" comment further up.
    try {
        [System.IO.File]::WriteAllText($recoveryHashMarker, $recoveryDefaultsHash, (New-Object System.Text.UTF8Encoding($false)))
    } catch {
        Fail "could not write $recoveryHashMarker ($($_.Exception.Message)) -- refusing to report PASS when the stale-config guard's own bookkeeping cannot be trusted for the next run."
    }

    # Publish into this tree's own firmware/KilnFW_recovery/build/, which is
    # exactly where tools/check_recovery_image_size.py's DEFAULT_RECOVERY_BIN
    # looks. A plain Copy-Item -Force is sufficient here (unlike
    # check_00_kilnfw_target_build.ps1's careful MoveFileEx dance): this
    # check runs in run_all_checks.ps1's phase 1 (the two target builds),
    # which finishes entirely before phase 2 -- where
    # check_recovery_image_size.ps1 lives -- ever starts, so there is no
    # concurrent reader of this file to race.
    $mainRecoveryBuildDir = Join-Path $repoRoot "firmware\KilnFW_recovery\build"
    New-Item -ItemType Directory -Force -Path $mainRecoveryBuildDir | Out-Null
    $publishedBin = Join-Path $mainRecoveryBuildDir ([System.IO.Path]::GetFileName($binPath))
    $publishedElf = Join-Path $mainRecoveryBuildDir ([System.IO.Path]::GetFileName($elfPath))
    Copy-Item -LiteralPath $binPath -Destination $publishedBin -Force
    Copy-Item -LiteralPath $elfPath -Destination $publishedElf -Force

    # tools/check_recovery_image_size.py's default path is specifically
    # firmware\KilnFW_recovery\build\recovery.bin (chosen to mirror
    # KilnCtrl.bin's sibling-naming convention, per that script's own
    # docstring) -- publish under that exact name too, alongside whatever
    # name idf.py itself produced, so the two scripts do not have to agree
    # on a project-name-derived filename.
    $canonicalBin = Join-Path $mainRecoveryBuildDir "recovery.bin"
    if ($publishedBin -ne $canonicalBin) {
        Copy-Item -LiteralPath $publishedBin -Destination $canonicalBin -Force
    }

    Write-Host "PASS: built $([System.IO.Path]::GetFileName($binPath)) ($((Get-Item -LiteralPath $binPath).Length) B) against sdkconfig.defaults hash $recoveryDefaultsHash, published to $mainRecoveryBuildDir (including as recovery.bin for tools/check_recovery_image_size.py)."
} finally {
    Exit-BuildLock -Lock $lock
}
} finally {
    Exit-KilnBuildGate -Gate $buildGate
}
