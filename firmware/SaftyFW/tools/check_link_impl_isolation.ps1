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
#   - 2026-08-27: the same "different algorithm/purpose, allowlisted by path
#     with a written reason" treatment extended to KilnFW's own NVS/flash
#     record-integrity CRC32s and host-test stub headers, found when this
#     check ran for the first time via tools/run_all_checks.ps1 and reported
#     14 hits -- 12 of which turned out to be this class of false positive,
#     not a from-scratch reimplementation of the LINK's CRC16-CCITT-FALSE:
#       - firmware/KilnFW/App/drivers/persist/boot_guard.c -- crc32_compute()/
#         record_crc(): CRC32/0xEDB88320 integrity check over a boot-guard
#         NVS record (this module's own host tests run off-target with no
#         ESP-IDF ROM available, hence a local table-less CRC32 instead of
#         esp_rom_crc32_le() -- see the file's own comment on crc32_compute()).
#       - firmware/KilnFW/App/drivers/safety/watchdog_cfg.c -- crc32_compute()/
#         record_crc(): same CRC32/0xEDB88320, same off-target-host-test
#         reasoning, over a watchdog-config NVS record.
#       - firmware/KilnFW/App/drivers/safety/crash_report.c -- compute_crc(): CRC32
#         (via esp_crc32_le()) over a crash-report NVS record.
#       - firmware/KilnFW/App/drivers/http/profiles_http.c -- compute_profile_crc():
#         CRC32 (via esp_crc32_le()) over a saved kiln-profile NVS slot.
#       - firmware/KilnFW/App/drivers/http/zones_http.c -- compute_zones_crc():
#         CRC32 (via esp_crc32_le()) over a zones-config NVS record.
#       - firmware/KilnFW/App/drivers/safety/safety_cfg_store.c and its header --
#         safety_cfg_store_cached_crc(): a plain ACCESSOR returning an
#         already-stored uint16_t field (`s_store.config_crc`), computing
#         nothing at all. It happens to be named "crc" (the value it returns
#         IS the safety-config CRC the RP2040 reported) and happens to match
#         the definition pattern (return type + open paren), but there is no
#         CRC algorithm anywhere in this function's body to drift.
#       - firmware/KilnFW/App/test/test_safety_cfg_http.c -- its one-line test
#         stub of safety_cfg_store_cached_crc() (`{ return 0; }`), for the
#         same reason as the real accessor just above: a stub returning a
#         fixed value, not a CRC computation.
#       - firmware/KilnFW/App/test/stubs/esp_crc.h and esp_rom_crc.h --
#         host-test STUB headers that re-declare ESP-IDF's own
#         esp_crc32_le()/esp_rom_crc32_le() ROM functions so off-target host
#         tests can link. Neither is a link-layer implementation; both exist
#         solely because ESP-IDF's real ROM functions are unavailable when
#         building for the host, and both compute the SAME CRC32 the ROM
#         function they stand in for computes (see each stub's own header
#         comment), not a from-scratch CRC16-CCITT-FALSE for the framing this
#         check protects.
#     None of the nine files above touches the link's CRC16-CCITT-FALSE or its
#     0x7E byte-stuffing -- all nine are CRC32 (a different polynomial/width
#     entirely) over NVS/flash records, or plain accessors/stubs that compute
#     nothing. Allowlisted by path, not by widening the regex, for the same
#     reason as the bootloader/config_store entries above: an exemption a
#     reader cannot audit is how a check becomes decorative.
#     2026-09-14: two more of the same shape added when this pass wired
#     kiln_cfg_swap.c into the target build -- kiln_cfg_swap.c's own
#     pending_crc() (esp_crc32_le() over the pending-swap record) and
#     test_kiln_cfg_swap.c's fake safety_cfg_store_cached_crc() stub (plain
#     accessor, computes nothing) -- see their own entries below for detail.
#
# uart_protocol.c no longer needs an entry here: as of 2026-08-27 it calls
# kilnlink_crc16_ccitt_false()/kilnlink_stuff() directly at every use site
# (see its own header comment) instead of through the two local wrapper
# functions whose NAMES used to trip this check even though their BODIES were
# pure pass-throughs. Removing that indirection was proven byte-identical in
# firmware/KilnFW/App/test/test_uart_protocol_link_delegate.c before it
# landed -- this closes SaftyFW/TODO.md Phase 1's "KilnFW's uart_protocol.c
# delegating framing/CRC, proven byte-identical" item.
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
    # NOTE: `return $result` alone would let PowerShell's pipeline unroll a
    # single-element (or single-line-file) array down to a bare scalar --
    # for a one-line source file this silently turns $result from a
    # 1-element string array into the line's plain String, so the caller's
    # $codeLines[0] then indexes a single CHARACTER instead of the line and
    # the CRC/stuff patterns never match. The unary comma forces this to
    # stay an array of exactly the lines read, regardless of count.
    return ,$result
}

