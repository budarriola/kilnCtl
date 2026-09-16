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
# SKIPS (exit 3, run_all_checks.ps1's reserved SKIP status -- never exit 0)
# when there is no build/KilnCtrl.elf or no Xtensa objdump: this check
# measures a build artifact, and a checkout that has never been built has
# nothing to measure. Run `build_kilnfw` first for it to mean anything. A
# skip must never read as a pass -- see run_all_checks.ps1's header for the
# exit-3 contract this follows.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\check_main_task_stack_budget.ps1
#   ... -ElfPath <path>        # point at another build (e.g. a worktree)
#   ... -StackBytes 4096       # negative test: force the budget down
#   ... -SdkconfigPath <path>  # the config that produced -ElfPath (required
#                              # for an ELF in elf_archive/; otherwise inferred
#                              # from -ElfPath, never from the repo root)
#
# CONFIG_ESP_MAIN_TASK_STACK_SIZE is read from the sdkconfig belonging to the
# ELF being measured, resolved by check_all_task_stack_budgets.py's shared
# ordered resolution. It is NOT read from <repo root>/firmware/KilnFW/sdkconfig,
# which is gitignored and therefore absent in every clean worktree -- that
# hardcoded path made this check fail on an environment gap rather than on any
# real budget violation.

param(
    [string]$ElfPath,
    [int]$StackBytes = 0,
    [string]$SdkconfigPath
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
if ($SdkconfigPath) { $argv += @("--sdkconfig", $SdkconfigPath) }

& python @argv
exit $LASTEXITCODE
