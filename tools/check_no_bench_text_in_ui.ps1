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
$netDir = Join-Path $repoRoot "firmware\KilnFW\App\drivers\net"

if (-not (Test-Path $httpDir)) { throw "check_no_bench_text_in_ui: $httpDir not found -- has it moved?" }
if (-not (Test-Path $uiDir)) { throw "check_no_bench_text_in_ui: $uiDir not found -- has it moved?" }
if (-not (Test-Path $netDir)) { throw "check_no_bench_text_in_ui: $netDir not found -- has it moved?" }

# Specific net/ files in scope: the served pages/handlers that reach an end
# user, not the whole net/ directory (which also holds low-level link/state
# code with no user-visible strings).
$NetHtmlNames = @("ota_page.html", "security_page.html", "wifi_provision_page.html")
$NetHttpCNames = @("wifi_prov_api.c", "web_auth_login.c", "ota_auth.c", "ota_pico_relay.c")

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

# --- Jargon class: internal repo/process jargon (doc filenames, "owner
# decision", "worktree", bare source filenames, etc.) that leaks
# development-context language into user-facing text without necessarily
# containing a bench/fixture word. Checked as a second, separately-named
# pattern list so a hit reads as its own class in the failure message. ---
$JargonPatterns = @(
    '\b[A-Z][A-Z0-9_]{2,}\.md\b'
    '\bdocs/'
    '\bCLAUDE\.md\b'
    '\bproject_[a-z0-9_]{8,}\b'
    'owner decision|owner request|quoted verbatim|per owner'
    '\bworktree\b'
)
$JargonRegex = ($JargonPatterns -join '|')
# Bare source-filename mentions (e.g. "readiness_http.c"), applied only to
# HTML text nodes / quoted JS strings. The negative lookbehind keeps this
# off property-access chains like "p.c" in chart code (main_page.html).
$SourceFilePattern = '(?<![.\w])[A-Za-z_][A-Za-z0-9_]{2,}\.(c|h|py|ps1|cmake)\b'

# Whole-file, temporary allowlist for the jargon-class pass (Pass 4) only.
# setup_wizard_page.html is mid-rewrite by another agent as of 2026-09-19;
# excluding it here avoids a merge collision with that in-flight work. This
# entry should be removed once that rewrite lands and the file is clean.
$JargonFileAllowlist = @(
    'setup_wizard_page\.html$'
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

# --- Pass 1: served pages (.html, .js) under drivers/http/ and the
# specific net/ pages named above. ---
$pages = @(Get-ChildItem -Path $httpDir -File | Where-Object { $_.Extension -in @(".html", ".js") })
if ($pages.Count -eq 0) {
    throw "check_no_bench_text_in_ui: found 0 .html/.js files under $httpDir -- expected several served pages. Scan target has gone blind."
}
$netPages = @(Get-ChildItem -Path $netDir -File | Where-Object { $NetHtmlNames -contains $_.Name })
if ($netPages.Count -eq 0) {
    throw "check_no_bench_text_in_ui: found 0 of the expected net/ pages ($($NetHtmlNames -join ', ')) under $netDir -- scan target has gone blind."
}
$pages = @($pages) + @($netPages)
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

# --- Pass 3: string-literal lines in drivers/http/*_http.c and the named
# net/ handler files. Scans every line that carries a string literal
# (contains a quote), NOT just lines matching a specific call shape --
# multi-line C string continuations (the normal shape for a long
# snprintf/detail message split across several literals) carry no call
# anchor on their own line and were previously invisible here. ESP_LOG*
# calls (developer log lines nobody using the product ever sees) are
# excluded, including their continuation lines when the call itself spans
# multiple lines, tracked via a running paren-depth count.
function Get-NonEspLogQuotedLineIndices {
    param([string[]]$Lines)
    $indices = @()
    $espLogDepth = 0
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        $line = $Lines[$i]
        if ($espLogDepth -gt 0) {
            $opens = ([regex]::Matches($line, '\(')).Count
            $closes = ([regex]::Matches($line, '\)')).Count
            $espLogDepth += ($opens - $closes)
            if ($espLogDepth -lt 0) { $espLogDepth = 0 }
            continue
        }
        if ($line -match 'ESP_LOG[IWE]\s*\(') {
            $opens = ([regex]::Matches($line, '\(')).Count
            $closes = ([regex]::Matches($line, '\)')).Count
            $espLogDepth = $opens - $closes
            if ($espLogDepth -lt 0) { $espLogDepth = 0 }
            continue
        }
        if ($line -match '"') { $indices += $i }
    }
    return $indices
}

