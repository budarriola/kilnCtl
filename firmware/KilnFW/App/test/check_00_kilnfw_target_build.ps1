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

# KNOWN MAIN REPO ROOT (constant, not derived).
# --------------------------------------------
# The one shared tree on this machine whose invocations get the pre-warmed
# "C:\wt\checkbuild" build worktree instead of their own hash-tagged one
# (see the identity-check comment further down, where this constant is
# compared against), and whose firmware\KilnFW\sdkconfig is the fallback
# board-tuned config for a tree that has none of its own.
#
# Deliberately a fixed string, not derived from git: `git rev-parse
# --git-common-dir` was considered and rejected. From a LINKED worktree of
# the main tree it does resolve to the main tree's .git and would work here
# -- but the whole reason this constant exists is the OTHER case: "an
# unrelated private clone that happens to consider itself main by its own
# git's reckoning" (see below), i.e. a totally separate `git clone`, not a
# linked worktree of this repo at all. That clone's own .git IS its
# git-common-dir -- there is no shared ancestry to walk to reach the real
# machine-wide shared tree, because none exists in its git object graph.
# Only a value fixed outside git (this constant) can name "the one specific
# directory on THIS machine", independent of how any given invoking tree
# relates to it in git's own worktree/clone bookkeeping.
#
# Override with the environment variable KILNCTL_MAIN_REPO_ROOT if this
# checkout ever moves, or on a different machine where the shared tree
# lives somewhere else.
$KnownMainRepoRoot = if ($env:KILNCTL_MAIN_REPO_ROOT) { $env:KILNCTL_MAIN_REPO_ROOT } else { "C:\Users\budar\OneDrive\Desktop\kilnCtl" }

$ErrorActionPreference = "Stop"

# Native git/idf.py output on stderr (informational lines, e.g. "From
# <path>" on a fetch) is promoted to a terminating NativeCommandError under
# "Stop" -- the same trap run_all_checks.ps1's own header documents. Every
# external-process call in this script runs under "Continue" and checks
# $LASTEXITCODE explicitly instead.
$ErrorActionPreference = "Continue"

# A global named mutex (the shared build_lock.ps1 helper) so that building in
# this tree's persistent build worktree, and publishing into this tree's
# firmware/KilnFW/build/ directory, never races another concurrent run.
#
# NOT shared with check_bootloader_builds.ps1 (corrected 2026-09-16, D6 of
# docs/audits/review_check00_per_tree_build_2026-09-15.md -- the previous
# wording here claimed it was). That script takes "saftyfw_bootloader_build";
# nothing else in the repo takes the "kilnfw_checkbuild_worktree[_<hex>]" names
# this script uses. The lock is real and load-bearing, just not shared with
# that script: the runs it actually serializes are two invocations of THIS
# check from the same tree, and (see the prune below) this check's prune pass
# against another tree's build directory.
. (Join-Path $PSScriptRoot "..\..\..\..\tools\build_lock.ps1")

# This file lives at firmware/KilnFW/App/test/ -- four levels below repo root.
# -LiteralPath: Resolve-Path glob-expands otherwise, so a tree path containing
# [ or ] would fail to resolve here (same bug class as the prune's Test-Path,
# D1 below -- every path predicate in this file takes the literal form).
$repoRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..\..\..")
$IdfProfile = "C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"

if (-not (Test-Path -LiteralPath $IdfProfile)) {
    Write-Host "SKIP: ESP-IDF profile script not found at $IdfProfile -- toolchain not installed on this machine" -ForegroundColor Yellow
    exit 3
}

# BUILD DIRECTORY IS PER-INVOKING-TREE (2026-09-15).
# --------------------------------------------------
# Short path is required: the default .claude\worktrees\... path overflows
# the MSVC/xtensa command line on this project (see CLAUDE.md). Persistent
# across runs on purpose -- see the cost discussion above.
#
# Until this revision that path was one fixed string, "C:\wt\checkbuild", no
# matter which tree invoked the check. The mirror step below always did read
# the INVOKING tree ($repoRoot is derived from $PSScriptRoot, never
# hardcoded), so a run from a private worktree genuinely compiled that
# worktree's source -- that part was never broken, and a claim that this
# check "always builds the main tree" is false. What WAS broken is that every
# concurrent session on this machine mirrored its own source over the same
# shared destination. Two consequences, both demonstrated 2026-09-15:
#   * Cross-tree destruction. Mirroring from a tree whose lvgl submodule was
#     not initialised EMPTIED the shared worktree's lvgl checkout (see the
#     exclude-path defect fixed in Mirror-Tree below). The next session's run
#     then died with "Failed to resolve component 'lvgl': unknown name" -- a
#     CMake configure error indistinguishable, to whoever reads it, from a
#     real break in their own source.
#   * Thrash, and readers seeing someone else's build. With CCACHE_DISABLE=1
#     two trees alternating through one build directory recompile essentially
#     everything each time, and anything reading the artifacts mid-run is
#     reading a build that was grading a different tree's source -- one
#     documented source of the transiently-incomplete-ELF readings that have
#     produced a phantom regression report here before.
#
# Fix: the build directory is a function of the invoking tree. The main
# worktree keeps the historical "C:\wt\checkbuild" exactly as before (same
# path, same lock name, same already-warm build directory -- no behaviour
# change and no extra disk for the common case); any other tree gets
# "C:\wt\checkbuild_<10 hex of SHA256 of that tree's path>" plus its own
# build-lock name, so concurrent invocations from different trees neither
# share a directory nor queue behind one another.
#
# DISK. One persistent ESP-IDF build directory per tree that has ever run
# this check, ~425 MB each (measured 2026-09-15). They do not accumulate:
# each carries a .checkbuild_source marker naming the tree it belongs to, and
# every run prunes any sibling C:\wt\checkbuild_<hex> whose recorded tree no
# longer exists on disk -- which is exactly what happens when a throwaway
# agent worktree is removed. Only directories that BOTH match that naming
# scheme AND carry the marker are ever removed, so the hand-made worktrees
# under C:\wt\ and check_01's own checkbuild_origin_kilnfw are never touched.
function Fail([string]$msg) {
    Write-Host ""
    Write-Host "FAILED: $msg" -ForegroundColor Red
    exit 1
}

$headCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
if (-not $headCommit) {
    Fail "could not resolve HEAD in $repoRoot"
}

# Which tree am I? The shared "C:\wt\checkbuild" path is reserved for the
# ONE known shared main tree on this machine, named here as a fixed literal
# constant -- NOT derived by asking the invoking tree's own git metadata
# "am I the first entry of my own `git worktree list`". That self-referential
# test is trivially true for ANY standalone repository (a `git clone` that is
# not even a linked worktree of the shared repo reports itself as the sole,
# and therefore first, entry of its OWN worktree list), so it previously let
# a private git-repo copy claim the shared "C:\wt\checkbuild" path and mix its
# sources into the real main tree's build directory (reviewer finding,
# 2026-09-19: a bogus undefined-reference link failure traced back to exactly
# this). Comparing against a fixed, known absolute path closes that: only
# invocations from that one real tree ever get the shared, pre-warmed
# directory; every other tree -- including a linked worktree, a throwaway
# agent worktree under C:\wt\, or an unrelated private clone that happens to
# consider itself "main" by its own git's reckoning -- always gets its own
# hash-tagged directory below, so distinct source trees never share a mirror.
# ($KnownMainRepoRoot itself is defined once, near the top of this file.)
$repoRootFull = ([System.IO.Path]::GetFullPath($repoRoot.Path)).TrimEnd('\')

if ([string]::Equals($repoRootFull, $KnownMainRepoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    $WorktreePath = "C:\wt\checkbuild"
    $LockName = "kilnfw_checkbuild_worktree"
} else {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $hashBytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($repoRootFull.ToLowerInvariant()))
    } finally {
        $sha.Dispose()
    }
    $treeTag = (($hashBytes | ForEach-Object { $_.ToString("x2") }) -join "").Substring(0, 10)
    $WorktreePath = "C:\wt\checkbuild_$treeTag"
    $LockName = "kilnfw_checkbuild_worktree_$treeTag"
}
$WorktreeSdkconfig = Join-Path $WorktreePath "firmware\KilnFW\sdkconfig"
$MarkerFile = Join-Path $WorktreePath ".checkbuild_source"
Write-Host "Invoking tree:   $repoRootFull"
Write-Host "Build worktree:  $WorktreePath"

