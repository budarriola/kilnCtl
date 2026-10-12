# check_host_test_verdict.ps1 -- unit check for tools/host_test_verdict.ps1,
# the summary verdict of firmware/KilnFW/App/test/build_host_tests.ps1.
# Key case: a dropped executable (Built < Expected) must report MISMATCH and
# exit nonzero even while another executable FAILS (it used to be hidden).
# Contract: exit 0 PASS, anything else FAIL.
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "host_test_verdict.ps1")
$failures = @()
function Assert-True { param([bool]$Cond, [string]$What) if (-not $Cond) { $script:failures += $What; Write-Host "  FAIL: $What" } else { Write-Host "  ok:   $What" } }

$v = Get-HostTestVerdict -Built 99 -Expected 99 -BuildFailures 0 -RunFailures 0
Assert-True ((-not $v.Mismatch) -and $v.ExitCode -eq 0) "all built, none failed: pass"
$v = Get-HostTestVerdict -Built 98 -Expected 99 -BuildFailures 0 -RunFailures 0
Assert-True ($v.Mismatch -and $v.ExitCode -ne 0 -and $v.MismatchText -match '^MISMATCH: 98 ') "dropout alone: MISMATCH and nonzero"
$v = Get-HostTestVerdict -Built 98 -Expected 99 -BuildFailures 0 -RunFailures 1
Assert-True ($v.Mismatch -and $v.ExitCode -ne 0 -and $v.MismatchText -match '^MISMATCH: 98 ') "dropout while a test fails: MISMATCH still reported, nonzero"
$v = Get-HostTestVerdict -Built 99 -Expected 99 -BuildFailures 0 -RunFailures 1
Assert-True ((-not $v.Mismatch) -and $v.ExitCode -ne 0) "failure without dropout: no MISMATCH, nonzero"
$v = Get-HostTestVerdict -Built 1 -Expected 2 -BuildFailures 1 -RunFailures 0
Assert-True ($v.ExitCode -ne 0 -and -not $v.Mismatch) "build failure under -Only: nonzero, not a MISMATCH"
$v = Get-HostTestVerdict -Built 98 -Expected 99 -BuildFailures 1 -RunFailures 0
Assert-True ((-not $v.Mismatch) -and $v.ExitCode -ne 0) "one build failure, no dropout: no false MISMATCH, nonzero"
$v = Get-HostTestVerdict -Built 97 -Expected 99 -BuildFailures 1 -RunFailures 0
Assert-True ($v.Mismatch -and $v.ExitCode -ne 0 -and $v.MismatchText -match "97 built \+ 1 build failures") "build failure plus a dropout: MISMATCH names both"
$v = Get-HostTestVerdict -Built 1 -Expected 2 -BuildFailures 0 -RunFailures 0
Assert-True ($v.Mismatch -and $v.ExitCode -ne 0 -and $v.MismatchText -match "^MISMATCH") "dropout under -Only: MISMATCH line and nonzero (INFO-2)"

# Wiring (REVIEW_DEVBREAKFX10 LOW-1): the call site in build_host_tests.ps1.
$bht = Join-Path $PSScriptRoot "..\firmware\KilnFW\App\test\build_host_tests.ps1"
$src = Get-Content $bht -Raw
$dot = [regex]::Matches($src, '(?m)^\s*\.\s+\(Join-Path \$PSScriptRoot "\.\./\.\./\.\./\.\./tools/host_test_verdict\.ps1"\)\s*$')
Assert-True ($dot.Count -eq 1) "build_host_tests.ps1 dot-sources host_test_verdict.ps1 exactly once"
$callRe = '(?m)^\s*\$verdict = Get-HostTestVerdict -Built \$script:builtExes\.Count -Expected \$totalExpected -BuildFailures \$script:buildFailures\.Count -RunFailures \$script:failedExes\.Count\s*$'
Assert-True ([regex]::Matches($src, $callRe).Count -eq 1) "verdict assigned once, straight from Get-HostTestVerdict with the real counters"
Assert-True ([regex]::Matches($src, '\$verdict\s*=(?!\s*Get-HostTestVerdict)').Count -eq 0) "no other assignment to `$verdict"
$tailRe = '(?s)\$verdict = Get-HostTestVerdict[^\r\n]*\r?\n\s*if \(\$verdict\.Mismatch\) \{ Write-Host \$verdict\.MismatchText \}\r?\n\s*if \(\$verdict\.ExitCode -ne 0\) \{ exit \$verdict\.ExitCode \}\r?\n\s*if \(\$script:auxBuildFailures\.Count -gt 0\) \{ exit 1 \}\r?\n\s*\r?\n\s*Write-Host "all \$totalExpected host test executables built and passed"\r?\n\s*exit 0'
Assert-True ([regex]::IsMatch($src, $tailRe)) "verdict ExitCode reaches exit before the success exit 0"

