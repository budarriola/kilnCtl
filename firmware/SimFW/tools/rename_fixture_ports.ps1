# rename_fixture_ports.ps1 -- name the SimFW fixture's two COM ports
# "PiPicoUnitTest" in Windows Device Manager.
#
# MUST RUN ELEVATED. HKLM\SYSTEM\CurrentControlSet\Enum is ACL'd against
# non-admin writes; the script says so and exits rather than half-applying.
#
# WHY THIS EXISTS AT ALL -- the firmware already carries the right names.
# usb_descriptors.c sets Product = "PiPicoUnitTest" and the two CDC interface
# strings to "PiPicoUnitTest Control" / "PiPicoUnitTest Console". Windows
# does not use any of them for a COM port's displayed name: usbser.sys writes
# FriendlyName = "USB Serial Device (COMnn)" when the device is first
# installed, and never revisits it. Worse, Windows caches a composite
# device's strings against its VID/PID/serial at first enumeration, so
# reflashing new descriptors onto a board this PC has already seen changes
# nothing on its own. Linux and macOS read the descriptors directly and show
# the firmware's names with no help from this script.
#
# WHAT IT DOES NOT FIX: pyserial's `interface` and `product` fields stay
# None on this backend regardless -- it reads the registry, not the device.
# kilnsim's auto-detect therefore does NOT depend on this script having run;
# it resolves the protocol port by USB interface number (link.py's
# _usb_interface_number). Running this is a convenience for humans reading
# Device Manager, not a prerequisite for anything.
#
# Re-run it after a fresh Windows install, a different USB port (a new
# device instance gets a fresh FriendlyName), or a driver reinstall.
#
# Usage (from an elevated PowerShell):
#   powershell -ExecutionPolicy Bypass -File firmware\SimFW\tools\rename_fixture_ports.ps1
#   ...-File ... -WhatIf     # show what would change, write nothing
param([switch]$WhatIf)

$ErrorActionPreference = "Stop"

$isAdmin = ([Security.Principal.WindowsPrincipal] `
            [Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $WhatIf) {
    throw ("This script must run elevated -- HKLM\SYSTEM\CurrentControlSet\Enum " +
           "refuses non-admin writes. Re-run it from an Administrator PowerShell, " +
           "or pass -WhatIf to see what it would do.")
}

# The fixture's USB identity (firmware/SimFW/docs/HARDWARE.md section 7).
# PID_F00A is SimFW running; PID_0003 is its ROM bootloader and PID_000C is a
# Debug Probe -- neither is touched here. Interface 0 is the benchproto
# control CDC, interface 2 the console CDC (usb_descriptors.c declares them
# in that order).
$VidPid = "VID_2E8A&PID_F00A"
$InterfaceNames = @{
    0 = "PiPicoUnitTest Control"
    2 = "PiPicoUnitTest Console"
}
$CompositeName = "PiPicoUnitTest"

$enumRoot = "HKLM:\SYSTEM\CurrentControlSet\Enum\USB"
$changed = 0
$seen = 0

foreach ($iface in $InterfaceNames.Keys | Sort-Object) {
    $miKey = Join-Path $enumRoot ("{0}&MI_{1:00}" -f $VidPid, $iface)
    if (-not (Test-Path $miKey)) {
        Write-Host ("  no device key for interface {0} -- fixture not installed on this PC?" -f $iface) -ForegroundColor Yellow
        continue
    }
    foreach ($inst in Get-ChildItem $miKey) {
        $seen++
        $params = Join-Path $inst.PSPath "Device Parameters"
        $port = $null
        if (Test-Path $params) {
            $port = (Get-ItemProperty -Path $params -Name PortName -ErrorAction SilentlyContinue).PortName
        }
        # Keep the COM number in the displayed name: Device Manager shows
        # FriendlyName verbatim, and a name without it is less useful than
        # the generic one it replaces. It goes stale if Windows reassigns
        # the port, which is what re-running this script is for.
        $want = $InterfaceNames[$iface]
        if ($port) { $want = "$want ($port)" }
        $have = (Get-ItemProperty -Path $inst.PSPath -Name FriendlyName -ErrorAction SilentlyContinue).FriendlyName

        if ($have -eq $want) {
            Write-Host ("  already named: {0}" -f $want) -ForegroundColor DarkGray
            continue
        }
        Write-Host ("  {0}: '{1}' -> '{2}'" -f $inst.PSChildName, $have, $want)
        if (-not $WhatIf) {
            Set-ItemProperty -Path $inst.PSPath -Name FriendlyName -Value $want
            $changed++
        }
    }
}

# The composite parent, so the device itself reads as PiPicoUnitTest under
# "Universal Serial Bus devices" rather than "USB Composite Device".
$parentKey = Join-Path $enumRoot $VidPid
if (Test-Path $parentKey) {
    foreach ($inst in Get-ChildItem $parentKey) {
        $seen++
        $have = (Get-ItemProperty -Path $inst.PSPath -Name FriendlyName -ErrorAction SilentlyContinue).FriendlyName
        if ($have -eq $CompositeName) {
            Write-Host ("  already named: {0}" -f $CompositeName) -ForegroundColor DarkGray
            continue
        }
        Write-Host ("  {0}: '{1}' -> '{2}'" -f $inst.PSChildName, $have, $CompositeName)
        if (-not $WhatIf) {
            Set-ItemProperty -Path $inst.PSPath -Name FriendlyName -Value $CompositeName
            $changed++
        }
    }
}

if ($seen -eq 0) {
    throw ("Found no {0} device keys at all. Plug the fixture in at least once " +
           "so Windows installs it, then re-run." -f $VidPid)
}

if ($WhatIf) {
    Write-Host ""
    Write-Host "-WhatIf: nothing was written." -ForegroundColor Cyan
    exit 0
}

Write-Host ""
Write-Host ("Renamed {0} device(s). Unplug and replug the fixture (or refresh " -f $changed) -ForegroundColor Green
Write-Host "Device Manager) to see the new names." -ForegroundColor Green
exit 0
