# check_recovery_apply.ps1 -- builds and runs test_recovery_apply.c (host test of
# recovery_apply.c, the recovery image's stage -> app copy core, with flash,
# SHA-256, image verification and the boot-partition switch injected) under
# MSVC, then runs negative tests: each mutant (a scratch copy of
# recovery_apply.c with one safety rule broken) must make the same test binary
# FAIL, else the test is vacuous for that rule.
#
# recovery_apply.c links the SAME stage_header.c / update_semver.c the
# application's stager uses (firmware/KilnFW/App/drivers/update), so the header
# format has one implementation; ota_image_crc32 is supplied by the test (the
# target uses the esp_rom wrapper).
#
# Contract (tools/run_all_checks.ps1): exit 0 PASS, exit 3 SKIP (no MSVC),
# anything else FAIL. Scratch is PID-keyed under $env:TEMP, deleted in finally.
$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
. (Join-Path $here "..\..\..\tools\build_gate.ps1")

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = $null
if (Test-Path $vswhere) {
    $installPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if ($installPath) {
        $candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvarsall.bat"
        if (Test-Path $candidate) { $vcvars = $candidate }
    }
}
if (-not $vcvars) {
    $fallback = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    if (Test-Path $fallback) { $vcvars = $fallback }
}
if (-not $vcvars) {
    Write-Host "SKIP: vcvarsall.bat not found -- cannot build the host test with MSVC."
    exit 3
}

$work = Join-Path $env:TEMP "recovery_apply_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

$appDrivers = (Resolve-Path (Join-Path $here "..\..\KilnFW\App\drivers")).Path
$updateDir = Join-Path $appDrivers "update"
$httpDir = Join-Path $appDrivers "http"
foreach ($f in @("stage_header.c", "stage_header.h", "update_semver.c")) {
    if (-not (Test-Path (Join-Path $updateDir $f))) { throw "missing shared source $updateDir\$f" }
}
if (-not (Test-Path (Join-Path $httpDir "ota_image_crc.h"))) { throw "missing $httpDir\ota_image_crc.h" }

$shared = @(
    (Join-Path $here "recovery_image_check.c"),
    (Join-Path $updateDir "stage_header.c"),
    (Join-Path $updateDir "update_semver.c")
)

