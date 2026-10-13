<#
.SYNOPSIS
  Join the kiln board's own Wi-Fi AP using the SSID and passphrase shown on
  its LCD.

.DESCRIPTION
  Captures a frame of the bench LCD with capture_lcd.ps1 -Full and reads the
  network with kilnctrl.lcd_ap_reader:

    -Mode recovery  The recovery image's SoftAP. The SSID and the 12-character
                    random per-boot passphrase are decoded from the WIFI: QR
                    code the recovery screen draws at the top right. If the
                    QR is missing or undecodable (older recovery image, glare,
                    no network yet) they are read by glyph segmentation
                    against the firmware's 5x7 font instead (never guessed; a
                    warning says so). 5 and S render near-identically, so
                    that fallback may return several candidates; each is
                    tried in turn. A QR read yields exactly one.
    -Mode normal    The normal firmware's AP fallback. SSID and password are
                    decoded from the WIFI: QR code on the Network page (QR
                    only, no fallback; needs an ADMIN LCD session).
                    The decoder is the zxing-cpp package, a declared PcTools
                    dependency (uv sync in tools/PcTools).

  Each candidate is tried through a temporary netsh WLAN profile (WPA2PSK/AES)
  whose XML is written to %TEMP% and deleted right after `netsh wlan add
  profile`; success is confirmed by `netsh wlan show interfaces` reporting the
  SSID connected. A failed candidate's profile is deleted before the next.

  The passphrase is never printed or logged. Output is the SSID, the index of
  the candidate that worked, and the connection state.

.PARAMETER Mode
  recovery (default) or normal.

.PARAMETER Image
  Read this saved -Full capture instead of capturing a new frame.

.PARAMETER DryRun
  Read the screen and print the SSID and glyph count only. Does not join.

.PARAMETER Disconnect
  Disconnect and delete the profile left by a previous join, then exit.

.PARAMETER ProfileName
  netsh profile name. Defaults to the SSID. With -Disconnect and no
  -ProfileName, the default recovery SSID kilnctl-recovery is used.

