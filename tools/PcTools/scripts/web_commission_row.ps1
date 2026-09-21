# web_commission_row.ps1 -- thin wrapper around
# tools/PcTools/src/kilnctrl/web_commission_row.py for the M18 web-UI
# commissioning class (docs/COMMISSIONING_WEB_RUNBOOK.md).
#
# -DryRun validates a row's page + selector against SOURCE only -- no
# network, no board, no browser. This is the only mode exercised while the
# board is held by another session.
#
# Live mode (no -DryRun) logs into the real board once per invocation
# (credentials from KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD, User scope),
# drives headless Chrome over CDP for the named row, screenshots to
# -ScreenshotDir, and prints PASS/FAIL with the read-back.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\PcTools\scripts\web_commission_row.ps1 -Row W28 -DryRun
#   powershell -ExecutionPolicy Bypass -File tools\PcTools\scripts\web_commission_row.ps1 -Row W28 -Host 192.168.1.156

param(
    [Parameter(Mandatory = $true)][string]$Row,
    [switch]$DryRun,
    [string]$BoardHost,
    [string]$ScreenshotDir = (Join-Path $PSScriptRoot '..\..\..\logs\web_commission')
)

$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..\..')
$venvPython = Join-Path $repoRoot 'tools\PcTools\.venv\Scripts\python.exe'
if (-not (Test-Path $venvPython)) {
    Write-Error "PcTools venv python not found at $venvPython"
    exit 2
}

$modulePath = Join-Path $repoRoot 'tools\PcTools\src\kilnctrl\web_commission_row.py'

$pyArgs = @($modulePath, $Row)
if ($DryRun) {
    $pyArgs += '--dry-run'
} else {
    if (-not $BoardHost) {
        Write-Error 'live mode requires -BoardHost'
        exit 2
    }
    $pyArgs += @('--host', $BoardHost, '--screenshot-dir', $ScreenshotDir)
    # Credentials are read from the environment by the Python module itself
    # (KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD, User scope) -- never passed
    # on this command line, per docs/agent_rules/COMMON.md.
    $env:KILNCTL_WEB_USERNAME = [Environment]::GetEnvironmentVariable('KILNCTL_WEB_USERNAME', 'User')
    $env:KILNCTL_WEB_PASSWORD = [Environment]::GetEnvironmentVariable('KILNCTL_WEB_PASSWORD', 'User')
}

& $venvPython $pyArgs
exit $LASTEXITCODE
