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
# NO SDKCONFIG SEEDING, AND WHY THAT IS NOW CORRECT (2026-09-16).
# Earlier revisions of this check copied a board-tuned `sdkconfig` from the
# invoking tree (later, falling back to the main worktree's) into the build
# worktree below, on the stated grounds that `sdkconfig` is gitignored, that
# ESP-IDF cannot build without one, and -- critically -- that regenerating
# one from Kconfig defaults would silently target plain `esp32` because
# `firmware/KilnFW/sdkconfig.defaults` pinned no CONFIG_IDF_TARGET.
#
# THAT LAST PREMISE WAS FALSE WHEN IT WAS WRITTEN. 827dd887 added
# CONFIG_IDF_TARGET="esp32s3" to sdkconfig.defaults, and 827dd887 is an
# ANCESTOR of e15ad517, the commit that wrote the justification quoting it
# (`git merge-base --is-ancestor 827dd887 e15ad517` exits 0). The target has
# been pinned in committed, tracked content this whole time.
#
# Verified directly rather than reasoned about, 2026-09-16: a fresh
# `git worktree add` at origin/main with NO sdkconfig of any kind, lvgl
# initialized and nothing else provisioned, runs `idf.py build` to exit 0,
# compiles with xtensa-esp32s3-elf-gcc ("Building ESP-IDF components for
# target esp32s3"), and the sdkconfig it generates contains
# CONFIG_IDF_TARGET="esp32s3". No other setting in the main tree's sdkconfig
# proved load-bearing for the build.
#
# So the seed is not merely unnecessary -- it was the thing standing between
# this check and the stronger check the old header itself wished for. With it
# gone, this check now builds against nothing but committed content plus
# `sdkconfig.defaults`, which is exactly what a fresh clone of origin/main
# gets. It therefore DOES now prove "a fresh clone of origin/main builds",
# and it catches the Kconfig-default drift the seeded run could not see by
# construction (feedback_gitignored_config_hides_mismatch.md's FT6336U
# incident is that class). Three defects went with the seed: a SHA256
# "integrity" comparison taken AFTER the copy, comparing the copy against its
# own source (a source file caught mid-write hashed identically on both sides
# and passed); a cross-worktree read reaching outside this check's own
# hermetic inputs; and a missing-sdkconfig FAIL that reported a purely local
# provisioning gap in this check's own origin/main-is-broken language.
#
# What this check still does NOT prove: that the binary it produces matches
# what the bench board runs. The board's own tuned sdkconfig may differ from
# sdkconfig.defaults, and catching THAT divergence is check_00's job (it
# mirrors the local tree and deliberately uses the board-tuned config), not
# this one's. The two checks are now complementary rather than redundant.
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
# actually invoke the compiler (worktree setup, submodule init) is a FAIL,
# never a silent PASS -- same contract as check_00.
#
# sdkconfig is no longer on that list in either direction: this check does
# not read, copy, or require one (see the no-seeding block above). The build
# worktree generates its own from the committed sdkconfig.defaults.
#
# FAILURE ATTRIBUTION: not every FAIL here is origin/main's fault. A failed
# `git worktree add` or a failed lvgl `submodule update --init` is a defect
# in THIS machine's environment, and reporting it in this check's
# "origin/main is broken, stop pulling it" language is actively harmful --
# that is the one message that stops a whole team pulling. Such failures go
# through Fail-Local, which says so plainly; only a genuine compile/link/
# artifact failure against the fetched ref goes through Fail.

$ErrorActionPreference = "Stop"
$ErrorActionPreference = "Continue"

. (Join-Path $PSScriptRoot "..\..\..\..\tools\build_lock.ps1")
. (Join-Path $PSScriptRoot "..\..\..\..\tools\build_gate.ps1")
. (Join-Path $PSScriptRoot "..\..\..\..\tools\pushed_build_stamp.ps1")

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

