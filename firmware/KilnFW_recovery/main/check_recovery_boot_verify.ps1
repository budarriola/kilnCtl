# check_recovery_boot_verify.ps1 -- builds and runs test_recovery_boot_verify.c (host
# test of recovery_boot_verify.c, the esp_ota_set_boot_partition() read-back mirrored
# from KilnFW's boot_partition_verify.c) under MSVC, then runs negative tests: each
# mutant (a scratch copy of recovery_boot_verify.c with one rule broken) must make the
# same test binary FAIL. A second, source-level guard: both set-boot sites in
# recovery_http.c must call the verified wrapper and never esp_ota_set_boot_partition().
#
# Contract (tools/run_all_checks.ps1): exit 0 PASS, exit 3 SKIP (no MSVC),
# anything else FAIL. Scratch is PID-keyed under $env:TEMP, deleted in finally.
$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
. (Join-Path $here "..\..\..\tools\build_gate.ps1")

# Source-level guard (cheap, no build): recovery_http.c must not call the raw setter.
$http = (Get-Content (Join-Path $here "recovery_http.c") -Raw) -replace "(?m)//.*$", ""  # ignore line comments
if ($http -match "(?<![A-Za-z_])esp_ota_set_boot_partition\s*\(") {
    Write-Host "FAIL: recovery_http.c calls esp_ota_set_boot_partition() directly -- use recovery_boot_partition_set_and_verify()."
    exit 1
}
$wrapperCalls = ([regex]::Matches($http, "recovery_boot_partition_set_and_verify\s*\(")).Count
if ($wrapperCalls -lt 2) {
    Write-Host "FAIL: expected >= 2 recovery_boot_partition_set_and_verify() call sites in recovery_http.c (upload + exit), found $wrapperCalls."
    exit 1
}

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

$work = Join-Path $env:TEMP "recovery_boot_verify_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null
$stubs = Join-Path $here "host_stubs"

function Build-And-Run {
    param([string]$Impl, [string]$Tag)
    $exe = Join-Path $work "t_$Tag.exe"
    $obj = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $obj -Force | Out-Null
    $test = Join-Path $here "test_recovery_boot_verify.c"
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && call `"$vcvars`" x64 >nul && cl /nologo /W3 /WX /std:c11 /I`"$stubs`" /I`"$here`" `"$test`" `"$Impl`" /Fe:`"$exe`" /Fo:`"$obj\\`" /Fd:`"$obj\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_boot_verify"
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

function Test-Mutant {
    param([string]$Needle, [string]$Replacement, [string]$Tag)
    $orig = Join-Path $here "recovery_boot_verify.c"
    $src = Get-Content $orig -Raw
    $mutant = $src.Replace($Needle, $Replacement)
    if ($mutant -eq $src) {
        throw "negative test ${Tag}: could not find '$Needle' in recovery_boot_verify.c to mutate -- update this check."
    }
    $mpath = Join-Path $work ("mutant_" + $Tag + "_recovery_boot_verify.c")
    Set-Content -Path $mpath -Value $mutant -Encoding ascii
    $bad = Build-And-Run -Impl $mpath -Tag $Tag
    if ($bad.Exit -eq 0) {
        throw "NEGATIVE TEST FAILED ($Tag): the mutant still passed -- the host test is vacuous for it."
    }
}

try {
    $good = Build-And-Run -Impl (Join-Path $here "recovery_boot_verify.c") -Tag "real"
    Write-Host $good.Output
    if ($good.Exit -ne 0) { throw "test_recovery_boot_verify reported failures (exit $($good.Exit))." }
    if ($good.Output -notmatch "RESULT pass=(\d+) fail=0") {
        throw "test_recovery_boot_verify never printed a passing RESULT line."
    }
    $passCount = [int]$Matches[1]
    if ($passCount -lt 12) { throw "only $passCount assertions ran -- test looks gutted." }

    Test-Mutant -Needle "if (!recovery_boot_partition_matches(requested, actual)) {" -Replacement "if (0) {" -Tag "noreadback"
    Test-Mutant -Needle "requested->address == actual->address &&" -Replacement "" -Tag "noaddr"
    Test-Mutant -Needle "requested->subtype == actual->subtype;" -Replacement "1;" -Tag "nosubtype"
    Test-Mutant -Needle "return ESP_ERR_INVALID_STATE;" -Replacement "return ESP_OK;" -Tag "mismatchok"
    Test-Mutant -Needle "if (err != ESP_OK) {" -Replacement "if (0) {" -Tag "seterr"
    Test-Mutant -Needle "if (requested == NULL) {" -Replacement "if (0) {" -Tag "nullreq"

    Write-Host "check_recovery_boot_verify: PASS ($passCount assertions; negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
