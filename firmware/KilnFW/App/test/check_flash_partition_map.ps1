# check_flash_partition_map.ps1 -- FLASH_BUDGET.md section 4.3.
#
# Section 2 of that doc built its partition table (and the "65,536 B
# contiguous tail" / "12,288 B boxed-in gap" / "57,344 B forced-alignment
# pad" figures) by hand from the CSV's own comments. The doc's own history
# records that going stale silently once already (an earlier draft's first
# row omitted 12 K and undercounted the chip by 16 K) -- the table has been
# restructured three times (OTA, the `factory` move, `logs`) and each time
# was another chance for a hand tally to drift from the real CSV.
#
# This script computes the map FROM partitions.csv itself -- offsets, sizes,
# gaps, and the unallocated remainder -- so a future restructure (Lane C's
# job, not this script's) has something to check its arithmetic against
# instead of asserting it. It also fails loudly on a malformed table
# (overlapping partitions, a partition that runs past the chip, a
# non-monotonic/unparseable CSV) rather than silently printing nonsense --
# that is the actual point of running this as a check_*.ps1 guard picked up
# by tools/run_all_checks.ps1, not just a one-off report.
#
# THIS SCRIPT PERFORMS NO PARTITION-TABLE-CHANGE STEPS. It only computes and
# reports. FLASH_BUDGET.md section 7 lists the hazard checklist for an
# actual revision (erase otadata, read out coredump first, reflash the
# bootloader, archive the NVS partitions, append-only discipline) -- this
# script prints the *inputs* that checklist needs (which partitions are
# `nvs`-subtype and must be archived, whether `otadata`/`coredump` exist and
# their sizes, which partitions are `app`-type and must stay equal-sized) so
# that checklist can be worked from real numbers, but performing any of its
# steps is explicitly out of scope here.
#
# Usage:
#   powershell -File firmware\KilnFW\App\test\check_flash_partition_map.ps1
#   powershell -File firmware\KilnFW\App\test\check_flash_partition_map.ps1 -CsvPath <path> -FlashSizeBytes <n>
#
# -CsvPath and -FlashSizeBytes exist so this script's own negative tests (and
# any future caller checking a *candidate* revised table before it is
# committed) can point it at something other than the live
# firmware/KilnFW/partitions.csv. Default FlashSizeBytes is read from
# firmware/KilnFW/sdkconfig's CONFIG_ESPTOOLPY_FLASHSIZE_* if that
# (gitignored, build-generated) file exists, else falls back to 16 MB with an
# explicit warning -- see FLASH_BUDGET.md section 2.1's own note that
# sdkconfig drift has bitten this repo before (feedback_gitignored_config_
# hides_mismatch).
#
# Exit code is non-zero (with a specific diagnostic) if the CSV cannot be
# parsed, if any two partitions overlap, or if any partition (or the table
# itself) runs past the flash size.

param(
    [string]$CsvPath,
    [Nullable[long]]$FlashSizeBytes
)

$ErrorActionPreference = "Stop"

# $PSScriptRoot = ...\firmware\KilnFW\App\test
$kilnfwRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

if (-not $CsvPath) {
    $CsvPath = Join-Path $kilnfwRoot "partitions.csv"
}
if (-not (Test-Path $CsvPath)) {
    throw "check_flash_partition_map.ps1: partitions CSV not found at $CsvPath"
}

# --- Flash size: prefer sdkconfig's own answer over a hardcoded assumption ---
if (-not $FlashSizeBytes) {
    $sdkconfigPath = Join-Path $kilnfwRoot "sdkconfig"
    $sizeMap = @{
        'CONFIG_ESPTOOLPY_FLASHSIZE_1MB'  = 1MB
        'CONFIG_ESPTOOLPY_FLASHSIZE_2MB'  = 2MB
        'CONFIG_ESPTOOLPY_FLASHSIZE_4MB'  = 4MB
        'CONFIG_ESPTOOLPY_FLASHSIZE_8MB'  = 8MB
        'CONFIG_ESPTOOLPY_FLASHSIZE_16MB' = 16MB
        'CONFIG_ESPTOOLPY_FLASHSIZE_32MB' = 32MB
    }
    $resolved = $null
    if (Test-Path $sdkconfigPath) {
        $sdkLines = Get-Content -Path $sdkconfigPath
        foreach ($key in $sizeMap.Keys) {
            if ($sdkLines -match "^$key=y") {
                $resolved = $sizeMap[$key]
                break
            }
        }
    }
    if ($resolved) {
        $FlashSizeBytes = [long]$resolved
    } else {
        $FlashSizeBytes = 16MB
        Write-Host "WARNING: could not read flash size from $sdkconfigPath (file absent or setting not found)." -ForegroundColor Yellow
        Write-Host "  Falling back to 16 MB (FLASH_BUDGET.md section 2.1's confirmed figure for this board)." -ForegroundColor Yellow
        Write-Host "  sdkconfig is gitignored and build-generated -- a fresh clone or CI checkout may not have one yet." -ForegroundColor Yellow
    }
}