# REVIEW_DEVBREAKFX11 LOW-2 / REVIEW_DEVBREAKFX12 LOW-1..3, INFO-1: no bypass elsewhere in the file
# (AST/token based, so comments and strings are ignored).
$AstNs = [System.Management.Automation.Language.Ast].Assembly
$tokens = $null; $perr = $null
$ast = [System.Management.Automation.Language.Parser]::ParseInput($src, [ref]$tokens, [ref]$perr)
Assert-True ($perr.Count -eq 0) "build_host_tests.ps1 parses"
$lines = $src -split "`r?`n"
function Norm-Name { param([string]$n) $n -replace '^(?i:script|global|local|private):', '' }
function Find-All { param($node, [scriptblock]$pred) @($node.FindAll($pred, $true)) }
$exitLines = @($tokens | Where-Object { $_.Kind -eq 'Exit' } | ForEach-Object { $lines[$_.Extent.StartLineNumber - 1].Trim() })
$wantExits = @('exit 1', 'if ($verdict.ExitCode -ne 0) { exit $verdict.ExitCode }', 'if ($script:auxBuildFailures.Count -gt 0) { exit 1 }', 'exit 0')
Assert-True (($exitLines -join '|') -eq ($wantExits -join '|')) "exactly the expected exit statements, in order (got: $($exitLines -join ' | '))"
$noMatch = '(?s)if \(\$script:onlySelected\.Count -eq 0\) \{[^}]*?matched no host-test executable"\r?\n\s*exit 1\r?\n\s*\}'
Assert-True ([regex]::IsMatch($src, $noMatch)) "-Only no-match path exits 1"

# Outer try: top-level try/finally (parent is the named block) whose finally releases the build lock.
$outerTries = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.TryStatementAst] -and $n.Finally -ne $null -and $n.Parent -is [System.Management.Automation.Language.NamedBlockAst] -and $n.Finally.Extent.Text -match "Exit-BuildLock" })
Assert-True ($outerTries.Count -eq 1) "exactly one top-level try/finally releasing the build lock"
$tryAst = $outerTries[0]
$finStmts = @($tryAst.Finally.Statements | ForEach-Object { $_.Extent.Text.Trim() })
Assert-True ($finStmts.Count -eq 1 -and $finStmts[0] -eq 'Exit-BuildLock -Lock $buildLock') "outer finally holds only the lock release"
Assert-True ($tryAst.CatchClauses.Count -eq 0) "outer try has no catch clause (a catch would turn a thrown error into exit 0) (LOW-3)"
Assert-True ((Find-All $ast { param($n) $n -is [System.Management.Automation.Language.TrapStatementAst] }).Count -eq 0) "no trap statement (LOW-3)"

# Script-scope break/continue/return terminate the script with exit 0.
function Has-Ancestor { param($node, [scriptblock]$pred) $p = $node.Parent; while ($p) { if (& $pred $p) { return $true }; $p = $p.Parent }; $false }
$LoopT = [System.Management.Automation.Language.LoopStatementAst]; $SwT = [System.Management.Automation.Language.SwitchStatementAst]
$FnT = [System.Management.Automation.Language.FunctionDefinitionAst]; $SbT = [System.Management.Automation.Language.ScriptBlockExpressionAst]
$badBC = @(Find-All $ast { param($n) ($n -is [System.Management.Automation.Language.BreakStatementAst] -or $n -is [System.Management.Automation.Language.ContinueStatementAst]) } | Where-Object { -not (Has-Ancestor $_ { param($a) $a -is $LoopT -or $a -is $SwT -or $a -is $FnT }) })
Assert-True ($badBC.Count -eq 0) "every break/continue has a loop, switch or function ancestor (LOW-3) (bad: $(@($badBC | % { $_.Extent.StartLineNumber }) -join ','))"
$badRet = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.ReturnStatementAst] } | Where-Object { -not (Has-Ancestor $_ { param($a) $a -is $FnT -or $a -is $SbT }) })
Assert-True ($badRet.Count -eq 0) "every return has a function or scriptblock ancestor (LOW-3) (bad: $(@($badRet | % { $_.Extent.StartLineNumber }) -join ','))"