# $Impls: full paths of the .c files linked with the test.
function Build-And-Run {
    param([string[]]$Impls, [string]$Tag)
    $exe = Join-Path $work "t_$Tag.exe"
    $obj = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $obj -Force | Out-Null
    $test = Join-Path $here "test_recovery_apply.c"
    $srcs = ($Impls | ForEach-Object { "`"$_`"" }) -join " "
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && call `"$vcvars`" x64 >nul && cl /nologo /W3 /WX /std:c11 /I`"$here`" /I`"$updateDir`" /I`"$httpDir`" `"$test`" $srcs /Fe:`"$exe`" /Fo:`"$obj\\`" /Fd:`"$obj\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_apply"
    try {
        $ErrorActionPreference = "Continue"
        $bo = cmd /c $cmd 2>&1
        $bx = $LASTEXITCODE
        $ErrorActionPreference = "Stop"
    } finally {
        Exit-KilnBuildGate -Gate $gate
    }
    if ($bx -ne 0) {
        $bo | ForEach-Object { Write-Host $_ }
        throw "cl failed building the $Tag variant (exit $bx)."
    }
    $ErrorActionPreference = "Continue"
    $ro = cmd /c "`"$exe`" 2>&1"
    $rx = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    return @{ Exit = $rx; Output = ($ro -join "`n") }
}

# Mutant: apply every @(needle, replacement) pair to recovery_apply.c (a "<NL>"
# token stands for the file's own line ending), link with the real shared
# sources, expect the test to FAIL. Every needle must be present.
function Test-Mutant {
    param([string]$Tag, [string[][]]$Pairs)
    $orig = Join-Path $here "recovery_apply.c"
    $src = Get-Content $orig -Raw
    $nl = if ($src.Contains("`r`n")) { "`r`n" } else { "`n" }
    $mutant = $src
    foreach ($pair in $Pairs) {
        $n = $pair[0].Replace("<NL>", $nl)
        $r = $pair[1].Replace("<NL>", $nl)
        if (-not $mutant.Contains($n)) {
            throw "negative test ${Tag}: could not find '$($pair[0])' in recovery_apply.c to mutate -- update this check."
        }
        $mutant = $mutant.Replace($n, $r)
    }
    $mpath = Join-Path $work ("mutant_" + $Tag + "_recovery_apply.c")
    Set-Content -Path $mpath -Value $mutant -Encoding ascii
    $bad = Build-And-Run -Impls (@($mpath) + $shared) -Tag $Tag
    if ($bad.Exit -eq 0) {
        throw "NEGATIVE TEST FAILED ($Tag): the mutant still passed -- the host test is vacuous for it."
    }
}

try {
    $good = Build-And-Run -Impls (@((Join-Path $here "recovery_apply.c")) + $shared) -Tag "real"
    Write-Host $good.Output
    if ($good.Exit -ne 0) { throw "test_recovery_apply reported failures (exit $($good.Exit))." }
    if ($good.Output -notmatch "RESULT pass=(\d+) fail=0") {
        throw "test_recovery_apply never printed a passing RESULT line."
    }
    $passCount = [int]$Matches[1]
    if ($passCount -lt 90) { throw "only $passCount assertions ran -- test looks gutted." }

    # Header / state gates.
    Test-Mutant "installable" @(, @("if (!stage_header_is_installable(&hdr)) {", "if (0) {"))
    Test-Mutant "blankhdr" @(, @("if (hs == STAGE_HDR_BLANK) {", "if (0) {"))
    Test-Mutant "badhdr" @(, @("if (hs != STAGE_HDR_OK) {", "if (0) {"))
    # Staged bytes must match the header's sha256.
    Test-Mutant "stagehash" @(, @("if (!stage_header_sha256_matches(&hdr, sha)) {<NL>        return fail(prog, RECOVERY_APPLY_ERR_STAGE_HASH);",
                                  "if (0) {<NL>        return fail(prog, RECOVERY_APPLY_ERR_STAGE_HASH);"))
    # Image identity and size.
    Test-Mutant "identity" @(, @("if (vr != RIC_OK) {", "if (vr == 12345) {"))
    Test-Mutant "toobig" @(@("if (len > io->app_size) {", "if (len > io->app_size * 100u) {"),
                           @("if (vr == RIC_OVERSIZE) {", "if (vr == 12345) {"))
    # Copy mechanics.
    Test-Mutant "eraseahead" @(, @("if (off >= erased) {", "if (off == 0) {"))
    Test-Mutant "writeoff" @(, @("io->app_write(io->ctx, off, scratch, n) != 0", "io->app_write(io->ctx, 0, scratch, n) != 0"))
    Test-Mutant "modflag" @(, @("prog->app_modified = true;", "(void)0;"))
    Test-Mutant "shaabort" @(, @("if (io->sha_abort) {", "if (0) {"))
    Test-Mutant "scratchlen" @(, @("scratch_len < RECOVERY_APPLY_CHUNK ||", "0 ||"))
    # Readback and verification of what landed in app.
    Test-Mutant "readbacksrc" @(, @("hash_region(io, io->app_read, 0, len, scratch, sha)",
                                    "hash_region(io, io->stage_read, STAGE_IMAGE_OFFSET, len, scratch, sha)"))
    Test-Mutant "apphash" @(, @("if (!stage_header_sha256_matches(&hdr, sha)) {<NL>        return fail(prog, RECOVERY_APPLY_ERR_APP_HASH);",
                                "if (0) {<NL>        return fail(prog, RECOVERY_APPLY_ERR_APP_HASH);"))
    Test-Mutant "appverify" @(, @("if (io->app_verify(io->ctx, len) != 0) {", "if (0) {"))
    # Boot partition: only after verify, failure honoured, never early.
    Test-Mutant "setbootok" @(, @("if (io->set_boot(io->ctx) != 0) {", "if (io->set_boot(io->ctx) == 12345) {"))
    Test-Mutant "bootearly" @(, @("set_phase(prog, RECOVERY_APPLY_VERIFYING);",
                                  "set_phase(prog, RECOVERY_APPLY_VERIFYING); (void)io->set_boot(io->ctx);"))
    # The stage header goes only after set_boot, only its own sector, and does go.
    Test-Mutant "eraseearly" @(, @("set_phase(prog, RECOVERY_APPLY_COPYING);",
                                   "set_phase(prog, RECOVERY_APPLY_COPYING); (void)io->stage_erase(io->ctx, 0, STAGE_HEADER_SECTOR);"))
    Test-Mutant "erasewide" @(, @("io->stage_erase(io->ctx, 0, STAGE_HEADER_SECTOR)", "io->stage_erase(io->ctx, 0, 65536u)"))
    Test-Mutant "noerase" @(, @("prog->stage_cleared = io->stage_erase(io->ctx, 0, STAGE_HEADER_SECTOR) == 0;",
                                "prog->stage_cleared = true;"))

    Write-Host "check_recovery_apply: PASS ($passCount assertions; negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
