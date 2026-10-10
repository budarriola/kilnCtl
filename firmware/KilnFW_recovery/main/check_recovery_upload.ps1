# check_recovery_upload.ps1 -- builds and runs test_recovery_upload.c (host test
# of recovery_upload.c's streaming loop and ESP OTA sink, with the ESP-IDF
# calls stubbed by host_stubs/) under MSVC, then runs negative tests: each
# mutant (a scratch copy of recovery_upload.c with one rule broken) must make
# the same test binary FAIL, else the test is vacuous for that rule.
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

$work = Join-Path $env:TEMP "recovery_upload_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

$stubs = Join-Path $here "host_stubs"

# $Impls: full paths of the .c files linked with the test.
function Build-And-Run {
    param([string[]]$Impls, [string]$Tag)
    $exe = Join-Path $work "t_$Tag.exe"
    $obj = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $obj -Force | Out-Null
    $test = Join-Path $here "test_recovery_upload.c"
    $srcs = ($Impls | ForEach-Object { "`"$_`"" }) -join " "
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && cl /nologo /W3 /WX /std:c11 /I`"$stubs`" /I`"$here`" `"$test`" $srcs /Fe:`"$exe`" /Fo:`"$obj\\`" /Fd:`"$obj\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_upload" -Lane light
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

# Mutant: replace $Needle with $Replacement in recovery_upload.c (a "<NL>" token
# in either string stands for the file's own line ending), link with the real
# image-check implementation, expect the test to FAIL.
function Test-Mutant {
    param([string]$Needle, [string]$Replacement, [string]$Tag)
    $orig = Join-Path $here "recovery_upload.c"
    $src = Get-Content $orig -Raw
    $nl = if ($src.Contains("`r`n")) { "`r`n" } else { "`n" }
    $n = $Needle.Replace("<NL>", $nl)
    $r = $Replacement.Replace("<NL>", $nl)
    $mutant = $src.Replace($n, $r)
    if ($mutant -eq $src) {
        throw "negative test ${Tag}: could not find '$Needle' in recovery_upload.c to mutate -- update this check."
    }
    $mpath = Join-Path $work ("mutant_" + $Tag + "_recovery_upload.c")
    Set-Content -Path $mpath -Value $mutant -Encoding ascii
    $bad = Build-And-Run -Impls @($mpath, (Join-Path $here "recovery_image_check.c")) -Tag $Tag
    if ($bad.Exit -eq 0) {
        throw "NEGATIVE TEST FAILED ($Tag): the mutant still passed -- the host test is vacuous for it."
    }
}

try {
    $good = Build-And-Run -Impls @((Join-Path $here "recovery_upload.c"), (Join-Path $here "recovery_image_check.c")) -Tag "real"
    Write-Host $good.Output
    if ($good.Exit -ne 0) { throw "test_recovery_upload reported failures (exit $($good.Exit))." }
    if ($good.Output -notmatch "RESULT pass=(\d+) fail=0") {
        throw "test_recovery_upload never printed a passing RESULT line."
    }
    $passCount = [int]$Matches[1]
    if ($passCount -lt 60) { throw "only $passCount assertions ran -- test looks gutted." }

    # Length gate.
    Test-Mutant -Needle "total == 0 ||" -Replacement "total == 123456789u ||" -Tag "lenzero"
    Test-Mutant -Needle "|| total > cfg->max_len" -Replacement "|| total > cfg->max_len * 100u" -Tag "oversize"
    # First-chunk gate.
    Test-Mutant -Needle "if (vr != RIC_OK) {" -Replacement "if (vr == 12345) {" -Tag "firstgate"
    # Allocation floor.
    Test-Mutant -Needle "RECOVERY_UPLOAD_CHUNK + RECOVERY_INTERNAL_FLOOR_BYTES) {" `
        -Replacement "RECOVERY_UPLOAD_CHUNK) {" -Tag "floor"
    # recv timeouts: retry budget, and the reset on progress.
    Test-Mutant -Needle "++timeouts > RECV_TIMEOUT_RETRIES" -Replacement "++timeouts > 100" -Tag "retrybudget"
    # Overall upload deadline (slow-drip).
    Test-Mutant -Needle "if (esp_timer_get_time() > deadline_us) {" -Replacement "if (0) {" -Tag "deadline"
    Test-Mutant -Needle "timeouts = 0;<NL>        got += (size_t)n;" -Replacement "got += (size_t)n;" -Tag "noreset"
    # Short body.
    # (the mutant pretends the short read was complete; a bare "ignore the check" would loop forever)
    Test-Mutant -Needle "if (got < want) {" -Replacement "got = want;<NL>        if (0) {" -Tag "shortmid"
    Test-Mutant -Needle "if (got < first) {" -Replacement "if (got < first && first == 0xFFFFFFFFu) {" -Tag "shortfirst"
    # Abort on failure.
    Test-Mutant -Needle "sink->abort(sink->ctx);<NL>            *http_status = too_slow ? 504 : 400;" `
        -Replacement "*http_status = too_slow ? 504 : 400;" -Tag "abortmid"
    # Deadline reported as too slow, not as a lost connection.
    Test-Mutant -Needle "*too_slow = true; // distinct from a lost connection" `
        -Replacement "(void)0; // distinct from a lost connection" -Tag "tooslow"
    Test-Mutant -Needle "sink->abort(sink->ctx);<NL>            *http_status = 500;" `
        -Replacement "*http_status = 500;" -Tag "abortwrite"
    # ESP sink: esp_ota_end failure must be reported, handle released exactly once.
    Test-Mutant -Needle "return esp_ota_end(st->handle) == ESP_OK;" `
        -Replacement "(void)esp_ota_end(st->handle); return true;" -Tag "endok"
    Test-Mutant -Needle "st->begun = false; // esp_ota_end releases the handle whether or not it succeeds" `
        -Replacement "(void)0;" -Tag "begunflag"
    Test-Mutant -Needle "if (st->begun) {" -Replacement "if (1) {" -Tag "abortguard"
    Test-Mutant -Needle "return esp_ota_write(st->handle, data, len) == ESP_OK;" `
        -Replacement "(void)esp_ota_write(st->handle, data, len); return true;" -Tag "writeok"
    Test-Mutant -Needle "&st->handle) != ESP_OK) {" -Replacement "&st->handle) == 424242) {" -Tag "beginok"
    # Error response.
    Test-Mutant -Needle "(possibly huge) unread request body.<NL>    return ESP_FAIL;" `
        -Replacement "(possibly huge) unread request body.<NL>    return ESP_OK;" -Tag "sendfail"
    Test-Mutant -Needle 'httpd_resp_set_hdr(req, "Connection", "close");' `
        -Replacement 'httpd_resp_set_hdr(req, "Connection", "keep-alive");' -Tag "connclose"
    Test-Mutant -Needle 'case 413: status = "413 Payload Too Large"; break;' -Replacement "" -Tag "status413"

    # recovery_http.c read_body_exact (Pico push): only the overall deadline may set too_slow (504);
    # the no-progress stall must report a lost connection (400), like read_exact above.
    $rh = Get-Content (Join-Path $here "recovery_http.c") -Raw
    if ($rh -match '(?s)static size_t read_body_exact\(.*?\r?\n}\r?\n') { $fn = $Matches[0] } else { throw "read_body_exact not found in recovery_http.c -- update this check." }
    if (([regex]::Matches($fn, '\*too_slow = true')).Count -ne 1) {
        throw "read_body_exact must set too_slow only for the overall deadline, not the no-progress stall."
    }
    if ($fn -notmatch '(?s)esp_timer_get_time\(\) > deadline_us\) \{\s*\*too_slow = true') {
        throw "read_body_exact: the overall-deadline branch must be the one that sets too_slow."
    }

    Write-Host "check_recovery_upload: PASS ($passCount assertions; negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
