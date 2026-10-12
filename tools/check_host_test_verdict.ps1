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
$v = Get-HostTestVerdict -Built 1 -Expected 2 -BuildFailures 1 -RunFailures 0 -OnlyRun $true
Assert-True ($v.ExitCode -ne 0 -and -not $v.Mismatch) "build failure under -Only: nonzero, not a MISMATCH"
$v = Get-HostTestVerdict -Built 98 -Expected 99 -BuildFailures 1 -RunFailures 0 -OnlyRun $false
Assert-True ((-not $v.Mismatch) -and $v.ExitCode -ne 0) "one build failure, no dropout: no false MISMATCH, nonzero"
$v = Get-HostTestVerdict -Built 97 -Expected 99 -BuildFailures 1 -RunFailures 0 -OnlyRun $false
Assert-True ($v.Mismatch -and $v.ExitCode -ne 0 -and $v.MismatchText -match "97 built \+ 1 build failures") "build failure plus a dropout: MISMATCH names both"
$v = Get-HostTestVerdict -Built 1 -Expected 2 -BuildFailures 0 -RunFailures 0 -OnlyRun $true
Assert-True ($v.Mismatch -and $v.ExitCode -ne 0 -and $v.MismatchText -match "^MISMATCH") "dropout under -Only: MISMATCH line and nonzero (INFO-2)"

# Wiring (REVIEW_DEVBREAKFX10 LOW-1): the call site in build_host_tests.ps1.
$bht = Join-Path $PSScriptRoot "..\firmware\KilnFW\App\test\build_host_tests.ps1"
$src = Get-Content $bht -Raw
$dot = [regex]::Matches($src, '(?m)^\s*\.\s+\(Join-Path \$PSScriptRoot "\.\./\.\./\.\./\.\./tools/host_test_verdict\.ps1"\)\s*$')
Assert-True ($dot.Count -eq 1) "build_host_tests.ps1 dot-sources host_test_verdict.ps1 exactly once"
$callRe = '(?m)^\s*\$verdict = Get-HostTestVerdict -Built \$script:builtExes\.Count -Expected \$totalExpected -BuildFailures \$script:buildFailures\.Count -RunFailures \$script:failedExes\.Count -OnlyRun \(\[bool\]\$Only\)\s*$'
Assert-True ([regex]::Matches($src, $callRe).Count -eq 1) "verdict assigned once, straight from Get-HostTestVerdict with the real counters"
Assert-True ([regex]::Matches($src, '\$verdict\s*=(?!\s*Get-HostTestVerdict)').Count -eq 0) "no other assignment to `$verdict"
$tailRe = '(?s)\$verdict = Get-HostTestVerdict[^\r\n]*\r?\n\s*if \(\$verdict\.Mismatch\) \{ Write-Host \$verdict\.MismatchText \}\r?\n\s*if \(\$verdict\.ExitCode -ne 0\) \{ exit \$verdict\.ExitCode \}\r?\n\s*\r?\n\s*Write-Host "all \$totalExpected host test executables built and passed"\r?\n\s*exit 0'
Assert-True ([regex]::IsMatch($src, $tailRe)) "verdict ExitCode reaches exit before the success exit 0"

if ($failures.Count -gt 0) { Write-Host "check_host_test_verdict: FAIL ($($failures.Count))"; exit 1 }
Write-Host "check_host_test_verdict: PASS"; exit 0

