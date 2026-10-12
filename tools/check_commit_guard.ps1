# check_commit_guard.ps1 -- commit_guard.ps1 against a scratch bare origin (never a real remote).
# Covers F8 (fetches before comparing: a file another session changed on origin/dev after our last fetch
# is not reported UNCHANGED) and F17 (comma strings under -File, binary numstat, deleted paths).
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:fails = 0
function Assert([bool]$c, [string]$w) { if ($c) { Write-Host "  ok: $w" } else { Write-Host "  FAIL: $w" -ForegroundColor Red; $script:fails++ } }
$env:GIT_AUTHOR_NAME = "t"; $env:GIT_AUTHOR_EMAIL = "t@example.invalid"
$env:GIT_COMMITTER_NAME = "t"; $env:GIT_COMMITTER_EMAIL = "t@example.invalid"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("cguard_chk_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$origin = Join-Path $tmp "origin.git"; $work = Join-Path $tmp "work"; $other = Join-Path $tmp "other"
function Run-CG([string[]]$a) {
    Push-Location $work
    try { $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "commit_guard.ps1") @a 2>&1 | Out-String; $rc = $LASTEXITCODE }
    finally { Pop-Location }
    return [pscustomobject]@{ Rc = $rc; Out = $o }
}
function Put([string]$repo, [string]$f, [string]$content) { [IO.File]::WriteAllText((Join-Path $repo $f), $content, (New-Object Text.UTF8Encoding($false))) }
function Commit([string]$repo, [string]$msg) {
    git -C $repo add -A *>$null; git -C $repo commit -m $msg *>$null
    return (git -C $repo rev-parse HEAD).Trim()
}
try {
    git init --bare -b dev $origin *>$null
    git clone $origin $work *>$null
    git -C $work checkout -b dev *>$null
    git -C $work config core.autocrlf false
    Put $work "a.txt" "one`ntwo`nthree`n"; Put $work "b.txt" "x`ny`n"; Put $work "gone.txt" "bye`n"
    [IO.File]::WriteAllBytes((Join-Path $work "bin.dat"), [byte[]](0, 1, 2, 3, 0, 5))
    Commit $work "base" | Out-Null
    git -C $work push origin dev *>$null

    Write-Host "case: unchanged / changed / confirm"
    $r = Run-CG @("-Path", "a.txt")
    Assert ($r.Rc -eq 0 -and $r.Out -match "UNCHANGED") "unchanged file passes"
    Put $work "a.txt" "one`ntwo`nthree`nfour`n"
    $r = Run-CG @("-Path", "a.txt")
    Assert ($r.Rc -eq 1 -and $r.Out -match "REFUSED") "changed file refused without -Confirm"
    $r = Run-CG @("-Path", "a.txt", "-Confirm")
    Assert ($r.Rc -eq 0 -and $r.Out -match "CONFIRMED") "changed file passes with -Confirm"
    $r = Run-CG @("-Path", "a.txt", "-ExpectedMaxLines", "0", "-Confirm")
    Assert ($r.Rc -eq 1 -and $r.Out -match "EXCEEDS") "budget of 0 lines exceeded -> refused even with -Confirm"
    git -C $work restore a.txt *>$null

    Write-Host "case: F8 compares against the CURRENT origin/dev (fetches first)"
    git clone $origin $other *>$null
    git -C $other checkout dev *>$null
    Put $other "b.txt" "x`ny`nz from other`n"
    Commit $other "other changes b" | Out-Null
    git -C $other push origin dev *>$null
    $before = (git -C $work rev-parse "origin/dev:b.txt").Trim()
    $r = Run-CG @("-Path", "b.txt")
    Assert ($before -ne (git -C $work rev-parse "origin/dev:b.txt").Trim()) "origin/dev tracking ref advanced (fetch happened)"
    Assert ($r.Rc -eq 1 -and $r.Out -notmatch "UNCHANGED") "stale working copy of b.txt is flagged, not UNCHANGED"
    git -C $work merge --ff-only origin/dev *>$null

    Write-Host "case: fetch failure refuses"
    $r = Run-CG @("-Path", "a.txt", "-Branch", "nosuchremote/dev")
    Assert ($r.Rc -eq 1) "unfetchable branch -> exit 1"

    Write-Host "case: F17 comma-joined arrays under -File"
    Put $work "a.txt" "one`ntwo`nthree`nfour`n"; Put $work "b.txt" "changed`n"
    $r = Run-CG @("-Path", "a.txt,b.txt", "-ExpectedMaxLines", "10,10", "-Confirm")
    Assert ($r.Rc -eq 0 -and $r.Out -match "a\.txt" -and $r.Out -match "b\.txt") "-Path a,b -ExpectedMaxLines 10,10 accepted as two entries"
    $r = Run-CG @("-Path", "a.txt,b.txt", "-ExpectedMaxLines", "10,1", "-Confirm")
    Assert ($r.Rc -eq 1 -and $r.Out -match "EXCEEDS") "per-path budget applies to the second entry"
    $r = Run-CG @("-Path", "a.txt,b.txt", "-ExpectedMaxLines", "10", "-Confirm")
    Assert ($r.Rc -eq 1 -and $r.Out -match "one entry per") "count mismatch after splitting -> usage error"
    $r = Run-CG @("-Path", "a.txt", "-ExpectedMaxLines", "abc", "-Confirm")
    Assert ($r.Rc -eq 1 -and $r.Out -match "not an integer") "non-integer budget -> usage error"
    git -C $work restore a.txt b.txt *>$null

    Write-Host "case: F17 binary change is not 0 lines"
    [IO.File]::WriteAllBytes((Join-Path $work "bin.dat"), [byte[]](9, 0, 8, 0, 7, 0, 6, 0))
    $r = Run-CG @("-Path", "bin.dat", "-ExpectedMaxLines", "5", "-Confirm")
    Assert ($r.Rc -eq 1 -and $r.Out -match "binary") "binary change with a declared budget refused"
    $r = Run-CG @("-Path", "bin.dat")
    Assert ($r.Rc -eq 1 -and $r.Out -match "BINARY") "binary change without -Confirm refused and labelled"
    $r = Run-CG @("-Path", "bin.dat", "-Confirm")
    Assert ($r.Rc -eq 0) "binary change with -Confirm and no budget passes"
    git -C $work restore bin.dat *>$null

    Write-Host "case: F17 deleted path"
    Remove-Item -LiteralPath (Join-Path $work "gone.txt") -Force
    $r = Run-CG @("-Path", "gone.txt")
    Assert ($r.Rc -eq 1 -and $r.Out -match "DELETED") "deleted tracked file is guardable, refused without -Confirm"
    $r = Run-CG @("-Path", "gone.txt", "-Confirm")
    Assert ($r.Rc -eq 0) "deleted tracked file passes with -Confirm"
    $r = Run-CG @("-Path", "never_existed.txt")
    Assert ($r.Rc -eq 1 -and $r.Out -match "does not exist") "path in neither tree nor branch -> error"
} finally { Set-Location $here; Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
if ($script:fails -gt 0) { Write-Host "check_commit_guard: $script:fails FAILED" -ForegroundColor Red; exit 1 }
Write-Host "check_commit_guard: all cases passed"; exit 0
