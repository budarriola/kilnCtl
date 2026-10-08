# checkcache: ok
# check_safety_trip_mask_docs.ps1 -- pins documented SAFETY_TRIP_MAIN_FAULT
# (S6a) trip_mask constants against the firmware's own mask formula.
#
# WHY THIS EXISTS: CLAUDE.md stated "S6a (mainFault) is bit 6, `0x0040`" for
# most of 2026-09-09. The firmware actually computes
# `link_frame_trip_mask_for_reason()` (firmware/SaftyFW/src/tasks/
# link_frame.c) as `1 << (reason - 1)`; SAFETY_TRIP_MAIN_FAULT is reason 6,
# so its mask is bit 5, `0x0020` -- `0x0040` is bit 6, which is actually
# reason 7 (SAFETY_TRIP_LINK_DEAD / S6b), a DIFFERENT guard. That wrong
# constant was quoted in instructions of the form "confirm the mask shows
# ONLY bit 6 (0x0040) before clearing the trip" and got propagated into
# docs/MCP_SERVERS.md the same day. No harm resulted only because the agents
# involved happened to also check trip_reason directly -- acting on the
# mask value alone could mean clearing a genuine, different trip while
# believing it was the benign post-reset S6a handshake artifact.
#
# WHAT THIS CHECKS:
#   1. link_frame.c still computes the mask as `1u << ((uint8_t)reason - 1u)`
#      (or an equivalent `1 << (reason - 1)` shape) -- if this formula's
#      shape changes, every constant this script checks may need
#      recalculating, so this check fails loudly rather than silently
#      validating stale doc values against a formula that no longer exists.
#   2. Every prose mention of SAFETY_TRIP_MAIN_FAULT (S6a) alongside a hex
#      trip_mask literal in CLAUDE.md and docs/MCP_SERVERS.md states
#      `0x0020`, never `0x0040` -- keyed on proximity to "MAIN_FAULT" or
#      "S6a" so a rewrite of the surrounding sentence doesn't silently drop
#      out of scope, and scoped to hex literals near a "trip_mask" mention
#      so an unrelated `0x0040` elsewhere in the file is not a false
#      positive.
#
# WHAT WOULD INVALIDATE THIS CHECK: the mask formula changing shape (the
# comment above already covers that -- update reason (1) is designed to
# catch it); SAFETY_TRIP_MAIN_FAULT being renumbered away from reason 6
# (firmware/SaftyFW/docs/ARCHITECTURE.md's enum comment says renumbering
# before first release is allowed) -- if that happens, update the expected
# mask constant here to match, don't delete the check.
#
# Usage: powershell -File tools\check_safety_trip_mask_docs.ps1

param(
    [string]$LinkFrameCPath,
    [string[]]$DocPaths
)

$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
if (-not $LinkFrameCPath) {
    $LinkFrameCPath = Join-Path $root "..\firmware\SaftyFW\src\tasks\link_frame.c"
}
$LinkFrameCPath = (Resolve-Path $LinkFrameCPath).Path

if (-not $DocPaths -or $DocPaths.Count -eq 0) {
    $DocPaths = @(
        (Join-Path $root "..\CLAUDE.md"),
        (Join-Path $root "..\docs\MCP_SERVERS.md")
    )
}
$DocPaths = $DocPaths | ForEach-Object { (Resolve-Path $_).Path }

$failures = @()

# ---------------------------------------------------------------------------
# 1. Pin the formula's shape in link_frame.c.
# ---------------------------------------------------------------------------
$cText = Get-Content -Raw -Path $LinkFrameCPath
$formulaMatch = [regex]::Match($cText, 'return\s*\(uint16_t\)\(1u\s*<<\s*\(\(uint8_t\)reason\s*-\s*1u\)\);')
if (-not $formulaMatch.Success) {
    throw "check_safety_trip_mask_docs.ps1: link_frame_trip_mask_for_reason()'s formula in $LinkFrameCPath no longer matches the expected '1u << ((uint8_t)reason - 1u)' shape -- has it changed? If so, recheck every mask constant this script pins and update it, don't just relax this regex."
}

# SAFETY_TRIP_MAIN_FAULT = reason 6 (firmware/SaftyFW/docs/ARCHITECTURE.md).
# Recompute from the same formula this check just confirmed, rather than a
# second hardcoded literal, so the two can't independently drift.
$mainFaultReason = 6
$expectedMask = 1 -shl ($mainFaultReason - 1)
$expectedHex = "0x{0:x4}" -f $expectedMask

# ---------------------------------------------------------------------------
# 2. Scan doc files for a wrong constant near a MAIN_FAULT/S6a mention.
# ---------------------------------------------------------------------------
foreach ($docPath in $DocPaths) {
    # Look at each line plus one line of trailing context (some prose wraps
    # the guard name and the hex literal across a Markdown line break).
    $lines = Get-Content -Path $docPath
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $windowEnd = [Math]::Min($i + 1, $lines.Count - 1)
        $window = ($lines[$i..$windowEnd] -join " ")
        if ($window -notmatch "MAIN_FAULT|S6a") { continue }
        if ($window -notmatch "mask") { continue }
        foreach ($hexMatch in [regex]::Matches($window, '0x00[0-9A-Fa-f]{2}')) {
            $found = $hexMatch.Value.ToLowerInvariant()
            if ($found -eq $expectedHex) { continue }
            # A wrong-value hex literal is fine when the surrounding text is
            # explicitly contrasting it (e.g. "0x0020 -- not 0x0040, which is
            # bit 6") -- that is the document correctly warning readers away
            # from the mistake, not making it. Only flag a wrong literal that
            # is NOT immediately preceded by a negation word.
            $precedingStart = [Math]::Max(0, $hexMatch.Index - 20)
            $preceding = $window.Substring($precedingStart, $hexMatch.Index - $precedingStart)
            if ($preceding -match "\bnot\b[\*\s\x60'""]*$") { continue }
            $failures += "$($docPath):$($i + 1): mentions SAFETY_TRIP_MAIN_FAULT/S6a's trip_mask as '$found' (not clearly negated), but reason $mainFaultReason -> 1 << ($mainFaultReason - 1) = $expectedHex (link_frame_trip_mask_for_reason(), $LinkFrameCPath). Line: $($lines[$i].Trim())"
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "SAFETY TRIP MASK DOC CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "A document states the wrong trip_mask constant for SAFETY_TRIP_MAIN_FAULT (S6a). See link_frame_trip_mask_for_reason() -- mask is 1 << (reason - 1), reason 6 -> $expectedHex, not any other value."
}

Write-Host "Safety trip mask doc check passed: no doc states SAFETY_TRIP_MAIN_FAULT's trip_mask as anything but $expectedHex."
exit 0
