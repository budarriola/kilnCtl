# wait_for.ps1 -- bounded, encoding-aware waiter. Use this instead of a
# hand-written `until grep ...; do sleep; done` loop (which misses matches in
# UTF-16 logs written by PowerShell `*>` and can spin forever).
#
# Conditions (any combination; -All is the default, -Any ends on the first):
#   -Pid 123,456            all listed processes have exited
#   -LogFile f -Pattern re  a line of the decoded log matches the regex
#   -File f                 the file exists
# -TimeoutSec N (required, 1..7200)   -PollSec S (default 15)
# -Tail N (default 20)                -SummaryPattern re (lines of the final log)
#
# Encoding of the log is re-detected on every read: UTF-16LE/BE and UTF-8,
# each with or without a BOM. Read incrementally. A missing log is tolerated
# until it appears or the deadline passes.
#
# First stdout line: WAIT_START {json} (conditions accepted; the waiter is running).
# Last stdout line:  WAIT_RESULT {json}
# Exit: 0 MET, 124 TIMEOUT, 2 ERROR.
#
# From Bash: powershell -ExecutionPolicy Bypass -File tools/wait_for.ps1 -TimeoutSec 600 -LogFile x.log -Pattern '^DONE'

param(
    [Alias('Pid')][string[]]$WaitPid,
    [string]$LogFile,
    [string]$Pattern,
    [string]$File,
    [switch]$Any,
    [switch]$All,
    [double]$TimeoutSec = 0,
    [double]$PollSec = 15,
    [int]$Tail = 20,
    [string]$SummaryPattern
)

$ErrorActionPreference = 'Stop'
$sw = [System.Diagnostics.Stopwatch]::StartNew()

function Emit([string]$status, $met, $unmet, $matched, $tail, $summary, $err) {
    $o = [ordered]@{
        status       = $status
        met          = @($met)
        unmet        = @($unmet)
        elapsed_sec  = [math]::Round($sw.Elapsed.TotalSeconds, 2)
        matched_line = $matched
        tail         = @($tail)
        summary      = @($summary)
    }
    if ($err) { $o['error'] = $err }
    Write-Output ('WAIT_RESULT ' + ($o | ConvertTo-Json -Compress -Depth 4))
    if ($status -eq 'MET') { exit 0 } elseif ($status -eq 'TIMEOUT') { exit 124 } else { exit 2 }
}

# Encoding from the first bytes of the file: Enc, Skip (BOM length), Name.
function Get-LogEncoding([byte[]]$b) {
    $n = $b.Length
    if ($n -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) { return @{ Enc = (New-Object System.Text.UTF8Encoding($false)); Skip = 3; Name = 'utf8' } }
    if ($n -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE) { return @{ Enc = (New-Object System.Text.UnicodeEncoding($false, $false)); Skip = 2; Name = 'utf16le' } }
    if ($n -ge 2 -and $b[0] -eq 0xFE -and $b[1] -eq 0xFF) { return @{ Enc = (New-Object System.Text.UnicodeEncoding($true, $false)); Skip = 2; Name = 'utf16be' } }
    # No BOM: ASCII-heavy UTF-16 has a NUL in every other byte.
    $lim = [math]::Min($n, 64); $even = 0; $odd = 0
    for ($i = 0; $i -lt $lim; $i++) { if ($b[$i] -eq 0) { if ($i % 2 -eq 0) { $even++ } else { $odd++ } } }
    if ($odd -ge 2 -and $odd -gt 2 * $even) { return @{ Enc = (New-Object System.Text.UnicodeEncoding($false, $false)); Skip = 0; Name = 'utf16le' } }
    if ($even -ge 2 -and $even -gt 2 * $odd) { return @{ Enc = (New-Object System.Text.UnicodeEncoding($true, $false)); Skip = 0; Name = 'utf16be' } }
    return @{ Enc = (New-Object System.Text.UTF8Encoding($false)); Skip = 0; Name = 'utf8' }
}

# Reads bytes [from, EOF) with a share mode that tolerates a writer.
function Read-Bytes([string]$path, [long]$from) {
    $fs = New-Object System.IO.FileStream($path, 'Open', 'Read', 'ReadWrite,Delete')
    try {
        $len = $fs.Length
        if ($from -gt $len) { $from = 0 }
        $fs.Seek($from, 'Begin') | Out-Null
        $buf = New-Object byte[] ($len - $from)
        $got = 0
        while ($got -lt $buf.Length) {
            $r = $fs.Read($buf, $got, $buf.Length - $got)
            if ($r -le 0) { break }
            $got += $r
        }
        if ($got -ne $buf.Length) { [Array]::Resize([ref]$buf, $got) }
        return , $buf
    } finally { $fs.Dispose() }
}

function Read-WholeLog([string]$path) {
    $b = Read-Bytes $path 0
    if ($b.Length -eq 0) { return , @() }
    $e = Get-LogEncoding $b
    $s = $e.Enc.GetString($b, $e.Skip, $b.Length - $e.Skip)
    $lines = @($s -split "`r?`n")
    if ($lines.Count -gt 0 -and $lines[-1] -eq '') { $lines = @($lines | Select-Object -First ($lines.Count - 1)) }
    return , $lines
}

