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
$tailRe = '(?s)\$verdict = Get-HostTestVerdict[^\r\n]*\r?\n\s*if \(\$verdict\.Mismatch\) \{ Write-Host \$verdict\.MismatchText \}\r?\n\s*if \(\$verdict\.ExitCode -ne 0\) \{ exit \$verdict\.ExitCode \}\r?\n\s*if \(\$script:auxBuildFailures\.Count -gt 0\) \{ exit 1 \}\r?\n\s*\r?\n\s*Write-Host "all \$totalExpected host test executables built and passed"\r?\n\s*exit 0'
Assert-True ([regex]::IsMatch($src, $tailRe)) "verdict ExitCode reaches exit before the success exit 0"

# REVIEW_DEVBREAKFX11 LOW-2: no bypass elsewhere in the file (AST/token based, so comments and strings are ignored).
$tokens = $null; $perr = $null
$ast = [System.Management.Automation.Language.Parser]::ParseInput($src, [ref]$tokens, [ref]$perr)
Assert-True ($perr.Count -eq 0) "build_host_tests.ps1 parses"
$lines = $src -split "`r?`n"
$exitLines = @($tokens | Where-Object { $_.Kind -eq 'Exit' } | ForEach-Object { $lines[$_.Extent.StartLineNumber - 1].Trim() })
$wantExits = @('exit 1', 'if ($verdict.ExitCode -ne 0) { exit $verdict.ExitCode }', 'if ($script:auxBuildFailures.Count -gt 0) { exit 1 }', 'exit 0')
Assert-True (($exitLines -join '|') -eq ($wantExits -join '|')) "exactly the expected exit statements, in order (got: $($exitLines -join ' | '))"
$noMatch = '(?s)if \(\$script:onlySelected\.Count -eq 0\) \{[^}]*?matched no host-test executable"\r?\n\s*exit 1\r?\n\s*\}'
Assert-True ([regex]::IsMatch($src, $noMatch)) "-Only no-match path exits 1"
$tryAst = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.TryStatementAst] -and $n.Finally -ne $null }, $true)
$finStmts = @($tryAst.Finally.Statements | ForEach-Object { $_.Extent.Text.Trim() })
Assert-True ($finStmts.Count -eq 1 -and $finStmts[0] -eq 'Exit-BuildLock -Lock $buildLock') "outer finally holds only the lock release"
$fns = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-HostTestVerdict' }, $true))
Assert-True ($fns.Count -eq 0) "Get-HostTestVerdict is not redefined in build_host_tests.ps1"
$te = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$totalExpected' }, $true) | ForEach-Object { $_.Right.Extent.Text.Trim() })
Assert-True (($te -join '|') -eq '99|$script:onlySelected.Count') "`$totalExpected is only the literal 99 or the -Only selection count (got: $($te -join ' | '))"
foreach ($v in 'failedExes', 'builtExes') {
    $ap = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq "`$script:$v" -and $n.Operator -eq 'PlusEquals' }, $true))
    Assert-True ($ap.Count -eq 1) "exactly one append to `$script:$v"
}
$fe = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$script:failedExes' -and $n.Operator -eq 'PlusEquals' }, $true))
Assert-True ($fe.Count -eq 1 -and $fe[0].Right.Extent.Text.Trim() -eq '$Name') "failedExes append is `$Name"
$cl = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text -eq '$script:buildFailures' -and $n.Operator -eq 'PlusEquals' }, $true) | ForEach-Object { $_.Right.Extent.Text.Trim() })
Assert-True (-not ($cl -contains '"sim_credibility_gate_closedloop"')) "closedloop compile failure is not in buildFailures (LOW-1)"
Assert-True ($src -match '\$script:auxBuildFailures \+= "sim_credibility_gate_closedloop"') "closedloop compile failure goes to auxBuildFailures (LOW-1)"

if ($failures.Count -gt 0) { Write-Host "check_host_test_verdict: FAIL ($($failures.Count))"; exit 1 }
Write-Host "check_host_test_verdict: PASS"; exit 0

