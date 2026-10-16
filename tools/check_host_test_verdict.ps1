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
Assert-True ($v.ExitCode -ne 0 -and -not $v.Mismatch) "build failure with a partial selection: nonzero, not a MISMATCH"
$v = Get-HostTestVerdict -Built 98 -Expected 99 -BuildFailures 1 -RunFailures 0
Assert-True ((-not $v.Mismatch) -and $v.ExitCode -ne 0) "one build failure, no dropout: no false MISMATCH, nonzero"
$v = Get-HostTestVerdict -Built 97 -Expected 99 -BuildFailures 1 -RunFailures 0
Assert-True ($v.Mismatch -and $v.ExitCode -ne 0 -and $v.MismatchText -match "97 built \+ 1 build failures") "build failure plus a dropout: MISMATCH names both"
$v = Get-HostTestVerdict -Built 1 -Expected 2 -BuildFailures 0 -RunFailures 0
Assert-True ($v.Mismatch -and $v.ExitCode -ne 0 -and $v.MismatchText -match "^MISMATCH") "dropout with a partial selection: MISMATCH line and nonzero (INFO-2)"

# Wiring (REVIEW_DEVBREAKFX10 LOW-1): the call site in build_host_tests.ps1.
$bht = Join-Path $PSScriptRoot "..\firmware\KilnFW\App\test\build_host_tests.ps1"
$src = Get-Content $bht -Raw
$dot = [regex]::Matches($src, '(?m)^\s*\.\s+\(Join-Path\s+\$PSScriptRoot\s+"\.\./\.\./\.\./\.\./tools/host_test_verdict\.ps1"\)\s*$')
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

# Outer try: top-level try/finally (parent is the script's EndBlock) whose finally releases the build lock.
$outerTries = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.TryStatementAst] -and $n.Finally -ne $null -and $n.Parent -eq $ast.EndBlock -and $n.Finally.Extent.Text -match "Exit-BuildLock" })
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
# LOW-3 (REVIEW_DEVBREAKFX13): break/continue are dynamically scoped, so a function ancestor is NOT enough;
# a loop or switch must come before any function boundary.
function Has-LoopBeforeFn { param($node) $p = $node.Parent; while ($p) { if ($p -is $LoopT -or $p -is $SwT) { return $true }; if ($p -is $FnT -or $p -is $SbT) { return $false }; $p = $p.Parent }; $false }
$badBC = @(Find-All $ast { param($n) ($n -is [System.Management.Automation.Language.BreakStatementAst] -or $n -is [System.Management.Automation.Language.ContinueStatementAst]) } | Where-Object { -not (Has-LoopBeforeFn $_) })
Assert-True ($badBC.Count -eq 0) "every break/continue has a loop or switch ancestor before any function boundary (LOW-3) (bad: $(@($badBC | % { $_.Extent.StartLineNumber }) -join ','))"
$badRet = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.ReturnStatementAst] } | Where-Object { -not (Has-Ancestor $_ { param($a) $a -is $FnT -or $a -is $SbT }) })
Assert-True ($badRet.Count -eq 0) "every return has a function or scriptblock ancestor (LOW-3) (bad: $(@($badRet | % { $_.Extent.StartLineNumber }) -join ','))"

# INFO-1: shadowing / non-keyword exits.
$fns = @(Find-All $ast { param($n) $n -is $FnT -and (Norm-Name $n.Name) -ieq 'Get-HostTestVerdict' })
Assert-True ($fns.Count -eq 0) "Get-HostTestVerdict is not redefined in build_host_tests.ps1 (scope prefix stripped)"
$envExit = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.InvokeMemberExpressionAst] -and $n.Member.Extent.Text -match '^(?i:Exit|SetShouldExit)$' })
Assert-True ($envExit.Count -eq 0) "no [Environment]::Exit / SetShouldExit call"
$aliasCmd = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.CommandAst] -and "$($n.GetCommandName())" -match '^(?i:Set-Alias|New-Alias|sal|nal)$' })
Assert-True ($aliasCmd.Count -eq 0) "no Set-Alias/New-Alias command that could shadow the verdict function"
$fnDrive = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.VariableExpressionAst] -and $n.VariablePath.UserPath -match '^(?i:function:|alias:|variable:)' })
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
Assert-True (($te | ForEach-Object { "$($_.Operator):$($_.Right.Extent.Text.Trim())" }) -join '|' -eq 'Equals:103|Equals:$script:onlySelected.Count') "`$totalExpected is only the literal 103 or the -Only selection count, no script: re-assignment (got: $(($te | % { $_.Right.Extent.Text.Trim() }) -join ' | '))"
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

