# checkcache: ok
# check_nvs_write_guard_coverage.ps1 -- keeps every NVS/flash write call
# site inside this codebase's PSRAM-stack-guarded modules actually guarded.
#
# DRAM_PSRAM_STATUS.md section 9 (2026-09-02, write-path re-audit): the third
# pass found run_state.c/relay_cycles.c/profile_executor_firing_stats.c
# completely unguarded despite an earlier pass believing the write-side
# hazard was closed. This pass re-checked that fix and found the SAME class
# of gap one level down: run_state.c's and relay_cycles.c's own
# migrate_from_default_partition() functions write NVS
# (nvs_set_blob()/nvs_commit()) but never got the caller_stack_is_external()
# guard their file's main write path (persist_locked() / the equivalent)
# already carries -- and profiles_http.c's nvs_erase_slot() had the same gap
# next to its own already-guarded nvs_save_slot(). Each is init/httpd-worker-
# only today (not reachable from a PSRAM-stacked task), so none of this was
# live, but every one of them would have read as "this file is covered" to
# the next audit that checks file-level guard presence instead of every
# write call site. This script checks per-FUNCTION, not per-file, so that
# class of gap cannot reopen silently.
#
# What it checks: for every .c file under App/drivers that this project has
# opted into the caller_stack_is_external() convention (the file already
# defines that predicate -- see $guardedFiles below), every function BODY
# that calls nvs_set_*()/nvs_commit()/esp_partition_write()/
# esp_partition_erase_range()/hal_kv_set_*()/hal_kv_commit()/hal_kv_erase_*()
# must also call caller_stack_is_external() somewhere in that same function
# body. A function that writes without the guard fails the check by name.
#
# 2026-09-06: extended to the hal_kv_* forms once HW_ABSTRACTION.md
# Phase 3 item 3 (the nvs.h -> hal_kv.h migration) started landing in
# $guardedFiles -- relay_cycles.c and run_state.c now write via
# hal_kv_set_blob()/hal_kv_commit(), not nvs_set_blob()/nvs_commit()
# directly, so the old nvs_-only pattern had quietly started matching zero
# write sites in those two files and passing vacuously (green with no
# coverage).
#
# This is deliberately scoped to files that have already opted into the
# convention, not every NVS-writing file in the tree (adaptive_tune.c,
# boot_guard.c, crash_report.c, ota_http.c, ota_record.c, profiles_builtin.c,
# time_sync.c, touch_cal_store.c, unit_pref.c, watchdog_cfg.c, wifi_prov.c,
# zones_config_store.c) -- DRAM_PSRAM_STATUS.md section 7.3's per-candidate
# trace found none of those reachable from a relocation candidate task, so
# adding the guard there is a different task's call, not this plan's. If a
# future pass adds the guard to one of those files, add its path to
# $guardedFiles in the same commit so this check starts covering it too.
#
# Usage: powershell -File tools\check_nvs_write_guard_coverage.ps1 [-DriversDir <path>]
# -DriversDir is for smoke-testing against a simulated post-layer-move tree
# (or any other alternate root); normal use omits it and gets the real repo
# path.
param(
    [string]$DriversDir
)
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
if ($DriversDir) {
    $driversDir = (Resolve-Path $DriversDir).Path
} else {
    $driversDir = Join-Path $root "..\firmware\KilnFW\App\drivers"
    $driversDir = (Resolve-Path $driversDir).Path
}

# Same comment-stripping helper as check_stack_margin_registration.ps1 /
# check_uri_handler_cap.ps1 (duplicated rather than imported -- this project
# has no shared PowerShell module mechanism).
function Get-CodeOnlyLines {
    param([string]$Path)
    $inBlockComment = $false
    $lines = Get-Content -Path $Path
    $result = @()
    foreach ($line in $lines) {
        $code = $line
        if ($inBlockComment) {
            $endIdx = $code.IndexOf("*/")
            if ($endIdx -ge 0) {
                $code = $code.Substring($endIdx + 2)
                $inBlockComment = $false
            } else {
                $result += ""
                continue
            }
        }
        $lineCommentIdx = $code.IndexOf("//")
        if ($lineCommentIdx -ge 0) {
            $code = $code.Substring(0, $lineCommentIdx)
        }
        while ($true) {
            $startIdx = $code.IndexOf("/*")
            if ($startIdx -lt 0) { break }
            $endIdx = $code.IndexOf("*/", $startIdx)
            if ($endIdx -ge 0) {
                $code = $code.Substring(0, $startIdx) + $code.Substring($endIdx + 2)
            } else {
                $code = $code.Substring(0, $startIdx)
                $inBlockComment = $true
                break
            }
        }
        $result += $code
    }
    return $result
}

# Files that have opted into the caller_stack_is_external() convention --
# each already defines that predicate once. See this script's top comment
# for why this list is not "every NVS-writing file in the tree".
$guardedFiles = @(
    "kiln_cfg_store.c",
    "safety_cfg_store.c",
    "profiles_http.c",
    "relay_cycles.c",
    "run_state.c",
    "profile_executor_firing_stats.c"
)

$writePattern = 'nvs_set_\w+\s*\(|nvs_commit\s*\(|esp_partition_write\s*\(|esp_partition_erase_range\s*\(|hal_kv_set_\w+\s*\(|hal_kv_commit\s*\(|hal_kv_erase_\w+\s*\('
$guardCallPattern = 'caller_stack_is_external\s*\('
$signaturePattern = '([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*$'

