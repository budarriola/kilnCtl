# bench_test.ps1 -- standardized, callable testing of the whole kilnCtl
# controller on the bench (docs/BENCH_TEST_SYSTEM_PLAN.md).
#
# A thin caller of the `kilnctrl` MCP server's bench_test_run tool over
# HTTP (port 8767, plan §2.1) -- the runner itself lives in plain Python
# under tools/PcTools/src/kilnctrl/bench_test/, this script only forwards
# arguments and translates the result into an exit-code contract.
#
# Sections 2.4/8 of the plan do not pin an exact exit-code scheme, so this
# is Wave 2's own choice -- four-way rather than run_all_checks.ps1's
# three-way, because "the board refused to even start" (a bad E-stop/link/
# crash state) and "the harness itself couldn't do its job" (unreachable
# MCP server, bad suite/case name) are different failure shapes an operator
# needs to tell apart at a glance:
#   0 -- every requested case PASSed (preflight ok, no FAIL/SKIP/
#        INCONCLUSIVE/NOT_RUN)
#   1 -- preflight passed but at least one case FAILed
#   2 -- the board's own preflight refused the run outright (RunOutcome.
#        exit_code in tools/PcTools/src/kilnctrl/bench_test/runner.py) --
#        no case was attempted
#   3 -- harness error (couldn't reach/parse the MCP server, an unknown
#        suite/case name, or a run that reached the board but left
#        something SKIP/INCONCLUSIVE/NOT_RUN with nothing FAILed -- plan
#        §6 rule 11's requirement that SKIP/INCONCLUSIVE never look like a
#        clean PASS)
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
    # Opt-in only: lets LCD-19's stop_gated sub-check start a real bench
    # firing to probe whether Stop is PIN-gated while heating (owner
    # decision 2026-09-30). Independent of -NoHeat/allow_heat -- LCD-19 is
    # not spec.heat-marked, so allow_heat alone never gates it; this is the
    # separate, default-off flag that does.
    [switch]$LcdStopHeat,
    [string]$Tag,
    [string]$ApPassword,
    [string]$HostAddr,
    [int]$Port = 8767,
    # A human is physically at the bench: operator-only cases (SP-08, SP-09,
    # WEB-WIFI-06) ask a yes/no question instead of SKIPping with reason
    # "requires --attended" (docs/BENCH_TEST_SYSTEM_PLAN.md §6, Wave 3b).
    [switch]$Attended,
    # Opts into FL-10/FL-11 (ESP/Pico JTAG flash round trip) -- independent
    # of -Attended, since a flash needs no operator present.
    [switch]$AllowFlash
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
    lcd_stop_heat = [bool]$LcdStopHeat
}
if ($Cases) { $toolArgs["cases"] = ($Cases -join ",") }
if ($Tag) { $toolArgs["tag"] = $Tag }
if ($ApPassword) { $toolArgs["ap_password"] = $ApPassword }
if ($HostAddr) { $toolArgs["host"] = $HostAddr }
if ($Attended) { $toolArgs["attended"] = [bool]$Attended }
if ($AllowFlash) { $toolArgs["allow_flash"] = [bool]$AllowFlash }

$jsonArgs = $toolArgs | ConvertTo-Json -Compress

$output = & $venvPython $clientScript "bench_test_run" $jsonArgs --port $Port 2>&1
$callExit = $LASTEXITCODE
Write-Host $output

if ($callExit -ne 0) {
    Write-Error "bench_test.ps1: could not reach bench_test_run on the kilnctrl MCP server (port $Port) -- is it running and fresh? .\tools\PcTools\scripts\mcp_servers.ps1 status"
    exit 3
}

$outputText = $output | Out-String

# A bad suite/case name (KeyError/ValueError, caught by
# mcp_server_bench_test.py) comes back as a plain "error: ..." string with
# no exit_code at all -- that is the harness failing to even start a run,
# not the board's own preflight refusing one.
if ($outputText -match "^error:") {
    exit 3
}

# The tool's own text reply carries "exit_code=N" (mcp_server_bench_test.py's
# bench_test_run()) -- parse that rather than re-deriving pass/fail here, so
# there is exactly one place (the Python runner) that decides verdicts.
if ($outputText -match "exit_code=(\d+)") {
    exit [int]$Matches[1]
}

Write-Error "bench_test.ps1: could not find an exit_code in bench_test_run's reply -- treating as a harness error"
exit 3
