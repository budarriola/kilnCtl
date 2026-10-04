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
# HOW IT PARSES. Each file listed in $files has its C comments stripped from
# the whole text (string/char literals are left alone), then every
# malloc( / calloc( call is found and its argument text is extracted with
# balanced parentheses, so a call whose arguments wrap across lines is seen
# exactly like a one-line call. heap_caps_malloc() is not matched.
#
# WHAT IT FLAGS. From a call's argument text, integer literals below 1024
# (suffixes u/l/ul/ull etc. allowed) and sizeof() of a plain scalar type (no
# '[' inside) are dropped. The call is flagged if what remains contains a
# non-scalar sizeof, an integer literal of 1024 or more, or ANY identifier
# (a runtime-sized or macro-sized allocation can be large). Fix a flagged
# site by using persist_scratch_alloc(), or heap_caps_malloc(...,
# MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) with a plain-malloc fallback.
#
# ALLOWLIST. A flagged call that is deliberately plain is listed in $allow,
# keyed by repo-relative file path plus the whitespace-normalized call text
# (e.g. "malloc(sizeof(*v2))"), with the exact number of occurrences expected
# and a one-line reason. Nothing is detected automatically: a fallback after a
# heap_caps_malloc() attempt must be allowlisted like any other site. The
# count must match exactly, so a duplicated call or a removed one fails the
# check, and an entry that matches nothing fails as stale; the list cannot rot.
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

# file | normalized call text | expected count | reason
$allow = @(
    @("firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c", "malloc(sizeof(*v1))", 1,
      "v1 migration buffer: bounded legacy schema struct, once-per-board migration at boot, never on the import path"),
    @("firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c", "malloc(sizeof(*v2))", 2,
      "v2 migration buffer (two sites): bounded legacy schema struct, once-per-board migration at boot, never on the import path"),
    @("firmware/KilnFW/App/drivers/http/backup_import.c", "malloc(sizeof(*scratch))", 1,
      "internal fallback after a heap_caps_malloc(SPIRAM) attempt"),
    @("firmware/KilnFW/App/drivers/http/backup_import.c", "malloc(sizeof(zone_candidate_t) * MAX31856_CHANNEL_COUNT)", 1,
      "internal fallback after a heap_caps_malloc(SPIRAM) attempt"),
    @("firmware/KilnFW/App/drivers/http/backup_import.c", "malloc(sizeof(timing_profile_candidate_t) * MAX31856_CHANNEL_COUNT)", 1,
      "internal fallback after a heap_caps_malloc(SPIRAM) attempt"),
    @("firmware/KilnFW/App/drivers/http/backup_import.c", "malloc(sizeof(backup_import_job_ctx_t))", 1,
      "24-byte job context handed to the async task, not scratch")
)

$scalarSizeof = 'sizeof\s*\(\s*(?:const\s+|unsigned\s+|signed\s+)*(?:char|uint8_t|int8_t|uint16_t|int16_t|uint32_t|int32_t|uint64_t|int64_t|int|short|long|float|double|size_t|bool)\b[^\[\)]*\)'
$intLiteral = '(?<![\w.])(0[xX][0-9a-fA-F]+|\d+)[uU]?[lL]{0,2}(?![\w.])'

# Replace comments with spaces (newlines preserved, so line numbers survive);
# string and char literals are matched first so "//" inside them survives.
function Get-CodeText {
    param([string]$Path)
    $text = [System.IO.File]::ReadAllText($Path)
    $pat = '"(?:\\.|[^"\\\r\n])*"|''(?:\\.|[^''\\\r\n])*''|/\*.*?\*/|//[^\r\n]*'
    return [regex]::Replace($text, $pat, {
        param($m)
        if ($m.Value.StartsWith('/')) { [regex]::Replace($m.Value, '[^\r\n]', ' ') } else { $m.Value }
    }, 'Singleline')
}

