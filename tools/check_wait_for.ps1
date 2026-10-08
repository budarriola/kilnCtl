# check_wait_for.ps1 -- behavioural test for tools/wait_for.ps1 and
# tools/decode_log.ps1. Covers: UTF-16LE log written with `*>`, UTF-8 with and
# without a BOM, UTF-16BE, a pattern arriving mid-wait, PID exit, timeout (124),
# a file/log that appears later, -Any versus -All, tail/summary decoding, bad
# arguments (2), and decode_log round-tripping.
#
# Exit: 0 pass, 1 fail.

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$waiter = Join-Path $PSScriptRoot 'wait_for.ps1'
$decoder = Join-Path $PSScriptRoot 'decode_log.ps1'
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("wait_for_check_" + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $tmp | Out-Null

$script:fails = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) { Write-Output "PASS: $name" } else { Write-Output "FAIL: $name $detail"; $script:fails++ }
}

# Runs wait_for.ps1 in a child powershell exactly as Bash would.
function Run-Wait([string[]]$argv) {
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $waiter @argv
    $code = $LASTEXITCODE
    $line = @($out | Where-Object { $_ -like 'WAIT_RESULT *' }) | Select-Object -Last 1
    $json = $null
    if ($line) { $json = ($line.Substring(12) | ConvertFrom-Json) }
    return [pscustomobject]@{ Code = $code; Json = $json; Raw = ($out -join "`n") }
}

function Test-Started([string]$path) {
    try {
        $fs = New-Object System.IO.FileStream($path, 'Open', 'Read', 'ReadWrite,Delete')
        try { return ((New-Object System.IO.StreamReader($fs)).ReadToEnd() -like '*WAIT_START*') } finally { $fs.Dispose() }
    } catch { return $false }
}

# Starts the waiter, lets it get going, runs $act, then collects its result.
# Asserts the waiter was still running when $act fired (so the condition really
# arrived mid-wait).
function Run-WaitMid([string[]]$argv, [scriptblock]$act) {
    $o = Join-Path $tmp ('mid_' + [guid]::NewGuid().ToString('N').Substring(0, 6) + '.out')
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $waiter) + $argv
    $p = Start-Process -FilePath powershell -ArgumentList $a -PassThru -WindowStyle Hidden -RedirectStandardOutput $o
    $null = $p.Handle  # cache the handle so ExitCode is readable
    $dl = [DateTime]::UtcNow.AddSeconds(60)
    while ([DateTime]::UtcNow -lt $dl -and -not $p.HasExited -and -not (Test-Started $o)) { Start-Sleep -Milliseconds 200 }
    $running = -not $p.HasExited
    & $act
    $p.WaitForExit(60000) | Out-Null
    $code = $p.ExitCode
    $out = @(Get-Content $o)
    $line = @($out | Where-Object { $_ -like 'WAIT_RESULT *' }) | Select-Object -Last 1
    $json = $null
    if ($line) { $json = ($line.Substring(12) | ConvertFrom-Json) }
    return [pscustomobject]@{ Code = $code; Json = $json; Raw = ($out -join "`n"); WasRunning = $running }
}

function Write-Bytes([string]$path, [byte[]]$b) { [System.IO.File]::WriteAllBytes($path, $b) }
function Enc-Bytes($enc, [string]$s, [bool]$bom) {
    $b = $enc.GetBytes($s)
    if ($bom) { return ,([byte[]]($enc.GetPreamble() + $b)) }
    return ,$b
}

