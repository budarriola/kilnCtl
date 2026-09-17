# check_stop_path_never_gated.ps1 -- mechanically enforces the LCD half of
# docs/WEB_AUTH_PLAN.md section 9: "the LCD Stop control ... must never
# route through the keypad. On the LCD the Start/Stop control is one merged
# widget that reads 'Stop' only while RUNNING or PAUSED -- so the gate is:
# that widget demands a PIN when it would Start, and never when it would
# Stop."
#
# Scope note: this check covers only the LCD side of section 9
# (ui_home_fire_btn_cb() in ui_page_home_actions.c), which is fully
# implemented and dependency-free. The HTTP side of the same section --
# `POST /api/profile_exec/stop` bypassing authentication unconditionally at
# the enforcement point -- is NOT covered here: as of this writing
# http_auth_check() (App/drivers/http/http_auth_enforce.c) takes no uri at
# all, only a route_tier_t, and neither it nor http_auth_http.c's
# kiln_http_prehandler() special-cases /api/profile_exec/stop anywhere.
# That means a session-less client currently gets DENY_NO_SESSION calling
# stop while web auth is on -- section 9's own acceptance criterion ("a host
# test drives the enforcement function with every combination ... against
# /api/profile_exec/stop and asserts ALLOW in all five") does not hold on
# this tree today. That gap lives in the enforcement point (plan item 5),
# which is owned by another in-flight change, not by this check -- see
# docs/WEB_AUTH_PLAN.md section 9's own note above this script's reference,
# and report it rather than adding a check here that is red by construction
# until that lands.
#
# WHAT THIS CHECKS: inside ui_home_fire_btn_cb(), the branch taken when the
# firing is RUNNING or PAUSED (i.e. the button reads "Stop") must call
# ui_home_show_stop_confirm() directly, with no PIN-gating call
# (ui_lcd_lock_run_gated(), or any LCD_PIN_ROLE_* reference) anywhere in that
# branch. The opposite branch (Idle/Done/Faulted, i.e. "Start") is asserted
# to still be gated -- proving this check can tell the two branches apart
# rather than passing on a file with no gating anywhere.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\check_stop_path_never_gated.ps1 [-SourceFile <path>]
param(
    [string]$SourceFile
)
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
if ($SourceFile) {
    $sourceFile = (Resolve-Path $SourceFile).Path
} else {
    $sourceFile = Join-Path $root "..\firmware\KilnFW\App\drivers\ui\ui_page_home_actions.c"
    $sourceFile = (Resolve-Path $sourceFile).Path
}

