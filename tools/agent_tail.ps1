# agent_tail.ps1 -- read-only: show what a Claude Code background subagent is
# doing, and flag one that is stuck. Never writes, never kills anything.
#
# WHY: on 2026-10-08 an agent polled /c/wt/nosign_tb1.log for 5 h after the
# build behind it had died; nothing surfaced it. This reads the subagent
# transcript (JSONL) and applies two heuristics.
#
# USAGE
#   powershell -ExecutionPolicy Bypass -File tools\agent_tail.ps1 -Id acab7c75e525e6c62 [-N 10]
#   powershell -ExecutionPolicy Bypass -File tools\agent_tail.ps1 -All [-SinceHours 12]
#
# Transcripts: <ProjectsDir>\<session-id>\subagents\agent-<id>.jsonl (+ .meta.json
# holding the description). ProjectsDir defaults to the kilnCtl project dir under
# $env:USERPROFILE\.claude\projects.
#
# STUCK? heuristics
#   (a) the last K (default 4) tool_use commands are the same after stripping
#       digits, and their tool_results (rejections excluded) are unchanged.
#   (b) the last command polls a log (path inside an `until grep` or wait_for.ps1
#       command), that log's LastWriteTime is older than -StaleMin (30), AND no
#       ninja/cmake/cl/*-gcc/cc1/python/pytest process is running.
#       -ProcessSnapshot overrides the process list (tests).
#
# Last stdout line is a JSON summary (like land.ps1 / wait_for.ps1).
# EXIT: 0 ok, 1 agent not found, 2 usage.
[CmdletBinding()]
param(
    [string]$Id,
    [switch]$All,
    [int]$N = 10,
    [double]$SinceHours = 12,
    [int]$K = 4,
    [int]$StaleMin = 30,
    [string]$ProjectsDir = (Join-Path $env:USERPROFILE '.claude\projects\c--Users-budar-OneDrive-Desktop-kilnCtl'),
    [string[]]$ProcessSnapshot = $null
)
$ErrorActionPreference = 'Stop'
if ($null -ne $ProcessSnapshot) { $ProcessSnapshot = @($ProcessSnapshot | ForEach-Object { $_ -split ',' }) }

function Out-Json($o) { Write-Output ($o | ConvertTo-Json -Compress -Depth 6) }
if (-not $Id -and -not $All) { Write-Host 'usage: agent_tail.ps1 -Id <agentId> | -All'; Out-Json @{ error = 'usage' }; exit 2 }

