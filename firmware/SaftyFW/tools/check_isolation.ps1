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
# "/* */" block comments stripped first, and the CONTENTS of string and
# character literals blanked. Deliberate: this file's own header comments
# (and link_task.c's) name "the link" and "the relay" in prose to explain the
# rule to a human reader -- that prose is the point, not a violation of it.
# What must never appear is the symbol in actual code: an #include line
# safety_core.c compiles, or a GPIO6/relay call link_task.c actually
# executes. Stripping comments first is what lets the check be both strict on
# real violations and silent on the doc comments describing why they must not
# happen.
#
# 2026-08-24: string literals are blanked for exactly the same reason, after a
# real false positive. link_task.c compares a rejection reason handed back to
# it by config_store:
#
#   strcmp(reason, "refused: relay is ARMED, config writes are refused while ARMED")
#
# `relay` matched the word inside that literal and failed the check. That
# line never touches GPIO6 or any relay API -- it is wire-protocol text being
# mapped onto a rejection enum, i.e. prose that happens to live in a string
# rather than in a comment. Treating it as a violation would have pushed
# someone toward the worst available fix: renaming the operator-facing message
# to satisfy a regex.
#
# Blanking is applied to every line EXCEPT #include lines, and that exception
# is load-bearing. Rule 1 matches the header name INSIDE the quotes, so
# blanking there would turn a genuine violation -- #include "link_task.h" in
# safety_core.c -- into '#include ""' and pass it. That is not hypothetical:
# an earlier revision of this file blanked unconditionally, reported "Isolation
# check passed" against exactly that injected violation, and was caught only
# by negative-testing BOTH rules rather than the one being fixed.
#
# For every other line, blanking cannot weaken rule 2: a relay call or a GPIO6
# access cannot hide inside a string literal, because a string is data, not
# something link_task executes. Quote characters are KEPT so a line does not
# collapse into something unrecognisable; only what is between them goes.
# Blanks the CONTENTS of double-quoted string and single-quoted character
# literals, keeping the surrounding quotes. Honours backslash escapes so an
# embedded \" does not end the literal early. Single-line only, which matches
# this codebase (no raw/multi-line literals in C); an unterminated literal is
# treated as running to end of line, which fails safe -- it blanks MORE, and
# rule 1's #include lines never reach that state.
function Remove-StringLiteralContents {
    param([string]$Text)
    $out = New-Object System.Text.StringBuilder
    $i = 0
    while ($i -lt $Text.Length) {
        $ch = $Text[$i]
        if ($ch -eq '"' -or $ch -eq "'") {
            $quote = $ch
            [void]$out.Append($quote)
            $i++
            while ($i -lt $Text.Length) {
                if ($Text[$i] -eq '') {
                    $i += 2   # skip the escape and whatever it escapes
                    continue
                }
                if ($Text[$i] -eq $quote) { break }
                $i++
            }
            if ($i -lt $Text.Length) {
                [void]$out.Append($quote)
                $i++
            }
            continue
        }
        [void]$out.Append($ch)
        $i++
    }
    return $out.ToString()
}

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
        # NOT on #include lines: rule 1 matches the header NAME inside the
        # quotes ('^\s*#include\s*["<].*?(uart|link)'), so blanking it would
        # turn a real violation -- #include "link_task.h" in safety_core.c --
        # into '#include ""' and pass. That regression was caught by
        # negative-testing BOTH rules after this change, not by review; an
        # earlier revision of this file shipped the blanking unconditionally
        # and silently blinded rule 1 while still passing its own run.
        if ($code -notmatch '^\s*#\s*include') {
            $code = Remove-StringLiteralContents -Text $code
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
# Globbed, not a literal two-name list: link_task.c has been split three times
# (link_task_commit_reject.c, link_task_tc_type_gate.c,
# link_task_announce_eval.c) to make pure decision logic host-testable, and a
# hardcoded list silently stops covering whatever moved out -- exactly the
# "a split breaks a filename-keyed check" class CLAUDE.md names. The two base
# files stay named explicitly so a rename/deletion still reports MISSING.
$linkTaskFiles = @(
    (Join-Path $root "src\tasks\link_task.c"),
    (Join-Path $root "src\tasks\link_task.h")
)
$linkTaskFiles += @(Get-ChildItem -Path (Join-Path $root "src\tasks") -File |
    Where-Object { $_.Name -like 'link_task_*.c' -or $_.Name -like 'link_task_*.h' } |
    ForEach-Object { $_.FullName })
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
