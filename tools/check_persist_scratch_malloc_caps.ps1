# checkcache: ok
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
# SCOPE. Every .c under firmware/KilnFW/App/drivers/persist/ (discovered, recursive) plus
# the $extraFiles list below; the rule is meant to cover the whole persist layer.
#
# HOW IT PARSES. Each file in scope ($files) has its C comments stripped from
# the whole text (string/char literals are left alone), then every
# malloc( / calloc( / realloc( call is found and its whole argument text is
# extracted with balanced parentheses, so a call whose arguments wrap across
# lines is seen exactly like a one-line call. String and char literal bodies
# are blanked first, so text inside a string is never mistaken for a call.
# heap_caps_malloc() is not matched.
#
# WHAT IT FLAGS. A call is flagged if its argument text has two or more
# numeric factors (integer literals plus scalar sizeofs, e.g. 512 * 4).
# Otherwise integer literals below 1024
# (suffixes u/l/ul/ull etc. allowed) and sizeof() of a plain scalar type (no
# '[' inside) are dropped, and the call is flagged if what remains contains a
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
#
# COVERAGE. kiln_cfg_store_cfg_fs.c and zones_config_cfg_fs.c (their
# *_FILE_BUF_MAX read/write scratch) now use persist_scratch_alloc() and are
# listed in $files. Also not matched by design:
# heap_caps_malloc(), so firing_stats_cfg_fs_save()'s deliberate
# MALLOC_CAP_INTERNAL buffer (it is a flash-write source kept internal by choice,
# not by necessity) and the other explicit-caps sites in the newer files never need an
# allowlist entry; this check only catches a regression back to plain malloc.
param(
    [string]$RepoRoot
)
$ErrorActionPreference = "Stop"
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }

# Discovery, not a hand-kept list (2026-10-08: an audit added a plain malloc in an unlisted persist
# file and this check missed it). The rule covers the whole persist layer, so EVERY .c under
# $persistDir is scanned and a new file is covered automatically. $extraFiles are the non-persist
# files on the backup/import and cfgfs status paths that the same rule covers. A deliberate plain
# malloc anywhere in scope goes in $allow below with a reason; do not narrow the discovery.
$persistDir = "firmware/KilnFW/App/drivers/persist"
$extraFiles = @(
    "firmware/KilnFW/App/drivers/http/backup_import.c",
    "firmware/KilnFW/App/drivers/http/diagnostics_http.c",
    "firmware/KilnFW/App/drivers/http/profiles_http.c",
    "firmware/KilnFW/App/drivers/http/zones_http_post.c",
    "firmware/KilnFW/App/drivers/http/zone_aux_convert_http.c",
    "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
    "firmware/KilnFW/App/drivers/http/wifi_provision_http.c",
    "firmware/KilnFW/App/drivers/http/kiln_cfg_http.c",
    "firmware/KilnFW/App/drivers/control/profile_executor_firing_stats.c",
    "firmware/KilnFW/App/drivers/control/adaptive_tune_model.c"
)
$persistRoot = Join-Path $RepoRoot $persistDir
if (-not (Test-Path $persistRoot)) { Write-Host "FAIL: $persistDir not found -- moved/renamed? update this script"; exit 1 }
$persistFound = @(Get-ChildItem -Path $persistRoot -Recurse -File -Filter *.c | Sort-Object FullName | ForEach-Object {
    $_.FullName.Substring($RepoRoot.Length).TrimStart([char]92, [char]47).Replace([string][char]92, "/") })
if ($persistFound.Count -lt 45) { Write-Host "FAIL: persist discovery found only $($persistFound.Count) file(s); glob went blind"; exit 1 }
$files = $persistFound + $extraFiles

# Adoption guard (vacuity audit 2026-10-07): a file that already calls persist_scratch_alloc() has opted
# into this rule, so a regression to plain malloc there must be seen. Three such files were missing from
# the list above and a mutation to plain malloc in them passed. Any App .c using the helper must be listed.
$adopters = @(Get-ChildItem -Path (Join-Path $RepoRoot "firmware/KilnFW/App") -Recurse -File -Filter *.c |
    Where-Object { ($_.FullName -replace "[^A-Za-z0-9_.:]", "/") -notmatch "/(test|build)/" -and $_.Name -ne "persist_scratch.c" } |
    Where-Object { [regex]::IsMatch([IO.File]::ReadAllText($_.FullName), "\bpersist_scratch_alloc\s*\(") })
