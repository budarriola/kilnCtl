# check_persist_scratch_malloc_caps.ps1 -- keeps big plain malloc()/calloc()
# scratch out of the persist layer and the backup import path.
#
# WHY. CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192 puts EVERY plain malloc() of
# <= 8192 B in internal RAM. Bench finding 2026-10-04: a no-op
# POST /api/backup/import dropped heap_internal min_free from 15727 B to
# 2595 B (owner floor: >= 8192 B) because the kiln_configs commit path held
# several plain-malloc scratch structs (kiln_cfg_import_scratch_t ~4.85 KB,
# export scratch ~3.2 KB, zones_cfg_t ~2.3 KB, a 4 KB hash buffer, cfg_fs
# write scratch ~2.8 KB) at once on top of the 8 KB internal http_async_job
# stack. Plain CPU-side scratch there now goes through
# persist/persist_scratch.h's persist_scratch_alloc() (PSRAM first, internal
# fallback) -- the same shape backup_import.c already used.
#
# WHAT IT FLAGS. In the four files below, after stripping comments, any
# malloc( / calloc( call whose size argument is (a) a sizeof() of anything
# other than a plain scalar type, or (b) a numeric literal >= 1024, or (c) an
# ALL_CAPS macro/constant (size unknown here, assumed large). Fix by using
# persist_scratch_alloc(), or heap_caps_malloc(..., MALLOC_CAP_SPIRAM |
# MALLOC_CAP_8BIT) with a plain-malloc fallback. Calls that are themselves a
# fallback directly after a heap_caps_malloc() are allowed.
#
# Remaining plain sites must be listed in $allow below with a reason; an
# allowlist entry that no longer matches anything is itself an error, so the
# list cannot rot.
param(
    [string]$RepoRoot
)
$ErrorActionPreference = "Stop"
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }

$files = @(
    "firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c",
    "firmware/KilnFW/App/drivers/persist/kiln_package.c",
    "firmware/KilnFW/App/drivers/persist/cfg_fs.c",
    "firmware/KilnFW/App/drivers/http/backup_import.c"
)

# file-basename | exact trimmed code line | reason
$allow = @(
    @("kiln_cfg_store.c", "kiln_cfg_store_blob_v1_t *v1 = malloc(sizeof(*v1));",
      "once-per-board v1->v3 migration at boot (nvs_load_store), never on the import path; reads straight from NVS"),
    @("kiln_cfg_store.c", "kiln_cfg_store_blob_v2_t *v2 = malloc(sizeof(*v2));",
      "once-per-board v1/v2->v3 migration at boot (two sites), never on the import path"),
    @("backup_import.c", "scratch = malloc(sizeof(*scratch));",
      "internal fallback directly after a heap_caps_malloc(SPIRAM) attempt"),
    @("backup_import.c", "zone_candidates = malloc(sizeof(zone_candidate_t) * MAX31856_CHANNEL_COUNT);",
      "internal fallback directly after a heap_caps_malloc(SPIRAM) attempt"),
    @("backup_import.c", "timing_profile_candidates = malloc(sizeof(timing_profile_candidate_t) * MAX31856_CHANNEL_COUNT);",
      "internal fallback directly after a heap_caps_malloc(SPIRAM) attempt"),
    @("backup_import.c", "backup_import_job_ctx_t *ctx = malloc(sizeof(backup_import_job_ctx_t));",
      "24-byte job context handed to the async task, not scratch")
)

function Get-CodeLines {
    param([string]$Path)
    $text = [System.IO.File]::ReadAllText($Path)
    # Strip block comments but keep newlines so line numbers survive.
    $text = [regex]::Replace($text, '/\*.*?\*/', { param($m) ([regex]::Replace($m.Value, '[^\r\n]', ' ')) }, 'Singleline')
    $text = [regex]::Replace($text, '//[^\r\n]*', '')
    return ,($text -split "`r?`n")
}

$scalar = '^(char|uint8_t|int8_t|uint16_t|int16_t|uint32_t|int32_t|uint64_t|int64_t|int|unsigned|float|double|size_t|bool)\b'
$fail = @()
$used = @{}
foreach ($rel in $files) {
    $path = Join-Path $RepoRoot $rel
    if (-not (Test-Path $path)) { throw "check_persist_scratch_malloc_caps.ps1: $rel not found -- moved/renamed? update this script" }
    $base = Split-Path $rel -Leaf
    $lines = Get-CodeLines $path
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $ln = $lines[$i]
        foreach ($m in [regex]::Matches($ln, '(?<![\w])(malloc|calloc)\s*\(([^;]*)\)')) {
            $arg = $m.Groups[2].Value
            $big = $false
            foreach ($s in [regex]::Matches($arg, 'sizeof\s*\(\s*([^)]*?)\s*\)')) {
                if ($s.Groups[1].Value -notmatch $scalar) { $big = $true }
            }
            foreach ($n in [regex]::Matches($arg, '(?<![\w.])(0x[0-9a-fA-F]+|\d+)u?(?![\w.])')) {
                $v = $n.Groups[1].Value
                $num = if ($v -like '0x*') { [Convert]::ToInt64($v.Substring(2), 16) } else { [int64]$v }
                if ($num -ge 1024) { $big = $true }
            }
            if ($arg -match '(?<![\w])[A-Z][A-Z0-9_]{3,}(?![\w])') { $big = $true }
            if (-not $big) { continue }
            $trim = $ln.Trim()
            $ok = $false
            foreach ($a in $allow) {
                if ($a[0] -eq $base -and $trim -eq $a[1]) { $ok = $true; $used["$($a[0])|$($a[1])"] = $true }
            }
            if (-not $ok) {
                $fail += "${rel}:$($i + 1): plain $($m.Groups[1].Value)($($arg.Trim())) -- use persist_scratch_alloc() (PSRAM first) or add a reasoned `$allow entry"
            }
        }
    }
}
foreach ($a in $allow) {
    if (-not $used["$($a[0])|$($a[1])"]) {
        $fail += "stale allowlist entry (no matching site): $($a[0]) :: $($a[1])"
    }
}
if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
Write-Host "OK: no unallowlisted large plain malloc/calloc in persist scratch files ($($allow.Count) allowlisted sites)"
exit 0
