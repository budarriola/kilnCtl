# lib_skip_fast.ps1 -- shared by the check_*stack_budget*/dram_bss wrappers in
# this directory (dot-source it). Runs the wrapper's python script and, ONLY
# for the one SKIP that is a direct consequence of `run_all_checks.ps1 -Fast`
# (-Fast skips check_00_kilnfw_target_build.ps1, the sole producer of
# build/KilnCtrl.elf), relabels it SKIP-FAST so the runner does not fail the
# run over it. Strict on purpose: the relabel needs exit code 3, the exact
# message "SKIP: no ELF at" (a 0-byte ELF, missing objdump, parse failure etc.
# print other messages), KILNCTL_CHECKS_FAST set, and NO explicit -ElfPath
# (an explicit path that is missing is the caller's mistake, not -Fast).
# The python scripts stay env-agnostic.
function Invoke-CheckPythonWithSkipFast {
    param([string[]]$PyArgs, [bool]$ExplicitElf)
    $output = & python @PyArgs
    $code = $LASTEXITCODE
    if ($code -eq 3 -and $env:KILNCTL_CHECKS_FAST -and -not $ExplicitElf -and
            ($output -join "`n") -match 'SKIP: no ELF at') {
        $output | ForEach-Object { Write-Host ($_ -replace 'SKIP: no ELF at', 'SKIP-FAST: no ELF at') }
        return 3
    }
    $output | Write-Host
    return $code
}
