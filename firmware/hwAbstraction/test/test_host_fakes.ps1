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
# contract). fake_flash landed once hal_flash.h existed and is covered below
# too, with its own negative test (erase-before-program AND-semantics
# enforcement in fake_flash_program()).
#
# fake_scratch, fake_wdt, fake_pwm and fake_sysinfo (hal_scratch.h/hal_wdt.h/
# hal_pwm.h/hal_sysinfo.h) are covered below too, each with its own negative
# test: fake_scratch's slot-4 hard-reservation refusal, fake_wdt's
# advance-past-timeout fired-latch, fake_pwm's out-of-range duty rejection,
# fake_sysinfo's coredump-erase-clears-presence contract.
#
# NOTE (2026-09-05): attempting to add /WX (treat warnings as errors)
# alongside the existing /W3 broke the build -- fake_kv.c (host/fake_kv.c,
# lines 94/233/425) triggers C4996 on strncpy under MSVC's default runtime
# checks, and C4996 becomes a hard error under /WX. fake_kv.c is an existing
# file outside this pass's scope, so /WX was NOT added here; the four new
# fakes/tests in this pass compile clean under /WX on their own, but the
# script still passes only /W3 below since it builds every case with one
# shared flag set.
#
# Usage: powershell -ExecutionPolicy Bypass -File test_host_fakes.ps1

$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
$hostDir = Join-Path (Split-Path -Parent $here) "host"
$ifaceDir = Join-Path (Split-Path -Parent $here) "interface"
$commonDir = Join-Path (Split-Path -Parent $here) "common"
# PID-suffixed: a fixed "_fakes_work" name collides when two instances of
# this script run concurrently (seen as DirectoryNotFoundException around
# line 207 during run_all_checks.ps1, when one instance's cleanup deleted
# files the other instance was still using). Each instance gets its own
# directory and cleans up only that one.
$workDir = Join-Path $here "_fakes_work_$PID"
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
    @{ Name = "fake_time"; Fake = "fake_time.c"; Test = "test_fake_time.c" },
    @{ Name = "fake_flash"; Fake = "fake_flash.c"; Test = "test_fake_flash.c" },
    @{ Name = "fake_scratch"; Fake = "fake_scratch.c"; Test = "test_fake_scratch.c" },
    @{ Name = "fake_wdt"; Fake = "fake_wdt.c"; Test = "test_fake_wdt.c" },
    @{ Name = "fake_pwm"; Fake = "fake_pwm.c"; Test = "test_fake_pwm.c" },
    @{ Name = "fake_sysinfo"; Fake = "fake_sysinfo.c"; Test = "test_fake_sysinfo.c" }
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

# --- 7) Negative test: mutate fake_flash's erase-before-program AND semantics ---
# Proves test_fake_flash.c's AND-semantics assertion can actually fail:
# hal_flash.h does not pin erase-before-program enforcement, so fake_flash.c
# models it by ANDing new bits into the existing image (real NOR-flash
# behavior) rather than overwriting -- if program() is mutated to a plain
# overwrite, a caller that skipped hal_flash_erase() (e.g. program ignores
# the erase requirement) would silently get away with it on the fake even
# though real hardware would corrupt the write, exactly the bug class this
# fake exists to catch on host.
Write-Host "`n--- Negative test: fake_flash erase-before-program AND semantics ---"

$flashGoodBlock = (@'
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < apply_len; i++) {
        s_image[offset + i] &= src[i];
    }
'@) -replace "`r`n", "`n"

$flashMutantBlock = (@'
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < apply_len; i++) {
        /* MUTANT: plain overwrite -- program ignores the erase requirement. */
        s_image[offset + i] = src[i];
    }
'@) -replace "`r`n", "`n"

