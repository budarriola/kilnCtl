# checkcache: ok
# Guards tools\agent_tail.ps1 (subagent transcript tail + STUCK? heuristics).
#
# Synthetic transcripts (healthy / stuck-repeat / stuck-dead-log / old-log-but-
# build-running) are generated in a temp projects dir, plus the real trimmed
# fixture tools\test_fixtures\agent_tail\ (the 2026-10-08 agent that polled
# nosign_tb1.log for 5 h; its log path is rewritten to a temp file so the check
# does not depend on C:\wt). Then each heuristic is negative-tested in-process:
# a mutated COPY of agent_tail.ps1 must turn the matching case red.
#
# -ScriptUnderTest points the check at a (mutated) script; -NoMutations skips
# the self-test (used for the recursive mutated runs).
# Exit: 0 pass, 1 fail.
param([string]$ScriptUnderTest, [switch]$NoMutations)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $ScriptUnderTest) { $ScriptUnderTest = Join-Path $here 'agent_tail.ps1' }
$fixDir = Join-Path $here 'test_fixtures\agent_tail'
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('agent_tail_chk_' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$failures = @(); $n = 0
function Assert-True($c, $m) { $script:n++; if (-not $c) { $script:failures += $m } }

function Add-Agent([string]$proj, [string]$id, [string]$desc, [object[]]$events) {
    $d = Join-Path $proj 's1\subagents'
    New-Item -ItemType Directory -Force -Path $d | Out-Null
    $lines = foreach ($e in $events) {
        $blk = switch ($e.k) {
            'USE' { @{ type = 'tool_use'; input = @{ command = $e.t } } }
            'RES' { @{ type = 'tool_result'; content = $e.t } }
            'TXT' { @{ type = 'text'; text = $e.t } }
        }
        (@{ timestamp = $e.ts; message = @{ content = @($blk) } } | ConvertTo-Json -Compress -Depth 6)
    }
    $lines = @('not json at all', '{"type":"attachment"}') + @($lines)
    [IO.File]::WriteAllLines((Join-Path $d "agent-$id.jsonl"), [string[]]$lines)
    if ($desc) { Set-Content -LiteralPath (Join-Path $d "agent-$id.meta.json") -Value (@{ description = $desc } | ConvertTo-Json -Compress) -Encoding ascii }
}
function Mk-Poll([string]$proj, [string]$id, [string]$log, [int]$count, [int]$vary) {
    $base = (Get-Date).ToUniversalTime().AddMinutes(-($count * 2 + 1))
    $ev = @()
    for ($i = 0; $i -lt $count; $i++) {
        $ts = $base.AddMinutes($i * 2).ToString('o')
        $ts2 = $base.AddMinutes($i * 2 + 1).ToString('o')
        $unix = '/' + $log.Substring(0, 1).ToLower() + '/' + ($log.Substring(3) -replace '\\', '/')
        if ($vary -eq 1) { $cmd = "echo step$i && ls /tmp/unique$i$i$i" } else { $cmd = "timeout 585 bash -c 'until grep -q ""^EXIT"" $unix; do sleep 20; done'; tail -c $(300 + $i) $unix" }
        $res = if ($vary -eq 1) { "output number $i is different text $i-$i" } elseif ($vary -eq 2) { "progress: compiled $($i * 7) of 99 files" } else { "Building target (CCACHE=1) waiting" }
        $ev += @{ k = 'USE'; ts = $ts; t = $cmd }
        $ev += @{ k = 'RES'; ts = $ts2; t = $res }
    }
    Add-Agent $proj $id "synthetic $id" $ev
}
function Run-Tool([string]$script, [string]$proj, [string[]]$more) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -ProjectsDir $proj @more 2>&1 | Out-String
    $json = $null
    $last = ($out -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 1)
    try { $json = $last | ConvertFrom-Json } catch {}
    [pscustomobject]@{ Out = $out; Json = $json; Stuck = ($out -match 'STUCK\?') }
}

