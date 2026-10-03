# check_recovery_auth.ps1 -- builds and runs test_recovery_auth.c (host test of
# the recovery image's pure auth pieces: recovery_auth.c secret selection and
# fallback derivation format, plus ota_auth.c nonce/lockout behaviour) under
# MSVC, then runs negative tests: each mutant (a scratch copy of the source
# with one rule broken) must make the same test binary FAIL, else the test is
# vacuous for that rule.
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

$work = Join-Path $env:TEMP "recovery_auth_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

# $Impls: full paths of the .c files linked with the test.
function Build-And-Run {
    param([string[]]$Impls, [string]$Tag)
    $exe = Join-Path $work "t_$Tag.exe"
    $obj = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $obj -Force | Out-Null
    $test = Join-Path $here "test_recovery_auth.c"
    $srcs = ($Impls | ForEach-Object { "`"$_`"" }) -join " "
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && call `"$vcvars`" x64 >nul && cl /nologo /W3 /WX /std:c11 /I`"$here`" `"$test`" $srcs /Fe:`"$exe`" /Fo:`"$obj\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_auth"
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

# Mutant: replace $Needle with $Replacement in $File (basename in $here), link
# with the real versions of $Others, expect the test to FAIL.
function Test-Mutant {
    param([string]$File, [string]$Needle, [string]$Replacement, [string[]]$Others, [string]$Tag)
    $orig = Join-Path $here $File
    $src = Get-Content $orig -Raw
    $mutant = $src.Replace($Needle, $Replacement)
    if ($mutant -eq $src) {
        throw "negative test ${Tag}: could not find '$Needle' in $File to mutate -- update this check."
    }
    $mpath = Join-Path $work ("mutant_" + $Tag + "_" + $File)
    Set-Content -Path $mpath -Value $mutant -Encoding ascii
    $impls = @($mpath) + ($Others | ForEach-Object { Join-Path $here $_ })
    $bad = Build-And-Run -Impls $impls -Tag $Tag
    if ($bad.Exit -eq 0) {
        throw "NEGATIVE TEST FAILED ($Tag): the mutant still passed -- the host test is vacuous for it."
    }
}

try {
    $auth = Join-Path $here "recovery_auth.c"
    $otaAuth = Join-Path $here "ota_auth.c"
    $good = Build-And-Run -Impls @($auth, $otaAuth) -Tag "real"
    Write-Host $good.Output
    if ($good.Exit -ne 0) { throw "test_recovery_auth reported failures (exit $($good.Exit))." }
    if ($good.Output -notmatch "RESULT pass=(\d+) fail=0") {
        throw "test_recovery_auth never printed a passing RESULT line."
    }
    $passCount = [int]$Matches[1]
    if ($passCount -lt 60) { throw "only $passCount assertions ran -- test looks gutted." }

    $o = @("ota_auth.c")
    $r = @("recovery_auth.c")
    # Short ap_pass (< 8) must not be usable (would start a weak/failing WPA2 AP).
    Test-Mutant -File "recovery_auth.c" -Needle "n >= RAUTH_AP_PASS_MIN" -Replacement "n >= 1" `
        -Others $o -Tag "minlen"
    # Build key must feed the preimage.
    Test-Mutant -File "recovery_auth.c" -Needle "memcpy(out + plen, build_key, klen);" `
        -Replacement "(void)build_key;" -Others $o -Tag "key"
    # Query must be signed.
    Test-Mutant -File "recovery_auth.c" -Needle "memcpy(out + n, query, query_len);" `
        -Replacement "(void)query;" -Others $o -Tag "query"
    # Ring: a client must reuse its own slot.
    Test-Mutant -File "recovery_auth.c" -Needle "pick = rauth_ring_find(r, client);" `
        -Replacement "pick = NULL;" -Others $o -Tag "ringown"
    # Ring: eviction must take the OLDEST nonce.
    Test-Mutant -File "recovery_auth.c" -Needle "- pick->seq) < 0" `
        -Replacement "- pick->seq) > 0" -Others $o -Tag "ringold"
    # Ring: a spent/expired slot must be reclaimed before a live one is evicted.
    Test-Mutant -File "recovery_auth.c" -Needle "now_ms) != OTA_AUTH_NONCE_OK) {" `
        -Replacement "now_ms) == 12345) {" -Others $o -Tag "ringspent"
    # Nonce: single use.
    Test-Mutant -File "ota_auth.c" -Needle "s->used = true;" -Replacement "s->used = false;" `
        -Others $r -Tag "reuse"
    # Nonce: 30 s expiry.
    Test-Mutant -File "ota_auth.c" -Needle "> OTA_AUTH_NONCE_EXPIRY_MS)" `
        -Replacement "> OTA_AUTH_NONCE_EXPIRY_MS * 100u)" -Others $r -Tag "expiry"
    # Lockout: doubling backoff.
    Test-Mutant -File "ota_auth.c" -Needle "backoff_ms *= 2u;" -Replacement "backoff_ms *= 1u;" `
        -Others $r -Tag "backoff"
    # Lockout: 15 min cap.
    # (both the in-loop clamp and the final clamp name the macro, so replace every use)
    Test-Mutant -File "ota_auth.c" -Needle "OTA_AUTH_LOCKOUT_MAX_MS" `
        -Replacement "0x7FFFFFFFu" -Others $r -Tag "cap"

    Write-Host "check_recovery_auth: PASS ($passCount assertions; negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
