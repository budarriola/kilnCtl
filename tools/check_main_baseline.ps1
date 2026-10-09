# check_main_baseline.ps1 -- unit test for the "known failures on main" baseline
# (tools/main_baseline_lib.ps1, run_all_checks.ps1 -FailOnlyOnNew, land.ps1
# -AllowKnownFailures). Synthetic result files and a scratch git repo under the
# temp dir; no board, no network, nothing outside temp.
# Cases: NEW/KNOWN/FIXED classification, the ancestor (lineage) rule and the
# merge-base preference, the atomic write, recording only at a clean
# HEAD == origin/main, -FailOnlyOnNew through a scratch run_all_checks, and
# land.ps1 -AllowKnownFailures (dry run).
#
# Negative-tested by hand 2026-10-08: each guard broken in turn, RED, restored
# (docs/audits/GATE_NEGATIVE_TEST_EVIDENCE.md).
# checkcache: ok

$ErrorActionPreference = "Continue"
$tools = $PSScriptRoot
. (Join-Path $tools "checkcache_lib.ps1")
. (Join-Path $tools "main_baseline_lib.ps1")
$script:fails = 0
function Assert([bool]$cond, [string]$what) {
    if ($cond) { Write-Host "  ok: $what" } else { Write-Host "  FAIL: $what" -ForegroundColor Red; $script:fails++ }
}
function Same($a, $b) { return ((@($a) -join '|') -ceq (@($b) -join '|')) }

