# checkcache: ok
# check_system_mode_gate_call_sites.ps1 -- docs/SYSTEM_MODE_GATE.md section 3.6 slice 6.
# Every mutation handler (or the single helper a handler routes through) on the explicit
# allowlist below must contain a system_mode_gate_check() call. Fails when:
#   - an allowlisted function is missing from its file (rename/delete: cannot go vacuous),
#   - an allowlisted function no longer calls system_mode_gate_check(),
#   - a file under App/drivers outside the allowlist calls system_mode_gate_check()
#     (a new gate site must be added here deliberately).
# Comments and string/char literal contents are blanked first. The gate result must be consumed
# (if/return/tested assignment); (void) calls and if (0) blocks do not count. Helper entries also
# assert the helper is still wired (see $wiring).
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
  "http/dashboard_exec_http.c"      = @("profile_exec_start_post_handler")
  "http/dashboard_autotune_http.c"  = @("autotune_start_post_handler")
  "bridge/uart_bridge_ext_control.c" = @("control_zones_write_refused")
  "owners/kiln_io_owner.c"          = @("system_mode_gate_blocks_relay")
  "update/update_http.c"            = @("mode_gate_refuses","st_claim_begin")
  "update/update_settings_http.c"   = @("settings_post_handler")
  "control/autotune_engine.c"       = @("autotune_begin_run_locked")
  "control/autotune_engine_guard.c" = @("autotune_engine_accept")
  "control/profile_executor_run.c"  = @("profile_executor_run")
}

# Tokenizer: blanks comments (newlines kept) and the CONTENTS of string/char literals (quotes kept),
# so "/api/*" or '{' can neither open a fake comment nor unbalance brace counting.
function Strip([string]$t) {
  $sb = New-Object System.Text.StringBuilder
  $i = 0; $n = $t.Length
  while ($i -lt $n) {
    $c = $t[$i]; $d = if ($i + 1 -lt $n) { $t[$i + 1] } else { [char]0 }
    if ($c -eq '/' -and $d -eq '*') {
      $i += 2
      while ($i -lt $n -and -not ($t[$i] -eq '*' -and $i + 1 -lt $n -and $t[$i + 1] -eq '/')) { if ($t[$i] -eq "`n") { [void]$sb.Append("`n") }; $i++ }
      $i += 2; [void]$sb.Append(' ')
    } elseif ($c -eq '/' -and $d -eq '/') {
      while ($i -lt $n -and $t[$i] -ne "`n") { $i++ }
    } elseif ($c -eq '"' -or $c -eq "'") {
      [void]$sb.Append($c); $i++
      while ($i -lt $n -and $t[$i] -ne $c -and $t[$i] -ne "`n") { if ($t[$i] -eq [char]92) { $i++ }; $i++ }
      [void]$sb.Append($c); $i++
    } else { [void]$sb.Append($c); $i++ }
  }
  $sb.ToString()
}
function MatchEnd([string]$t, [int]$open) {   # $open = index of '{'; returns index after matching '}'
  $i = $open + 1; $d = 1
  while ($i -lt $t.Length -and $d -gt 0) { $c = $t[$i]; if ($c -eq '{') { $d++ } elseif ($c -eq '}') { $d-- }; $i++ }
  $i
}
function Body([string]$t, [string]$name) {
  $m = [regex]::Match($t, "(?m)^[A-Za-z_][^\n;{}]*\b$([regex]::Escape($name))\s*\([^;{]*?\)\s*\{")
  if (-not $m.Success) { return $null }
  $e = MatchEnd $t ($m.Index + $m.Length - 1)
  return $t.Substring($m.Index, $e - $m.Index)
}
# Remove dead if (0)/if (false) statements so a gate inside them does not count.
function DropDead([string]$b) {
  while ($true) {
    $m = [regex]::Match($b, 'bif\s*\(\s*(0|false)\s*\)\s*')
    if (-not $m.Success) { return $b }
    $j = $m.Index + $m.Length
    if ($j -lt $b.Length -and $b[$j] -eq '{') { $e = MatchEnd $b $j }
    else { $semi = $b.IndexOf(';', $j); $e = if ($semi -lt 0) { $b.Length } else { $semi + 1 } }
    $b = $b.Substring(0, $m.Index) + ' ' + $b.Substring($e)
  }
}
# True when some gate call's result is consumed: if (...gate), return gate, or var = gate tested in an if.
function GateConsumed([string]$b) {
  $b = DropDead $b
  if ($b -match 'if\s*\(\s*!?\s*system_mode_gate_check\s*\(') { return $true }
  if ($b -match 'return\s+system_mode_gate_check\s*\(') { return $true }
  foreach ($m in [regex]::Matches($b, 'b([A-Za-z_]\w*)\s*=\s*system_mode_gate_check\s*\(')) {
    $v = [regex]::Escape($m.Groups[1].Value)
    if ($b.Substring($m.Index + $m.Length) -match "if\s*\([^)]*b$vb") { return $true }
  }
  return $false
}
# Helper wiring: file -> (regex, minimum count) that must still hold in comment-stripped source.
$wiring = @(
  @("http/aux_outputs_http.c",         '\.mode_blocked\s*=\s*op_mode_blocked\b', 1),
  @("http/zone_aux_convert_http.c",    '\.mode_blocked\s*=\s*op_mode_blocked\b', 1),
  @("owners/kiln_io_owner.c",          'if\s*\(\s*system_mode_gate_blocks_relay\s*\(\s*\)', 2),
  @("bridge/uart_bridge_ext_control.c", 'if\s*\(\s*control_zones_write_refused\s*\(', 2),
  @("update/update_http.c",            '\bmode_gate_refuses\s*\(\s*req\b', 3)
)

$fail = @(); $checked = 0
foreach ($rel in $allow.Keys) {
  $p = Join-Path $drivers $rel
  if (-not (Test-Path $p)) { $fail += "allowlisted file missing: $rel"; continue }
  $t = Strip (Get-Content $p -Raw)
  foreach ($fn in $allow[$rel]) {
    $b = Body $t $fn
    if ($null -eq $b) { $fail += "${rel}: allowlisted function '$fn' not found (renamed/removed? update the allowlist deliberately)"; continue }
    if (-not (GateConsumed $b)) { $fail += "${rel}: '$fn' does not consume a live system_mode_gate_check() result (if/return/tested assignment; (void) and if (0) do not count)" }
    $checked++
  }
}
foreach ($w in $wiring) {
  $p = Join-Path $drivers $w[0]
  $cnt = if (Test-Path $p) { [regex]::Matches((Strip (Get-Content $p -Raw)), $w[1]).Count } else { 0 }
  if ($cnt -lt $w[2]) { $fail += "$($w[0]): helper wiring '$($w[1])' found $cnt time(s), need >= $($w[2]) (helper no longer wired?)" }
}
Get-ChildItem $drivers -Recurse -Filter *.c | % {
  $rel = $_.FullName.Substring($drivers.Length + 1).Replace('\','/')
  if ($rel -eq "safety/system_mode_gate.c" -or $allow.Contains($rel)) { return }
  if ((Strip (Get-Content $_.FullName -Raw)) -match 'system_mode_gate_check\s*\(') { $fail += "${rel}: calls system_mode_gate_check() but is not on the allowlist" }
}
if ($checked -ne 24) { $fail += "checked $checked allowlist entries, expected exactly 24: update the count deliberately when the allowlist changes" }
if ($fail) { $fail | % { Write-Host "FAIL: $_" }; exit 1 }
Write-Host "PASS: $checked gated handlers/helpers call system_mode_gate_check()"
