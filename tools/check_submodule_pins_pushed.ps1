# check_submodule_pins_pushed.ps1 -- fail when a submodule pin (the gitlink sha in
# <Commit>) names a commit that does not exist on the submodule's remote, so a
# fresh clone of the landed commit could not check the submodule out.
#
# For every submodule in .gitmodules: PASS if the pinned sha is the tip of any remote
# ref (git ls-remote); else PASS if `git fetch --depth 1 <url> <sha>` succeeds in a
# scratch bare repo; else FAIL. Unreachable remote -> SKIP (exit 3), never PASS.
#
# Needs the network, so it is NOT checkcache-able (no "checkcache: ok" marker) and is
# excluded from run_all_checks.ps1's glob; tools/land.ps1 and tools/dev_promote.ps1 run
# it before pushing.
#
# Exit: 0 PASS, 1 FAIL, 3 SKIP (network unreachable).
[CmdletBinding()]
param(
    [string]$RepoPath = (Split-Path -Parent $PSScriptRoot),
    [string]$Commit = 'HEAD'
)
$ErrorActionPreference = 'Continue'

$cfg = & git -C $RepoPath config --file .gitmodules --get-regexp '^submodule\..*\.(path|url)$' 2>$null
if ($LASTEXITCODE -ne 0 -or -not $cfg) { Write-Host "PASS: no submodules in .gitmodules"; exit 0 }
$mods = @{}
foreach ($line in $cfg) {
    if ($line -match '^submodule\.(.+)\.(path|url)\s+(.+)$') {
        if (-not $mods.ContainsKey($Matches[1])) { $mods[$Matches[1]] = @{} }
        $mods[$Matches[1]][$Matches[2]] = $Matches[3].Trim()
    }
}

$scratch = Join-Path ([IO.Path]::GetTempPath()) ("subpins_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
$failed = 0; $skipped = 0
try {
    foreach ($name in ($mods.Keys | Sort-Object)) {
        $path = $mods[$name]['path']; $url = $mods[$name]['url']
        $tree = & git -C $RepoPath ls-tree $Commit -- $path 2>$null
        if (-not $tree -or $tree -notmatch '^160000 commit ([0-9a-f]{40})\t') {
            Write-Host "NOTE: $path has no gitlink in $Commit; nothing to check"
            continue
        }
        $sha = $Matches[1]
        $refs = & git ls-remote $url 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host "SKIP: $path -- cannot reach $url ($(($refs | Out-String).Trim()))" -ForegroundColor Yellow
            $skipped++; continue
        }
        $tips = @($refs | ForEach-Object { ($_ -split "\s+")[0] })
        if ($tips -contains $sha) { Write-Host "PASS: $path $sha is a remote tip"; continue }
        if (-not (Test-Path $scratch)) { & git init --bare -q $scratch 2>&1 | Out-Null }
        & git -C $scratch fetch --depth 1 $url $sha 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) { Write-Host "PASS: $path $sha is fetchable from $url"; continue }
        Write-Host "FAIL: $path pins $sha, not on $url" -ForegroundColor Red
        Write-Host "      push the submodule commit to its remote before landing" -ForegroundColor Red
        $failed++
    }
} finally {
    if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue }
}
if ($failed -gt 0) { exit 1 }
if ($skipped -gt 0) { Write-Host "SKIP: $skipped submodule(s) unchecked (network); this is not a PASS"; exit 3 }
Write-Host "PASS: all submodule pins are on their remotes"
exit 0