function Run-Cases([string]$script, [string]$label) {
    $proj = Join-Path $tmp ("proj_" + [guid]::NewGuid().ToString('N').Substring(0, 6))
    $freshLog = Join-Path $tmp 'fresh.log'; Set-Content $freshLog 'x'
    $deadLog = Join-Path $tmp 'dead.log'; Set-Content $deadLog 'x'; (Get-Item $deadLog).LastWriteTime = (Get-Date).AddHours(-5)
    $none = @('explorer', 'svchost')
    $res = @{}
    Mk-Poll $proj 'healthy0000000001' $freshLog 6 1
    Mk-Poll $proj 'repeat0000000001' $freshLog 6 0         # (a): identical polls, unchanged results, FRESH log
    Mk-Poll $proj 'deadlog000000001' $deadLog 2 0         # (b): 2 polls only (< K), log 5 h old
    Mk-Poll $proj 'progress0000001' $freshLog 6 2          # identical polls but results advance: not stuck
    Mk-Poll $proj 'deadbuild0000001' $deadLog 2 0         # (b) must stay quiet when a build runs
    # real fixture with its log path rewritten to a temp 5 h old log
    $realLog = Join-Path $tmp 'nosign_tb1.log'; Set-Content $realLog 'x'; (Get-Item $realLog).LastWriteTime = (Get-Date).AddHours(-5)
    $rd = Join-Path $proj 's1\subagents'
    $raw = Get-Content -LiteralPath (Join-Path $fixDir 'agent-acab7c75e525e6c62.jsonl') -Raw
    $unix = '/' + $realLog.Substring(0, 1).ToLower() + '/' + ($realLog.Substring(3) -replace '\\', '/')
    $raw = $raw.Replace('/c/wt/nosign_tb1.log', $unix)
    [IO.File]::WriteAllText((Join-Path $rd 'agent-acab7c75e525e6c62.jsonl'), $raw)
    Copy-Item (Join-Path $fixDir 'agent-acab7c75e525e6c62.meta.json') $rd

    $r = @{}
    $r.healthy = Run-Tool $script $proj @('-Id', 'healthy0000000001', '-ProcessSnapshot', 'explorer')
    $r.repeat = Run-Tool $script $proj @('-Id', 'repeat0000000001', '-ProcessSnapshot', 'explorer')
    $r.dead = Run-Tool $script $proj @('-Id', 'deadlog000000001', '-ProcessSnapshot', 'explorer')
    $r.progress = Run-Tool $script $proj @('-Id', 'progress0000001', '-ProcessSnapshot', 'explorer')
    $r.deadbuild = Run-Tool $script $proj @('-Id', 'deadbuild0000001', '-ProcessSnapshot', 'ninja,explorer')
    $r.mcponly = Run-Tool $script $proj @('-Id', 'deadbuild0000001', '-ProcessSnapshot', "python::C:\Python\python.exe -m kilnctrl.mcp_server --port 8767 --root $tmp,python::python tools/mykicadMcp/kicad_mcp_server.py --port 8766,pdf-mcp,explorer")
    $r.pytest = Run-Tool $script $proj @('-Id', 'deadbuild0000001', '-ProcessSnapshot', 'python::python -m pytest tools/PcTools/tests -x,explorer')
    $r.real = Run-Tool $script $proj @('-Id', 'acab7c75e525e6c62', '-ProcessSnapshot', 'explorer')
    $r.all = Run-Tool $script $proj @('-All', '-ProcessSnapshot', 'explorer')
    $r.missing = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -ProjectsDir $proj -Id nosuchagent 2>&1 | Out-Null; $missingCode = $LASTEXITCODE
    $r.usage = & powershell -NoProfile -ExecutionPolicy Bypass -File $script -ProjectsDir $proj 2>&1 | Out-Null; $usageCode = $LASTEXITCODE

    Assert-True (-not $r.healthy.Stuck) "$label healthy: must not be flagged"
    Assert-True ($r.repeat.Stuck -and $r.repeat.Out -match '\(a\)') "$label repeat: (a) must flag"
    Assert-True ($r.dead.Stuck -and $r.dead.Out -match '\(b\)') "$label dead log: (b) must flag"
    Assert-True ($r.mcponly.Stuck -and $r.mcponly.Out -match '\(b\)') "$label dead log with only MCP-server python: (b) must flag"
    Assert-True (-not $r.pytest.Stuck) "$label dead log with pytest python running: must not flag"
    Assert-True (-not $r.progress.Stuck) "$label progress (same cmd, advancing results): must not flag"
    Assert-True (-not $r.deadbuild.Stuck) "$label dead log with ninja running: must not flag"
    Assert-True ($r.real.Stuck -and $r.real.Out -match '\(b\)') "$label real fixture: must flag (b)"
    Assert-True ($r.real.Out -match 'Remove WP11 release signing') "$label real fixture: description shown"
    Assert-True ($r.real.Out -match ' USE ' -and $r.real.Out -match ' RES ' -and $r.real.Out -match ' TXT ') "$label real fixture: USE/RES/TXT lines"
    Assert-True ($r.real.Json -and $r.real.Json.stuck_count -eq 1 -and $r.real.Json.agents[0].stuck -eq $true) "$label real fixture: JSON summary stuck"
    Assert-True ($r.all.Json -and $r.all.Json.count -eq 6 -and $r.all.Json.stuck_count -eq 4) "$label -All: 6 agents, 4 stuck (got $($r.all.Json.count)/$($r.all.Json.stuck_count))"
    Assert-True ($r.all.Json -and $r.all.Json.elapsed_s -lt 10) "$label -All: elapsed_s under 10 (got $($r.all.Json.elapsed_s))"
    Assert-True ($r.all.Out -match 'STUCK\?') "$label -All: table carries STUCK?"
    Assert-True ($missingCode -eq 1) "$label unknown id exits 1 (got $missingCode)"
    Assert-True ($usageCode -eq 2) "$label no args exits 2 (got $usageCode)"
}

