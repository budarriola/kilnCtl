# check_negtest.ps1 -- unit test for tools\negtest.ps1 against a tiny scratch
# git repo in the temp dir (never this repo, never C:\wt, never a real build).
#
# The fixture "test" is test.ps1, which checks calc.ps1 and fails loudly with
# "FAIL: <name>" lines. It also refuses to run if its -OutDir is not a fresh
# empty directory (the prebuilt-binary class, ed854ac5/ba230bca), and appends
# to $env:NEGTEST_FIXTURE_LOG each time it runs so "the command never ran" is
# observable.
#
# Covered: a CAUGHT mutation (with the fresh-out-dir requirement); a MISSED one;
# -ExpectPattern naming a different failure -> MISSED with a note; a find
# string with 0 and with 2 matches -> exit 2, no copy made, command never run;
# baseline failing -> exit 2 and no CAUGHT verdict; the real-tree guard
# (command edits a mutated file in the real tree; command creates an untracked
# file there) -> exit 2; a timeout -> TIMEOUT, process killed, exit 2;
# -IncludeDirty carrying a tracked diff and an untracked file into the copy
# (and a mutation of an untracked file refused without it); -Diff; a
# -Mutations file sequentially and with -Parallel 2; usage errors; the stale
# sweep removing a copy whose owner is dead and keeping one whose owner lives.
# After EVERY run: no copy left under the copy root, the scratch repo has one
# worktree, and its status is what the case expects.
#
# -ScriptUnderTest points the check at a mutated COPY of negtest.ps1 so each
# assertion can be negative-tested without touching the real script. The copy
# must live in tools\ (it dot-sources lib_safe_remove.ps1 next to itself).
# checkcache: ok
param([string]$ScriptUnderTest, [string]$Group = '')
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $ScriptUnderTest) { $ScriptUnderTest = Join-Path $here 'negtest.ps1' }
. (Join-Path $here 'lib_safe_remove.ps1')

