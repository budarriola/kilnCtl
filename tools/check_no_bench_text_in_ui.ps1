# check_no_bench_text_in_ui.ps1 -- guards against developer-bench/test-fixture
# language leaking into anything an END USER of shipped firmware actually
# sees: the served web pages (firmware/KilnFW/App/drivers/http/*.html,*.js),
# the LCD UI strings (firmware/KilnFW/App/drivers/ui/*.c,*.h), and the JSON
# message/reason/hint/detail strings HTTP handlers send back to the browser
# (firmware/KilnFW/App/drivers/http/*_http.c).
#
# Owner request 2026-09-19, verbatim: "look at the web interface for things
# like this that are hard coded in reffrenceing the test fixture. this should
# not be in code we are intending to release" -- quoting zones_page.html's
# heater-load DORMANT explainer, which used to read "...On this ~4 W bench
# fixture every zone sits under the sweep's noise floor...". Release firmware
# must not describe the developer's own bench (its ~4 W load, specific relay/
# GPIO wiring, the bench webcam, cplval/coupling-capture logs) to an operator
# who has no idea what any of that means for their kiln.
#
# Developer-only material is explicitly OUT OF SCOPE and not scanned here:
# code comments (// and /* */ and <!-- -->), docs/*.md, check/test scripts,
# and MCP tool source. Those are for the people maintaining this repo, not
# for someone using the product, and mentioning the bench there is normal and
# expected. This check strips comments before scanning for exactly that
# reason -- it must not fail on an honest comment explaining bench-only
# behavior to the next developer.
#
# Mechanics:
#   1. Comment stripping (HTML <!-- -->, then C-style // and /* */, same
#      two-pass approach as check_hal_include_boundary.ps1's
#      Get-CodeOnlyLines, extended for HTML) over every served page in
#      firmware/KilnFW/App/drivers/http/*.html and *.js.
#   2. Same C-style comment stripping over every firmware/KilnFW/App/drivers/
#      ui/*.c and *.h file (the LCD's own strings).
#   3. A narrower pass over firmware/KilnFW/App/drivers/http/*_http.c: any
#      line that is (a) not a comment and (b) looks like it hands a string to
#      the HTTP client -- httpd_resp_send_err(...), httpd_resp_sendstr(...),
#      or an snprintf(...) into a buffer literally named detail/msg/hint/
#      reason/message -- is scanned for the forbidden list. Server-side
#      ESP_LOGI/ESP_LOGW/ESP_LOGE calls are deliberately NOT scanned here:
#      those are developer log lines nobody using the product ever sees.
#
# The forbidden word list ($ForbiddenPatterns below) is maintained by hand,
# same shape as check_hal_include_boundary.ps1's allowlists: add a new
# pattern there when a new class of bench-only language is found; add a new
# $Allowlist entry (Path regex + Content regex + Reason) only for a
# confirmed, reviewed, legitimate use that happens to contain a forbidden
# word (e.g. an internal API route name kept unchanged deliberately) --
# never to silence a real leak.
#
# Exit: 0 pass, 1 fail (violations printed), 2 usage/setup error.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\check_no_bench_text_in_ui.ps1

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$httpDir = Join-Path $repoRoot "firmware\KilnFW\App\drivers\http"
$uiDir = Join-Path $repoRoot "firmware\KilnFW\App\drivers\ui"

if (-not (Test-Path $httpDir)) { throw "check_no_bench_text_in_ui: $httpDir not found -- has it moved?" }
if (-not (Test-Path $uiDir)) { throw "check_no_bench_text_in_ui: $uiDir not found -- has it moved?" }

# --- Forbidden word list. Word-boundary regex, case-insensitive. Kept narrow
# and specific on purpose -- broad words like "test" or "board" alone would
# flag huge amounts of legitimate product language (self-test features,
# "this board" describing the real product hardware). ---
$ForbiddenPatterns = @(
    '\bbench\b'
    '\bbenches\b'
    '\bfixture\b'
    '\bfixtures\b'
    '\bcplval\w*'
    'coupling captur\w+'
    '\bwebcam\b'
    'noise floor'
    '~?4\s?[wW]\b'
    '\btest rig\b'
    '\bdev board\b'
    '\bbudarriola\b'
)
$ForbiddenRegex = ($ForbiddenPatterns -join '|')

