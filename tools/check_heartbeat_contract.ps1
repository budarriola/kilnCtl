# checkcache: ok
# check_heartbeat_contract.ps1 -- the PC-side heartbeat that keeps
# uart_bridge.c's link watchdog fed (e3e8ec6, "The heartbeat the firmware
# required, nobody was sending") is a cross-language producer/consumer pair:
# the consumer (UART_BRIDGE_LINK_TIMEOUT_MS, a C #define) has no compiler or
# linker relationship to its producer (link_hub.py's _heartbeat_loop). Every
# check_guard_input_producers.ps1/check_unused_setters.ps1-shaped guard in
# this repo is C-only (AST/regex over .c/.h), so neither can see this pair at
# all -- ROADMAP.md M10 names this exact gap. This is the fourth instance of
# "a consumer whose producer does not exist" this project has shipped
# (current_sense_set_cal, sample_counter_advancing, i_normal_a, and this one);
# the other three now have guards. This is the heartbeat's.
#
# What "wrong" looks like here is not "the field is never assigned" (this
# check doesn't parse Python or C) -- it's three specific regressions that
# have each already happened once to a sibling contract in this codebase:
#   1. The producer call site is deleted or never reached (LinkHub.start()
#      not calling threading.Thread(target=self._heartbeat_loop, ...).start()).
#   2. The interval/timeout constants drift wide of the "comfortably under
#      UART_BRIDGE_LINK_TIMEOUT_MS/2" margin link_hub.py's own comment
#      promises -- someone loosens _HEARTBEAT_INTERVAL_S on one side, or
#      raises UART_BRIDGE_LINK_TIMEOUT_MS on the other, without checking the
#      other file.
#   3. _HEARTBEAT_TASK_ID collides with a real UART_TASK_ID_* (0-13) and
#      steals a bridge task's inbox instead of running invisibly beside it.
#
# This is regex-over-source, not a parser -- same tradeoff every other check_
# script in this repo makes, and same failure mode if the constants are ever
# rewritten in a form these patterns don't expect: this check goes quiet
# rather than wrong, which is why every number pulled out is echoed on
# failure, and why -DebugValues exists to see them on a green run too.
#
# Usage: powershell -File tools\check_heartbeat_contract.ps1
#        powershell -File tools\check_heartbeat_contract.ps1 -DebugValues

