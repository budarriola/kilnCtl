# check_aux_relay_conflict_sites.ps1 -- mechanical guard for the aux-output /
# zone relay-ownership invariant (docs/SPARE_RELAY_ONOFF_PLAN.md, WP-1).
#
# Invariant: an aux (spare-relay on/off) output and a zone must never bind the
# same relay. The aux store (persist/aux_outputs_cfg.c) and the zones store
# are two pieces of state joined by that invariant, neither owning it, so any
# NEW code path that writes a zone's persistent relay_mask or commits a whole
# zones blob can silently break it ("reset one side of a pair", CLAUDE.md).
#
# This check lists every non-test .c under firmware/KilnFW/App whose
# comment-stripped text calls zones_config_set_relay_mask[_no_save](),
# zones_config_import_blob(), zones_config_restore_snapshot_no_save(), or
# assigns a `.relay_mask =` / `->relay_mask =` field, and FAILS if the file is
# not on the allowlist below. A new entry needs a reviewed reason that the path is
# covered by zones_config_json_validate()'s aux hook (or is not a persistent
# authority). The paths that commit a zones_cfg_t WITHOUT calling validate
# (POST /api/zones, backup import) are listed in $mustCall below: the check also
# FAILS unless each of those files still calls the explicit aux conflict helper
# (zones_config_json_aux_conflict_mask / _aux_enabled_mask), so deleting the
# explicit check can no longer pass silently.
#
# Exit: 0 PASS, 1 FAIL. Usage:
#   powershell -ExecutionPolicy Bypass -File tools\check_aux_relay_conflict_sites.ps1 [-AppDir <path>]
param([string]$AppDir)

$ErrorActionPreference = "Stop"
if (-not $AppDir) { $AppDir = Join-Path $PSScriptRoot "..\firmware\KilnFW\App" }
$AppDir = (Resolve-Path $AppDir).Path

$allow = @{
    'zones_config_accessors.c'            = 'defines the setters/import/restore; validated by zones_config_json_validate on commit'
    'zones_http_post.c'                   = 'two functions write relay_mask here. zones_http_zone_free_for_aux only CLEARS it (to 0) so the relay can move to an aux entry; clearing cannot create a conflict. zones_http_zone_restore_after_aux PUTS BACK a saved nonzero mask, which CAN conflict, so it runs zones_config_json_aux_conflict_mask on its candidate and refuses (function-scoped, see $mustCallInFunc). The ordinary commit runs the same explicit check (see $mustCall)'
    'zones_http_post_parse.c'             = 'per-zone field parse only; the commit in zones_http_post.c runs the explicit aux conflict check (see $mustCall)'
    'backup_import.c'                     = 'explicit post-batch aux conflict check with whole-batch rollback (see $mustCall); the _no_save setters do not validate'
    'kiln_cfg_store.c'                    = 'kiln package apply; whole-blob commit via zones_config_import_blob (validated)'
    'kiln_cfg_swap.c'                     = 'kiln package swap; whole-blob commit via zones_config_import_blob (validated)'
    'zones_config_convert.c'              = 'version-to-version struct copies, not a commit authority'
    'zones_config_migrate.c'              = 'NVS migration copy, loaded blob re-validated on load'
    'profile_executor_config_reload.c'    = 'runtime cache of an already-validated mask, not persistent'
    'profile_executor_run.c'              = 'runtime cache of an already-validated mask, not persistent'
}

# Files that commit zone relay masks without zones_config_json_validate(): each must call
# the explicit aux conflict helper. Path -> required call (regex).
$mustCall = @{
    'zones_http_post.c' = 'zones_config_json_aux_conflict_mask\s*\('
    'backup_import.c'   = 'zones_config_json_aux_enabled_mask\s*\('
}

# Function-scoped: the named function body (not merely the file) must contain the call. The restore
# path PUTS BACK a relay mask, so it is the one place in zones_http_post.c that can re-create a conflict.
$mustCallInFunc = @(
    @{ File = 'zones_http_post.c'; Func = 'zones_http_zone_restore_after_aux'; Rx = 'zones_config_json_aux_conflict_mask\s*\(' }
)

