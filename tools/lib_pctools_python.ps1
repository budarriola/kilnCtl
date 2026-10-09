# Shared PcTools interpreter resolver (dot-source this file).
#
# WHY: tools\PcTools\.venv is gitignored and per-clone, so a worktree minted by
# tools\worktree_mint.ps1 under C:\wt\ never has one. A check that fell back to
# whatever `python` is on PATH there ran without the PcTools dependencies (e.g.
# check_release_manifest.ps1 failed in every worktree with "the 'cryptography'
# package is required") while passing in the main tree, which made every
# worktree -Fast run show a spurious failure. Every check that wants the
# PcTools dependencies resolves its interpreter through this one function.
#
# No junction is created in the worktree on purpose: a .venv junction into the
# main tree is what let a worktree removal delete the main venv on 2026-10-07
# (see tools\lib_safe_remove.ps1). Borrowing the main tree's interpreter by
# path is read-only and cannot be deleted through the worktree.
#
# Order: the tree's own healthy venv (python.exe AND pyvenv.cfg) ->
# $env:KILNCTL_PCTOOLS_PYTHON -> the main tree's venv (parent of
# `git rev-parse --git-common-dir`) -> python/python3 on PATH (unless
# -NoPathFallback). Returns $null when nothing resolved and PATH was refused or
# empty. A borrowed interpreter still runs THIS tree's scripts: callers pass
# script paths from their own repo root.

function Resolve-PcToolsPython {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [switch]$NoPathFallback
    )
    $own = Join-Path $RepoRoot "tools\PcTools\.venv\Scripts\python.exe"
    $ownCfg = Join-Path $RepoRoot "tools\PcTools\.venv\pyvenv.cfg"
    if ((Test-Path -LiteralPath $own) -and (Test-Path -LiteralPath $ownCfg)) { return $own }
    $envPy = $env:KILNCTL_PCTOOLS_PYTHON
    if (-not [string]::IsNullOrWhiteSpace($envPy) -and (Test-Path -LiteralPath $envPy)) { return $envPy }
    $common = $null
    try { $common = (& git -C $RepoRoot rev-parse --path-format=absolute --git-common-dir 2>$null) } catch { $common = $null }
    if ($LASTEXITCODE -eq 0 -and $common) {
        $mainRoot = Split-Path -Parent ([string]($common | Select-Object -First 1)).Trim()
        $mainPy = Join-Path $mainRoot "tools\PcTools\.venv\Scripts\python.exe"
        $mainCfg = Join-Path $mainRoot "tools\PcTools\.venv\pyvenv.cfg"
        if ((Test-Path -LiteralPath $mainPy) -and (Test-Path -LiteralPath $mainCfg)) { return $mainPy }
    }
    if (-not $NoPathFallback) {
        $cmd = Get-Command python -ErrorAction SilentlyContinue
        if (-not $cmd) { $cmd = Get-Command python3 -ErrorAction SilentlyContinue }
        if ($cmd) { return $cmd.Source }
    }
    return $null
}
