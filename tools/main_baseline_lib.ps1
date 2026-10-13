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
# LINEAGE RULE. A baseline only counts if its commit is an ANCESTOR of HEAD, OR (dev flow, review
# F6) its recorded TREE equals the tree of a commit in HEAD's recent history: a promote commit on
# main is a single-parent squash that shares its tree with the dev commit it names, so it is never
# an ancestor of dev but its tree is. Such a baseline is Exact when that tree is origin/main's
# current tree. A baseline from another lineage (or a commit this repo cannot resolve) is ignored. Among the
# usable baselines the one for the merge-base with origin/main is preferred; otherwise
# the newest (by commit date), with a warning that it is from an older main sha.
#
# CLASSIFICATION of a check that does not pass here (FAIL, SKIP, BUSY):
#   KNOWN  the baseline has the same non-pass status for it (FAIL, or SKIP)
#   NEW    anything else (passed on main, absent from the baseline, BUSY on main)
# FIXED  = failed/skipped on main, passes here.
# KNOWN only EXCUSES a failure (-FailOnlyOnNew exit 0, land -AllowKnownFailures) when the
# selected baseline is Exact (its commit == merge-base of HEAD with origin/main). From an
# older baseline the classification is shown but every failure counts as NEW (a check that
# failed at B0, was fixed at B1 and re-broke on a branch from B1 must not read as KNOWN).
# A failing check also stores a SIGNATURE (hash of its normalised FAIL/assert/error lines);
# a KNOWN failure whose signature differs from main's is CHANGED and counts as NEW. A
# baseline row without a signature (older format) is CHANGED when the current failure has
# one (a signature on only one side is never KNOWN); both sides unsigned matches, with a warning. A SKIP recorded
# on another host is not KNOWN here (the missing toolchain may be this host's only).
# DEV FLOW (2026-10-10): dev is never an ancestor of main (squash promote), so a run on dev has no main
# baseline unless someone ran the suite on clean main. A coordinator full run on a CLEAN dev tip
# (HEAD == origin/dev or an ancestor of it) therefore also records, keyed by its tree (ref=origin/dev).
# Such a baseline is an ancestor of every later dev commit, and is Exact when its commit is the merge-base
# of HEAD with origin/dev (or its tree is origin/dev's tree). When dev is later promoted, the squash commit
# shares the dev tip's tree, so the same file is also the main baseline for that tree (tree match above).
# RECORDING also requires HEAD and `git status --porcelain` identical at run START and END.

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
          [string]$Fingerprint = "", [Parameter(Mandatory = $true)]$Results, [string]$RepoRoot = "", [string]$Ref = "origin/main")
    $rows = @($Results | Sort-Object Path | ForEach-Object {
        $row = [ordered]@{ check = ([string]$_.Path -replace '/', '\'); status = [string]$_.Status }
        $sig = if ($_.PSObject.Properties['Signature']) { [string]$_.Signature } else { "" }
        if (($_.Status -ceq 'FAIL' -or $_.Status -ceq 'SKIP') -and $sig) { $row['sig'] = $sig }
        $row })
    $file = "$Tree-$Mode.json"
    $obj = [ordered]@{
        schema = $script:MainBaselineSchema; mode = $Mode; commit = $Commit; tree = $Tree
        ref = $Ref; fingerprint = $Fingerprint; time_utc = (Get-Date).ToUniversalTime().ToString("o")
        host = $env:COMPUTERNAME; results = $rows
    }
    Write-MainBaselineJsonAtomic -Path (Join-Path $Dir $file) -Object $obj
    $ptr = [ordered]@{ schema = $script:MainBaselineSchema; mode = $Mode; commit = $Commit; tree = $Tree
                       file = $file; time_utc = $obj.time_utc }
    # Move the pointer only forward: the new commit must equal or descend from the
    # pointer's commit (a slow older run must not overwrite a newer pointer).
    $pp = Join-Path $Dir "latest-$Mode.json"
    $move = $true
    if ($RepoRoot -and (Test-Path -LiteralPath $pp)) {
        try {
            $old = [System.IO.File]::ReadAllText($pp) | ConvertFrom-Json -ErrorAction Stop
            $oc = [string]$old.commit
            if ($oc -and $oc -cne $Commit) {
                & git -C $RepoRoot cat-file -e "$oc^{commit}" 2>$null
                if ($LASTEXITCODE -eq 0) {
                    & git -C $RepoRoot merge-base --is-ancestor $oc $Commit 2>$null
                    if ($LASTEXITCODE -ne 0) { $move = $false }
                }
            }
        } catch { }
    }
    if ($move) { Write-MainBaselineJsonAtomic -Path $pp -Object $ptr }
    return (Join-Path $Dir $file)
}

# Failure signature: SHA1 of the sorted unique normalised FAIL/assert/error lines of a
# check's output (timestamps, paths, temp names, hex ids and numbers stripped). "" when
# the output has no such line.
function Get-MainFailureSignature {
    param([string]$Output, $ExitCode = $null, [string]$Reason = "")
    $lines = New-Object System.Collections.Generic.HashSet[string]
    $norm = {
        param([string]$l)
        $n = $l.Trim()
        $n = [regex]::Replace($n, '\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(\.\d+)?Z?', '<ts>')
        $n = [regex]::Replace($n, '\d{1,2}:\d{2}:\d{2}(\.\d+)?', '<t>')
        $n = [regex]::Replace($n, '[A-Za-z]:[\\/][^\s''"()\[\]]*', '<path>')
        $n = [regex]::Replace($n, '(?<![\w.])/(?:[\w.\-]+/)+[\w.\-]*', '<path>')
        $n = [regex]::Replace($n, '[A-Za-z0-9_]*(?:test|tmp|scratch|checkbuild)_[0-9a-f]{6,}', '<tmp>')
        $n = [regex]::Replace($n, '\b[0-9a-fA-F]{7,}\b', '<hex>')
        $n = [regex]::Replace($n, '\d+', '#')
        return $n
    }
    # The exit code is part of the signature (TIMEOUT vs 1 differ even with no FAIL line).
    if ($null -ne $ExitCode -and [string]$ExitCode -ne "") { [void]$lines.Add("exit=" + [string]$ExitCode) }
    # A SKIP's signature is its reason line, so a SKIP for a new reason is CHANGED.
    if (-not [string]::IsNullOrWhiteSpace($Reason)) { [void]$lines.Add("reason=" + (& $norm $Reason)) }
    foreach ($l in ($Output -split "`r?`n")) {
        if ($l -notmatch '(?i)\b(FAIL|FAILED|FAILURE|ASSERT\w*|ERROR|exception|TIMEOUT)\b') { continue }
        [void]$lines.Add((& $norm $l))
    }
    if ($lines.Count -eq 0) { return "" }
    $text = (@($lines) | Sort-Object { $_ } -CaseSensitive) -join "`n"
    $sha = [System.Security.Cryptography.SHA1]::Create()
    return ([BitConverter]::ToString($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($text))) -replace '-', '').ToLowerInvariant()
}