$flashOrigContent = (Get-Content (Join-Path $hostDir "fake_flash.c") -Raw) -replace "`r`n", "`n"
if (-not $flashOrigContent.Contains($flashGoodBlock)) {
    $failures += "Negative test setup FAILED: expected AND-semantics block not found verbatim in fake_flash.c -- source drifted from what this script mutates. Update flashGoodBlock/flashMutantBlock together with fake_flash.c."
} else {
    $flashMutantContent = $flashOrigContent.Replace($flashGoodBlock, $flashMutantBlock)
    $flashMutantSrc = Join-Path $workDir "fake_flash_mutant.c"
    Set-Content -Path $flashMutantSrc -Value $flashMutantContent -Encoding ASCII -NoNewline

    $flashMutantExe = Join-Path $workDir "fake_flash_mutant.exe"
    $r = Invoke-ClLink -SourceFiles @($flashMutantSrc, (Join-Path $here "test_fake_flash.c"), (Join-Path $commonDir "hal_status.c")) `
        -OutExe $flashMutantExe -IncludeDirs @($ifaceDir, $hostDir)
    if ($r.ExitCode -ne 0) {
        $failures += "Negative test: mutant fake_flash.c failed to COMPILE (expected it to compile and fail the test at runtime instead):`n$($r.Output)"
    } else {
        $run = Run-Exe -ExePath $flashMutantExe
        $parsed = Parse-Result -Output $run.Output
        if (-not $parsed.Matched) {
            $failures += "Negative test: mutant fake_flash test binary produced no RESULT line:`n$($run.Output)"
        } elseif ($run.ExitCode -eq 0 -and $parsed.Fail -eq 0) {
            $failures += "NEGATIVE TEST FAILED: the plain-overwrite mutant passed test_fake_flash.c cleanly -- the erase-before-program AND-semantics assertion does not actually catch this bug."
        } else {
            Write-Host "OK   negative test: fake_flash plain-overwrite mutant correctly fails test_fake_flash.c (pass=$($parsed.Pass) fail=$($parsed.Fail))"
            Write-Host "     mutant failure detail:`n$($run.Output)"
        }
    }
}

# --- 8) Negative test: mutate fake_scratch's slot-4 hard reservation ---
# Proves test_fake_scratch.c's slot-4-refusal assertion can actually fail:
# hal_scratch.h requires hal_scratch_write_u32() to refuse
# HAL_SCRATCH_SLOT_WATCHDOG_ENABLE outright (pico-sdk's watchdog_enable()
# owns that register). If the refusal is dropped, a caller could silently
# stomp the watchdog's own register.
Write-Host "`n--- Negative test: fake_scratch slot-4 hard reservation ---"

