# check_ui_budget_asserts.ps1 -- the lint half of the two-layer pattern this
# repo already uses for enforcing an invariant that lives inside a
# _Static_assert (see firmware/SimFW/tools/check_single_owner.ps1's DMA
# budget section for the sibling of this script): the compiler is the
# AUTHORITATIVE check (it evaluates the real arithmetic against the real
# ui_theme.h constants at every build), but nothing stops a future edit from
# deleting the assertion itself -- e.g. "this keeps failing, let's just
# remove it" during an unrelated pass. This script is the no-compiler mirror:
# it greps for each assertion's exact text and fails loudly if any is
# missing, so a deleted assertion is caught even by someone who only runs
# this script and never actually builds the firmware.
#
# TODO.md's two LCD no-scroll budget items (2026-08-24, both closed the same
# pass this script was added in):
#   - ui_page_temperature.c's per-zone relay-row fit depended on RUNTIME
#     relay count with no compile-time cap -- fixed with
#     UI_PAGE_TEMPERATURE_MAX_RELAYS_PER_ZONE (tied to KILN_IO_RELAY_COUNT)
#     and a _Static_assert bounding MAX31856_CHANNEL_COUNT zone cards against
#     ui_theme.h's UI_THEME_PAGE_CONTENT_BUDGET_PX.
#   - ui_page_network.c's worst-case fit was computed at ~268px against a
#     ~264px budget -- already over. Fixed by splitting Scan/Saved into
#     ui_page_network_manage.c (TODO.md's own named remedy) and adding
#     _Static_asserts to all three of the resulting pages' worst cases.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_ui_budget_asserts.ps1
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "_drivers_layout.ps1")
$driversRoot = Get-DriversRoot -TestDir $PSScriptRoot
if (-not (Test-Path $driversRoot)) {
    throw "check_ui_budget_asserts.ps1: expected source tree not found at $driversRoot"
}

# Each entry: the file, and the exact _Static_assert condition(s) that file
# must still contain (as literal substrings, whitespace-normalized below so
# a reflow/re-indent doesn't false-fail this check). One file may carry more
# than one assertion (ui_page_network.c has two: STA-mode and AP-mode are
# separate reachable states that must each independently fit the budget).
$required = @(
    @{
        File = "ui_page_temperature.c"
        Asserts = @(
            "_Static_assert(UI_PAGE_TEMPERATURE_MAX_RELAYS_PER_ZONE == KILN_IO_RELAY_COUNT,",
            "_Static_assert(UI_PAGE_TEMPERATURE_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,"
        )
    },
    @{
        File = "ui_page_network.c"
        Asserts = @(
            "_Static_assert(UI_PAGE_NETWORK_STA_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,",
            "_Static_assert(UI_PAGE_NETWORK_AP_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,"
        )
    },
    @{
        File = "ui_page_network_manage.c"
        Asserts = @(
            "_Static_assert(UI_PAGE_NETWORK_MANAGE_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,"
        )
    },
    @{
        File = "ui_page_diagnostics.c"
        Asserts = @(
            "_Static_assert(UI_PAGE_DIAGNOSTICS_SAFETY_BH_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,",
            "_Static_assert(UI_PAGE_DIAGNOSTICS_THERMO_FAULT_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,"
        )
    },
    @{
        File = "ui_page_profile_detail.c"
        Asserts = @(
            "_Static_assert(UI_PAGE_PROFILE_DETAIL_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,"
        )
    }
)

# Also require the shared budget constant itself -- deleting
# UI_THEME_PAGE_CONTENT_BUDGET_PX from ui_theme.h would make every assert
# above fail to COMPILE (a hard build error, so that particular deletion is
# already caught for free) but this script should say so in its own words
# too rather than relying solely on the build failing for an unrelated-
# looking reason.
$themeFile = "ui_theme.h"
$themeConst = "#define UI_THEME_PAGE_CONTENT_BUDGET_PX"

$failures = @()

try {
    $themePath = Resolve-DriverFile -DriversDir $driversRoot -BaseName $themeFile
} catch {
    $themePath = $null
}
if (-not $themePath) {
    $failures += "$themeFile not found anywhere under $driversRoot"
} else {
    $themeText = Get-Content -Path $themePath -Raw
    if ($themeText -notmatch [regex]::Escape($themeConst)) {
        $failures += "$themeFile no longer defines UI_THEME_PAGE_CONTENT_BUDGET_PX -- this is the single " +
            "source of truth every page's no-scroll _Static_assert below checks against " +
            "(TODO.md's no-scroll LCD items, ui_page_home.c's original ~264px-budget derivation)."
    }
}

foreach ($entry in $required) {
    try {
        $path = Resolve-DriverFile -DriversDir $driversRoot -BaseName $entry.File
    } catch {
        $failures += "$($entry.File) not found anywhere under $driversRoot"
        continue
    }
    # Whitespace-normalize (collapse runs of spaces/tabs/newlines to a single
    # space) before matching, so a re-wrap of the _Static_assert's argument
    # list across a different number of lines doesn't false-fail this check
    # -- only the assertion's CONDITION and the fact that it is a
    # _Static_assert at all are load-bearing here, not its exact line
    # breaks.
    $raw = Get-Content -Path $path -Raw
    # Strip C comments BEFORE matching. This is not hygiene -- it is the
    # difference between a real check and a vacuous one. This codebase
    # documents a fixed bug in prose right next to the fixed code, so a
    # future comment quoting one of these _Static_assert lines (explaining
    # why it exists, say) would satisfy the search below even if the
    # assertion itself had been deleted -- the check would then pass on its
    # own explanation. firmware/SimFW/test/test_i2c_owner_bus_scan_probe_len_
    # coverage.c and firmware/SaftyFW/test/test_boot_checkin_coverage.c both
    # strip comments for exactly this reason; the latter's header comment
    # records that its target file's post-mortem comment really did mention
    # the searched-for call, and would have defeated a naive scan.
    $stripped = $raw -replace '(?s)/\*.*?\*/', ' '
    $stripped = $stripped -replace '(?m)//.*$', ' '
    $normalized = ($stripped -replace '\s+', ' ')
    foreach ($assertText in $entry.Asserts) {
        $normalizedAssert = ($assertText -replace '\s+', ' ')
        if ($normalized -notmatch [regex]::Escape($normalizedAssert)) {
            $failures += "$($entry.File): missing required _Static_assert -- `"$assertText`" " +
                "-- this is the compile-time half of TODO.md's no-scroll LCD budget fix; do not delete " +
                "it to silence a failing build, fix the real overflow instead (shrink a fixed height, " +
                "or split more content into another page via kiln_ui_show())."
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "UI BUDGET ASSERT CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "$($failures.Count) missing no-scroll budget _Static_assert(s) -- see TODO.md's no-scroll LCD " +
        "items and this script's own header comment."
}

Write-Host ("UI budget assert check passed: UI_THEME_PAGE_CONTENT_BUDGET_PX and all per-page worst-case " +
    "_Static_asserts (ui_page_temperature.c, ui_page_network.c, ui_page_network_manage.c, " +
    "ui_page_diagnostics.c, ui_page_profile_detail.c) are present.")
exit 0
