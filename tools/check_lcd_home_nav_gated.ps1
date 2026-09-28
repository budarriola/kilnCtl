# check_lcd_home_nav_gated.ps1 -- mechanically enforces the LCD half of the
# 2026-09-28 owner decision: on the LCD, a user who is not logged in may only
# VIEW the home/dashboard page. Every other page reached from the home page
# must demand the PIN first, EXCEPT the physical credential-reset gesture,
# which must remain reachable without a PIN by design (the gesture itself is
# the escape hatch for a lost/forgotten PIN, and gating it behind the PIN it
# exists to recover from would make it useless).
#
# WHAT THIS CHECKS, all in firmware/KilnFW/App/drivers/ui/ui_page_home_actions.c:
#   - ui_home_menu_nav_cb() (-> the Config hub) must reference a PIN gate
#     (ui_lcd_lock_run_gated() / LCD_PIN_ROLE_*).
#   - ui_home_profile_btn_cb() (-> the profile picker) must reference a PIN
#     gate.
#   - ui_home_auth_reset_corner_tap_cb() (the physical credential-reset
#     gesture) must NOT reference a PIN gate -- proving this check can tell
#     "should be gated" apart from "must stay reachable," not just grep for
#     the gate pattern everywhere and demand it appear.
#
# This does not check ui_home_fire_btn_cb() (Start/Stop) -- that is
# check_stop_path_requires_pin.ps1's job, since it needs to tell two
# branches of one function apart rather than three whole functions.
#
# Usage: powershell -ExecutionPolicy Bypass -File tools\check_lcd_home_nav_gated.ps1 [-SourceFile <path>]
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

# Same comment-stripping helper as check_stop_path_requires_pin.ps1 /
# check_kiln_auth_config_isolation.ps1 (duplicated rather than imported --
# this project has no shared PowerShell module mechanism).
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
function Get-FunctionBody {
    param(
        [Parameter(Mandatory)][string]$Text,
        [Parameter(Mandatory)][string]$FunctionName
    )
    $sigPattern = [regex]::Escape($FunctionName) + '\s*\([^)]*\)\s*\{'
    $sigMatch = [regex]::Match($Text, $sigPattern)
    if (-not $sigMatch.Success) {
        throw "check_lcd_home_nav_gated.ps1: could not find function '$FunctionName' -- has it been renamed or moved? Update this script."
    }
    $bodyStart = $sigMatch.Index + $sigMatch.Length
    $depth = 1
    $i = $bodyStart
    while ($depth -gt 0 -and $i -lt $Text.Length) {
        $c = $Text[$i]
        if ($c -eq '{') { $depth++ }
        elseif ($c -eq '}') { $depth-- }
        $i++
    }
    if ($depth -ne 0) {
        throw "check_lcd_home_nav_gated.ps1: unbalanced braces scanning '$FunctionName' -- cannot trust the parse."
    }
    return $Text.Substring($bodyStart, $i - $bodyStart - 1)
}

function Invoke-HomeNavGateScan {
    param(
        [Parameter(Mandatory)][string]$SourceFile
    )
    if (-not (Test-Path $SourceFile)) {
        throw "check_lcd_home_nav_gated.ps1: $SourceFile does not exist."
    }
    $codeLines = Get-CodeOnlyLines -Path $SourceFile
    $text = [string]::Join("`n", $codeLines)

    $gatePattern = 'ui_lcd_lock_run_gated|LCD_PIN_ROLE_[A-Za-z0-9_]+'

    $menuBody = Get-FunctionBody -Text $text -FunctionName "ui_home_menu_nav_cb"
    $profileBody = Get-FunctionBody -Text $text -FunctionName "ui_home_profile_btn_cb"
    $resetBody = Get-FunctionBody -Text $text -FunctionName "ui_home_auth_reset_corner_tap_cb"

    return [PSCustomObject]@{
        MenuNavGated    = [regex]::IsMatch($menuBody, $gatePattern)
        ProfileBtnGated = [regex]::IsMatch($profileBody, $gatePattern)
        AuthResetGated  = [regex]::IsMatch($resetBody, $gatePattern)
    }
}

# Only run when invoked directly (not dot-sourced by the negative test).
if ($MyInvocation.InvocationName -ne '.') {
    $result = Invoke-HomeNavGateScan -SourceFile $sourceFile

    Write-Host "LCD home-nav-gated check: scanned $sourceFile."

    $failures = @()
    if (-not $result.MenuNavGated) {
        $failures += "ui_home_menu_nav_cb() (-> Config hub) does not reference a PIN gate -- owner decision 2026-09-28 requires every page reached from home to demand the PIN."
    }
    if (-not $result.ProfileBtnGated) {
        $failures += "ui_home_profile_btn_cb() (-> profile picker) does not reference a PIN gate -- owner decision 2026-09-28 requires every page reached from home to demand the PIN."
    }
    if ($result.AuthResetGated) {
        $failures += "ui_home_auth_reset_corner_tap_cb() (the physical credential-reset gesture) references a PIN gate -- this gesture must stay reachable WITHOUT a PIN, since it is the recovery path for a lost PIN. Gating it would make it useless."
    }

    if ($failures.Count -gt 0) {
        Write-Host "LCD HOME NAV GATED CHECK FAILED:" -ForegroundColor Red
        foreach ($f in $failures) {
            Write-Host "  - $f" -ForegroundColor Red
        }
        throw "$($failures.Count) failure(s) above. Owner decision 2026-09-28: only the home/dashboard view itself stays reachable without a PIN."
    }

    Write-Host "LCD home nav gated check passed: Config hub and profile picker are gated; the credential-reset gesture is not."
    exit 0
}