# SDKCONFIG PROVISIONING FOR A FRESH WORKTREE (2026-09-15).
# ---------------------------------------------------------
# sdkconfig is gitignored (board-specific, hand-tuned), so a freshly created
# worktree -- exactly the clean-worktree-at-origin/main workflow this project
# treats as standing practice -- has none of its own. Regenerating one from
# Kconfig defaults is not an option: it is KNOWN to diverge from what the
# board actually runs (CLAUDE.md's "gitignored config hides mismatch", which
# is how the FT6336U/NS2009 touch-panel mismatch was found).
#
# CORRECTION (2026-09-16): this paragraph also used to claim that
# "sdkconfig.defaults additionally pins no CONFIG_IDF_TARGET, so a
# regenerated config silently targets plain esp32". THAT IS FALSE, and was
# false when written -- 827dd887, the very commit that introduced the
# surrounding per-tree build-directory scheme, ADDED
# CONFIG_IDF_TARGET="esp32s3" to sdkconfig.defaults. Verified directly
# 2026-09-16: a fresh worktree with no sdkconfig at all builds to exit 0
# under xtensa-esp32s3-elf-gcc and generates CONFIG_IDF_TARGET="esp32s3".
# The wrong-target argument is therefore NOT a reason to copy anything, and
# check_01_kilnfw_pushed_build.ps1 has dropped its own copy entirely on the
# strength of that evidence.
#
# The copy stays HERE, for the one reason that survives: this check exists to
# answer "does MY tree build the way the BOARD is configured", uncommitted
# edits included. Building against sdkconfig.defaults instead would answer a
# different question -- the one check_01 now answers -- and would stop this
# check from ever seeing a board-config-dependent break.
# Prefer the invoking tree's own sdkconfig; fall back to the main worktree's
# and SAY SO. If neither exists there is no honest config to build against:
# FAIL, never SKIP (run_all_checks.ps1 fails the run on a SKIP by default,
# and a missing board config is a provisioning defect to fix, not an absent
# toolchain to shrug at).
$MainSdkconfig = Join-Path $repoRoot "firmware\KilnFW\sdkconfig"
if (-not (Test-Path -LiteralPath $MainSdkconfig)) {
    $fallbackSdkconfig = Join-Path $KnownMainRepoRoot "firmware\KilnFW\sdkconfig"
    if (Test-Path -LiteralPath $fallbackSdkconfig) {
        Write-Host "NOTE: $repoRootFull has no firmware\KilnFW\sdkconfig (gitignored; absent in a fresh worktree) -- using the main worktree's board-tuned config at $fallbackSdkconfig" -ForegroundColor Yellow
        $MainSdkconfig = $fallbackSdkconfig
    } else {
        Fail "no firmware\KilnFW\sdkconfig in the invoking tree ($repoRootFull) and none in the main worktree ($KnownMainRepoRoot) either. This check must build against the BOARD-TUNED config rather than one regenerated from Kconfig defaults -- a regenerated config does build correctly for esp32s3 (sdkconfig.defaults pins CONFIG_IDF_TARGET), but it answers check_01's question, 'does a fresh clone of origin/main build', instead of this check's, 'does this tree build the way the board is actually configured'. Run the IDE workspace setup, or copy a known-good sdkconfig into the main tree, before this check can say anything truthful."
    }
}

