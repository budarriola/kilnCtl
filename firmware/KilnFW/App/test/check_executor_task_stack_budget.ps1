# check_executor_task_stack_budget.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob picks up check_executor_task_stack_budget.py.
#
# The measurement, the incident it came from (2026-09-09 profile_executor
# panic, a stack overflow -- docs/audits/executor_panic_stack_overflow_2026-09-09.md),
# and its stated limits all live in the Python script's module docstring.
# Short version: the deepest statically-reachable stack path from
# `executor_task_entry` (profile_executor's own task entry point), summed out
# of the built ELF, must stay under a fixed ceiling on this task's 4096 B
# stack -- this task had NO budget check at all until this pass, despite being
# registered for stack-margin reporting since before then.
#
# NAME/SORT ORDER: this file's "check_" prefix sorts it after
# check_00_kilnfw_target_build.ps1 (which publishes a fresh KilnCtrl.elf
# before any ELF-reading check runs) and alongside its stack-budget siblings
# (check_httpd_task_stack_budget.ps1, check_main_task_stack_budget.ps1,
# check_system_uart_bridge_stack_budget.ps1,
# check_uart_log_bridge_stack_budget.ps1) in plain FullName sort order, which
# is all run_all_checks.ps1 guarantees.
#
# SKIPS (exit 3, run_all_checks.ps1's reserved SKIP status -- never exit 0)
# when there is no build/KilnCtrl.elf or no Xtensa objdump: this check
# measures a build artifact. Run `build_kilnfw` (or
# check_00_kilnfw_target_build.ps1) first for it to mean anything.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\check_executor_task_stack_budget.ps1
#   ... -ElfPath <path>            # point at another build (e.g. a worktree)
#   ... -CeilingBytes 100          # negative test: force the ceiling down
#   ... -StackBytes 100            # negative test: force the configured stack size down

param(
    [string]$ElfPath,
    [int]$CeilingBytes = 0,
    [int]$StackBytes = 0
)

$ErrorActionPreference = "Stop"
$script = Join-Path $PSScriptRoot "check_executor_task_stack_budget.py"
if (-not (Test-Path $script)) {
    Write-Host "check_executor_task_stack_budget: FAIL -- $script is missing"
    exit 1
}

$argv = @($script)
if ($ElfPath) { $argv += @("--elf", $ElfPath) }
if ($CeilingBytes -gt 0) { $argv += @("--ceiling-bytes", "$CeilingBytes") }
if ($StackBytes -gt 0) { $argv += @("--stack-bytes", "$StackBytes") }

. (Join-Path $PSScriptRoot "lib_skip_fast.ps1")
exit (Invoke-CheckPythonWithSkipFast -PyArgs $argv -ExplicitElf ([bool]$ElfPath))
