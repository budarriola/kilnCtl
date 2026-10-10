# Guards tools\wt_status.ps1 (worktree report + safe prune for C:\wt).
#
# Everything runs against a scratch root in the temp dir with its own scratch
# git repo (bare "origin" + clone); the real C:\wt is never read or touched.
# Covered: every classification (ACTIVE by process, ACTIVE by recent file,
# HAS_WORK ahead / dirty tracked / untracked, STALE_CLEAN including gitignored
# and build\ noise, ORPHAN_DIR, UNKNOWN for locked / clean-but-recent /
# unregistered-with-.git / .kicad_*), process-path boundary matching
# (foo vs foo_longer), helpers (.buildgate-style dot dirs, *_logs, loose
# files) listed and never pruned, -WhatIf removing nothing, -Prune refusing
# HAS_WORK / ACTIVE / UNKNOWN, and junction safety: a junction inside an orphan
# dir and one inside a stale registered worktree must be unlinked, with the
# junction targets (sentinel files outside the root) surviving.
#
# -ScriptUnderTest points the check at a mutated COPY of wt_status.ps1 so each
# assertion can be negative-tested without touching the real script. The copy
# must live in tools\ (it dot-sources lib_safe_remove.ps1 and calls
# worktree_mint.ps1 next to itself).
param([string]$ScriptUnderTest)
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $ScriptUnderTest) { $ScriptUnderTest = Join-Path $here "wt_status.ps1" }
. (Join-Path $here "lib_safe_remove.ps1")

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("wt_status_chk_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $scratch | Out-Null
$failures = New-Object System.Collections.Generic.List[string]
$assertions = 0
$procs = @()

$sw0 = [Diagnostics.Stopwatch]::StartNew()
function Step([string]$m) { Write-Host ("[{0,5:N1}s] {1}" -f $script:sw0.Elapsed.TotalSeconds, $m) }
function Assert-True($cond, [string]$msg) {
    $script:assertions++
    if (-not $cond) { $script:failures.Add($msg) }
}
function G { & git -c user.name=t -c user.email=t@example.invalid -c core.autocrlf=false @args 2>&1 | Out-Null }

function Set-TreeAge([string]$dir, [double]$hours) {
    # Junction-safe by construction: call BEFORE creating any junction in $dir.
    $t = [DateTime]::UtcNow.AddHours(-$hours)
    foreach ($i in (Get-ChildItem -LiteralPath $dir -Force -Recurse)) {
        if ($i.PSIsContainer) { [System.IO.Directory]::SetLastWriteTimeUtc($i.FullName, $t) }
        else { [System.IO.File]::SetLastWriteTimeUtc($i.FullName, $t) }
    }
    [System.IO.Directory]::SetLastWriteTimeUtc($dir, $t)
}
function Set-DirAge([string]$dir, [double]$hours) { [System.IO.Directory]::SetLastWriteTimeUtc($dir, [DateTime]::UtcNow.AddHours(-$hours)) }
function New-Junction([string]$link, [string]$target) {
    cmd /c mklink /J "$link" "$target" | Out-Null
    if (-not ((Get-Item -LiteralPath $link -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "fixture: junction not created at $link" }
}

try {
    # ---- scratch repo: bare origin + clone, one commit on main ----
    $origin = Join-Path $scratch "origin.git"
    $repo = Join-Path $scratch "repo"
    $wt = Join-Path $scratch "wt"
    $ext = Join-Path $scratch "external"
    New-Item -ItemType Directory -Path $wt, $ext | Out-Null
    G init --bare -b main $origin
    G clone $origin $repo
    G -C $repo checkout -b main
    Set-Content -LiteralPath (Join-Path $repo "a.txt") -Value "a"
    Set-Content -LiteralPath (Join-Path $repo ".gitignore") -Value "*.log`nbuild/`nvenv_link`nlogs/"
    Set-Content -LiteralPath (Join-Path $repo "tracked.kicad_pcb") -Value "board"; G -C $repo add a.txt .gitignore tracked.kicad_pcb   # tracked board file: must not make registered worktrees UNKNOWN
    G -C $repo commit -m "base"
    G -C $repo push -u origin main
    $mainSha = (& git -C $repo rev-parse main).Trim()
    Assert-True ($LASTEXITCODE -eq 0 -and $mainSha) "fixture: scratch repo has no main commit"

    function New-Wt([string]$name) {
        $p = Join-Path $wt $name
        G -C $repo worktree add --detach $p origin/main
        if (-not (Test-Path -LiteralPath (Join-Path $p "a.txt"))) { throw "fixture: worktree $name not created" }
        return $p
    }
    function New-Orphan([string]$name) {
        $p = Join-Path $wt $name
        New-Item -ItemType Directory -Path (Join-Path $p "sub") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $p "sub\f.txt") -Value "x"
        return $p
    }

    # junction targets live OUTSIDE the root and hold sentinels
    $ext1 = Join-Path $ext "t1"; $ext2 = Join-Path $ext "t2"
    foreach ($e in $ext1, $ext2) { New-Item -ItemType Directory -Path (Join-Path $e "deep") -Force | Out-Null; Set-Content -LiteralPath (Join-Path $e "deep\sentinel.txt") -Value "keep" }

    Step 'fixtures: scratch repo ready'
    # ---- registered worktrees ----
    $p = New-Wt "stale_clean";   Set-TreeAge $p 3; New-Junction (Join-Path $p "venv_link") $ext1; Set-DirAge $p 3
    $p = New-Wt "stale_ignored"
    Set-Content -LiteralPath (Join-Path $p "x.log") -Value "log"; Set-TreeAge $p 3
    New-Item -ItemType Directory -Path (Join-Path $p "build") | Out-Null; Set-Content -LiteralPath (Join-Path $p "build\out.o") -Value "fresh object"; Set-DirAge $p 3   # fresh, but build\ is skipped
    $p = New-Wt "ignored_data";  New-Item -ItemType Directory -Path (Join-Path $p "logs\coupling") | Out-Null; Set-Content -LiteralPath (Join-Path $p "logs\coupling\cap.tsv") -Value "captured"; Set-TreeAge $p 3   # gitignored captured data: never prune
    $p = New-Wt "midpick";       $gd = (& git -C $p rev-parse --absolute-git-dir).Trim(); Set-Content -LiteralPath (Join-Path $gd "CHERRY_PICK_HEAD") -Value $mainSha; Set-TreeAge $p 3   # mid-cherry-pick: never prune
    $p = New-Wt "midrebase"      # real conflicted rebase: only copy of the commit lives in rebase state
    Set-Content -LiteralPath (Join-Path $p "rb.txt") -Value "mine"; G -C $p add rb.txt; G -C $p commit -m "rebase mine"
    G -C $repo branch rbother origin/main
    $rbo = Join-Path $scratch "rbother"; G -C $repo worktree add $rbo rbother
    Set-Content -LiteralPath (Join-Path $rbo "rb.txt") -Value "theirs"; G -C $rbo add rb.txt; G -C $rbo commit -m "rebase theirs"
    G -C $repo worktree remove --force $rbo
    G -C $p rebase rbother
    $gdrb = (& git -C $p rev-parse --absolute-git-dir).Trim()
    Assert-True ((Test-Path -LiteralPath (Join-Path $gdrb "rebase-merge")) -or (Test-Path -LiteralPath (Join-Path $gdrb "rebase-apply"))) "fixture: midrebase is not mid-rebase"
    Set-TreeAge $p 3
    $p = New-Wt "cwd_wt";        Set-TreeAge $p 0.5; $cwdWt = $p   # a live process has its cwd here but no path in its command line
    $p = New-Wt "pfx";           Set-TreeAge $p 3
    $p = New-Wt "ahead";         Set-Content -LiteralPath (Join-Path $p "n.txt") -Value "n"; G -C $p add n.txt; G -C $p commit -m "unpushed work"; Set-TreeAge $p 3
    $p = New-Wt "dirty";         Set-TreeAge $p 3; Add-Content -LiteralPath (Join-Path $p "a.txt") -Value "edit"; [System.IO.File]::SetLastWriteTimeUtc((Join-Path $p "a.txt"), [DateTime]::UtcNow.AddHours(-3)); Set-DirAge $p 3
    $p = New-Wt "untracked";     Set-Content -LiteralPath (Join-Path $p "new.txt") -Value "u"; Set-TreeAge $p 3
    $p = New-Wt "recent_clean";  # fresh files from checkout
    $p = New-Wt "proc_wt";       Set-TreeAge $p 3; $procWt = $p
    $p = New-Wt "idle30";        Set-TreeAge $p 0.5
    $p = New-Wt "locked_wt";     Set-TreeAge $p 3; G -C $repo worktree lock $p

    Step 'fixtures: worktrees ready'
    # ---- unregistered dirs and helpers ----
    $p = New-Orphan "orphan";        Set-TreeAge $p 3; New-Junction (Join-Path $p "sub\link_out") $ext2; Set-DirAge $p 3
    $p = New-Orphan "orphan_recent"
    $p = New-Orphan "orphan_git";    Set-Content -LiteralPath (Join-Path $p ".git") -Value "gitdir: nowhere"; Set-TreeAge $p 3
    $p = New-Orphan "orphan_kicad";  Set-Content -LiteralPath (Join-Path $p "sub\x.kicad_pcb") -Value "board"; Set-TreeAge $p 3
    $p = New-Orphan "pfx_longer";    Set-TreeAge $p 3; $procPfx = $p
    $p = New-Orphan "somejob_logs";  Set-TreeAge $p 3
    $p = New-Orphan ".buildgate";    Set-TreeAge $p 3
    $p = New-Orphan ".checkcache";   Set-TreeAge $p 3
    Set-Content -LiteralPath (Join-Path $wt "loose.log") -Value "x"; [System.IO.File]::SetLastWriteTimeUtc((Join-Path $wt "loose.log"), [DateTime]::UtcNow.AddHours(-3))

    Step 'fixtures: orphans/helpers ready'
    # ---- live processes whose command line is under two directories ----
    foreach ($d in $procWt, $procPfx) {
        $procs += Start-Process -FilePath "powershell.exe" -WindowStyle Hidden -PassThru -ArgumentList @("-NoProfile", "-Command", "`"Start-Sleep -Seconds 600 ; '$d\marker.txt'`"")
    }
    # Race fix: a fixed 2 s sleep let a slow powershell startup (disk/AV contention) leave the
    # process invisible to wt_status's Win32_Process scan, so proc_wt/pfx_longer looked idle and
    # were pruned. Wait until both command lines are actually visible (up to 60 s).
    $seenDeadline = (Get-Date).AddSeconds(60)
    do {
        Start-Sleep -Milliseconds 500
        $cl = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | ForEach-Object { ("" + $_.CommandLine) -replace '/', '\' })
        $seen = @($procWt, $procPfx | Where-Object { $d = $_; @($cl | Where-Object { $_.IndexOf("$d\marker.txt", [StringComparison]::OrdinalIgnoreCase) -ge 0 }).Count -gt 0 }).Count
    } while ($seen -lt 2 -and (Get-Date) -lt $seenDeadline)
    Assert-True ($seen -eq 2) "fixture: helper processes for proc_wt/pfx_longer never became visible"

    $procs += Start-Process -FilePath "powershell.exe" -WindowStyle Hidden -PassThru -WorkingDirectory $cwdWt -ArgumentList @("-NoProfile", "-Command", "Start-Sleep -Seconds 600")
    Start-Sleep -Seconds 2
    # ---- cherry-landed: same patch on origin/main under a different sha ----
    $p = New-Wt "landed"; Set-Content -LiteralPath (Join-Path $p "l.txt") -Value "l"; G -C $p add l.txt; G -C $p commit -m "landed work"
    $landedSha = (& git -C $p rev-parse HEAD).Trim()
    G -C $repo cherry-pick -x $landedSha
    G -C $repo push origin main
    G -C $repo fetch origin
    Assert-True ((& git -C $repo rev-parse HEAD).Trim() -ne $landedSha) "fixture: cherry-pick kept the same sha"
    Set-TreeAge $p 3
    $p = New-Wt "ahead2"; Set-Content -LiteralPath (Join-Path $p "n2.txt") -Value "n2"; G -C $p add n2.txt; G -C $p commit -m "unlanded two"; Set-TreeAge $p 3
    Step 'fixtures: processes started'
    $common = @{ Root = $wt; Repo = $repo; Base = "origin/main"; PassThru = $true; Quiet = $true }
    function Run-Tool([hashtable]$extra) {
        $a = @{}; foreach ($k in $script:common.Keys) { $a[$k] = $script:common[$k] }
        foreach ($k in $extra.Keys) { $a[$k] = $extra[$k] }
        return (& $script:ScriptUnderTest @a)
    }
    function Row($res, [string]$n) { return ($res.Rows | Where-Object { $_.Name -eq $n } | Select-Object -First 1) }

    # ======== report mode ========
    $r = Run-Tool @{ Size = $true }
    $expect = [ordered]@{
        stale_clean = "STALE_CLEAN"; stale_ignored = "STALE_CLEAN"; pfx = "STALE_CLEAN"
        ignored_data = "HAS_WORK"; midrebase = "HAS_WORK"; cwd_wt = "UNKNOWN"; midpick = "HAS_WORK"; ahead = "HAS_WORK"; ahead2 = "HAS_WORK"; landed = "STALE_CLEAN"; dirty = "HAS_WORK"; untracked = "HAS_WORK"
        recent_clean = "ACTIVE"; proc_wt = "ACTIVE"; orphan_recent = "ACTIVE"; pfx_longer = "ACTIVE"
        idle30 = "UNKNOWN"; locked_wt = "UNKNOWN"; orphan_git = "UNKNOWN"; orphan_kicad = "UNKNOWN"
        orphan = "ORPHAN_DIR"
    }
    foreach ($k in $expect.Keys) {
        $row = Row $r $k
        Assert-True ($null -ne $row) "report: no row for '$k'"
        if ($row) { Assert-True ($row.Class -eq $expect[$k]) "classification: '$k' expected $($expect[$k]) got $($row.Class) ($($row.Note))" }
    }
    Assert-True ((Row $r "stale_clean").Registered) "report: stale_clean should be registered"
    Assert-True (-not (Row $r "orphan").Registered) "report: orphan should not be registered"
    Assert-True ((Row $r "ahead").Ahead -eq 1) "report: ahead should show 1 commit ahead, got $((Row $r 'ahead').Ahead)"
    Assert-True ((Row $r "landed").Ahead -eq 1 -and (Row $r "landed").Unlanded -eq 0 -and (Row $r "landed").AheadCommits[0] -like "[[]on-main] * landed work") "cherry: rebased-landed commit must be marked on-main and not count as unlanded: $((Row $r 'landed').AheadCommits -join '|')"
    Assert-True ((Row $r "ahead2").Unlanded -eq 1 -and (Row $r "ahead2").AheadCommits[0] -like "[[]unlanded] * unlanded two") "cherry: unlanded commit must be marked unlanded"
    Assert-True ((Row $r "ahead").AheadCommits[0] -like "* unpushed work") "report: ahead commit subject missing"
    Assert-True ((Row $r "dirty").Modified -eq 1 -and (Row $r "dirty").Untracked -eq 0) "report: dirty should be 1 modified 0 untracked"
    Assert-True ((Row $r "untracked").Untracked -eq 1 -and (Row $r "untracked").Modified -eq 0) "report: untracked should be 0 modified 1 untracked"
    Assert-True ((Row $r "stale_ignored").Untracked -eq 0) "report: gitignored files must not count as untracked"
    Assert-True ((Row $r "stale_ignored").IdleMinutes -gt 120) "report: build\ mtimes must be skipped for idle time"
    Assert-True ((Row $r "cwd_wt").Procs -eq 0) "report: cwd-only process is invisible (documented LIMIT); only the idle threshold protects cwd_wt"
    Assert-True ((Row $r "midrebase").Note -match 'rebase' -or (Row $r "midrebase").GitState -match 'rebase') "report: midrebase should name the rebase state ($((Row $r 'midrebase').Note))"
    Assert-True ((Row $r "proc_wt").Procs -ge 1) "report: proc_wt should show a live process"
    Assert-True ((Row $r "pfx").Procs -eq 0) "report: pfx must not match the process under pfx_longer (path boundary)"
    Assert-True ((Row $r "pfx_longer").Procs -ge 1) "report: pfx_longer should show its process"
    Assert-True ((Row $r "stale_clean").Head -match '^[0-9a-f]{6,}$') "report: detached head should show a sha"
    Assert-True ((Row $r "stale_clean").SizeMB -gt 0 -or (Row $r "stale_clean").SizeMB -eq 0) "report: -Size should populate SizeMB"
    Assert-True ($null -ne (Row $r "stale_clean").SizeMB) "report: -Size left SizeMB null"
    $hn = @($r.Helpers | ForEach-Object { $_.Name })
    foreach ($h in ".buildgate", ".checkcache", "somejob_logs", "loose.log") { Assert-True ($hn -contains $h) "helpers: '$h' not listed" }
    Assert-True (-not (@($r.Rows | ForEach-Object { $_.Name }) -contains ".buildgate")) "helpers: .buildgate must not be classified"
    $r2 = Run-Tool @{}
    Assert-True ($null -eq (Row $r2 "stale_clean").SizeMB) "report: SizeMB must be null without -Size"
    Assert-True ($r.Counts["STALE_CLEAN"] -eq 4 -and $r.Counts["ORPHAN_DIR"] -eq 1 -and $r.Counts["HAS_WORK"] -eq 7) "summary counts wrong: $($r.Counts | Out-String)"

    Step 'report mode done'
    # ======== -Prune -WhatIf ========
    $before = @(Get-ChildItem -LiteralPath $wt -Force | ForEach-Object { $_.Name }) | Sort-Object
    $w = Run-Tool @{ Prune = $true; WhatIf = $true }
    $after = @(Get-ChildItem -LiteralPath $wt -Force | ForEach-Object { $_.Name }) | Sort-Object
    Assert-True (($before -join ',') -eq ($after -join ',')) "WhatIf removed or created something under the root"
    Assert-True (@(& git -C $repo worktree list).Count -eq 17) "WhatIf changed the worktree registry"
    $would = @($w.Prune | Where-Object { $_.Action -eq 'would remove' } | ForEach-Object { $_.Name }) | Sort-Object
    Assert-True (($would -join ',') -eq 'landed,orphan,pfx,stale_clean,stale_ignored') "WhatIf should list exactly landed,orphan,pfx,stale_clean,stale_ignored; got: $($would -join ',')"
    Assert-True ((Test-Path -LiteralPath (Join-Path $ext1 "deep\sentinel.txt")) -and (Test-Path -LiteralPath (Join-Path $ext2 "deep\sentinel.txt"))) "WhatIf touched a junction target"

    Step 'WhatIf done'
    # ======== -Prune ========
    $p = Run-Tool @{ Prune = $true }
    Assert-True ($p.Failed -eq 0) "prune reported $($p.Failed) failure(s): $(($p.Prune | Where-Object { $_.Result -match 'FAIL|PARTIAL' } | ForEach-Object { $_.Name + ': ' + $_.Result }) -join '; ')"
    foreach ($gone in "stale_clean", "stale_ignored", "pfx", "orphan", "landed") {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $wt $gone))) "prune: '$gone' should be removed"
    }
    $wtList = (& git -C $repo worktree list) -join "`n"
    foreach ($gone in "stale_clean", "stale_ignored", "pfx", "landed") { Assert-True ($wtList -notmatch "wt/$gone(\s|$)") "prune: '$gone' still registered" }
    foreach ($keep in "ignored_data", "midrebase", "cwd_wt", "midpick", "ahead", "ahead2", "dirty", "untracked", "recent_clean", "proc_wt", "idle30", "locked_wt", "orphan_recent", "orphan_git", "orphan_kicad", "pfx_longer", "somejob_logs", ".buildgate", ".checkcache", "loose.log") {
        Assert-True (Test-Path -LiteralPath (Join-Path $wt $keep)) "prune: '$keep' must survive (HAS_WORK/ACTIVE/UNKNOWN/helper)"
    }
    $touched = @($p.Prune | ForEach-Object { $_.Name })
    foreach ($never in "ignored_data", "midrebase", "cwd_wt", "midpick", "ahead", "ahead2", "dirty", "untracked", "recent_clean", "proc_wt", "pfx_longer", "orphan_recent", "idle30", "locked_wt", "orphan_git", "orphan_kicad") {
        Assert-True ($touched -notcontains $never) "prune: '$never' must never be a prune candidate"
    }
    Assert-True (Test-Path -LiteralPath (Join-Path $ext1 "deep\sentinel.txt")) "JUNCTION: registered-worktree prune followed a junction and deleted its target"
    Assert-True (Test-Path -LiteralPath (Join-Path $ext2 "deep\sentinel.txt")) "JUNCTION: orphan prune followed a junction and deleted its target"
    Assert-True ((Test-Path -LiteralPath (Join-Path $repo "a.txt"))) "prune damaged the scratch main repo"
}
finally {
    foreach ($pr in $procs) { try { Stop-Process -Id $pr.Id -Force -ErrorAction SilentlyContinue } catch { } }
    try { Start-Sleep -Milliseconds 300; Remove-TreeSafe -Path $scratch } catch { }
}

if ($failures.Count -gt 0) {
    Write-Host "WT_STATUS CHECK FAILED ($($failures.Count) of $assertions assertion(s)):" -ForegroundColor Red
    foreach ($m in $failures) { Write-Host "  $m" -ForegroundColor Red }
    exit 1
}
Write-Host "wt_status check passed: $assertions assertions (classifications, helpers, WhatIf, prune guards, junction targets survived)."
exit 0