# A local/environment failure -- this check could not SET UP a build, so it
# never found out anything about origin/main. It must NOT inherit Fail's
# "stop pulling origin/main" footer: that message is the one that halts
# everyone else's work, and emitting it for a broken local git state or a
# submodule that would not clone is a false accusation against the pushed
# ref. Still exit 1, not 3 -- a machine that cannot set up this build is a
# provisioning defect somebody must fix, not a prerequisite to shrug at.
function Fail-Local([string]$msg) {
    Write-Host ""
    Write-Host "FAILED: could not set up the origin/main build -- $msg" -ForegroundColor Red
    Write-Host "        This is a LOCAL/ENVIRONMENT failure, NOT a statement about" -ForegroundColor Red
    Write-Host "        origin/main: this check never got as far as compiling it, so it" -ForegroundColor Red
    Write-Host "        says nothing either way about whether the pushed ref builds." -ForegroundColor Red
    Write-Host "        Fix this machine; do not treat it as a reason to stop pulling." -ForegroundColor Red
    exit 1
}

if (-not (Test-Path $IdfProfile)) {
    Write-Host "SKIP: ESP-IDF profile script not found at $IdfProfile -- toolchain not installed on this machine" -ForegroundColor Yellow
    exit 3
}

# NO SDKCONFIG SEED RESOLUTION HAPPENS HERE ANY MORE, deliberately: the
# whole block that used to sit at this point is gone. See the no-seeding
# section in the header for the evidence (827dd887 pinned
# CONFIG_IDF_TARGET="esp32s3" in sdkconfig.defaults, and a fresh worktree
# with no sdkconfig builds for esp32s3 at exit 0). Nothing outside this
# check's own build worktree is read.

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

# ONE SHARED BUILD WORKTREE IS CORRECT HERE, unlike check_00.
# check_00 moved to a per-invoking-tree build directory in 0facf63e because
# its build worktree MIRRORS the invoking tree, so two trees genuinely need
# two directories. This check's worktree is reset --hard to the fetched
# origin/main every run: its content is a pure function of origin/main and
# is identical no matter who invokes it. Two concurrent runs therefore want
# the SAME directory, serialized -- not two copies of it at ~425 MB each.
# Serialization is the build lock below, which is why creating this worktree
# now happens INSIDE that lock (see there).
#
# RESIDUAL EXPOSURE, STATED PLAINLY (2026-09-16). Moving creation and
# submodule init inside the lock closes the race between two runs of THIS
# script. It does not, and cannot, protect this directory from an actor that
# never takes the lock -- a hand-run 'rmdir /s /q' of the build tree, an
# editor indexing it, a cleanup script. One such incident was observed while
# this fix was being written: a concurrent full-suite run failed here with
# "ld.exe: reopening esp-idf/esp_hw_support/libesp_hw_support.a: No such
# file or directory", i.e. a LINK failure that reads exactly like a broken
# origin/main, caused by an out-of-lock deletion of this directory; the same
# run's log also showed the tree sitting at another session's deliberate
# "NEGATIVE TEST ONLY - deliberate compile break" commit. A shared directory
# plus a lock only everyone honours is the accepted cost of not copying
# ~425 MB per tree. If you are about to delete or check out this directory
# by hand, take the same lock first: tools/build_lock.ps1,
# Enter-BuildLock -Name "kilnfw_checkbuild_origin_worktree".
$WorktreePath = "C:\wt\checkbuild_origin_kilnfw"

foreach ($v in @("MSYSTEM", "MSYSTEM_PREFIX", "MSYSTEM_CARCH", "MSYSTEM_CHOST", "MSYS", "MSYS2_PATH_TYPE")) {
    if (Test-Path "Env:$v") { Remove-Item "Env:$v" }
}