function ConvertFrom-PartitionSize {
    param([string]$Text, [string]$FieldName, [string]$RowContext)
    $t = $Text.Trim()
    if ($t -match '^0[xX][0-9a-fA-F]+$') {
        return [Convert]::ToInt64($t, 16)
    }
    if ($t -match '^\d+[kKmM]$') {
        $num = [long]($t.Substring(0, $t.Length - 1))
        $suffix = $t.Substring($t.Length - 1).ToLower()
        if ($suffix -eq 'k') { return $num * 1KB }
        else { return $num * 1MB }
    }
    if ($t -match '^\d+$') {
        return [long]$t
    }
    throw "check_flash_partition_map.ps1: cannot parse $FieldName '$Text' in row: $RowContext"
}

# --- Parse the CSV: strip comments/blanks, split remaining data rows ---
$rawLines = Get-Content -Path $CsvPath
$rows = @()
foreach ($line in $rawLines) {
    $trimmed = $line.Trim()
    if ($trimmed.Length -eq 0) { continue }
    if ($trimmed.StartsWith('#')) { continue }
    $fields = $trimmed.Split(',') | ForEach-Object { $_.Trim() }
    if ($fields.Count -lt 5) {
        throw "check_flash_partition_map.ps1: malformed row (need at least Name,Type,SubType,Offset,Size): '$line'"
    }
    $name = $fields[0]
    $type = $fields[1]
    $subtype = $fields[2]
    $offset = ConvertFrom-PartitionSize -Text $fields[3] -FieldName "Offset" -RowContext $line
    $size = ConvertFrom-PartitionSize -Text $fields[4] -FieldName "Size" -RowContext $line
    if ($size -le 0) {
        throw "check_flash_partition_map.ps1: partition '$name' has non-positive size ($size) -- row: '$line'"
    }
    $rows += [PSCustomObject]@{
        Name    = $name
        Type    = $type
        SubType = $subtype
        Offset  = $offset
        Size    = $size
        End     = $offset + $size
    }
}

if ($rows.Count -eq 0) {
    throw "check_flash_partition_map.ps1: no partition rows found in $CsvPath -- empty table or every line treated as a comment"
}

$sorted = $rows | Sort-Object Offset

# --- Validate: no overlaps, nothing runs past the chip ---
$errors = @()
for ($i = 0; $i -lt $sorted.Count; $i++) {
    $p = $sorted[$i]
    if ($p.End -gt $FlashSizeBytes) {
        $errors += "'$($p.Name)' ends at 0x$($p.End.ToString('X')) ($($p.End) B), past the $($FlashSizeBytes / 1MB) MB flash (0x$($FlashSizeBytes.ToString('X')))"
    }
    if ($i -gt 0) {
        $prev = $sorted[$i - 1]
        if ($p.Offset -lt $prev.End) {
            $errors += "'$($prev.Name)' (0x$($prev.Offset.ToString('X'))..0x$($prev.End.ToString('X'))) overlaps '$($p.Name)' (0x$($p.Offset.ToString('X'))..0x$($p.End.ToString('X')))"
        }
    }
}

# --- Compute gaps (unallocated ranges), including before the first entry
#     and after the last, regardless of whether the table is valid, so a
#     failure report still shows where the problem is.
#
#     Two kinds of gap are NOT freely reclaimable, and are labelled as such
#     rather than folded into one undifferentiated "unallocated" total
#     (FLASH_BUDGET.md section 1 makes exactly this distinction --
#     conflating them is the mistake this script exists to prevent):
#       - the range before the first CSV row: the bootloader and the
#         partition table itself live there but are NOT rows in
#         partitions.csv (they are placed by CONFIG_BOOTLOADER_OFFSET_IN_
#         FLASH / CONFIG_PARTITION_TABLE_OFFSET, outside this file), so this
#         script cannot see them as "used" -- only infer that this leading
#         range is reserved, not free.
#       - a gap immediately before an `app`-type partition: gen_esp32part.py
#         forces `app` partitions onto 64 KB boundaries, so a small gap
#         right before one is very likely that forced alignment pad, not
#         reclaimable space -- same reasoning as the 57,344 B pad before
#         `ota_0` in section 1. This is a heuristic inferred from adjacency
#         in the CSV, not a hardcoded byte count. ---
$gaps = @()
$cursor = 0
foreach ($p in $sorted) {
    if ($p.Offset -gt $cursor) {
        $gapSize = $p.Offset - $cursor
        if ($cursor -eq 0) {
            $reason = "reserved: bootloader + partition table (not represented in partitions.csv)"
            $reclaimable = $false
        } elseif ($p.Type -eq 'app') {
            $reason = "likely forced 64K-alignment pad ahead of app partition '$($p.Name)' -- not reclaimable"
            $reclaimable = $false
        } else {
            $reason = "boxed-in / free gap"
            $reclaimable = $true
        }
        $gaps += [PSCustomObject]@{ Start = $cursor; End = $p.Offset; Size = $gapSize; Reason = $reason; Reclaimable = $reclaimable }
    }
    if ($p.End -gt $cursor) { $cursor = $p.End }
}
if ($cursor -lt $FlashSizeBytes) {
    $gaps += [PSCustomObject]@{ Start = $cursor; End = $FlashSizeBytes; Size = $FlashSizeBytes - $cursor; Reason = "contiguous free tail"; Reclaimable = $true }
}

