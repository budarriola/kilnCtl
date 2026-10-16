# checkcache: ok
# check_psram_task_stacks.ps1 -- keeps the task stacks moved to PSRAM (2026-10-14,
# docs/audits/AUDIT_INTERNAL_RAM_XIP.md "Resolution") in PSRAM.
# WHY: with CONFIG_SPIRAM_XIP_FROM_PSRAM, flash erase/write no longer freezes the cache, so a
# task that only reads/writes NVS, LittleFS or esp_partition_write may live on a PSRAM stack.
# mmap-class calls (esp_image_*, esp_ota_end/set_boot_partition, esp_partition_mmap, core dump
# summary) still freeze it and assert an internal caller stack, so those tasks stay internal
# (see the audit). This check pins the ones that were moved: each must be created by an
# xTaskCreate*WithCaps call whose argument list names MALLOC_CAP_SPIRAM, never plain
# xTaskCreate*; and XIP must still be on in sdkconfig.defaults. If a moved task grows a call
# to an mmap-class API, move it back AND edit its entry here with the reason.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# file (repo-relative) -> entry point function / task name literal, as it appears in the call
$tasks = @(
    @('firmware/KilnFW/App/drivers/control/profile_executor_start.c', 'executor_task_entry'),
    @('firmware/KilnFW/App/drivers/control/profile_executor_start.c', 'watchdog_task_entry'),
    @('firmware/KilnFW/App/drivers/persist/kiln_cfg_swap_worker.c', 'swap_worker_task'),
    @('firmware/KilnFW/App/drivers/ui/screen_idle.c', 'screen_idle_task'),
    @('firmware/KilnFW/App/drivers/net/wifi_prov.c', 'owner_task'),
    @('firmware/KilnFW/App/drivers/safety/danger_mode.c', 'danger_mode_task'),
    @('firmware/KilnFW/App/drivers/bridge/uart_bridge_system.c', 'system_bridge_task'),
    @('firmware/hwAbstraction/esp/uart/hal_uart_esp.c', 'hal_uart_esp_event_task')
)

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

# Returns the argument text of every xTaskCreate*( call (balanced parens), keyed by variant name.
function Get-CreateCalls([string]$t) {
    $res = @()
    foreach ($m in [regex]::Matches($t, '(?<![A-Za-z0-9_])(xTaskCreate\w*)\s*\(')) {
        $depth = 1; $j = $m.Index + $m.Length
        while ($j -lt $t.Length -and $depth -gt 0) {
            if ($t[$j] -eq '(') { $depth++ } elseif ($t[$j] -eq ')') { $depth-- }
            $j++
        }
        $res += [pscustomobject]@{ Fn = $m.Groups[1].Value; Args = $t.Substring($m.Index + $m.Length, $j - $m.Index - $m.Length - 1) }
    }
    $res
}

