# checkcache: ok
# check_app_hold_registration.ps1 -- F5HOLDER LOW-1 (docs/audits/REVIEW_F5HOLDER_2026-10-10.md).
#
# SAFETY_FAULT_SRC_APP is one link bit shared by the profile executor and the boot safe-state
# latches. The executor no longer infers a foreign holder: a latch outside drivers/control must call
# profile_executor_note_external_app_hold() before it asserts, or a halt / relay-unknown release
# drops the boot latch. Host tests do not compile main.c / main_control_bringup.c, so this scan is
# the only mechanical guard: every safety_link_set_fault_source() call under firmware/KilnFW/App
# (outside test/ and the profile_executor*.c files; F5HOLDERFX LOW-3: the rest of drivers/control, e.g. autotune_engine.c, IS scanned) whose mask is not a single non-APP SAFETY_FAULT_SRC_* constant
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
    Where-Object { $_.FullName -notmatch '[\\/]test[\\/]' -and $_.Name -notmatch '^profile_executor' }
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
        if ($mask -match '^[A-Za-z_]\w*$') {
            # F5HOLDERFX2 LOW-2 / F5HOLDERFX3 LOW-3: a local variable is accepted only when EVERY use of it
            # inside the enclosing function is safe: each assignment is plain "=" or "|=" (no +=, ^=, &=, ++,
            # ...), no "&name" (address passed on), and each right-hand side is, after removing
            # parenthesised "x == NAMED_CONSTANT" conditions, made ONLY of literal non-APP
            # SAFETY_FAULT_SRC_* names and ? : | ( ). A bare variable/field operand, a macro, a call or
            # anything else falls through to the registration requirement (fail loud).
            $fnStart = $s.LastIndexOf("`n{", $m.Index)
            if ($fnStart -lt 0) { $fnStart = 0 }
            $fnEnd = $s.IndexOf("`n}", $m.Index)
            if ($fnEnd -lt 0) { $fnEnd = $s.Length }
            $body = $s.Substring($fnStart, $fnEnd - $fnStart)
            $esc = [regex]::Escape($mask)
            $arx = [regex]('(?<![\w.>])' + $esc + '\s*(\|=|\+=|-=|\^=|&=|\*=|/=|%=|<<=|>>=|=(?!=)|\+\+|--)\s*([^;]*);')
            $assigns = $arx.Matches($body)
            $allOk = ($assigns.Count -gt 0)
            if ([regex]::IsMatch($body, '(?<!&)&\s*' + $esc + '\b') -or [regex]::IsMatch($body, '(\+\+|--)\s*' + $esc + '\b')) { $allOk = $false }
            foreach ($am in $assigns) {
                if ($am.Groups[1].Value -ne '=' -and $am.Groups[1].Value -ne '|=') { $allOk = $false; continue }
                $rhs = $am.Groups[2].Value
                $rest = [regex]::Replace($rhs, '\(\s*[\w.>\-\[\]]+\s*(==|!=)\s*[A-Z][A-Z0-9_]*\s*\)', ' ')
                $rest = [regex]::Replace($rest, '\bSAFETY_FAULT_SRC_(?!APP\b)[A-Z0-9_]+\b', ' ')
                if ($rhs -notmatch 'SAFETY_FAULT_SRC_(?!APP\b)\w+' -or $rest -notmatch '^[\s?:|()]*$') { $allOk = $false }
            }
            if ($allOk) { continue }
        }
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
