# check_uart_log_bridge_stack_budget.ps1 -- thin wrapper so
# run_all_checks.ps1's check_*.ps1 glob picks up
# check_uart_log_bridge_stack_budget.py.
#
# The measurement, why this task (a ceiling, not a fraction-of-stack
# budget -- same reasoning as check_httpd_task_stack_budget.ps1 and
# check_system_uart_bridge_stack_budget.ps1), the fix applied, and its
# stated limits all live in the Python script's module docstring. Short
# version: 2026-09-08's stack-margin registration pass found
# uart_log_bridge -- the log delivery path -- the only LOW task in the
# whole registry (1080 B free of a 4096 B stack, 26.4%). This is that
# check, rooted at uart_log_bridge_task.
#
# SKIPS (exit 3, run_all_checks.ps1's reserved SKIP status -- never exit 0)
# when there is no build/KilnCtrl.elf or no Xtensa objdump.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\check_uart_log_bridge_stack_budget.ps1
#   ... -ElfPath <path>            # point at another build (e.g. a worktree)
#   ... -CeilingBytes 1500         # negative test: force the ceiling down

param(
    [string]$ElfPath,
    [int]$CeilingBytes = 0
)

$ErrorActionPreference = "Stop"
$script = Join-Path $PSScriptRoot "check_uart_log_bridge_stack_budget.py"
if (-not (Test-Path $script)) {
    Write-Host "check_uart_log_bridge_stack_budget: FAIL -- $script is missing"
    exit 1
}

$argv = @($script)
if ($ElfPath) { $argv += @("--elf", $ElfPath) }
if ($CeilingBytes -gt 0) { $argv += @("--ceiling-bytes", "$CeilingBytes") }

. (Join-Path $PSScriptRoot "lib_skip_fast.ps1")
exit (Invoke-CheckPythonWithSkipFast -PyArgs $argv -ExplicitElf ([bool]$ElfPath))
