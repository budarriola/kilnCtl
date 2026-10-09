# main_baseline.ps1 -- seed / show the "known failures on main" baseline.
#
#   -Record   mint a clean worktree at origin/main (tools\worktree_mint.ps1 -Label
#             mainbase) and start `run_all_checks.ps1 -Fast` there DETACHED. That run
#             records the baseline itself (clean tree, HEAD == origin/main), so
#             nothing more is needed here. Prints the pid and the log path. The run
#             takes the build gate like any other run; this script never touches
#             tools\build_gate.ps1. -Full runs the full suite instead of -Fast.
#             -Remove deletes the worktree afterwards (only once the run is done).
#   -Show     print the latest baseline's failing checks (default mode fast).
#
# Baseline files: C:\wt\.mainbaseline (KILNCTL_MAINBASELINE_DIR overrides).
# Logic and rules (lineage, NEW/KNOWN/FIXED): tools\main_baseline_lib.ps1.
[CmdletBinding()]
param(
    [switch]$Record,
    [switch]$Show,
    [switch]$Full,
    [string]$Remove
)
$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "checkcache_lib.ps1")
. (Join-Path $PSScriptRoot "main_baseline_lib.ps1")
$mode = if ($Full) { "full" } else { "fast" }

if ($Remove) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "worktree_mint.ps1") -Remove -Path $Remove
    exit $LASTEXITCODE
}

if ($Show) {
    $dir = Get-MainBaselineDir
    $ptrPath = Join-Path $dir "latest-$mode.json"
    if (-not (Test-Path -LiteralPath $ptrPath)) { Write-Host "No $mode baseline recorded in $dir. Run: tools\main_baseline.ps1 -Record"; exit 1 }
    $ptr = [System.IO.File]::ReadAllText($ptrPath) | ConvertFrom-Json
    $b = Read-MainBaselineFile -Path (Join-Path $dir $ptr.file)
    if ($null -eq $b) { Write-Host "latest-$mode.json points at an unreadable baseline ($($ptr.file))"; exit 1 }
    Write-Host "Baseline ($mode): origin/main $($b.commit) tree $($b.tree) recorded $($b.time_utc)"
    $bad = @($b.results | Where-Object { $_.status -in @('FAIL', 'SKIP', 'BUSY') })
    $n = @($b.results).Count
    Write-Host "$n checks recorded; $($bad.Count) not passing:"
    foreach ($x in $bad) { Write-Host "  $($x.status)  $($x.check)" }
    exit 0
}

if ($Record) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "worktree_mint.ps1") -Label mainbase -Base origin/main 2>&1 | Out-String
    $m = [regex]::Match($out, '(?m)^WORKTREE:\s*(.+?)\s*$')
    if (-not $m.Success) { Write-Host $out; Write-Host "worktree mint failed"; exit 1 }
    $wt = $m.Groups[1].Value
    $log = "$wt.run.log"    # beside the worktree, never inside it (it must stay clean)
    # Start-Process (PS 5.1) does not quote array elements: quote the path so a space cannot split it.
    $args2 = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", ('"' + (Join-Path $wt "tools\run_all_checks.ps1") + '"'))
    if (-not $Full) { $args2 += "-Fast" }
    $p = Start-Process -FilePath "powershell.exe" -ArgumentList $args2 -WorkingDirectory $wt `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" -WindowStyle Hidden -PassThru
    Write-Host "Baseline run ($mode) started detached."
    Write-Host "WORKTREE: $wt"
    Write-Host "PID: $($p.Id)"
    Write-Host "LOG: $log"
    Write-Host "When the process exits: tools\main_baseline.ps1 -Show ; then tools\main_baseline.ps1 -Remove $wt"
    exit 0
}

Write-Host "usage: main_baseline.ps1 -Record [-Full] | -Show [-Full] | -Remove <worktree>"
exit 2