# Returns the argument text between the '(' at $open and its matching ')',
# or $null if unbalanced.
function Get-BalancedArgs {
    param([string]$Text, [int]$Open)
    $depth = 0
    for ($k = $Open; $k -lt $Text.Length; $k++) {
        $c = $Text[$k]
        if ($c -eq '"' -or $c -eq "'") {
            $q = $c
            $k++
            while ($k -lt $Text.Length -and $Text[$k] -ne $q) {
                if ($Text[$k] -eq '\') { $k++ }
                $k++
            }
            continue
        }
        if ($c -eq '(') { $depth++ }
        elseif ($c -eq ')') {
            $depth--
            if ($depth -eq 0) { return $Text.Substring($Open + 1, $k - $Open - 1) }
        }
    }
    return $null
}

function Test-Flagged {
    param([string]$ArgText)
    $rest = [regex]::Replace($ArgText, $scalarSizeof, ' ')
    foreach ($n in [regex]::Matches($rest, $intLiteral)) {
        $v = $n.Groups[1].Value
        $num = if ($v -match '^0[xX]') { [Convert]::ToInt64($v.Substring(2), 16) } else { [int64]$v }
        if ($num -ge 1024) { return $true }
    }
    $rest = [regex]::Replace($rest, $intLiteral, ' ')
    # Anything identifier-shaped left (non-scalar sizeof, macro, variable) is flagged.
    return ($rest -match '[A-Za-z_]\w*')
}

$fail = @()
$found = @{}      # "rel|call" -> count
$where = @{}      # "rel|call" -> list of line numbers
foreach ($rel in $files) {
    $path = Join-Path $RepoRoot $rel
    if (-not (Test-Path $path)) { throw "check_persist_scratch_malloc_caps.ps1: $rel not found -- moved/renamed? update this script" }
    $text = Get-CodeText $path
    foreach ($m in [regex]::Matches($text, '(?<![\w])(malloc|calloc)\s*\(')) {
        $open = $m.Index + $m.Length - 1
        $argText = Get-BalancedArgs $text $open
        $line = ($text.Substring(0, $m.Index) -split "`n").Count
        if ($null -eq $argText) {
            $fail += "${rel}:${line}: unbalanced parentheses in $($m.Groups[1].Value)( call -- cannot analyse"
            continue
        }
        if (-not (Test-Flagged $argText)) { continue }
        $norm = ($m.Groups[1].Value + "(" + $argText + ")") -replace '\s+', ' '
        $norm = $norm -replace '\(\s+', '(' -replace '\s+\)', ')'
        $key = "$rel|$norm"
        $found[$key] = 1 + [int]$found[$key]
        if (-not $where.ContainsKey($key)) { $where[$key] = @() }
        $where[$key] += $line
    }
}

$allowKeys = @{}
foreach ($a in $allow) { $allowKeys["$($a[0])|$($a[1])"] = $a }

foreach ($key in ($found.Keys | Sort-Object)) {
    if (-not $allowKeys.ContainsKey($key)) {
        $rel, $call = $key -split '\|', 2
        $fail += "${rel}:$($where[$key] -join ','): plain $call -- use persist_scratch_alloc() (PSRAM first) or add a reasoned `$allow entry"
    }
}
foreach ($a in $allow) {
    $key = "$($a[0])|$($a[1])"
    $n = [int]$found[$key]
    if ($n -eq 0) {
        $fail += "stale allowlist entry (no matching site): $($a[0]) :: $($a[1])"
    } elseif ($n -ne $a[2]) {
        $fail += "allowlist count mismatch: $($a[0]) :: $($a[1]) expected $($a[2]) found $n (lines $($where[$key] -join ','))"
    }
}
if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
$total = 0
foreach ($a in $allow) { $total += $a[2] }
Write-Host "OK: no unallowlisted large plain malloc/calloc in persist scratch files ($($allow.Count) allowlist entries, $total allowlisted sites)"
exit 0
