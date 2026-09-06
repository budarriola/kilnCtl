# test_host_fakes.ps1 -- Phase 2 host-test runner for
# firmware/hwAbstraction/host/{fake_gpio,fake_adc,fake_uart,fake_spi,fake_i2c}.c.
#
# Compiles each fake plus its standalone test .c under MSVC (same
# vswhere/vcvarsall discovery as compile_headers.ps1 in this directory),
# runs the resulting .exe, and parses its "RESULT pass=N fail=N" line.
#
# Also runs FOUR negative tests end to end (CLAUDE.md / the plan's "negative-
# test every fake"): fake_gpio.c's hal_gpio_init_out is deliberately broken
# in a scratch copy (direction-then-level instead of the required
# latch-before-direction level-then-direction), fake_spi.c's async-pool-
# exhaustion wedge latch is dropped, and fake_i2c.c's NACK-on-transfer status
# is flipped to HAL_OK -- each mutant is built against the SAME test binary
# and run again. If a mutant does not make its test fail, the corresponding
# assertion is proven vacuous and this script fails loud.
#
# fake_kv/fake_time/fake_flash are not covered here: as of this pass
# firmware/hwAbstraction/interface/ has no hal_kv.h/hal_time.h/hal_flash.h
# (only hal_status/hal_gpio/hal_adc/hal_uart/hal_spi/hal_i2c exist), and
# host fakes must follow an existing interface header rather than invent
# one -- see firmware/hwAbstraction/host/README.md.
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
    @{ Name = "fake_uart"; Fake = "fake_uart.c"; Test = "test_fake_uart.c" },
    @{ Name = "fake_spi";  Fake = "fake_spi.c";  Test = "test_fake_spi.c" },
    @{ Name = "fake_i2c";  Fake = "fake_i2c.c";  Test = "test_fake_i2c.c" }
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

# --- 3) Negative test: mutate fake_spi's async-pool-exhaustion wedge latch ---
# Proves test_fake_spi.c's wedge assertion can actually fail: dropping the
# `b->wedged = true;` line means HAL_NO_MEM is still returned on pool
# exhaustion but hal_spi_bus_is_wedged() never reports it, which the plan's
# "distinct side effects (wedge latch vs not...)" spec forbids.
Write-Host "`n--- Negative test: fake_spi async pool exhaustion wedge latch ---"