if (-not $Group) {
    # Parent: run each independent group in its own child (own scratch repo) concurrently.
    # Every assertion still runs; this only overlaps the process/git-spawn latency that dominates.
    $groups = 'A', 'A2', 'B', 'C', 'D', 'E', 'F', 'G', 'H1', 'H2', 'I'
    # Per-group minimum assertion counts (recorded from a green run); a group that
    # silently skips its body would report far fewer.
    $minAssert = @{ 'A' = 14; 'A2' = 15; 'B' = 26; 'C' = 10; 'D' = 13; 'E' = 8; 'F' = 18; 'G' = 7; 'H1' = 9; 'H2' = 9; 'I' = 5 }
    # 4a: $groups and $minAssert must name exactly the same groups.
    $missingMin = @($groups | Where-Object { -not $minAssert.ContainsKey($_) })
    $extraMin = @($minAssert.Keys | Where-Object { $groups -notcontains $_ })
    if ($missingMin.Count -gt 0 -or $extraMin.Count -gt 0) {
        Write-Host "NEGTEST CHECK FAILED: group/minAssert mismatch. groups without minAssert: [$($missingMin -join ',')]; minAssert without group: [$($extraMin -join ',')]" -ForegroundColor Red
        exit 1
    }
    $tmpd = Join-Path ([System.IO.Path]::GetTempPath()) ("negchk_par_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Path $tmpd -Force | Out-Null
    $procs = @{}
    $scratchOf = @{}
    foreach ($g in $groups) {
        $scratchOf[$g] = Join-Path ([System.IO.Path]::GetTempPath()) ("negtest_chk_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
        $env:NEGCHK_SCRATCH_DIR = $scratchOf[$g]
        $procs[$g] = Start-Process -FilePath powershell.exe -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $tmpd "$g.out") -RedirectStandardError (Join-Path $tmpd "$g.err") `
            -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $MyInvocation.MyCommand.Path, '-ScriptUnderTest', $ScriptUnderTest, '-Group', $g)
        $null = $procs[$g].Handle
    }
    Remove-Item Env:\NEGCHK_SCRATCH_DIR -ErrorAction SilentlyContinue
    $groupTimeoutMs = 15 * 60 * 1000
    $totalA = 0; $bad = New-Object System.Collections.Generic.List[string]
    foreach ($g in $groups) {
        $null = $procs[$g].Handle
        if (-not $procs[$g].WaitForExit($groupTimeoutMs)) {
            & taskkill.exe /T /F /PID $procs[$g].Id 2>&1 | Out-Null
            $bad.Add("group ${g}: timed out after 15 min and was killed")
            # 4e: clean up the killed child's scratch repo and its worktree copies.
            Start-Sleep -Seconds 2
            try {
                $r = Join-Path $scratchOf[$g] 'repo'
                if (Test-Path -LiteralPath $r) {
                    Get-ChildItem -LiteralPath (Join-Path $scratchOf[$g] 'copies') -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'negtest_*' -and $_.PSIsContainer } | ForEach-Object {
                        try { Remove-ReparsePointsUnder -Path $_.FullName | Out-Null } catch { }
                        & git -C $r worktree remove --force --force $_.FullName 2>&1 | Out-Null
                    }
                }
                Remove-TreeSafe -Path $scratchOf[$g]
            } catch { $bad.Add("group ${g}: scratch cleanup after timeout failed: $($_.Exception.Message)") }
            continue
        }
        $txt = (Get-Content -LiteralPath (Join-Path $tmpd "$g.out") -Raw -ErrorAction SilentlyContinue)
        $m = [regex]::Match("$txt", 'GROUP_RESULT (\d+) (\d+)')
        if (-not $m.Success) { $bad.Add("group ${g}: no result or crashed (exit $($procs[$g].ExitCode)): $txt $(Get-Content -LiteralPath (Join-Path $tmpd "$g.err") -Raw -ErrorAction SilentlyContinue)") }
        else {
            $n = [int]$m.Groups[1].Value; $f = [int]$m.Groups[2].Value
            $totalA += $n; Write-Host "group ${g}: $n assertions"
            if ($f -ne 0) { $bad.Add("group ${g}: GROUP_RESULT reports $f failure(s)") }
            if ($procs[$g].ExitCode -ne 0) { $bad.Add("group ${g}: child exit code $($procs[$g].ExitCode)") }
            if ($n -lt $minAssert[$g]) { $bad.Add("group ${g}: only $n assertions ran, minimum $($minAssert[$g])") }
        }
        foreach ($l in ("$txt" -split "`n")) { if ($l -match '^  \S' ) { $bad.Add("group ${g}:" + $l.TrimEnd()) } }
    }
    Remove-Item -LiteralPath $tmpd -Recurse -Force -ErrorAction SilentlyContinue
    if ($bad.Count -gt 0) {
        Write-Host "NEGTEST CHECK FAILED ($($bad.Count) problem(s)):" -ForegroundColor Red
        foreach ($m in $bad) { Write-Host "  $m" -ForegroundColor Red }
        exit 1
    }
    Write-Host "negtest check passed: $totalA assertions across $($groups.Count) parallel groups (caught, missed, bad find, baseline, real-tree guard, timeout, dirty, diff, mutations file, parallel, stale sweep)."
    exit 0
}

$scratch = if ($env:NEGCHK_SCRATCH_DIR) { $env:NEGCHK_SCRATCH_DIR } else { Join-Path ([System.IO.Path]::GetTempPath()) ("negtest_chk_" + [guid]::NewGuid().ToString("N").Substring(0, 8)) }
$repo = Join-Path $scratch "repo"
$copies = Join-Path $scratch "copies"
$logs = Join-Path $scratch "logs"
$fixtureLog = Join-Path $scratch "ran.log"
New-Item -ItemType Directory -Path $repo, $copies, $logs -Force | Out-Null
$env:GIT_OPTIONAL_LOCKS = '0'
$failures = New-Object System.Collections.Generic.List[string]
$assertions = 0
$sw0 = [Diagnostics.Stopwatch]::StartNew()
function Step([string]$m) { Write-Host ("[{0,5:N1}s] {1}" -f $script:sw0.Elapsed.TotalSeconds, $m) }
function Assert-True($cond, [string]$msg) {
    $script:assertions++
    if (-not $cond) { $script:failures.Add($msg) }
}
function G { & git -c user.name=t -c user.email=t@example.invalid -c core.autocrlf=false @args 2>&1 | Out-Null }
function Status { return ((& git -C $script:repo status --porcelain 2>$null) -join "`n") }

$testCmd = "powershell -NoProfile -ExecutionPolicy Bypass -File test.ps1 -OutDir '{OUT}'"

function Run-Neg([string]$case, [string[]]$extra, $expectStatus = $null) {
    # Runs the script under test; returns @{ Exit; Json; Text }. Asserts the
    # universal post-conditions (no copy left, one worktree, status as expected).
    if (Test-Path -LiteralPath $script:fixtureLog) { Remove-Item -LiteralPath $script:fixtureLog -Force }
    $statusBefore = Status
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $script:ScriptUnderTest,
        '-RepoRoot', $script:repo, '-CopyRoot', $script:copies, '-LogDir', (Join-Path $script:logs $case)) + $extra
    $env:NEGTEST_FIXTURE_LOG = $script:fixtureLog
    $out = & powershell.exe @argv 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    Remove-Item Env:\NEGTEST_FIXTURE_LOG -ErrorAction SilentlyContinue
    $text = ($out -join "`n")
    $last = @($out | Where-Object { $_ -match '^\{' }) | Select-Object -Last 1
    $json = $null
    try { if ($last -and ($out[-1] -eq $last)) { $json = $last | ConvertFrom-Json } } catch { }
    Assert-True ($null -ne $json) "${case}: last stdout line is not JSON (exit $code): $($out | Select-Object -Last 3)"
    $left = @(Get-ChildItem -LiteralPath $script:copies -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'negtest_*' })
    Assert-True ($left.Count -eq 0) "${case}: copy left behind under the copy root: $($left.Name -join ', ')"
    $wl = @(& git -C $script:repo worktree list 2>$null)
    Assert-True ($wl.Count -eq 1) "${case}: scratch repo still has $($wl.Count) worktrees registered"
    $want = if ($null -ne $expectStatus) { $expectStatus } else { $statusBefore }
    $statusAfter = Status
    Assert-True ($statusAfter -eq $want) "${case}: scratch repo status changed: before '$statusBefore' after '$statusAfter'"
    return @{ Exit = $code; Json = $json; Text = $text }
}
function Ran { return (Test-Path -LiteralPath $script:fixtureLog) }