# Scope: every .c/.h under firmware/, excluding CommonFW (the one legal
# implementation site) and this project's own build/ output directories
# (generated headers, never source).
$allowlistPaths = @(
    (Join-Path $firmwareRoot "SaftyFW\bootloader\crc32.c"),
    (Join-Path $firmwareRoot "SaftyFW\bootloader\crc32.h"),
    (Join-Path $firmwareRoot "SaftyFW\src\config_store.c"),
    (Join-Path $firmwareRoot "SaftyFW\src\config_store.h"),
    (Join-Path $firmwareRoot "SaftyFW\src\config_store_flash.c"),
    # 2026-08-27 additions -- see the header comment's dated entry above for
    # why each of these is a different algorithm/purpose (CRC32 record
    # integrity, or a plain accessor/stub computing nothing) than the link's
    # CRC16-CCITT-FALSE, not a from-scratch reimplementation of it.
    (Join-Path $firmwareRoot "KilnFW\App\drivers\persist\boot_guard.c"),
    # docs/PICO_AUTO_UPDATE_PLAN.md pico_update_attempts.c -- same class as
    # boot_guard.c immediately above: a standalone table-less CRC32 (IEEE
    # 802.3/zlib polynomial) over this module's OWN NVS record, deliberately
    # copied from boot_guard.c's algorithm rather than shared (so this
    # module's host tests build standalone, off-target, without pulling in
    # boot_guard.c) -- nothing to do with the link's CRC16-CCITT-FALSE.
    (Join-Path $firmwareRoot "KilnFW\App\drivers\persist\pico_update_attempts.c"),
    # docs/WEB_AUTH_PLAN.md item 2 (credential storage): web_auth_store.c's
    # crc32_compute() is the SAME class as boot_guard.c's/pico_update_
    # attempts.c's entries above -- a standalone table-less CRC32 (IEEE
    # 802.3/zlib polynomial) over this module's OWN kiln_auth NVS blobs
    # (web/lcd/policy records), deliberately copied rather than shared so
    # this module's host tests build standalone off-target -- nothing to do
    # with the link's CRC16-CCITT-FALSE.
    (Join-Path $firmwareRoot "KilnFW\App\drivers\persist\web_auth_store.c"),
    # docs/TOTP_PASSWORD_RESET_PLAN.md WT-A part 2: totp_config.c's
    # crc32_compute()/secret_blob_crc() are the SAME class as web_auth_
    # store.c's entry just above -- a standalone table-less CRC32 (IEEE
    # 802.3/zlib polynomial) over this module's OWN kiln_auth NVS blob (the
    # TOTP secret/last-counter record), deliberately copied rather than
    # shared so this module's host tests build standalone off-target --
    # nothing to do with the link's CRC16-CCITT-FALSE. test_totp_config_
    # persist.c's own test_crc32() is the matching host-test-side copy, same
    # reason test files elsewhere in this list carry their own copy.
    (Join-Path $firmwareRoot "KilnFW\App\drivers\persist\totp_config.c"),
    (Join-Path $firmwareRoot "KilnFW\App\test\test_totp_config_persist.c"),
    (Join-Path $firmwareRoot "KilnFW\App\drivers\safety\watchdog_cfg.c"),
    (Join-Path $firmwareRoot "KilnFW\App\drivers\safety\crash_report.c"),
    (Join-Path $firmwareRoot "KilnFW\App\drivers\http\profiles_http.c"),
    # zones_http.c's compute_zones_crc() (originally allowlisted here) moved
    # out during the 2026-09-01 zones_http.c split (commits c524ead/b1c072f)
    # into zones_config_json.c/zones_config_store.c, split further into three
    # named functions along the way (one per NVS record: the zones blob,
    # relay names, zone normals). Same class as every other entry in this
    # list -- esp_crc32_le() record integrity over an NVS blob, zeroing the
    # crc32 field first, nothing to do with the link's CRC16-CCITT-FALSE --
    # just relocated by the split, not newly introduced. zones_http.c itself
    # no longer matches (only a comment referencing the new name remains), so
    # its old entry is replaced rather than kept alongside these.
    (Join-Path $firmwareRoot "KilnFW\App\drivers\persist\zones_config_json.c"),
    (Join-Path $firmwareRoot "KilnFW\App\drivers\persist\zones_config_json.h"),
    # zones_config_json.c (1867 lines) was itself split on 2026-09-04 under
    # ROADMAP.md's 1500-line rule; zones_config_json_compute_crc() -- same
    # esp_crc32_le() NVS-record-integrity function as above, just relocated
    # again -- landed in zones_config_migrate.c. Same reasoning, replaced
    # rather than kept alongside since zones_config_json.c no longer defines
    # it.
    (Join-Path $firmwareRoot "KilnFW\App\drivers\persist\zones_config_migrate.c"),
    (Join-Path $firmwareRoot "KilnFW\App\drivers\persist\zones_config_store.c"),
    (Join-Path $firmwareRoot "KilnFW\App\drivers\safety\safety_cfg_store.c"),
    (Join-Path $firmwareRoot "KilnFW\App\drivers\safety\safety_cfg_store.h"),
    # kiln_cfg_swap.c's pending_crc() (2026-09-14, docs/audits/kiln_swap_
    # transaction_2026-09-14.md's H10) -- same class as every other entry in
    # this list: esp_crc32_le() record-integrity over its OWN NVS blob (the
    # pending-swap crash-recovery record, a SEPARATE key from kiln_cfg_
    # store's own), zeroing the crc32 field first, nothing to do with the
    # link's CRC16-CCITT-FALSE. test_kiln_cfg_swap.c's fake safety_cfg_
    # store_cached_crc() stub is the SAME "plain accessor named crc, computes
    # nothing" shape as test_safety_cfg_http.c's identical stub two entries
    # below -- allowlisted alongside it for the same reason.
    (Join-Path $firmwareRoot "KilnFW\App\drivers\persist\kiln_cfg_swap.c"),
    (Join-Path $firmwareRoot "KilnFW\App\test\test_kiln_cfg_swap.c"),
    (Join-Path $firmwareRoot "KilnFW\App\test\test_safety_cfg_http.c"),
    (Join-Path $firmwareRoot "KilnFW\App\test\stubs\esp_crc.h"),
    (Join-Path $firmwareRoot "KilnFW\App\test\stubs\esp_rom_crc.h"),
    # firmware/KilnFW/App/test/test_uart_protocol_link_delegate.c: deliberately
    # DOES contain a from-scratch old_crc16_ccitt_false()/old_stuff() -- a
    # verbatim, intentionally-frozen reproduction of the local implementation
    # uart_protocol.c used to have, kept ONLY so this test can compare it
    # against kilnlink_crc16_ccitt_false()/kilnlink_stuff() and prove they are
    # byte-identical (see the file's own header comment). It is test
    # scaffolding proving isolation, not a second production implementation --
    # the same reasoning firmware/CommonFW/test/test_uart_protocol_delegate.c
    # already relies on by living under the \CommonFW\ exclusion above; this
    # file needs its own path entry because it lives under KilnFW instead.
    (Join-Path $firmwareRoot "KilnFW\App\test\test_uart_protocol_link_delegate.c"),

    # 2026-08-28: firmware/UnitTestFw is a restored, standalone legacy test
    # fixture (git history restore, explicitly restore-only -- not built,
    # not maintained forward). Its uart_protocol.c is a stale fork of
    # KilnFW's own file FROM BEFORE the 2026-08-23 migration to CommonFW's
    # kilnlink_crc16_ccitt_false()/kilnlink_stuff(); it was allowlisted here
    # once already under its previous name before being deleted (when the
    # bench-fixture firmware that replaced it was itself later deleted in
    # turn), so this is a re-entry, not a new exception. Migrating it to
    # delegate like KilnFW did would mean actively maintaining code this
    # project deliberately chose not to build going forward -- see
    # tools/check_no_duplicate_crc.ps1's matching allowlist entry for the
    # same file and its Python mirror.
    (Join-Path $firmwareRoot "UnitTestFw\UnitTest\App\drivers\espInterfaces\uart_protocol.c"),

    # 2026-09-07: cfg_fs_format_gate.c's lfs_style_crc() (and
    # test_cfg_fs_format_gate.c's identical test-side ref_crc(), used to
    # independently construct fixture bytes rather than share the
    # production function with what it's testing) reproduce the pinned
    # joltwallet/littlefs component's own on-disk CRC-32 (0xEDB88320,
    # lfs_util.c's lfs_crc()) bit-for-bit, so the auto-format gate can tell
    # a genuine LittleFS superblock commit from coincidental bytes without
    # linking the full ESP-IDF LittleFS driver into a pure/host-tested
    # module. Same class as every other entry above: a different
    # algorithm/purpose (LittleFS's own on-disk integrity check, not the
    # kilnlink UART link's CRC16-CCITT-FALSE) that happens to also be
    # called "crc", not a from-scratch reimplementation of the link
    # protocol.
    (Join-Path $firmwareRoot "KilnFW\App\drivers\persist\cfg_fs_format_gate.c"),
    (Join-Path $firmwareRoot "KilnFW\App\test\test_cfg_fs_format_gate.c"),

    # 2026-09-18: ota_image_crc.c/.h -- the ESP's CRC-32 over a STAGED PICO
    # IMAGE, introduced by fabd270f. Same class as every entry above, and in
    # fact the weakest possible case for a real violation:
    #   - Different algorithm and purpose. This is standard CRC-32/zlib
    #     (0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF) over a firmware
    #     IMAGE, compared byte-for-byte against SaftyFW's bootloader_crc32()
    #     -- which is itself the FIRST entry in this very allowlist, for
    #     exactly this reason. It is not the link's CRC16-CCITT-FALSE
    #     framing, and has no byte-stuffing anywhere near it.
    #   - It contains NO CRC arithmetic that could drift. The entire module
    #     is a delegation to esp_rom_crc32_le(): no polynomial, no bit loop,
    #     no table, no final XOR. It matches this check's pattern only on the
    #     shape "integer return type + an identifier containing crc + an open
    #     paren" -- the same false-positive shape as safety_cfg_store_cached_
    #     crc() (a plain field accessor) and the esp_crc.h/esp_rom_crc.h
    #     stubs already allowlisted above.
    #   - It exists to ELIMINATE a duplicate, not to add one. Before fabd270f
    #     this CRC was open-coded inline in ota_http_pico.c with the wrong
    #     parameterization (seed 0xFFFFFFFF plus a caller-side final XOR, on
    #     top of a primitive that already complements at both ends), so the
    #     ESP and the Pico disagreed about the ARITHMETIC and no ESP-driven
    #     Pico firmware update could ever have succeeded -- precisely the
    #     silent two-implementations-drift failure this check exists to
    #     prevent. Factoring it into one small, ESP-IDF-free, LINKABLE module
    #     is what lets a known-answer test exist at all (test_ota_image_crc.c
    #     asserts CRC-32("123456789") == 0xCBF43926, the same number
    #     SaftyFW's own bootloader_crc32() test asserts): HTTP handlers in
    #     this tree are target-build-only and never link into the host suite,
    #     so a test written against ota_pico_do_stage() would never have run.
    #     Pushing this arithmetic back out of a linkable module to satisfy a
    #     name-shaped grep would delete the only thing pinning the fix. See
    #     docs/audits/pico_ota_staged_crc_mismatch_2026-09-18.md.
    # Allowlisted by path with a written reason, per this file's standing
    # convention, rather than by widening $crcPattern: the pattern's breadth
    # is what makes it catch a genuine from-scratch reimplementation, and an
    # exemption a reader cannot audit is how a check becomes decorative.
    # (test_ota_image_crc.c needs no entry -- it defines no function whose
    # name matches, only void test_* helpers, and must stay scanned.)
    (Join-Path $firmwareRoot "KilnFW\App\drivers\http\ota_image_crc.c"),
    (Join-Path $firmwareRoot "KilnFW\App\drivers\http\ota_image_crc.h")
)