if ($adopters.Count -lt 8) { Write-Host "FAIL: only $($adopters.Count) persist_scratch_alloc() adopter file(s) found under App; adoption guard went blind"; exit 1 }
$unlisted = @()
foreach ($ad in $adopters) {
    $relp = $ad.FullName.Substring($RepoRoot.Length).TrimStart([char]92, [char]47).Replace([string][char]92, "/")
    if ($files -notcontains $relp) { $unlisted += $relp }
}
if ($unlisted.Count -gt 0) {
    $unlisted | ForEach-Object { Write-Host "FAIL: $_ uses persist_scratch_alloc() but is not in the `$files list of check_persist_scratch_malloc_caps.ps1" }
    exit 1
}

# file | normalized call text | expected count | reason
$allow = @(
    ,@("firmware/KilnFW/App/drivers/persist/cfg_fs_mount.c", "malloc(CFG_FS_SCAN_CHUNK_BYTES)", 1,
      "boot-time partition scan chunk (4096 B) on main_boot_early, deliberately heap-not-stack per docs/audits/boot_hang_2026-09-08.md; runs before PSRAM scratch policy matters and degrades to ESP_ERR_NO_MEM")
    ,@("firmware/KilnFW/App/drivers/persist/cfg_fs_mount.c", "malloc(sizeof(*gate))", 1,
      "boot-time format-gate scratch, heap-not-stack for the main-task stack budget; failure refuses auto-format, never crashes")
    ,@("firmware/KilnFW/App/drivers/persist/log_store.c", "calloc(1, sizeof(*rd))", 1,
      "log_store_reader_t is a small (about 24 B) open-to-close handle that owns a FILE*; calloc gives the zeroing the reader relies on, and PSRAM buys nothing at that size")
    ,@("firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c", "malloc(sizeof(*v1))", 1,
      "v1 migration buffer: bounded legacy schema struct, once-per-board migration at boot, never on the import path")
    ,@("firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c", "malloc(sizeof(*v2))", 2,
      "v2 migration buffer (two sites): bounded legacy schema struct, once-per-board migration at boot, never on the import path")
    ,@("firmware/KilnFW/App/drivers/http/backup_import.c", "malloc(sizeof(*scratch))", 1,
      "internal fallback after a heap_caps_malloc(SPIRAM) attempt")
    ,@("firmware/KilnFW/App/drivers/http/backup_import.c", "malloc(sizeof(zone_candidate_t) * MAX31856_CHANNEL_COUNT)", 1,
      "internal fallback after a heap_caps_malloc(SPIRAM) attempt")
    ,@("firmware/KilnFW/App/drivers/http/backup_import.c", "malloc(sizeof(timing_profile_candidate_t) * MAX31856_CHANNEL_COUNT)", 1,
      "internal fallback after a heap_caps_malloc(SPIRAM) attempt")
    ,@("firmware/KilnFW/App/drivers/http/backup_import.c", "malloc(sizeof(backup_import_job_ctx_t))", 1,
      "24-byte job context handed to the async task, not scratch")
    ,@("firmware/KilnFW/App/drivers/http/diagnostics_http.c", "malloc((size_t)len_ul)", 1,
      "GET /api/coredump chunk buffer (clamped to COREDUMP_HTTP_CHUNK_MAX 4096 B): a deliberate crash-forensics read, not the /api/cfgfs status path; left plain"))

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
        $v = $m.Value
        if ($v.StartsWith('/')) { [regex]::Replace($v, '[^\r\n]', ' ') }
        else { $v.Substring(0, 1) + (' ' * ($v.Length - 2)) + $v.Substring($v.Length - 1) }
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
    # Two or more numeric factors (literals and scalar sizeofs together), e.g.
    # 512 * 4 or sizeof(uint32_t) * 1000, can add up to a large size.
    $factors = [regex]::Matches($ArgText, $scalarSizeof).Count
    $stripped = [regex]::Replace($ArgText, $scalarSizeof, ' ')
    $factors += [regex]::Matches($stripped, $intLiteral).Count
    if ($factors -ge 2) { return $true }
    $rest = $stripped
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
$found = [System.Collections.Hashtable]::new([StringComparer]::Ordinal)   # "rel|call" -> count
$where = [System.Collections.Hashtable]::new([StringComparer]::Ordinal)   # "rel|call" -> list of line numbers
foreach ($rel in $files) {
    $path = Join-Path $RepoRoot $rel
    if (-not (Test-Path $path)) { throw "check_persist_scratch_malloc_caps.ps1: $rel not found -- moved/renamed? update this script" }
    $text = Get-CodeText $path
    foreach ($m in [regex]::Matches($text, '(?<![\w])(malloc|calloc|realloc)\s*\(')) {
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

$allowKeys = [System.Collections.Hashtable]::new([StringComparer]::Ordinal)
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
