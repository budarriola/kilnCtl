# checkcache: ok
# check_app_hold_registration.ps1 -- F5HOLDER LOW-1 (docs/audits/REVIEW_F5HOLDER_2026-10-10.md).
#
# SAFETY_FAULT_SRC_APP is one link bit shared by the profile executor and the boot safe-state
# latches. The executor no longer infers a foreign holder: a latch outside drivers/control must call
# profile_executor_note_external_app_hold() before it asserts, or a halt / relay-unknown release
# drops the boot latch. Host tests do not compile main.c / main_control_bringup.c, so this scan is
# the only mechanical guard: every safety_link_set_fault_source() call under firmware/KilnFW/App
# (outside drivers/control and test/) whose mask is not a single non-APP SAFETY_FAULT_SRC_* constant
# and whose assert argument is not literal false must be preceded, within 8 lines, by the
# registration call. Comments are stripped first.
#
# Usage: powershell -File tools\check_app_hold_registration.ps1 [-AppDir <path>]
param([string]$AppDir = (Join-Path $PSScriptRoot '..\firmware\KilnFW\App'))
$ErrorActionPreference = 'Stop'
$AppDir = (Resolve-Path $AppDir).Path
$fail = New-Object System.Collections.Generic.List[string]
$checked = 0
$files = Get-ChildItem -Path $AppDir -Recurse -Filter *.c -File |
    Where-Object { $_.FullName -notmatch '[\\/](test|drivers[\\/]control)[\\/]' }
foreach ($f in $files) {
    $raw = [IO.File]::ReadAllText($f.FullName)
    # blank out comments but keep line structure
    $s = [regex]::Replace($raw, '/\*.*?\*/', { param($m) [regex]::Replace($m.Value, '[^\r\n]', ' ') }, 'Singleline')
    $s = [regex]::Replace($s, '//[^\r\n]*', '')
    $rx = [regex]'safety_link_set_fault_source\s*\(\s*([^,;()]+(?:\([^)]*\))?[^,;()]*)\s*,\s*([^,;]+?)\s*,\s*([^;]+?)\)\s*;'
    foreach ($m in $rx.Matches($s)) {
        $mask = $m.Groups[2].Value.Trim()
        $asrt = $m.Groups[3].Value.Trim()
        if ($asrt -eq 'false') { continue }
        if ($mask -match '^SAFETY_FAULT_SRC_(?!APP\b)\w+$') { continue }
        $checked++
        $line = ($s.Substring(0, $m.Index) -split "`n").Count
        $lines = $s -split "`n"
        $from = [Math]::Max(0, $line - 1 - 8)
        $window = ($lines[$from..($line - 1)]) -join "`n"
        if ($window -notmatch 'profile_executor_note_external_app_hold\s*\(') {
            $fail.Add("$($f.FullName.Substring($AppDir.Length + 1)):${line}: APP-capable fault-source assert ($mask) not preceded by profile_executor_note_external_app_hold()")
        }
    }
}
if ($checked -lt 2) { $fail.Add("only $checked APP-capable assert site(s) found; expected the two boot latches (main.c, main_control_bringup.c) -- scan is broken") }
if ($fail.Count -gt 0) { $fail | ForEach-Object { Write-Host "FAIL: $_" }; exit 1 }
Write-Host "PASS: check_app_hold_registration ($checked APP-capable assert site(s), all registered)"
exit 0