# --- Reviewed exceptions. Each entry: PathPattern (regex against the
# repo-relative path), ContentPattern (regex against the matched line, after
# comment stripping) and Reason. A line is exempted only if BOTH patterns
# match. Keep this list short -- most bench-language findings should be
# fixed, not allowlisted. ---
$Allowlist = @(
    @{ PathPattern = 'safety_commissioning_page\.html$'; ContentPattern = 'api/safety/commissioning/bench_preset'
       Reason = "internal API route name (not user-visible text); the visible button/dialog text was reworded to 'test preset' 2026-09-19, but the wire route itself is unchanged behavior, out of scope for a string-only sweep" }
    @{ PathPattern = 'safety_cfg_http\.c$'; ContentPattern = '"/api/safety/commissioning/bench_preset"'
       Reason = "same internal API route name, server side" }
)

function Test-Allowlisted {
    param([string]$RelPath, [string]$Line)
    foreach ($a in $Allowlist) {
        if ($RelPath -match $a.PathPattern -and $Line -match $a.ContentPattern) { return $true }
    }
    return $false
}

# --- C-style comment stripper (// and /* */), reproduced from
# check_hal_include_boundary.ps1's Get-CodeOnlyLines. ---
function Get-CCommentStripped {
    param([string[]]$Lines)
    $inBlockComment = $false
    $result = @()
    foreach ($line in $Lines) {
        $code = $line
        if ($inBlockComment) {
            $endIdx = $code.IndexOf("*/")
            if ($endIdx -ge 0) {
                $code = $code.Substring($endIdx + 2)
                $inBlockComment = $false
            } else {
                $result += ""
                continue
            }
        }
        while ($true) {
            $startIdx = $code.IndexOf("/*")
            if ($startIdx -lt 0) { break }
            $endIdx = $code.IndexOf("*/", $startIdx)
            if ($endIdx -ge 0) {
                $code = $code.Substring(0, $startIdx) + $code.Substring($endIdx + 2)
            } else {
                $code = $code.Substring(0, $startIdx)
                $inBlockComment = $true
                break
            }
        }
        $lineCommentIdx = $code.IndexOf("//")
        if ($lineCommentIdx -ge 0) {
            $code = $code.Substring(0, $lineCommentIdx)
        }
        $result += $code
    }
    return $result
}

# --- HTML comment stripper (<!-- ... -->), applied BEFORE the C-style
# stripper for .html files so a <!-- ... // ... --> block doesn't confuse the
# // handling, and so plain HTML text (outside any <script>) is covered too.
# ---
function Get-HtmlCommentStripped {
    param([string[]]$Lines)
    $inComment = $false
    $result = @()
    foreach ($line in $Lines) {
        $code = $line
        $out = ""
        while ($true) {
            if ($inComment) {
                $endIdx = $code.IndexOf("-->")
                if ($endIdx -ge 0) {
                    $code = $code.Substring($endIdx + 3)
                    $inComment = $false
                } else {
                    $code = ""
                    break
                }
            }
            $startIdx = $code.IndexOf("<!--")
            if ($startIdx -lt 0) {
                $out += $code
                break
            }
            $out += $code.Substring(0, $startIdx)
            $code = $code.Substring($startIdx + 4)
            $inComment = $true
        }
        $result += $out
    }
    return $result
}

function Get-RelPath {
    param([string]$FullPath)
    return ($FullPath.Substring($repoRoot.Length + 1) -replace '\\', '/')
}

$violations = @()

