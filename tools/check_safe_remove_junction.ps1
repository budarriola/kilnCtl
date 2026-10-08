# Guards tools\lib_safe_remove.ps1 (2026-10-07 incident: removing worktree
# C:\wt\nvsclose_i5kg0h deleted the main tree's tools\PcTools\.venv through a
# junction). Two parts:
#   1. Behavioural: in a scratch dir, build a target dir with a sentinel file
#      and a fake worktree holding a junction to it; Remove-TreeSafe must
#      delete the worktree and leave the sentinel. A control run of the OLD
#      call (Remove-Item -Recurse -Force) on a second fixture reports whether
#      this PowerShell still follows junctions (informational: it is
#      version-dependent, so it never fails the check).
#   2. Static: every `git worktree remove` COMMAND LINE (a line starting with git, & git, $x = git or
#      $x = & git; not a message string, vacuity audit 2026-10-07) in a tools/ or firmware/ .ps1
#      must be preceded within 6 lines by Remove-ReparsePointsUnder / Remove-TreeSafe.
# Never touches the real .venv: everything lives under a fresh temp dir.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $here
. (Join-Path $here "lib_safe_remove.ps1")

$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("safe_remove_chk_" + [guid]::NewGuid().ToString("N").Substring(0,8))
New-Item -ItemType Directory -Path $scratch | Out-Null
$failures = New-Object System.Collections.Generic.List[string]

