# check_partition_labels_vs_firmware.ps1 -- do partitions.csv and the firmware
# agree about which partitions exist and what kind of thing lives in each?
#
# WHY THIS IS SEPARATE FROM check_flash_partition_map.ps1: that script reads
# partitions.csv and checks it against ITSELF (no overlaps, nothing past the
# chip) and against a hardcoded expected map. Both of those can pass perfectly
# while the firmware looks up a partition label that is not in the table at
# all, or opens a `spiffs` partition with the NVS API. Neither the build nor
# the host tests catch that: esp_partition_find_first() and
# nvs_flash_init_partition() take the label as a plain string and fail at
# RUNTIME, on the board, usually as a quiet ESP_ERR_NOT_FOUND in a boot log
# nobody is reading. That failure mode is exactly the "reset one side of a
# pair" class CLAUDE.md documents: two pieces of state (the CSV row and the
# string literal in the .c file) joined by a contract nothing expresses as a
# shared type.
#
# It checks BOTH directions:
#
#   forward  -- every partition label the firmware looks up must exist in
#               partitions.csv, with a subtype compatible with the API used to
#               open it (an *_NVS_PARTITION define must name an `nvs`-subtype
#               row; a .partition_label passed to esp_vfs_spiffs_register must
#               name a `spiffs` row; likewise littlefs).
#
#   reverse  -- every mountable data partition in the CSV (subtype nvs /
#               spiffs / littlefs) must be referenced by at least one firmware
#               source file, OR be listed in $DeclaredButUnused below with a
#               reason. A partition nothing mounts is either dead flash or a
#               half-landed feature; both are worth failing on rather than
#               discovering months later.
#
# Usage:
#   powershell -File firmware\KilnFW\App\test\check_partition_labels_vs_firmware.ps1
#   powershell -File ... -CsvPath <path> -SourceRoot <dir>
#
# -CsvPath / -SourceRoot exist so this script's negative tests can point it at
# a deliberately broken copy without touching the real tree (per
# feedback_negative_test_restore_by_hand -- a negative test must never need a
# `git checkout --` to undo). Negative-tested 2026-09-07 four ways: a CSV row
# renamed, a row's subtype changed, a firmware label typo'd, and the `cfg`
# allowlist entry removed. All four failed with the specific diagnostic.

param(
    [string]$CsvPath,
    [string]$SourceRoot
)

$ErrorActionPreference = "Stop"

# $PSScriptRoot = ...\firmware\KilnFW\App\test
$appRoot    = Split-Path -Parent $PSScriptRoot          # ...\firmware\KilnFW\App
$kilnfwRoot = Split-Path -Parent $appRoot               # ...\firmware\KilnFW

if (-not $CsvPath)    { $CsvPath    = Join-Path $kilnfwRoot "partitions.csv" }
if (-not $SourceRoot) { $SourceRoot = Join-Path $appRoot "drivers" }

if (-not (Test-Path $CsvPath))    { throw "check_partition_labels_vs_firmware.ps1: partitions CSV not found at $CsvPath" }
if (-not (Test-Path $SourceRoot)) { throw "check_partition_labels_vs_firmware.ps1: source root not found at $SourceRoot" }

# --- Partitions that legitimately exist in the table with no firmware
#     reference yet. Each needs a reason and, ideally, the step that removes
#     it. Adding a label here is the deliberate act that makes the reverse
#     check pass; forgetting to remove it is the only way this list rots, so
#     keep the "remove when" clause concrete. ---
$DeclaredButUnused = @{
    'cfg' = 'Added 2026-09-07 as the flash-layout half of the user-data-on-a-filesystem decision (docs/FILESYSTEM_PLAN.md). No mount call exists yet -- FILESYSTEM_USER_DATA_PLAN.md section 5 step 1 adds cfg_fs_mount(); REMOVE THIS ENTRY in that step.'
    'nvs' = 'The stock default NVS partition, kept unresized on purpose so pre-2026-08-13 firmware still finds working data there and the one-time split migration has somewhere to read from (see partitions.csv). Current firmware never opens it by name. Do not remove the partition; this entry stays until that rollback window is closed.'
}

