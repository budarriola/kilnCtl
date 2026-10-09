# check_boot_guard_reset_reachability.ps1 -- RELEASE_HARDENING.md
# section 6 / docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md.
#
# boot_guard_reset_counter() is a deliberate, externally-triggered clear of
# the recovery-mode counter -- correct ONLY when called by something that
# KNOWS a deliberate, just-verified-good flash happened (today: the
# authenticated POST /api/ota/esp/boot_guard_reset route in
# ota_http_recovery.c, called from flash_firmware()'s verify step). A
# negative test run during that fix's own development proved that wiring
# this function into every boot_guard_init() call instead (i.e. an
# unconditional firmware boot path calling it on its own) defeats the
# counter entirely -- it masks a genuinely, repeatedly failing board instead
# of tripping recovery mode, which is the whole point of the counter. See
# CLAUDE.md's "boot_guard_reset_counter()" paragraph and
# firmware/KilnFW/App/test/test_boot_guard.c's
# "additive escape hatch for a deliberate flash, not a general-purpose
# unlatch" comment.
#
# That property (unconditional-boot-path code must never call this
# function) has no compiler or host-test enforcement of its own today --
# test_boot_guard.c and test_ota_http.c prove the function's OWN behavior
# (verify-with-retry, gating on real success) but nothing stops a future
# change from ALSO calling it from main_boot_early.c or boot_guard_init()
# itself alongside the existing, correct call site. This check is that
# guard: a static grep over checked-in firmware source for every call
# (not declaration/definition) of boot_guard_reset_counter(), refusing if
# any call site exists outside the allowed list below.
#
# Usage:
#   powershell -File firmware\KilnFW\App\test\check_boot_guard_reset_reachability.ps1
#   powershell -File ... -SourceRoot <dir>
#
# -SourceRoot exists so this script's own negative test can point it at a
# deliberately sabotaged copy of the tree without touching the real one
# (per feedback_negative_test_restore_by_hand -- restore by hand with Edit,
# never `git checkout --`/`git restore`/`git stash`, since this tree is
# shared).

param(
    [string]$SourceRoot
)

$ErrorActionPreference = "Stop"

# $PSScriptRoot = ...\firmware\KilnFW\App\test
$appRoot = Split-Path -Parent $PSScriptRoot
if (-not $SourceRoot) { $SourceRoot = $appRoot }

if (-not (Test-Path $SourceRoot)) {
    throw "check_boot_guard_reset_reachability.ps1: $SourceRoot not found"
}

# Allowed production call site(s), relative-path-suffix match so this works
# whether $SourceRoot is the real App/ or a copied fake tree rooted the same
# way. Test files (test_*.c anywhere under a test/ or Test/ directory) are
# always allowed -- they deliberately call/stub the real function to prove
# its own behavior.
$allowedProdSuffixes = @(
    "drivers\http\ota_http_recovery.c",
    "drivers/http/ota_http_recovery.c"
)

$sourceFiles = Get-ChildItem -Path $SourceRoot -Recurse -File -Include *.c,*.h |
    Where-Object { $_.FullName -notmatch '\\build[\\/]' }

# Matches a CALL: boot_guard_reset_counter(  with nothing between the parens
# (the function takes no arguments), but excludes the definition/declaration
# form "boot_guard_reset_counter(void)" only when it is the function's own
# signature line (starts with a return-type token or is inside boot_guard.c
# right after a doc comment). Simpler and more robust: declarations/
# definitions always spell the parameter as "(void)"; every real call site
# spells it "()" with nothing inside. That is a clean, unambiguous split in
# this codebase's own style (verified against boot_guard.h/.c and every
# current call site).
$callPattern = 'boot_guard_reset_counter\(\)'

$violations = @()

foreach ($file in $sourceFiles) {
    $isTestFile = $file.FullName -match '\\test[\\/]|\\Test[\\/]' -or $file.Name -match '^test_'
    if ($isTestFile) { continue }

    $isAllowedProd = $false
    foreach ($suffix in $allowedProdSuffixes) {
        if ($file.FullName.Replace('/', '\').EndsWith($suffix.Replace('/', '\'))) {
            $isAllowedProd = $true
            break
        }
    }
    if ($isAllowedProd) { continue }

    $lines = @(Get-Content -Path $file.FullName)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $line = $lines[$i]
        $trimmed = $line.Trim()
        # Skip comment lines (//, /* ... */ single-line, or a line that is
        # purely inside a block comment starting with * ) -- this check
        # cares about actual calls compiled into the binary, not prose
        # mentioning the function name.
        if ($trimmed.StartsWith("//") -or $trimmed.StartsWith("*") -or $trimmed.StartsWith("/*")) { continue }
        if ($line -match $callPattern) {
            $violations += "$($file.FullName):$($i + 1): $($line.Trim())"
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host "BOOT_GUARD_RESET_COUNTER REACHABILITY CHECK FAILED:" -ForegroundColor Red
    Write-Host "boot_guard_reset_counter() is called from an unexpected site." -ForegroundColor Red
    Write-Host "Only $($allowedProdSuffixes[0]) (the authenticated POST /api/ota/esp/boot_guard_reset route," -ForegroundColor Red
    Write-Host "reached only after flash_firmware()'s post-flash verification confirms full success) may call it." -ForegroundColor Red
    Write-Host "A call from an unconditional boot path (boot_guard_init(), main_boot_early.c, etc.) defeats the" -ForegroundColor Red
    Write-Host "recovery-mode counter -- see docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md." -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  $v" -ForegroundColor Red
    }
    throw "$($violations.Count) unexpected boot_guard_reset_counter() call site(s) found"
}

Write-Host "boot_guard_reset_counter() reachability check passed: no call sites outside $($allowedProdSuffixes[0]) and test files."
exit 0
