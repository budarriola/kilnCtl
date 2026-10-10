# checkcache: ok
# check_touch_cal_exit_target_caller.ps1 -- lcd_touch_cal_saved_exit_target() may be called
# ONLY from ui_page_touch_cal.c, exactly once (owner decision 2026-10-10 A1: it gives a
# role-free board access to touch_test after a successful touch calibration). The rule
# was a comment in lcd_auth_state.h; this makes it mechanical. A second caller anywhere in
# firmware/KilnFW (outside App/test/ and build/) fails.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$app = Join-Path $root 'firmware\KilnFW'
if (-not (Test-Path $app)) { throw "check_touch_cal_exit_target_caller: $app not found; check is blind." }
$fn = 'lcd_touch_cal_saved_exit_target'
$allowedFile = 'firmware/KilnFW/App/drivers/ui/ui_page_touch_cal.c'
$files = Get-ChildItem -Path $app -Recurse -File -Include '*.c', '*.h', '*.cpp' |
    Where-Object { $_.FullName -notmatch '[\\/](test|build)[\\/]' }
if ($files.Count -lt 50) { throw "only $($files.Count) source files scanned; check is blind." }
$viol = @(); $allowedCalls = 0; $defs = 0
foreach ($f in $files) {
    $t = [IO.File]::ReadAllText($f.FullName)
    $t = [regex]::Replace($t, '/\*.*?\*/', { param($m) ($m.Value -replace '[^\n]', '') }, 'Singleline')
    $t = [regex]::Replace($t, '//[^\n]*', '')
    $rel = $f.FullName.Substring($root.Length + 1) -replace '\\', '/'
    $lines = $t -split "`n"
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -notmatch "\b$fn\s*\(") { continue }
        # declaration/definition: the line begins with the return type, not a call expression
        if ($lines[$i] -match "^\s*const\s+char\s*\*\s*$fn\s*\(") { $defs++; continue }
        if ($rel -eq $allowedFile) { $allowedCalls++; continue }
        $viol += "${rel}:$($i + 1): call to $fn() outside $allowedFile"
    }
}
if ($defs -lt 2) { throw "check_touch_cal_exit_target_caller: declaration+definition of $fn not found ($defs); renamed? check is blind." }
if ($allowedCalls -ne 1) { $viol += "$allowedFile has $allowedCalls call(s) of $fn(), want exactly 1" }
if ($viol.Count) {
    $viol | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    throw "$($viol.Count) violation(s): $fn() grants role-free touch_test access and may only be called by finish_calibration() in ui_page_touch_cal.c"
}
Write-Host "touch_cal exit-target caller check passed: $fn() has exactly one caller in $allowedFile ($($files.Count) files scanned)."
exit 0