# RESULT REUSE (2026-10-07, tools/pushed_build_stamp.ps1): a PASS for exactly
# this origin/main sha is reused instead of rebuilt, and concurrent callers
# queue on a per-sha lock (no gate slot held) rather than all building it.
Write-Host "origin/main is $originSha"
$pushedSlot = Enter-PushedBuildSlot -Name "kilnfw" -Sha $originSha
if ($pushedSlot.Reused) {
    Write-PushedBuildReused -Stamp $pushedSlot.Stamp -Sha $originSha -What "KilnFW target"
    exit 0
}
Write-Host "No reusable PASS stamp for $originSha -- building (this run owns the build)."
try {
# Build lock FIRST; a gate slot is held only around the compile (never while queued on a lock).
$lock = Enter-BuildLock -Name "kilnfw_checkbuild_origin_worktree"
try {
    # CREATION IS INSIDE THE LOCK (2026-09-16). It used to sit above, outside
    # it, so two concurrent first-runs from different trees both saw
    # "directory absent" and both ran `git worktree add` at the same path --
    # one of them failing, and taking a red check_01 with it, for a reason
    # that had nothing to do with origin/main. The same applied to the lvgl
    # submodule init. Both are now serialized with the build itself.
    if (-not (Test-Path $WorktreePath)) {
        Write-Host "Setting up persistent origin/main build worktree at $WorktreePath (first run) ..."
        & git -C $repoRoot worktree add --detach $WorktreePath $originSha 2>&1 | Write-Host
        if ($LASTEXITCODE -ne 0) {
            Fail-Local "git worktree add at $WorktreePath failed (exit $LASTEXITCODE)"
        }
    }

    $lvglGitFile = Join-Path $WorktreePath "firmware\KilnFW\components\lvgl\.git"
    if (-not (Test-Path $lvglGitFile)) {
        Write-Host "Initializing lvgl submodule in worktree ..."
        & git -C $WorktreePath submodule update --init firmware/KilnFW/components/lvgl 2>&1 | Write-Host
        if ($LASTEXITCODE -ne 0) {
            Fail-Local "git submodule update --init for lvgl failed (exit $LASTEXITCODE)"
        }
    }

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
    # `sdkconfig` is no longer excluded from the clean, and any sdkconfig left
    # behind by an older, seeding revision of this check is removed outright.
    # Leaving one in place would silently keep this build running on a
    # cross-worktree seed forever: idf.py reuses an existing sdkconfig and
    # would never regenerate from sdkconfig.defaults, so the strengthening
    # this change is for would be inert on exactly the machines that had run
    # the old version -- i.e. all of them.
    & git -C $WorktreePath clean -fdx -e build -e "components/lvgl" 2>&1 | Write-Host

    $WorktreeSdkconfig = Join-Path $WorktreePath "firmware\KilnFW\sdkconfig"
    if (Test-Path -LiteralPath $WorktreeSdkconfig) {
        Write-Host "Removing a pre-existing $WorktreeSdkconfig (leftover seed from an older revision of this check) so it is regenerated from the committed sdkconfig.defaults ..."
        Remove-Item -LiteralPath $WorktreeSdkconfig -Force
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

    # BUILD THE SAFTYFW SLOT A/B IMAGES THE EMBED_FILES GUARD REQUIRES
    # (2026-09-20). App/drivers/CMakeLists.txt FATAL_ERRORs at configure time
    # if firmware\SaftyFW\build\SaftyFW_slotA.bin / SaftyFW_slotB.bin are
    # missing -- introduced by the Pico embedded auto-update chain (docs/PICO_AUTO_UPDATE_PLAN.md).
    # This worktree is a PRISTINE `git worktree add` of origin/main (see the
    # header comment above) with no prior SaftyFW build ever run in it, so
    # without this step the very next `idf.py build` below would fail
    # immediately on a from-scratch clone of origin/main -- not because
    # origin/main's KilnFW source is actually broken, but because nothing
    # upstream in this worktree ever produced these two files. Shared with
    # check_00_kilnfw_target_build.ps1, which hit the same guard first but
    # happened not to notice because its mirrored worktree carries build
    # history across runs; see lib_saftyfw_slot_images.ps1 for the full
    # rationale and why a missing arm-none-eabi toolchain is a hard FAIL
    # here too, never a SKIP.
    . (Join-Path $PSScriptRoot "lib_saftyfw_slot_images.ps1")
    Ensure-SaftyfwSlotImages -KilnfwWorktreePath $WorktreePath

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
    $buildGate = Enter-KilnBuildGate -Label "kilnfw_pushed_build"
    try {
        $buildOutput = & idf.py -C (Join-Path $WorktreePath "firmware\KilnFW") build 2>&1
        $buildExit = $LASTEXITCODE
    } finally {
        Exit-KilnBuildGate -Gate $buildGate
    }

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
# Every verification above passed (Fail exits 1 before here): record it.
Write-PushedBuildStamp -Path (Get-PushedBuildStampPath -Name "kilnfw") -Sha $originSha -Detail "KilnCtrl.bin $((Get-Item $binPath).Length) bytes"
} finally {
    Exit-PushedBuildSlot -Slot $pushedSlot
}

Write-Host ""
Write-Host "PASS: origin/main (commit $originSha) builds KilnFW target cleanly. [built]" -ForegroundColor Green
exit 0
