# test_host_fakes.ps1 -- Phase 2 host-test runner for
# firmware/hwAbstraction/host/{fake_gpio,fake_adc,fake_uart}.c.
#
# Compiles each fake plus its standalone test .c under MSVC (same
# vswhere/vcvarsall discovery as compile_headers.ps1 in this directory),
# runs the resulting .exe, and parses its "RESULT pass=N fail=N" line.
#
# Also runs ONE negative test end to end (CLAUDE.md / the plan's "negative-
# test every fake"): fake_gpio.c's hal_gpio_init_out is deliberately broken
# in a scratch copy (direction-then-level instead of the required
# latch-before-direction level-then-direction) and the same test binary is
# rebuilt against the mutant and run again. If the mutant does not make
# test_fake_gpio fail, the order-checking assertion is proven vacuous and
# this script fails loud.
#
# Usage: powershell -ExecutionPolicy Bypass -File test_host_fakes.ps1

$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
$hostDir = Join-Path (Split-Path -Parent $here) "host"
$ifaceDir = Join-Path (Split-Path -Parent $here) "interface"
$workDir = Join-Path $here "_fakes_work"
if (Test-Path $workDir) { Remove-Item -Recurse -Force $workDir }
New-Item -ItemType Directory -Path $workDir | Out-Null

# --- vcvarsall discovery (mirrors compile_headers.ps1 in this directory) ---
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
    throw "vcvarsall.bat not found via vswhere or the known fallback path -- install MSVC Build Tools with the C++ workload, or update this script's fallback path."
}
Write-Host "Using vcvarsall.bat: $vcvars"

