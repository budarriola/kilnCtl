# bench_test.ps1 -- standardized, callable testing of the whole kilnCtl
# controller on the bench (docs/BENCH_TEST_SYSTEM_PLAN.md).
#
# A thin caller of the `kilnctrl` MCP server's bench_test_run tool over
# HTTP (port 8767, plan §2.1) -- the runner itself lives in plain Python
# under tools/PcTools/src/kilnctrl/bench_test/, this script only forwards
# arguments and translates the result into the same three-way exit-code
# contract tools/run_all_checks.ps1 uses:
#   0 -- every requested case PASSed
#   3 -- nothing FAILed, but something was SKIP/INCONCLUSIVE/NOT_RUN
#   1 -- otherwise (a FAIL, a preflight failure, or the call itself failed)
#
# Requires the `kilnctrl` MCP server to be running and fresh (see
# tools\PcTools\scripts\mcp_servers.ps1 status). If bench_test_run doesn't
# appear yet, the server needs restarting after this wave was added --
# see docs/MCP_SERVERS.md "Stale-server self-announcing".
param(
    [Parameter(Mandatory = $true)][string]$Suite,
    [string[]]$Cases,
    [switch]$DryRun,
    [switch]$NoHeat,
    [string]$Tag,
    [string]$ApPassword,
    [string]$HostAddr,
    [int]$Port = 8767
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$venvPython = Join-Path $repoRoot "tools\PcTools\.venv\Scripts\python.exe"
$clientScript = Join-Path $repoRoot "tools\PcTools\scripts\bench_test_client.py"

if (-not (Test-Path $venvPython)) {
    Write-Error "bench_test.ps1: venv python not found at $venvPython -- run tools/setup.ps1 first"
    exit 1
}

$toolArgs = @{
    suite      = $Suite
    dry_run    = [bool]$DryRun
    allow_heat = -not [bool]$NoHeat
}
if ($Cases) { $toolArgs["cases"] = ($Cases -join ",") }
if ($Tag) { $toolArgs["tag"] = $Tag }
if ($ApPassword) { $toolArgs["ap_password"] = $ApPassword }
if ($HostAddr) { $toolArgs["host"] = $HostAddr }

$jsonArgs = $toolArgs | ConvertTo-Json -Compress

$output = & $venvPython $clientScript "bench_test_run" $jsonArgs --port $Port 2>&1
$callExit = $LASTEXITCODE
Write-Host $output

if ($callExit -ne 0) {
    Write-Error "bench_test.ps1: could not reach bench_test_run on the kilnctrl MCP server (port $Port) -- is it running and fresh? .\tools\PcTools\scripts\mcp_servers.ps1 status"
    exit 1
}

# The tool's own text reply carries "exit_code=N" (mcp_server_bench_test.py's
# bench_test_run()) -- parse that rather than re-deriving pass/fail here, so
# there is exactly one place (the Python runner) that decides verdicts.
if ($output -match "exit_code=(\d+)") {
    exit [int]$Matches[1]
}

Write-Error "bench_test.ps1: could not find an exit_code in bench_test_run's reply -- treating as a hard failure"
exit 1