# --- Pass 1: served pages (.html, .js) under drivers/http/ ---
$pages = Get-ChildItem -Path $httpDir -File | Where-Object { $_.Extension -in @(".html", ".js") }
if ($pages.Count -eq 0) {
    throw "check_no_bench_text_in_ui: found 0 .html/.js files under $httpDir -- expected several served pages. Scan target has gone blind."
}
foreach ($f in $pages) {
    $rel = Get-RelPath -FullPath $f.FullName
    $raw = Get-Content -Path $f.FullName
    $stripped = $raw
    if ($f.Extension -eq ".html") { $stripped = Get-HtmlCommentStripped -Lines $stripped }
    $stripped = Get-CCommentStripped -Lines $stripped
    for ($i = 0; $i -lt $stripped.Count; $i++) {
        if ($stripped[$i] -match $ForbiddenRegex) {
            if (Test-Allowlisted -RelPath $rel -Line $stripped[$i]) { continue }
            $violations += "${rel}:$($i+1): $($stripped[$i].Trim())"
        }
    }
}

# --- Pass 2: LCD UI strings under drivers/ui/ ---
$uiFiles = Get-ChildItem -Path $uiDir -File | Where-Object { $_.Extension -in @(".c", ".h") }
if ($uiFiles.Count -eq 0) {
    throw "check_no_bench_text_in_ui: found 0 .c/.h files under $uiDir -- expected the LCD driver sources. Scan target has gone blind."
}
foreach ($f in $uiFiles) {
    $rel = Get-RelPath -FullPath $f.FullName
    $raw = Get-Content -Path $f.FullName
    $stripped = Get-CCommentStripped -Lines $raw
    for ($i = 0; $i -lt $stripped.Count; $i++) {
        # Only lines that carry a string literal -- an identifier or
        # variable name containing one of the forbidden words with no quote
        # on the line is not a user-visible string.
        if ($stripped[$i] -notmatch '"') { continue }
        if ($stripped[$i] -match $ForbiddenRegex) {
            if (Test-Allowlisted -RelPath $rel -Line $stripped[$i]) { continue }
            $violations += "${rel}:$($i+1): $($stripped[$i].Trim())"
        }
    }
}

# --- Pass 3: JSON msg/reason/hint/detail strings HTTP handlers send back,
# under drivers/http/*_http.c. Restricted to lines that look like they hand a
# string to the client (httpd_resp_send_err/httpd_resp_sendstr, or an
# snprintf into a buffer named detail/msg/hint/reason/message) so ordinary
# ESP_LOG* developer log lines are not in scope. ---
$httpCFiles = Get-ChildItem -Path $httpDir -File -Filter "*_http.c"
if ($httpCFiles.Count -eq 0) {
    throw "check_no_bench_text_in_ui: found 0 *_http.c files under $httpDir -- expected several HTTP handler files. Scan target has gone blind."
}
$clientStringPattern = 'httpd_resp_send_err\s*\(|httpd_resp_sendstr\s*\(|snprintf\s*\(\s*(detail|msg|hint|reason|message)\b'
foreach ($f in $httpCFiles) {
    $rel = Get-RelPath -FullPath $f.FullName
    $raw = Get-Content -Path $f.FullName
    $stripped = Get-CCommentStripped -Lines $raw
    for ($i = 0; $i -lt $stripped.Count; $i++) {
        if ($stripped[$i] -notmatch $clientStringPattern) { continue }
        if ($stripped[$i] -match $ForbiddenRegex) {
            if (Test-Allowlisted -RelPath $rel -Line $stripped[$i]) { continue }
            $violations += "${rel}:$($i+1): $($stripped[$i].Trim())"
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host "BENCH/FIXTURE TEXT FOUND IN USER-FACING UI:" -ForegroundColor Red
    foreach ($v in $violations) { Write-Host "  $v" -ForegroundColor Red }
    throw "$($violations.Count) forbidden-word hit(s) in user-facing web/LCD/HTTP-message text -- reword to product language, or add a reviewed entry to `$Allowlist in tools\check_no_bench_text_in_ui.ps1 if this is a confirmed non-user-visible exception."
}

Write-Host "check_no_bench_text_in_ui: PASS ($($pages.Count) served page(s), $($uiFiles.Count) UI source file(s), $($httpCFiles.Count) *_http.c file(s) scanned, 0 violations)"
exit 0