function Invoke-ClLink {
    param([string[]]$SourceFiles, [string]$OutExe, [string[]]$IncludeDirs)
    $incFlags = ($IncludeDirs | ForEach-Object { "/I`"$_`"" }) -join " "
    $srcList = ($SourceFiles | ForEach-Object { "`"$_`"" }) -join " "
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && call `"$vcvars`" x64 >nul && cl /nologo /W3 /std:c11 $incFlags $srcList /Fe:`"$OutExe`" /Fo:`"$workDir\\`""
    $out = cmd /c $cmd 2>&1
    return @{ ExitCode = $LASTEXITCODE; Output = ($out -join "`n") }
}

function Run-Exe {
    # Runs via cmd /c rather than PowerShell's native invocation: a test
    # binary's stderr FAIL lines (expected on the negative-test mutant)
    # would otherwise be wrapped as a terminating NativeCommandError under
    # $ErrorActionPreference = "Stop".
    param([string]$ExePath)
    $out = cmd /c "`"$ExePath`" 2>&1"
    $exit = $LASTEXITCODE
    return @{ ExitCode = $exit; Output = ($out -join "`n") }
}

function Parse-Result {
    param([string]$Output)
    if ($Output -match "RESULT pass=(\d+) fail=(\d+)") {
        return @{ Pass = [int]$Matches[1]; Fail = [int]$Matches[2]; Matched = $true }
    }
    return @{ Pass = 0; Fail = 0; Matched = $false }
}

$failures = @()
$totalPass = 0
$totalFail = 0

# --- 1) Normal build+run for each fake ---
$cases = @(
    @{ Name = "fake_gpio"; Fake = "fake_gpio.c"; Test = "test_fake_gpio.c" },
    @{ Name = "fake_adc";  Fake = "fake_adc.c";  Test = "test_fake_adc.c" },
    @{ Name = "fake_uart"; Fake = "fake_uart.c"; Test = "test_fake_uart.c" }
)

foreach ($c in $cases) {
    $fakeSrc = Join-Path $hostDir $c.Fake
    $testSrc = Join-Path $here $c.Test
    $exe = Join-Path $workDir "$($c.Name).exe"
    $r = Invoke-ClLink -SourceFiles @($fakeSrc, $testSrc, (Join-Path $ifaceDir "hal_status.c")) `
        -OutExe $exe -IncludeDirs @($ifaceDir, $hostDir)
    if ($r.ExitCode -ne 0) {
        $failures += "$($c.Name): compile failed:`n$($r.Output)"
        continue
    }
    $run = Run-Exe -ExePath $exe
    $parsed = Parse-Result -Output $run.Output
    if (-not $parsed.Matched) {
        $failures += "$($c.Name): test binary produced no RESULT line:`n$($run.Output)"
        continue
    }
    $totalPass += $parsed.Pass
    $totalFail += $parsed.Fail
    if ($run.ExitCode -ne 0 -or $parsed.Fail -gt 0) {
        $failures += "$($c.Name): FAILED (pass=$($parsed.Pass) fail=$($parsed.Fail)):`n$($run.Output)"
    } else {
        Write-Host "OK   $($c.Name): pass=$($parsed.Pass) fail=$($parsed.Fail)"
    }
}

# --- 2) Negative test: mutate fake_gpio's latch-before-direction order ---
# Proves test_fake_gpio.c's event-order assertion can actually fail, per
# CLAUDE.md's "negative-test every check"/every fake. Uses a scratch copy;
# the real fake_gpio.c on disk is never touched.
Write-Host "`n--- Negative test: fake_gpio latch-before-direction ---"

$goodBlock = (@'
    s_pins[num].level = idle_level;
    record(FAKE_GPIO_EV_SET_LEVEL, num, idle_level, HAL_GPIO_DIR_OUT, HAL_GPIO_PULL_NONE);
    s_pins[num].dir = HAL_GPIO_DIR_OUT;
    s_pins[num].initialized = true;
    record(FAKE_GPIO_EV_INIT_OUT, num, idle_level, HAL_GPIO_DIR_OUT, HAL_GPIO_PULL_NONE);
'@) -replace "`r`n", "`n"

$mutantBlock = (@'
    s_pins[num].dir = HAL_GPIO_DIR_OUT;
    s_pins[num].initialized = true;
    record(FAKE_GPIO_EV_INIT_OUT, num, idle_level, HAL_GPIO_DIR_OUT, HAL_GPIO_PULL_NONE);
    s_pins[num].level = idle_level;
    record(FAKE_GPIO_EV_SET_LEVEL, num, idle_level, HAL_GPIO_DIR_OUT, HAL_GPIO_PULL_NONE);
'@) -replace "`r`n", "`n"

# Normalize to LF so the comparison/replace is robust regardless of this
# script's own or fake_gpio.c's line-ending convention on checkout.
$origContent = (Get-Content (Join-Path $hostDir "fake_gpio.c") -Raw) -replace "`r`n", "`n"
if (-not $origContent.Contains($goodBlock)) {
    $failures += "Negative test setup FAILED: expected latch-before-direction block not found verbatim in fake_gpio.c -- source drifted from what this script mutates. Update goodBlock/mutantBlock together with fake_gpio.c."
} else {
    $mutantContent = $origContent.Replace($goodBlock, $mutantBlock)
    $mutantSrc = Join-Path $workDir "fake_gpio_mutant.c"
    Set-Content -Path $mutantSrc -Value $mutantContent -Encoding ASCII -NoNewline

    $mutantExe = Join-Path $workDir "fake_gpio_mutant.exe"
    $r = Invoke-ClLink -SourceFiles @($mutantSrc, (Join-Path $here "test_fake_gpio.c"), (Join-Path $ifaceDir "hal_status.c")) `
        -OutExe $mutantExe -IncludeDirs @($ifaceDir, $hostDir)
    if ($r.ExitCode -ne 0) {
        $failures += "Negative test: mutant fake_gpio.c failed to COMPILE (expected it to compile and fail the test at runtime instead):`n$($r.Output)"
    } else {
        $run = Run-Exe -ExePath $mutantExe
        $parsed = Parse-Result -Output $run.Output
        if (-not $parsed.Matched) {
            $failures += "Negative test: mutant test binary produced no RESULT line:`n$($run.Output)"
        } elseif ($run.ExitCode -eq 0 -and $parsed.Fail -eq 0) {
            $failures += "NEGATIVE TEST FAILED: the direction-then-level mutant passed test_fake_gpio.c cleanly -- the latch-before-direction event-order assertion does not actually catch this bug."
        } else {
            Write-Host "OK   negative test: direction-then-level mutant correctly fails test_fake_gpio.c (pass=$($parsed.Pass) fail=$($parsed.Fail))"
            Write-Host "     mutant failure detail:`n$($run.Output)"
        }
    }
}

Remove-Item -Recurse -Force $workDir -ErrorAction SilentlyContinue

Write-Host "`n=== Totals (normal runs only): pass=$totalPass fail=$totalFail ==="

if ($failures.Count -gt 0) {
    Write-Host "`n=== FAILURES ==="
    foreach ($f in $failures) { Write-Host $f }
    exit 1
}

Write-Host "`nAll hwAbstraction host-fake tests passed, including the negative test."
exit 0