$env:GIT_AUTHOR_NAME = "t"; $env:GIT_AUTHOR_EMAIL = "t@example.invalid"
$env:GIT_COMMITTER_NAME = "t"; $env:GIT_COMMITTER_EMAIL = "t@example.invalid"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("mainbase_test_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$bdir = Join-Path $tmp "baselines"
$savedDir = $env:KILNCTL_MAINBASELINE_DIR
$env:KILNCTL_MAINBASELINE_DIR = $bdir
function Row($p, $s) { [pscustomobject]@{ Path = $p; Status = $s } }
function Commit-File([string]$dir, [string]$name, [string]$text, [string]$msg) {
    $full = Join-Path $dir $name
    $parent = Split-Path -Parent $full
    if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Set-Content -LiteralPath $full -Value $text -Encoding ascii
    git -C $dir add -- $name *>$null
    git -C $dir commit -m $msg *>$null
}
function Rev([string]$dir, [string]$ref) { (git -C $dir rev-parse $ref).Trim() }
function Tree([string]$dir, [string]$ref) { (git -C $dir rev-parse "$ref^{tree}").Trim() }

try {
    # ---------------------------------------------------------------- 1
    Write-Host "case: NEW / KNOWN / FIXED classification"
    $base = [pscustomobject]@{ results = @(
        (Row "tools\check_a.ps1" "PASS"), (Row "tools\check_b.ps1" "FAIL"), (Row "tools\check_c.ps1" "FAIL"),
        (Row "tools\check_d.ps1" "SKIP"), (Row "tools\check_e.ps1" "PASS"), (Row "tools\check_f.ps1" "BUSY"),
        (Row "tools\check_g.ps1" "SKIP-FAST")) | ForEach-Object { [pscustomobject]@{ check = $_.Path; status = $_.Status } } }
    $cur = @((Row "tools/check_a.ps1" "FAIL"),      # passed on main        -> NEW
             (Row "tools\check_b.ps1" "FAIL"),      # failed on main        -> KNOWN
             (Row "tools\check_c.ps1" "PASS"),      # failed on main        -> FIXED
             (Row "tools\check_d.ps1" "SKIP"),      # skipped on main       -> KNOWN
             (Row "tools\check_e.ps1" "PASS"),      # unchanged             -> nothing
             (Row "tools\check_f.ps1" "FAIL"),      # BUSY (unknown) on main-> NEW
             (Row "tools\check_h.ps1" "FAIL"),      # absent on main        -> NEW
             (Row "tools\check_g.ps1" "SKIP-FAST")) # -Fast skip            -> nothing
    $cmp = Compare-MainBaseline -Current $cur -Baseline $base -Exact
    Assert (Same $cmp.New @("tools/check_a.ps1", "tools\check_f.ps1", "tools\check_h.ps1")) "NEW = passed-on-main, BUSY-on-main and absent checks (slashes normalised)"
    Assert (Same $cmp.Known @("tools\check_b.ps1", "tools\check_d.ps1")) "KNOWN = same non-pass status on main"
    Assert (Same $cmp.Fixed @("tools\check_c.ps1")) "FIXED = fails on main, passes here"

    Write-Host "case: non-exact baseline never excuses (finding 1)"
    $cmpI = Compare-MainBaseline -Current $cur -Baseline $base
    Assert ($cmpI.Known.Count -eq 0 -and (Same $cmpI.Downgraded @("tools\check_b.ps1", "tools\check_d.ps1"))) "without -Exact nothing is KNOWN, would-be KNOWN is Downgraded"
    Assert (($cmpI.New -contains "tools\check_b.ps1") -and ($cmpI.New -contains "tools\check_d.ps1")) "Downgraded failures count as NEW"

    Write-Host "case: failure signature (finding 2)"
    $sigA = Get-MainFailureSignature -Output "x`nFAIL: expected 3 got 4 at C:\wt\abc_123\foo.c 12:01:02 2026-10-08T01:02:03Z tmp_deadbeef1"
    $sigB = Get-MainFailureSignature -Output "FAIL: expected 5 got 9 at D:\other\bar.c 23:59:59 2026-01-01T00:00:00Z tmp_cafebabe22"
    $sigC = Get-MainFailureSignature -Output "FAIL: something entirely different"
    Assert ($sigA -and $sigA -ceq $sigB) "signature ignores timestamps, paths, temp names and numbers"
    Assert ($sigA -cne $sigC) "different failure text -> different signature"
    Assert ((Get-MainFailureSignature -Output "all quiet") -ceq "") "no FAIL line -> empty signature"
    $sbase = [pscustomobject]@{ host = "H"; results = @(
        [pscustomobject]@{ check = "tools\check_s.ps1"; status = "FAIL"; sig = $sigA },
        [pscustomobject]@{ check = "tools\check_o.ps1"; status = "FAIL" }) }
    $scur = @([pscustomobject]@{ Path = "tools\check_s.ps1"; Status = "FAIL"; Signature = $sigC },
              [pscustomobject]@{ Path = "tools\check_o.ps1"; Status = "FAIL"; Signature = "" })
    $cs = Compare-MainBaseline -Current $scur -Baseline $sbase -Exact
    Assert ((Same $cs.Changed @("tools\check_s.ps1")) -and ($cs.New -contains "tools\check_s.ps1")) "same check failing differently is CHANGED and counts as NEW"
    Assert ((Same $cs.Known @("tools\check_o.ps1")) -and $cs.Warnings.Count -ge 1) "both sides unsigned matches on status only, with a warning"
    $scur2 = @([pscustomobject]@{ Path = "tools\check_s.ps1"; Status = "FAIL"; Signature = $sigB })
    Assert ((Same (Compare-MainBaseline -Current $scur2 -Baseline $sbase -Exact).Known @("tools\check_s.ps1"))) "same signature stays KNOWN"

    Write-Host "case: one-sided signature, exit code and SKIP reason are CHANGED (review finding 1)"
    # exit code is part of the signature: TIMEOUT vs exit 1 with identical (or no) FAIL lines
    $sigT = Get-MainFailureSignature -Output "TIMEOUT: exceeded 600s per-check wall-clock cap" -ExitCode "timeout"
    $sigT2 = Get-MainFailureSignature -Output "partial output, no keyword line" -ExitCode "timeout"
    $sig1 = Get-MainFailureSignature -Output "partial output, no keyword line" -ExitCode 1
    Assert ($sigT2 -and $sigT2 -cne $sig1) "same output, TIMEOUT vs exit 1 -> different signature"
    Assert ((Get-MainFailureSignature -Output "TIMEOUT: x") -cne "") "TIMEOUT is a signature keyword"
    $obase = [pscustomobject]@{ host = "H"; results = @(
        [pscustomobject]@{ check = "tools\check_p.ps1"; status = "FAIL"; sig = $sig1 },
        [pscustomobject]@{ check = "tools\check_q.ps1"; status = "FAIL" },
        [pscustomobject]@{ check = "tools\check_r.ps1"; status = "FAIL"; sig = $sig1 },
        [pscustomobject]@{ check = "tools\check_k.ps1"; status = "SKIP"; sig = (Get-MainFailureSignature -Reason "SKIP: no arm-none-eabi-gcc") },
        [pscustomobject]@{ check = "tools\check_k2.ps1"; status = "SKIP"; sig = (Get-MainFailureSignature -Reason "SKIP: no arm-none-eabi-gcc") }) }
    $ocur = @([pscustomobject]@{ Path = "tools\check_p.ps1"; Status = "FAIL"; Signature = $sigT2 },   # exit 1 -> TIMEOUT, no FAIL line
              [pscustomobject]@{ Path = "tools\check_q.ps1"; Status = "FAIL"; Signature = $sig1 },    # baseline row has no signature
              [pscustomobject]@{ Path = "tools\check_r.ps1"; Status = "FAIL"; Signature = "" },       # current has none, baseline does
              [pscustomobject]@{ Path = "tools\check_k.ps1"; Status = "SKIP"; Signature = (Get-MainFailureSignature -Reason "SKIP: no cmake") },
              [pscustomobject]@{ Path = "tools\check_k2.ps1"; Status = "SKIP"; Signature = (Get-MainFailureSignature -Reason "SKIP: no arm-none-eabi-gcc") })
    $co = Compare-MainBaseline -Current $ocur -Baseline $obase -Exact -HostName "H"
    Assert (Same $co.Changed @("tools\check_k.ps1", "tools\check_p.ps1", "tools\check_q.ps1", "tools\check_r.ps1")) "TIMEOUT-vs-exit1, one-sided signatures and a new SKIP reason are CHANGED"
    Assert ((Same $co.Known @("tools\check_k2.ps1")) -and ($co.New.Count -eq 4)) "a SKIP for the same reason stays KNOWN"

    Write-Host "case: SKIP recorded on another host (finding 5)"
    $hb = [pscustomobject]@{ host = "OTHERHOST"; results = @([pscustomobject]@{ check = "tools\check_t.ps1"; status = "SKIP" }) }
    $hc = @([pscustomobject]@{ Path = "tools\check_t.ps1"; Status = "SKIP" })
    $ch = Compare-MainBaseline -Current $hc -Baseline $hb -Exact -HostName "HERE"
    Assert ($ch.Known.Count -eq 0 -and $ch.New -contains "tools\check_t.ps1" -and $ch.Warnings.Count -eq 1) "SKIP from another host is NEW with a warning"
    Assert ((Compare-MainBaseline -Current $hc -Baseline $hb -Exact -HostName "OTHERHOST").Known.Count -eq 1) "SKIP from the same host is KNOWN"

    # ---------------------------------------------------------------- 2
    Write-Host "case: atomic write"
    $res = @((Row "tools\check_a.ps1" "PASS"), (Row "tools\check_b.ps1" "FAIL"))
    $f1 = Write-MainBaseline -Dir $bdir -Mode "fast" -Commit ("a" * 40) -Tree ("b" * 40) -Fingerprint "fp" -Results $res
    Assert (Test-Path -LiteralPath $f1) "baseline file written as <tree>-<mode>.json"
    Assert ((Split-Path -Leaf $f1) -ceq (("b" * 40) + "-fast.json")) "file name is <tree-hash>-<mode>.json"
    $f1b = Write-MainBaseline -Dir $bdir -Mode "fast" -Commit ("a" * 40) -Tree ("b" * 40) -Fingerprint "fp" -Results ($res + (Row "tools\check_z.ps1" "PASS"))
    $j = Read-MainBaselineFile -Path $f1b
    Assert (@($j.results).Count -eq 3) "rewriting the same tree replaces the file"
    $tmps = @(Get-ChildItem -LiteralPath $bdir -Force -Filter "*.tmp" -ErrorAction SilentlyContinue)
    Assert ($tmps.Count -eq 0) "no temp file left behind"
    $ptr = [System.IO.File]::ReadAllText((Join-Path $bdir "latest-fast.json")) | ConvertFrom-Json
    Assert ($ptr.commit -ceq ("a" * 40) -and $ptr.tree -ceq ("b" * 40) -and $ptr.file -ceq (("b" * 40) + "-fast.json")) "latest-fast.json names the commit, tree and file"
    # A failed write must leave the previous file intact: the destination is a
    # directory, so the final move cannot succeed.
    $blocked = Join-Path $bdir "blocked.json"
    New-Item -ItemType Directory -Path $blocked | Out-Null
    $threw = $false
    try { Write-MainBaselineJsonAtomic -Path $blocked -Object @{ x = 1 } } catch { $threw = $true }
    Assert $threw "a failed final move throws"
    Assert (@(Get-ChildItem -LiteralPath $bdir -Force -Filter "*.tmp").Count -eq 0) "a failed write removes its temp file"
    Remove-Item -LiteralPath $bdir -Recurse -Force

    # ---------------------------------------------------------------- 3
    Write-Host "case: lineage (ancestor) rule and merge-base preference"
    $repo = Join-Path $tmp "repo"
    git init -b main $repo *>$null
    Commit-File $repo "f.txt" "1" "A"
    $A = Rev $repo HEAD
    Commit-File $repo "f.txt" "2" "B"
    $B = Rev $repo HEAD
    git -C $repo checkout -b side $A *>$null          # other lineage: forks from A, never in main
    Commit-File $repo "s.txt" "x" "X"
    $X = Rev $repo HEAD
    git -C $repo checkout main *>$null
    git -C $repo update-ref refs/remotes/origin/main $B
    $r1 = @((Row "tools\check_b.ps1" "FAIL"))
    [void](Write-MainBaseline -Dir $bdir -Mode "fast" -Commit $A -Tree (Tree $repo $A) -Results $r1)
    [void](Write-MainBaseline -Dir $bdir -Mode "fast" -Commit $X -Tree (Tree $repo $X) -Results $r1)
    $sel = Select-MainBaseline -Dir $bdir -Mode "fast" -RepoRoot $repo
    Assert ($null -ne $sel.Baseline -and $sel.Baseline.commit -ceq $A) "ancestor baseline (older main sha) is used"
    Assert ($sel.Warning -match 'older main sha') "older sha is warned about"
    Assert ($sel.Ignored -eq 1) "the other-lineage baseline is ignored"
    [void](Write-MainBaseline -Dir $bdir -Mode "fast" -Commit $B -Tree (Tree $repo $B) -Results $r1)
    $sel = Select-MainBaseline -Dir $bdir -Mode "fast" -RepoRoot $repo
    Assert ($sel.Baseline.commit -ceq $B -and $sel.Exact -and -not $sel.Warning) "baseline for the merge-base wins, no warning"
    Remove-Item -LiteralPath $bdir -Recurse -Force
    [void](Write-MainBaseline -Dir $bdir -Mode "fast" -Commit $X -Tree (Tree $repo $X) -Results $r1)
    $sel = Select-MainBaseline -Dir $bdir -Mode "fast" -RepoRoot $repo
    Assert ($null -eq $sel.Baseline -and $sel.Reason -match 'ancestor') "only an other-lineage baseline -> none usable"
    $sel = Select-MainBaseline -Dir $bdir -Mode "full" -RepoRoot $repo
    Assert ($null -eq $sel.Baseline) "no baseline for the other mode"
    Remove-Item -LiteralPath $bdir -Recurse -Force
    $sel = Select-MainBaseline -Dir $bdir -Mode "fast" -RepoRoot $repo
    Assert ($null -eq $sel.Baseline) "missing directory -> none"

    # ---------------------------------------------------------------- 4
    Write-Host "case: recordable only at a clean HEAD == origin/main"
    Assert ((Test-MainBaselineRecordable -RepoRoot $repo).Ok) "clean HEAD == origin/main records"
    git -C $repo update-ref refs/remotes/origin/main $A
    Assert (-not (Test-MainBaselineRecordable -RepoRoot $repo).Ok) "HEAD ahead of origin/main does not record"
    git -C $repo update-ref refs/remotes/origin/main $B
    $C = (git -C $repo commit-tree "$B^{tree}" -p $B -m advance | Out-String).Trim()
    git -C $repo update-ref refs/remotes/origin/main $C
    Assert ((Test-MainBaselineRecordable -RepoRoot $repo).Ok) "HEAD behind an advanced origin/main still records"
    git -C $repo update-ref refs/remotes/origin/main $B
    Add-Content -LiteralPath (Join-Path $repo "f.txt") -Value "dirty"
    Assert (-not (Test-MainBaselineRecordable -RepoRoot $repo).Ok) "dirty tree does not record"
    git -C $repo checkout -- f.txt *>$null

    Write-Host "case: HEAD and status must be identical at run start and end (finding 3)"
    $s0 = Get-MainBaselineStartState -RepoRoot $repo
    Assert ((Test-MainBaselineRecordable -RepoRoot $repo -Start $s0).Ok) "unchanged HEAD/status since start records"
    $s1 = [pscustomobject]@{ Head = $A; Status = $s0.Status }
    Assert (-not (Test-MainBaselineRecordable -RepoRoot $repo -Start $s1).Ok) "HEAD moved since start does not record"
    $s2 = [pscustomobject]@{ Head = $s0.Head; Status = " M f.txt`n" }
    Assert (-not (Test-MainBaselineRecordable -RepoRoot $repo -Start $s2).Ok) "dirty-at-start (cleaned later) does not record"

    Write-Host "case: latest pointer only moves forward (finding 4)"
    $pd = Join-Path $tmp "ptr"
    [void](Write-MainBaseline -Dir $pd -Mode "fast" -Commit $B -Tree (Tree $repo $B) -Results $r1 -RepoRoot $repo)
    [void](Write-MainBaseline -Dir $pd -Mode "fast" -Commit $A -Tree (Tree $repo $A) -Results $r1 -RepoRoot $repo)
    $pj = [System.IO.File]::ReadAllText((Join-Path $pd "latest-fast.json")) | ConvertFrom-Json
    Assert ($pj.commit -ceq $B) "an older commit's baseline does not move the pointer back"
    Assert (Test-Path -LiteralPath (Join-Path $pd ((Tree $repo $A) + "-fast.json"))) "the older baseline file is still written"
    [void](Write-MainBaseline -Dir $pd -Mode "fast" -Commit $X -Tree (Tree $repo $X) -Results $r1 -RepoRoot $repo)
    $pj = [System.IO.File]::ReadAllText((Join-Path $pd "latest-fast.json")) | ConvertFrom-Json
    Assert ($pj.commit -ceq $B) "a non-descendant commit does not move the pointer"
    [void](Write-MainBaseline -Dir $pd -Mode "fast" -Commit $C -Tree (Tree $repo $B) -Results $r1 -RepoRoot $repo)
    $pj = [System.IO.File]::ReadAllText((Join-Path $pd "latest-fast.json")) | ConvertFrom-Json
    Assert ($pj.commit -ceq $C) "a descendant commit moves the pointer"

    # ---------------------------------------------------------------- 5
    Write-Host "case: run_all_checks -FailOnlyOnNew (scratch repo)"
    $sc = Join-Path $tmp "scratch"
    function ScTool([string]$n) { Join-Path (Join-Path $sc "tools") $n }
    git init -b main $sc *>$null
    New-Item -ItemType Directory -Path (Join-Path $sc "tools") | Out-Null
    Copy-Item (Join-Path $tools "run_all_checks.ps1"), (Join-Path $tools "lib_pctools_python.ps1"), (Join-Path $tools "checkcache_lib.ps1"), (Join-Path $tools "main_baseline_lib.ps1") (Join-Path $sc "tools")
    Set-Content -LiteralPath (ScTool "check_ok.ps1") -Value "exit 0" -Encoding ascii
    Set-Content -LiteralPath (ScTool "check_known.ps1") -Value "Write-Host 'boom'; exit 1" -Encoding ascii
    git -C $sc add -A *>$null; git -C $sc commit -m base *>$null
    git -C $sc update-ref refs/remotes/origin/main (Rev $sc HEAD)
    function Run-Checks([string[]]$more) {
        Push-Location $sc
        try {
            $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $sc "tools\run_all_checks.ps1") -Fast -AllowFewerChecks -NoCache -MaxParallel 2 @more 2>&1 | Out-String
            return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $o }
        } finally { Pop-Location }
    }
    $r = Run-Checks @()
    if ($r.Code -ne 1 -or $r.Out -notmatch 'Recorded main baseline') { Write-Host $r.Out }
    Assert ($r.Code -eq 1) "baseline-seeding run fails (check_known fails)"
    Assert ($r.Out -match 'Recorded main baseline \(fast\)') "clean HEAD == origin/main run records a baseline"
    $b = Read-MainBaselineFile -Path (Join-Path $bdir ((Tree $sc HEAD) + "-fast.json"))
    $st = @{}; foreach ($x in $b.results) { $st[[string]$x.check] = [string]$x.status }
    Assert ($st["tools\check_ok.ps1"] -ceq "PASS" -and $st["tools\check_known.ps1"] -ceq "FAIL") "recorded PASS and FAIL per check"
    $r = Run-Checks @("-Only", "check_known")
    Assert ($r.Out -match 'Main baseline not recorded: -Only') "a filtered run does not record"

    Set-Content -LiteralPath (ScTool "check_new.ps1") -Value "exit 1" -Encoding ascii
    git -C $sc add -A *>$null; git -C $sc commit -m addnew *>$null
    $r = Run-Checks @()
    Assert ($r.Code -eq 1 -and $r.Out -match '(?m)^\s+NEW\s+tools\\check_new\.ps1' -and $r.Out -match '(?m)^\s+KNOWN\s+tools\\check_known\.ps1') "NEW and KNOWN listed"
    Assert ($r.Out -notmatch 'Recorded main baseline') "HEAD ahead of origin/main does not record"
    $r = Run-Checks @("-FailOnlyOnNew")
    Assert ($r.Code -eq 1 -and $r.Out -match 'exit code stays 1') "-FailOnlyOnNew still fails on a NEW failure"

    git -C $sc rm -q tools/check_new.ps1 *>$null; git -C $sc commit -m rmnew *>$null
    $r = Run-Checks @()
    Assert ($r.Code -eq 1) "default exit code unchanged: a KNOWN failure still fails"
    $r = Run-Checks @("-FailOnlyOnNew")
    Assert ($r.Code -eq 0 -and $r.Out -match 'NOT a clean run') "-FailOnlyOnNew exits 0 loudly when every failure is KNOWN"
    Set-Content -LiteralPath (ScTool "check_known.ps1") -Value "exit 0" -Encoding ascii
    git -C $sc add -A *>$null; git -C $sc commit -m fix *>$null
    $r = Run-Checks @()
    Assert ($r.Code -eq 0 -and $r.Out -match '(?m)^\s+FIXED\s+tools\\check_known\.ps1') "FIXED listed, green run exits 0"
    # No usable baseline: nothing is KNOWN, so -FailOnlyOnNew cannot excuse anything.
    Remove-Item -LiteralPath $bdir -Recurse -Force
    Set-Content -LiteralPath (ScTool "check_known.ps1") -Value "exit 1" -Encoding ascii
    git -C $sc add -A *>$null; git -C $sc commit -m rebreak *>$null
    $r = Run-Checks @("-FailOnlyOnNew")
    Assert ($r.Code -eq 1 -and $r.Out -match 'No usable baseline') "-FailOnlyOnNew with no baseline keeps the failing exit code"

    # Inexact baseline (finding 1): baseline at an ancestor of HEAD that is not the
    # merge-base with origin/main must not excuse a failure.
    Remove-Item -LiteralPath $bdir -Recurse -Force -ErrorAction SilentlyContinue
    $prev = Rev $sc "HEAD~1"
    [void](Write-MainBaseline -Dir $bdir -Mode "fast" -Commit $prev -Tree (Tree $sc "HEAD~1") `
        -Results @((Row "tools\check_known.ps1" "FAIL"), (Row "tools\check_ok.ps1" "PASS")))
    $r = Run-Checks @("-FailOnlyOnNew")
    Assert ($r.Code -eq 1 -and $r.Out -match 'NOT at the merge-base') "-FailOnlyOnNew with a non-merge-base baseline keeps the failing exit code"

    # ---------------------------------------------------------------- 6
    Write-Host "case: land.ps1 -AllowKnownFailures (dry run)"
    $land = Join-Path $tools "land.ps1"
    $origin = Join-Path $tmp "origin.git"
    git init --bare -b main $origin *>$null
    $seed = Join-Path $tmp "lseed"
    git clone $origin $seed *>$null
    git -C $seed checkout -b main *>$null
    New-Item -ItemType Directory -Path (Join-Path $seed "tools") | Out-Null
    Copy-Item (Join-Path $tools "checkcache_lib.ps1"), (Join-Path $tools "main_baseline_lib.ps1") (Join-Path $seed "tools")
    Commit-File $seed "f.txt" "base" "seed"
    git -C $seed push origin HEAD:main *>$null
    $mainSha = Rev $seed HEAD
    $lc = Join-Path $tmp "lclone"
    git clone $origin $lc *>$null
    Commit-File $lc "mine.txt" "x" "mine"
    [void](Write-MainBaseline -Dir $bdir -Mode "fast" -Commit $mainSha -Tree (Tree $lc "origin/main") `
        -Results @([pscustomobject]@{ Path = "tools\check_known.ps1"; Status = "FAIL"; Signature = (Get-MainFailureSignature -Output "x" -ExitCode 1) }, (Row "tools\check_ok.ps1" "PASS")))
    $log = Join-Path $tmp "run.log"
    function Run-Land([string[]]$more) {
        Push-Location $lc
        try {
            $o = & powershell -NoProfile -ExecutionPolicy Bypass -File $land -AllowStandaloneClone -DryRun -CheckLog $log @more 2>&1 | Out-String
            $line = ($o -split "`r?`n" | Where-Object { $_.Trim().StartsWith("{") } | Select-Object -Last 1)
            $j = $null; if ($line) { try { $j = $line | ConvertFrom-Json } catch {} }
            return [pscustomobject]@{ Code = $LASTEXITCODE; Json = $j; Out = $o }
        } finally { Pop-Location }
    }
    @("Run mode: fast", "  FAIL  tools\check_known.ps1 (exit 1)", "1 passed, 0 skipped (0 due to -Fast), 1 failed.") | Set-Content -LiteralPath $log -Encoding Unicode
    $r = Run-Land @()
    Assert ($r.Code -eq 1) "KNOWN failure is refused without -AllowKnownFailures"
    $r = Run-Land @("-AllowKnownFailures")
    Assert ($r.Code -eq 0 -and ($r.Json.known_fails -contains 'tools\check_known.ps1') -and @($r.Json.new_fails).Count -eq 0) "KNOWN failure accepted and listed in known_fails"
    @("Run mode: fast", "  FAIL  tools\check_known.ps1 (exit 1)", "  FAIL  tools\check_new.ps1 (exit 1)", "0 passed, 0 skipped (0 due to -Fast), 2 failed.") | Set-Content -LiteralPath $log -Encoding Unicode
    $r = Run-Land @("-AllowKnownFailures")
    Assert ($r.Code -eq 1 -and ($r.Json.new_fails -contains 'tools\check_new.ps1')) "a NEW failure still refuses and is listed in new_fails"
    # CHANGED signature (finding 2): same check fails on main but for a different reason.
    $sigMain = Get-MainFailureSignature -Output "FAIL: original reason" -ExitCode 1
    [void](Write-MainBaseline -Dir $bdir -Mode "fast" -Commit $mainSha -Tree (Tree $lc "origin/main") `
        -Results @([pscustomobject]@{ Path = "tools\check_known.ps1"; Status = "FAIL"; Signature = $sigMain }))
    @("Run mode: fast", "  FAIL  tools\check_known.ps1 (exit 1)", "", "--- tools\check_known.ps1 (exit 1) ---", "FAIL: original reason", "",
      "0 passed, 0 skipped (0 due to -Fast), 1 failed.") | Set-Content -LiteralPath $log -Encoding Unicode
    $r = Run-Land @("-AllowKnownFailures")
    Assert ($r.Code -eq 0 -and ($r.Json.known_fails -contains 'tools\check_known.ps1')) "same failure signature as main stays KNOWN in land"
    @("Run mode: fast", "  FAIL  tools\check_known.ps1 (exit 1)", "", "--- tools\check_known.ps1 (exit 1) ---", "FAIL: a brand new reason", "",
      "0 passed, 0 skipped (0 due to -Fast), 1 failed.") | Set-Content -LiteralPath $log -Encoding Unicode
    $r = Run-Land @("-AllowKnownFailures")
    Assert ($r.Code -eq 1) "a different failure signature is not excused by land"
    # Non-exact baseline (finding 1): ancestor of HEAD, not the merge-base.
    Remove-Item -LiteralPath $bdir -Recurse -Force
    [void](Write-MainBaseline -Dir $bdir -Mode "fast" -Commit (Rev $lc HEAD) -Tree (Tree $lc HEAD) -Results @((Row "tools\check_known.ps1" "FAIL")))
    @("Run mode: fast", "  FAIL  tools\check_known.ps1 (exit 1)", "0 passed, 0 skipped (0 due to -Fast), 1 failed.") | Set-Content -LiteralPath $log -Encoding Unicode
    $r = Run-Land @("-AllowKnownFailures")
    Assert ($r.Code -eq 1) "a non-merge-base baseline excuses nothing in land"
    # Other lineage: a baseline whose commit is not an ancestor of HEAD is ignored.
    Remove-Item -LiteralPath $bdir -Recurse -Force
    $other = Join-Path $tmp "other"
    git init -b main $other *>$null
    Commit-File $other "z.txt" "z" "unrelated"
    [void](Write-MainBaseline -Dir $bdir -Mode "fast" -Commit (Rev $other HEAD) -Tree (Tree $other HEAD) -Results @((Row "tools\check_known.ps1" "FAIL")))
    @("Run mode: fast", "  FAIL  tools\check_known.ps1 (exit 1)", "0 passed, 0 skipped (0 due to -Fast), 1 failed.") | Set-Content -LiteralPath $log -Encoding Unicode
    $r = Run-Land @("-AllowKnownFailures")
    Assert ($r.Code -eq 1) "a baseline from another lineage excuses nothing"

    # Review finding 2: land must fetch BEFORE the gate, and re-run the known-failing check
    # after the rebase. Scenario: HEAD is based on M1 (origin advanced) but the local
    # refs/remotes/origin/main is stale at M0; the exact baseline is at M1.
    Write-Host "case: land fetches before the -AllowKnownFailures gate; known-failing checks re-run after the rebase"
    Remove-Item -LiteralPath $bdir -Recurse -Force -ErrorAction SilentlyContinue
    $m0 = Rev $lc "origin/main"
    Commit-File $seed "m1.txt" "m1" "m1"
    git -C $seed push origin HEAD:main *>$null
    $m1 = Rev $seed HEAD
    git -C $lc fetch origin *>$null
    git -C $lc rebase origin/main *>$null
    git -C $lc update-ref refs/remotes/origin/main $m0
    Assert ((Rev $lc "origin/main") -ceq $m0) "scenario: local origin/main is stale"
    [void](Write-MainBaseline -Dir $bdir -Mode "fast" -Commit $m1 -Tree (Tree $lc $m1) `
        -Results @([pscustomobject]@{ Path = "tools\check_known.ps1"; Status = "FAIL"; Signature = (Get-MainFailureSignature -Output "FAIL: r" -ExitCode 1) }))
    @("Run mode: fast", "  FAIL  tools\check_known.ps1 (exit 1)", "", "--- tools\check_known.ps1 (exit 1) ---", "FAIL: r", "",
      "0 passed, 0 skipped (0 due to -Fast), 1 failed.") | Set-Content -LiteralPath $log -Encoding Unicode
    $r = Run-Land @("-AllowKnownFailures")
    Assert ($r.Code -eq 0 -and ($r.Json.known_fails -contains 'tools\check_known.ps1')) "known failure matched against the FETCHED origin/main (stale ref refreshed first)"
    $argsFile = Join-Path $tmp "stub_args.txt"
    $stub = Join-Path $tmp "stub_checks.ps1"
    Set-Content -LiteralPath $stub -Encoding ascii -Value @("param([string]`$Only,[switch]`$AllowFewerChecks,[switch]`$FailOnlyOnNew)", "Add-Content -LiteralPath '$argsFile' -Value ('ONLY=' + `$Only + ' FOON=' + `$FailOnlyOnNew)", "exit 0")
    Push-Location $lc
    try { $lo = & powershell -NoProfile -ExecutionPolicy Bypass -File $land -AllowStandaloneClone -CheckLog $log -AllowKnownFailures -ChecksScript $stub 2>&1 | Out-String; $lcode = $LASTEXITCODE } finally { Pop-Location }
    $sa = if (Test-Path $argsFile) { Get-Content -Raw $argsFile } else { "" }
    Assert ($lcode -eq 0) "land with a known failure lands (exit $lcode)"
    Assert ($sa -match 'check_known' -and $sa -match 'FOON=True') "post-rebase run re-runs the known-failing check under -FailOnlyOnNew (stub saw: $($sa.Trim()))"
} finally {
    $env:KILNCTL_MAINBASELINE_DIR = $savedDir
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:fails -gt 0) { Write-Host "check_main_baseline: $($script:fails) FAILED" -ForegroundColor Red; exit 1 }
Write-Host "check_main_baseline: all cases passed" -ForegroundColor Green
exit 0
