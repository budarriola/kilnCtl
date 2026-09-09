# check_all_task_stack_budgets.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob picks up check_all_task_stack_budgets.py.
#
# One script (see its own module docstring for "why one table, not one file
# per task") drives the ELF-call-graph walk (stack_budget_lib.py) for every
# stack_margin_register()-registered KilnFW task that does NOT already have
# a dedicated check_*_task_stack_budget.py of its own -- 28 tasks as of
# 2026-09-09, including safety_poll and lvgl, the two named in the
# 2026-09-04 panic post-mortem (docs/audits/...). See the .py file's
# docstring for the full incident history, the address-keyed-graph fix for
# the real `owner_task` name collision between kiln_io_owner.c and
# thermo_owner.c, and the source-derived (never hand-copied) stack sizes.
#
# NAME/SORT ORDER: run_all_checks.ps1 discovers check_*.ps1 by glob and runs
# them in plain FullName sort order with no other ordering guarantee. This
# file's "check_a..." prefix sorts it after check_00_kilnfw_target_build.ps1
# (which publishes a fresh KilnCtrl.elf before any ELF-reading check runs)
# -- same requirement the five pre-existing check_*_task_stack_budget.ps1
# siblings rely on, unaffected by this new file.
#
# SKIPS (exit 3, run_all_checks.ps1's reserved SKIP status -- never exit 0)
# when there is no build/KilnCtrl.elf or no Xtensa objdump: this check
# measures a build artifact. Run `build_kilnfw` (or
# check_00_kilnfw_target_build.ps1) first for it to mean anything.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\check_all_task_stack_budgets.ps1
#   ... -ElfPath <path>                       # point at another build (e.g. a worktree)
#   ... -ForceCeiling "taskname=bytes"        # negative test: force one task's ceiling down
#   ... -DumpCeilings                         # print a fresh CEILING_BYTES table instead of grading

param(
    [string]$ElfPath,
    [string[]]$ForceCeiling = @(),
    [switch]$DumpCeilings
)

$ErrorActionPreference = "Stop"
$script = Join-Path $PSScriptRoot "check_all_task_stack_budgets.py"
if (-not (Test-Path $script)) {
    Write-Host "check_all_task_stack_budgets: FAIL -- $script is missing"
    exit 1
}

$argv = @($script)
if ($ElfPath) { $argv += @("--elf", $ElfPath) }
if ($DumpCeilings) { $argv += @("--dump-ceilings") }
foreach ($fc in $ForceCeiling) { $argv += @("--force-ceiling", $fc) }

& python @argv
exit $LASTEXITCODE
