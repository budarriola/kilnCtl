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
# and kiln_nvs_reset_refuses_write() must itself consult relay_authority_reset_refuses_writer() (so the
# reset job's own task stays exempt).
#
# This check fails when main.c stops making any of those four calls (comments are stripped first, so a
# commented-out install does not count), or when the kiln_nvs predicate stops consulting the relay_authority
# predicate. Self-tested by tools\negtest.ps1 (see the mutations in this check's own history).
#
# Usage: powershell -File tools\check_reset_fence_hooks.ps1 [-MainFile <path to main.c>]
param(
    [string]$MainFile
)

$ErrorActionPreference = "Stop"

if (-not $MainFile) {
    $MainFile = Join-Path $PSScriptRoot "..\firmware\KilnFW\App\main.c"
}
if (-not (Test-Path -LiteralPath $MainFile)) {
    Write-Host "FAIL: main.c not found at $MainFile"
    exit 1
}
$text = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $MainFile).Path)

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

# The kiln_nvs predicate must keep consulting the shared reset predicate (and be a real function definition).
$predPat = 'static\s+bool\s+kiln_nvs_reset_refuses_write\s*\([^)]*\)\s*\{[^}]*relay_authority_reset_refuses_writer\s*\(\s*\)[^}]*\}'
if ($stripped -notmatch $predPat) {
    Write-Host "FAIL: kiln_nvs_reset_refuses_write() is missing or no longer calls relay_authority_reset_refuses_writer()"
    $fail++
}

if ($fail -gt 0) {
    Write-Host "check_reset_fence_hooks: $fail problem(s)"
    exit 1
}
Write-Host "PASS: check_reset_fence_hooks (all 4 reset-fence hooks installed by main.c)"
exit 0
