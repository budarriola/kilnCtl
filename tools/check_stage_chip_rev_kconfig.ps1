# checkcache: ok
# check_stage_chip_rev_kconfig.ps1 -- OF-02 (docs/REVIEW_OAFIX_2026_10_18.md).
# KILNCTL_STAGE_CHIP_REV_CHECK is defined separately in the app and recovery Kconfig. App off + recovery on
# would let the app accept a stage that recovery refuses at apply. Both definitions must exist and have the same default.
# Usage: powershell -File tools\check_stage_chip_rev_kconfig.ps1 [-AppKconfig <p>] [-RecKconfig <p>]
param(
    [string]$AppKconfig = (Join-Path $PSScriptRoot '..\firmware\KilnFW\App\drivers\Kconfig'),
    [string]$RecKconfig = (Join-Path $PSScriptRoot '..\firmware\KilnFW_recovery\main\Kconfig.projbuild')
)
$ErrorActionPreference = 'Stop'
function Get-Default([string]$path) {
    $t = [IO.File]::ReadAllText($path)
    $m = [regex]::Match($t, '(?ms)^\s*config\s+KILNCTL_STAGE_CHIP_REV_CHECK\s*\r?\n(.*?)(?=^\s*(?:config|menu|endmenu|choice|endchoice|menuconfig|comment|if|endif)\b|\z)')
    if (-not $m.Success) { return $null }
    $d = [regex]::Match($m.Groups[1].Value, '(?m)^\s*default\s+(\S+)')
    if (-not $d.Success) { return '' }
    return $d.Groups[1].Value
}
$a = Get-Default $AppKconfig
$r = Get-Default $RecKconfig
if ($null -eq $a -or $null -eq $r) { Write-Host "FAIL: KILNCTL_STAGE_CHIP_REV_CHECK missing (app='$a' recovery='$r')"; exit 1 }
if ($a -eq '' -or $a -ne $r) { Write-Host "FAIL: KILNCTL_STAGE_CHIP_REV_CHECK default differs or is absent (app='$a' recovery='$r')"; exit 1 }
Write-Host "PASS: check_stage_chip_rev_kconfig (both default $a)"
