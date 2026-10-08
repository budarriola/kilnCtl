# checkcache: ok
# check_system_mode_gate_call_sites.ps1 -- docs/SYSTEM_MODE_GATE_PLAN.md section 3.6 slice 6.
# Every mutation handler (or the single helper a handler routes through) on the explicit
# allowlist below must contain a system_mode_gate_check() call. Fails when:
#   - an allowlisted function is missing from its file (rename/delete: cannot go vacuous),
#   - an allowlisted function no longer calls system_mode_gate_check(),
#   - a file under App/drivers outside the allowlist calls system_mode_gate_check()
#     (a new gate site must be added here deliberately).
# Comments are stripped first, so a comment that mentions the call does not count.
# Presence-in-function only; per-function ordering ("gate first") is covered by the handler
# host tests named in the plan.
param([string]$DriversDir)
$ErrorActionPreference = "Stop"
$drivers = if ($DriversDir) { $DriversDir } else { Join-Path $PSScriptRoot "..\firmware\KilnFW\App\drivers" }
$drivers = (Resolve-Path $drivers).Path

# file (relative to drivers) -> functions that must call the gate.
$allow = [ordered]@{
  "http/zones_http_post.c"          = @("zones_post_handler")
  "http/zones_http_pid.c"           = @("zones_pid_post_handler")
  "http/kiln_cfg_http.c"            = @("apply_post_handler")
  "http/backup_import.c"            = @("backup_import_post_handler","backup_import_job_recheck_refused")
  "http/iter_tune_http.c"           = @("iter_tune_restore_post_handler")
  "http/adaptive_tune_http.c"       = @("enable_post_handler","revert_post_handler")
  "http/factory_reset.c"            = @("factory_reset_execute","reset_post_handler")
  "http/cfg_fs_format_http.c"       = @("format_confirm_post_handler")
  "http/ota_http_recovery.c"        = @("ota_recovery_boot_post_handler")
  "http/aux_outputs_http.c"         = @("op_mode_blocked")
  "http/zone_aux_convert_http.c"    = @("op_mode_blocked")
  "http/dashboard_exec_http.c"      = @("*")
  "http/dashboard_autotune_http.c"  = @("*")
  "bridge/uart_bridge_ext_control.c" = @("control_zones_write_refused")
  "owners/kiln_io_owner.c"          = @("system_mode_gate_blocks_relay")
  "update/update_http.c"            = @("mode_gate_refuses","st_claim_begin")
  "update/update_settings_http.c"   = @("settings_post_handler")
  "control/autotune_engine.c"       = @("*")
  "control/autotune_engine_guard.c" = @("autotune_engine_accept")
  "control/profile_executor_run.c"  = @("*")
}

function Strip([string]$t) {
  $t = [regex]::Replace($t, '/\*.*?\*/', { param($m) ([regex]::Matches($m.Value,"`n") | % { "`n" }) -join "" }, 'Singleline')
  [regex]::Replace($t, '//[^\r\n]*', '')
}
function Body([string]$t, [string]$name) {
  $m = [regex]::Match($t, "(?m)^[A-Za-z_][^\n;{}]*\b$([regex]::Escape($name))\s*\([^;{]*?\)\s*\{")
  if (-not $m.Success) { return $null }
  $i = $m.Index + $m.Length; $d = 1
  while ($i -lt $t.Length -and $d -gt 0) { $c = $t[$i]; if ($c -eq '{') { $d++ } elseif ($c -eq '}') { $d-- }; $i++ }
  return $t.Substring($m.Index, $i - $m.Index)
}

$fail = @(); $checked = 0
foreach ($rel in $allow.Keys) {
  $p = Join-Path $drivers $rel
  if (-not (Test-Path $p)) { $fail += "allowlisted file missing: $rel"; continue }
  $t = Strip (Get-Content $p -Raw)
  foreach ($fn in $allow[$rel]) {
    if ($fn -eq "*") {
      if ($t -notmatch 'system_mode_gate_check\s*\(') { $fail += "${rel}: no system_mode_gate_check() call" }
      $checked++; continue
    }
    $b = Body $t $fn
    if ($null -eq $b) { $fail += "${rel}: allowlisted function '$fn' not found (renamed/removed? update the allowlist deliberately)"; continue }
    if ($b -notmatch 'system_mode_gate_check\s*\(') { $fail += "${rel}: '$fn' does not call system_mode_gate_check()" }
    $checked++
  }
}
Get-ChildItem $drivers -Recurse -Filter *.c | % {
  $rel = $_.FullName.Substring($drivers.Length + 1).Replace('\','/')
  if ($rel -eq "safety/system_mode_gate.c" -or $allow.Contains($rel)) { return }
  if ((Strip (Get-Content $_.FullName -Raw)) -match 'system_mode_gate_check\s*\(') { $fail += "${rel}: calls system_mode_gate_check() but is not on the allowlist" }
}
if ($checked -ne 24) { $fail += "checked $checked allowlist entries, expected exactly 24: update the count deliberately when the allowlist changes" }
if ($fail) { $fail | % { Write-Host "FAIL: $_" }; exit 1 }
Write-Host "PASS: $checked gated handlers/helpers call system_mode_gate_check()"
