# check_nvs_key_length.ps1 -- catches an NVS/hal_kv key, namespace, or
# partition literal longer than ESP-IDF's real limit BEFORE it reaches a
# board, complementing (not replacing) the compile-time
# NVS_KEY_LEN_CHECK() macro (firmware/KilnFW/App/drivers/persist/
# nvs_key_check.h).
#
# ESP-IDF's NVS_KEY_NAME_MAX_SIZE is 16 bytes INCLUDING the NUL terminator,
# so a literal must be <= 15 usable characters. zones_config_store.c's
# "zone_normals_cfg" (16 chars, introduced ddbd024, renamed 2026-09-06) shipped
# and silently failed nvs_set_blob() for weeks: the pre-HAL host stub modeled
# one shared blob slot with no key-length check at all, and the production
# setter did not log its own failure -- host tests and the build both stayed
# green while every write under that key failed on real hardware.
# NVS_KEY_LEN_CHECK() now guards every literal that macro is applied to, but
# that guard only fires if someone remembers to write the
# `NVS_KEY_LEN_CHECK(...)` line next to every new #define -- this script is
# the independent, no-effort-required backstop: it does not care whether the
# macro was applied, it re-derives the answer straight from the tracked
# source text.
#
# What it checks: every tracked (git ls-files) firmware/KilnFW/App file
# (excluding test/, stubs/, build/) for a `#define <NAME> "<literal>"` where
# NAME contains NVS_KEY, NVS_NAMESPACE, or NVS_PARTITION (case-insensitive,
# covers KILN_NVS_PARTITION, FIRING_STATS_NVS_PARTITION, ADAPTIVE_TUNE_NVS_KEY_*,
# etc. -- every existing macro name in the tree follows one of these three
# shapes). Any matched literal longer than 15 characters is a hard failure,
# named with file:line and the literal itself.
#
# Deliberately narrower than a check on every quoted string in the tree
# (JSON field names, log strings, and status enum tags like "SKIPPED_MODEL_
# NOT_PERSISTED" are 15+ chars routinely and are not NVS keys) -- this greps
# only the #define shape those macros are always declared with, which is
# also exactly the shape NVS_KEY_LEN_CHECK() is written against.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\check_nvs_key_length.ps1

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$appDir = Join-Path $repoRoot "firmware\KilnFW\App"

if (-not (Test-Path $appDir)) {
    throw "check_nvs_key_length: $appDir not found -- has it moved?"
}

Push-Location $repoRoot
try {
    $trackedRaw = git ls-files -- "firmware/KilnFW/App/*.c" "firmware/KilnFW/App/*.h"
} finally {
    Pop-Location
}

$tracked = $trackedRaw |
    Where-Object {
        $_ -notmatch '(^|/)test/' -and
        $_ -notmatch '(^|/)stubs/' -and
        $_ -notmatch '(^|/)build/'
    }

if ($tracked.Count -lt 20) {
    throw "check_nvs_key_length: only found $($tracked.Count) tracked .c/.h files under firmware/KilnFW/App -- expected 20+. git ls-files probably ran from the wrong directory or the tree moved; this check has gone blind, not found a shrunk tree."
}

# Matches: #define NAME "literal"  where NAME contains one of the three
# macro-name shapes every real NVS define in this tree uses. Tolerates
# extra whitespace and a trailing comment.
$definePattern = '^\s*#\s*define\s+(\S*(?:NVS_KEY|NVS_NAMESPACE|NVS_PARTITION)\S*)\s+"([^"]*)"'

$violations = @()
$scanned = 0

foreach ($rel in $tracked) {
    $full = Join-Path $repoRoot ($rel -replace '/', '\')
    if (-not (Test-Path $full)) { continue }
    $scanned++
    $lineNum = 0
    foreach ($line in Get-Content -Path $full) {
        $lineNum++
        if ($line -match $definePattern) {
            $name = $Matches[1]
            $literal = $Matches[2]
            if ($literal.Length -gt 15) {
                $violations += "${rel}:${lineNum}: #define $name `"$literal`" is $($literal.Length) chars -- exceeds NVS's 15-usable-character key/namespace/partition limit (NVS_KEY_NAME_MAX_SIZE=16 including NUL). Rename to <=15 chars; the old key's data is unreachable on real hardware regardless (nvs_set_*() rejects it at ESP_ERR_NVS_KEY_TOO_LONG), so a straight rename is safe unless a shorter alias was already live before (check git log -S on the literal)."
            }
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host "NVS KEY LENGTH CHECK FAILED ($scanned files scanned):" -ForegroundColor Red
    foreach ($v in $violations) { Write-Host "  $v" -ForegroundColor Red }
    throw "$($violations.Count) NVS key/namespace/partition literal(s) exceed the 15-character limit."
}

Write-Host "NVS key length check passed ($scanned files scanned, 0 violations)." -ForegroundColor Green
exit 0