$httpCFiles = @(Get-ChildItem -Path $httpDir -File -Filter "*_http.c")
if ($httpCFiles.Count -eq 0) {
    throw "check_no_bench_text_in_ui: found 0 *_http.c files under $httpDir -- expected several HTTP handler files. Scan target has gone blind."
}
$netCFiles = @(Get-ChildItem -Path $netDir -File | Where-Object { $NetHttpCNames -contains $_.Name })
if ($netCFiles.Count -eq 0) {
    throw "check_no_bench_text_in_ui: found 0 of the expected net/ handler files ($($NetHttpCNames -join ', ')) under $netDir -- scan target has gone blind."
}
$httpCFiles = @($httpCFiles) + @($netCFiles)
foreach ($f in $httpCFiles) {
    $rel = Get-RelPath -FullPath $f.FullName
    $raw = Get-Content -Path $f.FullName
    $stripped = Get-CCommentStripped -Lines $raw
    $quotedIdx = Get-NonEspLogQuotedLineIndices -Lines $stripped
    foreach ($i in $quotedIdx) {
        if ($stripped[$i] -match $ForbiddenRegex) {
            if (Test-Allowlisted -RelPath $rel -Line $stripped[$i]) { continue }
            $violations += "${rel}:$($i+1): $($stripped[$i].Trim())"
        }
    }
}

# --- Pass 4: jargon class -- development-context/internal-process language
# (doc filenames, "owner decision", bare source filenames, etc.) over the
# same served pages as Pass 1, minus the temporary allowlist above. Reported
# separately from $violations so the failure message names this as its own
# class, distinct from the bench/fixture word list. ---
$jargonViolations = @()
foreach ($f in $pages) {
    $rel = Get-RelPath -FullPath $f.FullName
    $skip = $false
    foreach ($p in $JargonFileAllowlist) { if ($rel -match $p) { $skip = $true; break } }
    if ($skip) { continue }
    $raw = Get-Content -Path $f.FullName
    $stripped = $raw
    if ($f.Extension -eq ".html") { $stripped = Get-HtmlCommentStripped -Lines $stripped }
    $stripped = Get-CCommentStripped -Lines $stripped
    for ($i = 0; $i -lt $stripped.Count; $i++) {
        $line = $stripped[$i]
        $hit = $null
        if ($line -match $JargonRegex) {
            $hit = $Matches[0]
        } elseif ($line -match $SourceFilePattern) {
            $hit = $Matches[0]
        }
        if ($null -eq $hit) { continue }
        if (Test-Allowlisted -RelPath $rel -Line $line) { continue }
        $jargonViolations += "${rel}:$($i+1): $($line.Trim())"
    }
}

if ($violations.Count -gt 0) {
    Write-Host "BENCH/FIXTURE TEXT FOUND IN USER-FACING UI:" -ForegroundColor Red
    foreach ($v in $violations) { Write-Host "  $v" -ForegroundColor Red }
}
if ($jargonViolations.Count -gt 0) {
    Write-Host "DEVELOPMENT-CONTEXT / INTERNAL-JARGON TEXT FOUND IN USER-FACING UI:" -ForegroundColor Red
    foreach ($v in $jargonViolations) { Write-Host "  $v" -ForegroundColor Red }
}
if ($violations.Count -gt 0 -or $jargonViolations.Count -gt 0) {
    throw "$($violations.Count) forbidden-word hit(s) and $($jargonViolations.Count) jargon-class hit(s) in user-facing web/LCD/HTTP-message text -- reword to product language, or add a reviewed entry to `$Allowlist (bench/fixture words) or `$JargonFileAllowlist (jargon class) in tools\check_no_bench_text_in_ui.ps1 if this is a confirmed non-user-visible exception."
}

Write-Host "check_no_bench_text_in_ui: PASS ($($pages.Count) served page(s), $($uiFiles.Count) UI source file(s), $($httpCFiles.Count) *_http.c file(s) scanned, 0 violations)"
exit 0
