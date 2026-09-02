# check_label_column_overflow_wrap.ps1 -- guards against a bug class that hit
# this repo three times in one day (2026-09-01), each report from the owner
# looking like a different bug until traced to the same rule:
#
#   1. ota_page.html's RP2040 .kv block "looks smashed on the phone"
#   2. diagnostics_page.html's "Warning" label rendered as "War/nin/g"
#      (split vertically, one character per line)
#   3. safety_page.html's "Last trip (Frame D)" section wrapping vertically
#      on BOTH phone and desktop
#
# Root cause, all three: `overflow-wrap: anywhere` (and `word-break:
# break-all`, which has the same effect) does not just permit a mid-word
# break when a line would otherwise overflow -- per the CSS Text spec it
# ALSO participates in the element's min-content size calculation. Put that
# on a label column in a two-column label/value layout (`.row .label` /
# `.kv dt`, always a short, known string -- "Warning", "Reason", "Cause",
# never a hash or URL) and a flex/grid track sized off it collapses toward
# one character wide even when nothing would otherwise overflow, because the
# layout engine is now willing to shrink it arbitrarily far. The VALUE column
# (`.row .value` / `.kv dd`) is the opposite case: it legitimately carries
# unbreakable tokens (build commit hashes, hex codes, NVS section names) that
# really do need to be allowed to break, so it keeps `anywhere`/`break-all`
# on purpose. See diagnostics_page.html's and safety_page.html's `.row`
# comments, and ota_page.html's `.kv` comment, for the full per-site writeup.
#
# This is the grep-based mirror of that fix, same two-layer pattern as
# check_ui_budget_asserts.ps1 in this directory: nothing at build time stops
# a future page from copy-pasting `.foo .label { overflow-wrap: anywhere }`
# from some other rule that legitimately needs it, so this script exists to
# catch that at review time instead of waiting for the next "the label looks
# smashed" report.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_label_column_overflow_wrap.ps1
$ErrorActionPreference = "Stop"

$driversRoot = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "App\drivers"
if (-not (Test-Path $driversRoot)) {
    throw "check_label_column_overflow_wrap.ps1: expected source tree not found at $driversRoot"
}

# A "label-like" selector: any CSS rule whose selector names a class/element
# that this repo uses to hold a short, known string in a label/value pair --
# `.label`, or a `dt` (the `.kv` key/value convention ota_page.html uses).
# Deliberately word-bounded (`\blabel\b`, `\bdt\b`) so it does not also flag
# unrelated selectors that merely contain those letters as a substring (e.g.
# a hypothetical `.labelled-input`).
$labelSelectorPattern = '(\blabel\b|\bdt\b)[^{]*\{[^}]*\}'
$badValuePattern = 'overflow-wrap\s*:\s*anywhere|word-break\s*:\s*break-all'

$files = Get-ChildItem -Path $driversRoot -Filter "*_page.html" -File
$themeCss = Join-Path $driversRoot "theme.css"
if (Test-Path $themeCss) {
    $files = @($files) + (Get-Item $themeCss)
}

$failures = @()
$scanned = 0

foreach ($f in $files) {
    $text = Get-Content -Path $f.FullName -Raw
    # Only look inside <style> blocks for the .html files -- there is no
    # other CSS in these files, but being explicit keeps this from ever
    # matching something in a <script> block's string literals by accident.
    $styleBlocks = [regex]::Matches($text, '(?s)<style>(.*?)</style>')
    $cssText = if ($f.Extension -eq ".css") { $text } else { ($styleBlocks | ForEach-Object { $_.Groups[1].Value }) -join "`n" }
    if ([string]::IsNullOrEmpty($cssText)) { continue }
    $scanned++

    # Strip CSS comments before matching -- same reasoning as
    # check_ui_budget_asserts.ps1's identical step: this codebase documents
    # fixed bugs in prose right next to the fix, and several of those
    # comments now quote "overflow-wrap: anywhere" and "word-break:
    # break-all" verbatim while explaining why they were REMOVED from a
    # label rule. A naive scan of the raw text would flag the comment, not
    # the rule, and that false positive would make this check impossible to
    # keep green.
    $stripped = $cssText -replace '(?s)/\*.*?\*/', ' '

    foreach ($m in [regex]::Matches($stripped, $labelSelectorPattern)) {
        $rule = $m.Value
        if ($rule -match $badValuePattern) {
            $selector = ($rule -split '\{')[0].Trim()
            $failures += "$($f.Name): label-like selector `"$selector`" uses overflow-wrap:anywhere/word-break:break-all -- " +
                "this participates in min-content sizing and will collapse the label column toward one " +
                "character wide (the 'War/nin/g' bug). Use overflow-wrap: break-word instead, or move the " +
                "property to the paired .value/dd rule if it's genuinely the value column."
        }
    }
}

if ($scanned -lt 3) {
    throw "check_label_column_overflow_wrap.ps1: only scanned $scanned file(s) with a <style> block under " +
        "$driversRoot -- expected several *_page.html pages plus theme.css. This almost certainly means the " +
        "glob or the <style> extraction is broken, not that the tree shrank; investigate before trusting a " +
        "green result."
}

if ($failures.Count -gt 0) {
    Write-Host "LABEL COLUMN OVERFLOW-WRAP CHECK FAILED:" -ForegroundColor Red
    foreach ($fail in $failures) {
        Write-Host "  $fail" -ForegroundColor Red
    }
    throw "$($failures.Count) label-like selector(s) use overflow-wrap:anywhere/word-break:break-all -- see this script's header comment."
}

Write-Host "Label column overflow-wrap check passed: scanned $scanned file(s) under $driversRoot, no label-like selector uses overflow-wrap:anywhere or word-break:break-all."
exit 0
