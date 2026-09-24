<#
.SYNOPSIS
  Start, stop, or report status of the bench Wi-Fi hotspot (Windows Mobile Hotspot /
  Wi-Fi Direct tethering) used to give the ESP32-S3 kiln controller board a second,
  isolated 2.4 GHz AP for AP-fallback testing.

.DESCRIPTION
  Credentials are never passed on the command line or printed. SSID and password come
  only from the User-scope environment variables KILNCTL_HOTSPOT_SSID and
  KILNCTL_HOTSPOT_PASSWORD (see docs/BENCH_HOTSPOT.md). This script never echoes,
  logs, or writes the password anywhere.

.PARAMETER Action
  One of: start, stop, status

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\PcTools\scripts\bench_hotspot.ps1 -Action start
#>
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('start', 'stop', 'status')]
    [string]$Action
)

function Get-TetheringManager {
    [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager,Windows.Networking.NetworkOperators,ContentType=WindowsRuntime] | Out-Null
    [Windows.Networking.Connectivity.NetworkInformation,Windows.Networking.Connectivity,ContentType=WindowsRuntime] | Out-Null
    $profile = [Windows.Networking.Connectivity.NetworkInformation]::GetInternetConnectionProfile()
    if ($null -eq $profile) { throw "GetInternetConnectionProfile returned null -- no active internet connection profile to tether from" }
    return [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]::CreateFromConnectionProfile($profile)
}

function Wait-WinRtAction($WinRtAction) {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null
    $asTaskVoid = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and -not $_.IsGenericMethod
    })[0]
    $netTask = $asTaskVoid.Invoke($null, @($WinRtAction))
    $netTask.Wait(-1) | Out-Null
}

function Wait-WinRtOperation($WinRtOperation, $ResultType) {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null
    $asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.IsGenericMethod
    })[0]
    $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
    $netTask = $asTask.Invoke($null, @($WinRtOperation))
    $netTask.Wait(-1) | Out-Null
    return $netTask.Result
}

switch ($Action) {
    'start' {
        $ssid = [Environment]::GetEnvironmentVariable('KILNCTL_HOTSPOT_SSID', 'User')
        $pw = [Environment]::GetEnvironmentVariable('KILNCTL_HOTSPOT_PASSWORD', 'User')
        if ([string]::IsNullOrEmpty($ssid) -or [string]::IsNullOrEmpty($pw)) {
            Write-Error "KILNCTL_HOTSPOT_SSID / KILNCTL_HOTSPOT_PASSWORD are not both set in User scope."
            exit 1
        }
        $tm = Get-TetheringManager
        $config = New-Object Windows.Networking.NetworkOperators.NetworkOperatorTetheringAccessPointConfiguration
        $config.Ssid = $ssid
        $config.Passphrase = $pw
        $config.Band = [Windows.Networking.NetworkOperators.TetheringWiFiBand]::TwoPointFourGigahertz
        Wait-WinRtAction ($tm.ConfigureAccessPointAsync($config))
        $result = Wait-WinRtOperation ($tm.StartTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
        Write-Output "start: Status=$($result.Status) TetheringOperationalState=$($tm.TetheringOperationalState)"
    }
    'stop' {
        $tm = Get-TetheringManager
        $result = Wait-WinRtOperation ($tm.StopTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
        Write-Output "stop: Status=$($result.Status) TetheringOperationalState=$($tm.TetheringOperationalState)"
    }
    'status' {
        $tm = Get-TetheringManager
        $ssidSet = -not [string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable('KILNCTL_HOTSPOT_SSID', 'User'))
        $pwSet = -not [string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable('KILNCTL_HOTSPOT_PASSWORD', 'User'))
        Write-Output "TetheringOperationalState=$($tm.TetheringOperationalState)"
        Write-Output "ssid_env_set=$ssidSet password_env_set=$pwSet"
        Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
            Where-Object { $_.InterfaceAlias -like '*Local Area Connection*' } |
            Format-Table InterfaceAlias, IPAddress, PrefixLength -AutoSize
    }
}