try {
    Run-Cases $ScriptUnderTest 'base'

    if (-not $NoMutations -and $failures.Count -eq 0) {
        $src = Get-Content -LiteralPath $ScriptUnderTest -Raw
        $muts = @(
            @{ name = '(a) repeat detection disabled'; from = '$norm.Count -eq 1'; to = '$norm.Count -eq 99'; expect = 'repeat' },
            @{ name = '(a) result-unchanged test dropped'; from = '$ru.Count -eq 1'; to = '$true'; expect = 'progress' },
            @{ name = '(b) staleness test inverted'; from = '$ageLog -gt $StaleMin'; to = '$ageLog -lt $StaleMin'; expect = 'dead' },
            @{ name = '(b) MCP exclusion dropped'; from = 'if (Test-McpCmd $p.Cmd) { continue }'; to = ''; expect = 'mcponly' },
            @{ name = '(b) running-process test dropped'; from = '$bp.Count -eq 0'; to = '$true'; expect = 'dead build' }
        )
        foreach ($m in $muts) {
            Assert-True ($src.Contains($m.from)) "mutation anchor missing: $($m.name)"
            if (-not $src.Contains($m.from)) { continue }
            $mp = Join-Path $tmp 'agent_tail_mut.ps1'
            Set-Content -LiteralPath $mp -Value $src.Replace($m.from, $m.to) -Encoding UTF8
            $before = $failures.Count
            Run-Cases $mp ('mut:' + $m.name)
            $caught = $failures.Count -gt $before
            $failures = @($failures | Select-Object -First $before)
            Assert-True $caught "negative test: mutation '$($m.name)' was NOT caught by any case"
        }
    }
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count) {
    Write-Host "AGENT_TAIL CHECK FAILED ($($failures.Count) of $n assertion(s)):" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
Write-Host "PASS: agent_tail ($n assertions, 5 mutations caught)"
exit 0


