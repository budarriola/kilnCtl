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
# Exit: 0 PASS, 1 FAIL, 3 SKIP (network unreachable: DNS/connect/timeout only; a bad URL,
# missing repo or auth failure is FAIL). Git runs with prompts disabled and a bounded wait.
[CmdletBinding()]
param(
    [string]$RepoPath = (Split-Path -Parent $PSScriptRoot),
    [string]$Commit = 'HEAD'
)
$ErrorActionPreference = 'Continue'
# Never let git raise a credential prompt or hang: a missing/private repo must FAIL, not block.
$env:GIT_TERMINAL_PROMPT = '0'
$env:GCM_INTERACTIVE = 'never'
$TimeoutSec = 60
if ($env:KILNCTL_SUBPIN_TIMEOUT_SEC) { $TimeoutSec = [int]$env:KILNCTL_SUBPIN_TIMEOUT_SEC }

# Run git with a bounded wait; kill the whole process tree on timeout.
# Returns @{ Exit; Out; TimedOut }.
function Invoke-GitBounded([string[]]$GitArgs) {
    $so = [IO.Path]::GetTempFileName(); $se = [IO.Path]::GetTempFileName()
    try {
        $q = ($GitArgs | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join ' '
        $p = Start-Process -FilePath 'git' -ArgumentList $q -NoNewWindow -PassThru -RedirectStandardOutput $so -RedirectStandardError $se
        $null = $p.Handle
        if (-not $p.WaitForExit($TimeoutSec * 1000)) {
            & taskkill /T /F /PID $p.Id 2>&1 | Out-Null
            return @{ Exit = -1; Out = "timed out after ${TimeoutSec}s"; TimedOut = $true }
        }
        $p.WaitForExit()
        $txt = (Get-Content -Raw -ErrorAction SilentlyContinue $so) + (Get-Content -Raw -ErrorAction SilentlyContinue $se)
        return @{ Exit = $p.ExitCode; Out = "$txt"; TimedOut = $false }
    } finally { Remove-Item -Force -ErrorAction SilentlyContinue $so, $se }
}
# Only a genuine network/DNS/timeout failure is "unreachable"; bad URL / auth / not found is a FAIL.
$netPattern = 'Could not resolve host|Failed to connect|Connection timed out|Connection refused|Network is unreachable|Operation timed out|Connection reset|Recv failure|SSL_ERROR_SYSCALL|Temporary failure in name resolution|Name or service not known|No route to host|unable to connect to'

# Read .gitmodules from the commit being checked, not the working tree.
$treeEntries = & git -C $RepoPath ls-tree -r $Commit 2>$null
if ($LASTEXITCODE -ne 0) { Write-Host "FAIL: cannot read tree of $Commit in $RepoPath" -ForegroundColor Red; exit 1 }
$gitlinks = @($treeEntries | Where-Object { $_ -match '^160000 commit ' })
$cfg = & git -C $RepoPath config --blob "${Commit}:.gitmodules" --get-regexp '^submodule\..*\.(path|url)$' 2>$null
$cfgExit = $LASTEXITCODE
if (-not $cfg) {
    if ($gitlinks.Count -gt 0) {
        Write-Host "FAIL: $Commit has $($gitlinks.Count) gitlink(s) but no readable .gitmodules entries (git exit $cfgExit)" -ForegroundColor Red
        exit 1
    }
    Write-Host "PASS: no submodules at $Commit"; exit 0
}
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
        $r = Invoke-GitBounded @('ls-remote', $url)
        if ($r.Exit -ne 0) {
            $msg = $r.Out.Trim()
            if ($r.TimedOut -or $msg -match $netPattern) {
                Write-Host "SKIP: $path -- cannot reach $url ($msg)" -ForegroundColor Yellow
                $skipped++; continue
            }
            Write-Host "FAIL: $path -- ls-remote $url failed, not a network outage (bad URL, repo not found or auth): $msg" -ForegroundColor Red
            $failed++; continue
        }
        $refs = @($r.Out -split "`r?`n" | Where-Object { $_ })
        $tips = @($refs | ForEach-Object { ($_ -split "\s+")[0] })
        if ($tips -contains $sha) { Write-Host "PASS: $path $sha is a remote tip"; continue }
        if (-not (Test-Path $scratch)) { & git init --bare -q $scratch 2>&1 | Out-Null }
        $f = Invoke-GitBounded @('-C', $scratch, 'fetch', '--depth', '1', $url, $sha)
        if ($f.Exit -eq 0) { Write-Host "PASS: $path $sha is fetchable from $url"; continue }
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
