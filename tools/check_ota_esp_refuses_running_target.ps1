# check_ota_esp_refuses_running_target.ps1 -- source guard for the application
# image's POST /api/ota/esp handler (firmware/KilnFW/App/drivers/http/ota_http_esp.c).
#
# WHY. Single-slot table (partitions.csv: `app` ota_0 + factory `recovery`):
# esp_ota_get_next_update_partition(NULL) returns the RUNNING app partition, and
# esp_ota_begin() on it fails with PARTITION_CONFLICT (bench 2026-10-03, followed
# by a TASK_WDT while httpd purged the unread body at 32 B per read). The handler
# must compare the target against esp_ota_get_running_partition() (via the
# host-tested ota_http_esp_target_usable()) BEFORE esp_ota_begin(). Every ESP/Pico
# refusal after the response is sent must go through ota_http_refusal_drain()
# (bounded drain, ESP_FAIL only if the drain fails -- a bare close with unread
# data makes lwIP RST and the client loses the status), and a FAILED transfer
# (do_transfer returns true) must reach that drain while success returns ESP_OK.
# The handlers are target-only, so a host test cannot reach them; this greps the
# source with comments stripped (a comment must never satisfy a requirement).
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$dir = Join-Path $repoRoot "firmware/KilnFW/App/drivers/http"

function Strip-Comments([string]$t) {
    $t = [regex]::Replace($t, "/\*.*?\*/", "", "Singleline")
    return [regex]::Replace($t, "//[^\n]*", "")
}
$esp = Strip-Comments (Get-Content -Raw (Join-Path $dir "ota_http_esp.c"))
$pico = Strip-Comments (Get-Content -Raw (Join-Path $dir "ota_http_pico.c"))
$fail = @()

# The guard must be the WHOLE condition of an if-block that refuses (goto cleanup) -- a bare substring match
# was satisfied by "if (0 && !ota_http_esp_target_usable(...))" (vacuity audit 2026-10-07).
$um = [regex]::Match($esp, "if\s*\(\s*!\s*ota_http_esp_target_usable\(target,\s*esp_ota_get_running_partition\(\)\)\s*\)\s*\{[^{}]*goto\s+cleanup\s*;\s*\}")
$usable = if ($um.Success) { $um.Index } else { -1 }
$begin = $esp.IndexOf("esp_ota_begin(target")
if ($usable -lt 0) { $fail += "ota_http_esp.c no longer refuses (if (!ota_http_esp_target_usable(target, esp_ota_get_running_partition())) { ... goto cleanup; }) when the update target is the running partition" }
if ($begin -lt 0) { $fail += "ota_http_esp.c: esp_ota_begin(target...) call not found -- has the handler moved?" }
if ($usable -ge 0 -and $begin -ge 0 -and $usable -gt $begin) { $fail += "ota_http_esp.c: the running-partition check must come BEFORE esp_ota_begin()" }

$specs = @(
    @{ File = "ota_http_esp.c";  Src = $esp;  Kind = "esp";  Do = "ota_esp_do_transfer";  Ret = "return !ok;";           Buf = "s_ota_esp_chunk" },
    @{ File = "ota_http_pico.c"; Src = $pico; Kind = "pico"; Do = "ota_pico_do_stage";    Ret = "return !started_relay;"; Buf = "s_ota_pico_chunk" }
)
foreach ($sp in $specs) {
    $name = $sp.File; $src = $sp.Src
    # 1. do_transfer/do_stage reports FAILURE as true.
    $fm = [regex]::Match($src, "static\s+bool\s+$($sp.Do)\s*\(.*?\n\}", "Singleline")
    if (-not $fm.Success) { $fail += "${name}: $($sp.Do)() not found"; }
    elseif ($fm.Value -notmatch [regex]::Escape($sp.Ret)) { $fail += "${name}: $($sp.Do)() no longer ends in '$($sp.Ret)' (failed transfer must map to true)" }
    # 2. POST handler.
    $m = [regex]::Match($src, "esp_err_t\s+ota_$($sp.Kind)_post_handler\s*\(.*?\n\}", "Singleline")
    if (-not $m.Success) { $fail += "${name}: POST update handler not found"; continue }
    $h = $m.Value
    if ($h -match "return\s+ota_http_send_interlock_refusal") { $fail += "${name}: handler returns the interlock refusal result (ESP_OK) -- body would be purged at 32 B/read" }
    if ($h -match "already in progress[^\n]*\n\s*return ESP_OK") { $fail += "${name}: 'already in progress' refusal returns ESP_OK" }
    if ($h -match "return\s+ESP_FAIL\s*;") { $fail += "${name}: handler has a bare 'return ESP_FAIL;' -- refusals must drain via ota_http_refusal_drain() (bare close RSTs the client)" }
    $drain = [regex]::Matches($h, "return\s+ota_http_refusal_drain\(req,\s*$($sp.Buf),\s*sizeof\($($sp.Buf)\)\)\s*;").Count
    if ($drain -lt 3) { $fail += "${name}: expected >= 3 'return ota_http_refusal_drain(req, $($sp.Buf), sizeof(...))' (interlock, in-progress, failed transfer), found $drain" }
    if ($h -notmatch "if\s*\(\s*$($sp.Do)\(req,\s*ip\)\s*\)\s*\{\s*return\s+ota_http_refusal_drain\([^;]*;\s*\}\s*return\s+ESP_OK\s*;") {
        $fail += "${name}: handler tail must be 'if ($($sp.Do)(req, ip)) { return ota_http_refusal_drain(...); } return ESP_OK;'"
    }
}

if ($fail.Count -gt 0) {
    $fail | ForEach-Object { Write-Host "FAIL: $_" }
    throw "check_ota_esp_refuses_running_target: $($fail.Count) problem(s)"
}
Write-Host "check_ota_esp_refuses_running_target: OK"
exit 0
