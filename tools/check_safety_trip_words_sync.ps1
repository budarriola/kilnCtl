# checkcache: ok
# check_safety_trip_words_sync.ps1 -- guards the hand-mirrored safety
# trip/warn word tables against silent drift.
#
# Commit 52f4c944 ("S5 wording: make 'absent probe' the plain reading, not
# 'invalid'") had to hand-edit the SAME fact into THREE places:
#   - firmware/KilnFW/App/drivers/safety/safety_trip_words.h (C, feeds LCD/
#     web/JSON): safety_trip_words_short()'s switch, and the bits[] table
#     inside safety_warn_words_short()
#   - firmware/KilnFW/App/drivers/http/safety_page.html (JS): WARN_MASK_WORDS
#   - firmware/KilnFW/App/drivers/http/main_page.html (JS): SAFETY_TRIP_WORDS
# Nothing enforces these stay in sync -- exactly the "two copies joined by a
# contract nothing enforces" shape this repo has been burned by repeatedly
# (see CLAUDE.md's reset-one-side-of-a-pair section). This script is that
# enforcement, run as part of tools/run_all_checks.ps1.
#
# WHAT IS COMPARED, and why keyed on the guard identifier (S1..S15, or the
# warn-mask bit number that stands in for one) rather than on file paths or
# line numbers:
#
#   1. WARN_MASK_WORDS (JS, safety_page.html) vs the bits[] array inside
#      safety_warn_words_short() (C, safety_trip_words.h). Both tables are
#      declared, in their own header/inline comments, as byte-for-byte
#      mirrors of each other (safety_page.html: "JS mirror of
#      safety_trip_words.h's safety_warn_words_short()"). Keyed on the bit
#      number (which corresponds 1:1 with a guard's trip-reason code minus
#      one, or a reserved gap bit for a WARN-only guard) -- the check builds
#      a {bit -> string} dictionary from each side independently (regex over
#      each file's own table literal, not a fixed line number) and requires
#      the same set of keys AND identical string values.
#
#   2. Guard-tag cross-check between safety_trip_words_short() (C) and
#      SAFETY_TRIP_WORDS (JS, main_page.html). These two tables are NOT
#      required to be byte-identical -- one is a <=21-char LCD label, the
#      other a full sentence for a web fallback path -- but every entry in
#      each is required to name its guard with a trailing/leading "S<n>"
#      (or "S6a"/"S6b") tag, and for a given trip-reason CODE, that tag must
#      be the SAME letter+number in both tables. This is the check that
#      would have caught 52f4c944 pointing the wrong reason code at the
#      wrong guard name, though not a same-tag wording change -- see the
#      LCD-budget check below for the piece that does catch wording growth.
#
#   3. LCD character budget: every safety_trip_words_short() label must fit
#      the measured 21-character no-wrap budget (ui_page_home.c /
#      ui_page_safety.c, 480x320 LCD, no scrolling per CLAUDE.md's LCD
#      section) -- a label that grows past this truncates silently on the
#      panel with no error anywhere in this pipeline.
#
# WHAT WOULD INVALIDATE THIS CHECK (update it, don't just delete it, if any
# of these happen):
#   - safety_trip_words.h's bits[] literal syntax changes shape (currently
#     `{ 1u << N, "text" }` rows) -- the regex below is pinned to that exact
#     shape.
#   - WARN_MASK_WORDS or SAFETY_TRIP_WORDS stop being simple `key: 'value',`
#     JS object literals (e.g. become computed/generated) -- the regex
#     assumes a literal numeric key followed by a single- or double-quoted
#     string.
#   - The tables move to new files/paths (a past failure mode in this repo --
#     see CLAUDE.md "Splits break filename-keyed checks"): this script takes
#     the three file paths as parameters with today's paths as defaults
#     specifically so a mechanical `git mv` plus one parameter edit keeps it
#     alive, instead of it silently scanning nothing.
#   - A guard's short label legitimately needs to exceed 21 characters
#     because the LCD layout that budget was measured against changed --
#     update the $LcdCharBudget constant together with that layout change,
#     not by deleting the check.
#   - A real, intentional generation step replaces one or more of these
#     hand-written tables (see this script's companion note in the commit
#     that added it) -- delete the corresponding comparison, not the whole
#     script, since the others may still be hand-mirrored.
#
# This does NOT compare profile_executor.h's profile_executor_safety_trip_
# words() -- that is documented (in that header, and safety_trip_words.h's
# own top comment) as a deliberately separate, differently-sized table for a
# 96-byte fault_reason wire field, not a mirror of either table checked here.
#
# Usage: powershell -File tools\check_safety_trip_words_sync.ps1
param(
    [string]$CHeaderPath,
    [string]$SafetyPageHtmlPath,
    [string]$MainPageHtmlPath,
    [int]$LcdCharBudget = 21
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
if (-not $CHeaderPath) {
    $CHeaderPath = Join-Path $root "..\firmware\KilnFW\App\drivers\safety\safety_trip_words.h"
}
if (-not $SafetyPageHtmlPath) {
    $SafetyPageHtmlPath = Join-Path $root "..\firmware\KilnFW\App\drivers\http\safety_page.html"
}
if (-not $MainPageHtmlPath) {
    $MainPageHtmlPath = Join-Path $root "..\firmware\KilnFW\App\drivers\http\main_page.html"
}
$CHeaderPath = (Resolve-Path $CHeaderPath).Path
$SafetyPageHtmlPath = (Resolve-Path $SafetyPageHtmlPath).Path
$MainPageHtmlPath = (Resolve-Path $MainPageHtmlPath).Path

$failures = @()

# ---------------------------------------------------------------------------
# 1. WARN_MASK_WORDS (JS) vs safety_warn_words_short()'s bits[] table (C).
# ---------------------------------------------------------------------------

$cText = Get-Content -Raw -Path $CHeaderPath

$cBitsBlockMatch = [regex]::Match($cText,
    'safety_warn_words_short[\s\S]*?bits\[\]\s*=\s*\{([\s\S]*?)\};')
if (-not $cBitsBlockMatch.Success) {
    throw "check_safety_trip_words_sync.ps1: could not find safety_warn_words_short()'s bits[] table in $CHeaderPath -- has its shape changed? Update this script's regex (see header comment 'WHAT WOULD INVALIDATE THIS CHECK')."
}
$cBitsBlock = $cBitsBlockMatch.Groups[1].Value

$cWarnWords = @{}
foreach ($m in [regex]::Matches($cBitsBlock, '1u\s*<<\s*(\d+)\s*,\s*"([^"]*)"')) {
    $cWarnWords[[int]$m.Groups[1].Value] = $m.Groups[2].Value
}
if ($cWarnWords.Count -lt 5) {
    throw "check_safety_trip_words_sync.ps1: parsed only $($cWarnWords.Count) entries from safety_warn_words_short()'s bits[] table -- implausibly few, the parser has probably gone blind. Update the regex."
}

$jsText = Get-Content -Raw -Path $SafetyPageHtmlPath
$jsBlockMatch = [regex]::Match($jsText, 'WARN_MASK_WORDS\s*=\s*\{([\s\S]*?)\};')
if (-not $jsBlockMatch.Success) {
    throw "check_safety_trip_words_sync.ps1: could not find WARN_MASK_WORDS in $SafetyPageHtmlPath -- has it moved or been renamed? Update this script."
}
$jsBlock = $jsBlockMatch.Groups[1].Value

$jsWarnWords = @{}
foreach ($m in [regex]::Matches($jsBlock, "(\d+)\s*:\s*'([^']*)'")) {
    $jsWarnWords[[int]$m.Groups[1].Value] = $m.Groups[2].Value
}
if ($jsWarnWords.Count -lt 5) {
    throw "check_safety_trip_words_sync.ps1: parsed only $($jsWarnWords.Count) entries from WARN_MASK_WORDS -- implausibly few, the parser has probably gone blind. Update the regex."
}

$allBits = ($cWarnWords.Keys + $jsWarnWords.Keys) | Sort-Object -Unique
foreach ($bit in $allBits) {
    $inC = $cWarnWords.ContainsKey($bit)
    $inJs = $jsWarnWords.ContainsKey($bit)
    if (-not $inC) {
        $failures += "warn bit $bit present in $SafetyPageHtmlPath ('$($jsWarnWords[$bit])') but missing from safety_warn_words_short()'s bits[] table in $CHeaderPath"
        continue
    }
    if (-not $inJs) {
        $failures += "warn bit $bit present in safety_warn_words_short()'s bits[] table ('$($cWarnWords[$bit])') but missing from WARN_MASK_WORDS in $SafetyPageHtmlPath"
        continue
    }
    if ($cWarnWords[$bit] -ne $jsWarnWords[$bit]) {
        $failures += "warn bit $bit text drift: C safety_warn_words_short() says '$($cWarnWords[$bit])' but JS WARN_MASK_WORDS ($SafetyPageHtmlPath) says '$($jsWarnWords[$bit])'"
    }
}

# ---------------------------------------------------------------------------
# 2. Guard-tag cross-check: safety_trip_words_short() (C) vs SAFETY_TRIP_WORDS
#    (JS, main_page.html) -- same trip-reason code must name the same guard.
# ---------------------------------------------------------------------------

$cShortBlockMatch = [regex]::Match($cText,
    'safety_trip_words_short[\s\S]*?\{([\s\S]*?)default:\s*return[^;]*;\s*\}\s*\}')
if (-not $cShortBlockMatch.Success) {
    throw "check_safety_trip_words_sync.ps1: could not find safety_trip_words_short()'s switch body in $CHeaderPath -- update this script's regex."
}
$cShortBlock = $cShortBlockMatch.Groups[1].Value

$cShortLabels = @{}
foreach ($m in [regex]::Matches($cShortBlock, 'case\s+(\d+)\s*:\s*return\s*"([^"]*)"')) {
    $cShortLabels[[int]$m.Groups[1].Value] = $m.Groups[2].Value
}
if ($cShortLabels.Count -lt 10) {
    throw "check_safety_trip_words_sync.ps1: parsed only $($cShortLabels.Count) entries from safety_trip_words_short() -- implausibly few, update the regex."
}

$mainText = Get-Content -Raw -Path $MainPageHtmlPath
$mainBlockMatch = [regex]::Match($mainText, 'SAFETY_TRIP_WORDS\s*=\s*\{([\s\S]*?)\};')
if (-not $mainBlockMatch.Success) {
    throw "check_safety_trip_words_sync.ps1: could not find SAFETY_TRIP_WORDS in $MainPageHtmlPath -- has it moved or been renamed? Update this script."
}
$mainBlock = $mainBlockMatch.Groups[1].Value

$jsTripWords = @{}
foreach ($m in [regex]::Matches($mainBlock, "(\d+)\s*:\s*'([^']*)'")) {
    $jsTripWords[[int]$m.Groups[1].Value] = $m.Groups[2].Value
}
if ($jsTripWords.Count -lt 10) {
    throw "check_safety_trip_words_sync.ps1: parsed only $($jsTripWords.Count) entries from SAFETY_TRIP_WORDS -- implausibly few, update the regex."
}

$tagPattern = '[Ss](\d+[ab]?)'

foreach ($reason in $cShortLabels.Keys) {
    if ($reason -eq 0) { continue } # 'none' entries carry no guard tag on either side
    if (-not $jsTripWords.ContainsKey($reason)) {
        # Not every C reason code necessarily has a JS fallback entry (JS
        # table's own comment: it's an "old-firmware fallback"); absence
        # alone is not a drift finding here. Skip.
        continue
    }
    $cLabel = $cShortLabels[$reason]
    $jsLabel = $jsTripWords[$reason]

    $cTagMatch = [regex]::Match($cLabel, '^' + $tagPattern)
    $jsTagMatch = [regex]::Match($jsLabel, '\(' + $tagPattern + '\)\s*$')

    if (-not $cTagMatch.Success) {
        # Guards with no numbered "Sn" identity at all (config corrupt,
        # self-test fail) carry no tag on either side -- nothing to
        # cross-check, and that is correct, not drift.
        continue
    }
    if (-not $jsTagMatch.Success) {
        $failures += "reason $reason`: JS SAFETY_TRIP_WORDS label '$jsLabel' ($MainPageHtmlPath) has no trailing (S<n>) guard tag -- cannot cross-check against C"
        continue
    }
    if ($cTagMatch.Groups[1].Value -ne $jsTagMatch.Groups[1].Value) {
        $failures += "reason $reason`: guard tag mismatch -- C safety_trip_words_short() says S$($cTagMatch.Groups[1].Value) ('$cLabel') but JS SAFETY_TRIP_WORDS says S$($jsTagMatch.Groups[1].Value) ('$jsLabel')"
    }
}

# ---------------------------------------------------------------------------
# 3. LCD character budget on safety_trip_words_short()'s labels.
# ---------------------------------------------------------------------------

foreach ($reason in $cShortLabels.Keys) {
    $label = $cShortLabels[$reason]
    if ($label.Length -gt $LcdCharBudget) {
        $failures += "reason $reason`: safety_trip_words_short() label '$label' is $($label.Length) chars, over the $LcdCharBudget-char no-wrap LCD budget (480x320, see ui_page_home.c/ui_page_safety.c) -- it will truncate silently on the panel"
    }
}

# ---------------------------------------------------------------------------

if ($failures.Count -gt 0) {
    Write-Host "SAFETY TRIP WORDS SYNC CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "Safety trip/warn word tables have drifted apart (or a label has grown past the LCD budget). See $CHeaderPath's own top comment: all copies must be updated together."
}

Write-Host "Safety trip words sync check passed: $($cWarnWords.Count) warn-mask labels match byte-for-byte, $($cShortLabels.Count) LCD labels within the $LcdCharBudget-char budget, guard tags agree between C and JS trip-word tables."
exit 0
