# checkcache: ok
# check_gate_negative_test_table.ps1 -- every check that run_all_checks.ps1
# discovers must have a row in docs/audits/GATE_NEGATIVE_TEST_EVIDENCE.md
# (docs/RELEASE_HARDENING_PLAN.md section 3, acceptance step 4), and every
# row must name a check that still exists.
#
# A row whose status is "NOT AUDITED" is allowed: the point is that a gate is
# never silently absent from the evidence table, not that it is already
# negative-tested. A new check may therefore start as NOT AUDITED.
#
# Discovery is taken from `run_all_checks.ps1 -ListOnly -AllowFewerChecks`
# itself, not re-implemented here, so this table cannot drift from the runner.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$runner = Join-Path $repoRoot "tools\run_all_checks.ps1"
$table = Join-Path $repoRoot "docs\audits\GATE_NEGATIVE_TEST_EVIDENCE.md"

if (-not (Test-Path $table)) { Write-Host "FAIL: $table missing" -ForegroundColor Red; exit 1 }

$out = & powershell -NoProfile -ExecutionPolicy Bypass -File $runner -ListOnly -AllowFewerChecks 2>&1 | Out-String
$discovered = @()
foreach ($l in ($out -split "`r?`n")) {
    if ($l -match '^\s{2}(\S.*\.(?:ps1|py|js))\s*$') { $discovered += ($Matches[1] -replace '\\', '/') }
}
if ($discovered.Count -lt 50) {
    Write-Host "FAIL: only $($discovered.Count) checks parsed from -ListOnly output (expected >= 50); parser or runner changed" -ForegroundColor Red
    exit 1
}

$rows = @{}
foreach ($l in (Get-Content $table)) {
    if ($l -match '^\|\s*`([^`]+)`\s*\|\s*([^|]+?)\s*\|') { $rows[$Matches[1]] = $Matches[2] }
}
$valid = 'NEGATIVE-TESTED', 'PARTIAL', 'REVIEWED, NOT MUTATED', 'NOT AUDITED'
$bad = @()
foreach ($d in $discovered) {
    if (-not $rows.ContainsKey($d)) { $bad += "no row for discovered check: $d" }
}
foreach ($k in $rows.Keys) {
    if ($discovered -notcontains $k -and -not (Test-Path (Join-Path $repoRoot $k))) { $bad += "row names a check that no longer exists: $k" }
    if ($valid -notcontains $rows[$k]) { $bad += "row has unknown status '$($rows[$k])': $k" }
}
# The header's "Gate rows: N." and its per-status "## Counts" table must equal what the rows say.
$tableText = (Get-Content $table -Raw)
$tm = [regex]::Match($tableText, '(?m)^Gate rows:\s*(\d+)\.')
if (-not $tm.Success) { $bad += "header has no 'Gate rows: N.' line" }
elseif ([int]$tm.Groups[1].Value -ne $rows.Count) { $bad += "header says 'Gate rows: $($tm.Groups[1].Value)' but the table has $($rows.Count) rows" }
$cm = [regex]::Match($tableText, '(?ms)^## Counts\s*$(.*?)^## ')
if (-not $cm.Success) { $bad += "no '## Counts' section" }
else {
    $declared = @{}
    foreach ($l in ($cm.Groups[1].Value -split "`r?`n")) {
        if ($l -match '^\|\s*([^|]+?)\s*\|\s*(\d+)\s*\|\s*$') { $declared[$Matches[1]] = [int]$Matches[2] }
    }
    $actual = @{}
    foreach ($k in $rows.Keys) { $actual[$rows[$k]] = 1 + [int]$actual[$rows[$k]] }
    foreach ($st in (@($declared.Keys) + @($actual.Keys) | Sort-Object -Unique)) {
        if ($st -eq 'Status') { continue }
        $d = [int]$declared[$st]; $a2 = [int]$actual[$st]
        if ($d -ne $a2) { $bad += "Counts table says $d for '$st' but the rows have $a2" }
    }
}
if ($bad.Count -gt 0) {
    Write-Host "FAIL: GATE_NEGATIVE_TEST_EVIDENCE.md out of sync with discovered checks:" -ForegroundColor Red
    $bad | Sort-Object | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    Write-Host "Add a row (NOT AUDITED is allowed) or remove the stale one." -ForegroundColor Red
    exit 1
}
Write-Host "OK: $($discovered.Count) discovered checks all have a row ($($rows.Count) rows)."
exit 0
