# check_html_escape_helpers.ps1 -- narrow mechanical check for the drift
# class documented in docs/audits/web_code_duplication_drift_2026-09-18.md
# section 2.4: several of this project's HTML pages define their own local
# "escape HTML for innerHTML string-building" helper instead of using the
# one shared implementation, app.js's window.kcEscapeHtml. Each such local
# copy is a chance to silently drift from the canonical five-character set
# (& < > " ') -- exactly what happened to safety_config_page.html's local
# esc(), which was missing the single-quote replacement.
#
# This check does NOT ban local copies outright (aliasing to
# window.kcEscapeHtml, as safety_config_page.html now does, is preferred,
# but a page not loading app.js legitimately needs its own copy). What it
# enforces is narrower and mechanical: wherever a JS function whose body
# does a chain of String(...).replace(...) calls against HTML metacharacters
# exists (i.e. looks like an escape-for-HTML helper), every one of the five
# characters & < > " ' must be present. A helper missing one of the five is
# the exact silent-drift bug class this check exists to catch.
#
# Scope: firmware/KilnFW/App/drivers/http/**/*.html plus app.js itself
# (so app.js's own canonical kcEscapeHtml is checked too, and can never
# silently lose a character without turning this check red).
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\check_html_escape_helpers.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$httpDir = Join-Path $root "firmware\KilnFW\App\drivers\http"

if (-not (Test-Path -LiteralPath $httpDir)) {
    throw "check_html_escape_helpers.ps1: expected directory not found: $httpDir"
}

# Scan only git-tracked files under this tree, plus app.js explicitly (it
# lives directly in $httpDir so the recursive .html glob below already
# covers pages, but app.js is *.js so it needs its own pass).
Push-Location $root
try {
    $trackedHtml = git ls-files -- 'firmware/KilnFW/App/drivers/http/**/*.html' 'firmware/KilnFW/App/drivers/http/*.html'
    $trackedJs = git ls-files -- 'firmware/KilnFW/App/drivers/http/app.js'
} finally {
    Pop-Location
}

$targets = @($trackedHtml) + @($trackedJs) | Where-Object { $_ } | Select-Object -Unique

# One JS function definition, its whole body, up to the closing brace of the
# function (non-greedy, single function at a time). This is intentionally
# simple text matching, not a JS parser -- narrow and mechanical per the
# design note above, matching this repo's other check_*.ps1 scripts.
# The closing brace is matched as `\n\s*\}` rather than `\n\}` so a helper
# nested inside an indented block (e.g. an IIFE) is not silently skipped --
# a bare `\n\}` only matches a closing brace sitting at column 0.
$funcPattern = [regex]'(?ms)function\s+\w*[Ee]sc\w*\s*\([^)]*\)\s*\{.*?\n\s*\}|(?:window\.)?\w*[Ee]scapeHtml\s*=\s*function\s*\([^)]*\)\s*\{.*?\n\s*\};?'

$requiredChars = @('&', '<', '>', '"', "'")

# Every .replace(<regex-literal>, ...) call in the body, in either form:
#   .replace(/&/g, ...)               -- one character per call
#   .replace(/[&<>"']/g, function...) -- one character class for all five
# The characters that matter (& < > " ') are not special inside a JS regex
# literal or a character class (none needs backslash-escaping), so a plain
# substring check against the extracted regex source text is sufficient and
# avoids having to parse JS regex syntax.
$replaceRegexPattern = [regex]'\.replace\(\s*(/(?:\\.|[^/\\\n])*/)'

$violations = @()
$checkedCount = 0

foreach ($rel in $targets) {
    $full = Join-Path $root ($rel -replace '/', '\')
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { continue }

    $text = Get-Content -LiteralPath $full -Raw -ErrorAction Stop
    $funcMatches = $funcPattern.Matches($text)

    foreach ($m in $funcMatches) {
        $body = $m.Value
        # Only treat this as an HTML-escape helper if it actually chains
        # .replace(...) calls against a regex literal -- otherwise this
        # would also match unrelated "esc"-named functions (e.g. an
        # "escalate" or "descend" helper) that have nothing to do with
        # HTML escaping.
        $replaceRegexMatches = $replaceRegexPattern.Matches($body)
        if ($replaceRegexMatches.Count -eq 0) { continue }

        # Concatenate the source text of every regex literal passed to
        # .replace(...) in this function body -- covers both the
        # one-character-per-call style and the single-character-class style.
        $regexSource = [string]::Join('', ($replaceRegexMatches | ForEach-Object { $_.Groups[1].Value }))

        $checkedCount++
        $missing = @()
        foreach ($rc in $requiredChars) {
            if ($regexSource.IndexOf($rc) -lt 0) {
                $missing += $rc
            }
        }
        if ($missing.Count -gt 0) {
            # First line of the match, for a useful pointer into the file.
            $firstLine = ($body -split "`n")[0].Trim()
            $violations += [PSCustomObject]@{
                File    = $rel
                Snippet = $firstLine
                Missing = ($missing -join ' ')
            }
        }
    }
}

# Pinned floor, not just "> 0": as of this check's introduction there are
# at least three known helpers (app.js's window.kcEscapeHtml, zones_page.html's
# kgEsc, live_profile_page.html's escapeHtml). If the count ever drops below
# this floor, either a helper was deleted/renamed or the pattern silently
# stopped matching one -- both are the same "pattern erosion" failure this
# check exists to catch, so treat it as a failure, not a pass with fewer
# helpers found. When a genuinely new helper is legitimately added, raise
# this floor in the same change.
$minExpectedHelpers = 3

if ($checkedCount -eq 0) {
    throw "check_html_escape_helpers.ps1: found zero HTML-escape helper definitions under $httpDir -- the pattern likely broke, or the files moved. Refusing to pass vacuously."
}

if ($checkedCount -lt $minExpectedHelpers) {
    throw "check_html_escape_helpers.ps1: found only $checkedCount HTML-escape helper(s), expected at least $minExpectedHelpers (app.js kcEscapeHtml, zones_page.html kgEsc, live_profile_page.html escapeHtml). Either a helper was removed/renamed or the pattern stopped matching one -- if a new helper was legitimately added, raise `$minExpectedHelpers` in this script in the same change."
}

if ($violations.Count -gt 0) {
    Write-Host "HTML ESCAPE HELPER CHECK FAILED:" -ForegroundColor Red
    foreach ($v in $violations) {
        Write-Host "  $($v.File): '$($v.Snippet)' is missing replacement(s) for: $($v.Missing)" -ForegroundColor Red
    }
    throw "$($violations.Count) HTML-escape helper(s) missing one or more of the five required characters (& < > `" ') -- see docs/audits/web_code_duplication_drift_2026-09-18.md section 2.4"
}

Write-Host "HTML escape helper check passed: $checkedCount helper(s) found, all replace all five characters (& < > `" ')."
exit 0
