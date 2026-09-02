# check_stop_bar_body_padding.ps1 -- guards the fix for the owner's paired
# reports: "on the pc i can not scroll down enough to see all of the items"
# and "the pause/firing buttons cover some content on the pc web page" (and,
# 2026-09-01, a follow-up that the same fixed bar can cover the LAST button
# on a page at PHONE width too -- the reset page's "Factory default" button).
#
# theme.css's .kc-stop-bar is `position: fixed; bottom: 0`, deliberately (it
# must stay reachable through a live firing from every page -- see that
# rule's own comment for why it is NOT un-fixed the way the connection-lost
# banner is). A fixed-position bar overlaying the bottom of the viewport is
# only safe if something else reserves exactly that much space at the
# bottom of the document, and un-reserves it the instant the bar is hidden
# again -- nav.js's updateBodyPadding() plus the hooks in app.js that call
# it are that "something else". This script cannot render a page (no
# browser/jsdom in this toolchain) or talk to the live board (its HTTP
# server is not to be hit from automated tooling during a firing -- see
# BOOT.md/this repo's live-hardware safety rule), so it checks the thing it
# CAN check without either: that the specific lines this fix depends on are
# still present, verbatim enough to survive a reflow but not so loose that
# deleting the mechanism and leaving a comment about it would still pass.
#
# Same two-layer pattern as check_ui_budget_asserts.ps1 in this directory:
# nothing forces a future edit to keep calling updateBodyPadding() from
# every place that can change the bar's presence or size; this is the
# no-render mirror that fails loudly if one of those call sites goes missing.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_stop_bar_body_padding.ps1
$ErrorActionPreference = "Stop"

$driversRoot = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "App\drivers"
if (-not (Test-Path $driversRoot)) {
    throw "check_stop_bar_body_padding.ps1: expected source tree not found at $driversRoot"
}

function Get-StrippedJs([string]$path) {
    # Strip // and /* */ comments before matching, same reasoning as
    # check_ui_budget_asserts.ps1 and check_label_column_overflow_wrap.ps1:
    # this codebase documents fixed bugs in prose quoting the exact code
    # that fixed them, so a naive scan of the raw text would be satisfied by
    # a comment describing the mechanism even if the mechanism itself were
    # deleted.
    $raw = Get-Content -Path $path -Raw
    $stripped = $raw -replace '(?s)/\*.*?\*/', ' '
    $stripped = $stripped -replace '(?m)//.*$', ' '
    return ($stripped -replace '\s+', ' ')
}

$failures = @()

# --- theme.css: the bar must stay position:fixed, bottom:0 --------------
$themePath = Join-Path $driversRoot "theme.css"
if (-not (Test-Path $themePath)) {
    $failures += "theme.css not found at $themePath"
} else {
    $themeText = Get-Content -Path $themePath -Raw
    if ($themeText -notmatch '\.kc-stop-bar\s*\{[^}]*position:\s*fixed[^}]*bottom:\s*0') {
        $failures += "theme.css: .kc-stop-bar is no longer 'position: fixed; ... bottom: 0' -- " +
            "if this changed on purpose (e.g. un-fixing it like the connection-lost banner), the " +
            "body-padding reservation below becomes unnecessary and should be removed with it, not " +
            "left half-changed. See theme.css's own .kc-stop-bar comment before changing this."
    }
}

# --- nav.js: updateBodyPadding must exist and actually measure the bar --
$navPath = Join-Path $driversRoot "nav.js"
if (-not (Test-Path $navPath)) {
    $failures += "nav.js not found at $navPath"
} else {
    $nav = Get-StrippedJs $navPath
    if ($nav -notmatch "function updateBodyPadding\s*\(") {
        $failures += "nav.js: updateBodyPadding() is missing -- this is the function that reserves " +
            "body bottom padding equal to the stop bar's actual rendered height."
    }
    if ($nav -notmatch "querySelector\('\.kc-stop-bar'\)") {
        $failures += "nav.js: updateBodyPadding() no longer looks up .kc-stop-bar -- it cannot measure " +
            "a bar it doesn't find."
    }
    if ($nav -notmatch "hasAttribute\('hidden'\)") {
        $failures += "nav.js: updateBodyPadding() no longer checks the bar's hidden attribute -- " +
            "without this it would either always reserve space (a permanent dead gap with no firing " +
            "running) or never reserve it (the original bug)."
    }
    if ($nav -notmatch "offsetHeight") {
        $failures += "nav.js: updateBodyPadding() no longer reads the bar's offsetHeight -- the " +
            "reservation must track the bar's ACTUAL height (buttons sized by var(--ui-touch) plus " +
            "its own padding), not a hardcoded px guess."
    }
    if ($nav -notmatch "paddingBottom\s*=") {
        $failures += "nav.js: updateBodyPadding() no longer sets body.style.paddingBottom -- this is " +
            "the actual reservation; without it the function measures the bar and does nothing with it."
    }
    if ($nav -notmatch "window\.kcNav\s*=") {
        $failures += "nav.js: window.kcNav is no longer exported -- app.js's hooks below call " +
            "window.kcNav.updateBodyPadding() through this seam."
    }
}

# --- app.js: every place that can change the bar's presence or size must
#     re-run the reservation -------------------------------------------
$appPath = Join-Path $driversRoot "app.js"
if (-not (Test-Path $appPath)) {
    $failures += "app.js not found at $appPath"
} else {
    $app = Get-StrippedJs $appPath
    if ($app -notmatch "kcNav\.updateBodyPadding\s*\(\s*\)") {
        $failures += "app.js: no call to kcNav.updateBodyPadding() found at all -- the bar can attach " +
            "or detach (a firing starting/ending) with nothing telling nav.js to re-measure it."
    }
    # 2026-09-01: the ResizeObserver added after the reset-page report ("the
    # pause/firing buttons cover the last page button" on a phone, where the
    # bar's CONTENT -- not its visibility -- changed height when the
    # pause/stop/acknowledge buttons swapped). This is the mechanism that is
    # right by construction for every future cause of a height change, not
    # just the ones already known about -- see app.js's own comment on
    # observeStopBarHeight() for the full reasoning.
    if ($app -notmatch "ResizeObserver") {
        $failures += "app.js: no ResizeObserver on the stop bar -- setStopBarVisible() alone only " +
            "re-measures on a hidden<->shown transition, not when the bar's CONTENT changes height " +
            "at a constant visibility (e.g. the button set swapping and wrapping to a second line at " +
            "phone width). See app.js's observeStopBarHeight() comment (2026-09-01)."
    }
}

if ($failures.Count -gt 0) {
    Write-Host "STOP BAR BODY PADDING CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "$($failures.Count) piece(s) of the stop-bar body-padding reservation are missing -- see this script's header comment."
}

Write-Host "Stop bar body padding check passed: theme.css/.kc-stop-bar, nav.js/updateBodyPadding(), and app.js's hooks (including the ResizeObserver) are all present."
exit 0
