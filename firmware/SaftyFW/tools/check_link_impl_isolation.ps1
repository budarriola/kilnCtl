# check_link_impl_isolation.ps1 -- TODO.md Phase 1's CI grep: no CRC or
# byte-stuffing IMPLEMENTATION exists outside firmware/CommonFW. The link
# contract (framing: 0x7E delimiter/byte-stuffing, CRC16-CCITT) is shared
# code, implemented exactly once in firmware/CommonFW/src/kilnlink_crc.c and
# kilnlink_frame.c -- every consumer (KilnFW, SaftyFW, pc_tools) must CALL
# those functions, never re-derive the algorithm locally. A second, drifted
# implementation is exactly the kind of bug that passes every host test
# against itself and disagrees silently with its peer over real wire.
#
# Modeled on tools/check_isolation.ps1: same Get-CodeOnlyLines comment-strip
# (a comment saying "this implements CRC16" must not trip the check -- only
# actual code matters), same non-zero-exit-on-violation convention, same
# "throw" so this project's other build scripts (test/build_host_tests.ps1)
# and CMake custom targets fail loudly and identically.
#
# What this catches: a function DEFINITION (or prototype) whose name
# contains "crc" (any case) and returns an integer type -- i.e. someone has
# written a CRC computation from scratch -- or a function DEFINITION whose
# name contains "stuff" (catches both kilnlink_stuff-style names and
# "unstuff", since "unstuff" contains "stuff") -- i.e. someone has written
# frame byte-stuffing/unstuffing from scratch. It deliberately does NOT
# flag:
#   - a CALL to an existing CommonFW function (e.g. `kilnlink_crc16_ccitt_
#     false(buf, len)` used as an expression, not a definition -- the
#     pattern below requires a bare identifier immediately followed by `(`
#     at statement start with a return-type prefix, which a call site
#     lacks)
#   - a comment mentioning "CRC" or "byte-stuffing" in prose (already
#     stripped by Get-CodeOnlyLines)
#   - a struct/variable NAMED with "crc" in it (e.g. `uint16_t crc;`,
#     `config->config_crc`) -- the pattern requires an open-paren, so a
#     plain declaration or field access never matches
#   - generic string/JSON escaping (`json_escape()` et al, or vendored
#     libraries' `ScanCopyUnescapedString`/similar) -- deliberately matching
#     only "stuff"/"unstuff", not "escape", since "escape" is a completely
#     unrelated, common word in string-formatting code all over both
#     firmwares' HTTP handlers and has nothing to do with link-frame byte
#     stuffing. A first pass of this pattern that also matched "escape" hit
#     ten real, unrelated JSON-escaping functions; narrowing to "stuff" is
#     what makes the check specific to the actual hazard.
#   - vendored/third-party code under any `components\` directory (e.g.
#     KilnFW's `components/lvgl`, including its bundled FT800-FT813 display
#     driver and thorvg/rapidjson) -- code this project did not write and
#     does not own is out of scope for "no from-scratch reimplementation in
#     OUR code"; a vendor library's own CRC (e.g. an FTDI EVE chip's
#     `EVE_cmd_memcrc`, entirely unrelated hardware) is not what Phase 1's
#     item is about.
#   - firmware/SaftyFW/bootloader/crc32.c (and its header) and
#     firmware/SaftyFW/src/config_store.c / config_store.h /
#     config_store_flash.c -- explicit, documented exceptions: each is a
#     *different* algorithm/purpose (CRC32/0xEDB88320 image+metadata
#     integrity for the bootloader; a config-record integrity CRC for
#     config_store) than the link protocol's CRC16-CCITT framing this item
#     is about. Allowlisted by path, not by pattern, so they stay visible
#     rather than silently exempted by a loophole in the regex.
#
# What this WILL still flag, on purpose: firmware/KilnFW/App/drivers/
# espInterfaces/uart_protocol.c (and its host-test twin under
# firmware/UnitTestFw/) really does still implement its own
# crc16_ccitt_false()/stuff_and_send() outside CommonFW today -- this is the
# EXACT, currently-open TODO.md Phase 1 item ("KilnFW's uart_protocol.c
# delegating framing/CRC... before the old code is deleted") this check
# exists to close. It is expected to report these two hits until that
# refactor lands; that is the check doing its job, not a false positive to
# suppress.
#
# Usage: powershell -File tools\check_link_impl_isolation.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$firmwareRoot = Split-Path -Parent $root

# Same comment-stripping helper as check_isolation.ps1 (duplicated rather
# than imported -- this project has no shared PowerShell module mechanism,
# and check_isolation.ps1 is not something this new check should take a
# dependency on).
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

# Scope: every .c/.h under firmware/, excluding CommonFW (the one legal
# implementation site) and this project's own build/ output directories
# (generated headers, never source).
$allowlistPaths = @(
    (Join-Path $firmwareRoot "SaftyFW\bootloader\crc32.c"),
    (Join-Path $firmwareRoot "SaftyFW\bootloader\crc32.h"),
    (Join-Path $firmwareRoot "SaftyFW\src\config_store.c"),
    (Join-Path $firmwareRoot "SaftyFW\src\config_store.h"),
    (Join-Path $firmwareRoot "SaftyFW\src\config_store_flash.c")
)

$candidateFiles = Get-ChildItem -Path $firmwareRoot -Recurse -Include *.c, *.h -File |
    Where-Object {
        $full = $_.FullName
        ($full -notmatch '\\CommonFW\\') -and
        ($full -notmatch '\\build\\') -and
        ($full -notmatch '\\components\\') -and
        ($allowlistPaths -notcontains $full)
    }

# Function-DEFINITION patterns: an integer-ish return type, whitespace, an
# identifier containing the keyword, then an open paren -- this is what a
# definition (or a forward declaration/prototype, also worth flagging: a
# prototype for a from-scratch implementation is still evidence one exists
# or is about to) looks like. A call site (`x = kilnlink_crc16_ccitt_false(a, b);`)
# never starts with a type keyword, so it does not match.
$crcPattern = '\b(uint8_t|uint16_t|uint32_t|unsigned\s+short|unsigned\s+long|unsigned\s+int|int|size_t)\s+\**\w*crc\w*\s*\('
$stuffPattern = '\b(uint8_t|uint16_t|uint32_t|size_t|int|void)\s+\**\w*stuff\w*\s*\('

$failures = @()
foreach ($f in $candidateFiles) {
    $codeLines = Get-CodeOnlyLines -Path $f.FullName
    for ($i = 0; $i -lt $codeLines.Count; $i++) {
        if ($codeLines[$i] -match $crcPattern) {
            $failures += "$($f.FullName):$($i + 1): CRC implementation outside CommonFW -- $($codeLines[$i].Trim())"
        }
        if ($codeLines[$i] -match $stuffPattern) {
            $failures += "$($f.FullName):$($i + 1): byte-stuffing implementation outside CommonFW -- $($codeLines[$i].Trim())"
        }
    }
}

if ($failures.Count -gt 0) {
    Write-Host "LINK IMPLEMENTATION ISOLATION CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "$($failures.Count) CRC/byte-stuffing implementation(s) found outside firmware/CommonFW -- see TODO.md Phase 1"
}

Write-Host "Link implementation isolation check passed: no CRC/byte-stuffing implementation found outside firmware/CommonFW."
exit 0
