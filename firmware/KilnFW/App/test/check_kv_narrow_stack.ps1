# checkcache: ok
# check_kv_narrow_stack.ps1 -- guards ota_page.html's ".kv" fix (owner
# report: "the RP2040 (safety processor) section of the fw update looks
# smashed on the phone").
#
# `.kv` is the key/value grid ota_page.html uses for both the ESP32-S3 and
# RP2040 status blocks (`grid-template-columns: auto 1fr`). Two things have
# to both be true for it to degrade gracefully on a narrow phone instead of
# crushing the value column:
#   - `.kv dd` needs `min-width: 0` so the value track can actually shrink
#     (a grid item's default min-width is its own min-content, which floors
#     the column at an unbreakable token's width regardless of the `1fr`
#     track it lives in).
#   - a narrow-viewport media query collapsing `.kv` to a single stacked
#     column, because even a shrinkable `1fr` value column plus a long
#     multi-word label ("Safety processor (RP2040) build commit / date")
#     can still be tight enough to look cramped at 320-360px.
#
# `.kv dt` (the label column) deliberately does NOT get `min-width: 0` --
# see ota_page.html's own `.kv` comment and check_label_column_overflow_
# wrap.ps1's header comment for why a shrinkable label column is the bug,
# not the fix.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_kv_narrow_stack.ps1
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "_drivers_layout.ps1")
$driversRoot = Get-DriversRoot -TestDir $PSScriptRoot
$otaPath = Resolve-DriverFile -DriversDir $driversRoot -BaseName "ota_page.html"

$raw = Get-Content -Path $otaPath -Raw
$stripped = $raw -replace '(?s)/\*.*?\*/', ' '
$normalized = ($stripped -replace '\s+', ' ')

$failures = @()

if ($normalized -notmatch '\.kv dd \{[^}]*min-width:\s*0') {
    $failures += "ota_page.html: .kv dd no longer has min-width: 0 -- without it the value column " +
        "cannot shrink below an unbreakable token's width, which is most of what made the RP2040 " +
        "block look 'smashed' on a phone."
}

if ($normalized -notmatch '\.kv dt \{[^}]*min-width:\s*0') {
    # This is the CORRECT state, not a failure -- asserted here (inverted)
    # so a future edit that adds min-width:0 to .kv dt (copying the .kv dd
    # rule next to it without reading why they differ) is caught too. See
    # check_label_column_overflow_wrap.ps1 for the general form of this bug.
    Write-Host "  (ok: .kv dt correctly does NOT have min-width: 0 -- it is the protected label column)"
} else {
    $failures += "ota_page.html: .kv dt now has min-width: 0 -- this re-introduces the label-column-" +
        "collapse bug (see check_label_column_overflow_wrap.ps1): a shrinkable label column crushes " +
        "toward zero width when the row is tight, instead of the value column giving way first."
}

if ($normalized -notmatch '@media \(max-width:\s*\d+px\)\s*\{\s*\.kv \{[^}]*grid-template-columns:\s*1fr') {
    $failures += "ota_page.html: no narrow-viewport media query collapsing .kv to a single stacked " +
        "column ('grid-template-columns: 1fr') -- even a shrinkable value column can still be too " +
        "tight to read at 320-360px next to a long multi-word RP2040 label."
}

if ($failures.Count -gt 0) {
    Write-Host "KV NARROW STACK CHECK FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  $f" -ForegroundColor Red
    }
    throw "$($failures.Count) piece(s) of ota_page.html's .kv narrow-viewport fix are missing -- see this script's header comment."
}

Write-Host "KV narrow stack check passed: ota_page.html's .kv has a shrinkable value column, a protected label column, and a narrow-viewport single-column fallback."
exit 0