try {
    # ---------------------------------------------------------------- fixture
    Set-Content -LiteralPath (Join-Path $repo 'calc.ps1') -Encoding ASCII -Value @'
function Add-Two($a, $b) { return $a + $b }
function Test-Big($x) { return $x -gt 10 }
'@
    Set-Content -LiteralPath (Join-Path $repo 'test.ps1') -Encoding ASCII -Value @'
param([string]$OutDir, [switch]$ForceFail, [int]$SleepSec = 0)
if ($env:NEGTEST_FIXTURE_LOG) { Add-Content -LiteralPath $env:NEGTEST_FIXTURE_LOG -Value "ran" }
if (-not $OutDir -or -not (Test-Path -LiteralPath $OutDir)) { Write-Output "STALE: no out dir"; exit 3 }
if (@(Get-ChildItem -LiteralPath $OutDir -Force).Count -gt 0) { Write-Output "STALE: out dir not fresh"; exit 3 }
Set-Content -LiteralPath (Join-Path $OutDir "built.txt") -Value "artifact"
if ($SleepSec) { Start-Sleep -Seconds $SleepSec }
. "$PSScriptRoot\calc.ps1"
if (Test-Path -LiteralPath "$PSScriptRoot\extra.ps1") { . "$PSScriptRoot\extra.ps1" }
$fail = 0
if ($ForceFail) { Write-Output "FAIL: forced"; $fail++ }
if ((Add-Two 2 3) -ne 5) { Write-Output "FAIL: Add-Two"; $fail++ }
if (-not (Test-Big 11)) { Write-Output "FAIL: Test-Big 11"; $fail++ }
if (Test-Big 5) { Write-Output "FAIL: Test-Big 5"; $fail++ }
Write-Output "failures: $fail"
if ($fail) { exit 1 }
exit 0
'@
    Set-Content -LiteralPath (Join-Path $repo ".gitignore") -Encoding ASCII -Value "*.log"
    G init -q -b main $repo
    # Perf: this scratch repo is tiny and short-lived; no daemons, no gc, no optional index writes.
    foreach ($kv in @(@('core.fsmonitor', 'false'), @('core.untrackedCache', 'false'), @('gc.auto', '0'), @('maintenance.auto', 'false'), @('core.preloadIndex', 'false'))) { & git -C $repo config $kv[0] $kv[1] 2>&1 | Out-Null }
    G -C $repo add -A
    G -C $repo commit -q -m base
    Assert-True ((& git -C $repo rev-parse HEAD 2>$null) -match '^[0-9a-f]{40}$') "fixture: scratch repo has no commit"
    Push-Location $repo
    $pre = & powershell -NoProfile -ExecutionPolicy Bypass -File test.ps1 -OutDir (New-Item -ItemType Directory -Path (Join-Path $scratch "o0")).FullName
    Pop-Location
    Assert-True ($LASTEXITCODE -eq 0) "fixture: the unmutated fixture test does not pass: $pre"
    Step "fixture ready"

    $calc = Join-Path $repo 'calc.ps1'
    $mutA = @('-File', 'calc.ps1', '-Find', '$a + $b', '-Replace', '$a - $b')
    $mutB = @('-File', 'calc.ps1', '-Find', '-gt 10', '-Replace', '-ge 10')

    if ($Group -eq 'A') {
    # ---------------------------------------------------------------- caught
    $r = Run-Neg "caught" (@('-Command', $testCmd) + $mutA + @('-ExpectPattern', 'FAIL: Add-Two'))
    Assert-True ($r.Exit -eq 0) "caught: exit $($r.Exit), expected 0`n$($r.Text)"
    Assert-True ($r.Json.verdict -eq 'ALL_CAUGHT') "caught: verdict $($r.Json.verdict)"
    Assert-True (@($r.Json.mutations).Count -eq 1 -and $r.Json.mutations[0].verdict -eq 'CAUGHT') "caught: mutation verdict not CAUGHT"
    Assert-True ((@($r.Json.mutations[0].matched) -join '|') -match 'FAIL: Add-Two') "caught: matched lines do not show the failure"
    Assert-True ($r.Json.baseline.passed -eq $true) "caught: baseline not reported as passed"
    Assert-True ($r.Json.real_tree_unchanged -eq $true -and $r.Json.copies_removed -eq $true) "caught: guard/cleanup flags not true"
    Assert-True ($r.Text -match '(?m)^CAUGHT\s') "caught: no human CAUGHT line"
    Assert-True (@(Get-Content -LiteralPath $fixtureLog).Count -eq 2) "caught: fixture should have run twice (baseline + mutation)"
    Step "caught"
    }
    if ($Group -eq 'A2') {
    # ---------------------------------------------------------------- missed
    $r = Run-Neg "missed" (@('-Command', $testCmd) + $mutB)
    Assert-True ($r.Exit -eq 1) "missed: exit $($r.Exit), expected 1`n$($r.Text)"
    Assert-True ($r.Json.verdict -eq 'MISSED' -and $r.Json.mutations[0].verdict -eq 'MISSED') "missed: verdict $($r.Json.verdict)/$($r.Json.mutations[0].verdict)"
    Assert-True ($r.Text -match '(?m)^MISSED\s') "missed: no human MISSED line"

    # wrong pattern: fails, but not the way the caller said it must
    $r = Run-Neg "otherfail" (@('-Command', $testCmd) + $mutA + @('-ExpectPattern', 'FAIL: Test-Big'))
    Assert-True ($r.Exit -eq 1 -and $r.Json.mutations[0].verdict -eq 'MISSED') "otherfail: a failure without the expect pattern must be MISSED (exit $($r.Exit))"
    Assert-True ("$($r.Json.mutations[0].note)" -match 'another reason') "otherfail: note should say it failed for another reason"
    Step "missed"
    }
    if ($Group -eq 'B') {
    # ---------------------------------------------------------------- bad find strings
    $r = Run-Neg "nomatch" (@('-Command', $testCmd, '-File', 'calc.ps1', '-Find', 'no such text', '-Replace', 'x'))
    Assert-True ($r.Exit -eq 2 -and $r.Json.verdict -eq 'ERROR') "nomatch: exit $($r.Exit), expected 2"
    Assert-True ("$($r.Json.error)" -match 'matches 0 time') "nomatch: error does not say 0 matches: $($r.Json.error)"
    Assert-True (-not (Ran)) "nomatch: the command ran although the mutation was invalid"
    $r = Run-Neg "twomatch" (@('-Command', $testCmd, '-File', 'calc.ps1', '-Find', 'return', '-Replace', 'return 0;'))
    Assert-True ($r.Exit -eq 2) "twomatch: exit $($r.Exit), expected 2"
    Assert-True ("$($r.Json.error)" -match 'matches 2 time') "twomatch: error does not say 2 matches: $($r.Json.error)"
    Assert-True (-not (Ran)) "twomatch: the command ran although the mutation was invalid"
    $r = Run-Neg "noop" (@('-Command', $testCmd, '-File', 'calc.ps1', '-Find', '-gt 10', '-Replace', '-gt 10'))
    Assert-True ($r.Exit -eq 2 -and -not (Ran)) "noop: identical find/replace must be refused before running (exit $($r.Exit))"
    $r = Run-Neg "usage" (@('-File', 'calc.ps1', '-Find', '-gt 10', '-Replace', '-ge 10'))
    Assert-True ($r.Exit -eq 2 -and -not (Ran)) "usage: no -Command/-Preset must be exit 2 (got $($r.Exit))"
    Step "bad find strings"
    }
    if ($Group -eq 'C') {
    # ---------------------------------------------------------------- baseline must pass
    $r = Run-Neg "baselinefail" (@('-Command', ($testCmd + ' -ForceFail')) + $mutA)
    Assert-True ($r.Exit -eq 2 -and $r.Json.verdict -eq 'ERROR') "baselinefail: exit $($r.Exit) verdict $($r.Json.verdict), expected 2/ERROR"
    Assert-True ("$($r.Json.error)" -match 'BASELINE FAILED') "baselinefail: error does not name the baseline: $($r.Json.error)"
    Assert-True (@($r.Json.mutations | Where-Object { $_.verdict -eq 'CAUGHT' }).Count -eq 0) "baselinefail: a CAUGHT verdict was reported on a failing baseline"
    Assert-True ($r.Json.baseline.passed -eq $false) "baselinefail: baseline.passed should be false"
    Step "baseline"
    }
    if ($Group -eq 'D') {
    # ---------------------------------------------------------------- real-tree guard
    $r = Run-Neg "guardhash" (@('-Command', ($testCmd + "; Add-Content -LiteralPath '$calc' -Value '# touched'")) + $mutA) " M calc.ps1"
    Assert-True ($r.Exit -eq 2 -and $r.Json.real_tree_unchanged -eq $false) "guardhash: editing the real tree's mutated file must be exit 2 + real_tree_unchanged=false (exit $($r.Exit))"
    Assert-True ($r.Text -match 'REAL TREE CHANGED') "guardhash: no REAL TREE CHANGED message"
    G -C $repo checkout -- calc.ps1
    $stray = Join-Path $repo "stray.txt"
    $r = Run-Neg "guardstatus" (@('-Command', ($testCmd + "; Set-Content -LiteralPath '$stray' -Value x")) + $mutA) "?? stray.txt"
    Assert-True ($r.Exit -eq 2 -and $r.Json.real_tree_unchanged -eq $false) "guardstatus: an untracked file appearing in the real tree must be exit 2 (exit $($r.Exit))"
    Remove-Item -LiteralPath $stray -Force -ErrorAction SilentlyContinue
    Step "guard"
    }
    if ($Group -eq 'E') {
    # ---------------------------------------------------------------- timeout + cleanup after failure
    $t0 = [Diagnostics.Stopwatch]::StartNew()
    $r = Run-Neg "timeout" (@('-Command', ($testCmd + ' -SleepSec 60'), '-TimeoutMin', '0.05', '-NoBaseline') + $mutA)
    Assert-True ($r.Exit -eq 2 -and $r.Json.mutations[0].verdict -eq 'TIMEOUT') "timeout: expected exit 2 with TIMEOUT (exit $($r.Exit), verdict $($r.Json.mutations[0].verdict), error $($r.Json.error))`n$($r.Text)"
    $mutSec = [double]$r.Json.mutations[0].seconds
    Assert-True ($mutSec -gt 0 -and $mutSec -lt 30) "timeout: the timed-out mutation run took ${mutSec}s (whole run $([int]$t0.Elapsed.TotalSeconds)s); the 60s child was not killed at the 3s limit"
    Step "timeout"
    }
    if ($Group -eq 'F') {
    # ---------------------------------------------------------------- -IncludeDirty
    $tp = Join-Path $repo 'test.ps1'
    Add-Content -LiteralPath $tp -Encoding ASCII -Value 'if (Test-Big 10) { Write-Output "FAIL: Test-Big 10"; exit 1 }'
    # the line above lands after `exit 0`; move it before the final verdict instead
    $lines = @(Get-Content -LiteralPath $tp)
    $boundary = $lines[-1]; $lines = $lines[0..($lines.Count - 2)]
    $idx = [Array]::IndexOf($lines, 'Write-Output "failures: $fail"')
    $lines = $lines[0..($idx - 1)] + @($boundary, 'if (Get-Command Get-Half -ErrorAction SilentlyContinue) { if ((Get-Half 8) -ne 4) { Write-Output "FAIL: Get-Half"; exit 1 } }') + $lines[$idx..($lines.Count - 1)]
    Set-Content -LiteralPath $tp -Encoding ASCII -Value $lines
    Set-Content -LiteralPath (Join-Path $repo 'extra.ps1') -Encoding ASCII -Value 'function Get-Half($x) { return $x / 2 }'
    $dirty = Status
    $r = Run-Neg "dirty_tracked" (@('-Command', $testCmd, '-IncludeDirty') + $mutB + @('-ExpectPattern', 'FAIL: Test-Big 10'))
    Assert-True ($r.Exit -eq 0 -and $r.Json.mutations[0].verdict -eq 'CAUGHT') "dirty_tracked: the uncommitted boundary test should catch -ge (exit $($r.Exit))`n$($r.Text)"
    $r = Run-Neg "dirty_untracked" (@('-Command', $testCmd, '-IncludeDirty', '-File', 'extra.ps1', '-Find', '$x / 2', '-Replace', '$x / 4'))
    Assert-True ($r.Exit -eq 0 -and $r.Json.mutations[0].verdict -eq 'CAUGHT') "dirty_untracked: a mutation of an untracked file should be CAUGHT with -IncludeDirty (exit $($r.Exit))`n$($r.Text)"
    $r = Run-Neg "dirty_absent" (@('-Command', $testCmd, '-File', 'extra.ps1', '-Find', '$x / 2', '-Replace', '$x / 4'))
    Assert-True ($r.Exit -eq 2 -and -not (Ran)) "dirty_absent: an untracked file is not at HEAD without -IncludeDirty; must be exit 2 (exit $($r.Exit))"
    Assert-True ((Status) -eq $dirty) "dirty: the caller's uncommitted changes were disturbed"
    G -C $repo checkout -- test.ps1
    Remove-Item -LiteralPath (Join-Path $repo 'extra.ps1') -Force
    Step "include dirty"
    }
    if ($Group -eq 'G') {
    # ---------------------------------------------------------------- -Diff
    (Get-Content -LiteralPath $calc) -replace '\$a \+ \$b', '$a * $b' | Set-Content -LiteralPath $calc -Encoding ASCII
    $diffFile = Join-Path $scratch "mul.diff"
    & cmd.exe /d /c "git -C `"$repo`" diff > `"$diffFile`""
    G -C $repo checkout -- calc.ps1
    $r = Run-Neg "diff" @('-Command', $testCmd, '-Diff', $diffFile)
    Assert-True ($r.Exit -eq 0 -and $r.Json.mutations[0].verdict -eq 'CAUGHT') "diff: a -Diff mutation should be CAUGHT (exit $($r.Exit))`n$($r.Text)"
    Step "diff"
    }
    if ($Group -in 'H1', 'H2') {
    # ---------------------------------------------------------------- -Mutations, sequential and parallel
    $mf = Join-Path $scratch "muts.json"
    Set-Content -LiteralPath $mf -Encoding UTF8 -Value (@(
            @{ name = "minus"; file = "calc.ps1"; find = '$a + $b'; replace = '$a - $b'; expect = 'FAIL: Add-Two' },
            @{ name = "boundary"; file = "calc.ps1"; find = '-gt 10'; replace = '-ge 10' },
            @{ name = "low"; edits = @(@{ file = "calc.ps1"; find = '-gt 10'; replace = '-gt 4' }) }
        ) | ConvertTo-Json -Depth 5)
    foreach ($par in $(if ($Group -eq 'H1') { 1 } else { 2 })) {
        $r = Run-Neg "muts_p$par" @('-Command', $testCmd, '-Mutations', $mf, '-Parallel', "$par")
        $v = @($r.Json.mutations | ForEach-Object { "$($_.name)=$($_.verdict)" }) -join ','
        Assert-True ($r.Exit -eq 1) "muts_p${par}: exit $($r.Exit), expected 1 (one MISSED)`n$($r.Text)"
        Assert-True ($v -eq 'minus=CAUGHT,boundary=MISSED,low=CAUGHT') "muts_p${par}: verdicts '$v'"
        Assert-True ($r.Json.baseline.passed -eq $true) "muts_p${par}: baseline missing or not passed"
    }
    Step "mutations file"
    }
    if ($Group -eq 'I') {
    # ---------------------------------------------------------------- stale sweep
    $dead = Start-Process -FilePath powershell.exe -ArgumentList '-NoProfile', '-Command', 'exit 0' -PassThru -WindowStyle Hidden
    $deadTicks = $dead.StartTime.ToUniversalTime().Ticks
    $dead.WaitForExit()
    $deadCopy = Join-Path $copies "negtest_dead01"
    $liveCopy = Join-Path $copies "negtest_live01"
    G -C $repo worktree add --detach $deadCopy HEAD
    G -C $repo worktree add --detach $liveCopy HEAD
    Set-Content -LiteralPath "$deadCopy.owner.json" -Value (@{ pid = $dead.Id; start_ticks = $deadTicks; repo = $repo; copy = $deadCopy } | ConvertTo-Json -Compress)
    $me = (Get-Process -Id $PID).StartTime.ToUniversalTime().Ticks
    Set-Content -LiteralPath "$liveCopy.owner.json" -Value (@{ pid = $PID; start_ticks = $me; repo = $repo; copy = $liveCopy } | ConvertTo-Json -Compress)
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $ScriptUnderTest, '-RepoRoot', $repo, '-CopyRoot', $copies,
        '-LogDir', (Join-Path $logs 'sweep'), '-Command', $testCmd) + $mutA
    $out = & powershell.exe @argv 2>&1 | ForEach-Object { "$_" }
    Assert-True ($LASTEXITCODE -eq 0) "sweep: run failed (exit $LASTEXITCODE): $($out | Select-Object -Last 3)"
    Assert-True (-not (Test-Path -LiteralPath $deadCopy) -and -not (Test-Path -LiteralPath "$deadCopy.owner.json")) "sweep: copy owned by a dead process was not swept"
    Assert-True ((Test-Path -LiteralPath $liveCopy) -and (Test-Path -LiteralPath "$liveCopy.owner.json")) "sweep: copy owned by a LIVE process was removed"
    G -C $repo worktree remove --force $liveCopy
    Remove-Item -LiteralPath "$liveCopy.owner.json" -Force -ErrorAction SilentlyContinue
    Step "stale sweep"
    }
}
finally {
    try { & git -C $repo worktree prune 2>&1 | Out-Null } catch { }
    try { Start-Sleep -Milliseconds 300; Remove-TreeSafe -Path $scratch } catch { }
}

Write-Host "GROUP_RESULT $assertions $($failures.Count)"
if ($failures.Count -gt 0) {
    Write-Host "NEGTEST CHECK FAILED ($($failures.Count) of $assertions assertion(s)):" -ForegroundColor Red
    foreach ($m in $failures) { Write-Host "  $m" -ForegroundColor Red }
    exit 1
}
Write-Host "negtest check passed: $assertions assertions (caught, missed, bad find, baseline, real-tree guard, timeout, dirty, diff, mutations file, parallel, stale sweep)."
exit 0