function Get-FunctionBody([string]$text, [string]$fname) {
    $m = [regex]::Match($text, '(?m)^[A-Za-z_][^;{}()]*' + [regex]::Escape($fname) + '\s*\([^;{}]*\)\s*\{')
    if (-not $m.Success) { return $null }
    $i = $m.Index + $m.Length
    $depth = 1
    while ($i -lt $text.Length -and $depth -gt 0) {
        $c = $text[$i]
        if ($c -eq '{') { $depth++ } elseif ($c -eq '}') { $depth-- }
        $i++
    }
    if ($depth -ne 0) { return $null }
    return $text.Substring($m.Index, $i - $m.Index)
}

function Strip-Comments([string]$t) {
    $t = [regex]::Replace($t, '/\*.*?\*/', { param($m) ([regex]::Replace($m.Value, '[^\r\n]', ' ')) }, 'Singleline')
    return [regex]::Replace($t, '//[^\r\n]*', '')
}

$pat = '\bzones_config_set_relay_mask(_no_save)?\s*\(|\bzones_config_import_blob\s*\(|\bzones_config_restore_snapshot_no_save\s*\(|(\.|->)relay_mask\s*=[^=]'
$files = @(Get-ChildItem -Path $AppDir -Recurse -File -Filter *.c |
    Where-Object { ($_.FullName -replace '[^A-Za-z0-9_.:]', '/') -notmatch '/(test|build)/' })
if ($files.Count -lt 50) { Write-Host "FAIL: only $($files.Count) .c files under $AppDir; check went blind"; exit 1 }

$hit = @{}
foreach ($f in $files) {
    $t = Strip-Comments ([IO.File]::ReadAllText($f.FullName))
    if ([regex]::IsMatch($t, $pat)) { $hit[$f.Name] = $f.FullName }
}
$bad = @($hit.Keys | Where-Object { -not $allow.ContainsKey($_) } | Sort-Object)
$stale = @($allow.Keys | Where-Object { -not $hit.ContainsKey($_) } | Sort-Object)
if ($hit.Count -lt 5) { Write-Host "FAIL: only $($hit.Count) writer file(s) matched; pattern went blind"; exit 1 }
foreach ($s in $stale) { Write-Host "info: allowlisted file no longer matches: $s" }
if ($bad.Count -gt 0) {
    foreach ($b in $bad) { Write-Host "FAIL: unlisted zone relay_mask writer/committer: $($hit[$b])" }
    Write-Host "Review against the aux conflict invariant, then add to the allowlist with a reason."
    exit 1
}
$missing = @()
foreach ($k in $mustCall.Keys) {
    $f = $files | Where-Object { $_.Name -eq $k } | Select-Object -First 1
    if (-not $f) { $missing += "$k (file not found)"; continue }
    $src = Strip-Comments ([IO.File]::ReadAllText($f.FullName))
    $rx = [string]$mustCall[$k]
    if (-not [regex]::IsMatch($src, $rx)) { $missing += "$k (no call matching $rx)" }
}
foreach ($e in $mustCallInFunc) {
    $f = $files | Where-Object { $_.Name -eq $e.File } | Select-Object -First 1
    if (-not $f) { $missing += "$($e.File) (file not found)"; continue }
    $body = Get-FunctionBody (Strip-Comments ([IO.File]::ReadAllText($f.FullName))) $e.Func
    if ($null -eq $body) { $missing += "$($e.File) (function $($e.Func) not found)"; continue }
    if (-not [regex]::IsMatch($body, $e.Rx)) { $missing += "$($e.File) $($e.Func)() (no call matching $($e.Rx))" }
}
if ($missing.Count -gt 0) {
    foreach ($m in $missing) { Write-Host "FAIL: explicit aux conflict check missing: $m" }
    exit 1
}
Write-Host "PASS: $($hit.Count) writer file(s), all allowlisted; $($mustCall.Count + $mustCallInFunc.Count) explicit aux checks present"
exit 0