# Prune per-tree build worktrees whose owning tree is gone (a removed agent
# worktree). Deliberately narrow: a directory is only ever removed if its
# name matches the <hex> scheme this script itself generates AND it carries
# the marker file this script itself writes AND the tree named in that marker
# no longer exists. C:\wt holds many hand-made worktrees belonging to other
# sessions, plus check_01's checkbuild_origin_kilnfw; none of those can match
# all three conditions.
#
# THE OWNER-EXISTS TEST MUST BE LITERAL (2026-09-16, D1 of
# docs/audits/review_check00_per_tree_build_2026-09-15.md).
# `Test-Path $owner` glob-expands its argument. For an existing directory whose
# path contains [ or ] -- e.g. C:\wt\tree[1] -- `Test-Path` returns FALSE while
# [System.IO.Directory]::Exists() returns TRUE. A live session whose worktree
# happens to be named that way would therefore have been read as "gone" and its
# build directory `git worktree remove --force`d and `Remove-Item -Recurse`d out
# from under a running build. No bracketed worktree existed when this was found,
# so it never fired; it is closed here rather than left latent.
#
# THE MARKER MUST BE UTF-8, OR THE LITERAL PREDICATE IS ITSELF A DELETION PATH
# (2026-09-16, N1 of docs/audits/review_check00_d1_d6_closure_2026-09-16.md).
# The first version of this fix argued that the literal .NET predicate was
# better than `Test-Path -LiteralPath` because the marker's then `-Encoding
# ascii` write substituted `?` for any non-ASCII character, and `Test-Path`
# only matched such a marker because `?` is a single-character wildcard --
# "honestly failing rather than matching by luck". That direction is INVERTED
# and was measured to be so: for a live owner whose path contains a non-ASCII
# character the path really does exist, the marker is what is lossy, and the
# wildcard was load-bearing safety. `Test-Path` returned True and kept the
# directory; `[Directory]::Exists` returns False and PRUNES it -- D1's own
# failure shape, a live session's build directory deleted, reached through a
# different trigger. The actual fix is upstream of the predicate: the marker is
# now written and read as UTF-8 without a BOM (see the write site below), so it
# round-trips every path NTFS can name. With a faithful marker the literal
# predicate is both correct and safe, and neither half can be dropped.
#
# AND IT MUST NOT RACE THE VICTIM'S OWN BUILD. This loop deliberately runs
# OUTSIDE the $LockName lock taken further down -- and moving it inside would
# not help, because that lock is THIS tree's, while every directory this loop
# can delete belongs to a DIFFERENT tree holding a DIFFERENT per-tree lock. So
# the prune takes the victim's own lock (derivable from its directory name,
# which is exactly how $LockName is derived above) with a zero timeout: if the
# owning tree's check is running right now, the directory is left alone and a
# later run prunes it. The owner-is-gone condition is permanent, so deferring
# costs nothing but disk until the next invocation.
# Build directories that are deliberately SHARED rather than per-tree, and so
# are never marker-owned and never pruned -- see the long note inside the loop.
$SharedBuildDirNames = @("checkbuild", "checkbuild_origin_kilnfw", "checkbuild_origin_saftyfw")
foreach ($stale in (Get-ChildItem -LiteralPath "C:\wt" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -cmatch '^checkbuild_[0-9a-f]{10}$' })) {
    # -cmatch, not -match (2026-09-16, N5 of
    # docs/audits/review_check00_d1_d6_closure_2026-09-16.md): PowerShell's
    # -match is case-insensitive, so it accepts uppercase hex, while Win32
    # mutex names are case-SENSITIVE -- a mutex named with uppercase hex was
    # measured to be acquirable independently of its lowercase twin, so a
    # directory named that way would read as unlocked no matter who was
    # building in it. This script only ever generates lowercase hex, so the
    # case is unreachable today; the case-sensitive match keeps the directory
    # name and the mutex name agreeing on exactly one spelling.
    if ([string]::Equals($stale.FullName, $WorktreePath, [System.StringComparison]::OrdinalIgnoreCase)) { continue }
    # THE SHARED, NON-PER-TREE BUILD DIRECTORIES ARE DELIBERATELY UNRECLAIMABLE
    # (2026-09-16). C:\wt\checkbuild_origin_kilnfw (check_01_kilnfw_pushed_build.ps1)
    # and C:\wt\checkbuild_origin_saftyfw (check_01_saftyfw_pushed_build.ps1) carry
    # no .checkbuild_source marker, and MUST NOT be given one. That is not an
    # oversight in 827dd887's pruning logic -- it is the only correct state for
    # them. The marker means "this directory belongs to the one tree named
    # inside it, and dies with that tree"; those two directories belong to NO
    # tree. Their content is a pure function of origin/main, they are
    # deliberately shared by every invoking worktree (which is why they are one
    # directory instead of ~425 MB per tree), and they are hard-reset to the
    # fetched ref on every run. A marker naming whichever tree happened to
    # create one would be a false statement of ownership, and would make a
    # live, shared directory look reclaimable the moment that unrelated tree was
    # removed -- deleting it out from under every other session's check_01.
    # Strictly worse than leaving them permanently resident.
    #
    # They are already unreachable here: neither name matches the
    # ^checkbuild_[0-9a-f]{10}$ pattern above, and neither carries a marker, so
    # two independent conditions each exclude them (confirmed by fixture in
    # docs/audits/review_check00_per_tree_build_2026-09-15.md). This list is
    # belt-and-braces and changes no behaviour today: it states the intent by
    # name, so that a future widening of the name pattern cannot silently bring
    # a shared directory into range.
    if ($SharedBuildDirNames -contains $stale.Name) { continue }
    $staleMarker = Join-Path $stale.FullName ".checkbuild_source"
    if (-not (Test-Path -LiteralPath $staleMarker)) { continue }
    # Read as UTF-8, matching the write site's encoding exactly -- see the N1
    # note above. Get-Content -Raw with no -Encoding uses the ANSI codepage in
    # Windows PowerShell 5.1 and would mangle exactly the paths the UTF-8 write
    # exists to preserve.
    $owner = $null
    try { $owner = [System.IO.File]::ReadAllText($staleMarker, [System.Text.Encoding]::UTF8) } catch { $owner = $null }
    if (-not $owner) { continue }
    $owner = $owner.Trim()
    if ([System.IO.Directory]::Exists($owner) -or [System.IO.File]::Exists($owner)) { continue }

    $staleTag = $stale.Name.Substring("checkbuild_".Length)
    # The lock name comes from build_lock.ps1's own name builder, never from a
    # second copy of the "Global\kilnCtl_buildlock_" literal (2026-09-16, N3).
    $staleMutex = New-Object System.Threading.Mutex($false, (Get-BuildLockMutexName -Name "kilnfw_checkbuild_worktree_$staleTag"))
    $staleHeld = $false
    try {
        try {
            $staleHeld = $staleMutex.WaitOne(0)
        } catch [System.Threading.AbandonedMutexException] {
            # Previous holder died without releasing; we own it now.
            $staleHeld = $true
        }
        if (-not $staleHeld) {
            Write-Host "Not pruning $($stale.FullName): its owning tree '$owner' is gone, but its build lock is currently HELD -- a run is still using that directory. Leaving it for a later invocation rather than deleting a directory mid-build." -ForegroundColor Yellow
            continue
        }
        # RE-TEST UNDER THE LOCK (2026-09-16, N6 of the same review). The
        # owner-exists test above ran before the lock was taken, and the owning
        # tree can be recreated at the same path in between (an agent worktree
        # removed and re-added, or a `git worktree add` that has started but
        # not yet reached its own Enter-BuildLock). Re-testing here means the
        # decision to delete is made with the victim's lock actually in hand,
        # which -- together with the victim now taking that lock BEFORE it
        # creates its directory and writes its marker (see the lock site
        # below) -- leaves no window in which a live tree's directory is
        # deleted.
        if ([System.IO.Directory]::Exists($owner) -or [System.IO.File]::Exists($owner)) {
            Write-Host "Not pruning $($stale.FullName): its owning tree '$owner' reappeared between the first check and acquiring its build lock." -ForegroundColor Yellow
            continue
        }
        Write-Host "Pruning stale build worktree $($stale.FullName) -- its tree '$owner' no longer exists"
        & git -C $repoRoot worktree remove --force $stale.FullName 2>&1 | Write-Host
        if ([System.IO.Directory]::Exists($stale.FullName)) {
            Remove-Item -LiteralPath $stale.FullName -Recurse -Force -ErrorAction SilentlyContinue
        }
        & git -C $repoRoot worktree prune 2>&1 | Write-Host
    } finally {
        if ($staleHeld) {
            try { $staleMutex.ReleaseMutex() } catch { }
        }
        $staleMutex.Dispose()
    }
}