.PARAMETER Python
  Python interpreter with numpy and Pillow. Defaults to this tree's PcTools
  venv (a fresh worktree has none, so pass the main tree's).

.PARAMETER ReadAttempts
  Live captures to try before giving up on reading the screen (default 4).
  Ignored with -Image.

.PARAMETER ConnectTimeoutSec
  How long to wait for each candidate to connect (default 20).

.EXAMPLE
  .\join_board_ap.ps1 -DryRun
  .\join_board_ap.ps1
  .\join_board_ap.ps1 -Disconnect
#>
param(
    [ValidateSet('recovery', 'normal')]
    [string]$Mode = 'recovery',
    [string]$Image,
    [switch]$DryRun,
    [switch]$Disconnect,
    [string]$ProfileName,
    [int]$ReadAttempts = 4,
    [int]$ConnectTimeoutSec = 20,
    [string]$Python
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$venvPython = if ($Python) { $Python } else { Join-Path $repoRoot 'tools\PcTools\.venv\Scripts\python.exe' }
$netsh = Join-Path $env:SystemRoot 'System32\netsh.exe'

function Get-WlanState([string]$Ssid) {
    # Returns 'connected' only when the interface reports this SSID connected.
    $out = & $netsh wlan show interfaces 2>$null
    $state = $null
    $ifSsid = $null
    foreach ($line in $out) {
        if ($line -match '^\s*State\s*:\s*(.+?)\s*$') { $state = $Matches[1] }
        elseif ($line -match '^\s*SSID\s*:\s*(.+?)\s*$') { $ifSsid = $Matches[1] }
        if ($state -and $ifSsid) {
            if ($ifSsid -eq $Ssid) { return $state }
            $state = $null; $ifSsid = $null
        }
    }
    if ($state) { return $state }
    return 'not connected'
}

function Remove-WlanProfile([string]$Name) {
    & $netsh wlan delete profile name="$Name" 2>$null | Out-Null
}

function ConvertTo-XmlText([string]$s) {
    return [System.Security.SecurityElement]::Escape($s)
}

if ($Disconnect) {
    if (-not $ProfileName) { $ProfileName = 'kilnctl-recovery' }
    & $netsh wlan disconnect 2>$null | Out-Null
    Remove-WlanProfile $ProfileName
    Write-Output "profile $ProfileName deleted; state: $(Get-WlanState $ProfileName)"
    exit 0
}

if (-not (Test-Path $venvPython)) {
    Write-Error "PcTools venv python not found at $venvPython"
    exit 1
}

$tmp = [System.IO.Path]::GetTempPath()
$tag = [guid]::NewGuid().ToString('N')
$captureFile = $null
$candFile = Join-Path $tmp "kilnctl_ap_$tag.cand"
$xmlFile = Join-Path $tmp "kilnctl_ap_$tag.xml"

try {
    # A live read is frame-dependent (glyph segmentation, QR scale), so a failed
    # read is retried on a fresh capture. A fixed -Image is read once.
    $src = Join-Path $repoRoot 'tools\PcTools\src'
    $boot = 'import sys; sys.path.insert(0, sys.argv.pop(1)); from kilnctrl.lcd_ap_reader import main; sys.exit(main(sys.argv[1:]))'
    $attempts = if ($Image) { 1 } else { [Math]::Max(1, $ReadAttempts) }
    $read = $null
    for ($try = 1; $try -le $attempts; $try++) {
        if ($Image) {
            $imgPath = (Resolve-Path $Image).Path
        } else {
            if ($captureFile) { Remove-Item -LiteralPath $captureFile -Force -ErrorAction SilentlyContinue }
            $captureFile = Join-Path $tmp "kilnctl_ap_$tag.jpg"
            & (Join-Path $PSScriptRoot 'capture_lcd.ps1') -Full -Out $captureFile | Out-Null
            if (-not (Test-Path $captureFile)) { throw "capture_lcd.ps1 produced no frame" }
            $imgPath = $captureFile
        }
        # -I keeps a stray module in the cwd from shadowing the reader, and also
        # ignores PYTHONPATH, so the source dir goes on sys.path explicitly.
        $pyArgs = @('-I', '-c', $boot, $src, '--image', $imgPath, '--mode', $Mode)
        if (-not $DryRun) { $pyArgs += @('--candidates-out', $candFile) }
        $json = & $venvPython @pyArgs
        $rc = $LASTEXITCODE
        $read = ($json | Select-Object -Last 1) | ConvertFrom-Json
        if ($rc -eq 0 -and $read.ok) { break }
        if ($try -lt $attempts) { Write-Output "read attempt $try of $attempts failed: $($read.error); recapturing" }
    }
    if ($rc -ne 0 -or -not $read.ok) {
        Write-Output "read failed: $($read.error)"
        exit 2
    }
    foreach ($w in $read.warnings) { Write-Output "warning: $w" }
    $ssid = [string]$read.ssid
    if ($DryRun) {
        Write-Output "ssid: $ssid"
        Write-Output "glyph_count: $($read.glyph_count)"
        exit 0
    }
    if (-not $ProfileName) { $ProfileName = $ssid }

    $candidates = @(Get-Content -LiteralPath $candFile -Encoding UTF8 | Where-Object { $_ -ne '' })
    Remove-Item -LiteralPath $candFile -Force -ErrorAction SilentlyContinue
    Write-Output "ssid: $ssid ($($candidates.Count) candidate(s))"

    for ($i = 0; $i -lt $candidates.Count; $i++) {
        $xml = @"
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>$(ConvertTo-XmlText $ProfileName)</name>
  <SSIDConfig><SSID><name>$(ConvertTo-XmlText $ssid)</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM><security>
    <authEncryption><authentication>WPA2PSK</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>
    <sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>$(ConvertTo-XmlText $candidates[$i])</keyMaterial></sharedKey>
  </security></MSM>
</WLANProfile>
"@
        Remove-WlanProfile $ProfileName
        [System.IO.File]::WriteAllText($xmlFile, $xml, (New-Object System.Text.UTF8Encoding($false)))
        try {
            & $netsh wlan add profile filename="$xmlFile" user=current 2>$null | Out-Null
            $addRc = $LASTEXITCODE
        } finally {
            Remove-Item -LiteralPath $xmlFile -Force -ErrorAction SilentlyContinue
        }
        if ($addRc -ne 0) { throw "netsh wlan add profile failed (exit $addRc)" }
        & $netsh wlan connect name="$ProfileName" ssid="$ssid" 2>$null | Out-Null

        $state = 'not connected'
        $deadline = (Get-Date).AddSeconds($ConnectTimeoutSec)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Milliseconds 1000
            $state = Get-WlanState $ssid
            if ($state -eq 'connected') { break }
        }
        if ($state -eq 'connected') {
            Write-Output "candidate $($i + 1) of $($candidates.Count) worked; state: connected"
            Write-Output "profile $ProfileName kept; remove with -Disconnect"
            exit 0
        }
        Write-Output "candidate $($i + 1) of $($candidates.Count): state $state"
        Remove-WlanProfile $ProfileName
    }
    Write-Output "no candidate connected; state: $(Get-WlanState $ssid)"
    exit 3
} finally {
    $candidates = $null
    foreach ($f in @($candFile, $xmlFile, $captureFile)) {
        if ($f -and (Test-Path -LiteralPath $f)) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue }
    }
}
