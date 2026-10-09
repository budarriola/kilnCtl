# decode_log.ps1 -- print a text file as UTF-8 whatever its encoding
# (UTF-16LE/BE or UTF-8, with or without a BOM). Lets grep work on logs
# written by PowerShell `*>`:
#   powershell -ExecutionPolicy Bypass -File tools/decode_log.ps1 x.log | grep FAIL
param([Parameter(Mandatory = $true, Position = 0)][string]$Path)
$ErrorActionPreference = 'Stop'
$b = [System.IO.File]::ReadAllBytes($Path)
$skip = 0
if ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) { $enc = New-Object System.Text.UTF8Encoding($false); $skip = 3 }
elseif ($b.Length -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE) { $enc = New-Object System.Text.UnicodeEncoding($false, $false); $skip = 2 }
elseif ($b.Length -ge 2 -and $b[0] -eq 0xFE -and $b[1] -eq 0xFF) { $enc = New-Object System.Text.UnicodeEncoding($true, $false); $skip = 2 }
else {
    $lim = [math]::Min($b.Length, 64); $even = 0; $odd = 0
    for ($i = 0; $i -lt $lim; $i++) { if ($b[$i] -eq 0) { if ($i % 2 -eq 0) { $even++ } else { $odd++ } } }
    if ($odd -ge 2 -and $odd -gt 2 * $even) { $enc = New-Object System.Text.UnicodeEncoding($false, $false) }
    elseif ($even -ge 2 -and $even -gt 2 * $odd) { $enc = New-Object System.Text.UnicodeEncoding($true, $false) }
    else { $enc = New-Object System.Text.UTF8Encoding($false) }
}
$s = $enc.GetString($b, $skip, $b.Length - $skip)
$out = [Console]::OpenStandardOutput()
$ob = (New-Object System.Text.UTF8Encoding($false)).GetBytes($s)
$out.Write($ob, 0, $ob.Length)
$out.Flush()
