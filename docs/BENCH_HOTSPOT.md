# Bench Wi-Fi hotspot for AP-fallback testing

This PC can host a secured, isolated 2.4 GHz Wi-Fi network for testing the ESP32-S3 kiln
controller board when it needs a second AP that is not the lab's regular network — for
example, exercising the board's AP-fallback / provisioning path without touching the
production Wi-Fi.

The PC's Intel AX200 adapter does not support the older `netsh wlan` hosted-network API on
this driver, so the hotspot is implemented with Windows Mobile Hotspot (Wi-Fi Direct
tethering) over the WinRT `NetworkOperatorTetheringManager` API, driven from PowerShell.
The uplink is the PC's wired Ethernet connection; the hotspot itself is broadcast on
2.4 GHz (the ESP32-S3 only supports 2.4 GHz), secured with WPA2.

## Credentials

The SSID and password are **not** checked into the repo. They live only in User-scope
environment variables on this machine:

- `KILNCTL_HOTSPOT_SSID` — the network name
- `KILNCTL_HOTSPOT_PASSWORD` — a randomly generated WPA2 passphrase (16+ characters)

Any tool or script that needs to join the board to this network reads these two variables
directly; nothing prints, logs, or writes the password value anywhere.

## Starting, stopping, checking status

Use `tools/PcTools/scripts/bench_hotspot.ps1`:

```powershell
powershell -ExecutionPolicy Bypass -File tools\PcTools\scripts\bench_hotspot.ps1 -Action start
powershell -ExecutionPolicy Bypass -File tools\PcTools\scripts\bench_hotspot.ps1 -Action status
powershell -ExecutionPolicy Bypass -File tools\PcTools\scripts\bench_hotspot.ps1 -Action stop
```

`status` reports `TetheringOperationalState` (On/Off), whether the two credential env vars
are set (booleans only), `auto_off_disabled` ([bool], see below), and the IPv4 address bound
to the "Microsoft Wi-Fi Direct Virtual Adapter" (the hotspot's own gateway address, typically
`192.168.137.1/24`).

## Idle auto-off

Windows' Mobile Hotspot has a power-saving feature that turns the hotspot off after roughly
ten minutes with no client connected — confirmed on this machine (first hotspot session,
2026-09-23, turned itself `Off` unattended within about ten minutes of `start` with no
device joined). The `NetworkOperatorTetheringManager` WinRT API this script drives has no
software-facing knob for it: on this Windows build (11 Pro 26200) the manager exposes only
`ClientCount`, `MaxClientCount`, and `TetheringOperationalState` — no `PowerSavingEnabled` or
`IsNoConnectionsTimeoutEnabled` property exists to flip from PowerShell. The only known knob
is a registry value read by the `icssvc` service:
`HKLM:\SYSTEM\ControlSet001\Services\icssvc\Settings\PeerlessTimeoutEnabled` (DWORD `0`
disables the timeout), and writing under `HKLM` requires admin rights.

`bench_hotspot.ps1 -Action start` now tries this write itself, and restarts `icssvc` if it
succeeds. From a non-admin session (the normal case), the write is denied and the script
prints the exact one-time elevated command to run instead:

```powershell
New-ItemProperty -Path 'HKLM:\SYSTEM\ControlSet001\Services\icssvc\Settings' -Name PeerlessTimeoutEnabled -PropertyType DWord -Value 0 -Force; Restart-Service icssvc
```

Run that once from an elevated PowerShell, then `-Action start` again. `-Action status`
reports the current state as `auto_off_disabled=True`/`False` so a session can tell whether
the fix is already in place without needing admin rights itself.

## How this will be used

For AP-fallback / second-AP tests, a bench session:

1. Starts the hotspot (`-Action start`).
2. Points the board at this network — via its Wi-Fi provisioning UI or the relevant MCP
   tool — using the SSID/password from the env vars above, entered by a human or read
   programmatically from the environment, never hardcoded or logged.
3. Confirms the board associates and reaches this PC (e.g. `192.168.137.1`) rather than the
   lab network.
4. Stops the hotspot afterward (`-Action stop`) to leave the PC's networking state as
   found.

This hotspot is independent of the board's own Wi-Fi mode — starting or stopping it does
not touch the board, and the board's Wi-Fi configuration is changed only by whatever
explicit provisioning step a bench session performs separately.
