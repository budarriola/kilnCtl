# check_no_native_dialogs_in_ui.ps1 -- guards against a native browser
# window.confirm()/confirm()/alert()/prompt() dialog reappearing in the served
# web UI. Owner rule: every confirmation popup must be an in-page, cancellable,
# themed modal (the .kc-login-* pattern, generalized into app.js's shared
# kcConfirm()/kcAlert() -- see theme.css's .kc-confirm-panel and app.js's
# openConfirmModal()), never the browser's own blocking, unthemed, untestable
# native dialog. Commit 745f7f69 first proved the pattern
# (#forceProtoModal/confirmForceProtocol() in ota_page.html); this check
# followed once every remaining native-dialog call site across the served
# pages was converted to route through kcConfirm()/kcAlert().
#
# Scans every .html/.js file under firmware/KilnFW/App/drivers/http/ and
# firmware/KilnFW/App/drivers/net/ -- the full served-page surface, not just a
# named subset, so a newly added page is covered automatically.
#
# Mechanics: strip HTML comments (<!-- -->) then C-style comments (// and
# /* */) -- same two-pass Get-HtmlCommentStripped/Get-CCommentStripped
# approach check_no_bench_text_in_ui.ps1 uses -- so a comment merely
# discussing "confirm()" or "alert()" (there are several) never trips this.
# Then scan for a bare, lowercase confirm(/alert(/prompt( call, `\b`-bounded.
# This is deliberately CASE-SENITIVE and requires the literal '(' immediately
# after the word: kcConfirm(/kcAlert() never match (capital C/A), and neither
# does a comment/identifier like confirmForceProtocol( (no '(' right after
# "confirm"). window.confirm(/window.alert(/bare confirm(/alert( all match,
# since `\b` treats '.' as a boundary just like whitespace.
#
# Exit: 0 pass, 1 fail (violations printed), 2 usage/setup error.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\check_no_native_dialogs_in_ui.ps1

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$httpDir = Join-Path $repoRoot "firmware\KilnFW\App\drivers\http"
$netDir = Join-Path $repoRoot "firmware\KilnFW\App\drivers\net"

if (-not (Test-Path $httpDir)) { throw "check_no_native_dialogs_in_ui: $httpDir not found -- has it moved?" }
if (-not (Test-Path $netDir)) { throw "check_no_native_dialogs_in_ui: $netDir not found -- has it moved?" }

$ForbiddenRegex = '\bconfirm\(|\balert\(|\bprompt\('

# --- C-style comment stripper (// and /* */), reproduced from
# check_no_bench_text_in_ui.ps1 (itself reproduced from
# check_hal_include_boundary.ps1's Get-CodeOnlyLines). ---
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
# stripper for .html files, same as check_no_bench_text_in_ui.ps1. ---
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

$files = @(Get-ChildItem -Path $httpDir -File | Where-Object { $_.Extension -in @(".html", ".js") })
$files += @(Get-ChildItem -Path $netDir -File | Where-Object { $_.Extension -in @(".html", ".js") })
if ($files.Count -eq 0) {
    throw "check_no_native_dialogs_in_ui: found 0 .html/.js files under $httpDir or $netDir -- scan target has gone blind."
}

foreach ($f in $files) {
    $rel = Get-RelPath -FullPath $f.FullName
    $raw = Get-Content -Path $f.FullName
    $stripped = $raw
    if ($f.Extension -eq ".html") { $stripped = Get-HtmlCommentStripped -Lines $stripped }
    $stripped = Get-CCommentStripped -Lines $stripped
    for ($i = 0; $i -lt $stripped.Count; $i++) {
        if ($stripped[$i] -cmatch $ForbiddenRegex) {
            $violations += "${rel}:$($i+1): $($stripped[$i].Trim())"
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Host "NATIVE confirm()/alert()/prompt() DIALOG FOUND IN SERVED UI:" -ForegroundColor Red
    foreach ($v in $violations) { Write-Host "  $v" -ForegroundColor Red }
    throw "$($violations.Count) native-dialog hit(s) in served web UI -- every confirmation/notice must go through app.js's themed window.kcConfirm()/window.kcAlert() instead (see tools\check_no_native_dialogs_in_ui.ps1 header)."
}

Write-Host "check_no_native_dialogs_in_ui: PASS ($($files.Count) served page(s) scanned, 0 violations)"
exit 0
