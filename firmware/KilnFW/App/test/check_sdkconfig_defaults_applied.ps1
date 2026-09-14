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
# This script closes that hole for the specific keys this plan deliberately
# changes. It does NOT assert sdkconfig.defaults and sdkconfig match on
# every key -- most keys legitimately differ (menuconfig-derived toggles,
# per-board settings never pinned in defaults). It asserts only that the
# handful of keys a plan document changed on purpose have actually reached
# the generated sdkconfig, because those are exactly the changes someone
# will believe are live when they are not.
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
    'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL'  # DRAM_PSRAM_STATUS.md section 5, a0b8711: 16384 -> 8192
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
    foreach ($key in $watchedKeys) {
        Write-Host "  $key = $($defaultsValues[$key])"
    }
    exit 0
}

$sdkconfigLines = Get-Content -Path $SdkconfigPath

$mismatches = @()
foreach ($key in $watchedKeys) {
    $liveValue = Get-ConfigValue -Lines $sdkconfigLines -Key $key
    $wantValue = $defaultsValues[$key]
    if ($null -eq $liveValue) {
        $mismatches += "'$key' is set to $wantValue in sdkconfig.defaults but does not appear at all in the generated sdkconfig"
    } elseif ($liveValue -ne $wantValue) {
        $mismatches += "'$key': sdkconfig.defaults wants $wantValue, generated sdkconfig has $liveValue -- the defaults change is INERT on whatever this sdkconfig was built from. Regenerate (idf.py reconfigure or a clean build dir) before trusting this change is live."
    }
}

if ($mismatches.Count -gt 0) {
    Write-Host "CHECK FAILED: sdkconfig.defaults and the generated sdkconfig disagree on a watched key:" -ForegroundColor Red
    foreach ($m in $mismatches) {
        Write-Host "  $m" -ForegroundColor Red
    }
    throw "$($mismatches.Count) watched-key mismatch(es) between $DefaultsPath and $SdkconfigPath -- ESP-IDF's Kconfig defaults policy keeps the generated value over sdkconfig.defaults, so this plan step is not actually applied. See DRAM_PSRAM_STATUS.md section 5."
}

Write-Host "sdkconfig-defaults-applied check passed: $($watchedKeys.Count) watched key(s) agree between sdkconfig.defaults and the generated sdkconfig:"
foreach ($key in $watchedKeys) {
    Write-Host "  $key = $($defaultsValues[$key])"
}
exit 0