# --- validation ---------------------------------------------------------
$pids = @()
foreach ($p in @($WaitPid)) { if ($p) { foreach ($x in ($p -split '[,\s]+')) { if ($x) { $pids += $x } } } }
try { $pids = @($pids | ForEach-Object { [int]$_ }) } catch { Emit 'ERROR' @() @() $null @() @() "bad -Pid value: $($_.Exception.Message)" }
if ($TimeoutSec -le 0 -or $TimeoutSec -gt 7200) { Emit 'ERROR' @() @() $null @() @() '-TimeoutSec is required and must be in (0, 7200]' }
if ($PollSec -le 0) { Emit 'ERROR' @() @() $null @() @() '-PollSec must be > 0' }
if ($Any -and $All) { Emit 'ERROR' @() @() $null @() @() '-Any and -All are mutually exclusive' }
if ($LogFile -and -not $Pattern) { Emit 'ERROR' @() @() $null @() @() '-LogFile requires -Pattern' }
if ($Pattern -and -not $LogFile) { Emit 'ERROR' @() @() $null @() @() '-Pattern requires -LogFile' }
if (-not ($pids.Count -or $LogFile -or $File)) { Emit 'ERROR' @() @() $null @() @() 'no condition given (-Pid, -LogFile/-Pattern, -File)' }
try { if ($Pattern) { [void][regex]::new($Pattern) }; if ($SummaryPattern) { [void][regex]::new($SummaryPattern) } } catch { Emit 'ERROR' @() @() $null @() @() "bad regex: $($_.Exception.Message)" }

$names = @()
foreach ($p in $pids) { $names += "pid:$p" }
if ($LogFile) { $names += 'log' }
if ($File) { $names += 'file' }

# --- state --------------------------------------------------------------
$script:metSet = @{}
$script:matchedLine = $null
$script:offset = [long]0
$script:decoder = $null
$script:encName = $null
$script:carry = ''
$rx = if ($Pattern) { [regex]::new($Pattern) } else { $null }

function Poll-Log {
    if ($script:metSet.ContainsKey('log') -or -not (Test-Path -LiteralPath $LogFile -PathType Leaf)) { return }
    $all = Read-Bytes $LogFile 0
    if ($all.Length -eq 0) { return }
    if ($all.Length -lt $script:offset) { $script:offset = 0; $script:encName = $null }  # truncated/rotated
    # Re-detect encoding from the head of the file on every read.
    $e = Get-LogEncoding $all
    if ($e.Name -ne $script:encName) {
        $script:encName = $e.Name; $script:offset = [long]$e.Skip; $script:carry = ''
        $script:decoder = $e.Enc.GetDecoder()
    }
    if ($all.Length -le $script:offset) { return }
    $count = $all.Length - $script:offset
    $chars = New-Object char[] ($script:decoder.GetCharCount($all, [int]$script:offset, $count) + 4)
    $n = $script:decoder.GetChars($all, [int]$script:offset, $count, $chars, 0)
    $script:offset = $all.Length
    $text = $script:carry + (New-Object string ($chars, 0, $n))
    $parts = @($text -split "`r?`n")
    $script:carry = $parts[-1]
    foreach ($ln in $parts) {
        if ($rx.IsMatch($ln)) { $script:matchedLine = $ln; $script:metSet['log'] = $true; return }
    }
}

function Poll-All {
    foreach ($p in $pids) {
        $k = "pid:$p"
        if ($script:metSet.ContainsKey($k)) { continue }
        $alive = $false
        try { $null = Get-Process -Id $p -ErrorAction Stop; $alive = $true } catch { $alive = $false }
        if (-not $alive) { $script:metSet[$k] = $true }
    }
    if ($File -and -not $script:metSet.ContainsKey('file') -and (Test-Path -LiteralPath $File)) { $script:metSet['file'] = $true }
    if ($LogFile) { try { Poll-Log } catch { } }   # transient share/IO errors: retry next poll
}

function Is-Done {
    if ($Any) { return ($script:metSet.Count -gt 0) }
    return ($script:metSet.Count -eq $names.Count)
}

function Finish([string]$status) {
    $tailLines = @(); $sumLines = @()
    if ($LogFile -and (Test-Path -LiteralPath $LogFile -PathType Leaf)) {
        try {
            $lines = Read-WholeLog $LogFile
            if ($Tail -gt 0 -and $lines.Count -gt 0) { $tailLines = @($lines | Select-Object -Last $Tail) }
            if ($SummaryPattern) { $sumLines = @($lines | Where-Object { $_ -match $SummaryPattern }) }
        } catch { }
    }
    $met = @($names | Where-Object { $script:metSet.ContainsKey($_) })
    $unmet = @($names | Where-Object { -not $script:metSet.ContainsKey($_) })
    Emit $status $met $unmet $script:matchedLine $tailLines $sumLines $null
}

Write-Output ('WAIT_START ' + (@{ conditions = $names; timeout_sec = $TimeoutSec; poll_sec = $PollSec } | ConvertTo-Json -Compress))
[Console]::Out.Flush()

# --- wait ---------------------------------------------------------------
try {
    while ($true) {
        Poll-All
        if (Is-Done) { Finish 'MET' }
        $remaining = $TimeoutSec - $sw.Elapsed.TotalSeconds
        if ($remaining -le 0) { Finish 'TIMEOUT' }
        Start-Sleep -Milliseconds ([int]([math]::Min($PollSec, $remaining) * 1000))
    }
} catch {
    Emit 'ERROR' @() @() $null @() @() $_.Exception.Message
}