# Same comment-stripping helper as check_kiln_auth_config_isolation.ps1 /
# check_route_tier_coverage.ps1 (duplicated rather than imported -- this
# project has no shared PowerShell module mechanism).
function Get-CodeOnlyLines {
    param([string]$Path)
    $inBlockComment = $false
    $lines = Get-Content -Path $Path
    $result = @()
    foreach ($line in $lines) {
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
        $lineCommentIdx = $code.IndexOf("//")
        if ($lineCommentIdx -ge 0) {
            $code = $code.Substring(0, $lineCommentIdx)
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
        $result += $code
    }
    return $result
}

# Production entry point, called directly by the negative test (dot-sourced)
# against synthetic files -- do not reimplement this logic anywhere else.
function Invoke-StopPathGateScan {
    param(
        [Parameter(Mandatory)][string]$SourceFile,
        [string]$FunctionName = "ui_home_fire_btn_cb"
    )
    if (-not (Test-Path $SourceFile)) {
        throw "check_stop_path_never_gated.ps1: $SourceFile does not exist."
    }
    $codeLines = Get-CodeOnlyLines -Path $SourceFile
    $text = [string]::Join("`n", $codeLines)

    # Find the function body: from its opening brace to the matching closing
    # brace, by brace-depth counting starting at the first '{' after the
    # function name (handles nested if/else braces correctly, unlike a
    # naive "next standalone '}' at column 0" heuristic).
    $sigPattern = [regex]::Escape($FunctionName) + '\s*\([^)]*\)\s*\{'
    $sigMatch = [regex]::Match($text, $sigPattern)
    if (-not $sigMatch.Success) {
        throw "check_stop_path_never_gated.ps1: could not find function '$FunctionName' in $SourceFile -- has it been renamed or moved? Update this script."
    }
    $bodyStart = $sigMatch.Index + $sigMatch.Length
    $depth = 1
    $i = $bodyStart
    while ($depth -gt 0 -and $i -lt $text.Length) {
        $c = $text[$i]
        if ($c -eq '{') { $depth++ }
        elseif ($c -eq '}') { $depth-- }
        $i++
    }
    if ($depth -ne 0) {
        throw "check_stop_path_never_gated.ps1: unbalanced braces scanning '$FunctionName' in $SourceFile -- cannot trust the parse."
    }
    $body = $text.Substring($bodyStart, $i - $bodyStart - 1)

    # Split into the two top-level branches on the first top-level "} else {"
    # inside this body. Section 9's shape is a single if/else: the "if"
    # (RUNNING/PAUSED -> Stop) and the "else" (everything else -> Start).
    $elseMatch = [regex]::Match($body, '\}\s*else\s*\{')
    if (-not $elseMatch.Success) {
        throw "check_stop_path_never_gated.ps1: '$FunctionName' in $SourceFile has no if/else split -- has its shape changed from the merged Start/Stop button section 9 describes? Update this script."
    }
    $stopBranch = $body.Substring(0, $elseMatch.Index)
    $startBranch = $body.Substring($elseMatch.Index + $elseMatch.Length)

    $gatePattern = 'ui_lcd_lock_run_gated|LCD_PIN_ROLE_[A-Za-z0-9_]+'
    $stopCallPattern = 'ui_home_show_stop_confirm\s*\('

    $stopBranchGated = [regex]::IsMatch($stopBranch, $gatePattern)
    $stopBranchCallsStop = [regex]::IsMatch($stopBranch, $stopCallPattern)
    $startBranchGated = [regex]::IsMatch($startBranch, $gatePattern)

    return [PSCustomObject]@{
        StopBranchGated      = $stopBranchGated
        StopBranchCallsStop  = $stopBranchCallsStop
        StartBranchGated     = $startBranchGated
        StopBranchText       = $stopBranch
        StartBranchText      = $startBranch
    }
}

# Only run when invoked directly (not dot-sourced by the negative test).
if ($MyInvocation.InvocationName -ne '.') {
    $result = Invoke-StopPathGateScan -SourceFile $sourceFile

    Write-Host "Stop-path-never-gated check: scanned ui_home_fire_btn_cb() in $sourceFile."

    $failures = @()
    if (-not $result.StopBranchCallsStop) {
        $failures += "the RUNNING/PAUSED branch (Stop) does not call ui_home_show_stop_confirm() at all -- has the widget's shape changed?"
    }
    if ($result.StopBranchGated) {
        $failures += "the RUNNING/PAUSED branch (Stop) references a PIN gate (ui_lcd_lock_run_gated / LCD_PIN_ROLE_*) -- docs/WEB_AUTH_PLAN.md section 9 requires Stop to NEVER be gated."
    }
    if (-not $result.StartBranchGated) {
        $failures += "the else branch (Start) does not reference a PIN gate at all -- this check cannot distinguish a correctly-ungated Stop from a file where gating was removed everywhere, so this counts as a failure to keep the check honest."
    }

    if ($failures.Count -gt 0) {
        Write-Host "STOP PATH NEVER GATED CHECK FAILED:" -ForegroundColor Red
        foreach ($f in $failures) {
            Write-Host "  - $f" -ForegroundColor Red
        }
        throw "$($failures.Count) failure(s) above. A PIN surface must never be able to prevent a running firing from being stopped (docs/WEB_AUTH_PLAN.md section 9)."
    }

    Write-Host "Stop path never gated check passed: Stop branch is ungated and calls ui_home_show_stop_confirm(); Start branch is still gated."
    exit 0
}
