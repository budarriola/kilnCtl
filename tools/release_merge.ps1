# release_merge.ps1 -- add exactly ONE single-parent commit to origin/release for one release tag.
#
# Flow (docs/RELEASING.md "Release branch"): qualify a main commit X, then
#   release_merge.ps1 -Commit <X> -Tag v1.0.0-pre.N -Message "<gate evidence>" [-Push]
# builds M = commit-tree(tree of X, sole parent origin/release tip; message line 'Files identical to main commit <X> (tag <T>).'), pushes release as a plain
# fast-forward, creates the ANNOTATED tag on M (message = -Message) and pushes it. make_release.ps1
# then builds from M. Dry run unless -Push; it never touches the working tree or index (git plumbing
# only) and never force-pushes.
#
# Refusals (each names its rule): X not an ancestor of origin/main; the main commit named in the release tip's message not an ancestor of X;
# tag not valid per gen_build_info.cmake's rules (the ones check_release_version_regex.ps1 pins);
# tag already exists (local or origin); tag not semver-newer than every tag named in a release commit message.
#
# -RepoPath points at another repository (the scratch-repo negative tests); default is this repo.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Commit,
    [Parameter(Mandatory)][string]$Tag,
    [string]$Message = "",
    [switch]$Push,
    [string]$RepoPath = "",
    [string]$Trailer = "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
)
$ErrorActionPreference = 'Continue'
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $RepoPath) { $RepoPath = Split-Path -Parent $scriptRoot }
$cmakePath = Join-Path $scriptRoot '..\firmware\KilnFW\App\drivers\gen_build_info.cmake'

function Fail([string]$rule, [string]$msg) { Write-Host "REFUSED [$rule]: $msg" -ForegroundColor Red; exit 1 }
function G { $o = & git -C $RepoPath @args; $script:rc = $LASTEXITCODE; return $o }
function GOk { & git -C $RepoPath @args | Out-Null; return ($LASTEXITCODE -eq 0) }

