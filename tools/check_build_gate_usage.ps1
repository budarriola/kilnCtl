# check_build_gate_usage.ps1 -- lints callers of tools/build_gate.ps1 against the
# owner rule (2026-10-07): a gate slot is held ONLY around the compile command.
#
# Per .ps1 file that calls Enter-KilnBuildGate (tools/build_gate.ps1 excluded):
#   1. every Enter-KilnBuildGate needs an Exit-KilnBuildGate (count >= enters);
#   2. the file must not Enter-KilnBuildGate textually BEFORE its first
#      Enter-BuildLock (a slot would be held while queued on the lock);
#   3. no Enter-KilnBuildGate textually before a vcvarsall invocation line
#      (setup must run outside the slot; use Import-KilnVcvarsEnv).
# Comment lines are ignored. Usage: powershell -File tools\check_build_gate_usage.ps1 [-Root <dir>]
param([string]$Root)
$ErrorActionPreference = "Stop"
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$gateFile = (Resolve-Path (Join-Path $PSScriptRoot "build_gate.ps1")).Path
$bad = @()
$seen = 0
$files = Get-ChildItem -LiteralPath $Root -Recurse -Filter *.ps1 -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '[\\/](\.git|node_modules|\.venv|build[^\\/]*)[\\/]' -and $_.FullName -ne $gateFile -and $_.FullName -ne $PSCommandPath }
foreach ($f in $files) {
    $lines = @(Get-Content -LiteralPath $f.FullName)
    $code = @()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*#') { continue }
        $code += [pscustomobject]@{ N = $i + 1; T = $lines[$i] }
    }
    $enters = @($code | Where-Object { $_.T -match 'Enter-KilnBuildGate\b' -and $_.T -notmatch '^\s*function ' })
    if ($enters.Count -eq 0) { continue }
    $seen++
    $exits = @($code | Where-Object { $_.T -match 'Exit-KilnBuildGate\b' })
    if ($exits.Count -lt $enters.Count) { $bad += "$($f.FullName): $($enters.Count) Enter-KilnBuildGate but only $($exits.Count) Exit-KilnBuildGate" }
    $lock = $code | Where-Object { $_.T -match 'Enter-BuildLock\b' -and $_.T -notmatch '^\s*function ' } | Select-Object -First 1
    if ($lock -and $enters[0].N -lt $lock.N) { $bad += "$($f.FullName):$($enters[0].N): Enter-KilnBuildGate before Enter-BuildLock (line $($lock.N)) -- a slot would be held while queued on the lock" }
    $vc = $code | Where-Object { $_.T -match 'call\s+`?"?\$vcvars|vcvarsall' -and $_.T -notmatch 'Import-KilnVcvarsEnv|\$vcvars\s*=|Test-Path' } | Select-Object -First 1
    if ($vc -and $enters[0].N -lt $vc.N) { $bad += "$($f.FullName):$($enters[0].N): Enter-KilnBuildGate before vcvarsall (line $($vc.N)) -- run vcvars via Import-KilnVcvarsEnv outside the slot" }
}
if ($bad.Count -gt 0) {
    $bad | ForEach-Object { Write-Host "FAIL: $_" -ForegroundColor Red }
    exit 1
}
if ($seen -eq 0) { Write-Host "FAIL: found no Enter-KilnBuildGate callers under $Root -- lint is vacuous"; exit 1 }
Write-Host "PASS: $seen build-gate callers hold a slot only around the compile."
exit 0