# ---------------------------------------------------------------- CSV parse --
function ConvertFrom-PartitionSize {
    param([string]$Text)
    $t = $Text.Trim()
    if ($t -match '^0[xX][0-9a-fA-F]+$') { return [Convert]::ToInt64($t, 16) }
    if ($t -match '^\d+[kK]$')           { return [long]($t.Substring(0, $t.Length - 1)) * 1KB }
    if ($t -match '^\d+[mM]$')           { return [long]($t.Substring(0, $t.Length - 1)) * 1MB }
    if ($t -match '^\d+$')               { return [long]$t }
    throw "check_partition_labels_vs_firmware.ps1: cannot parse numeric field '$Text'"
}

$partitions = @{}
foreach ($line in (Get-Content -Path $CsvPath)) {
    $trimmed = $line.Trim()
    if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) { continue }
    $f = $trimmed.Split(',') | ForEach-Object { $_.Trim() }
    if ($f.Count -lt 5) { throw "check_partition_labels_vs_firmware.ps1: malformed row: '$line'" }
    $partitions[$f[0]] = [PSCustomObject]@{
        Name    = $f[0]
        Type    = $f[1]
        SubType = $f[2]
        Offset  = ConvertFrom-PartitionSize $f[3]
        Size    = ConvertFrom-PartitionSize $f[4]
    }
}
if ($partitions.Count -eq 0) { throw "check_partition_labels_vs_firmware.ps1: no partition rows found in $CsvPath" }

# ------------------------------------------------------------ source sweep --
# One reference = (label, required subtype or $null for "must merely exist",
# source file, why). Collected by scanning each .c/.h's full text, because
# some call sites (esp_partition_find_first) wrap across lines.
$refs = @()
$sourceFiles = Get-ChildItem -Path $SourceRoot -Recurse -File -Include *.c, *.h |
    Where-Object { $_.FullName -notmatch '[\\/](test|build|managed_components)[\\/]' }

