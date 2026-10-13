# checkcache: ok
# check_internal_alloc_allowlist.ps1 -- IRAMGATE (2026-10-13).
# Fails on any plain malloc/calloc/realloc/xTaskCreate/xTaskCreatePinnedToCore
# in firmware/KilnFW/App (non-test .c/.h) that is not on the reviewed allowlist
# below with a reason. WHY: CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192 puts every
# plain malloc <= 8 KB, and every xTaskCreate stack, in internal RAM, which sits
# near the owner floor (heap_internal min_free >= 8192 B). New scratch should use
# persist_scratch_alloc()/heap_caps_malloc(SPIRAM|8BIT); a stack that may live in
# PSRAM should use xTaskCreate*WithCaps. If a new site really must be internal
# (NVS/flash writer, DMA, ISR, cache-disabled path, early boot), add or raise its
# entry here WITH a reason. Counts are exact: a lower count also fails (so the
# list cannot rot). See docs/audits/INTERNAL_RAM_DRIFT_2026-10-13.md.
# -Dump prints current counts per file.
param([switch]$Dump)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$app = Join-Path $root 'firmware/KilnFW/App'

# file (relative to App) -> @(count, reason)
$allow = [ordered]@{
'drivers/bridge/uart_bridge_ext.c' = @(1, 'task stack stays internal: IO/owner/bridge task runs UART/I2C/SPI drivers and may run during cache-disabled flash ops')
'drivers/bridge/uart_bridge_system.c' = @(1, 'task stack stays internal: IO/owner/bridge task runs UART/I2C/SPI drivers and may run during cache-disabled flash ops')
'drivers/control/profile_executor_start.c' = @(2, 'executor task create + start-path alloc: executor persists via flash worker; task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'drivers/control/zones_current_sweep_task.c' = @(1, 'task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'drivers/http/backup_import.c' = @(2, 'import job ctx and pass-1 scratch; job task writes NVS; transient request/boot-time buffer, freed on exit; follow-up candidate for persist_scratch_alloc (see INTERNAL_RAM_DRIFT_2026-10-13.md)')
'drivers/http/diagnostics_http.c' = @(1, 'coredump read buffer (flash read path); transient request/boot-time buffer, freed on exit; follow-up candidate for persist_scratch_alloc (see INTERNAL_RAM_DRIFT_2026-10-13.md)')
'drivers/http/http_async_job.c' = @(1, 'async job task writes NVS/flash; task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'drivers/http/ota_http_esp.c' = @(1, 'OTA task writes flash; task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'drivers/http/ota_http_pico.c' = @(1, 'OTA task; task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'drivers/http/ota_http_recovery.c' = @(2, 'recovery OTA/apply tasks write flash; task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'drivers/http/profiles_edit_http.c' = @(1, 'profile_t scratch in save path; transient request/boot-time buffer, freed on exit; follow-up candidate for persist_scratch_alloc (see INTERNAL_RAM_DRIFT_2026-10-13.md)')
'drivers/http/sw_reset_http.c' = @(1, 'reboot handler body; task stack stays internal: early boot or reboot-path task, PSRAM may not be up/safe')
'drivers/hw/backlight_pwm.c' = @(1, 'backlight task disabled by default; moving gains nothing; task stack stays internal: IO/owner/bridge task runs UART/I2C/SPI drivers and may run during cache-disabled flash ops')
'drivers/hw/panel_spi_draw.c' = @(1, 'panel SPI DMA text buffer: DMA requires internal RAM')
'drivers/net/ota_pico_relay.c' = @(1, 'Pico relay task writes flash/UART; task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'drivers/net/pico_auto_update_boot.c' = @(1, 'boot-time updater task; task stack stays internal: early boot or reboot-path task, PSRAM may not be up/safe')
'drivers/net/wifi_prov.c' = @(1, 'wifi_prov task writes NVS; task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'drivers/owners/kiln_io_owner.c' = @(1, 'task stack stays internal: IO/owner/bridge task runs UART/I2C/SPI drivers and may run during cache-disabled flash ops')
'drivers/owners/thermo_owner.c' = @(1, 'task stack stays internal: IO/owner/bridge task runs UART/I2C/SPI drivers and may run during cache-disabled flash ops')
'drivers/persist/cfg_fs_mount.c' = @(3, 'LittleFS mount/scan/format buffers touch flash directly; transient request/boot-time buffer, freed on exit; follow-up candidate for persist_scratch_alloc (see INTERNAL_RAM_DRIFT_2026-10-13.md)')
'drivers/persist/kiln_cfg_store.c' = @(3, 'boot-time v1/v2 blob migration scratch, freed immediately; transient request/boot-time buffer, freed on exit; follow-up candidate for persist_scratch_alloc (see INTERNAL_RAM_DRIFT_2026-10-13.md)')
'drivers/persist/kiln_cfg_store_internal.h' = @(1, 'inline helper: PSRAM-first with internal fallback (documented fallback)')
'drivers/persist/kiln_cfg_swap_worker.c' = @(1, 'swap worker writes NVS; task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'drivers/persist/log_store.c' = @(1, 'log reader ctx, tiny, flash-read path; transient request/boot-time buffer, freed on exit; follow-up candidate for persist_scratch_alloc (see INTERNAL_RAM_DRIFT_2026-10-13.md)')
'drivers/persist/persist_scratch.h' = @(1, 'the sanctioned PSRAM-first helper''s internal fallback')
'drivers/safety/danger_mode.c' = @(1, 'danger_mode task writes NVS; task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'drivers/ui/screen_idle.c' = @(1, 'UI task drives LVGL/SPI; task stack stays internal: IO/owner/bridge task runs UART/I2C/SPI drivers and may run during cache-disabled flash ops')
'drivers/ui/ui_page_network.c' = @(2, 'UI worker tasks call wifi_prov (NVS writes); task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'drivers/ui/ui_page_network_manage.c' = @(3, 'UI worker tasks call wifi_prov (NVS writes); task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'drivers/update/update_fetch.c' = @(1, 'fetch writer task writes flash; task stack stays internal: task writes NVS/flash (cache-disabled windows)')
'main_network_http.c' = @(2, 'ota_confirm ctx/task at early boot; task stack stays internal: early boot or reboot-path task, PSRAM may not be up/safe')
}

function Strip-C([string]$t) {
    $sb = New-Object System.Text.StringBuilder
    $i = 0; $n = $t.Length
    while ($i -lt $n) {
        $c = $t[$i]; $d = if ($i + 1 -lt $n) { $t[$i + 1] } else { [char]0 }
        if ($c -eq '/' -and $d -eq '/') { while ($i -lt $n -and $t[$i] -ne "`n") { $i++ }; continue }
        if ($c -eq '/' -and $d -eq '*') { $i += 2; while ($i + 1 -lt $n -and -not ($t[$i] -eq '*' -and $t[$i + 1] -eq '/')) { $i++ }; $i += 2; continue }
        if ($c -eq '"' -or $c -eq "'") {
            $q = $c; [void]$sb.Append($q); $i++
            while ($i -lt $n -and $t[$i] -ne $q) { if ($t[$i] -eq '\') { $i++ }; $i++ }
            [void]$sb.Append($q); $i++; continue
        }
        [void]$sb.Append($c); $i++
    }
    $sb.ToString()
}

$rx = [regex]'(?<![A-Za-z0-9_.>])(malloc|calloc|realloc|xTaskCreate|xTaskCreatePinnedToCore)\s*\('
$cur = @{}
Get-ChildItem -Path $app -Recurse -Include *.c, *.h -File | Where-Object { $_.FullName -notmatch '[/\\]test[/\\]' } | ForEach-Object {
    $rel = $_.FullName.Substring($app.Length + 1) -replace '[\\]', '/'
    $m = $rx.Matches((Strip-C ([IO.File]::ReadAllText($_.FullName)))).Count
    if ($m -gt 0) { $cur[$rel] = $m }
}
if ($Dump) { $cur.GetEnumerator() | Sort-Object Name | ForEach-Object { "{0} {1}" -f $_.Name, $_.Value }; exit 0 }

$bad = @()
foreach ($k in $cur.Keys) {
    if (-not $allow.Contains($k)) { $bad += "NEW plain alloc/task-create site(s) in $k ($($cur[$k])): use persist_scratch_alloc()/heap_caps_malloc(SPIRAM) or xTaskCreate*WithCaps, or allowlist with a reason" }
    elseif ($cur[$k] -gt $allow[$k][0]) { $bad += "$k has $($cur[$k]) sites, allowlist says $($allow[$k][0])" }
    elseif ($cur[$k] -lt $allow[$k][0]) { $bad += "$k has $($cur[$k]) sites, allowlist says $($allow[$k][0]): lower the entry" }
}
foreach ($k in $allow.Keys) {
    if (-not $cur.ContainsKey($k)) { $bad += "allowlist entry $k has no sites left: remove it" }
    if (-not $allow[$k][1] -or $allow[$k][1].Length -lt 8) { $bad += "allowlist entry $k lacks a reason" }
}
if ($bad.Count) { $bad | ForEach-Object { "FAIL: $_" }; exit 1 }
"PASS: $($cur.Count) files, all plain alloc/task-create sites allowlisted"
