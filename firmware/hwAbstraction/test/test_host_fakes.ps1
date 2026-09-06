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
# fake_kv and fake_time landed once hal_kv.h/hal_time.h existed (commit
# 620c8ca) and are covered below, each with its own negative test (fake_kv's
# commit-durability contract, fake_time's delay_ms-must-advance-the-clock
# contract). fake_flash is still not covered here: interface/ has no
# hal_flash.h yet, and a host fake must follow an existing interface header
# rather than invent one -- see firmware/hwAbstraction/host/README.md.
#
# Usage: powershell -ExecutionPolicy Bypass -File test_host_fakes.ps1

$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
$hostDir = Join-Path (Split-Path -Parent $here) "host"
$ifaceDir = Join-Path (Split-Path -Parent $here) "interface"
$commonDir = Join-Path (Split-Path -Parent $here) "common"
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
    @{ Name = "fake_i2c";  Fake = "fake_i2c.c";  Test = "test_fake_i2c.c" },
    @{ Name = "fake_kv";   Fake = "fake_kv.c";   Test = "test_fake_kv.c" },
    @{ Name = "fake_time"; Fake = "fake_time.c"; Test = "test_fake_time.c" }
)

foreach ($c in $cases) {
    $fakeSrc = Join-Path $hostDir $c.Fake
    $testSrc = Join-Path $here $c.Test
    $exe = Join-Path $workDir "$($c.Name).exe"
    $r = Invoke-ClLink -SourceFiles @($fakeSrc, $testSrc, (Join-Path $commonDir "hal_status.c")) `
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
    $r = Invoke-ClLink -SourceFiles @($mutantSrc, (Join-Path $here "test_fake_gpio.c"), (Join-Path $commonDir "hal_status.c")) `
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
    $r = Invoke-ClLink -SourceFiles @($spiMutantSrc, (Join-Path $here "test_fake_spi.c"), (Join-Path $commonDir "hal_status.c")) `
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
    $r = Invoke-ClLink -SourceFiles @($i2cMutantSrc, (Join-Path $here "test_fake_i2c.c"), (Join-Path $commonDir "hal_status.c")) `
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

# --- 5) Negative test: mutate fake_kv's commit-durability contract ---
# Proves test_fake_kv.c's durability assertion can actually fail: commit()
# is supposed to merge pending writes into the committed store so they
# survive fake_kv_simulate_power_loss(); if commit() is short-circuited to a
# no-op, hal_kv.h's "nothing is durable until hal_kv_commit()" contract is
# violated in the OTHER direction -- a committed write silently reverts to
# never-happened after a simulated reset, which is exactly the durability
# bug this fake exists to catch on host.
Write-Host "`n--- Negative test: fake_kv commit-durability contract ---"

$kvGoodBlock = (@'
    if (s_next_write_fail_armed) {
        s_next_write_fail_armed = false;
        return s_next_write_fail_status;
    }

    /* Commit flushes the WHOLE partition's page in real NVS, not just this
     * handle's namespace -- merge every namespace's pending writes. */
    fake_kv_partition_t *part = &s_partitions[hs->partition_slot];
'@) -replace "`r`n", "`n"

$kvMutantBlock = (@'
    if (s_next_write_fail_armed) {
        s_next_write_fail_armed = false;
        return s_next_write_fail_status;
    }

    /* MUTANT: commit no longer merges pending into committed -- durability
     * contract silently broken. */
    return HAL_OK;

    /* Commit flushes the WHOLE partition's page in real NVS, not just this
     * handle's namespace -- merge every namespace's pending writes. */
    fake_kv_partition_t *part = &s_partitions[hs->partition_slot];
'@) -replace "`r`n", "`n"

$kvOrigContent = (Get-Content (Join-Path $hostDir "fake_kv.c") -Raw) -replace "`r`n", "`n"
if (-not $kvOrigContent.Contains($kvGoodBlock)) {
    $failures += "Negative test setup FAILED: expected commit-merge block not found verbatim in fake_kv.c -- source drifted from what this script mutates. Update kvGoodBlock/kvMutantBlock together with fake_kv.c."
} else {
    $kvMutantContent = $kvOrigContent.Replace($kvGoodBlock, $kvMutantBlock)
    $kvMutantSrc = Join-Path $workDir "fake_kv_mutant.c"
    Set-Content -Path $kvMutantSrc -Value $kvMutantContent -Encoding ASCII -NoNewline

    $kvMutantExe = Join-Path $workDir "fake_kv_mutant.exe"
    $r = Invoke-ClLink -SourceFiles @($kvMutantSrc, (Join-Path $here "test_fake_kv.c"), (Join-Path $commonDir "hal_status.c")) `
        -OutExe $kvMutantExe -IncludeDirs @($ifaceDir, $hostDir)
    if ($r.ExitCode -ne 0) {
        $failures += "Negative test: mutant fake_kv.c failed to COMPILE (expected it to compile and fail the test at runtime instead):`n$($r.Output)"
    } else {
        $run = Run-Exe -ExePath $kvMutantExe
        $parsed = Parse-Result -Output $run.Output
        if (-not $parsed.Matched) {
            $failures += "Negative test: mutant fake_kv test binary produced no RESULT line:`n$($run.Output)"
        } elseif ($run.ExitCode -eq 0 -and $parsed.Fail -eq 0) {
            $failures += "NEGATIVE TEST FAILED: the no-op-commit mutant passed test_fake_kv.c cleanly -- the commit-durability assertion does not actually catch this bug."
        } else {
            Write-Host "OK   negative test: fake_kv no-op-commit mutant correctly fails test_fake_kv.c (pass=$($parsed.Pass) fail=$($parsed.Fail))"
            Write-Host "     mutant failure detail:`n$($run.Output)"
        }
    }
}

# --- 6) Negative test: mutate fake_time's delay_ms-advances-the-clock contract ---
# Proves test_fake_time.c's delay_ms assertion can actually fail: hal_time.h
# requires hal_time_delay_ms to be the vTaskDelay/sleep_ms replacement, i.e.
# it must advance the (fake) clock. If it becomes a pure no-op (no real
# sleep, per this fake's "never real sleeps" contract, but also no clock
# advance), a caller measuring elapsed time across a delay would see zero
# elapsed time on host while hardware shows real progress -- silently
# reintroducing the idealized-input bug class this fake exists to avoid.
Write-Host "`n--- Negative test: fake_time delay_ms clock advance ---"

$timeGoodBlock = (@'
hal_status_t hal_time_delay_ms(uint32_t ms)
{
    /* Never a real sleep on host -- advances the fake clock instead, per
     * hal_time.h's "vTaskDelay/sleep_ms replacement" and this fake's
     * "never real sleeps" contract. */
    s_clock_us += (uint64_t)ms * 1000u;
    return HAL_OK;
}
'@) -replace "`r`n", "`n"

$timeMutantBlock = (@'
hal_status_t hal_time_delay_ms(uint32_t ms)
{
    /* MUTANT: delay_ms no longer advances the clock. */
    (void)ms;
    return HAL_OK;
}
'@) -replace "`r`n", "`n"

$timeOrigContent = (Get-Content (Join-Path $hostDir "fake_time.c") -Raw) -replace "`r`n", "`n"
if (-not $timeOrigContent.Contains($timeGoodBlock)) {
    $failures += "Negative test setup FAILED: expected delay_ms block not found verbatim in fake_time.c -- source drifted from what this script mutates. Update timeGoodBlock/timeMutantBlock together with fake_time.c."
} else {
    $timeMutantContent = $timeOrigContent.Replace($timeGoodBlock, $timeMutantBlock)
    $timeMutantSrc = Join-Path $workDir "fake_time_mutant.c"
    Set-Content -Path $timeMutantSrc -Value $timeMutantContent -Encoding ASCII -NoNewline

    $timeMutantExe = Join-Path $workDir "fake_time_mutant.exe"
    $r = Invoke-ClLink -SourceFiles @($timeMutantSrc, (Join-Path $here "test_fake_time.c"), (Join-Path $commonDir "hal_status.c")) `
        -OutExe $timeMutantExe -IncludeDirs @($ifaceDir, $hostDir)
    if ($r.ExitCode -ne 0) {
        $failures += "Negative test: mutant fake_time.c failed to COMPILE (expected it to compile and fail the test at runtime instead):`n$($r.Output)"
    } else {
        $run = Run-Exe -ExePath $timeMutantExe
        $parsed = Parse-Result -Output $run.Output
        if (-not $parsed.Matched) {
            $failures += "Negative test: mutant fake_time test binary produced no RESULT line:`n$($run.Output)"
        } elseif ($run.ExitCode -eq 0 -and $parsed.Fail -eq 0) {
            $failures += "NEGATIVE TEST FAILED: the non-advancing delay_ms mutant passed test_fake_time.c cleanly -- the delay-advances-the-clock assertion does not actually catch this bug."
        } else {
            Write-Host "OK   negative test: fake_time non-advancing delay_ms mutant correctly fails test_fake_time.c (pass=$($parsed.Pass) fail=$($parsed.Fail))"
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
