# check_host_test_verdict.ps1 -- unit check for tools/host_test_verdict.ps1,
# the summary verdict of firmware/KilnFW/App/test/build_host_tests.ps1.
# Key case: a dropped executable (Built < Expected) must report MISMATCH and
# exit nonzero even while another executable FAILS (it used to be hidden).
# Contract: exit 0 PASS, anything else FAIL.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "host_test_verdict.ps1")
$failures = @()
function Assert-True { param([bool]$Cond, [string]$What) if (-not $Cond) { $script:failures += $What; Write-Host "  FAIL: $What" } else { Write-Host "  ok:   $What" } }

$v = Get-HostTestVerdict -Built 99 -Expected 99 -BuildFailures 0 -RunFailures 0 -OnlyRun $false
Assert-True ((-not $v.Mismatch) -and $v.ExitCode -eq 0) "all built, none failed: pass"
$v = Get-HostTestVerdict -Built 98 -Expected 99 -BuildFailures 0 -RunFailures 0 -OnlyRun $false
Assert-True ($v.Mismatch -and $v.ExitCode -ne 0 -and $v.MismatchText -match '^MISMATCH: 98 ') "dropout alone: MISMATCH and nonzero"
$v = Get-HostTestVerdict -Built 98 -Expected 99 -BuildFailures 0 -RunFailures 1 -OnlyRun $false
Assert-True ($v.Mismatch -and $v.ExitCode -ne 0 -and $v.MismatchText -match '^MISMATCH: 98 ') "dropout while a test fails: MISMATCH still reported, nonzero"
$v = Get-HostTestVerdict -Built 99 -Expected 99 -BuildFailures 0 -RunFailures 1 -OnlyRun $false
Assert-True ((-not $v.Mismatch) -and $v.ExitCode -ne 0) "failure without dropout: no MISMATCH, nonzero"
$v = Get-HostTestVerdict -Built 2 -Expected 2 -BuildFailures 1 -RunFailures 0 -OnlyRun $true
Assert-True ($v.ExitCode -ne 0) "build failure under -Only: nonzero"

if ($failures.Count -gt 0) { Write-Host "check_host_test_verdict: FAIL ($($failures.Count))"; exit 1 }
Write-Host "check_host_test_verdict: PASS"; exit 0
