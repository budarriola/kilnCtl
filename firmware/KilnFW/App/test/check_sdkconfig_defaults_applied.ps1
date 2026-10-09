# check_sdkconfig_defaults_applied.ps1 -- DRAM_PSRAM_STATUS.md section 5.
#
# a0b8711 stepped CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 16384 -> 8192 in
# sdkconfig.defaults, believing that change was live on the board being
# worked on. It was not: firmware/KilnFW/sdkconfig is gitignored and
# build-generated, ESP-IDF's Kconfig defaults policy is "use the generated
# sdkconfig value over sdkconfig.defaults whenever the key already has a
# value there" -- so a plan step can land as a clean commit, read back as
# correct in sdkconfig.defaults, and still be completely inert on the
# running board, silently, with no build error and no test failure.
#
# This script closes that hole for every key sdkconfig.defaults pins. It does
# NOT assert the generated sdkconfig has only those keys -- most keys
# legitimately differ (menuconfig-derived toggles, Kconfig defaults never
# pinned). It asserts that each pinned key has reached the generated sdkconfig
# with the pinned value, because those are exactly the changes someone will
# believe are live when they are not. $watchedKeys below is only a guard that
# defaults still pins the keys a plan depends on.
#
# Add a line to $watchedKeys whenever a future phase of DRAM_PSRAM_STATUS.md
# (or any other plan) deliberately changes a value in sdkconfig.defaults
# that ESP-IDF's regenerate-on-value-present behavior can silently ignore.
#
# Usage:
#   powershell -File firmware\KilnFW\App\test\check_sdkconfig_defaults_applied.ps1
#   powershell -File firmware\KilnFW\App\test\check_sdkconfig_defaults_applied.ps1 -DefaultsPath <path> -SdkconfigPath <path>
#
# -DefaultsPath / -SdkconfigPath exist so this script's own negative tests
# can point it at fixtures instead of the real files.
#
# Exit code is non-zero if:
#   - sdkconfig.defaults is missing, or a watched key is missing from it
#     (the defaults file itself regressed)
#   - the generated sdkconfig exists and disagrees with sdkconfig.defaults
#     on any watched key (the silent-no-op case this script exists to catch)
#
# If the generated sdkconfig does not exist at all (clean checkout, no build
# yet), that is NOT a failure -- there is nothing to have gone stale yet.
# A clear warning is printed instead so the gap is not mistaken for a pass.

param(
    [string]$DefaultsPath,
    [string]$SdkconfigPath
)

$ErrorActionPreference = "Stop"

# $PSScriptRoot = ...\firmware\KilnFW\App\test
$kilnfwRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

if (-not $DefaultsPath) {
    $DefaultsPath = Join-Path $kilnfwRoot "sdkconfig.defaults"
}
if (-not $SdkconfigPath) {
    $SdkconfigPath = Join-Path $kilnfwRoot "sdkconfig"
}

if (-not (Test-Path $DefaultsPath)) {
    throw "check_sdkconfig_defaults_applied.ps1: sdkconfig.defaults not found at $DefaultsPath"
}

# Keys a plan document has deliberately changed, and that a stale generated
# sdkconfig would silently keep overriding. Add to this list, never remove
# an entry just because it currently passes -- removing it re-opens exactly
# the hole this script exists to close.
$watchedKeys = @(
    'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL',  # DRAM_PSRAM_STATUS.md section 5, a0b8711: 16384 -> 8192
    'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM', # 2026-09-23: bench-proven 10 vs. IDF's regenerated 16
    'CONFIG_ESP_WIFI_RX_BA_WIN',            # 2026-09-23: bench-proven 6 vs. IDF's regenerated 16
    'CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS',      # 2026-09-23: bench-proven 4 vs. IDF's regenerated 0
    'CONFIG_KILNCTL_ENABLE_GPIO_PROBE',     # 2026-09-23: pinned y so a clean worktree keeps gpio_probe
    'CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC',    # 2026-10-05 WP8: TLS option D; a stale sdkconfig keeps option B (does not fit)
    'CONFIG_MBEDTLS_DYNAMIC_BUFFER'         # 2026-10-05 WP8: TLS option D, same reason
)

function Get-ConfigValue {
    param([string[]]$Lines, [string]$Key)
    foreach ($line in $Lines) {
        $t = $line.Trim()
        if ($t -match "^$Key=(.+)$") {
            return $Matches[1].Trim()
        }
        if ($t -match "^# $Key is not set$") {
            return '(not set)'
        }
    }
    return $null
}