$bad = @()
foreach ($e in $tasks) {
    $path = Join-Path $root $e[0]
    if (-not (Test-Path $path)) { $bad += "$($e[0]) missing"; continue }
    $calls = Get-CreateCalls (Strip-C ([IO.File]::ReadAllText($path))) |
        Where-Object { $_.Args -match ('(?<![A-Za-z0-9_])' + [regex]::Escape($e[1]) + '(?![A-Za-z0-9_])') }
    if (-not $calls) { $bad += "$($e[0]): no xTaskCreate* call for $($e[1]) found"; continue }
    foreach ($c in $calls) {
        if ($c.Fn -notmatch 'WithCaps$') { $bad += "$($e[0]): $($e[1]) created with plain $($c.Fn) (internal stack); must be xTaskCreate*WithCaps + MALLOC_CAP_SPIRAM" }
        elseif ($c.Args -notmatch 'MALLOC_CAP_SPIRAM') { $bad += "$($e[0]): $($e[1]) $($c.Fn) call lacks MALLOC_CAP_SPIRAM" }
        elseif ($c.Args -match 'MALLOC_CAP_INTERNAL') { $bad += "$($e[0]): $($e[1]) $($c.Fn) call requests MALLOC_CAP_INTERNAL" }
    }
}
# Tasks that reach guarded NVS/cfg writes (hal_kv_write_safe_here() refuses from a PSRAM stack) and so
# must keep an INTERNAL stack: plain xTaskCreate*, never WithCaps.
$mustBeInternal = @(
    ,@('firmware/KilnFW/App/drivers/http/http_async_job.c', 'http_async_job_task')
)
foreach ($e in $mustBeInternal) {
    $path = Join-Path $root $e[0]
    $calls = Get-CreateCalls (Strip-C ([IO.File]::ReadAllText($path))) |
        Where-Object { $_.Args -match ('(?<![A-Za-z0-9_])' + [regex]::Escape($e[1]) + '(?![A-Za-z0-9_])') }
    if (-not $calls) { $bad += "$($e[0]): no xTaskCreate* call for $($e[1]) found"; continue }
    foreach ($c in $calls) {
        if ($c.Fn -match 'WithCaps$' -or $c.Args -match 'MALLOC_CAP_SPIRAM') { $bad += "$($e[0]): $($e[1]) must keep an internal stack (writes NVS; guards refuse a PSRAM stack) but uses $($c.Fn)" }
    }
}
$uart = [IO.File]::ReadAllText((Join-Path $root 'firmware/hwAbstraction/esp/uart/hal_uart_esp.c'))
$uart = [regex]::Replace($uart, '(?s)/\*.*?\*/|//[^\r\n]*', '')
if ($uart -notmatch 'vTaskDeleteWithCaps\s*\(' -or $uart -match '\bvTaskDelete\s*\(') { $bad += 'hal_uart_esp.c: WithCaps task must be deleted with vTaskDeleteWithCaps' }
# LOW-2 (REVIEW_RAM_PSRAM_STACKS): a task created WithCaps must be deleted with vTaskDeleteWithCaps
# (self-delete is supported in IDF 6.0.2: it spawns a cleanup task). A plain vTaskDelete frees neither
# the TCB nor the PSRAM stack, leaking internal RAM. Any file that creates a task with
# xTaskCreate*WithCaps may not call plain vTaskDelete( at all, except the allowlisted files below
# whose plain vTaskDelete deletes a different, plain-xTaskCreate task.
$plainDeleteOk = @{
    'firmware/KilnFW/App/drivers/update/update_fetch.c' = 'wr_task is created with plain xTaskCreatePinnedToCore'
}
$scanFiles = @('firmware/KilnFW/App', 'firmware/hwAbstraction', 'firmware/CommonFW') | ForEach-Object { Get-ChildItem -Path (Join-Path $root $_) -Recurse -Include *.c -File } |
    Where-Object { $_.FullName -notmatch "[/\\](test|build)[/\\]" }
foreach ($f in $scanFiles) {
    $raw = [IO.File]::ReadAllText($f.FullName)
    $code = Strip-C $raw
    if ($code -notmatch 'xTaskCreate\w*WithCaps') { continue }
    $rel = $f.FullName.Substring($root.Length + 1).Replace('\', '/')
    if ($plainDeleteOk.ContainsKey($rel)) { continue }
    if ($code -match '(?<![A-Za-z0-9_])vTaskDelete\s*\(') { $bad += "${rel}: creates a task with xTaskCreate*WithCaps but calls plain vTaskDelete(; use vTaskDeleteWithCaps" }
}
$defs = [IO.File]::ReadAllText((Join-Path $root 'firmware/KilnFW/sdkconfig.defaults'))
if ($defs -notmatch '(?m)^CONFIG_SPIRAM_XIP_FROM_PSRAM=y') { $bad += 'sdkconfig.defaults: CONFIG_SPIRAM_XIP_FROM_PSRAM=y missing; PSRAM task stacks are only safe around flash writes with XIP on' }
if ($bad.Count) { $bad | ForEach-Object { "FAIL: $_" }; exit 1 }
"PASS: $($tasks.Count) moved tasks create their stacks in PSRAM; XIP on"
