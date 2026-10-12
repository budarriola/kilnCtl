# checkcache: ok
# check_reset_fence_hooks.ps1 -- the factory-reset writer fence is only as good as its install lines.
#
# The fence is a set of PREDICATE HOOKS that main.c installs at boot (each is NULL = "refuse nothing" by
# default, which is why a dropped install line is silent -- every writer then simply stops being fenced and
# no test of the writer itself fails):
#   pref_cfg_fs_set_reset_refuse_hook(relay_authority_reset_refuses_writer)    pref_cfg_fs.h "WRITER FENCE"
#   cfg_fs_mount_set_write_refuse_hook(relay_authority_reset_refuses_writer)   cfg_fs_mount.c final worker check
#   cfg_fs_set_write_refuse_hook(relay_authority_reset_refuses_writer)         cfg_fs_write_atomic()
#   hal_kv_set_write_refuse_hook(kiln_nvs_reset_refuses_write)                 every kiln_nvs hal_kv write
# and kiln_nvs_reset_refuses_write() must itself consult relay_authority_reset_refuses_kiln_nvs_writer()
# (ea86f4d43: the same reset-in-flight predicate, true only when the reset erases kiln_nvs; so the
# reset job's own task stays exempt).
#
# This check fails when main.c stops making any of those four calls (comments are stripped first, so a
# commented-out install does not count), or when the kiln_nvs predicate's return is not exactly the three-term expression ending in the
# relay_authority predicate, or that predicate's body in relay_authority.c is edited (semantics: test_link_watchdog.c). Self-tested by tools\negtest.ps1 (see the mutations in this check's own history).
#
# Usage: powershell -File tools\check_reset_fence_hooks.ps1 [-MainFile <path to main.c>]
param(
    [string]$MainFile,
    [string]$RelayAuthorityFile
)

$ErrorActionPreference = "Stop"

if (-not $MainFile) {
    $MainFile = Join-Path $PSScriptRoot "..\firmware\KilnFW\App\main.c"
}
if (-not (Test-Path -LiteralPath $MainFile)) {
    Write-Host "FAIL: main.c not found at $MainFile"
    exit 1
}
if (-not $RelayAuthorityFile) {
    $RelayAuthorityFile = Join-Path $PSScriptRoot "..\firmware\KilnFW\App\drivers\owners\relay_authority.c"
}
if (-not (Test-Path -LiteralPath $RelayAuthorityFile)) {
    Write-Host "FAIL: relay_authority.c not found at $RelayAuthorityFile"
    exit 1
}
$text = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $MainFile).Path)
$raText = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $RelayAuthorityFile).Path)

# Strip block and line comments (string literals in main.c never contain comment openers around these calls).
$stripped = [regex]::Replace($text, '/\*.*?\*/', '', [System.Text.RegularExpressions.RegexOptions]::Singleline)
$stripped = [regex]::Replace($stripped, '//[^\r\n]*', '')

$required = @(
    @{ Name = 'pref_cfg_fs_set_reset_refuse_hook'; Arg = 'relay_authority_reset_refuses_writer' },
    @{ Name = 'cfg_fs_mount_set_write_refuse_hook'; Arg = 'relay_authority_reset_refuses_writer' },
    @{ Name = 'cfg_fs_set_write_refuse_hook'; Arg = 'relay_authority_reset_refuses_writer' },
    @{ Name = 'hal_kv_set_write_refuse_hook'; Arg = 'kiln_nvs_reset_refuses_write' }
)

$fail = 0
foreach ($r in $required) {
    $pat = '(?<![A-Za-z0-9_])' + [regex]::Escape($r.Name) + '\s*\(\s*' + [regex]::Escape($r.Arg) + '\s*\)'
    if ($stripped -notmatch $pat) {
        Write-Host "FAIL: main.c no longer installs $($r.Name)($($r.Arg)) -- the factory-reset writer fence it carries is off"
        $fail++
    }
}

