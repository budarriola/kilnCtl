# compile_esp_backends.ps1 -- syntax-only compile check for the ESP-IDF
# hwAbstraction backends (esp/gpio/hal_gpio_esp.c, esp/adc/hal_adc_esp.c,
# esp/common/hal_esp_common.c).
#
# Phase 1a is move-only: these backends are NOT wired into any CMakeLists
# yet, so there is no real build target to compile them through. This
# script stands in for that -- it invokes the same xtensa-esp32s3 gcc IDF
# uses, with -fsyntax-only, using the include-path list derived from
# firmware/KilnFW/build/compile_commands.json (an existing driver TU for
# the driver/gpio.h include set, plus the project_elf TU for esp_adc's
# include set, since no existing KilnFW source currently pulls in
# esp_adc/adc_oneshot.h -- see hal_adc_esp.c's "INTERFACE MISMATCH" note for
# why there is no ADC consumer to borrow a compile command from directly).
#
# Requires an existing firmware/KilnFW/build/compile_commands.json (run
# build_kilnfw once if it is missing).
#
# Usage: powershell -ExecutionPolicy Bypass -File compile_esp_backends.ps1

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
$CcPath = Join-Path $RepoRoot "firmware\KilnFW\build\compile_commands.json"

if (-not (Test-Path $CcPath)) {
    Write-Error "Missing $CcPath -- run build_kilnfw (or idf.py build) at least once first."
    exit 1
}

$cc = Get-Content $CcPath -Raw | ConvertFrom-Json

function Get-CommandFor([string]$suffix) {
    foreach ($entry in $cc) {
        if ($entry.file -replace '\\','/' -match [regex]::Escape($suffix) + '$') {
            return $entry.command
        }
    }
    return $null
}

$gpioConsumerCmd = Get-CommandFor "MAX31856.c"
$adcRefCmd = Get-CommandFor "project_elf_src_esp32s3.c"

if (-not $gpioConsumerCmd) {
    Write-Error "Could not find a compile command for MAX31856.c in compile_commands.json"
    exit 1
}
if (-not $adcRefCmd) {
    Write-Error "Could not find a compile command for project_elf_src_esp32s3.c in compile_commands.json"
    exit 1
}

# Reuse a REAL, already-working IDF compile command verbatim (order and all
# -- the esp_libc/newlib header set is order-sensitive, see the comment at
# the top of this file's history: a hand-merged include list produced
# stdio.h redefinition errors because it disturbed the toolchain's own
# implicit search order). Only the tail ("-o obj -c src") is swapped for
# "-fsyntax-only <our file>", and a few extra -I's are appended at the very
# end (append-only, so they cannot shift anything already working).
function Get-CompilerExe([string]$command) {
    return ($command -split ' ')[0]
}

function Get-BaseArgs([string]$command) {
    # Strip the leading compiler exe and the trailing "-o <obj> -c <src>".
    $withoutExe = $command.Substring((Get-CompilerExe $command).Length).Trim()
    $stripped = [regex]::Replace($withoutExe, '-o \S+\.obj -c \S+$', '').Trim()
    return $stripped
}

function Get-IncludeDirs([string]$command) {
    $m = [regex]::Matches($command, '-I(\S+)')
    return $m | ForEach-Object { $_.Groups[1].Value }
}

$compilerExe = Get-CompilerExe $gpioConsumerCmd
$baseArgsLine = Get-BaseArgs $gpioConsumerCmd

# esp_adc's include dirs are absent from the MAX31856.c command (no current
# KilnFW source pulls in esp_adc/adc_oneshot.h -- see hal_adc_esp.c's
# INTERFACE MISMATCH note #1), so they are appended from the project_elf
# reference command, which does have them.
$adcIncludeDirs = Get-IncludeDirs $adcRefCmd | Where-Object { $_ -match 'esp_adc' } | Select-Object -Unique

$HalDir = Join-Path $RepoRoot "firmware\hwAbstraction"
$ownIncludes = @(
    (Join-Path $HalDir "interface"),
    (Join-Path $HalDir "esp\common")
)

# GCC's @response-file parser treats backslash as an escape character even
# outside quotes, so a raw "C:\Users\..." path is silently mangled
# (backslash-letter pairs get collapsed). Use forward slashes, same as the
# paths already embedded in $baseArgsLine (CMake emits those with "/").
$extraArgs = @()
$extraArgs += ($ownIncludes | ForEach-Object { "-I" + ($_ -replace '\\', '/') })
$extraArgs += ($adcIncludeDirs | ForEach-Object { "-I$_" })

$sources = @(
    (Join-Path $HalDir "esp\gpio\hal_gpio_esp.c"),
    (Join-Path $HalDir "esp\adc\hal_adc_esp.c"),
    (Join-Path $HalDir "esp\common\hal_esp_common.c")
)

$failed = $false
$rspPath = Join-Path $env:TEMP "hal_esp_backend_compile.rsp"
foreach ($src in $sources) {
    Write-Host "== syntax-checking $src =="
    # gcc-style response file (@file) sidesteps cmd.exe's 8191-char command
    # line limit -- the merged include/define list from the real IDF build
    # command is long. One arg per line; gcc's driver splits on whitespace
    # same as a shell would, so the original argument string can go in as-is.
    $srcFwd = $src -replace '\\', '/'
    $rspContent = "$baseArgsLine $($extraArgs -join ' ') -fsyntax-only -Wall `"$srcFwd`""
    Set-Content -Path $rspPath -Value $rspContent -Encoding ascii -NoNewline
    & $compilerExe "@$rspPath"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED: $src" -ForegroundColor Red
        $failed = $true
    } else {
        Write-Host "OK: $src" -ForegroundColor Green
    }
}
Remove-Item -Path $rspPath -ErrorAction SilentlyContinue

if ($failed) {
    exit 1
}
Write-Host "All ESP hwAbstraction backends passed -fsyntax-only." -ForegroundColor Green
