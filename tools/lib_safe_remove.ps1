# Junction-safe tree removal (dot-source this file).
#
# WHY: on 2026-10-07 03:46 removing worktree C:\wt\nvsclose_i5kg0h deleted the
# main tree's tools\PcTools\.venv contents (pyvenv.cfg, Lib), breaking the MCP
# servers. That worktree's tools\PcTools\.venv was a JUNCTION to the main
# tree's .venv, and the removal recursed through it. In Windows PowerShell 5.1
# `Remove-Item -Recurse` (and `git worktree remove --force`) can follow
# junctions. Every path that deletes a worktree or a build tree must call
# Remove-TreeSafe instead: it first deletes every reparse point (junction or
# symlink) under the root AS A LINK, never entering it, and only then deletes
# the now link-free tree.

function Remove-ReparsePointsUnder {
    # Deletes each junction/symlink under $Path (and $Path itself if it is
    # one) as a link only. Never descends into a reparse point. Returns the
    # list of removed link paths.
    param([Parameter(Mandatory)][string]$Path)
    $root = [System.IO.Path]::GetFullPath($Path)
    $removed = New-Object System.Collections.Generic.List[string]
    $rp = [System.IO.FileAttributes]::ReparsePoint
    if (-not [System.IO.Directory]::Exists($root) -and -not [System.IO.File]::Exists($root)) { return @() }
    $rootAttr = [System.IO.File]::GetAttributes($root)
    if ($rootAttr -band $rp) {
        if ($rootAttr -band [System.IO.FileAttributes]::Directory) { [System.IO.Directory]::Delete($root, $false) }
        else { [System.IO.File]::Delete($root) }
        $removed.Add($root)
        return ,$removed.ToArray()
    }
    $stack = New-Object System.Collections.Generic.Stack[string]
    $stack.Push($root)
    while ($stack.Count -gt 0) {
        $dir = $stack.Pop()
        $entries = $null
        try { $entries = [System.IO.Directory]::GetFileSystemEntries($dir) } catch { continue }
        foreach ($e in $entries) {
            $attr = $null
            try { $attr = [System.IO.File]::GetAttributes($e) } catch { continue }
            $isDir = [bool]($attr -band [System.IO.FileAttributes]::Directory)
            if ($attr -band $rp) {
                # Delete the LINK. Directory.Delete(path,$false) on a junction or
                # directory symlink removes only the link (RemoveDirectory).
                try {
                    if ($isDir) { [System.IO.Directory]::Delete($e, $false) } else { [System.IO.File]::Delete($e) }
                    $removed.Add($e)
                } catch {
                    # Could not unlink: refuse to let the caller recurse into it.
                    throw "Remove-ReparsePointsUnder: cannot remove link '$e': $($_.Exception.Message)"
                }
            } elseif ($isDir) {
                $stack.Push($e)
            }
        }
    }
    return ,$removed.ToArray()
}

function Remove-TreeSafe {
    # Link-safe replacement for `Remove-Item -Recurse -Force $Path`.
    param([Parameter(Mandatory)][string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not [System.IO.Directory]::Exists($full) -and -not [System.IO.File]::Exists($full)) { return }
    $links = Remove-ReparsePointsUnder -Path $full
    foreach ($l in $links) { Write-Host "unlinked reparse point (target untouched): $l" }
    if ([System.IO.Directory]::Exists($full)) {
        Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue
    }
}
