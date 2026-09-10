# check_saftyfw_task_count.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob picks up check_saftyfw_task_count.py.
#
# 2026-09-10 (opus review): check_saftyfw_task_count.py existed on disk
# (added alongside check_saftyfw_task_stack_budgets.py, whose TASKS table
# comment claims "check_saftyfw_task_count.py ... fails loud" if a new task
# is added without a row) but had no .ps1 wrapper, unlike its sibling. With
# no wrapper, run_all_checks.ps1's `check_*.ps1` glob (tools/run_all_
# checks.ps1) never found it and it was never executed by anything -- the
# exact defect 5595e5b4 was written to fix, reproduced one level up: the
# file now exists, the sibling's comment is now true on disk, and the check
# still never ran. A tenth SaftyFW task added without a TASKS row would
# still ship with zero stack-budget coverage, silently, and
# run_all_checks.ps1's own "N/N passed" tally would never reflect that this
# cross-check ran at all.
#
# This check is a pure source-text grep (no ELF/objdump dependency,
# see the .py file's module docstring) -- it never legitimately SKIPs.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\SaftyFW\test\check_saftyfw_task_count.ps1

$ErrorActionPreference = "Stop"
$script = Join-Path $PSScriptRoot "check_saftyfw_task_count.py"
if (-not (Test-Path $script)) {
    Write-Host "check_saftyfw_task_count: FAIL -- $script is missing"
    exit 1
}

& python $script
exit $LASTEXITCODE
