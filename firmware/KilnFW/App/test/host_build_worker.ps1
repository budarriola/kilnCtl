# Child process for build_host_tests.ps1's parallel compile pool: runs ONE
# compile/link command line (read from -CmdFile) under ONE heavy build-gate
# slot via Invoke-KilnGatedCmd, then exits with the command's exit code. It is
# a separate PROCESS (not a runspace) because the gate's re-entrancy detection
# uses process-wide state ($env:KILNCTL_BUILD_GATE_HELD, $PID); the vcvars
# environment is inherited from the parent process.
param([Parameter(Mandatory)][string]$CmdFile, [Parameter(Mandatory)][string]$Label)
$ErrorActionPreference = "Continue"
. (Join-Path $PSScriptRoot "../../../../tools/build_gate.ps1")
$command = (Get-Content -Raw -LiteralPath $CmdFile).Trim()
Invoke-KilnGatedCmd -Label $Label -Command $command
exit $LASTEXITCODE
