# check_system_uart_bridge_stack_budget.ps1 -- thin wrapper so
# run_all_checks.ps1's check_*.ps1 glob picks up
# check_system_uart_bridge_stack_budget.py.
#
# The measurement, why this task (not a fraction-of-stack budget but a
# ceiling, same reasoning as check_httpd_task_stack_budget.ps1), and its
# stated limits all live in the Python script's module docstring. Short
# version: 2026-09-08's stack-margin audit found system_uart_bridge LOW on
# hardware (892 B free of a 3072 B internal-DRAM stack) with no ELF-based
# regression check -- this is that check, rooted at system_bridge_task.
#
# SKIPS (exit 3, run_all_checks.ps1's reserved SKIP status -- never exit 0)
# when there is no build/KilnCtrl.elf or no Xtensa objdump.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\check_system_uart_bridge_stack_budget.ps1
#   ... -ElfPath <path>            # point at another build (e.g. a worktree)
#   ... -CeilingBytes 1500         # negative test: force the ceiling down

param(
    [string]$ElfPath,
    [int]$CeilingBytes = 0
)

$ErrorActionPreference = "Stop"
$script = Join-Path $PSScriptRoot "check_system_uart_bridge_stack_budget.py"
if (-not (Test-Path $script)) {
    Write-Host "check_system_uart_bridge_stack_budget: FAIL -- $script is missing"
    exit 1
}

$argv = @($script)
if ($ElfPath) { $argv += @("--elf", $ElfPath) }
if ($CeilingBytes -gt 0) { $argv += @("--ceiling-bytes", "$CeilingBytes") }

& python @argv
exit $LASTEXITCODE