$spiGoodBlock = (@'
    if (free_pending < 0) {
        /* Pool exhaustion, distinct from a timeout: latches the wedge --
         * see hal_spi_bus_is_wedged's contract in fake_spi.h. */
        b->wedged = true;
        return HAL_NO_MEM;
    }
'@) -replace "`r`n", "`n"

$spiMutantBlock = (@'
    if (free_pending < 0) {
        /* Pool exhaustion, distinct from a timeout: latches the wedge --
         * see hal_spi_bus_is_wedged's contract in fake_spi.h. */
        return HAL_NO_MEM;
    }
'@) -replace "`r`n", "`n"

$spiOrigContent = (Get-Content (Join-Path $hostDir "fake_spi.c") -Raw) -replace "`r`n", "`n"
if (-not $spiOrigContent.Contains($spiGoodBlock)) {
    $failures += "Negative test setup FAILED: expected wedge-latch block not found verbatim in fake_spi.c -- source drifted from what this script mutates. Update spiGoodBlock/spiMutantBlock together with fake_spi.c."
} else {
    $spiMutantContent = $spiOrigContent.Replace($spiGoodBlock, $spiMutantBlock)
    $spiMutantSrc = Join-Path $workDir "fake_spi_mutant.c"
    Set-Content -Path $spiMutantSrc -Value $spiMutantContent -Encoding ASCII -NoNewline

    $spiMutantExe = Join-Path $workDir "fake_spi_mutant.exe"
    $r = Invoke-ClLink -SourceFiles @($spiMutantSrc, (Join-Path $here "test_fake_spi.c"), (Join-Path $ifaceDir "hal_status.c")) `
        -OutExe $spiMutantExe -IncludeDirs @($ifaceDir, $hostDir)
    if ($r.ExitCode -ne 0) {
        $failures += "Negative test: mutant fake_spi.c failed to COMPILE (expected it to compile and fail the test at runtime instead):`n$($r.Output)"
    } else {
        $run = Run-Exe -ExePath $spiMutantExe
        $parsed = Parse-Result -Output $run.Output
        if (-not $parsed.Matched) {
            $failures += "Negative test: mutant fake_spi test binary produced no RESULT line:`n$($run.Output)"
        } elseif ($run.ExitCode -eq 0 -and $parsed.Fail -eq 0) {
            $failures += "NEGATIVE TEST FAILED: the un-latched-wedge mutant passed test_fake_spi.c cleanly -- the wedge-on-pool-exhaustion assertion does not actually catch this bug."
        } else {
            Write-Host "OK   negative test: fake_spi wedge-latch mutant correctly fails test_fake_spi.c (pass=$($parsed.Pass) fail=$($parsed.Fail))"
            Write-Host "     mutant failure detail:`n$($run.Output)"
        }
    }
}

# --- 4) Negative test: mutate fake_i2c's NACK-on-transfer status ---
# Proves test_fake_i2c.c's HAL_IO-on-scripted-NACK assertion can actually
# fail: returning HAL_OK instead of HAL_IO would make a scripted NACK
# invisible to a caller of hal_i2c_transfer().
Write-Host "`n--- Negative test: fake_i2c NACK-on-transfer status ---"

$i2cGoodBlock = (@'
    if (s && s->nack) {
        return HAL_IO; /* NACK on a transfer, not a probe -- see hal_i2c.h */
    }
'@) -replace "`r`n", "`n"

$i2cMutantBlock = (@'
    if (s && s->nack) {
        return HAL_OK; /* NACK on a transfer, not a probe -- see hal_i2c.h */
    }
'@) -replace "`r`n", "`n"

$i2cOrigContent = (Get-Content (Join-Path $hostDir "fake_i2c.c") -Raw) -replace "`r`n", "`n"
if (-not $i2cOrigContent.Contains($i2cGoodBlock)) {
    $failures += "Negative test setup FAILED: expected NACK-on-transfer block not found verbatim in fake_i2c.c -- source drifted from what this script mutates. Update i2cGoodBlock/i2cMutantBlock together with fake_i2c.c."
} else {
    $i2cMutantContent = $i2cOrigContent.Replace($i2cGoodBlock, $i2cMutantBlock)
    $i2cMutantSrc = Join-Path $workDir "fake_i2c_mutant.c"
    Set-Content -Path $i2cMutantSrc -Value $i2cMutantContent -Encoding ASCII -NoNewline

    $i2cMutantExe = Join-Path $workDir "fake_i2c_mutant.exe"
    $r = Invoke-ClLink -SourceFiles @($i2cMutantSrc, (Join-Path $here "test_fake_i2c.c"), (Join-Path $ifaceDir "hal_status.c")) `
        -OutExe $i2cMutantExe -IncludeDirs @($ifaceDir, $hostDir)
    if ($r.ExitCode -ne 0) {
        $failures += "Negative test: mutant fake_i2c.c failed to COMPILE (expected it to compile and fail the test at runtime instead):`n$($r.Output)"
    } else {
        $run = Run-Exe -ExePath $i2cMutantExe
        $parsed = Parse-Result -Output $run.Output
        if (-not $parsed.Matched) {
            $failures += "Negative test: mutant fake_i2c test binary produced no RESULT line:`n$($run.Output)"
        } elseif ($run.ExitCode -eq 0 -and $parsed.Fail -eq 0) {
            $failures += "NEGATIVE TEST FAILED: the NACK-returns-HAL_OK mutant passed test_fake_i2c.c cleanly -- the NACK-on-transfer status assertion does not actually catch this bug."
        } else {
            Write-Host "OK   negative test: fake_i2c NACK-status mutant correctly fails test_fake_i2c.c (pass=$($parsed.Pass) fail=$($parsed.Fail))"
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