# THE LOCK COVERS DIRECTORY CREATION AND THE MARKER WRITE TOO (2026-09-16, N6
# of docs/audits/review_check00_d1_d6_closure_2026-09-16.md). It used to be
# taken only just before the mirror, so this tree created its own build
# directory and wrote its own ownership marker while holding nothing -- and a
# concurrent pruner that had already decided "owner gone" (a tree removed and
# recreated at the same path) could hold that same per-tree lock and delete the
# directory out from under a run that was still starting up. Taking it here,
# before the directory exists, means every operation this script performs
# against $WorktreePath -- create, mark, mirror, build, publish -- happens under
# the one lock a pruner must acquire before it can touch that directory.
#
# The prune loop above deliberately stays OUTSIDE this lock: it is this tree's
# lock, and every directory the prune can delete belongs to a different tree
# and is guarded by that tree's own lock, which the prune takes separately.
$lock = Enter-BuildLock -Name $LockName
try {
    if (-not (Test-Path -LiteralPath $WorktreePath)) {
        Write-Host "Setting up persistent build worktree at $WorktreePath (first run for this tree) ..."
        # PRUNE FIRST (2026-09-19). The directory is gone, but git may still
        # hold a registration for it under .git/worktrees -- the normal
        # outcome when someone deletes C:\wt\checkbuild_<hex> by hand to
        # force a from-scratch build, which is exactly what anyone chasing a
        # suspected stale-build-artifact result does. `git worktree add`
        # then refuses with "already registered" (exit 128), this check
        # FAILS, and the ELF-reading checks that run after it silently grade
        # the PREVIOUS run's stale KilnCtrl.elf -- confident numbers for a
        # build that never happened. Pruning here lets a hand-deleted
        # directory self-heal. `git worktree prune` only drops registrations
        # whose directory is already missing, so it can never disturb a live
        # worktree, this tree's own included.
        & git -C $repoRoot worktree prune 2>&1 | Write-Host
        & git -C $repoRoot worktree add --detach $WorktreePath $headCommit 2>&1 | Write-Host
        if ($LASTEXITCODE -ne 0) {
            Fail "git worktree add failed (exit $LASTEXITCODE)"
        }
    }

# WRITE THE OWNERSHIP MARKER IMMEDIATELY (2026-09-16, D5 of the same review).
# Until now the marker was written far below, after the mirror and the sentinel
# block. Any run that died in between -- a mirror failure, a sentinel failure,
# a killed process -- left a ~425 MB checkbuild_<hex> with NO marker, and the
# prune above skips markerless directories unconditionally (by design: that is
# what keeps it away from hand-made worktrees). Such a directory was therefore
# unreclaimable forever. Writing the marker here, the moment the directory is
# known to exist, means every directory this script creates is prunable from
# very close to its first instant.
#
# NOT "before anything that can fail" (2026-09-16, N4 of
# docs/audits/review_check00_d1_d6_closure_2026-09-16.md, which correctly
# caught that overclaim in the previous wording). The `git worktree add` above
# and its exit-code Fail sit between the directory appearing and this write.
# The reachable failures there are benign -- a bad commit-ish exits 128 having
# created nothing, and the one case that does leave a directory behind
# (a non-empty destination) is excluded by the Test-Path guard above -- but the
# window is not empty, and the claim should not say it is.
#
# UTF-8, NO BOM, AND A HARD FAILURE IF IT CANNOT BE WRITTEN (2026-09-16, N1 and
# N4 of docs/audits/review_check00_d1_d6_closure_2026-09-16.md). Two separate
# defects, one call site:
#   * -Encoding ascii substituted "?" for every non-ASCII character, so the
#     marker did not name the tree it claimed to. Once the prune's owner test
#     became literal (D1) that turned directly into "delete a live owner's
#     build directory" -- see the long N1 note at the prune loop. UTF-8
#     round-trips every path NTFS can name; no BOM, so the bytes are the path
#     and nothing else. The prune reads it back with the same encoding.
#   * Set-Content raises a NON-TERMINATING error under this script's
#     $ErrorActionPreference = "Continue", so a failed marker write simply
#     printed red text and carried on, leaving exactly the unreclaimable
#     ~425 MB markerless directory D5 set out to eliminate, with no signal in
#     the check's own verdict. [File]::WriteAllText throws, and the throw is
#     converted into this script's own loud Fail.
try {
    [System.IO.File]::WriteAllText($MarkerFile, $repoRootFull, (New-Object System.Text.UTF8Encoding($false)))
} catch {
    Fail "could not write the ownership marker $MarkerFile ($($_.Exception.Message)) -- refusing to continue, because an unmarked build directory is never reclaimed by the prune pass above and would leak ~425 MB permanently."
}

# CONTENT test, not merely the presence of the .git file: a /MIR with a
# mis-specified exclude has emptied this directory before while leaving .git
# behind, and the resulting downstream failure ("Failed to resolve component
# 'lvgl'") reads as a broken build rather than a broken checkout. Re-init
# whenever the component is not actually THERE.
$lvglGitFile = Join-Path $WorktreePath "firmware\KilnFW\components\lvgl\.git"
$lvglCMake = Join-Path $WorktreePath "firmware\KilnFW\components\lvgl\CMakeLists.txt"
if (-not (Test-Path -LiteralPath $lvglGitFile) -or -not (Test-Path -LiteralPath $lvglCMake)) {
    Write-Host "Initializing lvgl submodule in worktree ..."
    & git -C $WorktreePath submodule update --init --force firmware/KilnFW/components/lvgl 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Fail "git submodule update --init for lvgl failed (exit $LASTEXITCODE)"
    }
    if (-not (Test-Path -LiteralPath $lvglCMake)) {
        Fail "git submodule update --init for lvgl reported success (exit 0) but $lvglCMake is still missing -- refusing to build against an incomplete component tree."
    }
}

