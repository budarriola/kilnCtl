# check_httpd_task_stack_budget.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob picks up check_httpd_task_stack_budget.py.
#
# The measurement, the incident it came from, and its stated limits all live
# in the Python script's module docstring. Short version: the deepest
# statically-reachable stack path from ANY registered httpd URI handler,
# summed out of the built ELF, must stay under a fixed ceiling -- httpd_worker
# was measured CRITICAL (632 B free of 8192 B) on hardware 2026-09-08 after a
# day of handler additions, one of which (api_setup_progress_get_handler)
# put ~2.7 KB of locals directly on this shared task's stack.
#
# SKIPS (exit 3, run_all_checks.ps1's reserved SKIP status -- never exit 0)
# when there is no build/KilnCtrl.elf or no Xtensa objdump: this check
# measures a build artifact. Run `build_kilnfw` first for it to mean anything.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\check_httpd_task_stack_budget.ps1
#   ... -ElfPath <path>            # point at another build (e.g. a worktree)
#   ... -CeilingBytes 100          # negative test: force the ceiling down

param(
    [string]$ElfPath,
    [int]$CeilingBytes = 0
)

$ErrorActionPreference = "Stop"
$script = Join-Path $PSScriptRoot "check_httpd_task_stack_budget.py"
if (-not (Test-Path $script)) {
    Write-Host "check_httpd_task_stack_budget: FAIL -- $script is missing"
    exit 1
}

$argv = @($script)
if ($ElfPath) { $argv += @("--elf", $ElfPath) }
if ($CeilingBytes -gt 0) { $argv += @("--ceiling-bytes", "$CeilingBytes") }

. (Join-Path $PSScriptRoot "lib_skip_fast.ps1")
exit (Invoke-CheckPythonWithSkipFast -PyArgs $argv -ExplicitElf ([bool]$ElfPath))