# REVIEW_DEVBREAKFX13 MED-1: the code that decides a run failed. Behavioural test of the real runner,
# plus a pin on the condition guarding the failedExes append.
. (Join-Path $PSScriptRoot "host_test_exec.ps1")
$tmpD = Join-Path ([System.IO.Path]::GetTempPath()) ("htv_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmpD | Out-Null
try {
    $b3 = Join-Path $tmpD "exit3.bat"; $b0 = Join-Path $tmpD "exit0.bat"; $bs = Join-Path $tmpD "slow.bat"
    [System.IO.File]::WriteAllText($b3, "@echo off`r`nexit /b 3`r`n")
    [System.IO.File]::WriteAllText($b0, "@echo off`r`nexit /b 0`r`n")
    [System.IO.File]::WriteAllText($bs, "@echo off`r`nping -n 30 127.0.0.1 >nul`r`n")
    $r3 = Invoke-HostTestProcess -Name t3 -ExePath $b3 -TimeoutSec 60 6>$null
    Assert-True ("$r3" -eq "3") "Invoke-HostTestProcess: exit 3 returns 3 (got $r3)"
    $r0 = Invoke-HostTestProcess -Name t0 -ExePath $b0 -TimeoutSec 60 6>$null
    Assert-True ("$r0" -eq "0") "Invoke-HostTestProcess: exit 0 returns 0 (got $r0)"
    $rt = Invoke-HostTestProcess -Name tt -ExePath $bs -TimeoutSec 1 6>$null
    Assert-True ("$rt" -eq "124") "Invoke-HostTestProcess: timeout returns 124 (got $rt)"
    $global:HostTestTimedOut = $null
} finally { Remove-Item -LiteralPath $tmpD -Recurse -Force -ErrorAction SilentlyContinue }
$runFn = @(Find-All $ast { param($n) $n -is $FnT -and $n.Name -eq 'Invoke-HostTestRun' })
Assert-True ($runFn.Count -eq 1) "exactly one Invoke-HostTestRun function"
$okGuard = $false; $okSrc = $false
if ($runFn.Count -eq 1) {
    $fa = @(Find-All $runFn[0] { param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and $n.Operator -eq 'PlusEquals' -and $n.Left.Extent.Text -match 'failedExes' })
    $ee = @(Find-All $runFn[0] { param($n) $n -is [System.Management.Automation.Language.AssignmentStatementAst] -and $n.Left.Extent.Text.Trim() -eq '$exeExit' })
    $okSrc = ($ee.Count -eq 1 -and $ee[0].Operator -eq 'Equals' -and $ee[0].Right.Extent.Text.Trim() -eq 'Invoke-HostTestProcess -Name $Name -ExePath $ExePath -TimeoutSec $TestTimeoutSec')
    if ($fa.Count -eq 1) {
        $ip = $fa[0].Parent; while ($ip -and $ip -isnot [System.Management.Automation.Language.IfStatementAst]) { $ip = $ip.Parent }
        if ($ip -and $ip.Clauses.Count -eq 1 -and $ip.ElseClause -eq $null) {
            $c0 = $ip.Clauses[0]
            $okGuard = ($c0.Item1.Extent.Text.Trim() -eq '$exeExit -ne 0') -and ($fa[0].Extent.StartOffset -ge $c0.Item2.Extent.StartOffset) -and ($fa[0].Extent.EndOffset -le $c0.Item2.Extent.EndOffset)
        }
    }
}
Assert-True $okSrc "`$exeExit is assigned once, from Invoke-HostTestProcess (MED-1)"
Assert-True $okGuard "failedExes append sits in a bare if (`$exeExit -ne 0) with no else (MED-1)"

# REVIEW_DEVBREAKFX13 LOW-1/2/4: dot-source list pinned; deny-list applied to the script and every dot-sourced library.
$dotCmds = @(Find-All $ast { param($n) $n -is [System.Management.Automation.Language.CommandAst] -and $n.InvocationOperator -eq 'Dot' } | ForEach-Object { $_.Extent.Text.Trim() })
$wantDots = @('. (Join-Path $PSScriptRoot "../../../../tools/build_lock.ps1")', '. (Join-Path $PSScriptRoot "../../../../tools/host_test_exec.ps1")', '. (Join-Path $PSScriptRoot "../../../../tools/build_gate.ps1")', '. (Join-Path $PSScriptRoot "../../../../tools/host_test_verdict.ps1")')
Assert-True (($dotCmds -join '|') -eq ($wantDots -join '|')) "exact dot-source list and order (LOW-1) (got: $($dotCmds -join ' | '))"
$libNames = 'build_lock', 'host_test_exec', 'build_gate', 'host_test_verdict'
$units = @(@{ Label = 'build_host_tests.ps1'; Ast = $ast; IsLib = $false })
foreach ($ln in $libNames) {
    $lt = $null; $le = $null
    $la = [System.Management.Automation.Language.Parser]::ParseFile(([IO.Path]::Combine($PSScriptRoot, "$ln.ps1")), [ref]$lt, [ref]$le)
    Assert-True ($le.Count -eq 0) "$ln.ps1 parses"
    $units += @{ Label = "$ln.ps1"; Ast = $la; IsLib = $true; Tokens = $lt }
}
$allFn = @()
foreach ($u in $units) {
    $a = $u.Ast; $L = $u.Label
    $allFn += @(Find-All $a { param($n) $n -is $FnT } | ForEach-Object { (Norm-Name $_.Name).ToLowerInvariant() })
    $cmdBad = @(Find-All $a { param($n) $n -is [System.Management.Automation.Language.CommandAst] -and (
        "$($n.GetCommandName())" -match '^(?i:Invoke-Expression|iex|Get-Variable|gv|Set-Variable|sv|New-Variable|Remove-Variable|Clear-Variable|clv|Set-Alias|New-Alias|sal|nal)$' -or
        @($n.CommandElements | Where-Object { $_.Extent.Text -match '^[''"]?(?i:variable|function|alias):' }).Count -gt 0) })
    Assert-True ($cmdBad.Count -eq 0) "$L : no Invoke-Expression / variable-cmdlets / variable:|function:|alias: provider paths (LOW-2) (bad: $(@($cmdBad | % { $_.Extent.StartLineNumber }) -join ','))"
    $sbCreate = @(Find-All $a { param($n) $n -is [System.Management.Automation.Language.InvokeMemberExpressionAst] -and $n.Member.Extent.Text -match '^(?i:Create)$' -and $n.Expression.Extent.Text -match '(?i)scriptblock' })
    Assert-True ($sbCreate.Count -eq 0) "$L : no [scriptblock]::Create (LOW-2)"
    $ec = @(Find-All $a { param($n) $n -is [System.Management.Automation.Language.VariableExpressionAst] -and $n.VariablePath.UserPath -match '^(?i:ExecutionContext)$|^(?i:variable|function|alias):' -and $n.Extent.Text -ne '${function:Get-KilnBuildGateHoldDecision}' })  # the one known read-only use, build_gate.ps1 (runspace watchdog)
    Assert-True ($ec.Count -eq 0) "$L : no `$ExecutionContext / `${variable:..} (LOW-2)"
    $ps = @(Find-All $a { param($n) $n -is [System.Management.Automation.Language.MemberExpressionAst] -and $n.Member.Extent.Text -match '^(?i:PSVariable|InvokeCommand|InvokeScript|SessionState)$' })
    Assert-True ($ps.Count -eq 0) "$L : no PSVariable/InvokeScript/SessionState member access (LOW-2)"
    if ($u.IsLib) {
        $ex = @($u.Tokens | Where-Object { $_.Kind -eq 'Exit' })
        Assert-True ($ex.Count -eq 0) "$L : no exit statement (LOW-1)"
        $ee2 = @(Find-All $a { param($n) $n -is [System.Management.Automation.Language.InvokeMemberExpressionAst] -and $n.Member.Extent.Text -match '^(?i:Exit|SetShouldExit)$' } | Where-Object { $_.Arguments.Count -ne 1 -or $_.Arguments[0].Extent.Text -ne '124' })
        Assert-True ($ee2.Count -eq 0) "$L : [Environment]::Exit only with the nonzero 124 (LOW-1)"
        Assert-True ((Find-All $a { param($n) $n -is [System.Management.Automation.Language.CommandAst] -and $n.InvocationOperator -eq 'Dot' }).Count -eq 0) "$L : no dot-source of its own (LOW-1)"
    }
}
$dupFn = @($allFn | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
Assert-True ($dupFn.Count -eq 0) "no function name defined twice across build_host_tests.ps1 and its dot-sourced libraries (LOW-1) (dups: $($dupFn -join ','))"
# LOW-4: no catch clause anywhere in build_host_tests.ps1 (a nested catch swallows a throw and falls through to exit 0).
Assert-True ((Find-All $ast { param($n) $n -is [System.Management.Automation.Language.CatchClauseAst] }).Count -eq 0) "no catch clause anywhere in build_host_tests.ps1 (LOW-4)"

if ($failures.Count -gt 0) { Write-Host "check_host_test_verdict: FAIL ($($failures.Count))"; exit 1 }
Write-Host "check_host_test_verdict: PASS"; exit 0
