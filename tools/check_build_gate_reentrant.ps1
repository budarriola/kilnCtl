# check_build_gate_reentrant.ps1 -- unit check for tools/build_gate.ps1's
# re-entrant Enter/Exit-KilnBuildGate (a build script calling another gated
# build must neither deadlock nor take a second slot).
#
# Every gate here runs on PRIVATE mutex names (a PID-keyed Local\ light-lane
# prefix, one slot, a private gate dir), so this never contends with,
# or blocks, a real build holding the machine-wide Global\ slots.
#
# Slot ownership is probed from a SEPARATE process: a named mutex is
# recursive for its owning thread, so an in-process WaitOne(0) would always
# "succeed" and prove nothing.
#
# Cases:
#   1 inner Exit keeps the outer slot; outer Exit releases it
#   2 dot-sourcing build_gate.ps1 again mid-section keeps the record
#   3 an inner section that throws (Exit in its finally) keeps the outer slot
#   4 a leaked inner Enter (no Exit) does not strand the slot at outer Exit
#   5 KILNCTL_BUILD_GATE_TIMEOUT_SEC bounds the slot wait and the timeout
#     message matches run_all_checks.ps1's BUSY pattern
#
# Contract (tools/run_all_checks.ps1): exit 0 PASS, anything else FAIL.
$ErrorActionPreference = "Stop"

$env:KILNCTL_LIGHT_GATE_MUTEX_PREFIX = "Local\kilnctl_gatetest_$($PID)_"
$env:KILNCTL_LIGHT_GATE_SLOTS = "1"
# Private record/queue dir: never touch the real C:\wt\.buildgate.
$env:KILNCTL_BUILD_GATE_DIR = Join-Path $env:TEMP "kilnctl_gatetest_dir_$PID"
# A parent gated run would make every Enter "covered"; start clean.
Remove-Item Env:KILNCTL_BUILD_GATE_HELD -ErrorAction SilentlyContinue
$global:KilnBuildGateHeld = $null
Remove-Item Env:KILNCTL_BUILD_GATE_TIMEOUT_SEC -ErrorAction SilentlyContinue

. (Join-Path $PSScriptRoot "build_gate.ps1")

$slotName = Get-KilnBuildGateMutexName -SlotIndex 0 -Lane light
# Keep this in step with Complete-CheckResult in run_all_checks.ps1.
$busyPattern = 'build gate: timed out after \d+s waiting for a (heavy|light)-lane build slot'

$tmpWork = Join-Path $env:TEMP "check_build_gate_reentrant_$PID"
New-Item -ItemType Directory -Force -Path $tmpWork | Out-Null

# Child helper: mode "probe" exits 0 if the named mutex is FREE (acquired and
# released at once), 1 if held. Mode "hold" takes it, sleeps, releases.
# Mode "probe_after" sleeps first, then probes.
$child = Join-Path $tmpWork "child.ps1"
Set-Content -Path $child -Encoding ascii -Value @'
param([string]$Mode, [string]$Name, [int]$Seconds = 0)
$m = New-Object System.Threading.Mutex($false, $Name)
if ($Mode -eq "probe_after") { Start-Sleep -Seconds $Seconds; $Mode = "probe" }
$got = $false
try { $got = $m.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $got = $true }
if ($Mode -eq "probe") {
    if ($got) { $m.ReleaseMutex(); $m.Dispose(); exit 0 }
    $m.Dispose(); exit 1
}
if (-not $got) { exit 2 }
Start-Sleep -Seconds $Seconds
$m.ReleaseMutex(); $m.Dispose(); exit 0
'@

function Start-Child {
    param([string]$Mode, [string]$Name, [int]$Seconds = 0)
    $p = Start-Process -FilePath powershell -PassThru -NoNewWindow -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $child, $Mode, $Name, $Seconds)
    $null = $p.Handle
    return $p
}
function Test-SlotHeld {
    $p = Start-Child -Mode probe -Name $slotName
    $p.WaitForExit()
    return ($p.ExitCode -ne 0)
}

$failures = @()
function Assert-True { param([bool]$Cond, [string]$What) if (-not $Cond) { $script:failures += $What; Write-Host "  FAIL: $What" } else { Write-Host "  ok:   $What" } }