function Clip([string]$s, [int]$n) {
    $s = ($s -replace '\s+', ' ').Trim()
    if ($s.Length -gt $n) { $s.Substring(0, $n) } else { $s }
}
function ResultText($c) {
    if ($c -is [string]) { return $c }
    if ($c -is [System.Collections.IEnumerable]) {
        return (@($c | ForEach-Object { if ($_ -is [string]) { $_ } elseif ($_.text) { $_.text } else { '' } }) -join ' ')
    }
    return ''
}
function Get-Events([string]$path) {
    $ev = New-Object System.Collections.ArrayList
    foreach ($line in (Get-Content -LiteralPath $path -Tail 500 -Encoding UTF8)) {
        if (-not $line.StartsWith('{')) { continue }
        try { $o = $line | ConvertFrom-Json } catch { continue }
        if (-not $o.timestamp -or -not $o.message) { continue }
        $c = $o.message.content
        if ($c -is [string] -or $c -isnot [System.Collections.IEnumerable]) { continue }
        try { $t = [DateTime]::Parse([string]$o.timestamp, [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AdjustToUniversal -bor [Globalization.DateTimeStyles]::AssumeUniversal) } catch { continue }
        foreach ($b in $c) {
            switch ($b.type) {
                'tool_use' {
                    $cmd = if ($b.input.command) { [string]$b.input.command } else { ($b.input | ConvertTo-Json -Compress -Depth 3) }
                    [void]$ev.Add([pscustomobject]@{ Time = $t; Kind = 'USE'; Text = $cmd; Err = $false })
                }
                'tool_result' {
                    [void]$ev.Add([pscustomobject]@{ Time = $t; Kind = 'RES'; Text = (ResultText $b.content); Err = ($b.is_error -eq $true) })
                }
                'text' { [void]$ev.Add([pscustomobject]@{ Time = $t; Kind = 'TXT'; Text = [string]$b.text; Err = $false }) }
            }
        }
    }
    , $ev
}
function Strip-Digits([string]$s) { ($s -replace '\d+', '') -replace '\s+', ' ' }

function Get-PolledLogs([string]$cmd) {
    if ($cmd -notmatch 'until\s+grep|wait_for\.ps1') { return @() }
    $out = @()
    foreach ($m in [regex]::Matches($cmd, '(?<![\w])(?:/[a-zA-Z]/|[A-Za-z]:[\\/])[^\s''"|;&)]+')) {
        $p = $m.Value
        if ($p -match '^/([a-zA-Z])/(.*)$') { $p = $Matches[1].ToUpper() + ':\' + ($Matches[2] -replace '/', '\') }
        $out += $p
    }
    $m2 = [regex]::Match($cmd, '-LogFile\s+[''"]?([^\s''"]+)')
    if ($m2.Success) { $out += $m2.Groups[1].Value }
    $out | Select-Object -Unique
}
function Get-BuildProcs {
    $pat = '^(ninja|cmake|cl|cc1|cc1plus|cc1obj|pytest|python[\d.]*|pythonw)$|-gcc$|-g\+\+$'
    $names = if ($null -ne $ProcessSnapshot) { $ProcessSnapshot } else { (Get-Process | Select-Object -ExpandProperty ProcessName) }
    @($names | Where-Object { $_ -match $pat } | Select-Object -Unique)
}

function Analyze([string]$path) {
    $aid = ([IO.Path]::GetFileNameWithoutExtension($path)) -replace '^agent-', ''
    $desc = ''
    $meta = [IO.Path]::ChangeExtension($path, '.meta.json')
    if (Test-Path -LiteralPath $meta) { try { $desc = [string]((Get-Content -LiteralPath $meta -Raw | ConvertFrom-Json).description) } catch {} }
    $ev = Get-Events $path
    $reasons = @()
    $last = if ($ev.Count) { $ev[$ev.Count - 1].Time } else { (Get-Item -LiteralPath $path).LastWriteTimeUtc }
    $age = [math]::Round(((Get-Date).ToUniversalTime() - $last).TotalMinutes, 1)

    # pair each tool_use with the next RES (events are in order; results follow uses)
    $uses = @(); $res = @{}
    for ($i = 0; $i -lt $ev.Count; $i++) {
        if ($ev[$i].Kind -eq 'USE') {
            $uses += , $i
            $res[$i] = $null
            for ($j = $i + 1; $j -lt $ev.Count; $j++) {
                if ($ev[$j].Kind -eq 'USE') { break }
                if ($ev[$j].Kind -eq 'RES') { $res[$i] = $ev[$j]; break }
            }
        }
    }
    $lastCmd = ''
    if ($uses.Count) { $lastCmd = $ev[$uses[-1]].Text }

    # (a) repeat
    if ($uses.Count -ge $K) {
        $tail = $uses[($uses.Count - $K)..($uses.Count - 1)]
        $norm = @($tail | ForEach-Object { Strip-Digits $ev[$_].Text } | Select-Object -Unique)
        if ($norm.Count -eq 1) {
            $rs = @($tail | ForEach-Object { $res[$_] } | Where-Object { $_ -and -not $_.Err } | ForEach-Object { ($_.Text -replace '\s+',' ') })
            $ru = @($rs | Select-Object -Unique)
            if ($rs.Count -ge ($K - 1) -and $ru.Count -eq 1) {
                $reasons += "(a) last $K commands identical (digits stripped) with unchanged results (progress would change them)"
            }
        }
    }
    # (b) dead log
    if ($lastCmd) {
        foreach ($lp in (Get-PolledLogs $lastCmd)) {
            if (-not (Test-Path -LiteralPath $lp -PathType Leaf)) { continue }
            $lw = (Get-Item -LiteralPath $lp).LastWriteTime
            $ageLog = ((Get-Date) - $lw).TotalMinutes
            if ($ageLog -gt $StaleMin) {
                $bp = Get-BuildProcs
                if ($bp.Count -eq 0) {
                    $reasons += ("(b) polling {0}, last written {1:N0} min ago, no build/python process running" -f $lp, $ageLog)
                }
            }
        }
    }
    [pscustomobject]@{ Id = $aid; Desc = $desc; Last = $last; AgeMin = $age; Events = $ev; LastCmd = $lastCmd; Reasons = $reasons; Path = $path }
}

if (-not (Test-Path -LiteralPath $ProjectsDir)) { Write-Host "projects dir not found: $ProjectsDir"; Out-Json @{ error = 'no projects dir' }; exit 1 }
$files = @(Get-ChildItem -Path $ProjectsDir -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $sd = Join-Path $_.FullName 'subagents'
        if (Test-Path -LiteralPath $sd) { Get-ChildItem -LiteralPath $sd -Filter 'agent-*.jsonl' -File }
    })
if ($All) {
    $cut = (Get-Date).AddHours(-$SinceHours)
    $files = @($files | Where-Object { $_.LastWriteTime -ge $cut } | Sort-Object LastWriteTime -Descending)
} else {
    $files = @($files | Where-Object { $_.Name -eq "agent-$Id.jsonl" } | Select-Object -First 1)
    if ($files.Count -eq 0) { Write-Host "agent $Id not found under $ProjectsDir"; Out-Json @{ error = 'not found'; id = $Id }; exit 1 }
}

$rows = @($files | ForEach-Object { Analyze $_.FullName })
if (-not $All) {
    $r = $rows[0]
    Write-Host ("agent {0}" -f $r.Id)
    Write-Host ("description: {0}" -f $r.Desc)
    Write-Host ("last activity: {0:yyyy-MM-dd HH:mm:ss} local ({1} min ago)" -f $r.Last.ToLocalTime(), $r.AgeMin)
    foreach ($e in @($r.Events | Select-Object -Last $N)) {
        Write-Host ("{0:HH:mm:ss} {1} {2}" -f $e.Time.ToLocalTime(), $e.Kind, (Clip $e.Text 200))
    }
    foreach ($x in $r.Reasons) { Write-Host "STUCK? $x" -ForegroundColor Yellow }
} else {
    $tbl = $rows | ForEach-Object {
        [pscustomobject]@{ Id = $_.Id; Description = (Clip $_.Desc 30); AgeMin = $_.AgeMin; LastCommand = (Clip $_.LastCmd 50); Stuck = $(if ($_.Reasons.Count) { 'STUCK?' } else { '' }) }
    }
    if ($tbl) { ($tbl | Format-Table -AutoSize | Out-String -Width 250).TrimEnd() | Write-Host } else { Write-Host "no agent transcripts in the last $SinceHours h" }
}
$sum = @{
    agents = @($rows | ForEach-Object { @{ id = $_.Id; description = $_.Desc; last_utc = $_.Last.ToString('o'); age_min = $_.AgeMin; stuck = ($_.Reasons.Count -gt 0); reasons = @($_.Reasons); last_command = (Clip $_.LastCmd 120) } })
    count = $rows.Count
    stuck_count = @($rows | Where-Object { $_.Reasons.Count }).Count
}
Out-Json $sum
exit 0

