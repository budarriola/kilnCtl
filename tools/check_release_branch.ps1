# check_release_branch.ps1 -- standing check: origin/release holds only release commits.
# Offline (local refs). SKIPs if refs/remotes/origin/release is absent.
#
# Shape (docs/RELEASING.md "Release branch"): the first-parent chain is an empty-tree root commit and
# then only SINGLE-parent commits. Each release commit R: its message contains the line
# "Files identical to main commit <40-hex X> (tag <T>)."; X is an ancestor of origin/main; tree(R) ==
# tree(X); R carries exactly one v* tag, equal to <T>; successive X's ascend along main. Nothing else
# may be on the branch.
# LEGACY EXEMPTION (v1.0.0-pre.1 ONLY): that release predates the tag-on-release-commit rule, so its
# tag sits on X (bf9ddea2) instead of on R.
[CmdletBinding()]
param(
    [string]$RepoPath = "",
    [string]$ReleaseRef = "refs/remotes/origin/release",
    [string]$MainRef = "refs/remotes/origin/main"
)
$ErrorActionPreference = 'Continue'
$EmptyTree = '4b825dc642cb6eb9a060e54bf8d69288fbee4904'
$LegacyTag = 'v1.0.0-pre.1'
$NamedRe = 'Files identical to main commit ([0-9a-f]{40}) \(tag (v[^\s)]+)\)\.'
if (-not $RepoPath) { $RepoPath = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path) }
function G { $o = & git -C $RepoPath @args; $script:rc = $LASTEXITCODE; return $o }
$fails = New-Object System.Collections.Generic.List[string]
function Note-Fail([string]$m) { $fails.Add($m); Write-Host "FAIL: $m" -ForegroundColor Red }

$tip = G rev-parse --verify --quiet "$ReleaseRef^{commit}"
if ($rc -ne 0 -or -not $tip) { Write-Host "SKIP: $ReleaseRef is absent (git fetch origin release)."; exit 0 }
$mainTip = G rev-parse --verify --quiet "$MainRef^{commit}"
if ($rc -ne 0 -or -not $mainTip) { Write-Host "SKIP: $MainRef is absent."; exit 0 }

$chain = @(G rev-list --first-parent --reverse $tip)
$root = $chain[0]
if (@((G rev-list --parents -n 1 $root) -split ' ').Count -ne 1) { Note-Fail "first-parent root $($root.Substring(0,12)) has a parent; the branch must start at an empty-tree root." }
if ((G rev-parse "$root^{tree}") -ne $EmptyTree) { Note-Fail "first-parent root $($root.Substring(0,12)) tree is not the empty tree." }

$allowed = @{ $root = $true }
$prevX = $null
foreach ($c in $chain | Select-Object -Skip 1) {
    $allowed[$c] = $true
    $short = $c.Substring(0, 12)
    if (@((G rev-list --parents -n 1 $c) -split ' ').Count -ne 2) { Note-Fail "$short does not have exactly one parent."; continue }
    $m = [regex]::Match(((G log -1 --format=%B $c) -join "`n"), $NamedRe)
    if (-not $m.Success) { Note-Fail "$short message has no 'Files identical to main commit <sha> (tag <T>).' line."; continue }
    $x = $m.Groups[1].Value; $named = $m.Groups[2].Value
    G cat-file -e "$x^{commit}" | Out-Null
    if ($rc -ne 0) { Note-Fail "$short names main commit $x, which is not in this clone."; continue }
    $tags = @(G tag --points-at $c | Where-Object { $_ -cmatch '^v' })
    if ($named -ceq $LegacyTag) {
        $xtags = @(G tag --points-at $x | Where-Object { $_ -cmatch '^v' })
        if (-not ($tags.Count -eq 0 -and $xtags -ccontains $LegacyTag)) {
            if (-not ($tags.Count -eq 1 -and $tags[0] -ceq $LegacyTag)) { Note-Fail "$short legacy tag $LegacyTag is on neither the commit nor its named main commit." }
        }
    } elseif (-not ($tags.Count -eq 1 -and $tags[0] -ceq $named)) {
        Note-Fail "$short carries v* tag(s) [$($tags -join ',')], expected exactly one, equal to the message's '$named'."
    }
    G merge-base --is-ancestor $x $mainTip | Out-Null
    if ($rc -ne 0) { Note-Fail "$short names $($x.Substring(0,12)), which is not an ancestor of $MainRef." }
    if ((G rev-parse "$c^{tree}") -ne (G rev-parse "$x^{tree}")) { Note-Fail "$short tree differs from the named main commit's tree." }
    if ($prevX) {
        G merge-base --is-ancestor $prevX $x | Out-Null
        if ($rc -ne 0 -or $prevX -eq $x) { Note-Fail "$short names $($x.Substring(0,12)), which does not ascend from the previous release's $($prevX.Substring(0,12))." }
    }
    $prevX = $x
}
# Nothing else: every commit on release that is not on main must be the root or a chain commit.
foreach ($c in @(G rev-list $tip --not $mainTip)) {
    if (-not $allowed.ContainsKey($c)) { Note-Fail "$($c.Substring(0,12)) is on release but is neither the root, a release commit, nor on origin/main." }
}
if ($fails.Count -gt 0) { Write-Host "check_release_branch: $($fails.Count) failure(s)" -ForegroundColor Red; exit 1 }
Write-Host "check_release_branch: OK ($($chain.Count - 1) release commit(s) after the root)"
exit 0