# INFO-1: shadowing / non-keyword exits.
$fns = @(Find-All $ast { param($n) $n -is $FnT -and (Norm-Name $n.Name) -ieq 'Get-HostTestVerdict' })
Assert-True ($fns.Count -eq 0) "Get-HostTestVerdict is not redefined in build_host_tests.ps1 (scope prefix stripped)"
$envExit = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.InvokeMemberExpressionAst] -and $n.Member.Extent.Text -match '^(?i:Exit|SetShouldExit)$' })
Assert-True ($envExit.Count -eq 0) "no [Environment]::Exit / SetShouldExit call"
$aliasCmd = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.CommandAst] -and "$($n.GetCommandName())" -match '^(?i:Set-Alias|New-Alias|sal|nal)$' })
Assert-True ($aliasCmd.Count -eq 0) "no Set-Alias/New-Alias command that could shadow the verdict function"
$fnDrive = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.VariableExpressionAst] -and $n.VariablePath.UserPath -match '^(?i:function:|alias:)' })
Assert-True ($fnDrive.Count -eq 0) '[${function:...}] / alias: drive access is not used'
$varCmd = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.CommandAst] -and "$($n.GetCommandName())" -match '^(?i:Set-Variable|New-Variable|Clear-Variable|Remove-Variable|sv|clv)$' })
Assert-True ($varCmd.Count -eq 0) "no Set/New/Clear/Remove-Variable (could rewrite the counters by name)"

# LOW-2: counters pinned by variable name (any scope prefix stripped).
$asg = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] })
function Get-Asg { param([string]$name)
    $asg | Where-Object { $v = @($_.Left.FindAll({ param($x) $x -is [System.Management.Automation.Language.VariableExpressionAst] }, $true)); ($v | Where-Object { (Norm-Name $_.VariablePath.UserPath) -ieq $name }).Count -gt 0 }
}
foreach ($name in 'failedExes', 'builtExes', 'buildFailures', 'auxBuildFailures') {
    $all = @(Get-Asg $name)
    $init = @($all | Where-Object { $_.Operator -eq 'Equals' })
    Assert-True ($init.Count -eq 1 -and $init[0].Right.Extent.Text.Trim() -eq '@()') "exactly one initializer (= @()) of `$$name, no resets (got $($init.Count))"
    $other = @($all | Where-Object { $_.Operator -ne 'Equals' -and $_.Operator -ne 'PlusEquals' })
    Assert-True ($other.Count -eq 0) "`$$name is only initialised or appended to"
}
foreach ($name in 'failedExes', 'builtExes') {
    $ap = @(Get-Asg $name | Where-Object { $_.Operator -eq 'PlusEquals' })
    Assert-True ($ap.Count -eq 1) "exactly one append to `$$name"
}
$fe = @(Get-Asg 'failedExes' | Where-Object { $_.Operator -eq 'PlusEquals' })
Assert-True ($fe.Count -eq 1 -and $fe[0].Right.Extent.Text.Trim() -eq '$Name') "failedExes append is `$Name"
$cl = @(Get-Asg 'buildFailures' | Where-Object { $_.Operator -eq 'PlusEquals' } | ForEach-Object { $_.Right.Extent.Text.Trim() })
Assert-True (-not ($cl -contains '"sim_credibility_gate_closedloop"')) "closedloop compile failure is not in buildFailures (LOW-1)"
$te = @(Get-Asg 'totalExpected')
Assert-True (($te | ForEach-Object { "$($_.Operator):$($_.Right.Extent.Text.Trim())" }) -join '|' -eq 'Equals:99|Equals:$script:onlySelected.Count') "`$totalExpected is only the literal 99 or the -Only selection count, no script: re-assignment (got: $(($te | % { $_.Right.Extent.Text.Trim() }) -join ' | '))"
$os = @(Get-Asg 'onlySelected')
Assert-True (@($os | Where-Object { $_.Operator -eq 'Equals' }).Count -eq 1) "exactly one initializer of `$onlySelected"

# REVIEW_DEVBREAKFX12 LOW-1: the closedloop append, structurally. Exactly one, inside the
# `$LASTEXITCODE -ne 0` clause of its build's if (not the else, not a comment).
$aux = @(Get-Asg 'auxBuildFailures' | Where-Object { $_.Operator -eq 'PlusEquals' })
Assert-True ($aux.Count -eq 1 -and $aux[0].Right.Extent.Text.Trim() -eq '"sim_credibility_gate_closedloop"') "exactly one auxBuildFailures append, of the closedloop name (LOW-1)"
$okBranch = $false
if ($aux.Count -eq 1) {
    $p = $aux[0].Parent; while ($p -and $p -isnot [System.Management.Automation.Language.IfStatementAst]) { $p = $p.Parent }
    if ($p) {
        $c0 = $p.Clauses[0]
        $okBranch = ($c0.Item1.Extent.Text -match '\$LASTEXITCODE\s+-ne\s+0') -and
            ($aux[0].Extent.StartOffset -ge $c0.Item2.Extent.StartOffset) -and ($aux[0].Extent.EndOffset -le $c0.Item2.Extent.EndOffset)
    }
}
Assert-True $okBranch "closedloop auxBuildFailures append sits in the failure clause (`$LASTEXITCODE -ne 0) of its if, not the else (LOW-1)"

if ($failures.Count -gt 0) { Write-Host "check_host_test_verdict: FAIL ($($failures.Count))"; exit 1 }
Write-Host "check_host_test_verdict: PASS"; exit 0