try {
    Write-Host "case 1: inner Exit keeps the slot, outer Exit releases it"
    $outer = Enter-KilnBuildGate -Label "t_outer" -Lane light
    Assert-True (Test-SlotHeld) "slot held after outer Enter"
    $inner = Enter-KilnBuildGate -Label "t_inner" -Lane light
    Assert-True ([bool]$inner.Reentrant) "inner Enter is re-entrant"
    Exit-KilnBuildGate -Gate $inner
    Assert-True (Test-SlotHeld) "slot still held after inner Exit"
    Exit-KilnBuildGate -Gate $outer
    Assert-True (-not (Test-SlotHeld)) "slot free after outer Exit"

    Write-Host "case 2: re-dot-sourcing build_gate.ps1 mid-section"
    $outer = Enter-KilnBuildGate -Label "t_outer2" -Lane light
    . (Join-Path $PSScriptRoot "build_gate.ps1")
    $inner = Enter-KilnBuildGate -Label "t_inner2" -Lane light
    Assert-True ([bool]$inner.Reentrant) "inner Enter still re-entrant after re-dot-source"
    Exit-KilnBuildGate -Gate $inner
    Assert-True (Test-SlotHeld) "slot still held after inner Exit (re-dot-source)"
    Exit-KilnBuildGate -Gate $outer
    Assert-True (-not (Test-SlotHeld)) "slot free after outer Exit (re-dot-source)"

    Write-Host "case 3: inner section throws"
    $outer = Enter-KilnBuildGate -Label "t_outer3" -Lane light
    try {
        $inner = Enter-KilnBuildGate -Label "t_inner3" -Lane light
        try { throw "deliberate inner failure" } finally { Exit-KilnBuildGate -Gate $inner }
    } catch { }
    Assert-True (Test-SlotHeld) "slot still held after inner throw"
    $inner = Enter-KilnBuildGate -Label "t_inner3b" -Lane light
    Assert-True ([bool]$inner.Reentrant) "next inner Enter still re-entrant after inner throw"
    Exit-KilnBuildGate -Gate $inner
    Exit-KilnBuildGate -Gate $outer
    Assert-True (-not (Test-SlotHeld)) "slot free after outer Exit (inner throw)"

    Write-Host "case 4: leaked inner Enter"
    $outer = Enter-KilnBuildGate -Label "t_outer4" -Lane light
    $leaked = Enter-KilnBuildGate -Label "t_leak" -Lane light
    Exit-KilnBuildGate -Gate $outer
    Assert-True (-not (Test-SlotHeld)) "slot free after outer Exit despite a leaked inner"
    Exit-KilnBuildGate -Gate $leaked   # stale token: must be a harmless no-op
    $fresh = Enter-KilnBuildGate -Label "t_fresh" -Lane light
    Assert-True (-not [bool]$fresh.Reentrant) "next Enter takes a real slot again"
    Exit-KilnBuildGate -Gate $fresh
    Assert-True (-not (Test-SlotHeld)) "slot free after fresh Exit"

    Write-Host "case 5: timeouts"
    $env:KILNCTL_BUILD_GATE_TIMEOUT_SEC = "2"
    $holder = Start-Child -Mode hold -Name $slotName -Seconds 8
    Start-Sleep -Seconds 2
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $msg = ""
    try { $g = Enter-KilnBuildGate -Label "t_timeout" -Lane light -PollIntervalSeconds 1; Exit-KilnBuildGate -Gate $g } catch { $msg = "$_" }
    Assert-True ($sw.Elapsed.TotalSeconds -lt 6) "env timeout bounded the slot wait ($([int]$sw.Elapsed.TotalSeconds)s)"
    Assert-True ($msg -match $busyPattern) "slot timeout message matches the BUSY pattern"
    $holder.WaitForExit()
    Remove-Item Env:KILNCTL_BUILD_GATE_TIMEOUT_SEC -ErrorAction SilentlyContinue
}
catch {
    # Never echo the raw gate-timeout text: run_all_checks.ps1 would file
    # this check as BUSY instead of FAIL.
    $err = "$_" -replace 'timed out after', 'timed-out after'
    $failures += "unexpected error: $err"
    Write-Host "  FAIL: unexpected error: $err"
}
finally {
    Remove-Item -Recurse -Force $tmpWork -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $env:KILNCTL_BUILD_GATE_DIR -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host "check_build_gate_reentrant: FAIL ($($failures.Count) assertion(s))"
    exit 1
}
Write-Host "check_build_gate_reentrant: PASS"
exit 0