# Resolve a bare basename to a file anywhere under $driversDir. This makes the
# check agnostic to an upcoming move of every drivers/*.c file into layer
# subdirectories (drivers/<layer>/<file>) -- $guardedFiles below stays a list
# of basenames either way. Fails loud (not silently picks one) if a name is
# missing or ambiguous.
function Resolve-DriverFile {
    param([string]$DriversDir, [string]$BaseName)
    $found = Get-ChildItem -Path $DriversDir -Filter $BaseName -File -Recurse
    if ($found.Count -eq 0) {
        throw "check_nvs_write_guard_coverage.ps1: expected file '$BaseName' not found anywhere under $DriversDir -- has it moved or been renamed? Update `$guardedFiles."
    }
    if ($found.Count -gt 1) {
        $paths = ($found | ForEach-Object { $_.FullName }) -join ", "
        throw "check_nvs_write_guard_coverage.ps1: '$BaseName' matched more than one file under $DriversDir ($paths) -- this script cannot tell which one is the guarded module. Disambiguate."
    }
    return $found[0].FullName
}

$failures = @()
$filesChecked = 0
$functionsScanned = 0

foreach ($name in $guardedFiles) {
    $path = Resolve-DriverFile -DriversDir $driversDir -BaseName $name
    $filesChecked++
    $codeLines = Get-CodeOnlyLines -Path $path

    if (-not ($codeLines -join "`n" | Select-String -Pattern 'static bool caller_stack_is_external\s*\(\s*void\s*\)' -Quiet)) {
        throw "check_nvs_write_guard_coverage.ps1: $name is in the guarded-file list but no longer defines caller_stack_is_external() -- has the guard been removed, renamed, or moved to a shared header? Update this script (and, if the guard genuinely no longer applies, `$guardedFiles) to match."
    }

    $depth = 0
    $bodyLines = New-Object System.Collections.Generic.List[string]
    $funcName = $null
    $pendingSignatureLines = New-Object System.Collections.Generic.List[string]

    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        $line = $codeLines[$i]
        $trimmed = $line.Trim()

        if ($depth -eq 0) {
            if ($trimmed -eq "{") {
                # Function open brace at top level -- resolve the name from
                # the accumulated signature lines just before it.
                $sig = ($pendingSignatureLines -join " ").Trim()
                $m = [regex]::Match($sig, $signaturePattern)
                $funcName = if ($m.Success) { $m.Groups[1].Value } else { "<unknown near line $($i+1)>" }
                $pendingSignatureLines.Clear()
                $bodyLines.Clear()
                $depth = 1
                continue
            }
            if ($trimmed.Length -gt 0) {
                if ($trimmed.EndsWith(";") -or $trimmed.EndsWith("}")) {
                    # Statement/decl/prior function close at top level --
                    # not part of a signature in progress.
                    $pendingSignatureLines.Clear()
                } else {
                    $pendingSignatureLines.Add($trimmed)
                }
            }
            continue
        }

        # depth >= 1: inside a function body. Track brace depth (naive --
        # no string/char-literal awareness, matching this repo's other
        # brace-counting checks; braces inside string literals are rare
        # enough in this codebase's C style not to have tripped those up).
        $openCount = ([regex]::Matches($line, '\{')).Count
        $closeCount = ([regex]::Matches($line, '\}')).Count
        $bodyLines.Add($line)
        $depth += $openCount - $closeCount

        if ($depth -le 0) {
            $functionsScanned++
            $bodyText = $bodyLines -join "`n"
            $writesNvs = [regex]::IsMatch($bodyText, $writePattern)
            $hasGuardCall = [regex]::IsMatch($bodyText, $guardCallPattern)
            if ($writesNvs -and -not $hasGuardCall -and $funcName -ne "caller_stack_is_external") {
                $failures += "$name : $funcName()"
            }
            $depth = 0
            $bodyLines.Clear()
            $funcName = $null
        }
    }
}

if ($filesChecked -lt $guardedFiles.Count) {
    throw "check_nvs_write_guard_coverage.ps1: only checked $filesChecked of $($guardedFiles.Count) expected file(s) -- see errors above."
}

Write-Host "NVS write-guard coverage check: $functionsScanned function bod$(if ($functionsScanned -eq 1) {'y'} else {'ies'}) scanned across $filesChecked guarded file(s)."

if ($failures.Count -gt 0) {
    Write-Host "NVS WRITE GUARD COVERAGE CHECK FAILED:" -ForegroundColor Red
    Write-Host "  The following function(s) call nvs_set_*()/nvs_commit()/esp_partition_write()/" -ForegroundColor Red
    Write-Host "  esp_partition_erase_range()/hal_kv_set_*()/hal_kv_commit()/hal_kv_erase_*() but" -ForegroundColor Red
    Write-Host "  never call caller_stack_is_external() anywhere in their own body:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "    $f" -ForegroundColor Red
    }
    throw "A write call site was added to a PSRAM-stack-guarded module (DRAM_PSRAM_STATUS.md section 7.2/9) without the caller_stack_is_external() refusal every other write path in that file already carries. Add the same guard this file's other write function(s) use, or explain in DRAM_PSRAM_STATUS.md why this specific call site is exempt."
}

Write-Host "NVS write-guard coverage check passed: every write call site in $($guardedFiles.Count) guarded file(s) is guarded." -ForegroundColor Green
exit 0