# Macros must not shadow the predicate, its callee, or the helpers it uses (a function-like or object-like
# `#define` makes the real body irrelevant); main.c is not compiled into any host test, so this check is its only guard.
if ($stripped -match '(?m)^\s*#\s*(define|undef)\s+(relay_authority_reset_refuses_kiln_nvs_writer|kiln_nvs_reset_refuses_write|strcmp|NULL)\b') {
    Write-Host 'FAIL: main.c #defines/#undefs the kiln_nvs reset predicate, its relay_authority callee, strcmp or NULL -- the predicate body is shadowed'
    $fail++
}

# The kiln_nvs predicate's whole body must be a single return of partition != NULL && strcmp(partition, "kiln_nvs") == 0
# && relay_authority_reset_refuses_kiln_nvs_writer(). The parameter name is free (a rename is harmless), but every use
# must be the same identifier (backreference). Anything appended or inserted fails. (Strings kept; comments stripped.)
$predPat = 'static\s+bool\s+kiln_nvs_reset_refuses_write\s*\(\s*const\s+char\s*\*\s*(?<p>[A-Za-z_]\w*)\s*\)\s*\{\s*return\s+\k<p>\s*!=\s*NULL\s*&&\s*strcmp\s*\(\s*\k<p>\s*,\s*"kiln_nvs"\s*\)\s*==\s*0\s*&&\s*relay_authority_reset_refuses_kiln_nvs_writer\s*\(\s*\)\s*;\s*\}'
if ($stripped -notmatch $predPat) {
    Write-Host 'FAIL: kiln_nvs_reset_refuses_write() is missing or its body is no longer exactly one return of: <arg> != NULL && strcmp(<arg>, "kiln_nvs") == 0 && relay_authority_reset_refuses_kiln_nvs_writer()'
    $fail++
}

# The shared predicate in relay_authority.c: semantics are pinned by test_link_watchdog.c; this only keeps the
# three terms, forbids any return before the assignment or any statement between it and `return refuse;`, and
# forbids macros shadowing the state it reads.
$raStripped = [regex]::Replace($raText, '/\*.*?\*/', '', [System.Text.RegularExpressions.RegexOptions]::Singleline)
$raStripped = [regex]::Replace($raStripped, '//[^\r\n]*', '')
if ($raStripped -match '(?m)^\s*#\s*(define|undef)\s+(s_reset_in_flight_depth|s_reset_erases_kiln_nvs|s_reset_job_task|relay_authority_reset_refuses_kiln_nvs_writer)\b') {
    Write-Host 'FAIL: relay_authority.c #defines/#undefs reset state or the kiln_nvs reset predicate -- its body is shadowed'
    $fail++
}
$bodyPat = 'bool\s+relay_authority_reset_refuses_kiln_nvs_writer\s*\(\s*void\s*\)\s*\{(?<pre>[^{}]*?)bool\s+refuse\s*=\s*\(\s*s_reset_in_flight_depth\s*!=\s*0\s*\)\s*&&\s*s_reset_erases_kiln_nvs\s*&&\s*\(\s*s_reset_job_task\s*==\s*NULL\s*\|\|\s*s_reset_job_task\s*!=\s*me\s*\)\s*;(?<post>[^{}]*?)return\s+refuse\s*;\s*\}'
$bm = [regex]::Match($raStripped, $bodyPat)
# post may hold only the matching portEXIT_CRITICAL(...); call, so strip it before testing for other statements.
$postTxt = if ($bm.Success) { ($bm.Groups['post'].Value -replace 'portEXIT_CRITICAL\s*\(\s*&s_heat_claim_mux\s*\)\s*;', '').Trim() } else { '' }
$preOk = $bm.Success -and ($bm.Groups['pre'].Value -notmatch '\breturn\b|\bgoto\b')
if (-not $bm.Success -or $postTxt -ne '' -or -not $preOk) {
    Write-Host 'FAIL: relay_authority_reset_refuses_kiln_nvs_writer() body in relay_authority.c no longer computes (s_reset_in_flight_depth != 0) && s_reset_erases_kiln_nvs && (s_reset_job_task == NULL || s_reset_job_task != me) and returns it with no early return / reassignment of refuse'
    $fail++
}

if ($fail -gt 0) {
    Write-Host "check_reset_fence_hooks: $fail problem(s)"
    exit 1
}
Write-Host "PASS: check_reset_fence_hooks (all 4 reset-fence hooks installed by main.c)"
exit 0
