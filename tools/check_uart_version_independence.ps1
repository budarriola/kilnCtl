# check_uart_version_independence.ps1 -- SaftyFW/TODO.md's "Shared ids
# split out of uart_task_ids.h; PC-link ids left behind" item.
#
# UART_PROTOCOL_VERSION (firmware/KilnFW/App/drivers/common/uart_task_ids.h) is the
# PC<->ESP link's own wire-protocol version. KILNLINK_PROTOCOL_VERSION
# (firmware/CommonFW/include/kilnlink/kilnlink_version.h) is the ESP<->Pico
# isolated safety link's own, independent version. From 2026-08-17 to
# 2026-08-24 the former was a plain alias of the latter
# (`#define UART_PROTOCOL_VERSION ((uint16_t)KILNLINK_PROTOCOL_VERSION)`),
# which meant a version bump driven purely by the isolated link's contract
# (e.g. 5->6, Frame A growing tx_dropped_sat -- nothing on the PC link
# changed) silently dragged the PC link's version along with it. Because
# tools/PcTools/src/kilnctrl/devices.py's FirmwareVersion.compatible is a
# hard equality gate, that meant a real board refused every PC command with
# "device speaks vN, pc_tools speaks vN-1" until pc_tools was rebuilt for a
# protocol change that, on the PC link's own side, never happened. This has
# now bitten the project three times (2026-08-17 alias creation, 2026-08-23
# tx_dropped_sat bump, 2026-08-24 GET_CT_CAL/GET_PARAM/GET_CONFIG_PAGE id
# split). uart_task_ids.h's own doc comment on UART_PROTOCOL_VERSION now
# says, in prose, "never define this in terms of KILNLINK_PROTOCOL_VERSION
# again" -- a comment is weak on its own, so this script makes that rule
# durable rather than advisory.
#
# Modeled on firmware/SaftyFW/tools/check_link_impl_isolation.ps1 and
# tools/check_no_duplicate_crc.ps1: standalone (not wired into any build),
# non-zero-exit via `throw` on violation, same comment-stripping helper as
# check_link_impl_isolation.ps1 (a comment that merely MENTIONS
# KILNLINK_PROTOCOL_VERSION -- and uart_task_ids.h's own doc comment does,
# extensively, to explain the history above -- must not trip this check;
# only the actual #define matters).
#
# What this catches: the #define of UART_PROTOCOL_VERSION in
# firmware/KilnFW/App/drivers/common/uart_task_ids.h textually referencing any
# KILNLINK_* symbol on its value side (an alias, a cast of one, an
# arithmetic expression built from one -- any textual dependency at all).
# It deliberately does NOT flag:
#   - prose comments discussing KILNLINK_PROTOCOL_VERSION, the alias's
#     history, or the split (already stripped by Get-CodeOnlyLines)
#   - any other #define in the file referencing a KILNLINK_* symbol (e.g.
#     SAFETY_CMD_GET_CT_CAL's comment block, or a *different* macro that
#     legitimately needs to reference kilnlink -- this check is scoped to
#     the one line that burned the project three times, not to "no mention
#     of kilnlink anywhere in this file")
#   - kilnlink_version.h itself, or any file other than uart_task_ids.h
#
# Usage: powershell -File tools\check_uart_version_independence.ps1
#        (-DriversDir <path> to smoke-test against a simulated tree)
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

$targetMatches = @(Get-ChildItem -Path $driversDir -Filter "uart_task_ids.h" -File -Recurse)
if ($targetMatches.Count -eq 0) {
    throw "check_uart_version_independence.ps1: uart_task_ids.h not found anywhere under $driversDir -- has it moved or been renamed?"
}
if ($targetMatches.Count -gt 1) {
    $paths = ($targetMatches | ForEach-Object { $_.FullName }) -join ", "
    throw "check_uart_version_independence.ps1: uart_task_ids.h matched more than one file under $driversDir ($paths) -- cannot tell which one is the real header."
}
$targetFile = $targetMatches[0].FullName

# Same comment-stripping helper as check_link_impl_isolation.ps1 (duplicated
# rather than imported -- this project has no shared PowerShell module
# mechanism).
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

# Match the #define UART_PROTOCOL_VERSION line specifically, then check
# whether its value side mentions any KILNLINK_ symbol at all.
$definePattern = '^\s*#\s*define\s+UART_PROTOCOL_VERSION\b(.*)$'
$kilnlinkPattern = 'KILNLINK_'

$codeLines = Get-CodeOnlyLines -Path $targetFile
$failures = @()
$found = $false
for ($i = 0; $i -lt $codeLines.Count; $i++) {
    if ($codeLines[$i] -match $definePattern) {
        $found = $true
        $valueSide = $Matches[1]
        if ($valueSide -match $kilnlinkPattern) {
            $failures += "$($targetFile):$($i + 1): UART_PROTOCOL_VERSION is defined in terms of a KILNLINK_* symbol -- $($codeLines[$i].Trim())"
        }
    }
}

if (-not $found) {
    throw "check_uart_version_independence.ps1: no '#define UART_PROTOCOL_VERSION' found in $targetFile -- has it moved or been renamed? Update this script's pattern."
}

if ($failures.Count -gt 0) {
    Write-Host "UART VERSION INDEPENDENCE CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "UART_PROTOCOL_VERSION must never be re-derived from KILNLINK_PROTOCOL_VERSION (or any other KILNLINK_* symbol) -- see uart_task_ids.h's own doc comment and SaftyFW/TODO.md's 'Shared ids split out of uart_task_ids.h; PC-link ids left behind' item for why this bit real hardware three times."
}

Write-Host "UART version independence check passed: UART_PROTOCOL_VERSION is not defined in terms of any KILNLINK_* symbol."
exit 0
