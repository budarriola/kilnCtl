# main_baseline_lib.ps1 -- "known failures on main" baseline for run_all_checks.ps1,
# tools/land.ps1 and tools/main_baseline.ps1. Dot-source it; it defines functions only.
#
# PROBLEM (2026-10-08): every agent re-ran the full `run_all_checks.ps1 -Fast` and
# re-reported failures that were already on origin/main (check_release_manifest /
# cryptography, check_source_path_drift); the coordinator then had to check each time
# whether main was already fixed.
#
# RECORDING. A run_all_checks run on a CLEAN tree whose HEAD equals origin/main (and
# which was not filtered by -Only/-Skip) writes the per-check results (PASS / FAIL /
# SKIP / SKIP-FAST / BUSY), the mode (fast|full) and the environment fingerprint to
#   <dir>\<tree-hash>-<mode>.json      (written atomically: temp file + move/replace)
#   <dir>\latest-<mode>.json           (pointer: origin/main commit, tree, file)
# <dir> is C:\wt\.mainbaseline, override KILNCTL_MAINBASELINE_DIR (the unit test).
#
# LINEAGE RULE. A baseline only counts if its commit is an ANCESTOR of HEAD. A baseline
# from another lineage (or a commit this repo cannot resolve) is ignored. Among the
# usable baselines the one for the merge-base with origin/main is preferred; otherwise
# the newest (by commit date), with a warning that it is from an older main sha.
#
# CLASSIFICATION of a check that does not pass here (FAIL, SKIP, BUSY):
#   KNOWN  the baseline has the same non-pass status for it (FAIL, or SKIP)
#   NEW    anything else (passed on main, absent from the baseline, BUSY on main)
# FIXED  = failed/skipped on main, passes here.

$script:MainBaselineSchema = 1

function Get-MainBaselineDir {
    $d = $env:KILNCTL_MAINBASELINE_DIR
    if ([string]::IsNullOrWhiteSpace($d)) { $d = "C:\wt\.mainbaseline" }
    return $d
}

# Atomic write: temp file in the same directory, then Move (new) or Replace (existing).
function Write-MainBaselineJsonAtomic {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$Object)
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $tmp = Join-Path $dir (".{0}.{1}.{2}.tmp" -f (Split-Path -Leaf $Path), $PID, [guid]::NewGuid().ToString("N"))
    [System.IO.File]::WriteAllText($tmp, ($Object | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))
    try {
        if (Test-Path -LiteralPath $Path) { [System.IO.File]::Replace($tmp, $Path, [NullString]::Value) }
        else { [System.IO.File]::Move($tmp, $Path) }
    } catch {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        throw
    }
}

