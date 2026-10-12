# check_push_verify.ps1 -- push_verify.ps1 against a scratch bare origin (never a real remote).
# Covers F7: -Branch is ALWAYS resolved as refs/remotes/<remote>/<branch> after a fetch, so a commit
# stranded on a LOCAL branch named dev (or origin/dev) is never reported LANDED; a pushed commit is;
# a commit pushed by another clone is seen because the script fetches first.
$ErrorActionPreference = "Continue"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:fails = 0
. (Join-Path $here "lib_push_verify_walk.ps1")
function Assert([bool]$c, [string]$w) { if ($c) { Write-Host "  ok: $w" } else { Write-Host "  FAIL: $w" -ForegroundColor Red; $script:fails++ } }
$env:GIT_AUTHOR_NAME = "t"; $env:GIT_AUTHOR_EMAIL = "t@example.invalid"
$env:GIT_COMMITTER_NAME = "t"; $env:GIT_COMMITTER_EMAIL = "t@example.invalid"
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("pushver_chk_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$origin = Join-Path $tmp "origin.git"; $work = Join-Path $tmp "work"; $other = Join-Path $tmp "other"
function Run-PV([string[]]$a) {
    Push-Location $work
    # Bounded: a hung push_verify child is killed after 60 s and reported as rc 99 (FAIL), never hangs the check.
    try {
        $outF = [IO.Path]::GetTempFileName(); $errF = [IO.Path]::GetTempFileName()
        $argl = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$(Join-Path $here 'push_verify.ps1')`"") + ($a | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } })
        $p = Start-Process -FilePath "powershell" -ArgumentList $argl -NoNewWindow -PassThru -RedirectStandardOutput $outF -RedirectStandardError $errF
        $null = $p.Handle
        if (-not $p.WaitForExit(60000)) {
            try { $p.Kill() } catch { }
            $null = $p.WaitForExit(5000)
            $rc = 99; $o = "RUN-PV TIMEOUT after 60s"
        } else {
            $p.WaitForExit()
            $rc = $p.ExitCode
            $o = (Get-Content -LiteralPath $outF -Raw) + (Get-Content -LiteralPath $errF -Raw)
        }
    }
    finally { Pop-Location; Remove-Item -LiteralPath $outF, $errF -Force -ErrorAction SilentlyContinue }
    return [pscustomobject]@{ Rc = $rc; Out = $o }
}
# S3: survivors are polled (up to 10 s, every 250 ms) so a loaded machine does not give a false FAIL; on failure each survivor is printed.
function Get-PvSurvivors([int]$port, [string]$tmpLeaf) {
    @(Get-CimInstance Win32_Process -Filter "Name like 'git%'" -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -match "127\.0\.0\.1:$port" -or $_.CommandLine -match [regex]::Escape($tmpLeaf) })
}
function Assert-NoSurvivors([int]$port, [string]$tmpLeaf, [string]$w) {
    $deadline = [DateTime]::UtcNow.AddSeconds(10); $left = @(Get-PvSurvivors $port $tmpLeaf)
    while ($left.Count -gt 0 -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 250; $left = @(Get-PvSurvivors $port $tmpLeaf) }
    foreach ($s in $left) { Write-Host "  survivor: PID $($s.ProcessId) $($s.Name) $($s.CommandLine)" }
    Assert ($left.Count -eq 0) $w
}
function Commit([string]$repo, [string]$f, [string]$msg) {
    Set-Content -LiteralPath (Join-Path $repo $f) -Value $msg -Encoding ascii
    git -C $repo add $f *>$null; git -C $repo commit -m $msg *>$null
    return (git -C $repo rev-parse HEAD).Trim()
}
# R1: per-parent creation filter, tested on a synthetic snapshot (no process needed).
$t0 = [datetime]'2026-01-01T10:00:00'
function P($id, $ppid, $sec) { [pscustomobject]@{ ProcessId = $id; ParentProcessId = $ppid; CreationDate = $t0.AddSeconds($sec) } }
$snapR1 = @((P 18 12 1.5), (P 10 1 0), (P 11 10 1), (P 12 11 2), (P 13 11 -50), (P 14 12 -40), (P 15 10 0.0005), (P 16 10 -0.0005))
$w = @(Get-PvDescendantOrder -Snap $snapR1 -RootId 10 -RootCreated $t0 | ForEach-Object { $_.Id })
Assert (($w -contains 11) -and ($w -contains 12)) "R1: genuine chain kept"
Assert (-not ($w -contains 13)) "R1: child older than its parent (reused PID) dropped"
Assert (-not ($w -contains 18)) "R1: grandchild newer than the root but older than its own parent dropped"
Assert (-not ($w -contains 14)) "R1: grandchild older than its own parent dropped even though newer than the root"
Assert (($w -contains 15) -and ($w -contains 16)) "R1: root slack is about 1 ms (a child within 1 ms before the root start is kept)"
Assert ($w.IndexOf(11) -lt $w.IndexOf(12)) "R1: parent-first order"
$snapR1b = @((P 10 1 0), (P 17 10 -0.5))
Assert ((Get-PvDescendantOrder -Snap $snapR1b -RootId 10 -RootCreated $t0).Count -eq 0) "R1: child 0.5 s older than the root dropped (no 1 s slack)"
Assert (Test-PvStartMatch -Actual $t0 -Expected $t0.AddMilliseconds(1)) "R2: StartTime within tolerance of the snapshot matches"
Assert (-not (Test-PvStartMatch -Actual $t0.AddMinutes(5) -Expected $t0)) "R2: a reused PID (later StartTime) is skipped, not killed"
Assert (Test-PvStartMatch -Actual $t0 -Expected $t0.AddMilliseconds(1)) "S1: 1 ms difference matches"
Assert (-not (Test-PvStartMatch -Actual $t0 -Expected $t0.AddMilliseconds(5))) "S1: 5 ms difference does not match (tolerance ~2 ms)"
Assert (-not (Test-PvStartMatch -Actual $t0 -Expected $t0.AddMilliseconds(-5))) "S1: -5 ms difference does not match"
# S2: Stop-PvTargets kills only through a checked handle. Sacrificial process: one with a handle is killed, one without (reused-PID stand-in) survives.
$sacA = Start-Process -FilePath "ping" -ArgumentList "-n","40","127.0.0.1" -WindowStyle Hidden -PassThru
$sacB = Start-Process -FilePath "ping" -ArgumentList "-n","40","127.0.0.1" -WindowStyle Hidden -PassThru
try {
    $null = $sacA.Handle; $null = $sacB.Handle
    $hs = Open-PvTargets -Ids @($sacA.Id, $sacB.Id) -Created @{ $sacA.Id = $sacA.StartTime; $sacB.Id = $sacB.StartTime.AddMinutes(-30) }
    Assert ($hs.ContainsKey($sacA.Id) -and -not $hs.ContainsKey($sacB.Id)) "S2: precondition: only the matching-StartTime process gets a handle"
    Stop-PvTargets -Order ([int[]]@($sacA.Id, $sacB.Id)) -Handles $hs
    Assert ($sacA.WaitForExit(5000)) "S2: process with a verified handle is killed"
    Assert (-not $sacB.WaitForExit(1000)) "S2: process whose StartTime differs from the snapshot is NOT killed by bare PID"
} finally { foreach ($sp in @($sacA, $sacB)) { try { if (-not $sp.HasExited) { $sp.Kill() } } catch { } } }
$pvSrc = Get-Content -LiteralPath (Join-Path $here "push_verify.ps1") -Raw
$pvLibSrc = Get-Content -LiteralPath (Join-Path $here "lib_push_verify_walk.ps1") -Raw
# L1 (review LOW-3): kill-surface ALLOWLIST over push_verify.ps1 AND lib_push_verify_walk.ps1 (PowerShell AST, no regex comment
# stripping). Process-kill-capable tokens are permitted only at named sites: [PvJob]::Kill($fetchJob) and [PvJob]::KillProc($fetchH)
# once each in the script, Process.Kill() only inside Stop-PvTargets and Process.GetProcessById only inside Open-PvTargets in the
# lib, and in the C# block TerminateProcess/TerminateJobObject only at pinned call sites. Dynamic command names, process-spawning
# commands (cmd, Start-Process, nested powershell), CIM/WMI method invocation, Get-Process and Invoke-Expression are refused outright.
function Get-LintCount([string]$src, [string]$kind) { $r = Get-KillLintViolations $src $kind; return [int]$r.Count }
function Get-FnOf($n) { $p = $n.Parent; while ($p) { if ($p -is [System.Management.Automation.Language.FunctionDefinitionAst]) { return $p.Name }; $p = $p.Parent }; return '' }
function Get-KillLintViolations([string]$src, [string]$kind) {
    $tok = $null; $err = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseInput($src, [ref]$tok, [ref]$err)
    $v = New-Object System.Collections.ArrayList
    if ($err.Count -gt 0) { [void]$v.Add("parse error: $($err[0].Message)") }
    $bad = '^(stop-process|kill|spps|taskkill(\.exe)?|get-process|gps|ps|start-process|saps|start|invoke-cimmethod|invoke-wmimethod|invoke-expression|iex|invoke-command|icm|remove-ciminstance|remove-wmiobject|cmd(\.exe)?|wmic(\.exe)?|powershell(\.exe)?|pwsh(\.exe)?|stop-computer|start-job|start-threadjob|set-alias|new-alias|sal|nal|get-wmiobject|gwmi|new-ciminstance|invoke-item|ii|add-member)$'
    $addType = 0; $launch = 0
    foreach ($c in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] }, $true)) {
        $nm = $c.GetCommandName()
        # LOW-1: strip any module qualifier or path and a trailing .exe before matching, so 'Mod\Stop-Process' and 'C:\...\taskkill.exe' are seen as bare names.
        if ($nm) { $nm = ($nm -replace '^.*[\\/]', '') -replace '(?i)\.(exe|com|bat|cmd)$', '' }
        if ($c.InvocationOperator -eq [System.Management.Automation.Language.TokenKind]::Dot) {
            if ($c.Extent.Text -notmatch '^\.\s*\(Join-Path \$PSScriptRoot "lib_push_verify_walk[.]ps1"\)$') { [void]$v.Add("dot-source of something else: $($c.Extent.Text)") }
            continue
        }
        if (-not $nm) { [void]$v.Add("dynamic command name: $($c.Extent.Text)"); continue }
        if ($nm -match $bad) {
            [void]$v.Add("forbidden command: $nm")
        }
        if ($nm -ieq 'Add-Type') { $addType++; if ($kind -ne 'script') { [void]$v.Add("Add-Type outside the script") } }
        if ($nm -match '(?i)^(foreach-object|foreach|%)$') {
            foreach ($e in @($c.CommandElements | Select-Object -Skip 1)) {
                if ($e -is [System.Management.Automation.Language.CommandParameterAst] -and $e.ParameterName -match '(?i)^m') { [void]$v.Add("ForEach-Object -MemberName: $($c.Extent.Text)") }
                elseif ($e -is [System.Management.Automation.Language.StringConstantExpressionAst]) { [void]$v.Add("ForEach-Object member-name form: $($c.Extent.Text)") }
            }
        }
    }
    foreach ($t in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.TypeExpressionAst] }, $true)) {
        $tn = $t.TypeName.FullName
        if ($tn -match '(?i)(^|\.)(Process|ManagementObject|WmiObject|CimMethod\w*|ScriptBlock|Activator|Marshal)$|Management\.|Wmi|Cim') {
            if ($tn -match '(?i)diagnostics\.process$' -and (Get-FnOf $t) -eq 'Open-PvTargets' -and $kind -eq 'lib') { continue }
            [void]$v.Add("type [$tn] outside its allowed site")
        }
    }
    $killJob = 0; $killProc = 0; $libKill = 0; $libGet = 0
    foreach ($m in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.InvokeMemberExpressionAst] }, $true)) {
        if ($m.Member -isnot [System.Management.Automation.Language.StringConstantExpressionAst]) { [void]$v.Add("dynamic member call: $($m.Extent.Text)"); continue }
        $mn = [string]$m.Member.Value; $recv = $m.Expression.Extent.Text
        $args0 = (@($m.Arguments) | ForEach-Object { $_.Extent.Text }) -join ','
        if ($recv -eq '[PvJob]') {
            if ($mn -eq 'Kill') { if ($args0 -eq '$fetchJob') { $killJob++ } else { [void]$v.Add("[PvJob]::Kill with argument '$args0'") } }
            elseif ($mn -eq 'KillProc') { if ($args0 -eq '$fetchH') { $killProc++ } else { [void]$v.Add("[PvJob]::KillProc with argument '$args0'") } }
            elseif ($mn -eq 'Launch') { if ($args0 -match '^\$gitExe,') { $launch++ } else { [void]$v.Add("[PvJob]::Launch with first argument other than gitExe: $args0") } }
            elseif ($mn -notmatch '^(Create|WaitExit|ExitCode|StartFileTime|CloseProc|Close|Assign)$') { [void]$v.Add("[PvJob]::$mn not allowlisted") }
            continue
        }
        if ($mn -match '(?i)^(Kill|Terminate\w*|CloseMainWindow|GetProcess\w*|Start|Invoke\w*|CreateInstance|Create\w*|Load\w*|Add\w*Script\w*)$') {
            $fn = Get-FnOf $m
            if ($kind -eq 'lib' -and $mn -ceq 'Kill' -and $fn -eq 'Stop-PvTargets') { $libKill++; continue }
            if ($kind -eq 'lib' -and $mn -ceq 'GetProcessById' -and $fn -eq 'Open-PvTargets') { $libGet++; continue }
            [void]$v.Add("$recv.$mn() outside its allowed site (in '$fn')")
        }
    }
    if ($kind -eq 'script') {
        if ($addType -ne 1) { [void]$v.Add("expected exactly one Add-Type, found $addType") }
        if ($launch -ne 1) { [void]$v.Add("expected exactly one [PvJob]::Launch(gitExe, ...), found $launch") }
        # Every string constant outside the one C# here-string is scanned for process-API tokens too (a second, single-quoted Add-Type body).
        foreach ($sc in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.StringConstantExpressionAst] -and $n.StringConstantType -ne 'DoubleQuotedHereString' }, $true)) {
            if ($sc.Value -match '(?i)DllImport|OpenProcess|NtTerminate|TerminateProcess|TerminateJobObject') { [void]$v.Add("string constant carries a process-API token: $($Matches[0])") }
        }
        if ($killJob -ne 1) { [void]$v.Add("expected exactly one [PvJob]::Kill(`$fetchJob), found $killJob") }
        if ($killProc -ne 1) { [void]$v.Add("expected exactly one [PvJob]::KillProc(`$fetchH), found $killProc") }
        if (@($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Stop-PvTargets' }, $true)).Count -lt 1) { [void]$v.Add("no Stop-PvTargets call") }
    } else {
        if ($libKill -ne 1) { [void]$v.Add("expected exactly one .Kill() inside Stop-PvTargets, found $libKill") }
        if ($libGet -ne 1) { [void]$v.Add("expected exactly one GetProcessById inside Open-PvTargets, found $libGet") }
    }
    if ($kind -eq 'script') {
        $cs = ''
        foreach ($hs in $ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.StringConstantExpressionAst] -and $n.StringConstantType -eq 'DoubleQuotedHereString' }, $true)) { $cs += $hs.Value + "`n" }
        $cs = [regex]::Replace($cs, '//[^\r\n]*', '')
        if ($cs.Length -lt 200) { [void]$v.Add("C# block not found") }
        if ($cs -match '(?i)GetProcessById|Process\.GetProcess\w*|\.Kill\s*\(|taskkill|Process\.Start\b|OpenProcess|NtTerminate|EntryPoint|DuplicateHandle|ExitWindows|GenerateConsoleCtrlEvent') { [void]$v.Add("C# block uses a forbidden process API: $($Matches[0])") }
        $tp = @([regex]::Matches($cs, '(?<!extern bool )TerminateProcess\s*\(([^)]*)\)') | ForEach-Object { ($_.Groups[1].Value -replace '\s', '') })
        if (($tp -join '|') -ne 'pi.hp,1|h,1') { [void]$v.Add("TerminateProcess call sites/args not pinned to (pi.hp,1) and (h,1): $($tp -join '|')") }
        $tj = @([regex]::Matches($cs, '(?<!extern bool )TerminateJobObject\s*\(([^)]*)\)') | ForEach-Object { ($_.Groups[1].Value -replace '\s', '') })
        if (($tj -join '|') -ne 'j,1') { [void]$v.Add("TerminateJobObject call sites/args not pinned to (j,1): $($tj -join '|')") }
        if ($cs -notmatch 'public static void KillProc\(IntPtr h\)\s*\{\s*TerminateProcess\(h, 1\);\s*\}') { [void]$v.Add("KillProc body is not exactly TerminateProcess(h, 1)") }
        if ($cs -notmatch 'catch\s*\{\s*TerminateProcess\(pi\.hp, 1\);\s*throw;') { [void]$v.Add("Launch failure path does not terminate its own pi.hp") }
    }
    return ,$v
}
$pvV = Get-KillLintViolations $pvSrc 'script'
$libV = Get-KillLintViolations $pvLibSrc 'lib'
foreach ($x in @($pvV) + @($libV)) { Write-Host "  lint: $x" }
$pvCs = ''
$pvAst0 = [System.Management.Automation.Language.Parser]::ParseInput($pvSrc, [ref]$null, [ref]$null)
foreach ($hs in @($pvAst0.FindAll({ param($n) $n -is [System.Management.Automation.Language.StringConstantExpressionAst] -and $n.StringConstantType -eq 'DoubleQuotedHereString' }, $true))) { $pvCs += $hs.Value + "`n" }
$pvCs = [regex]::Replace($pvCs, '//[^\r\n]*', '')
Assert (@($pvV).Count -eq 0 -and @($libV).Count -eq 0) "S2/L1: kill-surface allowlist holds for push_verify.ps1 and lib_push_verify_walk.ps1"
Assert ((Get-LintCount $pvSrc 'script') -eq 0 -and (Get-LintCount $pvLibSrc 'lib') -eq 0) "L1: Get-LintCount returns 0 for the clean sources"
Assert ((Get-LintCount ($pvSrc + "`nWrite-Host hi") 'script') -eq 0) "L1: a harmless extra command is not flagged"
# The lint itself must reject each hole the 2026-10-12 review demonstrated (dead code inserted at the end of the file).
$holes = @{
    'Stop-Process' = 'if ($false) { Stop-Process -Id $fetchPid }'
    'cmd taskkill' = 'if ($false) { cmd /c taskkill /F /PID $fetchPid }'
    'Start-Process taskkill' = 'if ($false) { Start-Process taskkill -ArgumentList "/F /PID $fetchPid" }'
    'CIM Terminate' = 'if ($false) { Invoke-CimMethod -Query "select * from Win32_Process where ProcessId=$fetchPid" -MethodName Terminate }'
    'KillProc via Get-Process' = 'if ($false) { [PvJob]::KillProc((Get-Process -Id $fetchPid).Handle) }'
    'dynamic command name' = 'if ($false) { & (''Stop-'' + ''Process'') -Id $fetchPid }'
    'Process.Kill via GetProcessById' = 'if ($false) { [System.Diagnostics.Process]::GetProcessById($fetchPid).Kill() }'
    'second KillProc' = 'if ($false) { [PvJob]::KillProc($otherH) }'
    'dynamic member' = 'if ($false) { $p.("Ki" + "ll")() }'
}
$holes['module-qualified Stop-Process'] = 'if ($false) { Microsoft.PowerShell.Management\Stop-Process -Id 1 }'
$holes['absolute-path taskkill'] = 'if ($false) { C:\Windows\System32\taskkill.exe /F /PID 1 }'
$holes['Set-Alias'] = 'if ($false) { Set-Alias zz Stop-Process; zz }'
$holes['WMI ForEach-Object Terminate'] = 'if ($false) { Get-WmiObject Win32_Process | ForEach-Object Terminate }'
$holes['ForEach-Object -MemberName'] = 'if ($false) { Get-CimInstance Win32_Process | ForEach-Object -MemberName Terminate }'
$holes['InvokeScript'] = 'if ($false) { $ExecutionContext.InvokeCommand.InvokeScript("x") }'
$holes['Launch other exe'] = 'if ($false) { [PvJob]::Launch("C:\Windows\System32\taskkill.exe", "/F", $a, $b, $c, 0, [ref]$d, [ref]$e) }'
$holes['second single-quoted Add-Type'] = "if (`$false) { Add-Type -TypeDefinition 'public class Zk { public static void K(int p){} }' }"
$holes['qualified Get-Process ForEach Kill'] = 'if ($false) { Microsoft.PowerShell.Management\Get-Process | ForEach-Object Kill }'
$holes['concatenated EntryPoint'] = 'if ($false) { Add-Type -TypeDefinition ''using System.Runtime.InteropServices; public class Zk { [DllImport("kernel32.dll", EntryPoint = "Open" + "Process")] public static extern int O(); }'' }'
foreach ($k in $holes.Keys) {
    Assert ((Get-LintCount ($pvSrc + "`n" + $holes[$k]) 'script') -gt 0) "L1 hole closed (script): $k"
}
Assert ((Get-LintCount ($pvLibSrc + "`nif (`$false) { Stop-Process -Id 1 }") 'lib') -gt 0) "L1 hole closed (lib): Stop-Process"
Assert ((Get-LintCount ($pvLibSrc + "`nif (`$false) { `$x.Kill() }") 'lib') -gt 0) "L1 hole closed (lib): Kill outside Stop-PvTargets"
Assert ((Get-LintCount ($pvSrc.Replace('TerminateProcess(h, 1);', 'TerminateProcess(h, 1); TerminateProcess(pi.hp, 9);')) 'script') -gt 0) "L1 hole closed: extra C# TerminateProcess call site"
Assert ((Get-LintCount ($pvSrc.Replace('static extern bool CloseHandle(IntPtr h);', 'static extern bool CloseHandle(IntPtr h); [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint a, bool b, uint c);')) 'script') -gt 0) "L1 hole closed: C# OpenProcess"
Assert (($pvCs -match 'ResumeThread\(pi\.ht\)\s*==\s*0xFFFFFFFF') -and ($pvCs -match 'hin\s*==\s*bad') -and ($pvCs -match 'hout\s*==\s*bad') -and ($pvCs -match 'herr\s*==\s*bad')) "LOW-2: ResumeThread and every CreateFileW result is checked (INVALID_HANDLE_VALUE / 0xFFFFFFFF)"
Assert (($pvCs -match 'CreateProcessW\(gitExe,') -and ($pvSrc -match 'Get-Command git -CommandType Application')) "INFO-3: CreateProcessW gets the resolved git.exe path as lpApplicationName"
$me = Get-Process -Id $PID
$hm = Open-PvTargets -Ids @($PID) -Created @{ $PID = $me.StartTime }
Assert ($hm.ContainsKey($PID)) "R2: a live process whose StartTime matches the snapshot is opened"
$hx = Open-PvTargets -Ids @($PID) -Created @{ $PID = $me.StartTime.AddMinutes(-30) }
Assert (-not $hx.ContainsKey($PID)) "R2: a live process whose StartTime differs from the snapshot (reused PID) is skipped"
try {
    git init --bare -b dev $origin *>$null
    git clone $origin $work *>$null
    git -C $work checkout -b dev *>$null
    $c1 = Commit $work "a.txt" "c1"
    git -C $work push origin dev *>$null

    Write-Host "case: pushed commit is LANDED (default and bare branch name)"
    $r = Run-PV @("-Commit", $c1)
    Assert ($r.Rc -eq 0 -and $r.Out -match "VERDICT: LANDED") "default -Branch origin/dev -> LANDED"
    $r = Run-PV @("-Commit", $c1, "-Branch", "dev")
    Assert ($r.Rc -eq 0 -and $r.Out -match "VERDICT: LANDED") "-Branch dev resolves as origin/dev -> LANDED"

    Write-Host "case: unpushed commit on LOCAL dev is NOT LANDED (F7)"
    $c2 = Commit $work "b.txt" "c2 unpushed"
    $r = Run-PV @("-Commit", $c2, "-Branch", "dev")
    Assert ($r.Rc -eq 1 -and $r.Out -match "NOT LANDED") "-Branch dev does not trust local dev"
    $r = Run-PV @("-Commit", $c2)
    Assert ($r.Rc -eq 1 -and $r.Out -match "NOT LANDED" -and $r.Out -match "stranded") "default branch: NOT LANDED, names the stranded local branch"
    git -C $work branch origin/dev $c2 *>$null
    $r = Run-PV @("-Commit", $c2, "-Branch", "origin/dev")
    Assert ($r.Rc -eq 1 -and $r.Out -match "NOT LANDED") "a local branch named origin/dev cannot shadow the remote ref"
    git -C $work branch -D origin/dev *>$null

    Write-Host "case: commit pushed by another clone is seen (fetch first)"
    git clone $origin $other *>$null
    git -C $other checkout dev *>$null
    $c3 = Commit $other "c.txt" "c3 from other"
    git -C $other push origin dev *>$null
    git -C $work cat-file -e $c3 2>$null
    Assert ($LASTEXITCODE -ne 0) "precondition: work has not fetched c3"
    $r = Run-PV @("-Commit", $c3)
    Assert ($r.Rc -eq 0 -and $r.Out -match "VERDICT: LANDED") "verify fetches, so c3 is LANDED"

    Write-Host "case: refusals"
    $r = Run-PV @("-Commit", "deadbeefdeadbeefdeadbeefdeadbeefdeadbeef")
    Assert ($r.Rc -eq 1 -and $r.Out -match "does not resolve") "unknown commit -> NOT LANDED"
    $r = Run-PV @("-Commit", $c1, "-Branch", "origin/nosuch")
    Assert ($r.Rc -eq 1 -and $r.Out -match "UNKNOWN") "missing remote branch -> UNKNOWN, not LANDED"
    Write-Host "case: -Commit is required (P2); -FetchTimeoutSec validated (P3)"
    $r = Run-PV @()
    Assert ($r.Rc -eq 2 -and $r.Out -match "-Commit <sha> is required" -and $r.Out -notmatch "LANDED --") "no -Commit -> usage error exit 2, never LANDED for HEAD"
    git -C $work fetch -q origin *>$null
    $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "0")
    Assert ($r.Rc -eq 2 -and $r.Out -match "must be > 0") "-FetchTimeoutSec 0 -> usage error"
    $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "-5")
    Assert ($r.Rc -eq 2 -and $r.Out -match "must be > 0") "negative -FetchTimeoutSec -> usage error"

    $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2000001")
    Assert ($r.Rc -eq 2 -and $r.Out -match "must be <= 2000000") "oversized -FetchTimeoutSec -> usage error (INFO-4)"
    Write-Host "case: failed fetch -> UNKNOWN, never LANDED (P3)"
    $goodUrl = (git -C $work remote get-url origin).Trim()
    git -C $work remote set-url origin (Join-Path $tmp "no_such_remote.git") *>$null
    $r = Run-PV @("-Commit", $c1)
    Assert ($r.Rc -eq 1 -and $r.Out -match "VERDICT: UNKNOWN" -and $r.Out -match "failed" -and $r.Out -notmatch "LANDED --") "unreachable remote -> UNKNOWN (a stale ref would have said LANDED)"

    Write-Host "case: hung fetch times out and the git process tree is killed (P3)"
    $lis = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $lis.Start(); $port = $lis.LocalEndpoint.Port
    try {
        git -C $work remote set-url origin "http://127.0.0.1:$port/x.git" *>$null
        $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2")
        Assert ($r.Rc -eq 1 -and $r.Out -match "VERDICT: UNKNOWN" -and $r.Out -match "timed out") "a fetch that exceeds the timeout -> UNKNOWN, never LANDED"
        Assert-NoSurvivors $port (Split-Path -Leaf $tmp) "no git process for the hung fetch survives the timeout"
        # I2: survivors are matched by the URL (git-remote-http) OR this run's unique scratch dir (wrapper git and inner git carry it in -C).
        # toolfx8 T1: force the no-job fallback (test hook) and require the whole tree, grandchild included, to die.
        $env:PUSH_VERIFY_TEST_MODE = "1"; $env:PUSH_VERIFY_TEST_NO_JOB = "1"
        try { $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2") } finally { Remove-Item Env:\PUSH_VERIFY_TEST_NO_JOB, Env:\PUSH_VERIFY_TEST_MODE -ErrorAction SilentlyContinue }
        Assert ($r.Rc -eq 1 -and $r.Out -match "VERDICT: UNKNOWN" -and $r.Out -match "timed out" -and $r.Out -match "no job membership" -and $r.Out -match "walk found [1-9]\d* descendant") "no-job fallback: hung fetch -> UNKNOWN, and the fallback path (not the job) ran"
        Assert-NoSurvivors $port (Split-Path -Leaf $tmp) "no-job fallback: no git process (incl. git-remote-http grandchild) survives the timeout"
        # LOW-4: PUSH_VERIFY_TEST_NO_JOB without PUSH_VERIFY_TEST_MODE is ignored (the job path is used).
        $env:PUSH_VERIFY_TEST_NO_JOB = "1"
        try { $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2") } finally { Remove-Item Env:\PUSH_VERIFY_TEST_NO_JOB -ErrorAction SilentlyContinue }
        Assert ($r.Rc -eq 1 -and $r.Out -match "timed out" -and $r.Out -notmatch "no job membership") "stray PUSH_VERIFY_TEST_NO_JOB without PUSH_VERIFY_TEST_MODE: ignored (job path used)"
        # M1 (toolfx10): job path with the create->assign window widened by 300 ms. The child is created suspended, so no descendant can escape the job.
        $env:PUSH_VERIFY_TEST_MODE = "1"; $env:PUSH_VERIFY_TEST_ASSIGN_DELAY_MS = "300"
        try { $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2") } finally { Remove-Item Env:\PUSH_VERIFY_TEST_ASSIGN_DELAY_MS, Env:\PUSH_VERIFY_TEST_MODE -ErrorAction SilentlyContinue }
        Assert ($r.Rc -eq 1 -and $r.Out -match "VERDICT: UNKNOWN" -and $r.Out -match "timed out" -and $r.Out -notmatch "no job membership") "late job assign (300 ms): job path still used, hung fetch -> UNKNOWN"
        Assert-NoSurvivors $port (Split-Path -Leaf $tmp) "late job assign (300 ms): no git process (wrapper, git, remote-http) survives the timeout"
        # LOW-4: the delay hook is ignored without the test-mode flag (production env var stray), and capped with it.
        $env:PUSH_VERIFY_TEST_ASSIGN_DELAY_MS = "90000"
        try { $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2") } finally { Remove-Item Env:\PUSH_VERIFY_TEST_ASSIGN_DELAY_MS -ErrorAction SilentlyContinue }
        Assert ($r.Rc -eq 1 -and $r.Out -match "timed out" -and $r.Out -notmatch "TEST HOOK") "stray PUSH_VERIFY_TEST_ASSIGN_DELAY_MS without PUSH_VERIFY_TEST_MODE: ignored (no stall)"
        $env:PUSH_VERIFY_TEST_MODE = "1"; $env:PUSH_VERIFY_TEST_ASSIGN_DELAY_MS = "99999999999"
        try { $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2") } finally { Remove-Item Env:\PUSH_VERIFY_TEST_ASSIGN_DELAY_MS, Env:\PUSH_VERIFY_TEST_MODE -ErrorAction SilentlyContinue }
        Assert ($r.Rc -eq 1 -and $r.Out -match "timed out" -and $r.Out -notmatch "TEST HOOK") "oversized delay value parsed safely (ignored, no cast crash)"
        $env:PUSH_VERIFY_TEST_MODE = "1"; $env:PUSH_VERIFY_TEST_ASSIGN_DELAY_MS = "99999"
        try { $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2") } finally { Remove-Item Env:\PUSH_VERIFY_TEST_ASSIGN_DELAY_MS, Env:\PUSH_VERIFY_TEST_MODE -ErrorAction SilentlyContinue }
        Assert ($r.Out -match "assign delay 5000 ms") "delay hook with test mode is capped at 5000 ms"
    } finally { $lis.Stop() }
    git -C $work remote set-url origin $goodUrl *>$null

    # LOW-2: no-job path where the ROOT git has no descendant (mingw64 git.exe, git:// connects in process). Only KillProc can kill it.
    Write-Host "case: no-job fallback, root git without descendants (git://), KillProc is load-bearing"
    $mingwBin = Join-Path (Split-Path -Parent (Split-Path -Parent ((git --exec-path) -replace "/", "\"))) 'bin'
    $lis2 = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $lis2.Start(); $port2 = $lis2.LocalEndpoint.Port
    $oldPath = $env:PATH
    try {
        if (-not (Test-Path (Join-Path $mingwBin 'git.exe'))) { Assert $false "precondition: mingw64\bin\git.exe exists at $mingwBin" }
        else {
            git -C $work remote set-url origin "git://127.0.0.1:$port2/x.git" *>$null
            $env:PATH = "$mingwBin;$oldPath"
            $env:PUSH_VERIFY_TEST_MODE = "1"; $env:PUSH_VERIFY_TEST_NO_JOB = "1"
            try { $r = Run-PV @("-Commit", $c1, "-FetchTimeoutSec", "2") } finally { Remove-Item Env:\PUSH_VERIFY_TEST_NO_JOB, Env:\PUSH_VERIFY_TEST_MODE -ErrorAction SilentlyContinue; $env:PATH = $oldPath }
            Assert ($r.Rc -eq 1 -and $r.Out -match "VERDICT: UNKNOWN" -and $r.Out -match "timed out" -and $r.Out -match "no job membership") "root-only fallback: hung git:// fetch -> UNKNOWN via the no-job path"
            Assert-NoSurvivors $port2 (Split-Path -Leaf $tmp) "root-only fallback: the root git itself is killed (KillProc), no survivor"
        }
    } finally { $env:PATH = $oldPath; $lis2.Stop(); git -C $work remote set-url origin $goodUrl *>$null }
    Write-Host "case: fetch exits before the first handle read (MED-1)"
    $env:PUSH_VERIFY_TEST_MODE = "1"; $env:PUSH_VERIFY_TEST_POST_LAUNCH_SLEEP_MS = "1500"
    try { $r = Run-PV @("-Commit", $c1) } finally { Remove-Item Env:\PUSH_VERIFY_TEST_POST_LAUNCH_SLEEP_MS, Env:\PUSH_VERIFY_TEST_MODE -ErrorAction SilentlyContinue }
    Assert ($r.Rc -eq 0 -and $r.Out -match "VERDICT: LANDED") "git exits 0 long before the exit code is read -> still LANDED (native handle held, no null ExitCode)"

    Write-Host "case: explicit refspec beats a narrowed fetch refspec (P1)"
    git -C $other push -q origin HEAD:refs/heads/side *>$null                          # a branch that exists, so the narrowed fetch itself succeeds
    git -C $work config remote.origin.fetch "+refs/heads/side:refs/remotes/origin/side"
    $cx = Commit $other "x.txt" "cx doomed"
    git -C $other push origin dev *>$null
    git -C $work fetch -q origin "+refs/heads/dev:refs/remotes/origin/dev" *>$null      # work now knows cx on origin/dev
    git -C $other reset -q --hard HEAD~1 *>$null
    git -C $other push -q --force origin dev *>$null                                    # remote dev no longer has cx
    $r = Run-PV @("-Commit", $cx)
    Assert ($r.Rc -eq 1 -and $r.Out -match "NOT LANDED") "narrowed remote.origin.fetch cannot leave a stale ref that reports LANDED"
    git -C $work config remote.origin.fetch "+refs/heads/*:refs/remotes/origin/*"
} finally { Set-Location $here; Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
if ($script:fails -gt 0) { Write-Host "check_push_verify: $script:fails FAILED" -ForegroundColor Red; exit 1 }
Write-Host "check_push_verify: all cases passed"; exit 0