$scratchGoodBlock = (@'
hal_status_t hal_scratch_write_u32(uint8_t slot, uint32_t value) {
    if (!slot_in_range(slot)) {
        return HAL_INVALID_ARG;
    }
    if (slot == HAL_SCRATCH_SLOT_WATCHDOG_ENABLE) {
        return HAL_INVALID_ARG;
    }
    s_slots[slot] = value;
    return HAL_OK;
}
'@) -replace "`r`n", "`n"

$scratchMutantBlock = (@'
hal_status_t hal_scratch_write_u32(uint8_t slot, uint32_t value) {
    if (!slot_in_range(slot)) {
        return HAL_INVALID_ARG;
    }
    /* MUTANT: slot-4 hard reservation dropped. */
    s_slots[slot] = value;
    return HAL_OK;
}
'@) -replace "`r`n", "`n"

$scratchOrigContent = (Get-Content (Join-Path $hostDir "fake_scratch.c") -Raw) -replace "`r`n", "`n"
if (-not $scratchOrigContent.Contains($scratchGoodBlock)) {
    $failures += "Negative test setup FAILED: expected slot-4-refusal block not found verbatim in fake_scratch.c -- source drifted from what this script mutates. Update scratchGoodBlock/scratchMutantBlock together with fake_scratch.c."
} else {
    $scratchMutantContent = $scratchOrigContent.Replace($scratchGoodBlock, $scratchMutantBlock)
    $scratchMutantSrc = Join-Path $workDir "fake_scratch_mutant.c"
    Set-Content -Path $scratchMutantSrc -Value $scratchMutantContent -Encoding ASCII -NoNewline

    $scratchMutantExe = Join-Path $workDir "fake_scratch_mutant.exe"
    $r = Invoke-ClLink -SourceFiles @($scratchMutantSrc, (Join-Path $here "test_fake_scratch.c"), (Join-Path $commonDir "hal_status.c")) `
        -OutExe $scratchMutantExe -IncludeDirs @($ifaceDir, $hostDir)
    if ($r.ExitCode -ne 0) {
        $failures += "Negative test: mutant fake_scratch.c failed to COMPILE (expected it to compile and fail the test at runtime instead):`n$($r.Output)"
    } else {
        $run = Run-Exe -ExePath $scratchMutantExe
        $parsed = Parse-Result -Output $run.Output
        if (-not $parsed.Matched) {
            $failures += "Negative test: mutant fake_scratch test binary produced no RESULT line:`n$($run.Output)"
        } elseif ($run.ExitCode -eq 0 -and $parsed.Fail -eq 0) {
            $failures += "NEGATIVE TEST FAILED: the dropped-slot-4-reservation mutant passed test_fake_scratch.c cleanly -- the slot-4 hard-reservation assertion does not actually catch this bug."
        } else {
            Write-Host "OK   negative test: fake_scratch dropped-slot-4-reservation mutant correctly fails test_fake_scratch.c (pass=$($parsed.Pass) fail=$($parsed.Fail))"
            Write-Host "     mutant failure detail:`n$($run.Output)"
        }
    }
}

# --- 9) Negative test: mutate fake_wdt's advance-past-timeout fired latch ---
# Proves test_fake_wdt.c's fired-latch assertion can actually fail: if
# fake_wdt_advance_ms() stops latching fake_wdt_fired() on a lapsed feed, a
# test relying on it to detect a missed check-in would see a healthy
# watchdog that never actually fires.
Write-Host "`n--- Negative test: fake_wdt advance-past-timeout fired latch ---"

$wdtGoodBlock = (@'
    s_elapsed_since_feed_ms += ms;
    if (s_elapsed_since_feed_ms > s_timeout_ms) {
        s_fired = true;
    }
'@) -replace "`r`n", "`n"

$wdtMutantBlock = (@'
    s_elapsed_since_feed_ms += ms;
    /* MUTANT: fired latch never sets. */
    if (s_elapsed_since_feed_ms > s_timeout_ms) {
    }
'@) -replace "`r`n", "`n"

$wdtOrigContent = (Get-Content (Join-Path $hostDir "fake_wdt.c") -Raw) -replace "`r`n", "`n"
if (-not $wdtOrigContent.Contains($wdtGoodBlock)) {
    $failures += "Negative test setup FAILED: expected fired-latch block not found verbatim in fake_wdt.c -- source drifted from what this script mutates. Update wdtGoodBlock/wdtMutantBlock together with fake_wdt.c."
} else {
    $wdtMutantContent = $wdtOrigContent.Replace($wdtGoodBlock, $wdtMutantBlock)
    $wdtMutantSrc = Join-Path $workDir "fake_wdt_mutant.c"
    Set-Content -Path $wdtMutantSrc -Value $wdtMutantContent -Encoding ASCII -NoNewline

    $wdtMutantExe = Join-Path $workDir "fake_wdt_mutant.exe"
    $r = Invoke-ClLink -SourceFiles @($wdtMutantSrc, (Join-Path $here "test_fake_wdt.c"), (Join-Path $commonDir "hal_status.c")) `
        -OutExe $wdtMutantExe -IncludeDirs @($ifaceDir, $hostDir)
    if ($r.ExitCode -ne 0) {
        $failures += "Negative test: mutant fake_wdt.c failed to COMPILE (expected it to compile and fail the test at runtime instead):`n$($r.Output)"
    } else {
        $run = Run-Exe -ExePath $wdtMutantExe
        $parsed = Parse-Result -Output $run.Output
        if (-not $parsed.Matched) {
            $failures += "Negative test: mutant fake_wdt test binary produced no RESULT line:`n$($run.Output)"
        } elseif ($run.ExitCode -eq 0 -and $parsed.Fail -eq 0) {
            $failures += "NEGATIVE TEST FAILED: the never-fires mutant passed test_fake_wdt.c cleanly -- the advance-past-timeout fired-latch assertion does not actually catch this bug."
        } else {
            Write-Host "OK   negative test: fake_wdt never-fires mutant correctly fails test_fake_wdt.c (pass=$($parsed.Pass) fail=$($parsed.Fail))"
            Write-Host "     mutant failure detail:`n$($run.Output)"
        }
    }
}

# --- 10) Negative test: mutate fake_pwm's out-of-range duty rejection ---
# Proves test_fake_pwm.c's out-of-range assertion can actually fail: if
# hal_pwm_set_duty() stops rejecting duty_percent > 100, a caller bug (a
# miscomputed percentage) would silently be recorded as if valid instead of
# surfacing HAL_INVALID_ARG.
Write-Host "`n--- Negative test: fake_pwm out-of-range duty rejection ---"

$pwmGoodBlock = (@'
hal_status_t hal_pwm_set_duty(uint8_t duty_percent) {
    if (!s_initialized) {
        return HAL_NOT_READY;
    }
    if (duty_percent > 100) {
        return HAL_INVALID_ARG;
    }
    if (s_duty_history_count < FAKE_PWM_MAX_DUTY_HISTORY) {
        s_duty_history[s_duty_history_count++] = duty_percent;
    }
    return HAL_OK;
}
'@) -replace "`r`n", "`n"

$pwmMutantBlock = (@'
hal_status_t hal_pwm_set_duty(uint8_t duty_percent) {
    if (!s_initialized) {
        return HAL_NOT_READY;
    }
    /* MUTANT: out-of-range duty rejection dropped. */
    if (s_duty_history_count < FAKE_PWM_MAX_DUTY_HISTORY) {
        s_duty_history[s_duty_history_count++] = duty_percent;
    }
    return HAL_OK;
}
'@) -replace "`r`n", "`n"

$pwmOrigContent = (Get-Content (Join-Path $hostDir "fake_pwm.c") -Raw) -replace "`r`n", "`n"
if (-not $pwmOrigContent.Contains($pwmGoodBlock)) {
    $failures += "Negative test setup FAILED: expected out-of-range-duty block not found verbatim in fake_pwm.c -- source drifted from what this script mutates. Update pwmGoodBlock/pwmMutantBlock together with fake_pwm.c."
} else {
    $pwmMutantContent = $pwmOrigContent.Replace($pwmGoodBlock, $pwmMutantBlock)
    $pwmMutantSrc = Join-Path $workDir "fake_pwm_mutant.c"
    Set-Content -Path $pwmMutantSrc -Value $pwmMutantContent -Encoding ASCII -NoNewline

    $pwmMutantExe = Join-Path $workDir "fake_pwm_mutant.exe"
    $r = Invoke-ClLink -SourceFiles @($pwmMutantSrc, (Join-Path $here "test_fake_pwm.c"), (Join-Path $commonDir "hal_status.c")) `
        -OutExe $pwmMutantExe -IncludeDirs @($ifaceDir, $hostDir)
    if ($r.ExitCode -ne 0) {
        $failures += "Negative test: mutant fake_pwm.c failed to COMPILE (expected it to compile and fail the test at runtime instead):`n$($r.Output)"
    } else {
        $run = Run-Exe -ExePath $pwmMutantExe
        $parsed = Parse-Result -Output $run.Output
        if (-not $parsed.Matched) {
            $failures += "Negative test: mutant fake_pwm test binary produced no RESULT line:`n$($run.Output)"
        } elseif ($run.ExitCode -eq 0 -and $parsed.Fail -eq 0) {
            $failures += "NEGATIVE TEST FAILED: the dropped-range-check mutant passed test_fake_pwm.c cleanly -- the out-of-range duty rejection assertion does not actually catch this bug."
        } else {
            Write-Host "OK   negative test: fake_pwm dropped-range-check mutant correctly fails test_fake_pwm.c (pass=$($parsed.Pass) fail=$($parsed.Fail))"
            Write-Host "     mutant failure detail:`n$($run.Output)"
        }
    }
}

# --- 11) Negative test: mutate fake_sysinfo's coredump-erase-clears-presence ---
# Proves test_fake_sysinfo.c's erase-clears-presence assertion can actually
# fail: hal_sysinfo_coredump_erase() must clear the scripted presence flag
# (matching real esp_core_dump_image_erase()'s effect on a later
# esp_core_dump_image_check()) -- if it stops doing so, crash_report.c's
# real "erase after the report is consumed" flow would never actually erase
# on the fake, silently hiding that class of bug from host tests.
Write-Host "`n--- Negative test: fake_sysinfo coredump-erase clears presence ---"

$sysinfoGoodBlock = (@'
hal_status_t hal_sysinfo_coredump_erase(void) {
    s_coredump_present = false;
    return HAL_OK;
}
'@) -replace "`r`n", "`n"

$sysinfoMutantBlock = (@'
hal_status_t hal_sysinfo_coredump_erase(void) {
    /* MUTANT: erase no longer clears presence. */
    return HAL_OK;
}
'@) -replace "`r`n", "`n"

$sysinfoOrigContent = (Get-Content (Join-Path $hostDir "fake_sysinfo.c") -Raw) -replace "`r`n", "`n"
if (-not $sysinfoOrigContent.Contains($sysinfoGoodBlock)) {
    $failures += "Negative test setup FAILED: expected erase-clears-presence block not found verbatim in fake_sysinfo.c -- source drifted from what this script mutates. Update sysinfoGoodBlock/sysinfoMutantBlock together with fake_sysinfo.c."
} else {
    $sysinfoMutantContent = $sysinfoOrigContent.Replace($sysinfoGoodBlock, $sysinfoMutantBlock)
    $sysinfoMutantSrc = Join-Path $workDir "fake_sysinfo_mutant.c"
    Set-Content -Path $sysinfoMutantSrc -Value $sysinfoMutantContent -Encoding ASCII -NoNewline

    $sysinfoMutantExe = Join-Path $workDir "fake_sysinfo_mutant.exe"
    $r = Invoke-ClLink -SourceFiles @($sysinfoMutantSrc, (Join-Path $here "test_fake_sysinfo.c"), (Join-Path $commonDir "hal_status.c")) `
        -OutExe $sysinfoMutantExe -IncludeDirs @($ifaceDir, $hostDir)
    if ($r.ExitCode -ne 0) {
        $failures += "Negative test: mutant fake_sysinfo.c failed to COMPILE (expected it to compile and fail the test at runtime instead):`n$($r.Output)"
    } else {
        $run = Run-Exe -ExePath $sysinfoMutantExe
        $parsed = Parse-Result -Output $run.Output
        if (-not $parsed.Matched) {
            $failures += "Negative test: mutant fake_sysinfo test binary produced no RESULT line:`n$($run.Output)"
        } elseif ($run.ExitCode -eq 0 -and $parsed.Fail -eq 0) {
            $failures += "NEGATIVE TEST FAILED: the no-op-erase mutant passed test_fake_sysinfo.c cleanly -- the coredump-erase-clears-presence assertion does not actually catch this bug."
        } else {
            Write-Host "OK   negative test: fake_sysinfo no-op-erase mutant correctly fails test_fake_sysinfo.c (pass=$($parsed.Pass) fail=$($parsed.Fail))"
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