# --- Report: the computed map ---
Write-Host "Partition map computed from: $CsvPath"
Write-Host "Flash size: $FlashSizeBytes B (0x$($FlashSizeBytes.ToString('X')), $($FlashSizeBytes / 1MB) MB)"
Write-Host ""
Write-Host ("{0,-14} {1,-6} {2,-10} {3,-12} {4,-12} {5,-12}" -f "name", "type", "subtype", "offset", "size", "end")
foreach ($p in $sorted) {
    Write-Host ("{0,-14} {1,-6} {2,-10} 0x{3,-10} {4,-12} 0x{5,-10}" -f `
        $p.Name, $p.Type, $p.SubType, $p.Offset.ToString('X'), "$($p.Size) B", $p.End.ToString('X'))
}

Write-Host ""
Write-Host "Unallocated ranges:"
$totalUnallocated = 0
$totalReclaimable = 0
foreach ($g in $gaps) {
    $totalUnallocated += $g.Size
    if ($g.Reclaimable) { $totalReclaimable += $g.Size }
    $tag = if ($g.Reclaimable) { "reclaimable" } else { "NOT reclaimable" }
    Write-Host ("  0x{0}..0x{1}  {2,10} B  [{3}]  {4}" -f $g.Start.ToString('X'), $g.End.ToString('X'), $g.Size, $tag, $g.Reason)
}
Write-Host "Total unallocated: $totalUnallocated B"
Write-Host "Of which freely reclaimable (excludes the pre-table reserved region and forced app-alignment pads): $totalReclaimable B"

# --- Report: inputs the section 7 hazard checklist needs (does not perform
#     any of that checklist's steps) ---
Write-Host ""
Write-Host "Section 7 checklist inputs (informational only -- this script performs none of these steps):"
$nvsRows = $sorted | Where-Object { $_.SubType -eq 'nvs' }
if ($nvsRows) {
    Write-Host "  NVS partitions to archive before any table revision (checklist item 4):"
    foreach ($p in $nvsRows) {
        Write-Host "    $($p.Name)  0x$($p.Offset.ToString('X'))  $($p.Size) B"
    }
} else {
    Write-Host "  No 'nvs'-subtype partitions found (unexpected for this table)."
}
$otadataRow = $sorted | Where-Object { $_.Type -eq 'data' -and $_.SubType -eq 'ota' }
if ($otadataRow) {
    Write-Host "  otadata present ($($otadataRow.Name), $($otadataRow.Size) B) -- must be erased if any app offset/size changes (checklist item 1)."
} else {
    Write-Host "  No otadata partition found."
}
$coredumpRow = $sorted | Where-Object { $_.SubType -eq 'coredump' }
if ($coredumpRow) {
    Write-Host "  coredump present ($($coredumpRow.Name), $($coredumpRow.Size) B) -- read out with espcoredump.py before any move (checklist item 2)."
}
$appRows = $sorted | Where-Object { $_.Type -eq 'app' }
if ($appRows) {
    $distinctSizes = $appRows.Size | Sort-Object -Unique
    Write-Host "  app-type partitions (must stay equal-sized, per FLASH_BUDGET.md section 2): $($appRows.Name -join ', ')"
    if ($distinctSizes.Count -gt 1) {
        Write-Host "    WARNING: app partitions are NOT equal-sized today ($($distinctSizes -join ', ') B) -- check_sizes.py takes min() across all of them." -ForegroundColor Yellow
    }
}
Write-Host "  Bootloader must be reflashed against any new table (checklist item 3) -- always true, not conditional on the CSV."
Write-Host "  Append-only discipline (checklist item 5) cannot be verified from a single CSV snapshot -- diff against the previous committed partitions.csv by hand."

if ($errors.Count -gt 0) {
    Write-Host ""
    Write-Host "PARTITION MAP CHECK FAILED:" -ForegroundColor Red
    foreach ($e in $errors) {
        Write-Host "  $e" -ForegroundColor Red
    }
    throw "$($errors.Count) partition-table problem(s) found in $CsvPath -- see FLASH_BUDGET.md section 4.3"
}

Write-Host ""
Write-Host "Partition map check passed: $($sorted.Count) partitions, no overlaps, nothing exceeds the $($FlashSizeBytes / 1MB) MB flash."

# --- Expected-map assertions (FLASH_BUDGET.md section 5, 2026-09-02 pass) ---
#
# The generic checks above (no overlaps, nothing past the chip) pass for a
# huge range of tables, including a wrong one -- they cannot catch "someone
# put pico_img back at its old offset" or "logs grew back to 3072K by
# accident". This section pins down the specific values this restructure
# was supposed to produce, so a regression in EITHER direction (reverting
# the reclamation, or drifting further without updating this check) fails
# loudly. Only runs against the real partitions.csv (default $CsvPath) --
# a caller pointing this script at some other candidate table via -CsvPath
# is explicitly testing something else and should not trip these.
if (-not $PSBoundParameters.ContainsKey('CsvPath')) {
    $expected = @(
        # name           offset      size
        @('pico_img',    0x10000,    0xE0000),
        @('wifi_nvs',    0x187000,   0x6000),
        @('kiln_nvs',    0x18D000,   0x10000),
        @('profiles_nvs',0x19D000,   0x60000),
        @('otadata',     0x200000,   0x2000),
        # single application slot + recovery image, 2026-09-16
        # (docs/OTA_SINGLE_SLOT_PLAN.md section 1 / section 8 step 3):
        # 'ota_0'/'ota_1'/'factory' replaced by 'app'/'recovery'. See
        # partitions.csv's own "single application slot + recovery image"
        # header block for the fit arithmetic.
        @('app',         0x210000,   0x800000),
        @('recovery',    0xA10000,   0x1E0000),
        @('coredump',    0xBF0000,   0x100000),
        @('logs',        0xCF0000,   0xC0000),
        # cfg: added 2026-09-07, append-only into the free tail, sized from the
        # measured user-data inventory -- see partitions.csv's own `cfg` comment
        # block and docs/FILESYSTEM_PLAN.md. Grown 2026-09-19
        # (docs/PROFILE_SLOTS_100_PLAN.md section 7 task 5, partitions.csv's
        # "cfg grown to take the entire remaining tail" comment block) from
        # 0x80000 to 0x250000 -- offset unchanged, pure grow-in-place, new end
        # lands exactly at the 16 MiB chip boundary. Pinned here so a later
        # pass cannot quietly resize or relocate it: the data it holds (tuned
        # PID gains, the coupling matrix, and now up to 100 profile slots)
        # costs hours of bench firings to regenerate.
        @('cfg',         0xDB0000,   0x250000)
    )
    $mapErrors = @()
    foreach ($e in $expected) {
        $name, $off, $sz = $e
        $row = $sorted | Where-Object { $_.Name -eq $name }
        if (-not $row) {
            $mapErrors += "expected partition '$name' not found in $CsvPath"
            continue
        }
        if ($row.Offset -ne $off) {
            $mapErrors += "'$name' offset is 0x$($row.Offset.ToString('X')), expected 0x$($off.ToString('X'))"
        }
        if ($row.Size -ne $sz) {
            $mapErrors += "'$name' size is $($row.Size) B, expected $sz B"
        }
    }
    if ($sorted.Name -contains 'legacy_app') {
        $mapErrors += "'legacy_app' still present -- FLASH_BUDGET.md section 5.1 reclaimed this hole into 'pico_img', this placeholder should be gone"
    }
    if ($mapErrors.Count -gt 0) {
        Write-Host ""
        Write-Host "EXPECTED-MAP CHECK FAILED (FLASH_BUDGET.md section 5):" -ForegroundColor Red
        foreach ($e in $mapErrors) {
            Write-Host "  $e" -ForegroundColor Red
        }
        throw "$($mapErrors.Count) expected-map mismatch(es) in $CsvPath -- see FLASH_BUDGET.md section 5"
    }
    Write-Host "Expected-map check passed: pico_img relocated into the reclaimed legacy_app hole, logs shrunk to 768 KiB, legacy_app gone, cfg appended at 0xDB0000 (2.31 MiB)."
}

exit 0
