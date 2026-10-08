# checkcache: ok
# check_monocypher_vendored.ps1 -- the vendored Monocypher files under
# firmware/KilnFW/App/drivers/update/third_party/monocypher/ must stay byte-identical to the
# upstream release recorded in that directory's README.md (sha256 table). Re-vendor to update.
# Exit 0 pass, 1 fail.
param([string]$Dir)
$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $Dir) { $Dir = Join-Path $repoRoot "firmware\KilnFW\App\drivers\update\third_party\monocypher" }
$readme = Join-Path $Dir "README.md"
if (-not (Test-Path -LiteralPath $readme)) { Write-Host "FAIL: $readme missing"; exit 1 }
$rows = @([regex]::Matches((Get-Content -LiteralPath $readme -Raw), '(?m)^\| (\S+) \| \S+ \| ([0-9a-f]{64}) \|') | ForEach-Object { , @($_.Groups[1].Value, $_.Groups[2].Value) })
if ($rows.Count -lt 5) { Write-Host "FAIL: README lists $($rows.Count) hashed files, expected 5 (vacuous?)"; exit 1 }
$bad = 0
foreach ($r in $rows) {
    $p = Join-Path $Dir $r[0]
    if (-not (Test-Path -LiteralPath $p)) { Write-Host "FAIL: $($r[0]) missing"; $bad++; continue }
    $h = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower()
    if ($h -ne $r[1]) { Write-Host "FAIL: $($r[0]) sha256 $h != recorded $($r[1])"; $bad++ }
}
# A source/header file in the vendored directory that the README does not hash is unpinned code
# (vacuity audit 2026-10-07: an extra monocypher_x.c passed).
$listed = @($rows | ForEach-Object { $_[0] })
foreach ($f in @(Get-ChildItem -LiteralPath $Dir -File | Where-Object { $_.Extension -in '.c', '.h', '.S', '.s' })) {
    if ($listed -notcontains $f.Name) { Write-Host "FAIL: $($f.Name) is in the vendored directory but not in the README sha256 table"; $bad++ }
}
if ($bad) { exit 1 }
Write-Host "PASS: $($rows.Count) vendored Monocypher files match upstream 4.0.3 hashes"
exit 0
