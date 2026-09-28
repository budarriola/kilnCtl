# check_stop_path_requires_pin.ps1 -- mechanically enforces the LCD half of
# the 2026-09-28 owner decision reversing docs/WEB_AUTH_PLAN.md section 9's
# old "Stop is never gated" rule: the owner's own words were "stop needs
# login. there is an estop button" -- the hardware E-stop
# (docs/SAFETY_CASE.md H7, firmware-mediated on this bench) is the
# independent safety backstop, not this keypad, so the LCD Stop control may
# now require the same PIN as Start.
#
# This check used to be check_stop_path_never_gated.ps1 and asserted the
# OPPOSITE rule (Stop branch must NOT reference a PIN gate). It has been
# renamed and inverted to match the reversal, not just edited in place, so a
# stale reference to the old filename or the old rule text is easy to spot
# by name alone.
#
# Scope note: this check covers only the LCD side (ui_home_fire_btn_cb() in
# ui_page_home_actions.c). The HTTP side (`POST /api/profile_exec/stop`
# bypassing authentication unconditionally) is a SEPARATE, unchanged
# concern -- this 2026-09-28 owner decision is explicitly scoped to the LCD
# only, so the HTTP-side bypass at the enforcement point is left exactly as
# it was and is not asserted on here.
#
# WHAT THIS CHECKS: inside ui_home_fire_btn_cb(), BOTH branches must
# reference a PIN gate (ui_lcd_lock_run_gated() / LCD_PIN_ROLE_*) --
# the RUNNING/PAUSED branch (the button reads "Stop") equally with the
# Idle/Done/Faulted branch (the button reads "Start"). The Stop branch must
# still ultimately reach ui_home_show_stop_confirm() (directly, or via a
# gated callback wrapper such as ui_home_show_stop_confirm_gated_cb) so the
# existing confirm dialog is never bypassed.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\check_stop_path_requires_pin.ps1 [-SourceFile <path>]
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
        throw "check_stop_path_requires_pin.ps1: $SourceFile does not exist."
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
        throw "check_stop_path_requires_pin.ps1: could not find function '$FunctionName' in $SourceFile -- has it been renamed or moved? Update this script."
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
        throw "check_stop_path_requires_pin.ps1: unbalanced braces scanning '$FunctionName' in $SourceFile -- cannot trust the parse."
    }
    $body = $text.Substring($bodyStart, $i - $bodyStart - 1)

    # Split into the two top-level branches on the first top-level "} else {"
    # inside this body. The shape is a single if/else: the "if"
    # (RUNNING/PAUSED -> Stop) and the "else" (everything else -> Start).
    $elseMatch = [regex]::Match($body, '\}\s*else\s*\{')
    if (-not $elseMatch.Success) {
        throw "check_stop_path_requires_pin.ps1: '$FunctionName' in $SourceFile has no if/else split -- has its shape changed from the merged Start/Stop button design? Update this script."
    }
    $stopBranch = $body.Substring(0, $elseMatch.Index)
    $startBranch = $body.Substring($elseMatch.Index + $elseMatch.Length)

    $gatePattern = 'ui_lcd_lock_run_gated|LCD_PIN_ROLE_[A-Za-z0-9_]+'
    # Accepts either a direct call, ui_home_show_stop_confirm(...), or a
    # reference to a gated-callback wrapper passed by name with no
    # trailing parens, e.g. ui_home_show_stop_confirm_gated_cb.
    $stopCallPattern = 'ui_home_show_stop_confirm(_gated_cb)?\b'

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

    Write-Host "Stop-path-requires-pin check: scanned ui_home_fire_btn_cb() in $sourceFile."

    $failures = @()
    if (-not $result.StopBranchCallsStop) {
        $failures += "the RUNNING/PAUSED branch (Stop) does not reach ui_home_show_stop_confirm() (directly or via a gated callback) -- has the widget's shape changed?"
    }
    if (-not $result.StopBranchGated) {
        $failures += "the RUNNING/PAUSED branch (Stop) does not reference a PIN gate (ui_lcd_lock_run_gated / LCD_PIN_ROLE_*) -- owner decision 2026-09-28 requires Stop to be gated the same as Start ('stop needs login. there is an estop button')."
    }
    if (-not $result.StartBranchGated) {
        $failures += "the else branch (Start) does not reference a PIN gate at all -- this check cannot distinguish a correctly-gated Stop from a file where gating was removed everywhere, so this counts as a failure to keep the check honest."
    }

    if ($failures.Count -gt 0) {
        Write-Host "STOP PATH REQUIRES PIN CHECK FAILED:" -ForegroundColor Red
        foreach ($f in $failures) {
            Write-Host "  - $f" -ForegroundColor Red
        }
        throw "$($failures.Count) failure(s) above. Owner decision 2026-09-28: LCD Stop must require the PIN, the same as Start."
    }

    Write-Host "Stop path requires PIN check passed: Stop branch is gated and still reaches ui_home_show_stop_confirm(); Start branch is still gated."
    exit 0
}
