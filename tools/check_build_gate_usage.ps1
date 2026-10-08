# check_build_gate_usage.ps1 -- lints callers of tools/build_gate.ps1 against the
# owner rule (2026-10-07): a gate slot is held ONLY around the compile command.
#
# Per .ps1 file that calls Enter-KilnBuildGate or Invoke-KilnGatedCmd
# (tools/build_gate.ps1 excluded):
#   0. each Enter-KilnBuildGate needs a later Exit-KilnBuildGate, and nothing
#      between the pair may wait (Start-Sleep), take a build lock, run
#      vcvarsall/Import-KilnVcvarsEnv, take another gate, or run a test .exe;
#   1. every Enter-KilnBuildGate needs an Exit-KilnBuildGate (count >= enters);
#   2. no gate use textually BEFORE the file's first Enter-BuildLock (a slot
#      would be held while queued on the lock);
#   3. no gate use textually before a vcvarsall invocation line (setup must run
#      outside the slot; use Import-KilnVcvarsEnv).
# Comment lines are ignored. Usage: powershell -File tools\check_build_gate_usage.ps1 [-Root <dir>]
param([string]$Root)
$ErrorActionPreference = "Stop"
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$gateFile = (Resolve-Path (Join-Path $PSScriptRoot "build_gate.ps1")).Path
$bad = @()
$seen = 0
$innerBad = 'Start-Sleep|Enter-BuildLock|vcvarsall|Import-KilnVcvarsEnv|Enter-KilnBuildGate|&\s*\$\w*[Ee]xe\b|&\s*["''][^"'']*\.exe["'']'
$files = Get-ChildItem -LiteralPath $Root -Recurse -Filter *.ps1 -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -notmatch '[\/](\.git|node_modules|\.venv|build[^\/]*)[\/]' -and $_.FullName -ne $gateFile -and $_.FullName -ne $PSCommandPath -and $_.Name -ne 'run_all_checks.ps1' }
foreach ($f in $files) {
    $lines = @(Get-Content -LiteralPath $f.FullName)
    $code = @()
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*#') { continue }
        $code += [pscustomobject]@{ N = $i + 1; T = $lines[$i] }
    }
    $uses = @($code | Where-Object { $_.T -match '(Enter-KilnBuildGate|Invoke-KilnGatedCmd)\b' -and $_.T -notmatch '^\s*function ' })
    if ($uses.Count -eq 0) { continue }
    $seen++
    $enters = @($uses | Where-Object { $_.T -match 'Enter-KilnBuildGate\b' })
    $exits = @($code | Where-Object { $_.T -match 'Exit-KilnBuildGate\b' })
    if ($exits.Count -lt $enters.Count) { $bad += "$($f.FullName): $($enters.Count) Enter-KilnBuildGate but only $($exits.Count) Exit-KilnBuildGate" }
    foreach ($e in $enters) {
        $x = $exits | Where-Object { $_.N -gt $e.N } | Select-Object -First 1
        if (-not $x) { $bad += "$($f.FullName):$($e.N): Enter-KilnBuildGate has no later Exit-KilnBuildGate"; continue }
        foreach ($l in @($code | Where-Object { $_.N -ge $e.N -and $_.N -le $x.N })) {
            $t = $l.T
            if ($l.N -eq $e.N) { $t = $t.Substring($t.IndexOf('Enter-KilnBuildGate') + 19) }   # text after the Enter token
            if ($l.N -eq $x.N) { $t = $t.Substring(0, $t.IndexOf('Exit-KilnBuildGate')) }      # text before the Exit token
            if ($t -match $innerBad -and $t -notmatch '&\s*["'']?cmd(\.exe)?["'']?\b') {
                $bad += "$($f.FullName):$($l.N): '$($l.T.Trim())' inside the Enter/Exit-KilnBuildGate pair at line $($e.N) -- a slot is held only around the compile"
            }
        }
    }
    $lock = $code | Where-Object { $_.T -match 'Enter-BuildLock\b' -and $_.T -notmatch '^\s*function ' } | Select-Object -First 1
    if ($lock -and $uses[0].N -lt $lock.N) { $bad += "$($f.FullName):$($uses[0].N): gate use before Enter-BuildLock (line $($lock.N)) -- a slot would be held while queued on the lock" }
    $vc = $code | Where-Object { $_.T -match 'call\s+`?"?\$vcvars|vcvarsall' -and $_.T -notmatch 'Import-KilnVcvarsEnv|\$vcvars\s*=|Test-Path' } | Select-Object -First 1
    if ($vc -and $uses[0].N -lt $vc.N) { $bad += "$($f.FullName):$($uses[0].N): gate use before vcvarsall (line $($vc.N)) -- run vcvars via Import-KilnVcvarsEnv outside the slot" }
}
if ($bad.Count -gt 0) {
    $bad | ForEach-Object { Write-Host "FAIL: $_" -ForegroundColor Red }
    exit 1
}
if ($seen -eq 0) { Write-Host "FAIL: found no build-gate callers under $Root -- lint is vacuous"; exit 1 }
Write-Host "PASS: $seen build-gate callers hold a slot only around the compile."
exit 0