function New-Fixture([string]$name) {
    $target = Join-Path $scratch "$name\main_venv"
    $wt = Join-Path $scratch "$name\wt"
    New-Item -ItemType Directory -Path (Join-Path $target "Lib") -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $target "pyvenv.cfg") -Value "sentinel"
    Set-Content -LiteralPath (Join-Path $target "Lib\deep.txt") -Value "sentinel"
    New-Item -ItemType Directory -Path (Join-Path $wt "tools\PcTools") -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $wt "tools\PcTools\file.txt") -Value "x"
    $link = Join-Path $wt "tools\PcTools\.venv"
    cmd /c mklink /J "$link" "$target" | Out-Null
    if (-not ((Get-Item -LiteralPath $link -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "fixture setup: junction was not created"
    }
    return @{ Target = $target; Worktree = $wt; Link = $link }
}

try {
    # --- 1a. new code ---
    $f = New-Fixture "new"
    # Windows PowerShell 5.1's Remove-Item does not follow junctions on every box, so the sentinel
    # alone cannot prove Remove-TreeSafe unlinks first (vacuity audit 2026-10-07: skipping the unlink
    # step still passed). Require the unlink report as well.
    $tsOut = Remove-TreeSafe -Path $f.Worktree 6>&1 | Out-String
    if ($tsOut -notmatch 'unlinked reparse point') { $failures.Add("Remove-TreeSafe did not unlink the junction before deleting (no 'unlinked reparse point' report)") }
    if (Test-Path -LiteralPath $f.Worktree) { $failures.Add("Remove-TreeSafe left the worktree behind") }
    foreach ($rel in @("pyvenv.cfg", "Lib\deep.txt")) {
        if (-not (Test-Path -LiteralPath (Join-Path $f.Target $rel))) {
            $failures.Add("Remove-TreeSafe DELETED junction target content: $rel")
        }
    }

    # --- 1b. nested + direct-link root ---
    $f2 = New-Fixture "root"
    Remove-TreeSafe -Path $f2.Link
    if (Test-Path -LiteralPath $f2.Link) { $failures.Add("Remove-TreeSafe on a junction root did not remove the link") }
    if (-not (Test-Path -LiteralPath (Join-Path $f2.Target "pyvenv.cfg"))) { $failures.Add("Remove-TreeSafe on a junction root deleted its target") }

    # --- 1c/1d. production sequence vs. old call, both through real
    # `git worktree remove --force` (the call that actually followed the
    # junction on 2026-10-07; PowerShell 5.1 Remove-Item did not on this box).
    function New-GitFixture([string]$name) {
        $base = Join-Path $scratch $name
        New-Item -ItemType Directory -Path $base | Out-Null
        $repo = Join-Path $base "repo"
        & git init -q $repo | Out-Null
        & git -C $repo -c user.email=a@b -c user.name=a commit -q --allow-empty -m init | Out-Null
        $wt = Join-Path $base "wt"
        & git -C $repo worktree add -q $wt -b t_$name | Out-Null
        $target = Join-Path $base "main_venv"
        New-Item -ItemType Directory -Path (Join-Path $target "Lib") -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $target "pyvenv.cfg") -Value "sentinel"
        Set-Content -LiteralPath (Join-Path $target "Lib\deep.txt") -Value "sentinel"
        New-Item -ItemType Directory -Path (Join-Path $wt "tools") -Force | Out-Null
        cmd /c mklink /J (Join-Path $wt "tools\.venv") "$target" | Out-Null
        return @{ Repo = $repo; Worktree = $wt; Target = $target }
    }
    $g = New-GitFixture "gitnew"
    Remove-ReparsePointsUnder -Path $g.Worktree | Out-Null
    & git -C $g.Repo worktree remove --force $g.Worktree 2>&1 | Out-Null
    if (Test-Path -LiteralPath $g.Worktree) { $failures.Add("git worktree remove after unlinking left the worktree behind") }
    foreach ($rel in @("pyvenv.cfg", "Lib\deep.txt")) {
        if (-not (Test-Path -LiteralPath (Join-Path $g.Target $rel))) { $failures.Add("production sequence DELETED junction target content: $rel") }
    }
    $c = New-GitFixture "gitold"
    & git -C $c.Repo worktree remove --force $c.Worktree 2>&1 | Out-Null
    $oldKilledTarget = -not (Test-Path -LiteralPath (Join-Path $c.Target "pyvenv.cfg"))
    Write-Host ("control (old bare `git worktree remove --force` on a worktree with a junction): target content " +
        $(if ($oldKilledTarget) { "DELETED -- the hazard is real" } else { "survived on this git version (hazard is version dependent)" }))

    # --- 2. static guard ---
    $sites = 0
    $files = Get-ChildItem -LiteralPath (Join-Path $repoRoot "tools"), (Join-Path $repoRoot "firmware") -Recurse -Filter *.ps1 -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '[\/](build|\.venv|components)[\/]' -and $_.Name -ne "check_safe_remove_junction.ps1" }
    foreach ($file in $files) {
        $lines = @(Get-Content -LiteralPath $file.FullName)  # @(): a one-line file must not index into a string
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match '^\s*(?:\$\w+\s*=\s*)?(?:&\s*)?git\b.*worktree\s+remove\b') {
                $sites++
                $lo = [Math]::Max(0, $i - 6)
                $window = ($lines[$lo..$i] -join "`n")
                if ($window -notmatch 'Remove-ReparsePointsUnder|Remove-TreeSafe') {
                    $failures.Add("$($file.FullName.Substring($repoRoot.Length + 1)):$($i + 1): 'git worktree remove' without a preceding Remove-ReparsePointsUnder/Remove-TreeSafe")
                }
            }
        }
    }
    if ($sites -lt 2) { $failures.Add("static guard found only $sites 'git worktree remove' site(s); expected at least 2 (worktree_mint, check_00_kilnfw_target_build) -- the pattern has gone blind") }
}
finally {
    # Unlink anything left before deleting scratch, so a failure here can never
    # reach outside it.
    try { Remove-TreeSafe -Path $scratch } catch { }
}

if ($failures.Count -gt 0) {
    Write-Host "SAFE-REMOVE JUNCTION CHECK FAILED:" -ForegroundColor Red
    foreach ($m in $failures) { Write-Host "  $m" -ForegroundColor Red }
    exit 1
}
Write-Host "Safe-remove junction check passed: sentinel survived Remove-TreeSafe; $sites worktree-remove site(s) guarded."
exit 0
