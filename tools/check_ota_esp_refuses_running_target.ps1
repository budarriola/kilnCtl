# check_ota_esp_refuses_running_target.ps1 -- source guard for the application
# image's POST /api/ota/esp handler (firmware/KilnFW/App/drivers/http/ota_http_esp.c).
#
# WHY. Single-slot table (partitions.csv: `app` ota_0 + factory `recovery`):
# esp_ota_get_next_update_partition(NULL) returns the RUNNING app partition, and
# esp_ota_begin() on it fails with PARTITION_CONFLICT (bench 2026-10-03, followed
# by a TASK_WDT while httpd drained the unread body). The handler must compare
# the target against esp_ota_get_running_partition() (via the host-tested
# ota_http_esp_target_usable()) BEFORE esp_ota_begin(), and every ESP/Pico early
# refusal must return ESP_FAIL so httpd closes instead of draining the body.
# The handler is target-only, so a host test cannot reach it; this greps it.
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $repoRoot "firmware/KilnFW/App/drivers/http"
$esp = Get-Content -Raw (Join-Path $dir "ota_http_esp.c")
$pico = Get-Content -Raw (Join-Path $dir "ota_http_pico.c")
$fail = @()

$usable = $esp.IndexOf("ota_http_esp_target_usable(target, esp_ota_get_running_partition())")
$begin = $esp.IndexOf("esp_ota_begin(target")
if ($usable -lt 0) { $fail += "ota_http_esp.c no longer compares the update target against esp_ota_get_running_partition()" }
if ($begin -lt 0) { $fail += "ota_http_esp.c: esp_ota_begin(target...) call not found -- has the handler moved?" }
if ($usable -ge 0 -and $begin -ge 0 -and $usable -gt $begin) { $fail += "ota_http_esp.c: the running-partition check must come BEFORE esp_ota_begin()" }

# Early refusals must not return ESP_OK with an unread body.
foreach ($pair in @(@("ota_http_esp.c", $esp), @("ota_http_pico.c", $pico))) {
    $name = $pair[0]; $src = $pair[1]
    $m = [regex]::Match($src, "esp_err_t\s+ota_(esp|pico)_post_handler\s*\(.*?\n\}", "Singleline")
    if (-not $m.Success) { $fail += "${name}: POST update handler not found"; continue }
    if ($m.Value -match "return\s+ota_http_send_interlock_refusal") { $fail += "${name}: POST update handler returns the interlock refusal result (ESP_OK) -- must return ESP_FAIL so the body is not drained" }
    if ($m.Value -match "already in progress[^\n]*\n\s*return ESP_OK") { $fail += "${name}: 'update already in progress' refusal returns ESP_OK (body would be drained)" }
    if ($m.Value -notmatch "ESP_FAIL") { $fail += "${name}: POST update handler never returns ESP_FAIL" }
}

if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FAIL: $_" }
    throw "check_ota_esp_refuses_running_target: $($fail.Count) problem(s)"
}
Write-Host "check_ota_esp_refuses_running_target: OK"
exit 0