param(
    [switch]$DebugValues,
    [string]$DriversDir
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$linkHub = Join-Path $repoRoot "tools\PcTools\src\kilnctrl\link_hub.py"
if ($DriversDir) {
    $driversDir = (Resolve-Path $DriversDir).Path
} else {
    $driversDir = (Resolve-Path (Join-Path $repoRoot "firmware\KilnFW\App\drivers")).Path
}

# Resolve a bare basename to a file anywhere under $driversDir -- agnostic to
# the upcoming move of every drivers/*.c/.h file into layer subdirectories
# (drivers/<layer>/<file>). Fails loud rather than silently going blind if a
# name is missing or ambiguous.
function Resolve-DriverFile {
    param([string]$DriversDir, [string]$BaseName)
    $found = @(Get-ChildItem -Path $DriversDir -Filter $BaseName -File -Recurse)
    if ($found.Count -eq 0) {
        throw "check_heartbeat_contract: expected file '$BaseName' not found anywhere under $DriversDir -- has it moved or been renamed?"
    }
    if ($found.Count -gt 1) {
        $paths = ($found | ForEach-Object { $_.FullName }) -join ", "
        throw "check_heartbeat_contract: '$BaseName' matched more than one file under $DriversDir ($paths) -- this script cannot tell which one is the real header."
    }
    return $found[0].FullName
}

$bridgeHeader = Resolve-DriverFile -DriversDir $driversDir -BaseName "uart_bridge.h"

if (-not (Test-Path $linkHub)) {
    throw "check_heartbeat_contract: $linkHub not found -- did it move? This check is now blind, which is worse than the bug it looks for."
}

$hubText = Get-Content $linkHub -Raw
$headerText = Get-Content $bridgeHeader -Raw

$failures = @()

# --- 1. producer call site: LinkHub.start() actually starts the heartbeat
# thread, unconditionally (not behind an if, not commented out). Anchored on
# the exact target= this thread is created with, so a rename of
# _heartbeat_loop that forgets to update the start() call is caught the same
# as an outright deletion.
$startCall = [regex]::Match($hubText, 'threading\.Thread\(target=self\._heartbeat_loop,\s*daemon=True,\s*name="link-hub-heartbeat"\)\.start\(\)')
if (-not $startCall.Success) {
    $failures += "LinkHub never starts the heartbeat thread -- expected a line matching " +
                 "'threading.Thread(target=self._heartbeat_loop, daemon=True, name=`"link-hub-heartbeat`").start()' " +
                 "in $linkHub. This is the exact regression e3e8ec6 fixed: relays drop every ~5-20s on an idle link."
}
else {
    # Reject a commented-out match -- '#' anywhere before the call on its own
    # line means it's dead code that would still satisfy the regex above.
    $lineStart = $hubText.LastIndexOf("`n", $startCall.Index) + 1
    $linePrefix = $hubText.Substring($lineStart, $startCall.Index - $lineStart)
    if ($linePrefix.TrimStart() -match '^#') {
        $failures += "The heartbeat thread start() call is commented out in $linkHub -- producer exists in source but never runs."
    }
}

# --- 2. timing margin: interval + one ack timeout must stay comfortably
# under the firmware's link timeout. link_hub.py's own comment promises
# "under UART_BRIDGE_LINK_TIMEOUT_MS/2" -- enforce that, not a looser bound,
# so this check fails on the exact regression the comment is asserting won't
# happen rather than on some later, worse one.
function Get-PyFloatConst([string]$text, [string]$name) {
    $m = [regex]::Match($text, "(?m)^${name}\s*=\s*([0-9.]+)")
    if (-not $m.Success) { return $null }
    return [double]$m.Groups[1].Value
}

$intervalS = Get-PyFloatConst $hubText "_HEARTBEAT_INTERVAL_S"
$ackTimeoutS = Get-PyFloatConst $hubText "_HEARTBEAT_ACK_TIMEOUT_S"
if ($null -eq $intervalS) {
    $failures += "Could not parse _HEARTBEAT_INTERVAL_S out of $linkHub -- constant renamed or reformatted."
}
if ($null -eq $ackTimeoutS) {
    $failures += "Could not parse _HEARTBEAT_ACK_TIMEOUT_S out of $linkHub -- constant renamed or reformatted."
}

$timeoutMatch = [regex]::Match($headerText, '#define\s+UART_BRIDGE_LINK_TIMEOUT_MS\s+(\d+)u?')
if (-not $timeoutMatch.Success) {
    $failures += "Could not parse UART_BRIDGE_LINK_TIMEOUT_MS out of $bridgeHeader -- constant renamed, reformatted, or moved to another file."
}
$timeoutMs = if ($timeoutMatch.Success) { [double]$timeoutMatch.Groups[1].Value } else { $null }

if (($null -ne $intervalS) -and ($null -ne $ackTimeoutS) -and ($null -ne $timeoutMs)) {
    $worstGapMs = ($intervalS + $ackTimeoutS) * 1000.0
    $halfTimeoutMs = $timeoutMs / 2.0
    if ($DebugValues) {
        Write-Host "  _HEARTBEAT_INTERVAL_S=$intervalS  _HEARTBEAT_ACK_TIMEOUT_S=$ackTimeoutS  worst-case gap=${worstGapMs}ms"
        Write-Host "  UART_BRIDGE_LINK_TIMEOUT_MS=$timeoutMs  half=${halfTimeoutMs}ms"
    }
    if ($worstGapMs -ge $halfTimeoutMs) {
        $failures += "Heartbeat margin has thinned past what link_hub.py's own comment promises: " +
                      "(interval ${intervalS}s + ack-timeout ${ackTimeoutS}s = ${worstGapMs}ms) is not comfortably under " +
                      "UART_BRIDGE_LINK_TIMEOUT_MS/2 (${halfTimeoutMs}ms). A single missed heartbeat plus the next " +
                      "interval could now approach the firmware's 5s drop window."
    }
    if ($worstGapMs -ge $timeoutMs) {
        $failures += "Heartbeat worst-case gap (${worstGapMs}ms) has crossed UART_BRIDGE_LINK_TIMEOUT_MS itself " +
                      "(${timeoutMs}ms) -- a single missed ACK now drops relays on a healthy link."
    }
}

# --- 3. task-id isolation: _HEARTBEAT_TASK_ID must stay outside every real
# UART_TASK_ID_* the firmware registers (0-13, per link_hub.py's own
# comment). A future bridge task added at id >= 14 without checking this
# comment would silently steal the heartbeat's replies (or vice versa).
$taskIdMatch = [regex]::Match($hubText, '(?m)^_HEARTBEAT_TASK_ID\s*=\s*(\d+)')
if (-not $taskIdMatch.Success) {
    $failures += "Could not parse _HEARTBEAT_TASK_ID out of $linkHub -- constant renamed or reformatted."
}
else {
    $heartbeatTaskId = [int]$taskIdMatch.Groups[1].Value
    $taskIdsHeader = Resolve-DriverFile -DriversDir $driversDir -BaseName "uart_task_ids.h"
    if (Test-Path $taskIdsHeader) {
        $realIds = [regex]::Matches((Get-Content $taskIdsHeader -Raw), '#define\s+UART_TASK_ID_\w+\s+(\d+)') |
                   ForEach-Object { [int]$_.Groups[1].Value }
        if ($DebugValues) {
            Write-Host "  _HEARTBEAT_TASK_ID=$heartbeatTaskId  real UART_TASK_ID_* values: $($realIds -join ', ')"
        }
        if ($realIds -contains $heartbeatTaskId) {
            $failures += "_HEARTBEAT_TASK_ID ($heartbeatTaskId) collides with a real UART_TASK_ID_* in $taskIdsHeader -- " +
                          "the heartbeat's inbox would steal frames from (or be stolen by) a genuine bridge task."
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "FAILED: heartbeat contract check found $($failures.Count) issue(s):" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
    exit 1
}

Write-Host "check_heartbeat_contract: PASS -- producer runs, margin holds, task id is isolated." -ForegroundColor Green
exit 0