$defaultsLines = Get-Content -Path $DefaultsPath

$missingFromDefaults = @()
$defaultsValues = @{}
foreach ($key in $watchedKeys) {
    $v = Get-ConfigValue -Lines $defaultsLines -Key $key
    if ($null -eq $v) {
        $missingFromDefaults += $key
    } else {
        $defaultsValues[$key] = $v
    }
}

# Compare EVERY key sdkconfig.defaults pins (CONFIG_X=v and "# CONFIG_X is not
# set"), not just $watchedKeys. 2026-10-07: the main-tree sdkconfig drifted on
# the MBEDTLS dependents (DYNAMIC_FREE_CA_CERT / DYNAMIC_FREE_CONFIG_DATA) that
# the hand list did not name. A pinned key is a deliberate choice, so any
# disagreement with the generated file is the inert-change case. $watchedKeys
# stays as the "defaults must still pin these" regression guard above.
$compareKeys = New-Object System.Collections.Generic.List[string]
foreach ($key in $watchedKeys) { $compareKeys.Add($key) }
foreach ($line in $defaultsLines) {
    $t = $line.Trim()
    $k = $null
    if ($t -match '^(CONFIG_[A-Za-z0-9_]+)=(.+)$') { $k = $Matches[1]; $v = $Matches[2].Trim() }
    elseif ($t -match '^# (CONFIG_[A-Za-z0-9_]+) is not set$') { $k = $Matches[1]; $v = '(not set)' }
    if ($k) {
        $defaultsValues[$k] = $v
        if (-not $compareKeys.Contains($k)) { $compareKeys.Add($k) }
    }
}

if ($missingFromDefaults.Count -gt 0) {
    Write-Host "CHECK FAILED: watched key(s) missing from $DefaultsPath :" -ForegroundColor Red
    foreach ($k in $missingFromDefaults) { Write-Host "  $k" -ForegroundColor Red }
    throw "sdkconfig.defaults no longer sets $($missingFromDefaults.Count) watched key(s) -- see DRAM_PSRAM_STATUS.md section 5"
}

if (-not (Test-Path $SdkconfigPath)) {
    Write-Host "WARNING: generated sdkconfig not found at $SdkconfigPath." -ForegroundColor Yellow
    Write-Host "  This is expected on a clean checkout with no build yet -- there is nothing" -ForegroundColor Yellow
    Write-Host "  to have gone stale. Once a build directory exists, re-run this check to" -ForegroundColor Yellow
    Write-Host "  confirm the watched keys actually reached the generated config." -ForegroundColor Yellow
    Write-Host ""
    Write-Host "sdkconfig.defaults values for watched keys (unverified against a build):"
    foreach ($key in $compareKeys) {
        Write-Host "  $key = $($defaultsValues[$key])"
    }
    exit 0
}

$sdkconfigLines = Get-Content -Path $SdkconfigPath

$mismatches = @()
foreach ($key in $compareKeys) {
    $liveValue = Get-ConfigValue -Lines $sdkconfigLines -Key $key
    $wantValue = $defaultsValues[$key]
    if ($null -eq $liveValue) {
        $mismatches += "'$key' is set to $wantValue in sdkconfig.defaults but does not appear at all in the generated sdkconfig"
    } elseif ($liveValue -ne $wantValue) {
        $mismatches += "'$key': sdkconfig.defaults wants $wantValue, generated sdkconfig has $liveValue -- the defaults change is INERT on whatever this sdkconfig was built from. Regenerate (idf.py reconfigure or a clean build dir) before trusting this change is live."
    }
}

if ($mismatches.Count -gt 0) {
    Write-Host "CHECK FAILED: sdkconfig.defaults and the generated sdkconfig disagree on a pinned key:" -ForegroundColor Red
    foreach ($m in $mismatches) {
        Write-Host "  $m" -ForegroundColor Red
    }
    throw "$($mismatches.Count) pinned-key mismatch(es) between $DefaultsPath and $SdkconfigPath -- ESP-IDF's Kconfig defaults policy keeps the generated value over sdkconfig.defaults, so this plan step is not actually applied. See DRAM_PSRAM_STATUS.md section 5."
}

Write-Host "sdkconfig-defaults-applied check passed: $($compareKeys.Count) pinned key(s) agree between sdkconfig.defaults and the generated sdkconfig:"
foreach ($key in $compareKeys) {
    Write-Host "  $key = $($defaultsValues[$key])"
}
exit 0
