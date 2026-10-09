# checkcache: ok
# check_safety_baud_sync.ps1 -- the ESP<->RP2040 isolated link baud rate is
# hardcoded in FOUR places that must agree, with no negotiation on the wire.
# A mismatch does not degrade gracefully: it produces framing errors and
# nothing else. Measured 2026-08-25 with the ESP at 230400 and the Pico at
# 115200 -- 121 new crc/framing errors in 3 seconds, zero frames delivered.
#
# The four sites:
#   1. KilnFW      App/drivers/Kconfig            KILNCTL_SAFETY_BAUD_RATE default
#   2. SaftyFW     firmware/hwAbstraction/pico/uart/uart_owner.c
#                                                 UART_OWNER_BAUD_RATE -- HAL
#      Phase 1a (WP2) relocated this file byte-identical from SaftyFW's own
#      src/tasks/uart_owner.c into hwAbstraction/pico/uart/; the #define
#      itself did not move or change, only its file's path.
#   3. SaftyFW     bootloader/main.c              enter_recovery()'s uart_init
#   4. hwAbstraction firmware/hwAbstraction/pico/uart/hal_uart_pico.c
#                                                 HAL_UART_PICO_EXPECTED_BAUD --
#      a local mirror added by f1f7f3c because uart_owner.c exposes no baud
#      accessor and hwAbstraction must not #include SaftyFW's uart_owner.h
#      (one-way boundary, same rule as the KilnFW-side hwAbstraction split).
#      "hand until Phase 1a [accessor lands]" per that file's own comment --
#      until then this script is the only thing keeping it from silently
#      drifting from site 2. Sites 2 and 4 now live in the SAME directory
#      after WP2's move (both under hwAbstraction/pico/uart/) but remain two
#      textually separate #defines in two separate files -- the one-way
#      #include boundary this script's own header explains is exactly why
#      they were never merged into one, so this check's job (catching a
#      textual drift between them) did not go away just because they got
#      closer together on disk.
#
# WHY A SCRIPT AND NOT A _Static_assert: they live in two separate build
# systems (ESP-IDF/Kconfig and the Pico SDK), so no compile-time assert can
# see across them. This check exists because the mismatch actually shipped:
# on 2026-08-25 the Kconfig default was left at 921600 while both SaftyFW
# sites read 230400. It survived a full commit because KilnFW/sdkconfig --
# which DID say 230400 -- is gitignored, so the bench was testing a value a
# fresh clone would never build. The generated config hid the bug from
# everything except a clean checkout.
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

$sites = @()

$kconfigPath = Join-Path $repo 'firmware/KilnFW/App/drivers/Kconfig'
$kconfig = Get-Content -Raw $kconfigPath
# Take the `default` on the first line following the config declaration.
if ($kconfig -notmatch '(?ms)config\s+KILNCTL_SAFETY_BAUD_RATE\b.*?^\s*default\s+(\d+)') {
    throw "check_safety_baud_sync: could not find KILNCTL_SAFETY_BAUD_RATE's default in $kconfigPath -- the option was renamed or restructured, so this check is now blind. Fix the check, do not delete it."
}
$sites += [pscustomobject]@{ Name = 'KilnFW Kconfig default'; Baud = [int]$Matches[1]; Path = $kconfigPath }

$ownerPath = Join-Path $repo 'firmware/hwAbstraction/pico/uart/uart_owner.c'
$owner = Get-Content -Raw $ownerPath
if ($owner -notmatch '#define\s+UART_OWNER_BAUD_RATE\s+(\d+)u') {
    throw "check_safety_baud_sync: could not find UART_OWNER_BAUD_RATE in $ownerPath -- has uart_owner.c moved again or been renamed? Fix the check, do not delete it."
}
$sites += [pscustomobject]@{ Name = 'SaftyFW UART_OWNER_BAUD_RATE'; Baud = [int]$Matches[1]; Path = $ownerPath }

$blPath = Join-Path $repo 'firmware/SaftyFW/bootloader/main.c'
$bl = Get-Content -Raw $blPath
if ($bl -notmatch 'uart_init\(uart1,\s*(\d+)u\)') {
    throw "check_safety_baud_sync: could not find recovery-mode uart_init(uart1, ...) in $blPath -- fix the check, do not delete it."
}
$sites += [pscustomobject]@{ Name = 'SaftyFW bootloader recovery'; Baud = [int]$Matches[1]; Path = $blPath }

$halUartPicoPath = Join-Path $repo 'firmware/hwAbstraction/pico/uart/hal_uart_pico.c'
$halUartPico = Get-Content -Raw $halUartPicoPath
if ($halUartPico -notmatch '#define\s+HAL_UART_PICO_EXPECTED_BAUD\s+(\d+)u') {
    throw "check_safety_baud_sync: could not find HAL_UART_PICO_EXPECTED_BAUD in $halUartPicoPath -- fix the check, do not delete it."
}
$sites += [pscustomobject]@{ Name = 'hwAbstraction HAL_UART_PICO_EXPECTED_BAUD'; Baud = [int]$Matches[1]; Path = $halUartPicoPath }

$distinct = $sites.Baud | Sort-Object -Unique
if ($distinct.Count -ne 1) {
    Write-Host 'Safety-link baud rates DISAGREE:'
    foreach ($s in $sites) { Write-Host ("  {0,-32} {1}" -f $s.Name, $s.Baud) }
    throw "check_safety_baud_sync: $($distinct.Count) different baud rates across the four hardcoded sites ($($distinct -join ', ')). They must all match -- the link has no baud negotiation, and a mismatch means framing errors, not slow operation."
}

Write-Host ("Safety-link baud sync OK: all four sites agree at {0} baud." -f $distinct[0])
foreach ($s in $sites) { Write-Host ("  {0,-32} {1}" -f $s.Name, $s.Baud) }
