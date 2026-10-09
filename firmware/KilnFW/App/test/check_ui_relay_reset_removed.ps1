# check_ui_relay_reset_removed.ps1 -- the lint half of the two-layer pattern
# this repo already uses for a rule with no compiler to enforce it (see
# check_ui_budget_asserts.ps1's own header comment for the sibling of this
# script). UI_PLAN.md section 6.4 (2026-09-19, commit bf81fb86) removed the
# LCD's own two-tap Relay Life Reset control -- the reset capability stays,
# but only through the web diagnostics page (kcConfirm()); the driver-level
# relay_cycles_reset_timeout() function stays too, since the web path still
# calls it. Nothing stops a future edit from re-adding an LCD-side call into
# it (a partial revert, or a well-meaning "let's also expose this on the
# LCD" pass that forgets section 6.4's removal was deliberate, TODO.md-
# tracked, and owner-approved). This script greps every firmware/KilnFW/
# App/drivers/ui/*.c file and fails loudly if any of them mentions the bare
# identifier relay_cycles_reset -- even inside a comment, since a comment
# reintroducing that exact call is still a signal something is being
# re-added. The match is word-boundary, not a bare substring search: the
# DIFFERENT, still-legitimate identifier relay_cycles_reset_timeout()
# (documented in ui_page_diagnostics.c's own header comments as staying in
# the driver for the web path) contains relay_cycles_reset as a substring
# but is not followed by a word boundary, so it does not false-trip this
# check on its own -- unlike check_ui_budget_asserts.ps1's comment-stripping
# approach, word-boundary matching is enough here because the two
# identifiers are textually distinguishable by their suffix alone.
#
# Section 6.6 (2026-09-19, same commit) reworked the LCD home topbar's icon
# row to right-align via LV_FLEX_ALIGN_END and put the warning indicator
# before the settings gear in build order (so it is always to the LEFT of
# the gear once both are flex-END-aligned and the gear is added last -- see
# ui_topbar.c's own comment at the LV_FLEX_ALIGN_END call site). This script
# also checks that shape stays intact: the flex-align call must still be
# present, and warning_btn's construction must still appear at a lower line
# number than gear_btn's.
#
# Negative-tested 2026-09-19 (this script's own authoring pass): each
# required condition below was broken one at a time (relay_cycles_reset
# added inside a comment in ui_page_diagnostics.c; LV_FLEX_ALIGN_END call
# temporarily changed; warning_btn/gear_btn construction order swapped in
# ui_topbar.c), this script re-run and confirmed to report exactly that
# failure, then the change was reverted by hand (never `git checkout --`,
# since other sessions share this tree) and confirmed via `git diff` +
# `git hash-object` matching HEAD's blob before re-confirming PASS.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_ui_relay_reset_removed.ps1
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "_drivers_layout.ps1")
$driversRoot = Get-DriversRoot -TestDir $PSScriptRoot
if (-not (Test-Path $driversRoot)) {
    throw "check_ui_relay_reset_removed.ps1: expected source tree not found at $driversRoot"
}

$uiDir = Join-Path $driversRoot "ui"
if (-not (Test-Path $uiDir)) {
    throw "check_ui_relay_reset_removed.ps1: expected UI layer not found at $uiDir"
}

$failures = @()

# --- Part 1 (REQUIRED, section 6.4): no ui/*.c file may mention the bare
# identifier relay_cycles_reset (word-boundary match, so ui_page_diagnostics.c's
# own header comments naming the DIFFERENT, still-legitimate identifier
# relay_cycles_reset_timeout -- which stays in the driver for the web path --
# do not false-trip this on their own; see header comment above for why).
$uiFiles = Get-ChildItem -Path $uiDir -Filter "*.c" -File -Recurse
$forbidden = '(?<![A-Za-z0-9_])relay_cycles_reset(?![A-Za-z0-9_])'
foreach ($file in $uiFiles) {
    $raw = Get-Content -Path $file.FullName -Raw
    if ($raw -match $forbidden) {
        $failures += "$($file.Name): mentions the bare identifier relay_cycles_reset (even in a comment) -- " +
            "UI_PLAN.md section 6.4 (2026-09-19) removed the LCD's Relay Life Reset control " +
            "deliberately; the reset capability lives only on the web diagnostics page now. " +
            "If this is a real re-add, update this check (and UI_PLAN.md) rather than silence it."
    }
}

# --- Part 2 (optional, section 6.6): topbar right-alignment shape.
try {
    $topbarPath = Resolve-DriverFile -DriversDir $driversRoot -BaseName "ui_topbar.c"
} catch {
    $topbarPath = $null
}
if (-not $topbarPath) {
    $failures += "ui_topbar.c not found anywhere under $driversRoot -- section 6.6's topbar shape can't be checked."
} else {
    $lines = Get-Content -Path $topbarPath
    $alignLine = ($lines | Select-String -Pattern ([regex]::Escape("lv_obj_set_flex_align(icons, LV_FLEX_ALIGN_END,")) -SimpleMatch:$false)
    if (-not $alignLine) {
        $failures += "ui_topbar.c: missing required 'lv_obj_set_flex_align(icons, LV_FLEX_ALIGN_END,' -- " +
            "UI_PLAN.md section 6.6 (2026-09-19) right-aligned the topbar icon row; do not revert to " +
            "left/start alignment without updating this check and the plan."
    }

    $warningLine = ($lines | Select-String -Pattern "warning_btn = build_indicator" | Select-Object -First 1)
    $gearLine = ($lines | Select-String -Pattern "gear_btn = build_icon" | Select-Object -First 1)
    if (-not $warningLine) {
        $failures += "ui_topbar.c: missing expected 'warning_btn = build_indicator(...)' construction line."
    }
    if (-not $gearLine) {
        $failures += "ui_topbar.c: missing expected 'gear_btn = build_icon(...)' construction line."
    }
    if ($warningLine -and $gearLine) {
        if ($warningLine.LineNumber -ge $gearLine.LineNumber) {
            $failures += "ui_topbar.c: warning_btn is built at line $($warningLine.LineNumber), which is not " +
                "before gear_btn at line $($gearLine.LineNumber) -- with LV_FLEX_ALIGN_END, build order " +
                "determines left-to-right placement, and section 6.6 requires the warning indicator to " +
                "sit to the left of the settings gear."
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "check_ui_relay_reset_removed.ps1: FAILED --" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  $f" -ForegroundColor Red }
    exit 1
}

Write-Host "check_ui_relay_reset_removed.ps1: OK -- no LCD-side relay_cycles_reset call, and ui_topbar.c's right-aligned warning-before-gear shape (section 6.6) is intact." -ForegroundColor Green
exit 0