# {path -> output} from a run_all_checks log ("--- <path> (exit N) ---" sections).
function Get-LogFailureOutputs {
    param([string]$Text)
    $map = @{}
    $ms = [regex]::Matches($Text, '(?m)^--- (\S+) \(exit [^)]*\) ---[ \t]*\r?$')
    for ($i = 0; $i -lt $ms.Count; $i++) {
        $start = $ms[$i].Index + $ms[$i].Length
        $end = $Text.Length
        if ($i + 1 -lt $ms.Count) { $end = $ms[$i + 1].Index }
        $body = $Text.Substring($start, $end - $start)
        $sm = [regex]::Match($body, '(?m)^\s*\d+ passed,')
        if ($sm.Success) { $body = $body.Substring(0, $sm.Index) }
        $map[([string]$ms[$i].Groups[1].Value -replace '/', '\')] = $body
    }
    return $map
}

# HEAD + `git status --porcelain` snapshot; captured at run start and compared at the end.
function Get-MainBaselineStartState {
    param([Parameter(Mandatory = $true)][string]$RepoRoot)
    $head = (& git -C $RepoRoot rev-parse HEAD 2>$null | Out-String).Trim()
    $st = (& git -C $RepoRoot status --porcelain 2>$null | Out-String)
    return [PSCustomObject]@{ Head = $head; Status = $st }
}

# Recording is allowed only for a clean tree whose HEAD is origin/main or an ancestor of it.
# Returns @{ Ok; Reason; Commit; Tree }.
function Test-MainBaselineRecordable {
    param([Parameter(Mandatory = $true)][string]$RepoRoot, [string]$MainRef = "origin/main", $Start = $null, [string]$DevRef = "origin/dev")
    $no = { param($why) [PSCustomObject]@{ Ok = $false; Reason = $why; Commit = ""; Tree = "" } }
    $head = (& git -C $RepoRoot rev-parse HEAD 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $head) { return (& $no "cannot resolve HEAD") }
    $main = (& git -C $RepoRoot rev-parse $MainRef 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $main) { return (& $no "cannot resolve $MainRef") }
    # HEAD may be behind a ref that advanced (a fetch from another session) during
    # the long run; any commit on main's history is a legitimate main state.
    $ref = $MainRef
    if ($head -cne $main) {
        & git -C $RepoRoot merge-base --is-ancestor $head $main 2>$null
        if ($LASTEXITCODE -ne 0) {
            # Dev flow: a clean checkout of dev history records a dev-tip baseline.
            $dev = ""
            if ($DevRef) { $dev = (& git -C $RepoRoot rev-parse --verify --quiet "$DevRef^{commit}" 2>$null | Out-String).Trim() }
            $onDev = $false
            if ($dev) {
                if ($head -ceq $dev) { $onDev = $true }
                else { & git -C $RepoRoot merge-base --is-ancestor $head $dev 2>$null; $onDev = ($LASTEXITCODE -eq 0) }
            }
            if (-not $onDev) { return (& $no "HEAD is not $MainRef$(if ($DevRef) { " or $DevRef" }) nor an ancestor of either (only a clean checkout of main/dev history seeds a baseline)") }
            $ref = $DevRef
        }
    }
    $st = Get-CheckCacheTreeState -RepoRoot $RepoRoot
    if (-not $st.Clean) { return (& $no $st.Reason) }
    if ($null -ne $Start) {
        if ($Start.Head -cne $head) { return (& $no "HEAD moved during the run") }
        $now = (& git -C $RepoRoot status --porcelain 2>$null | Out-String)
        if ($now -cne $Start.Status) { return (& $no "working tree status changed during the run") }
        if ($Start.Status.Trim()) { return (& $no "working tree was not clean at run start") }
    }
    return [PSCustomObject]@{ Ok = $true; Reason = ""; Commit = $head; Tree = $st.Tree; Ref = $ref }
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
          [Parameter(Mandatory = $true)][string]$RepoRoot, [string]$MainRef = "origin/main", [string]$DevRef = "origin/dev")
    $r = [PSCustomObject]@{ Baseline = $null; Exact = $false; Warning = ""; Reason = ""; Ignored = 0 }
    if (-not (Test-Path -LiteralPath $Dir)) { $r.Reason = "no baseline directory ($Dir)"; return $r }
    $files = @(Get-ChildItem -LiteralPath $Dir -Filter "*-$Mode.json" -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike 'latest-*' -and $_.Name -notlike '.*' })
    if ($files.Count -eq 0) { $r.Reason = "no $Mode baseline recorded (run tools\main_baseline.ps1 -Record)"; return $r }
    $mb = (& git -C $RepoRoot merge-base HEAD $MainRef 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { $mb = "" }
    $usable = New-Object System.Collections.ArrayList
    $histTrees = $null
    $mainTree = (& git -C $RepoRoot rev-parse "$MainRef^{tree}" 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { $mainTree = "" }
    # Dev flow: merge-base / tree of origin/dev also make a baseline Exact (see DEV FLOW above).
    # for-each-ref prints nothing and no stderr when the ref is absent (a missing origin/dev must not
    # throw under the caller's $ErrorActionPreference = Stop).
    $mbDev = ""; $devTree = ""
    if ($DevRef -and (& git -C $RepoRoot for-each-ref "--format=%(objectname)" "refs/remotes/$DevRef" 2>$null | Out-String).Trim()) {
        $mbDev = (& git -C $RepoRoot merge-base HEAD $DevRef 2>$null | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) { $mbDev = "" }
        $devTree = (& git -C $RepoRoot rev-parse "$DevRef^{tree}" 2>$null | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) { $devTree = "" }
    }
    # B2a: on a main-side HEAD the dev merge-base is the dev fork point, a stale main commit that is an
    # ancestor of (or equal to) the main merge-base. It must not make a baseline Exact there.
    if ($mbDev -and $mb -and $mbDev -cne $mb) {
        & git -C $RepoRoot merge-base --is-ancestor $mbDev $mb 2>$null
        if ($LASTEXITCODE -eq 0) { $mbDev = "" }
    }
    $headTree = (& git -C $RepoRoot rev-parse "HEAD^{tree}" 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { $headTree = "" }
    foreach ($f in $files) {
        $b = Read-MainBaselineFile -Path $f.FullName
        if ($null -eq $b -or $b.mode -cne $Mode) { $r.Ignored++; continue }
        $treeMatch = $false
        & git -C $RepoRoot merge-base --is-ancestor ([string]$b.commit) HEAD 2>$null
        if ($LASTEXITCODE -ne 0) {
            # Not an ancestor: usable only through an equal tree somewhere in HEAD's history.
            $bt = if ($b.PSObject.Properties['tree']) { [string]$b.tree } else { "" }
            if ($null -eq $histTrees) {
                $histTrees = @{}
                foreach ($t in @(& git -C $RepoRoot log -n 2000 --format=%T HEAD 2>$null)) { if ($t) { $histTrees[[string]$t] = $true } }
            }
            if (-not $bt -or -not $histTrees.ContainsKey($bt)) { $r.Ignored++; continue }   # other lineage / unknown commit
            $treeMatch = $true
        }
        $ct = 0
        $ctText = (& git -C $RepoRoot show -s --format=%ct ([string]$b.commit) 2>$null | Out-String).Trim()
        [void][long]::TryParse($ctText, [ref]$ct)
        $isExact = ($mb -and $b.commit -ceq $mb) -or ($mainTree -and $b.PSObject.Properties['tree'] -and ([string]$b.tree) -ceq $mainTree) -or ($mbDev -and $b.commit -ceq $mbDev) -or ($devTree -and $b.PSObject.Properties['tree'] -and ([string]$b.tree) -ceq $devTree)
        # B2b: rank Exact baselines by specificity (HEAD tree, then dev merge-base/tree, then main), never by file order.
        $bTree = if ($b.PSObject.Properties['tree']) { [string]$b.tree } else { "" }
        $rank = 9
        if ($isExact) {
            if ($headTree -and $bTree -ceq $headTree) { $rank = 0 }
            elseif (($mbDev -and $b.commit -ceq $mbDev) -or ($devTree -and $bTree -ceq $devTree)) { $rank = 1 }
            else { $rank = 2 }
        }
        [void]$usable.Add([PSCustomObject]@{ B = $b; Ct = $ct; Exact = [bool]$isExact; Rank = $rank })
    }
    if ($usable.Count -eq 0) {
        $r.Reason = "no $Mode baseline is from an ancestor of HEAD ($($r.Ignored) from other lineages ignored)"
        return $r
    }
    $pick = $null
    $pick = @($usable | Where-Object { $_.Exact } | Sort-Object @{Expression={$_.Rank}}, @{Expression={$_.Ct};Descending=$true}, @{Expression={[string]$_.B.commit}})[0]
    if ($null -ne $pick) { $r.Exact = $true }
    else {
        $pick = @($usable | Sort-Object Ct -Descending)[0]
        $r.Warning = "baseline is from an older main sha $(([string]$pick.B.commit).Substring(0,10)); merge-base with $MainRef is $(if ($mb) { $mb.Substring(0,10) } else { 'unknown' })"
    }
    $r.Baseline = $pick.B
    return $r
}

# Current: array of @{Path;Status[;Signature]}. Returns @{ New; Known; Fixed; Changed;
# Downgraded; Warnings }. New includes Changed and Downgraded (what counts for exit codes).
# Known is only populated when -Exact (baseline commit == merge-base); otherwise would-be
# KNOWN failures go to Downgraded and New.
function Compare-MainBaseline {
    param([Parameter(Mandatory = $true)]$Current, [Parameter(Mandatory = $true)]$Baseline,
          [switch]$Exact, [string]$HostName = $env:COMPUTERNAME)
    $base = @{}; $bsig = @{}
    foreach ($x in @($Baseline.results)) {
        $k = ([string]$x.check -replace '/', '\').ToLowerInvariant()
        $base[$k] = [string]$x.status
        $bsig[$k] = if ($x.PSObject.Properties['sig']) { [string]$x.sig } else { "" }
    }
    $bhost = if ($Baseline.PSObject.Properties['host']) { [string]$Baseline.host } else { "" }
    $bad = @('FAIL', 'SKIP', 'BUSY')
    $new = @(); $known = @(); $fixed = @(); $changed = @(); $down = @(); $warn = @()
    foreach ($c in @($Current)) {
        $key = ([string]$c.Path -replace '/', '\').ToLowerInvariant()
        $was = if ($base.ContainsKey($key)) { $base[$key] } else { "" }
        if ($bad -contains $c.Status) {
            if ($was -ceq $c.Status -and $was -cne 'BUSY') {
                $p = [string]$c.Path
                $csig = if ($c.PSObject.Properties['Signature']) { [string]$c.Signature } else { "" }
                $sigged = ($c.Status -ceq 'FAIL' -or $c.Status -ceq 'SKIP')
                # A signature on one side and not the other, or two different ones, is CHANGED.
                if ($sigged -and ([bool]$bsig[$key] -or [bool]$csig) -and $bsig[$key] -cne $csig) {
                    $changed += $p; $new += $p
                } elseif ($c.Status -ceq 'SKIP' -and $bhost -and $HostName -and $bhost -cne $HostName) {
                    $warn += "SKIP of $p was recorded on host $bhost, this is $HostName; not KNOWN"
                    $new += $p
                } elseif (-not $Exact) {
                    $down += $p; $new += $p
                } else {
                    if ($sigged -and -not ($bsig[$key] -and $csig)) { $warn += "no failure signature for $p (neither side has one); matched on status only" }
                    $known += $p
                }
            } else { $new += [string]$c.Path }
        } elseif ($c.Status -ceq 'PASS' -and ($was -ceq 'FAIL' -or $was -ceq 'SKIP')) {
            $fixed += [string]$c.Path
        }
    }
    return [PSCustomObject]@{ New = @($new | Sort-Object); Known = @($known | Sort-Object); Fixed = @($fixed | Sort-Object)
                              Changed = @($changed | Sort-Object); Downgraded = @($down | Sort-Object); Warnings = @($warn) }
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
    $bref = if ($b.PSObject.Properties['ref']) { [string]$b.ref } else { "origin/main" }
    Write-Host "Baseline: $bref $(([string]$b.commit).Substring(0,10)) recorded $($b.time_utc)$ign"
    if ($sel.Warning) { Write-Host "WARNING: $($sel.Warning)" -ForegroundColor Yellow }
    $cmp = Compare-MainBaseline -Current $Current -Baseline $b -Exact:$sel.Exact
    if (-not $sel.Exact) {
        Write-Host "Baseline is NOT at the merge-base: KNOWN is informational only; every failure counts as NEW. Record a baseline at the merge-base (tools\main_baseline.ps1 -Record)." -ForegroundColor Yellow
    }
    foreach ($w in $cmp.Warnings) { Write-Host "WARNING: $w" -ForegroundColor Yellow }
    $nc = 'Green'; if ($cmp.New.Count) { $nc = 'Red' }
    Write-Host "NEW (fails here, passed on main): $($cmp.New.Count)" -ForegroundColor $nc
    foreach ($x in $cmp.New) { Write-Host "  NEW    $x" -ForegroundColor Red }
    foreach ($x in $cmp.Changed) { Write-Host "  CHANGED $x (fails on main too, but with a different failure signature; counts as NEW)" -ForegroundColor Red }
    Write-Host "KNOWN (also fails on main): $($cmp.Known.Count)" -ForegroundColor Yellow
    foreach ($x in $cmp.Known) { Write-Host "  KNOWN  $x" -ForegroundColor Yellow }
    foreach ($x in $cmp.Downgraded) { Write-Host "  (known-at-older-main, counts as NEW) $x" -ForegroundColor Yellow }
    Write-Host "FIXED (passes here, failed on main): $($cmp.Fixed.Count)" -ForegroundColor Green
    foreach ($x in $cmp.Fixed) { Write-Host "  FIXED  $x" -ForegroundColor Green }
    return [PSCustomObject]@{ Found = $true; Cmp = $cmp; Sel = $sel }
}
