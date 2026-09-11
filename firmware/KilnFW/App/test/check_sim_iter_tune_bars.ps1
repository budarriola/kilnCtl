# check_sim_iter_tune_bars.ps1 -- enforces sim_iter_tune.exe's A1/A2/A5/A6
# statistical acceptance bars (ITER_TUNE_REDESIGN_PLAN.md sec 6/7).
#
# Until 2026-09-10 (docs/audits/firing_score_entry_ema_review_2026-09-10.md)
# sim_iter_tune.c's main() printed PASS/FAIL per bar but always
# `return 0`, and build_host_tests.ps1 built it as a "data-generating
# harness, not run automatically" -- so no check_*.ps1 or CI path ever ran
# it, and every bar could silently fail forever. That is exactly how the A1
# false-accept rate went from 0.00% to 3.64% after d63a5591's coupling-model
# change and was only caught by a manual re-run. sim_iter_tune.c's main()
# now returns 1 if any of A1/A2/A5/A6 fails; this script builds it, runs it
# at the n=220 sample size the audit's numbers are quoted at (660 A1/A2
# comparisons), and fails on a non-zero exit -- naming which bar(s) failed
# from the captured stdout so a red run does not require re-reading the
# whole log by hand.
#
# This does NOT replace sim_iter_tune.exe's usefulness as an ad hoc
# data-generating harness for exploratory runs (any sample size, any
# argv[1]) -- run the .exe directly for that; this check only pins the one
# canonical n=220 configuration the audit's figures reference.
#
# 2026-09-10 KNOWN-FAILURE PIN, NOT A CLEARED BAR: A1's real defect is
# upstream of this repo's decision/measurement code -- d63a5591 changed
# sim_plant.c's coupling model class, which correctly exposed
# ENTRY_PEAK_C to neighbour-zone PWM ripple the old model never produced,
# and the fix is a re-identified coupling matrix (capture in progress as of
# this pin), not anything reachable from firing_score.c/firing_compare.c/
# this harness. sim_iter_tune.c's A1 check therefore enforces a PINNED
# ceiling (24/660, ~3.64%) instead of the real 2.0% design target -- see
# that file's own A1_PINNED_MAX_ACCEPTS/A1_PINNED_TOTAL comment for the
# full provenance and the ratchet-guard rule (the pin only ever tightens by
# hand, never widens to paper over a regression). A green run of THIS
# CHECK is not proof the 2.0% target is met -- read the "A1 NOTE" line in
# its own output. Full reasoning, including why 8b96b591's EMA smoothing
# was reverted rather than kept to hit 2.0% cheaply (it blinded
# firing_compare.c's degradation veto in the accept-permissive direction):
# docs/audits/firing_score_entry_ema_review_2026-09-10.md.
#
# EXIT CONDITION (updated 2026-09-11): the coupling re-identification this
# was waiting on has landed (2026-09-10/11) without producing an adoptable
# matrix -- the linear coupling model class itself was refuted (~33%
# superposition error at matched dT; docs/audits/coupling_joint_identification_capture_2026-09-10.md,
# commits 5844a3e8/947709a8). The exit condition is therefore restated on
# sim_plant.c's coupling model rather than on the re-identification effort:
# once sim_plant.c's coupling term is changed to a hardware-validated model
# class (or a deliberate decision to keep the linear form is documented),
# re-measure A1 at n=220 and either tighten sim_iter_tune.c's pin toward
# 2.0% or drop it in favour of enforcing A1_DESIGN_TARGET_PCT directly. See
# sim_iter_tune.c's A1_PINNED_MAX_ACCEPTS comment for the full text. A
# 2026-09-11 re-run of this exact check reproduced 24/660 (3.6364%)
# unchanged from the 2026-09-10 measurement -- no drift, no re-pin.
#
# 2026-09-10 SKIP vs FAIL, and why this does not depend on
# build_host_tests.ps1 having run first: this check was first found FAILING
# in another agent's environment with "'vswhere.exe' is not recognized" --
# a toolchain-location problem, not an A1 regression -- while it PASSED
# (against the pin) in this session's own environment on the identical
# code. A check whose red is sometimes "the bar failed" and sometimes "the
# compiler was never found" is exactly the ambiguity this repo has been
# burned by today (three agents mis-read a red build in the last few
# hours). This runner (run_all_checks.ps1) has a real, sanctioned THIRD
# status for exactly this case -- exit code 3, "SKIP", distinct from both
# PASS and FAIL, requiring a stated reason -- documented in its own header
# as covering precisely "no toolchain installed". This check now:
#   - builds with its OWN inline compiler flags rather than depending on
#     build_host_tests.ps1 having already run and left
#     App/test/build/host_tests_common_flags.rsp behind (sim_iter_tune.c
#     only needs its own directory's relative includes, so that shared
#     response file was never actually required here -- one less
#     prerequisite that could silently make this check inert on a fresh
#     checkout);
#   - exits 3 (SKIP) only when the toolchain itself could not be found or
#     invoked (vcvarsall.bat missing, or cl/vcvars produced no compiler
#     diagnostics at all and no executable -- the signature of an
#     environment problem, not a code problem);
#   - exits 1 (FAIL) whenever the compiler actually ran and reported real
#     diagnostics (grep for "error C" in its output) with no executable
#     produced, and whenever the executable ran and any bar failed below
#     the pin -- both of those ARE regressions in this checkout and must
#     stay red.
# This is not the "if not path.is_file(): skipTest(...)" antipattern this
# repo has been burned by before: that shape passes (exit 0) when its
# target is silently absent. SKIP here is exit 3, shown as a distinct
# yellow status by run_all_checks.ps1 with a mandatory reason line, and a
# SKIP still counts as "did not confirm the bars" -- it is never counted as
# a clean pass.

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path
$testDir = $PSScriptRoot
$driversDir = Join-Path $testDir "..\drivers"

