# lib_check_discovery.ps1 -- the ONE definition of "which check_*.ps1 scripts does the suite run".
# Shared by tools/run_all_checks.ps1 (runs them) and tools/check_build_gate_usage.ps1 (lints them), so a script the suite
# runs can never be invisible to the lint (REVIEW_DEVBREAK5 LOW-3: a gitignored build_agent/check_*.ps1 ran but was
# not linted because the lint enumerated with git alone).
function Get-KilnCheckScripts {
    param([Parameter(Mandatory)][string]$RepoRoot)
    Get-ChildItem -Path $RepoRoot -Filter "check_*.ps1" -Recurse -File |
        Where-Object {
            $_.FullName -notmatch '\\build\\' -and
            $_.FullName -notmatch '\\node_modules\\' -and
            # Untracked run archives (logs\wt_archive_*) hold stale copies of check_*.ps1.
            $_.FullName.Substring($RepoRoot.Length) -notmatch '^\\logs\\' -and
            # Any dotted directory: .venv, .git, .claude\worktrees\ (leftover per-agent copies of the whole tree).
            $_.FullName -notmatch '\\\.[^\\]+\\' -and
            # Needs the network (git ls-remote); not a standing check. land.ps1 and dev_promote.ps1 run it before pushing.
            $_.Name -ne 'check_submodule_pins_pushed.ps1'
        } |
        Sort-Object FullName
}
