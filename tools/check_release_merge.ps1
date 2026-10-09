# check_release_merge.ps1 -- release_merge.ps1 against a scratch bare origin (never a real remote).
# Covers: dry run pushes nothing; -Push lands ONE single-parent commit and an annotated tag
# atomically; default trailer is the Opus one; a rejected release push leaves no tag on origin
# and no local tag (atomic, F13); an existing tag and a non-ancestor commit are refused.
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:fails = 0
function Assert([bool]$c, [string]$w) { if ($c) { Write-Host "  ok: $w" } else { Write-Host "  FAIL: $w" -ForegroundColor Red; $script:fails++ } }
$env:GIT_AUTHOR_NAME = "t"; $env:GIT_AUTHOR_EMAIL = "t@example.invalid"
$env:GIT_COMMITTER_NAME = "t"; $env:GIT_COMMITTER_EMAIL = "t@example.invalid"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("relmerge_chk_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$origin = Join-Path $tmp "origin.git"; $work = Join-Path $tmp "work"
function Run-RelMerge([string[]]$a) {
    $o = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $here "release_merge.ps1") -RepoPath $work @a 2>&1 | Out-String
    return [pscustomobject]@{ Rc = $LASTEXITCODE; Out = $o }
}
function Commit([string]$f, [string]$msg) {
    Set-Content -LiteralPath (Join-Path $work $f) -Value $msg -Encoding ascii
    git -C $work add $f *>$null; git -C $work commit -m $msg *>$null
    return (git -C $work rev-parse HEAD).Trim()
}
try {
    git init --bare -b main $origin *>$null
    git clone $origin $work *>$null
    git -C $work checkout -b main *>$null
    $c1 = Commit "a.txt" "c1"
    git -C $work push origin main *>$null
    # release branch: one single-parent commit naming c1
    $tree = (git -C $work rev-parse "$c1^{tree}").Trim()
    $rmsg = Join-Path $tmp "rmsg.txt"
    [IO.File]::WriteAllText($rmsg, "Release v1.0.0-pre.1`n`nFiles identical to main commit $c1 (tag v1.0.0-pre.1).`n", (New-Object Text.UTF8Encoding($false)))
    $r1 = (git -C $work commit-tree $tree -F $rmsg).Trim()
    git -C $work push origin "${r1}:refs/heads/release" *>$null
    $c2 = Commit "b.txt" "c2"
    git -C $work push origin main *>$null
    git -C $work fetch origin *>$null

    Write-Host "case: dry run"
    $r = Run-RelMerge @("-Commit", $c2, "-Tag", "v1.0.0-pre.2")
    Assert ($r.Rc -eq 0 -and $r.Out -match "DRY RUN") "dry run succeeds"
    Assert (-not (git -C $origin tag --list v1.0.0-pre.2)) "dry run created no tag on origin"
    Assert ((git -C $origin rev-parse release).Trim() -eq $r1) "dry run left origin/release alone"

    Write-Host "case: refusals"
    $r = Run-RelMerge @("-Commit", $c1, "-Tag", "v1.0.0-pre.2")
    Assert ($r.Rc -ne 0 -and $r.Out -match "REFUSED") "commit already on release refused"
    $r = Run-RelMerge @("-Commit", $c2, "-Tag", "not-a-tag")
    Assert ($r.Rc -ne 0 -and $r.Out -match "tag-version-regex") "invalid tag refused"
    $r = Run-RelMerge @("-Commit", $c2, "-Tag", "v1.0.0-pre.1")
    Assert ($r.Rc -ne 0 -and $r.Out -match "semver-newer") "non-newer tag refused"

    Write-Host "case: rejected release push is atomic (F13)"
    $hook = Join-Path $origin "hooks\pre-receive"
    [IO.File]::WriteAllText($hook, "#!/bin/sh`nwhile read o n ref; do if [ `"`$ref`" = refs/heads/release ]; then echo no-release >&2; exit 1; fi; done`nexit 0`n", (New-Object Text.UTF8Encoding($false)))
    $r = Run-RelMerge @("-Commit", $c2, "-Tag", "v1.0.0-pre.2", "-Push")
    Assert ($r.Rc -ne 0) "push rejected -> non-zero exit"
    Assert (-not (git -C $origin tag --list v1.0.0-pre.2)) "no tag on origin after rejected atomic push"
    Assert (-not (git -C $work tag --list v1.0.0-pre.2)) "local tag removed after rejected push"
    Assert ((git -C $origin rev-parse release).Trim() -eq $r1) "origin/release unchanged after rejection"
    Remove-Item -LiteralPath $hook -Force

    Write-Host "case: -Push"
    $r = Run-RelMerge @("-Commit", $c2, "-Tag", "v1.0.0-pre.2", "-Message", "gate evidence", "-Push")
    Assert ($r.Rc -eq 0 -and $r.Out -match "pushed and verified") "push succeeds"
    $rel = (git -C $origin rev-parse release).Trim()
    Assert ((git -C $origin rev-parse "$rel^{tree}").Trim() -eq (git -C $work rev-parse "$c2^{tree}").Trim()) "release tree == commit tree"
    Assert ((git -C $origin rev-parse "$rel^").Trim() -eq $r1) "single parent is the old release tip"
    Assert ((git -C $origin rev-parse "v1.0.0-pre.2^{commit}").Trim() -eq $rel) "annotated tag peels to the release commit"
    Assert ((git -C $origin cat-file -t v1.0.0-pre.2).Trim() -eq "tag") "tag is annotated"
    $body = (git -C $origin log -1 --format=%B $rel) -join "`n"
    Assert ($body -match "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>") "default trailer is Opus 5.5"
    $r = Run-RelMerge @("-Commit", $c2, "-Tag", "v1.0.0-pre.3")
    Assert ($r.Rc -ne 0) "re-release of the same commit refused"
} finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
if ($script:fails -gt 0) { Write-Host "check_release_merge: $script:fails FAILED" -ForegroundColor Red; exit 1 }
Write-Host "check_release_merge: all cases passed"; exit 0