try {
    $text = "line one`r`nbuild finished`r`nDONE ok`r`n"

    # 1. UTF-16LE log produced by PowerShell `*>` (the case grep loops miss).
    $f = Join-Path $tmp 'u16.log'
    & powershell -NoProfile -Command "& { 'alpha'; 'DONE via redirect'; 'omega' } *> '$f'"
    $head = [System.IO.File]::ReadAllBytes($f)[0..1]
    Check 'setup: *> log is UTF-16LE BOM' ($head[0] -eq 0xFF -and $head[1] -eq 0xFE)
    $r = Run-Wait @('-LogFile', $f, '-Pattern', '^DONE', '-TimeoutSec', '20', '-PollSec', '0.5')
    Check 'utf16le via *> matches' ($r.Code -eq 0 -and $r.Json.status -eq 'MET' -and $r.Json.matched_line -eq 'DONE via redirect') $r.Raw
    Check 'utf16le tail is decoded' ($r.Json.tail -contains 'omega') $r.Raw

    # 2. Encoding variants.
    $variants = @(
        @('utf8 bom', (New-Object System.Text.UTF8Encoding($true)), $true),
        @('utf8 no bom', (New-Object System.Text.UTF8Encoding($false)), $false),
        @('utf16le no bom', (New-Object System.Text.UnicodeEncoding($false, $false)), $false),
        @('utf16be bom', (New-Object System.Text.UnicodeEncoding($true, $true)), $true)
    )
    foreach ($v in $variants) {
        $p = Join-Path $tmp (($v[0] -replace ' ', '_') + '.log')
        Write-Bytes $p (Enc-Bytes $v[1] $text $v[2])
        $r = Run-Wait @('-LogFile', $p, '-Pattern', '^DONE ok$', '-TimeoutSec', '20', '-PollSec', '0.5')
        Check "$($v[0]) matches" ($r.Code -eq 0 -and $r.Json.matched_line -eq 'DONE ok') $r.Raw
        # First-line pattern: a BOM left undecoded would defeat the anchor.
        $r = Run-Wait @('-LogFile', $p, '-Pattern', '^line one$', '-TimeoutSec', '20', '-PollSec', '0.5')
        Check "$($v[0]) first line anchored" ($r.Code -eq 0 -and $r.Json.matched_line -eq 'line one') $r.Raw
    }

    # 3. Pattern arriving mid-wait (incremental read, UTF-16LE appended in pieces,
    #    including a line split across two writes).
    $m = Join-Path $tmp 'mid.log'
    $le = New-Object System.Text.UnicodeEncoding($false, $true)
    Write-Bytes $m (Enc-Bytes $le "starting`r`n" $true)
    $r = Run-WaitMid @('-LogFile', $m, '-Pattern', '^FINISHED', '-TimeoutSec', '60', '-PollSec', '0.3') {
        $e = New-Object System.Text.UnicodeEncoding($false, $false)
        $fs = [System.IO.File]::Open($m, 'Append', 'Write', 'ReadWrite')
        $b = $e.GetBytes('FIN'); $fs.Write($b, 0, $b.Length); $fs.Flush()
        Start-Sleep -Milliseconds 1500
        $b = $e.GetBytes("ISHED now`r`n"); $fs.Write($b, 0, $b.Length); $fs.Dispose()
    }
    Check 'pattern arrives mid-wait (split line)' ($r.Code -eq 0 -and $r.Json.matched_line -eq 'FINISHED now' -and $r.WasRunning) $r.Raw

    # 4. Log file appearing later.
    $late = Join-Path $tmp 'late.log'
    $r = Run-WaitMid @('-LogFile', $late, '-Pattern', 'ready', '-TimeoutSec', '60', '-PollSec', '0.3') { 'ready' *> $late }
    Check 'log file appears later' ($r.Code -eq 0 -and $r.Json.matched_line -eq 'ready' -and $r.WasRunning) $r.Raw

    # 5. Timeout -> 124, missing log tolerated.
    $r = Run-Wait @('-LogFile', (Join-Path $tmp 'never.log'), '-Pattern', 'x', '-TimeoutSec', '2', '-PollSec', '0.3')
    Check 'timeout returns 124 TIMEOUT' ($r.Code -eq 124 -and $r.Json.status -eq 'TIMEOUT' -and $r.Json.unmet -contains 'log') $r.Raw
    $np = Join-Path $tmp 'nomatch.log'
    Write-Bytes $np (Enc-Bytes (New-Object System.Text.UTF8Encoding($false)) "nothing here`n" $false)
    $r = Run-Wait @('-LogFile', $np, '-Pattern', 'absent', '-TimeoutSec', '2', '-PollSec', '0.3')
    Check 'timeout with present log still 124 and tail kept' ($r.Code -eq 124 -and $r.Json.tail -contains 'nothing here') $r.Raw

    # 6. PID exit.
    $proc = Start-Process -FilePath powershell -ArgumentList '-NoProfile', '-Command', 'Start-Sleep -Seconds 3' -PassThru -WindowStyle Hidden
    $r = Run-Wait @('-Pid', "$($proc.Id)", '-TimeoutSec', '30', '-PollSec', '0.3')
    Check 'pid exit detected' ($r.Code -eq 0 -and $r.Json.met -contains "pid:$($proc.Id)" -and $r.Json.elapsed_sec -ge 1) $r.Raw
    $r = Run-Wait @('-Pid', "$($proc.Id),999999", '-TimeoutSec', '10', '-PollSec', '0.3')
    Check 'already-dead pids count as exited (comma list)' ($r.Code -eq 0) $r.Raw

    # 7. File appears later.
    $flag = Join-Path $tmp 'flag.txt'
    $r = Run-WaitMid @('-File', $flag, '-TimeoutSec', '60', '-PollSec', '0.3') { 'x' | Set-Content $flag }
    Check 'file appears later' ($r.Code -eq 0 -and $r.Json.met -contains 'file' -and $r.WasRunning) $r.Raw

    # 8. -Any versus -All: file exists now, a long-lived pid does not exit.
    $sleeper = Start-Process -FilePath powershell -ArgumentList '-NoProfile', '-Command', 'Start-Sleep -Seconds 40' -PassThru -WindowStyle Hidden
    try {
        $r = Run-Wait @('-Any', '-File', $np, '-Pid', "$($sleeper.Id)", '-TimeoutSec', '10', '-PollSec', '0.3')
        Check '-Any returns when one condition met' ($r.Code -eq 0 -and $r.Json.met -contains 'file' -and $r.Json.unmet -contains "pid:$($sleeper.Id)") $r.Raw
        $r = Run-Wait @('-All', '-File', $np, '-Pid', "$($sleeper.Id)", '-TimeoutSec', '2', '-PollSec', '0.3')
        Check '-All times out when one condition unmet' ($r.Code -eq 124 -and $r.Json.met -contains 'file') $r.Raw
        $r = Run-Wait @('-File', $np, '-Pid', "$($sleeper.Id)", '-TimeoutSec', '2', '-PollSec', '0.3')
        Check 'default is -All' ($r.Code -eq 124) $r.Raw
    } finally { Stop-Process -Id $sleeper.Id -Force -ErrorAction SilentlyContinue }

    # 9. Summary pattern and -Tail.
    $s = Join-Path $tmp 'sum.log'
    Write-Bytes $s (Enc-Bytes (New-Object System.Text.UnicodeEncoding($false, $true)) "noise 1`r`nPASS a`r`nnoise 2`r`nFAIL b`r`nDONE`r`n" $true)
    $r = Run-Wait @('-LogFile', $s, '-Pattern', '^DONE', '-TimeoutSec', '10', '-Tail', '2', '-SummaryPattern', '^(FAIL|PASS|DONE)')
    Check 'summary lines extracted' ((@($r.Json.summary) -join '|') -eq 'PASS a|FAIL b|DONE') $r.Raw
    Check '-Tail limits and decodes' ((@($r.Json.tail) -join '|') -eq 'FAIL b|DONE') $r.Raw

    # 10. Bad invocation -> 2 ERROR.
    $r = Run-Wait @('-TimeoutSec', '5')
    Check 'no condition is ERROR (2)' ($r.Code -eq 2 -and $r.Json.status -eq 'ERROR') $r.Raw
    $r = Run-Wait @('-File', $np)
    Check 'missing -TimeoutSec is ERROR (2)' ($r.Code -eq 2 -and $r.Json.status -eq 'ERROR') $r.Raw
    $r = Run-Wait @('-File', $np, '-TimeoutSec', '9000')
    Check '-TimeoutSec above 7200 is ERROR (2)' ($r.Code -eq 2) $r.Raw
    $r = Run-Wait @('-LogFile', $np, '-TimeoutSec', '5')
    Check '-LogFile without -Pattern is ERROR (2)' ($r.Code -eq 2) $r.Raw

    # 11. decode_log.ps1 prints UTF-8 for each encoding.
    foreach ($p in @($f, (Join-Path $tmp 'utf16le_no_bom.log'), (Join-Path $tmp 'utf16be_bom.log'), (Join-Path $tmp 'utf8_bom.log'))) {
        $o = & powershell -NoProfile -ExecutionPolicy Bypass -File $decoder $p
        Check "decode_log decodes $(Split-Path $p -Leaf)" ((($o -join "`n") -match 'DONE') -and (($o -join '') -notmatch "`0")) ($o -join '|')
    }
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

if ($script:fails -gt 0) { Write-Output "FAIL: $($script:fails) assertion(s) failed"; exit 1 }
Write-Output 'PASS: wait_for / decode_log'
exit 0
