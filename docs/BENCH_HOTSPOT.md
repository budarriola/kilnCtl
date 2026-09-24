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
are set (booleans only), and the IPv4 address bound to the "Microsoft Wi-Fi Direct Virtual
Adapter" (the hotspot's own gateway address, typically `192.168.137.1/24`).

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
