# Start / stop / inspect the PcTools MCP servers (kilnctrl, kicad).
#
# These servers moved from stdio to HTTP so their lifetime stops being tied to
# whichever editor happened to launch them: one process owns the serial port,
# every client attaches to it, and it survives a client restart. That trade
# means somebody has to start and stop them -- this script, and the VS Code
# action buttons that call it.
#
#   .\mcp_servers.ps1 start            # both, if not already up
#   .\mcp_servers.ps1 status           # what is listening, and what it publishes
#   .\mcp_servers.ps1 stop  -Server kilnctrl
#   .\mcp_servers.ps1 restart          # after editing server code
#
# Stopping goes through the server's own POST /shutdown so it can release its
# serial port on the way out. A COM port left open by a killed process stays
# unusable on Windows until the device is replugged, so Stop-Process is the
# fallback, never the first move.

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("start", "stop", "restart", "status")]
    [string]$Action = "status",

    [ValidateSet("all", "kilnctrl", "kicad")]
    [string]$Server = "all",

    # Seconds to wait for a freshly started server to answer /health.
    [int]$TimeoutSec = 25
)

$ErrorActionPreference = "Stop"
$PcTools = Split-Path -Parent $PSScriptRoot
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PcTools)
$LogDir = Join-Path $PcTools "logs"

# Port 8765 is kilnctrl's link_hub (link_hub.py HUB_PORT) and is not free.
#
# `kicad` lives in the mykicadMcp submodule with its own venv and its own
# hand-rolled HTTP server, so it gets its own Venv/Script pair here rather than
# a console-script name. It answers the same /health and /shutdown routes as the
# other two, which is what lets one script drive all three.
$Servers = @(
    @{ Name = "kilnctrl"; Port = 8767; Venv = $PcTools; Exe = "kilnctrl-mcp-server.exe"; Module = "kilnctrl.mcp_server" }
    @{ Name = "kicad";    Port = 8766; Venv = (Join-Path $RepoRoot "tools\mykicadMcp"); Exe = $null;
       Script = (Join-Path $RepoRoot "tools\mykicadMcp\kicad_mcp_server.py") }
)

function Select-Servers {
    if ($Server -eq "all") { return $Servers }
    return $Servers | Where-Object { $_.Name -eq $Server }
}

function Get-Health($entry) {
    try {
        return Invoke-RestMethod -Method Get -TimeoutSec 3 `
            -Uri "http://127.0.0.1:$($entry.Port)/health"
    } catch {
        return $null
    }
}

function Show-Status($entry) {
    $health = Get-Health $entry
    if ($null -eq $health) {
        Write-Host ("  {0,-9} DOWN   port {1}" -f $entry.Name, $entry.Port) -ForegroundColor DarkYellow
        return $false
    }
    Write-Host ("  {0,-9} UP     http://127.0.0.1:{1}{2}  pid {3}  publishes: {4}" -f `
        $entry.Name, $entry.Port, $health.endpoint, $health.pid, ($health.published_tools -join ", ")) `
        -ForegroundColor Green
    return $true
}

function Start-One($entry) {
    if (Get-Health $entry) {
        Write-Host "  $($entry.Name) already up on port $($entry.Port)"
        return
    }
    $python = Join-Path $entry.Venv ".venv\Scripts\python.exe"
    $arguments = @("--transport", "http", "--port", "$($entry.Port)")

    if ($entry.Script) {
        # A plain script (mykicadMcp), not an installed package.
        if (-not (Test-Path $python)) {
            throw "No venv at $($entry.Venv)\.venv -- run tools/setup.ps1 first."
        }
        $exe = $python
        $arguments = @($entry.Script) + $arguments
    } else {
        # The console script is preferred, but `python -m` is the fallback that
        # always works: a venv created before an entry point was added to
        # pyproject.toml has the package importable and the .exe missing, and
        # "run setup again" is the wrong answer to a server that can start today.
        $exe = Join-Path $entry.Venv ".venv\Scripts\$($entry.Exe)"
        if (-not (Test-Path $exe)) {
            if (-not (Test-Path $python)) {
                throw "No venv at $($entry.Venv)\.venv -- run tools/setup.ps1 first."
            }
            $exe = $python
            $arguments = @("-m", $entry.Module) + $arguments
        }
    }
    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
    $log = Join-Path $LogDir "$($entry.Name)-mcp.log"
    # -WindowStyle Hidden, not -NoNewWindow: these outlive the shell that
    # started them, and a console window per server is noise the operator did
    # not ask for. Everything the server says goes to the log instead.
    Start-Process -FilePath $exe `
        -ArgumentList $arguments `
        -WindowStyle Hidden `
        -WorkingDirectory $RepoRoot `
        -RedirectStandardOutput $log `
        -RedirectStandardError "$log.err" | Out-Null

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Get-Health $entry) {
            Write-Host "  $($entry.Name) up on port $($entry.Port)  (log: $log)" -ForegroundColor Green
            return
        }
        Start-Sleep -Milliseconds 400
    }
    Write-Host "  $($entry.Name) did not answer /health within ${TimeoutSec}s -- see $log.err" `
        -ForegroundColor Red
}

function Stop-One($entry) {
    $health = Get-Health $entry
    if ($null -eq $health) {
        Write-Host "  $($entry.Name) already down"
        return
    }
    try {
        Invoke-RestMethod -Method Post -TimeoutSec 5 -Uri "http://127.0.0.1:$($entry.Port)/shutdown" | Out-Null
    } catch {
        # The server may drop the connection as it exits; that is a successful
        # stop, not a failure. The /health poll below is the real verdict.
        Write-Verbose "shutdown request to $($entry.Name) did not return cleanly: $_"
    }
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        if ($null -eq (Get-Health $entry)) {
            Write-Host "  $($entry.Name) stopped" -ForegroundColor Green
            return
        }
        Start-Sleep -Milliseconds 300
    }
    Write-Host "  $($entry.Name) ignored /shutdown; killing pid $($health.pid)" -ForegroundColor Yellow
    try { Stop-Process -Id $health.pid -Force -ErrorAction Stop } catch { Write-Host "    $_" }
}

$selected = Select-Servers
switch ($Action) {
    "start"   { Write-Host "starting:"; $selected | ForEach-Object { Start-One $_ } }
    "stop"    { Write-Host "stopping:"; $selected | ForEach-Object { Stop-One $_ } }
    "restart" {
        Write-Host "restarting:"
        $selected | ForEach-Object { Stop-One $_ }
        $selected | ForEach-Object { Start-One $_ }
    }
    "status"  {
        Write-Host "PcTools MCP servers:"
        $up = 0
        foreach ($entry in $selected) { if (Show-Status $entry) { $up++ } }
        if ($up -lt @($selected).Count) {
            Write-Host "`nStart them with: .\tools\PcTools\scripts\mcp_servers.ps1 start" -ForegroundColor DarkGray
        }
    }
}
