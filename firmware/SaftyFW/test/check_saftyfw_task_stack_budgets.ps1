# check_saftyfw_task_stack_budgets.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob picks up check_saftyfw_task_stack_budgets.py.
#
# SaftyFW (RP2040/Cortex-M0+) had NO stack-budget check at all before this
# file, even though this is the fourth stack overflow the project has
# suffered and the 2026-09-09 instance (current_task/discrete_task on bare
# configMINIMAL_STACK_SIZE, fixed in 3afc5ea6) deadlocked the independent
# safety processor -- the guards were not running at all. See the .py
# file's module docstring and stack_budget_lib_arm.py for the full incident
# history and what this Thumb/Cortex-M0+ walk can and cannot measure (it is
# a genuinely different implementation from KilnFW's Xtensa
# stack_budget_lib.py, not a port -- there is no ARM equivalent of `entry
# a1,N`).
#
# SKIPS (exit 3, run_all_checks.ps1's reserved SKIP status -- never exit 0)
# when there is no build/SaftyFW.elf or no arm-none-eabi-objdump: this check
# measures a build artifact.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\SaftyFW\test\check_saftyfw_task_stack_budgets.ps1
#   ... -ElfPath <path>
#   ... -ForceCeiling "taskname=bytes"        # negative test: force one task's ceiling down
#   ... -DumpCeilings

param(
    [string]$ElfPath,
    [string[]]$ForceCeiling = @(),
    [switch]$DumpCeilings
)

$ErrorActionPreference = "Stop"
$script = Join-Path $PSScriptRoot "check_saftyfw_task_stack_budgets.py"
if (-not (Test-Path $script)) {
    Write-Host "check_saftyfw_task_stack_budgets: FAIL -- $script is missing"
    exit 1
}

$argv = @($script)
if ($ElfPath) { $argv += @("--elf", $ElfPath) }
if ($DumpCeilings) { $argv += @("--dump-ceilings") }
foreach ($fc in $ForceCeiling) { $argv += @("--force-ceiling", $fc) }

# SKIP-FAST: -Fast skips check_00_saftyfw_target_build.ps1, the producer of
# build/SaftyFW.elf, so the .py's "SKIP: no ELF at" is an expected consequence
# of -Fast. Relabeled only for that exact message, only when
# KILNCTL_CHECKS_FAST is set and no explicit -ElfPath was given. Any other
# SKIP (e.g. arm-none-eabi-objdump not found) stays a plain SKIP.
$output = & python @argv
$code = $LASTEXITCODE
if ($code -eq 3 -and $env:KILNCTL_CHECKS_FAST -and -not $ElfPath -and
        ($output -join "`n") -match 'SKIP: no ELF at') {
    $output | ForEach-Object { Write-Host ($_ -replace 'SKIP: no ELF at', 'SKIP-FAST: no ELF at') }
    exit 3
}
$output | Write-Host
exit $code