foreach ($file in $sourceFiles) {
    $text = Get-Content -Path $file.FullName -Raw
    if (-not $text) { continue }
    # Report paths relative to firmware/KilnFW when the file is under it (the
    # normal case); a negative test pointing -SourceRoot elsewhere keeps the
    # absolute path rather than getting a mangled substring.
    if ($file.FullName.StartsWith($kilnfwRoot, [StringComparison]::OrdinalIgnoreCase)) {
        $rel = $file.FullName.Substring($kilnfwRoot.Length).TrimStart('\', '/')
    } else {
        $rel = $file.FullName
    }

    # 1. #define <SOMETHING>PARTITION "label"
    #    If the define's name mentions NVS, the label must be an nvs-subtype
    #    row -- that is the pairing that actually goes wrong (opening a
    #    spiffs/littlefs partition with nvs_flash_init_partition()).
    foreach ($m in [regex]::Matches($text, '#define\s+([A-Z0-9_]*PARTITION[A-Z0-9_]*)\s+"([A-Za-z0-9_]+)"')) {
        $defName = $m.Groups[1].Value
        $label   = $m.Groups[2].Value
        $want    = if ($defName -match 'NVS') { 'nvs' } else { $null }
        $refs += [PSCustomObject]@{ Label = $label; WantSubType = $want; File = $rel; Why = "#define $defName" }
    }

    # 2. .partition_label = "label" -- classify by which VFS register call the
    #    same file makes. A file that sets a partition_label and registers
    #    neither is a genuine ambiguity, so it is reported rather than guessed.
    $spiffsHere   = $text -match 'esp_vfs_spiffs_register'
    $littlefsHere = $text -match 'esp_vfs_littlefs_register'
    foreach ($m in [regex]::Matches($text, '\.partition_label\s*=\s*"([A-Za-z0-9_]+)"')) {
        $label = $m.Groups[1].Value
        if     ($spiffsHere -and -not $littlefsHere)   { $want = 'spiffs' }
        elseif ($littlefsHere -and -not $spiffsHere)   { $want = 'littlefs' }
        else                                           { $want = $null }
        $refs += [PSCustomObject]@{ Label = $label; WantSubType = $want; File = $rel; Why = ".partition_label" }
    }

    # 3. esp_spiffs_info("label" / esp_littlefs_info("label"
    foreach ($m in [regex]::Matches($text, 'esp_spiffs_info\(\s*"([A-Za-z0-9_]+)"')) {
        $refs += [PSCustomObject]@{ Label = $m.Groups[1].Value; WantSubType = 'spiffs'; File = $rel; Why = "esp_spiffs_info()" }
    }
    foreach ($m in [regex]::Matches($text, 'esp_littlefs_info\(\s*"([A-Za-z0-9_]+)"')) {
        $refs += [PSCustomObject]@{ Label = $m.Groups[1].Value; WantSubType = 'littlefs'; File = $rel; Why = "esp_littlefs_info()" }
    }

    # 4. esp_partition_find_first(...,"label") -- existence only; this API is
    #    used with the explicit type/subtype arguments right there in the call,
    #    so the label is the only thing this script can usefully cross-check.
    foreach ($m in [regex]::Matches($text, 'esp_partition_find_first\s*\((?s).{0,200}?"([A-Za-z0-9_]+)"\s*\)')) {
        $refs += [PSCustomObject]@{ Label = $m.Groups[1].Value; WantSubType = $null; File = $rel; Why = "esp_partition_find_first()" }
    }

    # 5. nvs_report.c-style label arrays: a bare "label", array of partition
    #    names. Matched narrowly (the file must mention nvs_report) so this
    #    does not sweep up every string literal in the tree.
    if ($rel -match 'nvs_report\.[ch]$') {
        foreach ($m in [regex]::Matches($text, '^\s*"([A-Za-z0-9_]+)",\s*$', 'Multiline')) {
            $refs += [PSCustomObject]@{ Label = $m.Groups[1].Value; WantSubType = 'nvs'; File = $rel; Why = "nvs_report partition list" }
        }
    }
}

# ------------------------------------------------------------------ report --
Write-Host "Partition label agreement check"
Write-Host "  CSV:    $CsvPath"
Write-Host "  Source: $SourceRoot"
Write-Host "  $($partitions.Count) partitions in table, $($refs.Count) label reference(s) found in $($sourceFiles.Count) source file(s)."
Write-Host ""

$errors = @()

# --- Forward: firmware -> CSV ---
foreach ($r in ($refs | Sort-Object Label, File)) {
    $p = $partitions[$r.Label]
    if (-not $p) {
        $errors += "$($r.File): $($r.Why) names partition '$($r.Label)', which is NOT in $CsvPath. This fails at runtime on the board (ESP_ERR_NOT_FOUND), not at build time."
        continue
    }
    if ($r.WantSubType -and $p.SubType -ne $r.WantSubType) {
        $errors += "$($r.File): $($r.Why) opens '$($r.Label)' as '$($r.WantSubType)', but the table declares it subtype '$($p.SubType)'."
    }
}

$referenced = $refs.Label | Sort-Object -Unique
Write-Host "Labels referenced by firmware: $($referenced -join ', ')"

# --- Reverse: CSV -> firmware, for mountable data partitions only ---
$mountable = $partitions.Values | Where-Object { $_.Type -eq 'data' -and $_.SubType -in @('nvs', 'spiffs', 'littlefs') } | Sort-Object Name
$unusedReported = @()
foreach ($p in $mountable) {
    if ($referenced -contains $p.Name) { continue }
    if ($DeclaredButUnused.ContainsKey($p.Name)) {
        $unusedReported += $p.Name
        continue
    }
    $errors += "partition '$($p.Name)' (subtype $($p.SubType), $($p.Size) B at 0x$($p.Offset.ToString('X'))) is mountable but NO firmware source references it. Either wire it up, or add it to `$DeclaredButUnused in this script with a reason and the step that removes the entry."
}

# --- Allowlist hygiene: an entry for a partition that no longer exists, or
#     for one the firmware now DOES use, is stale and must be removed. ---
foreach ($name in $DeclaredButUnused.Keys) {
    if (-not $partitions.ContainsKey($name)) {
        $errors += "`$DeclaredButUnused lists '$name', which is not in $CsvPath at all -- remove the stale entry."
    } elseif ($referenced -contains $name) {
        $errors += "`$DeclaredButUnused lists '$name', but firmware now references it -- remove the entry (this is the reminder it was added to produce)."
    }
}

if ($unusedReported.Count -gt 0) {
    Write-Host ""
    Write-Host "Declared-but-unused (allowlisted, each with a documented reason):"
    foreach ($n in $unusedReported) { Write-Host "  $n -- $($DeclaredButUnused[$n])" }
}

if ($errors.Count -gt 0) {
    Write-Host ""
    Write-Host "PARTITION LABEL AGREEMENT CHECK FAILED:" -ForegroundColor Red
    foreach ($e in $errors) { Write-Host "  $e" -ForegroundColor Red }
    throw "$($errors.Count) partition-label disagreement(s) between $CsvPath and the firmware source."
}

Write-Host ""
Write-Host "Partition label agreement check passed: every firmware label exists in the table with a compatible subtype, and every mountable partition is either used or allowlisted with a reason."
exit 0
