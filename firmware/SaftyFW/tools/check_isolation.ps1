# check_isolation.ps1 -- the CI grep check both docs/ARCHITECTURE.md section 2
# ("The one rule that matters") and TODO.md Phase 2's own checklist call for:
#
#   safety_core.c does not #include the link header, and link_task.c does not
#   touch GPIO6.
#
# This is what makes "a frozen ESP cannot hang the safety processor"
# structural rather than a promise -- checked by machine on every build/CI
# run, not just by code review. Exits non-zero (and throws, matching this
# project's other build scripts -- see test/build_host_tests.ps1) on any
# violation.
#
# Usage: powershell -File tools\check_isolation.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$failures = @()

# Both rules below match against CODE only, with "//" line comments and
# "/* */" block comments stripped first. Deliberate: this file's own header
# comments (and link_task.c's) name "the link" and "the relay" in prose to
# explain the rule to a human reader -- that prose is the point, not a
# violation of it. What must never appear is the symbol in actual code: an
# #include line safety_core.c compiles, or a GPIO6/relay call link_task.c
# actually executes. Stripping comments first is what lets the check be both
# strict on real violations and silent on the doc comments describing why
# they must not happen.
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
        # Strip a line comment, then any block comment(s) remaining on this line.
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

# --- Rule 1: safety_core.{c,h} must not #include a link/uart header. -------
# Matches any #include line naming "uart" or "link" (case-insensitive) --
# deliberately broad rather than an exact filename allowlist, since the point
# is that safety_core has NO path to the link, not that it avoids one
# specific header today and gains a new one tomorrow undetected.
$safetyCoreFiles = @(
    (Join-Path $root "src\tasks\safety_core.c"),
    (Join-Path $root "src\tasks\safety_core.h")
)
foreach ($f in $safetyCoreFiles) {
    if (-not (Test-Path $f)) {
        $failures += "MISSING: $f (expected to exist and be checked for link/uart includes)"
        continue
    }
    $codeLines = Get-CodeOnlyLines -Path $f
    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        if ($codeLines[$i] -match '^\s*#include\s*["<].*?(uart|link)') {
            $failures += "$($f):$($i + 1): safety_core includes a link/uart header -- $($codeLines[$i].Trim())"
        }
    }
}

# --- Rule 2: link_task.{c,h} must never reference GPIO6 or the relay. ------
# Catches the pin number in any GPIOn spelling, this repo's own relay pin
# symbol/net names, and the word "relay" generally, in actual code (comments
# are already stripped by Get-CodeOnlyLines above).
$linkTaskFiles = @(
    (Join-Path $root "src\tasks\link_task.c"),
    (Join-Path $root "src\tasks\link_task.h")
)
$relayPattern = 'GPIO\s*0*6\b|SAFTYFW_PIN_RELAY|saftyRelay|relay_owner|\brelay\b'
foreach ($f in $linkTaskFiles) {
    if (-not (Test-Path $f)) {
        $failures += "MISSING: $f (expected to exist and be checked for GPIO6/relay references)"
        continue
    }
    $codeLines = Get-CodeOnlyLines -Path $f
    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        if ($codeLines[$i] -match $relayPattern) {
            $failures += "$($f):$($i + 1): link_task references GPIO6/the relay -- $($codeLines[$i].Trim())"
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "ISOLATION CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "$($failures.Count) isolation violation(s) found -- see docs/ARCHITECTURE.md section 2"
}

Write-Host "Isolation check passed: safety_core.{c,h} has no link/uart include; link_task.{c,h} has no GPIO6/relay reference."
exit 0
