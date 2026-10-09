# check_check_cache.ps1 -- unit test for tools/checkcache_lib.ps1 (the machine-wide
# check-result cache used by run_all_checks.ps1). Runs against a throwaway git
# repo and a throwaway cache directory; touches neither the real tree nor the
# real C:\wt\.checkcache.
#
# Each numbered case below was negative-tested by hand when this was written:
# the matching line in checkcache_lib.ps1 was broken, this script went RED, and
# the line was restored by hand (see the commit message for the list).
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
. (Join-Path $here "checkcache_lib.ps1")

$fails = New-Object System.Collections.Generic.List[string]
function Assert-That {
    param([bool]$Cond, [string]$Name)
    if ($Cond) { Write-Host "  ok    $Name" } else { Write-Host "  FAIL  $Name" -ForegroundColor Red; $fails.Add($Name) }
}

$work = Join-Path $env:TEMP "kilnctl_check_check_cache_$PID"
$repo = Join-Path $work "repo"
$cacheDir = Join-Path $work "cache"
$savedDir = $env:KILNCTL_CHECKCACHE_DIR
$savedOff = $env:KILNCTL_CHECKCACHE
$savedProbe = $env:KILNCTL_FPTEST_PROBE
try {
    New-Item -ItemType Directory -Path $repo -Force | Out-Null
    $env:KILNCTL_CHECKCACHE_DIR = $cacheDir
    Remove-Item Env:\KILNCTL_CHECKCACHE -ErrorAction SilentlyContinue
    Remove-Item Env:\KILNCTL_FPTEST_PROBE -ErrorAction SilentlyContinue

    function Tgit { $ErrorActionPreference = "Continue"; & git -C $repo -c user.email=t@t -c user.name=t -c commit.gpgsign=false @args 2>&1 | Out-Null; if ($LASTEXITCODE -ne 0) { throw "git $args failed" } }
    $rel0 = "check_ok.ps1"; $rel1 = "check_excluded.ps1"
    Tgit init -q
    Set-Content -LiteralPath (Join-Path $repo ".gitignore") -Value "ignored_out/"
    Set-Content -LiteralPath (Join-Path $repo $rel0) -Value "# checkcache: ok`nexit 0"
    Set-Content -LiteralPath (Join-Path $repo $rel1) -Value "# touches the board`nexit 0"
    Set-Content -LiteralPath (Join-Path $repo "data.txt") -Value "v1"
    Tgit add -A
    Tgit commit -q -m one

    $rel = "check_ok.ps1"
    function New-Ctx { param([switch]$Fast, [switch]$NoCache, [int]$MaxAgeDays = 7) Initialize-CheckCache -RepoRoot $repo -Fast:$Fast -NoCache:$NoCache -MaxAgeDays $MaxAgeDays }
    function Store { param($Ctx, [string]$R = $rel, [string]$Bucket = "pass")
        [void](Add-CheckCacheResult -Ctx $Ctx -Rel $R -Bucket $Bucket -DurationSec 3 -OutputText "out")
        return (Save-CheckCache -Ctx $Ctx) }

    Write-Host "case 1: hit on the same tree"
    $c = New-Ctx
    Assert-That $c.Enabled "clean tree enables the cache"
    Assert-That ($null -eq (Find-CheckCacheHit -Ctx $c -Rel $rel)) "miss before anything is stored"
    Assert-That ((Store $c) -eq 1) "PASS of a marked check is stored"
    $c2 = New-Ctx
    $h = Find-CheckCacheHit -Ctx $c2 -Rel $rel
    Assert-That ($null -ne $h) "hit from a fresh context on the same tree"
    Assert-That ($h.Worktree -eq "repo" -and $h.DurationSec -eq 3) "hit reports worktree and duration"
    Assert-That ($null -eq (Find-CheckCacheHit -Ctx (New-Ctx -Fast) -Rel $rel)) "miss in the other run mode (-Fast vs full)"
    Tgit commit -q --allow-empty -m "same tree, new commit"
    Assert-That ($null -ne (Find-CheckCacheHit -Ctx (New-Ctx) -Rel $rel)) "hit across commits with an identical tree"

    Write-Host "case 2: miss when the tree changes or is dirty"
    Add-Content -LiteralPath (Join-Path $repo "data.txt") -Value "dirty"
    $d = New-Ctx
    Assert-That (-not $d.Enabled) "modified tracked file disables the cache"
    Assert-That ($null -eq (Find-CheckCacheHit -Ctx $d -Rel $rel)) "no hit on a dirty tree even though an entry exists"
    Tgit add data.txt
    Assert-That (-not (New-Ctx).Enabled) "staged change disables the cache"
    Tgit commit -q -m "v2"
    $c3 = New-Ctx
    Assert-That ($c3.Enabled -and $null -eq (Find-CheckCacheHit -Ctx $c3 -Rel $rel)) "committed content change is a miss"
    [void](Store $c3)
    Set-Content -LiteralPath (Join-Path $repo "stray.txt") -Value "x"
    Assert-That (-not (New-Ctx).Enabled) "untracked non-ignored file disables the cache"
    Remove-Item -LiteralPath (Join-Path $repo "stray.txt")
    New-Item -ItemType Directory -Path (Join-Path $repo "ignored_out") -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $repo "ignored_out\a.o") -Value "x"
    Assert-That (New-Ctx).Enabled "gitignored build output does not disable the cache"
    Tgit update-index --assume-unchanged data.txt
    Assert-That (-not (New-Ctx).Enabled) "assume-unchanged tracked file disables the cache"
    Tgit update-index --no-assume-unchanged data.txt
    Assert-That (New-Ctx).Enabled "cache re-enabled once the bit is cleared"
    # a check that dirties the tree during the run: nothing may be stored
    $e = New-Ctx
    [void](Add-CheckCacheResult -Ctx $e -Rel $rel -Bucket pass -DurationSec 1)
    $before = @(Get-ChildItem -LiteralPath $cacheDir -Filter *.json).Count
    $origData = [System.IO.File]::ReadAllText((Join-Path $repo "data.txt"))
    Set-Content -LiteralPath (Join-Path $repo "data.txt") -Value "mid-run edit"
    Assert-That ((Save-CheckCache -Ctx $e) -eq 0) "tree dirtied mid-run stores nothing"
    Assert-That (@(Get-ChildItem -LiteralPath $cacheDir -Filter *.json).Count -eq $before) "no entry file written after mid-run dirtying"
    [System.IO.File]::WriteAllText((Join-Path $repo "data.txt"), $origData)

    Write-Host "case 3: miss on a changed fingerprint"
    $env:KILNCTL_FPTEST_PROBE = "1"
    $f = New-Ctx
    Assert-That ($null -eq (Find-CheckCacheHit -Ctx $f -Rel $rel)) "new KILNCTL_* env var changes the fingerprint -> miss"
    [void](Store $f)
    Assert-That ($null -ne (Find-CheckCacheHit -Ctx (New-Ctx) -Rel $rel)) "same probe value hits"
    $env:KILNCTL_FPTEST_PROBE = "2"
    Assert-That ($null -eq (Find-CheckCacheHit -Ctx (New-Ctx) -Rel $rel)) "different env value -> miss"
    Remove-Item Env:\KILNCTL_FPTEST_PROBE
    Assert-That ($null -ne (Find-CheckCacheHit -Ctx (New-Ctx) -Rel $rel)) "original fingerprint still hits its own entry"
    $env:KILNCTL_WEB_PASSWORD = "secret-a"
    $pw1 = (New-Ctx).Fingerprint
    $env:KILNCTL_WEB_PASSWORD = "secret-b"
    Assert-That ($pw1 -eq (New-Ctx).Fingerprint) "credential env vars are excluded from the fingerprint"
    Remove-Item Env:\KILNCTL_WEB_PASSWORD

    Write-Host "case 4: excluded (unmarked) checks never cache"
    $x = New-Ctx
    Assert-That ((Add-CheckCacheResult -Ctx $x -Rel "check_excluded.ps1" -Bucket pass -DurationSec 1) -eq $false) "unmarked check is not queued"
    Assert-That ((Save-CheckCache -Ctx $x) -eq 0) "nothing stored for an unmarked check"
    # plant an entry for the unmarked check with the correct key: lookup must still refuse
    $key = Get-CheckCacheKey -Ctx $x -Rel "check_excluded.ps1"
    $planted = [ordered]@{ schema = 1; key = $key; result = "PASS"; check = "check_excluded.ps1"; tree = $x.Tree; mode = $x.Mode
        fingerprint = $x.Fingerprint; time_utc = (Get-Date).ToUniversalTime().ToString("o"); worktree = "evil"; duration_s = 1; output_tail = "" }
    [void](Write-CheckCacheEntry -Dir $cacheDir -Key $key -Entry $planted)
    Assert-That ($null -eq (Find-CheckCacheHit -Ctx $x -Rel "check_excluded.ps1")) "planted entry for an unmarked check is not served"

    Write-Host "case 5: only PASS is ever stored"
    $n0 = @(Get-ChildItem -LiteralPath $cacheDir -Filter *.json).Count
    $y = New-Ctx
    foreach ($b in "fail", "skip", "skipfast", "busy", "timeout", "") {
        Assert-That ((Add-CheckCacheResult -Ctx $y -Rel $rel -Bucket $b -DurationSec 1) -eq $false) "bucket '$b' is not queued"
    }
    Assert-That ((Save-CheckCache -Ctx $y) -eq 0 -and @(Get-ChildItem -LiteralPath $cacheDir -Filter *.json).Count -eq $n0) "no entry written for non-PASS buckets"
    # a hand-planted FAIL entry with otherwise correct fields must not hit
    $yk = Get-CheckCacheKey -Ctx $y -Rel $rel
    Remove-Item -LiteralPath (Join-Path $cacheDir "$yk.json") -ErrorAction SilentlyContinue
    $bad = [ordered]@{ schema = 1; key = $yk; result = "FAIL"; check = $rel; tree = $y.Tree; mode = $y.Mode; fingerprint = $y.Fingerprint
        time_utc = (Get-Date).ToUniversalTime().ToString("o"); worktree = "w"; duration_s = 1; output_tail = "" }
    [void](Write-CheckCacheEntry -Dir $cacheDir -Key $yk -Entry $bad)
    Assert-That ($null -eq (Find-CheckCacheHit -Ctx $y -Rel $rel)) "FAIL entry is never served as a hit"

    Write-Host "case 6: expiry"
    $z = New-Ctx
    $zk = Get-CheckCacheKey -Ctx $z -Rel $rel
    Remove-Item -LiteralPath (Join-Path $cacheDir "$zk.json") -ErrorAction SilentlyContinue
    $old = [ordered]@{ schema = 1; key = $zk; result = "PASS"; check = $rel; tree = $z.Tree; mode = $z.Mode; fingerprint = $z.Fingerprint
        time_utc = (Get-Date).ToUniversalTime().AddDays(-8).ToString("o"); worktree = "w"; duration_s = 1; output_tail = "" }
    [void](Write-CheckCacheEntry -Dir $cacheDir -Key $zk -Entry $old)
    Assert-That ($null -eq (Find-CheckCacheHit -Ctx $z -Rel $rel)) "8-day-old entry is expired -> miss"
    Assert-That (-not (Test-Path -LiteralPath (Join-Path $cacheDir "$zk.json"))) "expired entry is deleted on read"
    $fresh = $old; $fresh.time_utc = (Get-Date).ToUniversalTime().AddDays(-6).ToString("o")
    [void](Write-CheckCacheEntry -Dir $cacheDir -Key $zk -Entry $fresh)
    Assert-That ($null -ne (Find-CheckCacheHit -Ctx $z -Rel $rel)) "6-day-old entry is still valid"
    Remove-Item -LiteralPath (Join-Path $cacheDir "$zk.json")
    $future = $old; $future.time_utc = (Get-Date).ToUniversalTime().AddDays(3).ToString("o")
    [void](Write-CheckCacheEntry -Dir $cacheDir -Key $zk -Entry $future)
    Assert-That ($null -eq (Find-CheckCacheHit -Ctx $z -Rel $rel)) "entry timestamped in the future is refused"
    # prune: old files and the entry cap
    $pd = Join-Path $work "prune"; New-Item -ItemType Directory -Path $pd -Force | Out-Null
    1..5 | ForEach-Object { $p = Join-Path $pd "k$_.json"; Set-Content -LiteralPath $p -Value "{}"; (Get-Item $p).LastWriteTime = (Get-Date).AddMinutes(-$_) }
    (Get-Item (Join-Path $pd "k5.json")).LastWriteTime = (Get-Date).AddDays(-9)
    Invoke-CheckCachePrune -Dir $pd -MaxAgeDays 7 -MaxEntries 3
    $left = @(Get-ChildItem -LiteralPath $pd -Filter *.json | ForEach-Object Name | Sort-Object)
    Assert-That (($left -join ",") -eq "k1.json,k2.json,k3.json") "prune drops expired file then oldest beyond the cap"

    Write-Host "case 7: atomic write"
    $ad = Join-Path $work "atomic"
    $ent = [ordered]@{ schema = 1; key = "abc"; result = "PASS" }
    Assert-That (Write-CheckCacheEntry -Dir $ad -Key "abc" -Entry $ent) "first write creates the file"
    Assert-That (-not (Write-CheckCacheEntry -Dir $ad -Key "abc" -Entry ([ordered]@{ result = "OTHER" }))) "second write to an existing key does not overwrite"
    Assert-That ((Get-Content -LiteralPath (Join-Path $ad "abc.json") -Raw | ConvertFrom-Json).result -eq "PASS") "first writer's content survived"
    Assert-That (@(Get-ChildItem -LiteralPath $ad -Force -Filter "*.tmp").Count -eq 0) "no temp files left behind"
    # a crashed writer's leftover temp (or truncated final) file is never read as an entry
    $c4 = New-Ctx
    $k4 = Get-CheckCacheKey -Ctx $c4 -Rel $rel
    Remove-Item -LiteralPath (Join-Path $cacheDir "$k4.json") -ErrorAction SilentlyContinue
    Set-Content -LiteralPath (Join-Path $cacheDir ".$k4.999.tmp") -Value '{"schema":1,"result":"PA'
    Assert-That ($null -eq (Find-CheckCacheHit -Ctx $c4 -Rel $rel)) "stray half-written .tmp is ignored"
    Set-Content -LiteralPath (Join-Path $cacheDir "$k4.json") -Value '{"schema":1,"result":"PA'
    Assert-That ($null -eq (Find-CheckCacheHit -Ctx $c4 -Rel $rel)) "truncated entry file is a miss"
    Assert-That (-not (Test-Path -LiteralPath (Join-Path $cacheDir "$k4.json"))) "corrupt entry is removed so it can be rewritten"
    # concurrent writers: N processes race the same key; exactly one file, valid JSON, no temps
    $cd = Join-Path $work "race"; New-Item -ItemType Directory -Path $cd -Force | Out-Null
    $lib = Join-Path $here "checkcache_lib.ps1"
    $procs = 1..6 | ForEach-Object {
        $cmd = ". '$lib'; `$e = [ordered]@{ schema = 1; key = 'racekey'; result = 'PASS'; who = $_ }; [void](Write-CheckCacheEntry -Dir '$cd' -Key 'racekey' -Entry `$e)"
        Start-Process powershell -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", $cmd) -PassThru -WindowStyle Hidden
    }
    $procs | ForEach-Object { $_.WaitForExit() }
    $raceFiles = @(Get-ChildItem -LiteralPath $cd -Force)
    Assert-That ($raceFiles.Count -eq 1 -and $raceFiles[0].Name -eq "racekey.json") "6 concurrent writers leave exactly one file and no temps"
    $okJson = $false
    try { $okJson = ((Get-Content -LiteralPath (Join-Path $cd "racekey.json") -Raw | ConvertFrom-Json).result -eq "PASS") } catch {}
    Assert-That $okJson "the surviving file is complete, valid JSON"

    Write-Host "case 8: controls"
    Assert-That (-not (New-Ctx -NoCache).Enabled) "-NoCache disables"
    $env:KILNCTL_CHECKCACHE = "0"
    Assert-That (-not (New-Ctx).Enabled) "KILNCTL_CHECKCACHE=0 disables"
    Remove-Item Env:\KILNCTL_CHECKCACHE
    $nogit = Join-Path $work "notarepo"; New-Item -ItemType Directory -Path $nogit -Force | Out-Null
    Assert-That (-not (Initialize-CheckCache -RepoRoot $nogit).Enabled) "a directory that is not a git repo disables (fail closed)"
}
finally {
    foreach ($p in @(@("KILNCTL_CHECKCACHE_DIR", $savedDir), @("KILNCTL_CHECKCACHE", $savedOff), @("KILNCTL_FPTEST_PROBE", $savedProbe))) {
        if ($null -eq $p[1]) { Remove-Item "Env:\$($p[0])" -ErrorAction SilentlyContinue } else { Set-Item "Env:\$($p[0])" $p[1] }
    }
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

if ($fails.Count -gt 0) {
    Write-Host ""
    Write-Host "check_check_cache: $($fails.Count) assertion(s) FAILED:" -ForegroundColor Red
    $fails | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
Write-Host ""
Write-Host "check_check_cache: all assertions passed." -ForegroundColor Green
exit 0