function Get-CmakeString([string]$text, [string]$name) {
    $m = [regex]::Match($text, '(?m)^\s*set\(' + [regex]::Escape($name) + '\s+"(.*)"\)\s*$')
    if (-not $m.Success) { Fail 'tag-regex' "gen_build_info.cmake: no set($name) line found" }
    return $m.Groups[1].Value.Replace('\\', '\')
}
function Test-TagValid([string]$tag) {
    $cmake = Get-Content -Raw -LiteralPath $cmakePath
    $pos = Get-CmakeString $cmake '_RV_TAG_REGEX'
    $neg = (Get-CmakeString $cmake '_RV_REJECT_REGEXES') -split ';'
    if ($tag.Length -gt 32) { return $false }
    if ($tag -cnotmatch $pos) { return $false }
    foreach ($n in $neg) { if ($tag -cmatch $n) { return $false } }
    return $true
}
# SemVer 2.0 precedence. Returns -1/0/1.
function Compare-Semver([string]$a, [string]$b) {
    $ra = [regex]::Match($a, '^v(\d+)\.(\d+)\.(\d+)(?:-(.+))?$'); $rb = [regex]::Match($b, '^v(\d+)\.(\d+)\.(\d+)(?:-(.+))?$')
    for ($i = 1; $i -le 3; $i++) {
        $x = [bigint]$ra.Groups[$i].Value; $y = [bigint]$rb.Groups[$i].Value
        if ($x -lt $y) { return -1 }; if ($x -gt $y) { return 1 }
    }
    $pa = $ra.Groups[4]; $pb = $rb.Groups[4]
    if (-not $pa.Success -and -not $pb.Success) { return 0 }
    if (-not $pa.Success) { return 1 }
    if (-not $pb.Success) { return -1 }
    $ia = $pa.Value -split '\.'; $ib = $pb.Value -split '\.'
    for ($i = 0; $i -lt [Math]::Min($ia.Count, $ib.Count); $i++) {
        $na = $ia[$i] -match '^\d+$'; $nb = $ib[$i] -match '^\d+$'
        if ($na -and $nb) { $c = ([bigint]$ia[$i]).CompareTo([bigint]$ib[$i]) }
        elseif ($na) { $c = -1 } elseif ($nb) { $c = 1 }
        else { $c = [string]::CompareOrdinal($ia[$i], $ib[$i]) }
        if ($c -ne 0) { return [Math]::Sign($c) }
    }
    return [Math]::Sign($ia.Count - $ib.Count)
}

# 1. fetch
G fetch --quiet origin --tags | Out-Null
if ($rc -ne 0) { Fail 'fetch' 'git fetch origin --tags failed.' }
$relTip = (G rev-parse --verify --quiet 'refs/remotes/origin/release^{commit}')
if ($rc -ne 0 -or -not $relTip) { Fail 'release-branch' 'origin/release does not exist.' }
$mainTip = (G rev-parse --verify --quiet 'refs/remotes/origin/main^{commit}')
if ($rc -ne 0 -or -not $mainTip) { Fail 'origin-main' 'origin/main does not exist.' }
$x = (G rev-parse --verify --quiet "$Commit^{commit}")
if ($rc -ne 0 -or -not $x) { Fail 'commit' "-Commit '$Commit' is not a commit." }

# 2. refusals
if (-not (GOk merge-base --is-ancestor $x $mainTip)) { Fail 'commit-on-origin-main' "$x is not an ancestor of origin/main." }
$NamedRe = 'Files identical to main commit ([0-9a-f]{40}) \(tag (v[^\s)]+)\)\.'
$tipMsg = (G log -1 --format=%B $relTip) -join "`n"
$nm = [regex]::Match($tipMsg, $NamedRe)
if (-not $nm.Success) { Fail 'release-tip-shape' "release tip $relTip has no 'Files identical to main commit <sha> (tag <T>).' line." }
$prevMain = $nm.Groups[1].Value
if ($x -eq $prevMain) { Fail 'commit-not-merged' "$x is already the main commit of the release tip." }
if (-not (GOk merge-base --is-ancestor $prevMain $x)) { Fail 'prev-release-commit-ancestor-of-commit' "the previous release's main commit $prevMain (named by the release tip) is not an ancestor of $x." }
if (-not (Test-TagValid $Tag)) { Fail 'tag-version-regex' "tag '$Tag' fails check_release_version_regex's rules (gen_build_info.cmake)." }
if (G tag --list $Tag) { Fail 'tag-free' "tag $Tag already exists locally." }
$rt = @(G ls-remote --tags origin "refs/tags/$Tag")
if ($rc -ne 0) { Fail 'tag-free' 'git ls-remote --tags origin failed (cannot prove the tag is free).' }
if ($rt.Count -gt 0) { Fail 'tag-free' "tag $Tag already exists on origin." }
$onRelease = @(G rev-list --first-parent $relTip) | ForEach-Object { [regex]::Match(((G log -1 --format=%B $_) -join "`n"), $NamedRe) } | Where-Object { $_.Success } | ForEach-Object { $_.Groups[2].Value }
foreach ($t in $onRelease) {
    if ($t -cmatch '^v\d+\.\d+\.\d+(-.+)?$' -and (Compare-Semver $Tag $t) -le 0) {
        Fail 'tag-semver-newer' "tag $Tag is not semver-newer than $t, already on release."
    }
}
# 3. build M with plumbing
$tree = (G rev-parse "$x^{tree}")
$subject = if ($Message) { ($Message -split "`r?`n")[0] } else { "Release $Tag" }
$body = "Release $Tag`n`n$subject`n`nFiles identical to main commit $x (tag $Tag).`n`n$Trailer`n"
$tmp = [IO.Path]::GetTempFileName()
[IO.File]::WriteAllText($tmp, $body, (New-Object Text.UTF8Encoding($false)))
$m = (G commit-tree $tree -p $relTip -F $tmp)
Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
if ($rc -ne 0 -or -not $m) { Fail 'commit-tree' 'git commit-tree failed.' }

# 4. verify
if ((G rev-parse "$m^{tree}") -ne (G rev-parse "$x^{tree}")) { Fail 'tree-equals-commit' 'merge tree differs from the commit tree.' }
if ((G rev-parse "$m^1") -ne $relTip) { Fail 'first-parent' 'first parent is not origin/release.' }
if (@((G rev-list --parents -n 1 $m) -split ' ').Count -ne 2) { Fail 'single-parent' 'new commit does not have exactly one parent.' }
Write-Host "merge commit $m  (tree == $x^{tree}, sole parent origin/release $relTip)"
$tagMsg = if ($Message) { $Message } else { "Release $Tag" }

if (-not $Push) {
    Write-Host "DRY RUN: nothing pushed. -Push would fast-forward origin/release to $m, then create and push annotated tag $Tag on it."
} else {
    G push origin "${m}:refs/heads/release" | Out-Null
    if ($rc -ne 0) { Fail 'push-release' 'plain fast-forward push of release was rejected (never forced).' }
    G tag -a $Tag $m -m $tagMsg | Out-Null
    if ($rc -ne 0) { Fail 'tag-create' "could not create tag $Tag on $m (release already pushed; create and push the tag by hand)." }
    G push origin "refs/tags/$Tag" | Out-Null
    if ($rc -ne 0) { Fail 'push-tag' "push of tag $Tag failed (release already pushed)." }
    G fetch --quiet origin --tags | Out-Null
    if ((G rev-parse 'refs/remotes/origin/release') -ne $m) { Fail 'verify-release' 'origin/release != the new merge after re-fetch.' }
    $peeled = @(G ls-remote origin "refs/tags/$Tag^{}")
    if (-not ($peeled -and $peeled[0] -match "^$m\s")) { Fail 'verify-tag' "origin tag $Tag does not peel to $m." }
    Write-Host "pushed and verified: origin/release == $m, tag $Tag peels to it."
}
Write-Host ""
Write-Host "release first-parent log:"
if ($Push) { G log --first-parent --oneline refs/remotes/origin/release } else { G log --first-parent --oneline $m }
exit 0