# Concurrent sessions are the norm in this repo: another agent's in-flight
# negative-test/mutant build can drop a *.c/*.h file under firmware/ (e.g.
# firmware/KilnFW/App/test/) that exists on disk for only as long as that
# agent's own check run needs it. This check's purpose (see header comment)
# is catching a real, committed from-scratch CRC/byte-stuffing reimplementation
# in OUR code -- a transient untracked scratch file is not that, and letting
# it fail this build is a false positive with no fix available to whichever
# session/reader hits it (the file is not theirs to delete). Scope to
# git-tracked files only: `git ls-files` from $firmwareRoot's repo.
$repoRootForGit = Split-Path -Parent $firmwareRoot
$trackedRelPaths = git -C $repoRootForGit ls-files -- 'firmware/*.c' 'firmware/*.h'
if ($LASTEXITCODE -ne 0 -or $null -eq $trackedRelPaths) {
    throw "check_link_impl_isolation.ps1: 'git ls-files' failed -- cannot determine which firmware/*.c|*.h files are tracked, refusing to scan untracked/stray files as a substitute"
}
$trackedFullPaths = [System.Collections.Generic.HashSet[string]]::new([string[]]@(
    $trackedRelPaths | ForEach-Object { (Join-Path $repoRootForGit ($_ -replace '/', '\')) }
), [System.StringComparer]::OrdinalIgnoreCase)

$candidateFiles = Get-ChildItem -Path $firmwareRoot -Recurse -Include *.c, *.h -File |
    Where-Object {
        $full = $_.FullName
        if (-not $trackedFullPaths.Contains($full)) {
            return $false  # untracked -- another agent's in-flight file? not this check's concern
        }
        ($full -notmatch '\\CommonFW\\') -and
        ($full -notmatch '\\build\\') -and
        ($full -notmatch '\\components\\') -and
        # Vendor LCD module datasheet-and-demo bundle (untracked, gitignored
        # -- see .gitignore's "Vendor LCD module datasheets/demo code" entry,
        # added 2026-08-30). Ships a full STM32 HAL tree whose CRC_HandleTypeDef
        # accessors/HAL_CRC_Calculate() etc. legitimately match this check's
        # "returns int, name contains crc, has an open paren" definition
        # pattern -- reference/demo material this project doesn't build or
        # own, not a from-scratch reimplementation of the link's CRC16-CCITT.
        ($full -notmatch '\\KilnFW\\Datasheets\\4\.0inch_SPI_Module_ST7796_MSP4030_MSP4031_V1\.0\\') -and
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
