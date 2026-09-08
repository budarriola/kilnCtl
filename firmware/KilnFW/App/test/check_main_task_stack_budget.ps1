# check_main_task_stack_budget.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob picks up check_main_task_stack_budget.py.
#
# The measurement, the incident it came from, and its stated limits all live
# in the Python script's module docstring. Short version: the `main` task's
# worst-case stack, summed over the deepest statically-reachable call path
# from app_main out of the built ELF, must stay under a fraction of
# CONFIG_ESP_MAIN_TASK_STACK_SIZE. Flashing 218f65f7 overflowed it 2x and
# panicked the board at boot with an IllegalInstruction and no usable
# backtrace -- docs/audits/boot_hang_2026-09-08.md.
#
# SKIPS (exit 0, loudly) when there is no build/KilnCtrl.elf or no Xtensa
# objdump: this check measures a build artifact, and a checkout that has
# never been built has nothing to measure. Run `build_kilnfw` first for it
# to mean anything.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\check_main_task_stack_budget.ps1
#   ... -ElfPath <path>        # point at another build (e.g. a worktree)
#   ... -StackBytes 4096       # negative test: force the budget down

param(
    [string]$ElfPath,
    [int]$StackBytes = 0
)

$ErrorActionPreference = "Stop"
$script = Join-Path $PSScriptRoot "check_main_task_stack_budget.py"
if (-not (Test-Path $script)) {
    Write-Host "check_main_task_stack_budget: FAIL -- $script is missing"
    exit 1
}

$argv = @($script)
if ($ElfPath) { $argv += @("--elf", $ElfPath) }
if ($StackBytes -gt 0) { $argv += @("--stack-bytes", "$StackBytes") }

& python @argv
exit $LASTEXITCODE