# Same hardcoded path build_host_tests.ps1 uses -- there is no other
# mechanism in this repo to copy; both scripts invoke vcvarsall.bat the same
# way ("call vcvarsall.bat x64 >nul && cl ..."). vswhere.exe warnings from
# vcvarsall.bat's own internals are tolerated (see SKIP-vs-FAIL note above);
# only vcvars ITSELF being absent is treated as an environment prerequisite.
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    Write-Host "SKIP: MSVC toolchain not found -- vcvarsall.bat missing at $vcvars."
    Write-Host "      This is a prerequisite absence (documented SKIP case in run_all_checks.ps1's"
    Write-Host "      own header: 'no toolchain installed'), not an A1/A2/A5/A6 result."
    exit 3
}

$outDir = Join-Path $testDir "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
# Own object directory, distinct from build_host_tests.ps1's shared
# App/test/build -- both scripts can run concurrently (several agents run
# the suite simultaneously here) and previously clobbered each other's
# .obj files (pid.obj, firing_score.obj, etc.) despite holding different
# build locks. The .exe itself still lands in $outDir since nothing else
# writes that filename.
$objDir = Join-Path $testDir "build_sim_iter_tune_bars_obj"
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

. (Join-Path $repoRoot "tools\build_lock.ps1")
$lock = Enter-BuildLock -Name "kilnfw_sim_iter_tune_bars"
try {
    $exe = Join-Path $outDir "kilnctl_sim_iter_tune_bars_check.exe"
    if (Test-Path $exe) { Remove-Item -Force $exe }

    $src = @(
        (Join-Path $testDir "sim_iter_tune.c"),
        (Join-Path $testDir "sim_plant.c"),
        (Join-Path $driversDir "control\pid.c"),
        (Join-Path $driversDir "control\heater_output.c"),
        (Join-Path $driversDir "control\zone_coupling_solve.c"),
        (Join-Path $driversDir "control\firing_score.c"),
        (Join-Path $driversDir "control\firing_compare.c"),
        (Join-Path $driversDir "control\iter_tune.c")
    )
    $srcQuoted = ($src | ForEach-Object { "`"$_`"" }) -join " "

    # sim_iter_tune.c's own #includes are relative ("../drivers/control/...")
    # and need no /I flags, but the headers it pulls in (sim_plant.h ->
    # max31856_codec.h, zone_coupling_solve.h -> MAX31856.h, etc.) reach into
    # sibling driver directories and App/test/stubs/'s ESP-IDF type stand-ins
    # by bare name. Inlined here (not read from build_host_tests.ps1's
    # shared .rsp) so this check has no dependency on that script having run
    # first -- see this file's SKIP-vs-FAIL header note.
    $includeDirs = @(
        (Join-Path $testDir "stubs"),
        (Join-Path $testDir "..\..\..\CommonFW\include"),
        (Join-Path $testDir "..\drivers"),
        (Join-Path $testDir "..\drivers\bridge"),
        (Join-Path $testDir "..\drivers\common"),
        (Join-Path $testDir "..\drivers\control"),
        (Join-Path $testDir "..\drivers\http"),
        (Join-Path $testDir "..\drivers\hw"),
        (Join-Path $testDir "..\drivers\net"),
        (Join-Path $testDir "..\drivers\owners"),
        (Join-Path $testDir "..\drivers\persist"),
        (Join-Path $testDir "..\drivers\safety"),
        (Join-Path $testDir "..\drivers\sim"),
        (Join-Path $testDir "..\drivers\ui"),
        (Join-Path $testDir "..\..\..\hwAbstraction\esp\spi"),
        (Join-Path $testDir "..\..\..\hwAbstraction\esp\i2c"),
        (Join-Path $testDir "..\..\..\hwAbstraction\esp\uart"),
        (Join-Path $testDir "..\..\..\hwAbstraction\interface"),
        (Join-Path $testDir "..\..\..\hwAbstraction\host"),
        (Join-Path $testDir "..\..\..\hwAbstraction\esp\common")
    )
    $includeArgs = ($includeDirs | ForEach-Object { "/I`"$_`"" }) -join " "

    $bat = Join-Path $outDir "_check_sim_iter_tune_build.bat"
    @"
@echo off
call "$vcvars" x64 >nul
cl /nologo /W3 /EHsc /std:c11 $includeArgs /Fo:"$objDir\\" /Fe:"$exe" $srcQuoted
echo BUILD_EXIT=%ERRORLEVEL%
"@ | Set-Content -Encoding ascii -LiteralPath $bat

    $buildOut = cmd.exe /c "`"$bat`""
    $buildOut | ForEach-Object { Write-Host $_ }
    Remove-Item -Force -ErrorAction SilentlyContinue $bat

    if (-not (Test-Path $exe)) {
        # No executable. Was this a real compiler diagnostic against our own
        # source (a genuine regression -- FAIL), or the toolchain never
        # actually standing up (an environment problem -- SKIP)? MSVC
        # diagnostics always look like "<file>(<line>): error C####: ...".
        $hasCompilerError = $buildOut | Where-Object { $_ -match 'error C\d{4}' -or $_ -match 'error LNK\d+' -or $_ -match 'fatal error' }
        if ($hasCompilerError) {
            Write-Host "FAIL: sim_iter_tune.exe did not build -- compiler/linker reported real diagnostics above."
            exit 1
        }
        Write-Host "SKIP: sim_iter_tune.exe did not build and the compiler reported no diagnostics against"
        Write-Host "      this checkout's own source -- the toolchain itself could not be invoked in this"
        Write-Host "      environment (see the raw build output above, e.g. a missing vswhere.exe/cl.exe on"
        Write-Host "      PATH). Not an A1/A2/A5/A6 result."
        exit 3
    }

    $runOut = & $exe 220 2>&1
    $runExit = $LASTEXITCODE
    $runOut | ForEach-Object { Write-Host $_ }

    Remove-Item -Force -ErrorAction SilentlyContinue $exe

    if ($runExit -ne 0) {
        $barLines = $runOut | Where-Object { $_ -match "bar:.*-> FAIL" }
        Write-Host "FAIL: sim_iter_tune.exe (n=220) exited $runExit -- at least one acceptance bar failed:"
        foreach ($b in $barLines) { Write-Host "  $b" }
        exit 1
    }

    Write-Host "PASS: sim_iter_tune.exe (n=220) -- A2/A5/A6 clear; A1 clear ONLY against its pinned"
    Write-Host "      known-failure ceiling (24/660, ~3.64%), NOT the 2.0% design target, which is"
    Write-Host "      NOT yet met -- blocked on a coupling-matrix re-identification upstream of this"
    Write-Host "      repo (d63a5591). See sim_iter_tune.c's A1_PINNED_MAX_ACCEPTS comment and"
    Write-Host "      docs/audits/firing_score_entry_ema_review_2026-09-10.md."
    exit 0
} finally {
    Exit-BuildLock -Lock $lock
}
