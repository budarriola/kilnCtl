# check_recovery_image_check.ps1 -- builds and runs test_recovery_image_check.c
# (host test of recovery_image_check.c: ESP image first-chunk validation and
# boot_guard record decode) under MSVC, then runs a negative test: a scratch
# copy of recovery_image_check.c with the project-name comparison broken must
# make the same test binary FAIL, else the test is vacuous.
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
# vcvarsall runs ONCE here, outside the build gate; the gate then covers only cl.
Import-KilnVcvarsEnv -Vcvars $vcvars

$work = Join-Path $env:TEMP "recovery_image_check_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

function Build-And-Run {
    param([string]$ImplC, [string]$Tag)
    $exe = Join-Path $work "t_$Tag.exe"
    $obj = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $obj -Force | Out-Null
    $test = Join-Path $here "test_recovery_image_check.c"
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && cl /nologo /W3 /WX /std:c11 /I`"$here`" `"$test`" `"$ImplC`" /Fe:`"$exe`" /Fo:`"$obj\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_image_check" -Lane light
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

try {
    $impl = Join-Path $here "recovery_image_check.c"
    $good = Build-And-Run -ImplC $impl -Tag "real"
    Write-Host $good.Output
    if ($good.Exit -ne 0) { throw "test_recovery_image_check reported failures (exit $($good.Exit))." }
    if ($good.Output -notmatch "RESULT pass=(\d+) fail=0") {
        throw "test_recovery_image_check never printed a passing RESULT line."
    }
    $passCount = [int]$Matches[1]
    if ($passCount -lt 30) { throw "only $passCount assertions ran -- test looks gutted." }

    # Negative test: break the project-name comparison in a scratch copy.
    $src = Get-Content $impl -Raw
    $needle = 'memcmp(name, expected_project, n) != 0'
    $mutant = $src.Replace($needle, '0')
    if ($mutant -eq $src) {
        throw "negative test: could not find '$needle' in recovery_image_check.c to mutate -- update this check."
    }
    $mpath = Join-Path $work "mutant_recovery_image_check.c"
    Set-Content -Path $mpath -Value $mutant -Encoding ascii
    # The mutant #includes the real header by name; the include dir covers it.
    $bad = Build-And-Run -ImplC $mpath -Tag "mutant"
    if ($bad.Exit -eq 0) {
        throw "NEGATIVE TEST FAILED: the project-name mutant still passed -- the host test is vacuous for project_name."
    }
    Write-Host "check_recovery_image_check: PASS ($passCount assertions; negative test mutant failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