# Mirror the main tree's actual firmware/KilnFW and firmware/hwAbstraction
# directories into the worktree -- this is what actually makes the build
# "current" every run, not the worktree's own git history. lvgl (a
# submodule, static, cloned once above) and build/ (gitignored, persistent,
# the whole point of keeping it) are excluded so this never touches them;
# sdkconfig is excluded here because it is copied and hash-verified
# separately below, not mirrored blindly.
# EXCLUDED SUBDIRECTORIES MUST BE ABSOLUTE, AND GIVEN FOR BOTH SIDES
# (2026-09-15). robocopy's /XD resolves a RELATIVE argument against the
# current directory, not against the source or destination root, so the
# "components\lvgl" that used to be passed here excluded nothing whatsoever
# and /MIR duly deleted the destination's lvgl checkout as an "extra"
# directory. ("build" survived only because robocopy also accepts a bare NAME
# containing no separator.) Demonstrated 2026-09-15: a run from a tree whose
# own lvgl submodule was uninitialised emptied the shared build worktree's
# lvgl, and the following run there failed with "Failed to resolve component
# 'lvgl'". Expanding each excluded subdirectory against BOTH roots removes
# the ambiguity. ($args is also an automatic variable in PowerShell -- the
# local array is named $roboArgs now so it does not shadow it.)
function Mirror-Tree([string]$src, [string]$dst, [string[]]$excludeSubdirs, [string[]]$excludeFiles) {
    $roboArgs = @($src, $dst, "/MIR", "/NFL", "/NDL", "/NJH", "/NJS", "/NP")
    if ($excludeSubdirs.Count -gt 0) {
        $roboArgs += "/XD"
        foreach ($rel in $excludeSubdirs) {
            $roboArgs += (Join-Path $src $rel)
            $roboArgs += (Join-Path $dst $rel)
        }
    }
    if ($excludeFiles.Count -gt 0) { $roboArgs += "/XF"; $roboArgs += $excludeFiles }
    & robocopy @roboArgs | Out-Null
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

# The whole mirror -> build -> verify -> publish sequence operates on this
# tree's persistent worktree/build dir ($WorktreePath) and on this tree's own
# firmware\KilnFW\build\, so the lock must span all of it, not just the final
# publish copy -- two concurrent runs FROM THE SAME TREE building into the
# same worktree at once would race ninja/cmake exactly like the two
# check_bootloader_builds.ps1 runs documented in build_lock.ps1's header, and
# a lock that only wrapped the publish step would not have prevented that.
# The lock NAME is per-tree for the same reason the directory is: runs from
# different trees now touch disjoint directories, so making them queue behind
# one global mutex would serialize work that cannot actually collide.
    # (The lock is taken further up now, before the build worktree directory is
    # created and its ownership marker written -- see N6 at that site. It used
    # to be taken here.)
    Write-Host "Mirroring current firmware/KilnFW, firmware/hwAbstraction and firmware/CommonFW into $WorktreePath ..."
    # NESTED build\ DIRECTORIES MUST BE NAMED EXPLICITLY (2026-09-16, D4 of
    # docs/audits/review_check00_per_tree_build_2026-09-15.md). Making the /XD
    # arguments absolute (the 2026-09-15 fix above, which was necessary and is
    # kept) silently narrowed their scope at the same time: robocopy treats a
    # bare NAME like "build" as "any directory with that name at any depth",
    # but an absolute path as "exactly that one directory". So the host-test
    # output trees -- firmware\KilnFW\App\test\build (418 files, ~22 MB in the
    # main tree) and firmware\CommonFW\test\build -- stopped being excluded and
    # have been mirrored into the build worktree on every run since, pure
    # wasted I/O carrying host .obj/.exe artifacts that the target build has no
    # use for. They are listed here so the absolute form covers what the bare
    # name used to, without giving back the ambiguity the bare name had.
    #
    # THE LIST WAS STILL INCOMPLETE (2026-09-16, N2 of
    # docs/audits/review_check00_d1_d6_closure_2026-09-16.md). The version above
    # named the nested host-test build trees but not firmware\CommonFW\build --
    # 427 files, 67.9 MB in the main tree, roughly three times what the D4 fix
    # recovered, mirrored on every run by both the bare-name and absolute-path
    # versions of this code. firmware\KilnFW\.venv (942 files, 12.7 MB,
    # containing among other things pip's own .../operations/build) is a Python
    # virtualenv, not target-build input, and is excluded here for the same
    # reason. Adding a new build-output or tooling directory under these three
    # trees means adding it here: /XD with an absolute path covers exactly one
    # directory, by design.
    # ONE DECLARATION OF WHAT IS MIRRORED, READ BY BOTH THE MIRROR AND ITS
    # VERIFICATION (2026-09-16, N7 closure). The post-mirror content assertion
    # below must walk exactly the set of files robocopy was asked to copy. Two
    # hand-maintained copies of these exclude lists would be CLAUDE.md's "reset
    # one side of a pair" class: a newly excluded directory dropped from the
    # mirror but still demanded by the verifier, or -- far worse, because it is
    # silent -- a tree that is mirrored but no longer verified. Both sides read
    # this one table.
    $MirrorSpecs = @(
        @{ Rel = "firmware\KilnFW"
           XD  = @("build", "App\test\build", ".venv", "components\lvgl", ".git")
           XF  = @("sdkconfig") },
        @{ Rel = "firmware\hwAbstraction"
           XD  = @("test\build", ".git")
           XF  = @() },
        @{ Rel = "firmware\CommonFW"
           XD  = @("build", "test\build", ".git")
           XF  = @() }
    )
    Mirror-Tree (Join-Path $repoRoot "firmware\KilnFW") (Join-Path $WorktreePath "firmware\KilnFW") `
        $MirrorSpecs[0].XD $MirrorSpecs[0].XF
    Mirror-Tree (Join-Path $repoRoot "firmware\hwAbstraction") (Join-Path $WorktreePath "firmware\hwAbstraction") `
        $MirrorSpecs[1].XD $MirrorSpecs[1].XF
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
        $MirrorSpecs[2].XD $MirrorSpecs[2].XF

    # DID THE MIRROR ACTUALLY DELIVER THIS TREE'S SOURCE? A check that cannot
    # see the source it was invoked to grade must FAIL loudly, never quietly
    # build whatever the previous run happened to leave behind. exit 3/SKIP is
    # deliberately NOT used for any of these: run_all_checks.ps1 fails the
    # whole run on a SKIP by default now, and more to the point a broken
    # mirror is a defect in this check's own setup, not an absent
    # prerequisite. Comparing content hashes (not just existence) is what
    # makes this an assertion about WHICH tree got built.
    $verifiedPairs = 0
    foreach ($rel in @(
        "firmware\KilnFW\CMakeLists.txt",
        "firmware\KilnFW\App\CMakeLists.txt",
        "firmware\CommonFW\CMakeLists.txt"
    )) {
        $srcFile = Join-Path $repoRoot $rel
        $dstFile = Join-Path $WorktreePath $rel
        if (-not (Test-Path -LiteralPath $srcFile)) { continue }
        if (-not (Test-Path -LiteralPath $dstFile)) {
            Fail "the mirror step did not deliver $dstFile from the invoking tree's $srcFile -- the build worktree does not contain the source this check was invoked to grade."
        }
        if ((Get-FileHash -LiteralPath $srcFile -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $dstFile -Algorithm SHA256).Hash) {
            Fail "after mirroring, $dstFile still differs from the invoking tree's $srcFile -- refusing to grade a build of source that is not this tree's."
        }
        $verifiedPairs++
    }
    if ($verifiedPairs -lt 2) {
        Fail "only $verifiedPairs mirror sentinel file(s) could be verified under $repoRoot -- refusing to report on a mirror whose success could not be confirmed (a vacuous assertion is the failure mode this block exists to prevent)."
    }

    # NEWEST-SOURCE WITNESS (2026-09-16, D3 of
    # docs/audits/review_check00_per_tree_build_2026-09-15.md).
    # The three sentinels above are all CMakeLists.txt files, which change
    # rarely, and they are compared source-against-destination. If the mirror
    # silently no-ops, the destination still holds an identical copy from the
    # PREVIOUS run, so all three pairs match while every .c file in the tree is
    # stale. The floor is genuinely enforced and every exit above is a Fail, so
    # the assertion is not vacuous -- but it proves "these three files agree",
    # not "this tree's source got mirrored", which is what the failure text
    # claims. This closes that gap by additionally pinning the ONE file whose
    # staleness the mirror is actually there to prevent: the most recently
    # modified mirrored source file in the invoking tree. An unchanged tree
    # passes trivially (the copy is already correct, which is the legitimate
    # no-op case the freshness signal below also accommodates); a tree carrying
    # a fresh edit cannot pass unless that edit physically arrived in the build
    # worktree.
    #
    # WHAT THE WITNESS ALONE DOES AND DOES NOT PROVE (2026-09-16, N7 of
    # docs/audits/review_check00_d1_d6_closure_2026-09-16.md). The original
    # wording here claimed the witness was "by construction the edit a no-op
    # mirror would have failed to deliver". That is FALSE and was measured to
    # be: the witness is whatever file has the newest mtime, which is the
    # edited file only when nothing else in three trees was touched more
    # recently, and `robocopy /MIR` SKIPS a file whose source and destination
    # have the same size and the same mtime even when the contents differ
    # (measured: destination kept its old bytes, robocopy exited 0). A stale
    # .c can therefore ride in behind an unrelated newer witness. The witness
    # is still a real strengthening over three rarely-changing CMakeLists.txt
    # pairs and is load-bearing -- poisoning the mirror fails the check here,
    # before idf.py runs -- but it is a sample, not a proof.
    #
    # So the witness is backed below by a CONTENT check over every file git
    # reports as modified or untracked in the three mirrored trees. That set is
    # exactly "the uncommitted change this check exists to catch before it is
    # even committed", named by content rather than by mtime, so neither an
    # unrelated newer file nor robocopy's same-size/same-mtime skip can hide
    # one. The residual gap, stated plainly: a COMMITTED change that a stale
    # destination copy happens to match in both size and mtime is still not
    # detected by either assertion. Closing that needs hashing every mirrored
    # file (thousands, every run) or /FFT-style forced copies; it is not
    # closed here, and no assertion in this block claims otherwise.
    #
    # The exclusions mirror Mirror-Tree's own: anything under a build\ tree,
    # the lvgl submodule, .git, and sdkconfig (handled separately by the
    # hash-verified copy below, and excluded from the mirror via /XF).
    $witness = $null
    foreach ($rel in @("firmware\KilnFW", "firmware\hwAbstraction", "firmware\CommonFW")) {
        $srcRoot = Join-Path $repoRoot $rel
        if (-not (Test-Path -LiteralPath $srcRoot)) { continue }
        $cand = Get-ChildItem -LiteralPath $srcRoot -Recurse -File -ErrorAction SilentlyContinue |
            Where-Object {
                $_.FullName -notmatch '\\build\\' -and
                $_.FullName -notmatch '\\components\\lvgl\\' -and
                $_.FullName -notmatch '\\\.git\\' -and
                $_.Name -ne 'sdkconfig'
            } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($cand -and ((-not $witness) -or ($cand.LastWriteTime -gt $witness.LastWriteTime))) { $witness = $cand }
    }
    if (-not $witness) {
        Fail "could not identify any mirrored source file under $repoRoot to use as the newest-source mirror witness -- refusing to report on a mirror whose success could not be confirmed."
    }
    $witnessRel = $witness.FullName.Substring($repoRootFull.Length).TrimStart('\')
    $witnessDst = Join-Path $WorktreePath $witnessRel
    if (-not (Test-Path -LiteralPath $witnessDst)) {
        Fail "the mirror step did not deliver $witnessDst -- $witnessRel is the most recently modified source file in the invoking tree (mtime $($witness.LastWriteTime)), i.e. precisely the edit this check exists to grade, and it is absent from the build worktree."
    }
    if ((Get-FileHash -LiteralPath $witness.FullName -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $witnessDst -Algorithm SHA256).Hash) {
        Fail "after mirroring, $witnessDst still differs from the invoking tree's $witnessRel -- that is the most recently modified source file in this tree (mtime $($witness.LastWriteTime)), so the build worktree is grading source older than this tree's newest edit."
    }
    Write-Host "Mirror witness: $witnessRel (newest-modified source, $($witness.LastWriteTime)) is present and byte-identical in the build worktree."

    # CONTENT-NAMED WITNESS SET: every uncommitted change in the mirrored trees
    # (2026-09-16, N7). Selected by git, not by mtime, so robocopy's
    # same-size/same-mtime skip cannot hide one behind an unrelated newer file.
    $dirtyPorcelain = @(& git -C $repoRoot status --porcelain --untracked-files=all -- "firmware/KilnFW" "firmware/hwAbstraction" "firmware/CommonFW" 2>$null)
    $dirtyChecked = 0
    foreach ($line in $dirtyPorcelain) {
        if (-not $line -or $line.Length -lt 4) { continue }
        $path = $line.Substring(3).Trim()
        # Renames are reported as "old -> new"; only the destination exists.
        if ($path -match '\s->\s') { $path = ($path -split '\s->\s')[-1] }
        $path = $path.Trim('"').Replace('/', '\')
        if ($path -match '\\build\\' -or $path -match '^firmware\\CommonFW\\build\\' -or
            $path -match '\\components\\lvgl\\' -or $path -match '\\\.venv\\' -or
            $path -match '\\sdkconfig$' -or $path -match '\\$') { continue }
        $dirtySrc = Join-Path $repoRoot $path
        # A deletion has no source file; /MIR removes the destination copy, and
        # a destination that is already absent is equally correct.
        if (-not (Test-Path -LiteralPath $dirtySrc)) { continue }
        $dirtyDst = Join-Path $WorktreePath $path
        if (-not (Test-Path -LiteralPath $dirtyDst)) {
            Fail "the mirror step did not deliver $dirtyDst -- $path is an uncommitted change in this tree (git reports it as '$($line.Substring(0,2))'), i.e. precisely the edit this check exists to grade before it is committed, and it is absent from the build worktree."
        }
        if ((Get-FileHash -LiteralPath $dirtySrc -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $dirtyDst -Algorithm SHA256).Hash) {
            Fail "after mirroring, $dirtyDst still differs from the invoking tree's $path -- that is an uncommitted change in this tree, so the build worktree is grading source that is not this tree's. (robocopy /MIR skips a file whose source and destination share a size and an mtime even when the contents differ; this assertion is what catches that.)"
        }
        $dirtyChecked++
    }
    if ($dirtyChecked -gt 0) {
        Write-Host "Mirror content check: all $dirtyChecked uncommitted change(s) in the mirrored trees are present and byte-identical in the build worktree."
    } else {
        Write-Host "Mirror content check: git reports no uncommitted changes in the mirrored trees, so the newest-source witness above is the only freshness assertion available."
    }
    # FULL-TREE CONTENT ASSERTION -- N7's STATED RESIDUAL GAP, CLOSED
    # (2026-09-16). The two assertions above are both samples. The witness is
    # one file chosen by mtime; the dirty-set check covers exactly what git
    # reports as modified or untracked. Neither says anything about a COMMITTED
    # file whose stale copy in the build worktree happens to match on both size
    # and mtime -- and robocopy /MIR SKIPS such a file, keeping the old bytes
    # and exiting 0. N7 recorded that gap and explicitly declined to close it.
    #
    # It is reachable, and not only in theory. Measured here 2026-09-16:
    #   * robocopy /MIR leaves the stale destination bytes in place when source
    #     and destination share a size and an mtime (exit 0, no diagnostic).
    #   * There is NO robocopy flag that repairs it: /IS, /IS /IT, /E /IS and
    #     /FFT were each measured against the same fixture and every one left
    #     the stale bytes. So "make the mirror always copy" is not available;
    #     the only instrument is to verify content after the fact.
    #   * `git checkout` is NOT a way in: it stamps mtime with the checkout
    #     time, which differs from the destination's older stamp, so the copy
    #     happens. That is the reassuring half.
    #   * `Copy-Item` (and robocopy, and archive extraction) DO preserve mtime.
    #     Restoring a same-length variant of a file from a preserved-timestamp
    #     copy is therefore a real way to produce this state -- and restoring a
    #     file by hand from a pristine copy is exactly what this project's own
    #     negative-test discipline requires after every sabotage. The collision
    #     needs the same length too, which a flipped comparison operator, a
    #     changed digit or a same-length identifier rename all satisfy.
    # A false PASS here is not cosmetic: this check is what stands between a
    # stale artifact and a board, and it PUBLISHES the ELF it built into the
    # shared build/ directory that every downstream stack-budget check reads.
    #
    # Cost, measured on this machine over the 1063 mirrored files (61.3 MB):
    # hashing one side with [SHA256]::ComputeHash over a FileStream is 0.82 s
    # (Get-FileHash, which is what the sample assertions use, is 1.94 s for the
    # same set -- 2.4x, so the .NET form is used here); both sides together are
    # ~1.6 s. The mirror step itself is 0.09 s in the steady no-op case and
    # 0.79 s cold. So this roughly doubles the pre-build phase in absolute
    # terms, against an idf.py build measured at ~27 s incremental and ~138 s
    # cold -- 1-6% of the run, to convert a documented silent false PASS into a
    # loud failure. The sample assertions above are KEPT rather than replaced:
    # they fail earlier and name the specific thing that went wrong, which is a
    # better diagnostic than "some file differs".
    $verifyStart = Get-Date
    $verifiedFiles = 0
    $shaAlg = [System.Security.Cryptography.SHA256]::Create()
    try {
        foreach ($spec in $MirrorSpecs) {
            $srcRoot = Join-Path $repoRoot $spec.Rel
            if (-not (Test-Path -LiteralPath $srcRoot)) { continue }
            $dstRoot = Join-Path $WorktreePath $spec.Rel
            # Same exclusions robocopy was given, derived from the same table:
            # /XD is an absolute directory prefix, /XF a bare filename at any depth.
            $excludedPrefixes = @()
            foreach ($x in $spec.XD) { $excludedPrefixes += ((Join-Path $srcRoot $x).ToLowerInvariant() + '\') }
            foreach ($f in (Get-ChildItem -LiteralPath $srcRoot -Recurse -File -Force -ErrorAction SilentlyContinue)) {
                $lower = $f.FullName.ToLowerInvariant()
                $skip = $false
                foreach ($e in $excludedPrefixes) { if ($lower.StartsWith($e)) { $skip = $true; break } }
                if ($skip) { continue }
                if ($spec.XF -contains $f.Name) { continue }
                $relPath = $f.FullName.Substring($srcRoot.Length).TrimStart('\')
                $dstFile = Join-Path $dstRoot $relPath
                if (-not [System.IO.File]::Exists($dstFile)) {
                    Fail "the mirror step did not deliver $dstFile -- $($spec.Rel)\$relPath exists in the invoking tree but not in the build worktree, so this check would be grading a source tree that is missing a file the invoking tree has."
                }
                $fsSrc = [System.IO.File]::OpenRead($f.FullName)
                try { $hSrc = [System.BitConverter]::ToString($shaAlg.ComputeHash($fsSrc)) } finally { $fsSrc.Dispose() }
                $fsDst = [System.IO.File]::OpenRead($dstFile)
                try { $hDst = [System.BitConverter]::ToString($shaAlg.ComputeHash($fsDst)) } finally { $fsDst.Dispose() }
                if ($hSrc -ne $hDst) {
                    Fail "after mirroring, $dstFile still differs from the invoking tree's $($spec.Rel)\$relPath. robocopy /MIR skips a file whose source and destination share a size and an mtime even when the contents differ (measured: it keeps the stale bytes and exits 0), and no robocopy flag repairs it -- so the build worktree is holding source that is NOT this tree's, and every artifact built from it would be graded as if it were. Delete $dstFile (or the whole build worktree) and re-run."
                }
                $verifiedFiles++
            }
        }
    } finally {
        $shaAlg.Dispose()
    }
    # Vacuity floor, in this file's established style: an assertion that
    # verified nothing must never be reported as an assertion that passed.
    if ($verifiedFiles -lt 100) {
        Fail "the full-tree mirror content assertion only compared $verifiedFiles file(s) under $repoRoot -- the invoking tree's mirrored source trees cannot really be that small (1063 files when this was written), so the enumeration or its exclusions are broken and this assertion is vacuous. Refusing to report on a mirror whose success could not be confirmed."
    }
    Write-Host ("Mirror content assertion: all {0} mirrored files are byte-identical in the build worktree ({1:N1}s)." -f $verifiedFiles, ((Get-Date) - $verifyStart).TotalSeconds)

    if (-not (Test-Path -LiteralPath $lvglCMake)) {
        Fail "$lvglCMake is missing after the mirror step -- the lvgl component has been deleted out of the build worktree. Downstream this surfaces as a confusing 'Failed to resolve component lvgl' CMake error rather than as the checkout problem it actually is."
    }

    # (The .checkbuild_source ownership marker used to be written HERE. It is
    # now written immediately after the worktree directory is created, well
    # before anything that can fail -- see D5 at that site. Writing it at this
    # point left every run that died earlier with an unreclaimable ~425 MB
    # markerless directory.)

    Copy-Item -LiteralPath $MainSdkconfig -Destination $WorktreeSdkconfig -Force
    $mainHash = (Get-FileHash -LiteralPath $MainSdkconfig -Algorithm SHA256).Hash
    $worktreeHash = (Get-FileHash -LiteralPath $WorktreeSdkconfig -Algorithm SHA256).Hash
    if ($mainHash -ne $worktreeHash) {
        Fail "sdkconfig copy did not verify (hash mismatch) -- refusing to build against an unconfirmed config"
    }

    # STALE CACHED BUILD CONFIG GUARD (2026-09-19).
    # ----------------------------------------------
    # $WorktreePath's build\ directory is PERSISTENT across runs (see the
    # cost discussion at the top of this file), and idf.py's own
    # CMAKE_CONFIGURE_DEPENDS wiring on sdkconfig does not reliably repeat a
    # full CMake configure for every Kconfig-gated `if(CONFIG_...)` branch in
    # a component CMakeLists.txt merely because ninja's ordinary incremental
    # build noticed sdkconfig's mtime changed -- a reviewer observed exactly
    # this class of drift directly: a checkbuild worktree whose build\ had
    # been configured from an older sdkconfig kept
    # CONFIG_KILNCTL_ENABLE_GPIO_PROBE=n cached in its generated
    # sdkconfig.h/CMake state even after this script copied in a newer
    # sdkconfig with that option set to y, and `idf.py build` (an ordinary
    # incremental ninja invocation, not a reconfigure) did not correct it.
    # The ELF that came out of that build then disagreed with the very
    # sdkconfig this script had just verified byte-identical above, and
    # every downstream ELF-grading check (the four stack-budget checks this
    # file's header names) silently graded the wrong binary.
    #
    # Fix: this script now tracks, itself, which sdkconfig content the build
    # directory was last actually reconfigured against -- a marker written
    # right next to the artifacts it produces (build\.sdkconfig_built.sha256,
    # a sibling of KilnCtrl.elf, not something idf.py maintains on its own).
    # If that marker disagrees with the sdkconfig just copied in (including
    # "no marker yet", which covers both a brand-new build directory and one
    # left over from before this guard existed), the build directory is
    # reconfigured explicitly (`idf.py reconfigure`) BEFORE `idf.py build`
    # runs, so this run's build reflects the sdkconfig that was just verified
    # rather than whatever CMake state happened to already be cached. The
    # marker is only updated after a build this script itself confirms
    # succeeded (see the write site below, after the freshness check), so a
    # failed or aborted run never claims a config it did not actually build.
    $SdkconfigHashMarker = Join-Path $WorktreePath "firmware\KilnFW\build\.sdkconfig_built.sha256"
    $CMakeCacheFile = Join-Path $WorktreePath "firmware\KilnFW\build\CMakeCache.txt"
    $previousBuiltHash = $null
    if (Test-Path -LiteralPath $SdkconfigHashMarker) {
        try { $previousBuiltHash = ([System.IO.File]::ReadAllText($SdkconfigHashMarker, [System.Text.Encoding]::UTF8)).Trim() } catch { $previousBuiltHash = $null }
    }
    $sdkconfigStale = ($previousBuiltHash -ne $mainHash)
    if ($sdkconfigStale -and (Test-Path -LiteralPath $CMakeCacheFile)) {
        $prevDisplay = if ($previousBuiltHash) { $previousBuiltHash } else { "(none recorded -- first run under this guard, or a build directory from before it existed)" }
        Write-Host "STALE CONFIG: build directory at $WorktreePath was last reconfigured against sdkconfig hash $prevDisplay; the sdkconfig just copied in hashes $mainHash. Running 'idf.py reconfigure' before building so cached CMake/Kconfig state (e.g. a component's compile-time CONFIG_* branch) cannot disagree with the config this run verified." -ForegroundColor Yellow
        & $IdfProfile *>&1 | Out-Null
        $reconfigureOutput = & idf.py -C (Join-Path $WorktreePath "firmware\KilnFW") reconfigure 2>&1
        $reconfigureExit = $LASTEXITCODE
        $reconfigureOutput | Write-Host
        if ($reconfigureExit -ne 0) {
            Fail "idf.py reconfigure failed (exit $reconfigureExit) while correcting a stale cached build config (previous sdkconfig hash $prevDisplay, current $mainHash) -- see output above."
        }
    } elseif ($sdkconfigStale) {
        Write-Host "No prior $CMakeCacheFile -- this build directory has never been configured, so the upcoming 'idf.py build' will perform a full first-time configure against sdkconfig hash $mainHash (no separate reconfigure step needed)."
    } else {
        Write-Host "Config unchanged: build directory at $WorktreePath was already configured against sdkconfig hash $mainHash -- no reconfigure needed."
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
            if (-not (Test-Path -LiteralPath $root)) { continue }
            $candidate = Get-ChildItem -LiteralPath $root -Recurse -File -ErrorAction SilentlyContinue |
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

    if (-not (Test-Path -LiteralPath $binPath) -or -not (Test-Path -LiteralPath $elfPath)) {
        Fail "idf.py build reported success (exit 0) but $binPath / $elfPath does not exist -- refusing to report PASS without a real build artifact."
    }

    # Positive freshness check, not just existence: both artifacts' last-write
    # time must be AT OR AFTER the newest tracked source mtime captured above
    # -- see the "FRESHNESS SIGNAL" comment. A stale elf/bin that predates the
    # newest source edit (the MSYSTEM no-op, or any other silent short-
    # circuit) must FAIL loudly here rather than be mistaken for a fresh
    # build; an elf/bin that is merely older than "now" but already reflects
    # every current source file (the legitimate no-op case) must PASS.
    $binTime = (Get-Item -LiteralPath $binPath).LastWriteTime
    $elfTime = (Get-Item -LiteralPath $elfPath).LastWriteTime
    # Small negative tolerance for filesystem timestamp granularity/clock skew.
    $tolerance = [TimeSpan]::FromSeconds(2)
    if (($binTime -lt $newestSourceTime.Subtract($tolerance)) -or ($elfTime -lt $newestSourceTime.Subtract($tolerance))) {
        Fail "idf.py build reported success (exit 0) but $binPath (mtime $binTime) / $elfPath (mtime $elfTime) predate the newest tracked source file's mtime ($newestSourceTime) -- the build silently did not relink against current source (known cause: MSYSTEM/MSYS environment inherited from a git-bash launcher confusing idf.py, or any other silent no-op). Refusing to publish a stale artifact as current."
    }

    # Record the sdkconfig this build directory is now known-good against,
    # ONLY after the build succeeded and passed the freshness check above --
    # see the "STALE CACHED BUILD CONFIG GUARD" comment. A run that fails
    # anywhere above this line leaves the previous marker (or none) in place,
    # so a subsequent run still sees the config as stale and reconfigures,
    # rather than this run falsely claiming a config it never finished
    # building against.
    try {
        [System.IO.File]::WriteAllText($SdkconfigHashMarker, $mainHash, (New-Object System.Text.UTF8Encoding($false)))
    } catch {
        Fail "could not write $SdkconfigHashMarker ($($_.Exception.Message)) -- refusing to report PASS when the stale-config guard's own bookkeeping cannot be trusted for the next run."
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
        Copy-Item -LiteralPath $SourcePath -Destination $TempPath -Force
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
        Remove-Item -LiteralPath $TempPath -Force -ErrorAction SilentlyContinue
        Fail "Could not publish $FinalPath -- MoveFileEx failed after $maxAttempts attempts with Win32 error $lastErr (5=ERROR_ACCESS_DENIED usually means another process has $FinalPath open; find and close it, e.g. a stuck objdump/nm/readelf). The stale existing $FinalPath was left untouched; refusing to report PASS with an unpublished build."
    }

    $elfTmp = Join-Path $mainBuildDir "KilnCtrl.elf.tmp_$PID"
    $binTmp = Join-Path $mainBuildDir "KilnCtrl.bin.tmp_$PID"
    Publish-BuildArtifact -SourcePath $elfPath -TempPath $elfTmp -FinalPath (Join-Path $mainBuildDir "KilnCtrl.elf")
    Publish-BuildArtifact -SourcePath $binPath -TempPath $binTmp -FinalPath (Join-Path $mainBuildDir "KilnCtrl.bin")
    Write-Host "Published fresh KilnCtrl.elf/.bin to $mainBuildDir"

    # ALSO PUBLISH compile_commands.json (2026-09-15). check_compile_esp_backends.ps1
    # (firmware/hwAbstraction/test/compile_esp_backends.ps1) and check_duplicate_symbols.ps1
    # (tools/check_duplicate_symbols.ps1) both CONSUME build output that only this
    # check ever produces, but before this fix this script published nothing for
    # either of them -- a genuinely fresh worktree with no prior hand-triggered
    # build_kilnfw run therefore FAILED the first (missing compile_commands.json)
    # and SKIPPED the second (missing object-file tree) even after a fully clean,
    # fully green run_all_checks.ps1 pass of THIS check. Multiple agents hit this
    # independently. Fix: publish both artifacts these checks actually read, the
    # same way the elf/bin are published above.
    $ccPath = Join-Path $WorktreePath "firmware\KilnFW\build\compile_commands.json"
    if (Test-Path -LiteralPath $ccPath) {
        $ccTmp = Join-Path $mainBuildDir "compile_commands.json.tmp_$PID"
        Publish-BuildArtifact -SourcePath $ccPath -TempPath $ccTmp -FinalPath (Join-Path $mainBuildDir "compile_commands.json")
        Write-Host "Published fresh compile_commands.json to $mainBuildDir"
    } else {
        Write-Host "NOTE: idf.py build did not produce $ccPath -- check_compile_esp_backends.ps1 will report its own missing-file error." -ForegroundColor Yellow
    }

    # ALSO PUBLISH THE SDKCONFIG THIS BUILD ACTUALLY USED (2026-09-15).
    # check_all_task_stack_budgets.py adjudicates its Kconfig-gated task rows
    # (gpio_probe, backlight_pwm) against the sdkconfig that produced the ELF
    # it measures, and resolves that config from --elf: a config published IN
    # the ELF's own directory wins over anything inferred from directory
    # layout, and there is deliberately no repo-root fallback.
    #
    # The ELF published just above was built in $WorktreePath against
    # $WorktreeSdkconfig -- which is NOT necessarily the invoking tree's own
    # firmware\KilnFW\sdkconfig, because a fresh worktree has none (gitignored)
    # and the provisioning step above falls back to the main worktree's
    # board-tuned config. Publishing the ELF without the config that produced
    # it therefore leaves that checker either grading against a config that did
    # not build this ELF, or -- in a fresh worktree, which has no sdkconfig at
    # all -- unable to adjudicate and failing even though THIS check just
    # succeeded. Publishing the real one alongside makes the published artifact
    # self-describing and closes both cases.
    $sdkTmp = Join-Path $mainBuildDir "sdkconfig.tmp_$PID"
    Publish-BuildArtifact -SourcePath $WorktreeSdkconfig -TempPath $sdkTmp -FinalPath (Join-Path $mainBuildDir "sdkconfig")
    Write-Host "Published the sdkconfig this build used to $mainBuildDir"

    # check_duplicate_symbols.ps1 walks this project's own component object
    # directories (esp-idf\{App,drivers,kilnlink,hwabstraction_esp}\CMakeFiles\...)
    # under firmware\KilnFW\build\ in the MAIN tree -- not in this check's own
    # persistent worktree -- so those .obj trees have to be mirrored out here too,
    # the same way Mirror-Tree already copies source trees IN above. This is
    # read-only input for a downstream check (nm inspection), never rebuilt from
    # it, so a plain /MIR mirror (last-writer-wins, no lock beyond the one this
    # whole try block already holds) is sufficient.
    $objComponentDirs = @(
        "esp-idf\App\CMakeFiles\__idf_App.dir",
        "esp-idf\drivers\CMakeFiles\__idf_drivers.dir",
        "esp-idf\kilnlink\CMakeFiles\__idf_kilnlink.dir",
        "esp-idf\hwabstraction_esp\CMakeFiles\__idf_hwabstraction_esp.dir"
    )
    foreach ($rel in $objComponentDirs) {
        $srcObjDir = Join-Path $WorktreePath "firmware\KilnFW\build\$rel"
        if (Test-Path -LiteralPath $srcObjDir) {
            $dstObjDir = Join-Path $mainBuildDir $rel
            New-Item -ItemType Directory -Force -Path $dstObjDir | Out-Null
            Mirror-Tree $srcObjDir $dstObjDir @() @()
        }
    }
    Write-Host "Published object-file trees for check_duplicate_symbols.ps1 to $mainBuildDir\esp-idf\*"
} finally {
    # Belt-and-suspenders: Publish-BuildArtifact already removes its own temp
    # file on a MoveFileEx failure, but an unexpected exception elsewhere in
    # the try block (e.g. Copy-Item itself throwing) could still leave a
    # KilnCtrl.{elf,bin}.tmp_$PID orphan in build/ -- exactly the class of
    # leftover found in this tree (KilnCtrl.elf.tmp_6288) before this fix.
    # $PID is this script's own process id, so this only ever removes a temp
    # file this run itself could have created, never another process's.
    $mainBuildDirCleanup = Join-Path $repoRoot "firmware\KilnFW\build"
    Remove-Item -LiteralPath (Join-Path $mainBuildDirCleanup "KilnCtrl.elf.tmp_$PID") -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath (Join-Path $mainBuildDirCleanup "KilnCtrl.bin.tmp_$PID") -Force -ErrorAction SilentlyContinue
    Exit-BuildLock -Lock $lock
}

Write-Host ""
Write-Host "PASS: KilnFW target build succeeded, $binPath produced (built against sdkconfig hash $mainHash)." -ForegroundColor Green
exit 0