# Results: array of objects with Path and Status ("PASS","FAIL","SKIP","SKIP-FAST","BUSY").
function Write-MainBaseline {
    param([Parameter(Mandatory = $true)][string]$Dir, [Parameter(Mandatory = $true)][string]$Mode,
          [Parameter(Mandatory = $true)][string]$Commit, [Parameter(Mandatory = $true)][string]$Tree,
          [string]$Fingerprint = "", [Parameter(Mandatory = $true)]$Results)
    $rows = @($Results | Sort-Object Path | ForEach-Object { [ordered]@{ check = ([string]$_.Path -replace '/', '\'); status = [string]$_.Status } })
    $file = "$Tree-$Mode.json"
    $obj = [ordered]@{
        schema = $script:MainBaselineSchema; mode = $Mode; commit = $Commit; tree = $Tree
        fingerprint = $Fingerprint; time_utc = (Get-Date).ToUniversalTime().ToString("o")
        host = $env:COMPUTERNAME; results = $rows
    }
    Write-MainBaselineJsonAtomic -Path (Join-Path $Dir $file) -Object $obj
    $ptr = [ordered]@{ schema = $script:MainBaselineSchema; mode = $Mode; commit = $Commit; tree = $Tree
                       file = $file; time_utc = $obj.time_utc }
    Write-MainBaselineJsonAtomic -Path (Join-Path $Dir "latest-$Mode.json") -Object $ptr
    return (Join-Path $Dir $file)
}

# Recording is allowed only for a clean tree whose HEAD is origin/main or an ancestor of it.
# Returns @{ Ok; Reason; Commit; Tree }.
function Test-MainBaselineRecordable {
    param([Parameter(Mandatory = $true)][string]$RepoRoot, [string]$MainRef = "origin/main")
    $no = { param($why) [PSCustomObject]@{ Ok = $false; Reason = $why; Commit = ""; Tree = "" } }
    $head = (& git -C $RepoRoot rev-parse HEAD 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $head) { return (& $no "cannot resolve HEAD") }
    $main = (& git -C $RepoRoot rev-parse $MainRef 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $main) { return (& $no "cannot resolve $MainRef") }
    # HEAD may be behind a ref that advanced (a fetch from another session) during
    # the long run; any commit on main's history is a legitimate main state.
    if ($head -cne $main) {
        & git -C $RepoRoot merge-base --is-ancestor $head $main 2>$null
        if ($LASTEXITCODE -ne 0) { return (& $no "HEAD is not $MainRef nor an ancestor of it") }
    }
    $st = Get-CheckCacheTreeState -RepoRoot $RepoRoot
    if (-not $st.Clean) { return (& $no $st.Reason) }
    return [PSCustomObject]@{ Ok = $true; Reason = ""; Commit = $head; Tree = $st.Tree }
}

function Read-MainBaselineFile {
    param([string]$Path)
    try {
        $o = [System.IO.File]::ReadAllText($Path) | ConvertFrom-Json -ErrorAction Stop
        if ($o.schema -ne $script:MainBaselineSchema -or -not $o.commit -or $null -eq $o.results) { return $null }
        return $o
    } catch { return $null }
}

# Pick the usable baseline for HEAD. Returns @{ Baseline; Exact; Warning; Reason; Ignored }.
function Select-MainBaseline {
    param([Parameter(Mandatory = $true)][string]$Dir, [Parameter(Mandatory = $true)][string]$Mode,
          [Parameter(Mandatory = $true)][string]$RepoRoot, [string]$MainRef = "origin/main")
    $r = [PSCustomObject]@{ Baseline = $null; Exact = $false; Warning = ""; Reason = ""; Ignored = 0 }
    if (-not (Test-Path -LiteralPath $Dir)) { $r.Reason = "no baseline directory ($Dir)"; return $r }
    $files = @(Get-ChildItem -LiteralPath $Dir -Filter "*-$Mode.json" -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike 'latest-*' -and $_.Name -notlike '.*' })
    if ($files.Count -eq 0) { $r.Reason = "no $Mode baseline recorded (run tools\main_baseline.ps1 -Record)"; return $r }
    $mb = (& git -C $RepoRoot merge-base HEAD $MainRef 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { $mb = "" }
    $usable = New-Object System.Collections.ArrayList
    foreach ($f in $files) {
        $b = Read-MainBaselineFile -Path $f.FullName
        if ($null -eq $b -or $b.mode -cne $Mode) { $r.Ignored++; continue }
        & git -C $RepoRoot merge-base --is-ancestor ([string]$b.commit) HEAD 2>$null
        if ($LASTEXITCODE -ne 0) { $r.Ignored++; continue }   # other lineage / unknown commit
        $ct = 0
        $ctText = (& git -C $RepoRoot show -s --format=%ct ([string]$b.commit) 2>$null | Out-String).Trim()
        [void][long]::TryParse($ctText, [ref]$ct)
        [void]$usable.Add([PSCustomObject]@{ B = $b; Ct = $ct })
    }
    if ($usable.Count -eq 0) {
        $r.Reason = "no $Mode baseline is from an ancestor of HEAD ($($r.Ignored) from other lineages ignored)"
        return $r
    }
    $pick = $null
    if ($mb) { $pick = @($usable | Where-Object { $_.B.commit -ceq $mb })[0] }
    if ($null -ne $pick) { $r.Exact = $true }
    else {
        $pick = @($usable | Sort-Object Ct -Descending)[0]
        $r.Warning = "baseline is from an older main sha $(([string]$pick.B.commit).Substring(0,10)); merge-base with $MainRef is $(if ($mb) { $mb.Substring(0,10) } else { 'unknown' })"
    }
    $r.Baseline = $pick.B
    return $r
}

# Current: array of @{Path;Status}. Returns @{ New; Known; Fixed } as arrays of strings.
function Compare-MainBaseline {
    param([Parameter(Mandatory = $true)]$Current, [Parameter(Mandatory = $true)]$Baseline)
    $base = @{}
    foreach ($x in @($Baseline.results)) { $base[([string]$x.check -replace '/', '\').ToLowerInvariant()] = [string]$x.status }
    $bad = @('FAIL', 'SKIP', 'BUSY')
    $new = @(); $known = @(); $fixed = @()
    foreach ($c in @($Current)) {
        $key = ([string]$c.Path -replace '/', '\').ToLowerInvariant()
        $was = if ($base.ContainsKey($key)) { $base[$key] } else { "" }
        if ($bad -contains $c.Status) {
            if ($was -ceq $c.Status -and $was -cne 'BUSY') { $known += [string]$c.Path } else { $new += [string]$c.Path }
        } elseif ($c.Status -ceq 'PASS' -and ($was -ceq 'FAIL' -or $was -ceq 'SKIP')) {
            $fixed += [string]$c.Path
        }
    }
    return [PSCustomObject]@{ New = @($new | Sort-Object); Known = @($known | Sort-Object); Fixed = @($fixed | Sort-Object) }
}

# Prints the "vs main baseline" section. Returns @{ Found; Cmp; Sel }.
function Show-MainBaselineSection {
    param([Parameter(Mandatory = $true)]$Current, [Parameter(Mandatory = $true)][string]$Mode,
          [Parameter(Mandatory = $true)][string]$RepoRoot)
    $sel = Select-MainBaseline -Dir (Get-MainBaselineDir) -Mode $Mode -RepoRoot $RepoRoot
    Write-Host ""
    Write-Host "=== vs main baseline ($Mode) ===" -ForegroundColor Cyan
    if ($null -eq $sel.Baseline) {
        Write-Host "No usable baseline: $($sel.Reason). All failures are unclassified; treat them as NEW." -ForegroundColor Yellow
        return [PSCustomObject]@{ Found = $false; Cmp = $null; Sel = $sel }
    }
    $b = $sel.Baseline
    $ign = ""
    if ($sel.Ignored -gt 0) { $ign = " ($($sel.Ignored) other-lineage baseline(s) ignored)" }
    Write-Host "Baseline: origin/main $(([string]$b.commit).Substring(0,10)) recorded $($b.time_utc)$ign"
    if ($sel.Warning) { Write-Host "WARNING: $($sel.Warning)" -ForegroundColor Yellow }
    $cmp = Compare-MainBaseline -Current $Current -Baseline $b
    $nc = 'Green'; if ($cmp.New.Count) { $nc = 'Red' }
    Write-Host "NEW (fails here, passed on main): $($cmp.New.Count)" -ForegroundColor $nc
    foreach ($x in $cmp.New) { Write-Host "  NEW    $x" -ForegroundColor Red }
    Write-Host "KNOWN (also fails on main): $($cmp.Known.Count)" -ForegroundColor Yellow
    foreach ($x in $cmp.Known) { Write-Host "  KNOWN  $x" -ForegroundColor Yellow }
    Write-Host "FIXED (passes here, failed on main): $($cmp.Fixed.Count)" -ForegroundColor Green
    foreach ($x in $cmp.Fixed) { Write-Host "  FIXED  $x" -ForegroundColor Green }
    return [PSCustomObject]@{ Found = $true; Cmp = $cmp; Sel = $sel }
}
