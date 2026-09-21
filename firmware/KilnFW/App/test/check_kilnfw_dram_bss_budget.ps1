# check_kilnfw_dram_bss_budget.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob picks up check_kilnfw_dram_bss_budget.py.
#
# Grades the size of the built ELF's internal-DRAM .dram0.bss section against a
# fixed ceiling (CEILING_BYTES in the .py). Every byte there is internal heap the
# board never gets: 2026-09-20 a single 42416 B static (profiles_http.c's
# s_profiles_fallback, grown by the 100-slot change) pushed internal free heap to
# 8447 B and the Wi-Fi ppTask aborted on esp_timer_create() == ESP_ERR_NO_MEM.
# See docs/audits/dram_bss_profiles_fallback_2026-09-20.md.
#
# SKIPS (exit 3, run_all_checks.ps1's reserved SKIP status -- never exit 0)
# when there is no build/KilnCtrl.elf or no Xtensa objdump: this check
# measures a build artifact. Run `build_kilnfw` first for it to mean anything.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\check_kilnfw_dram_bss_budget.ps1
#   ... -ElfPath <path>            # point at another build (e.g. a worktree)
#   ... -CeilingBytes 1000         # negative test: force the ceiling down

param(
    [string]$ElfPath,
    [int]$CeilingBytes = 0
)

$ErrorActionPreference = "Stop"
$script = Join-Path $PSScriptRoot "check_kilnfw_dram_bss_budget.py"
if (-not (Test-Path $script)) {
    Write-Host "check_kilnfw_dram_bss_budget: FAIL -- $script is missing"
    exit 1
}

$argv = @($script)
if ($ElfPath) { $argv += @("--elf", $ElfPath) }
if ($CeilingBytes -gt 0) { $argv += @("--ceiling-bytes", "$CeilingBytes") }

& python @argv
exit $LASTEXITCODE
