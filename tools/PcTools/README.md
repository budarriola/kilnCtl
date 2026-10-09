# PcTools / kilnctrl

PC-side counterpart to the kilnCtl main board's hardened UART protocol,
implemented in `App/drivers/espInterfaces/uart_protocol.{c,h}` and addressed by
`App/drivers/uart_task_ids.h`. That header is the wire contract: every task id,
subcommand and payload layout here matches it byte for byte.

What is on the other end (see [`../../firmware/KilnFW/docs/HARDWARE.md`](../../firmware/KilnFW/docs/HARDWARE.md) for
the full trace): an **ESP32-S3-DevKitC** driving three **MAX31856**
thermocouple channels over J6, an **SX1509** I/O expander (four relays, seven
digital I/O, the three `~DRDY` inputs, and the display's D/C and `~RESET`), an
**ILI9488** 480x320 TFT on J2, and an opto-isolated link to an **RP2040**
safety processor.

## Physical connection

The dev board has **two USB-C ports**. This protocol runs on UART0 (115200 8N1,
no flow control) exposed through the **USB-UART bridge** port (CP210x / CH340 /
FTDI), *not* the ESP32-S3's native USB-Serial-JTAG port. Port autodiscovery
scores JTAG-looking ports negatively for exactly this reason.

## Usage

Commands are written to run **from the repository root**. This package lives at
`tools/PcTools/`; it moved out of `KilnFW/pc_tools/` because it serves both
processors, so it is no longer inside either firmware.

```powershell
# Tkinter manual-control GUI
uv run --project tools/PcTools kilnctrl-gui

# MCP server (streamable HTTP on 127.0.0.1:8767/mcp)
uv run --project tools/PcTools kilnctrl-mcp-server
```

Both are also VS Code tasks — **"PcTools: Open GUI"** and
**"PcTools: Run MCP Server"**, defined in `kilnCtl.code-workspace` rather than in
a firmware's `tasks.json`, since neither belongs to one firmware. In practice the
servers are usually already running (status-bar buttons / workspace auto-start);
see **[`../../docs/MCP_SERVERS.md`](../../docs/MCP_SERVERS.md)** for how they are
started and stopped, the six-tool search facade both publish, and
`--transport stdio` for headless/CI use.

The package still imports as `kilnctrl`: that is the *system's* name, not the
main board's, so the directory moved and the package did not.

## Layout

| module | role |
| --- | --- |
| `protocol.py` | SLIP framing, CRC-16/CCITT-FALSE, `Frame`, enums, task ids, every subcommand constant |
| `serial_link.py` | `UartLink` (reader thread, retry/ACK logic), port discovery |
| `link_hub.py` | Lets several `kilnctrl` processes share one physical port |
| `devices.py` | Payload builders + response parsers for all twelve tasks |
| `thermo.py` | `ThermoClient`: owns task 1, MAX31856 queries + the auto-report push |
| `io_expander.py` | `IoClient`: owns task 2, SX1509 queries + the auto-report push |
| `info.py` | `InfoClient`: owns task 3, pin config / FW version, spots boot pushes |
| `display.py` | `DisplayClient`: owns task 4, READ_ID plus the blit stream; Pillow image conversion and a test-pattern generator |
| `device_log.py` | `LogClient`: owns task 5, the firmware's forwarded ESP_LOGx output |
| `safety.py` | `SafetyClient`: owns task 7, the isolated RP2040 link |
| `control.py` | `ControlClient`: owns task 8, zone PID/plant-model read + narrow writes |
| `profiles.py` | `ProfilesClient`: owns task 9, fire profile CRUD + execution control |
| `autotune.py` | `AutotuneClient`: owns task 10, PID autotune status/start/abort/accept |
| `wifi_uart.py` | `WifiUartClient`: owns task 11, Wi-Fi status/scan/provision/forget over UART |
| `probe.py` | `ProbeClient`: owns task 12, raw ESP32 GPIO probe (only on a `CONFIG_KILNCTL_ENABLE_GPIO_PROBE` build, default off) |
| `pin_overlay.py` | Badge coordinates in `assets/pinout.png` + overlay drawing |
| `session_log.py` | Per-session log files, semantic rollover, retention setting |
| `settings.py` | Persisted app settings (`settings.json`): last-used port, log retention |
| `actions.py` | Named-action registry (one entry per GUI button) backing `press_button` |
| `mcp_server.py` | MCP tools over streamable HTTP, plus the generic `press_button`/`list_buttons` pair |
| `gui.py` | Tkinter GUI: `manualCtrl`, `Logs` and `About` menus |
| `logic_capture.py` | Saleae Logic 2 automation-API (gRPC) client: device list, timed digital capture |

The `uart_*` naming inside those modules still refers to the UART protocol
itself, which has not changed; only the package identity did (`uart_control` ->
`kilnctrl`).

## Device pages / tool groups

Each device gets one GUI page, one action group and one MCP tool group, all
three built on the same `devices.py` builders.

### Thermocouples -- task 1 (`manualCtrl -> Thermocouples`)

Per-channel type (B/E/J/K/N/R/S/T plus the two raw voltage modes), averaging,
50/60 Hz filter, one-shot vs automatic conversion, TC and cold-junction
thresholds, cold-junction offset, fault read/clear, and raw register
read/write.

The live temperature readout is fed by the firmware's **auto-report push**
(`SET_AUTO_REPORT`), not by a polling loop: one frame per period carries all
three channels, and the page turns reporting off again when it closes. Faults
are shown as decoded text -- "open circuit (no thermocouple?)" -- never as a
hex byte.

`~DRDY` does **not** reach the ESP32 on this board. It goes to the SX1509, so
conversion-ready is reported by the I/O page, not here.

### I/O & Relays -- task 2 (`manualCtrl -> I/O & Relays`)

**Relay numbering is not K numbering**, and the UI says so on every row:

| control | schematic net | contactor | terminal block |
| --- | --- | --- | --- |
| Relay 1 | `Relay1` | **K3** | J8 |
| Relay 2 | `Relay2` | **K1** | J3 |
| Relay 3 | `Relay3` | **K2** | J4 |
| Relay 4 | `Relay4` | **K5** | J11 |

The firmware exposes the schematic's `Relay1..4` numbering rather than silently
renumbering, so the mapping is spelled out in the GUI rows, in every action
description and in every MCP tool docstring. Also on the page: digital I/O 1-7
with direction and pull-up control and a live level read, a DRDY indicator per
thermocouple channel, and a raw-register section (read/write, dir / pull-up /
open-drain / debounce / interrupt masks, the LED driver, soft and hard reset,
and an I2C scan). Expander state is pushed on a period *and* immediately on
every `~INT` edge, so an input change appears without waiting out the period.

### Display -- task 4 (`manualCtrl -> Display`)

Reset (soft or via the expander's `~RESET`), power, rotation, invert, clear,
fill/draw rect, draw line, text cursor/style/print, and `READ_ID` -- the only
way to tell a wired-up panel from drawing commands vanishing into an
unconnected connector.

**Send an image** is implemented: "Open Image..." loads a PNG/JPEG, scales it
to the panel (letterboxed by default so nothing is silently stretched),
converts to RGB565 and streams it with `BLIT_BEGIN`/`BLIT_DATA`/`BLIT_END`,
with a progress bar. Pillow is a declared dependency for this, imported lazily
so the rest of the package still works without it. **Test Pattern** streams
colour bars plus a grey ramp through the identical path and needs neither an
image file nor Pillow -- the bars make a swapped colour channel obvious and the
ramp catches an RGB565->RGB666 expansion that lost its low bits.

Expect this to be slow: RGB565 fits 63 pixels per protocol frame, so a full
480x320 image is ~2440 frames, on the order of a minute at 115200 baud. The
page says so rather than letting it look like a hang.

### Safety processor -- task 7 (`manualCtrl -> Safety Processor`)

Status of the isolated link, the fault line, E-stop, safety relay K4, heating
enable, the safety thermocouple and the three current-sense channels, plus
request-enable, ping, poll period and a manual fault-line override.

Two things this page is careful about, both easy to get backwards:

* **The isolated fault line is an ESP *output*.** GPIO6 drives U1's LED, which
  pulls the Pico's `mainFault` input low: it is this firmware telling the
  safety processor that the main controller has faulted. There is no hardware
  path for the Pico to signal the ESP at all. The firmware asserts it by itself
  on PC-link loss, a thermocouple fault or a watchdog trip; `SET_FAULT_OUT` is
  a manual override of that.
* **"Link down" is the expected state today.** The RP2040 firmware that would
  answer this protocol does not exist in this repository yet, so `link_up = 0`
  with age "never received" is normal. The page leads with a banner saying so
  and colours that state amber rather than red -- painting a known-absent
  peer as a fault just trains people to ignore the colour that should mean
  something.

`GET_STATUS` never blocks on the far side: the ESP polls on its own schedule
and caches the last good answer, so a dead link is a *successful* query
reporting `link_up = 0`, and a raised `SafetyQueryError` means the **ESP**
didn't answer -- a different fault entirely.

### Zones / PID -- task 8 (`manualCtrl -> Zones / PID (UART)`)

Read-back of every zone's PID gains, plant model and configured limits, plus
narrow writes: `Set PID` (Kp/Ki/Kd) and `Set Model` (K_dc/tau_s/dead_time_s;
an all-zero triple clears the model). Both writes reply ok/fail immediately
rather than silently, so a rejected zone index or out-of-range gain shows up
right away. `/api/zones`' whole-page fields -- zone naming, relay assignment,
heater window timing, cross-zone guard threshold -- are **not** writable here
(that endpoint validates ~20 fields per zone together and doesn't decompose
into a safe per-field wire write); use the web dashboard for those. Manual
relay control is the I/O page's `Set Relay`/`Set Relay Mask`, not duplicated
here.

### Fire Profiles -- task 9 (`manualCtrl -> Fire Profiles (UART)`)

List/create/edit/delete stored profiles (name, zone mask, up to 12 segments of
target_c/ramp_c_per_hr/dwell_min), and drive execution: start, stop, pause,
resume, acknowledge the last run, and a live exec-status readout (state,
segment progress, ramp-lock, per-zone actual/duty/fault). Every mutating
command replies ok/fail (+ error text on `Save`/`Start` failure) rather than
leaving the GUI to find out on the next poll.

### Autotune -- task 10 (`manualCtrl -> Autotune (UART)`)

Status (state/method/zone/elapsed/samples/fitted model/proposed gains/
predicted max ramp), start (step or relay method, with the same
setpoint/duty/relay-d/relay-h/rule fields the HTTP form exposes), abort and
accept. The cross-zone coupling matrix and the trace/history CSV dumps stay
HTTP-only (Open Web Dashboard) -- bulk/table data that doesn't fit one
253-byte frame and has no honest truncated form.

### Danger Zone -- factory reset (`manualCtrl -> Danger Zone (Factory Reset)`)

Scope-selectable factory reset (Wi-Fi only / kiln config only / profiles only
/ all) over UART (`SYSTEM_CMD_FACTORY_RESET`), so it works with no network.
Destructive on real hardware: a confirmation dialog followed by a
type-to-confirm text prompt, no default scope pre-selected. Mirrors
`POST /api/factory_reset` exactly -- same per-partition NVS erase, same
unconditional reboot ~500ms later; no reply frame either way, so the ACK is
the only immediate confirmation and the reboot itself (a fresh unsolicited
`GET_FW_VERSION` push) is the real evidence the erase happened.

### Wi-Fi Settings -- HTTP or UART (`manualCtrl -> Wi-Fi Settings`)

The existing HTTP-based popup gained a **"Use UART (no network needed)"**
checkbox: when checked, Refresh Status / Scan / Connect / mode switch go over
task 11 (`WifiUartClient`) instead of `urllib` HTTP against the host/IP field
-- the point being that Wi-Fi can be provisioned before the board has ever
joined a network, when there's no HTTP path to it yet. AP-identity editing
(`SET_AP_IDENTITY`) stays HTTP-only in this pass. The read-only Firing Status
popup is unchanged.

## Config presets

A named, known-good starting config, so a test run begins from the same board
twice. Presets are **DATA** under `config_presets/*.json`, never compiled into
firmware -- the bench fixture's 80 °C ceilings must not be capable of riding
into a real kiln build.

```
kiln_call(name="list_config_presets")
kiln_call(name="load_config_preset", args={"name": "bench_fixture",
                                            "host": "192.168.1.156",
                                            "safety_host": "192.168.1.156"})
kiln_call(name="factory_default_then_load_preset", args={"name": "bench_fixture"})
```

A preset is applied over three write paths, each verified by an independent
read-back (an ACK is never accepted as proof that a value landed):

| Section | Path | Reached with |
|---|---|---|
| `zones[].pid_*` / model | UART CONTROL task (`control.py`) | always |
| the rest of `zones_cfg_t` | `GET`/`POST /api/zones` (`zones_http_client.py`) | `host=` |
| `"safety"` | `POST /api/safety/commissioning` (`safety_cfg_http_client.py`) | `safety_host=` |
| `ramp_assist_enabled` | `POST /api/ramp_assist` (`ramp_assist_http_client.py`) | `host=` |

`host` and `safety_host` are normally the **same address**: both endpoints are
served by the ESP32, which is the only thing that can talk to the RP2040 at
all. Omit either and that section is reported as reference data, not written.

**`ramp_assist_enabled` is a REQUIRED top-level field on every preset, not
optional.** It pins the kiln-wide "ramp assist" flag (`docs/PID_EXPANSION_
PLAN.md` §7.5) to a known value rather than letting an experiment inherit
whatever the board happens to already have. This is not a style preference:
enabling ramp assist mid-run lets the executor silently stretch a ramp or
shorten a dwell, which invalidates a PID tuning run or A/B controller
comparison's tracking-error numbers outright. Every preset shipped in
`config_presets/` pins it `false`. `run_queue.py`'s HTTP-only apply path pins
it the same way, so a queued campaign can never start a firing without an
explicit answer to "is ramp assist on."

**The `"safety_ct_channel_map_backup"` section is never applied by default.**
`ct_channel_map[0..2]` states which relay each current transformer is
physically clamped around; committing all three makes the safety processor
clear `calibration_missing` and therefore grant heat. On a bench with no CT
fitted there is nothing for that map to be true about, so the assumed identity
map lives in its own section and is written only when a caller knowingly
passes `use_ct_map_backup=True` (a host-test scenario, or a deliberate
exercise of `commissioning_gate.c`'s accept path). The real map comes from the
zone current-sweep on the zones page once CTs exist.

## Config package conversion (`cfg_convert`)

`src/kilnctrl/cfg_convert.py` converts a kiln backup package -- the JSON
document `GET /api/backup/export` produces and `POST /api/backup/import`
accepts (`firmware/KilnFW/App/drivers/http/backup_export.c`/
`backup_import.c`) -- from any `BACKUP_FORMAT_VERSION` to any other, in
either direction, best effort. This is entirely a PC-side tool: the board
itself keeps its firmware migration limited to exactly one step, the version
it was built against minus one (`docs/CONFIG_MIGRATION_CHAIN_PLAN.md`), so a
board more than one release behind cannot read its own on-flash config and
would fall back to firmware defaults on its own. `cfg_convert` closes that
gap off-board: it produces a package already expressed in the shape the
target board's own firmware understands, so a restore never hands a board a
package it cannot read.

```powershell
# Convert a downloaded backup to an older package format (version 2):
python -m kilnctrl.cfg_convert C:\backups\kiln_2026-09-10.json --to-version 2 -o converted.json

# Convert to whatever format the board at this address currently understands,
# read live over HTTP from its own GET /api/backup/export -- no need to know
# the number:
python -m kilnctrl.cfg_convert C:\backups\kiln_2026-09-10.json --to-board 192.168.1.42 -o converted.json
```

Installed as the `kilnctrl-cfg-convert` console script too, once the venv is
active (`kilnctrl-cfg-convert <input> --to-version 2 -o converted.json`).

**Forward conversion** (older document to a newer version) follows the same
additive-field semantics the firmware's own backup exporter/importer already
use: most fields introduced by a later firmware are optional, presence-gated
keys that a document simply may or may not carry, so "converting forward"
mostly means leaving them unset rather than inventing a value for them. The
one genuinely structural change in the format's history is how zone-to-zone
thermal coupling is expressed -- a single `coupling_coeff`/
`coupling_neighbor_zone` pair (versions 1-3) versus a full `coupling_c<N>`
row, one key per neighbor channel (version 4 onward) -- and `cfg_convert`
expands or collapses that representation explicitly.

**Backward conversion is best effort by definition:** a field the target
version's document shape cannot express is dropped, and a coupling row with
more than one real neighbor collapses to whichever single coefficient has
the larger magnitude, discarding the rest. Every conversion prints (and can
write to a `--report` JSON file) a line for every field that did not survive
intact, naming it as `dropped`, `derived`, or `kept` with a short reason --
for example, collapsing a full coupling row into the single-neighbor v3
format reports the discarded neighbor(s) by name, and expanding a
single-neighbor v3 pair into a v4 row reports which channels are *derived*
zeros rather than real measurements. Running the same document A -> B -> A
does not silently "round-trip" -- the report on the second hop shows plainly
that the fields dropped on the way to B were not restored, because they no
longer exist anywhere to restore from.

**`cfg_convert` never fabricates a calibration value.** If the target
version has a calibration field (`normal_current_a` on the ESP zone side;
the Pico/SaftyFW-side analogue is `i_normal_a`, not carried in today's ESP
backup document but guarded the same way) that the source document does not
have, the output simply does not have it either -- it is never defaulted,
derived, or copied from another zone. An uncalibrated CT channel correctly
leaves its guard dormant; a fabricated reading would arm that guard on a
lie, which is worse.

**`cfg_convert` never reads or emits a credential.** It refuses outright
(exits non-zero, writes nothing) if the input document contains a
`kiln_auth` key or any Wi-Fi-credential-shaped key (SSID, password, PSK) at
any depth -- the real export format never contains these, so their presence
means the document should not be trusted, not that they should be quietly
carried through or stripped.

Because this module duplicates firmware's own idea of what a backup document
contains, `firmware/KilnFW/App/test/cfg_convert_field_mirror_drift_check.py`
(wired into `tools/run_all_checks.ps1` via
`check_cfg_convert_field_mirror_drift.ps1`, same as the project's other
`*_mirror_drift_check.py` scripts) extracts the live field vocabulary and
`BACKUP_FORMAT_VERSION`/`BACKUP_FORMAT_VERSION_MIN` constants straight out of
`backup_export.c`/`backup_import.c`/`backup_http_internal.h` and fails the
build the moment `cfg_convert.py`'s own copy disagrees.

Tests: `tools/PcTools/tests/test_cfg_convert.py`, run through pytest. Its
fixtures under `tests/fixtures/cfg_convert/` are **synthesized**, not
captured from a real board -- see that directory's own `README.md` for where
a real capture should live once one exists (the owner's release-step
requirement is to capture one before every version bump).

## Gate fields -- config values that make a feature REACHABLE, not just configured

`src/kilnctrl/gate_fields.py` is the checked-in inventory of **gate fields**:
config values that skip a whole feature/subsystem for one of their values
(`control_mode` gating the fuzzy PID layer is the archetype -- the fuzzy
layer only runs under `control_mode == 3`; `ct_installed` gating the
current-transformer-dependent safety guards S3/S4/S9/S11/S14 is another),
as distinct from fields that merely **tune** a feature that is already
running (`pid_kp` never skips the PID loop, at any value). Two incidents
paid for this list: an analysis pooled 28 `control_mode: 2` captures with
one `control_mode: 3` capture and reported a 37,008-sample conclusion whose
real n was 2178; separately, two campaigns ran for hours believing the
fuzzy layer was active because the readback checked `fuzzy_strength_pct`
(a tuning field) instead of `control_mode` (the gate). The list is not
exhaustive -- see the module docstring for what has and has not been
confirmed by reading the actual gating code.

Two consumers of that inventory:

* **`_check_gate_fields_consistent` in `run_queue.py`** -- runs during
  campaign preflight, before any board is touched (alongside the existing
  B9 arms-differ check): refuses a preset that sets a gated feature's
  tuning field without pinning that feature's gate to a reachable value in
  the same zone. This is the authoring-time fix for the "checked the wrong
  field" incident above. Every applied preset's gate values are also
  snapshotted into the capture's `meta` header line
  (`gate_fields.summarize_preset_gates`), so a capture answers "which
  gated features were reachable during this run" from its own header.
* **`capture_pool_provenance.py`** -- the general, N-file form of the
  pooling incident above: given a list of capture `.jsonl` paths and a gate
  field name, refuses (`assert_pool_gate_consistent`) or reports
  (`check_pool_gate_consistency`) a pool that mixes files where the gate
  was reachable with files where it was not, checked per zone from the
  per-tick `exec.zones[].<field>` telemetry every HTTP capture already
  records. Works for any gate field with per-sample telemetry (today:
  `control_mode`), not only the fuzzy layer -- a new gated feature gets
  this check for free rather than a bespoke pooling script. Complements
  (does not duplicate) `fuzzy_band_probe.py`'s own per-file, per-tick
  `control_mode` filtering for its membership-band math, and
  `bd_reachability_check.py`'s two-arm `bd_*` telemetry comparison:
  ```
  python -m kilnctrl.capture_pool_provenance control_mode capture1.jsonl capture2.jsonl ...
  ```

## Live-bench test harness (pytest)

`tests/bench_fixture_session.py` is the reusable precondition the live tests
were missing: **"the board is sitting on `config_presets/bench_fixture.json`,
and that has been read back and confirmed."** `tests/conftest.py` exposes it
as pytest fixtures, so the next live test that needs a known starting config
(zone PID behavior, autotune, dashboard reporting) takes one argument instead
of re-deriving what the board was configured with.

```
# host-only, the default -- no hardware, nothing skipped silently
python -m pytest tools/PcTools/tests -q

# including the live-bench tests
KILNCTRL_BENCH_HOST=192.168.1.156 python -m pytest tools/PcTools/tests -q -s
```

Without `KILNCTRL_BENCH_HOST` every live test **skips**; there is no mock
standing in for the board.

| Fixture | Scope | What it gives you |
|---|---|---|
| `bench_host_addr` | session | the address, or a skip |
| `bench_session` | session | a `BenchSession` whose zones + safety config were written **and verified** once |
| `bench` | function | the same session plus a `finally` teardown that force-stops the executor and asserts every relay is off |
| `cold_bench` | function | `bench`, plus the guarantee that the bench **started cold** — see below |

### Waiting for the bench to cool (`wait_for_cooldown`)

Thermal tests do not compose without a cooldown gate. Run back to back, the
second test starts on the first one's residual heat and reports a smaller
step, a smaller gain and a shorter dead time — all wrong in the same
direction, and none of it visible in the result.

`BenchSession.wait_for_cooldown(target_c=None, tolerance_c=3.0,
timeout_s=2700, poll_s=20, channels=None)` polls `/api/status` until the
hottest watched thermocouple is at or below a target, and **raises** if its
budget expires, naming the temperature actually reached. It never returns
"close enough".

The target is not a constant. By default it is
**`ambient_reference_c() + tolerance_c`**, read at the moment of the call,
where `ambient_reference_c()` is the *lowest valid cold-junction* (`cj_c`)
reading on the board. Two reasons:

* **The cold junction is the only ambient reference this board publishes**,
  and it moves with the room. This bench sits at ~34 °C cold junction while
  the room runs around 100 °F; a hardcoded 25 °C gate would simply never
  open.
* **Lowest, not mean** — a converter whose channel has just been driven hot
  picks some of that heat up through the board, so the coolest cold junction
  is the least-contaminated estimate of the room.

Hot junction vs cold junction: the cold junction supplies the *reference*
(what is the room doing), the hot junctions supply the *subject* (has the
element's heat dissipated). Judging each hot junction only against its own
cold junction would be tighter in principle, but on this jig both sit on the
same small board, so the cold junction lags the room upward during a firing
and that comparison closes early — exactly when it should not.

The decision logic is split out as two pure functions, `cooldown_target_c()`
and `cooldown_reached()`, and host-tested in
`tests/test_cooldown_policy.py` — including the negative cases: no
cold-junction reading **refuses** rather than defaulting to a constant, a
negative tolerance is refused at the call rather than 45 minutes later at the
timeout, an explicit target is never second-guessed even when unreachable,
and **NaN is not cool** (a dropped-out thermocouple must not open the gate).

**Why it is not an MCP tool.** A multi-minute blocking wait does not belong
in a `kiln_batch` round trip — that tool's contract is one request, in order,
stopping at the first failure. As a session method it composes the way a
precondition should: a test, or the `cold_bench` fixture, calls it *before*
the batch of hardware operations that needs a cold start.
`KILNCTRL_BENCH_COOLDOWN_TARGET_C` / `_TIMEOUT_S` override the fixture's
derived target and budget for a session that wants an absolute gate.

Writes go over HTTP (`/api/zones`, `/api/safety/commissioning`) reusing the
same read-back-verifying clients `load_config_preset` uses -- no second,
weaker verifier lives in the harness. PID gains and the thermal model have no
HTTP setter, so they are written only when the COM port happens to be free;
the MCP server normally owns it, and `BenchSession.uart_available` /
`uart_detail` record which happened rather than letting a test believe it
applied gains it never wrote. A true from-blank reset is still the MCP tool
`factory_default_then_load_preset` (`POST /api/factory_reset` is
challenge-response authenticated); this harness is the apply-and-verify step
that follows it.

**Everything is bounded to 80 °C by four independent checks**: the preset's
own `max_temp_c`/`abs_max_temp_c`, `_assert_preset_is_bench_safe()` refusing a
preset edit that raised either, `put_profile()` refusing to author a segment
above the ceiling, and `assert_within_fixture_ceiling()` reading live
temperatures before anything that could add heat. Each was proven capable of
failing before being trusted.

### `tests/test_live_bench_firing.py`

The firing-adjacent regression test. It authors a bounded 45 °C / 1-minute
profile into user slot 7, starts it through **the same
`POST /api/profile_exec/start` the dashboard's Start button posts to**, and
branches on the board's own live commissioning verdict
(`heat_is_permitted()`), never on a hardcoded expectation:

* **Heat not permitted (today** -- no CT is fitted, `ct_channel_map[0..2]` is
  uncommitted, so `commissioning_gate.c` reports `calibration_missing`**)**:
  the start endpoint itself is *accepted* -- measured, not assumed; the gate
  is downstream of `profile_executor.c` -- so the test asserts the stronger
  thing: for the whole time the executor believes it is firing, **no relay
  ever energizes and `safety_heating_enabled` stays false**. That is a
  regression test for the gate working, not for it existing.
* **Heat permitted** (after the CTs are fitted and the zone current-sweep
  commits a *measured* map): the same test runs the profile, checks the
  setpoint stays inside the ceiling, stops it, and confirms relays return to
  off. **No edit to this file is needed on that day.**

Also here: `test_known_good_config_landed` (config verified against a fresh
read of the live board, not against the writer's own say-so) and
`test_dashboard_status_consistent_with_known_config` (`/api/status` must agree
with the config that was just confirmed -- the "consumer without a producer"
defect class caught from outside).

### `tests/test_live_bench_tuning.py`

The **step and PID-tuning** tests, on the same harness. Two things, both
bounded by the same 80 °C ceiling and the same `finally` teardown (which now
also POSTs `/api/autotune/abort`, because `profile_executor.c` and
`autotune_engine.c` are peer heat owners and stopping only one of them is not
a teardown):

* **Closed-loop step** — a single-segment 45 °C profile on zone 0 at the
  zone's own 900 °C/h ramp, started through the dashboard's endpoint, with
  the PV trace sampled every 2 s by `BenchSession.sample_response()` (which
  asserts the ceiling on *every* sample, not once at the end).
* **Open-loop step autotune** — `autotune_engine.c`'s `AUTOTUNE_METHOD_STEP`
  driven end to end through `/api/autotune/start`, followed under a 300 s
  budget (the engine's own budget is 4 h), with the trace read from
  `/api/autotune/trace.csv`. Every terminal state is asserted differently:
  a `done` fit must be *physical* (τ > 0, K > 0, gains finite and
  non-negative), an `aborted` run must **say why**, and still-running at the
  budget is reported as inconclusive and aborted rather than passing quietly.
  **`/api/autotune/accept` is never posted** — accepting writes gains and the
  FOPDT model to NVS, and a regression test must not retune the bench; the
  test asserts the gains and model are byte-identical afterwards.

**Superseded 2026-08-29 — both tests now assert the plant, not just the
gate.** The 2026-08-28 measurement recorded here (PV flat, autotune aborting
with *"response too small to fit"*) was correct about the trace and wrong
about what it meant: `profile_executor.c`/`autotune_engine.c` never requested
heat enable, so **K4 was open for every firing this board had ever run**
(`heat_enable.h`), and zone 0's 2 s time-proportioning window could not render
any duty against the 10 s minimum on-time. With both fixed:

* the closed-loop step asserts **relay 1 closes, K4 is closed on at least half
  the samples, and zone 0 rises at ≥ 1.5 °C/min** while the loop is driving
  below setpoint (measured on this jig: 2.8–3.8 °C/min at full duty; 0.017
  °C/min was what an open K4 produced), plus overshoot and settle bounds;
* the open-loop autotune asserts it **converges** — `state=done`,
  `model_valid`, physical K/τ/L.

Two harness bugs were found by turning those observations into assertions,
and both are fixed here:

* **The step profile's dwell was shorter than the observation window.** A
  900 °C/h ramp with `dwell_min=1` put the setpoint at 45 °C in ~45 s and
  ended the run at ~105 s, with PV still at 38.6 °C and climbing. The
  remaining ~380 s of the sweep sampled a *cooling* jig, and the trace read
  as 0.38 °C/min — four times too slow, for a reason that has nothing to do
  with the plant. `STEP_DWELL_MIN` is now derived from `STEP_OBSERVE_S`.
* **`sample_response()` indexed channels positionally** in a list already
  filtered to valid ones, so a single invalid channel silently relabelled
  every channel after the gap. It is keyed by the channel's own index now,
  and each row carries `channels_c` for *every* zone — which is what makes
  the cross-zone measurement below possible at all.

Both tests take the `cold_bench` fixture: an open-loop fit or a rise rate
measured from a hot start comes out smaller, and nothing in the result says
so.

### `tests/test_live_bench_zone_interaction.py`

**"Auto zone interaction measurement."** The phrase could mean two things and
they are not the same feature, so the file says which it tests and why:

* **The coupling matrix and its RGA** — `autotune_engine.c`'s
  `autotune_coupling_matrix_t` (K[i][j] = zone j's FOPDT response to a duty
  step on zone i, filled a *row at a time* as autotune runs complete) and
  `pid_autotune_rga()`, served together at **`GET /api/autotune/matrix`**.
  This is the feature this repo actually built and named, and until now it had
  **never run on real data** — `autotune_engine.h` says so in as many words.
  The test fills two rows with real cold-start autotune runs (zones 0 and 1,
  same power cycle — the matrix lives in RAM, not NVS) and then asserts
  Bristol's identity on the result: **every row and column of Λ sums to 1**,
  for any invertible K. That is a property of the math, so it holds whatever
  this jig's coupling turns out to be — a real test of the firmware's
  implementation on measured data rather than a test of a number someone
  typed. A negative diagonal element fails: it means closing the other zones'
  loops *reverses* this zone's gain, i.e. per-zone PID is the wrong
  architecture for this plant.
* **Raw thermal coupling** — how much of zone 0's heat leaks into zones 1 and
  2 in this small shared enclosure. Reported in degrees, and as a fraction of
  the fired zone's own rise, from the `channels_c` field every sample now
  carries. *Reported*, not tightly asserted: a coupling figure is a property
  of the enclosure, not a pass/fail criterion. The assertion on it is the
  safety one — no unfired zone may exceed 60 °C.

`ramp_lock_lagging_mask` was considered as a third reading of the phrase and
rejected as the *primary* subject: it is a setpoint-coordination mechanism
(hold the shared ramp while a participating zone lags by more than
`PROFILE_EXECUTOR_RAMP_LOCK_BAND_C`), not an interaction measurement. It is
checked by the full-profile test instead, which is where it belongs.

### The full multi-segment profile (`test_live_bench_firing.py`)

Everything else in the suite fires a *single* segment, which cannot answer the
questions a real firing is made of. `test_full_multi_segment_profile_runs_to_
completion` runs a three-segment schedule (42 °C dwell 6 min → 52 °C dwell
6 min → a **down-ramp** to 46 °C) through `POST /api/profile_exec/start` and
asserts: every segment entered **in order**, every segment actually
**dwelled**, the peak reached within 3 °C of the top target (a profile that
completes on the clock while cold is not a firing), K4 closed for a real
fraction of the run, **no heat commanded during the down-ramp** while already
above the descending setpoint, ramp-lock never engaging on a single-zone
profile, and — at the end — every relay off **and K4 released** by the
completion path rather than by the test's own teardown.

The **relay-feedback** autotune method is deliberately unreachable from the harness:
`AUTOTUNE_RELAY_SETPOINT_HEADROOM_C` (50 °C) refuses any setpoint within
50 °C of `max_temp_c`, which under this fixture's 80 °C ceiling admits only
setpoints below the bench's own 35 °C ambient. A test for it would be testing
a parameter refusal, not a tune.

## Generic button press (MCP)

Every bespoke tool (`thermo_read`, `io_set_relay`, ...) also has a same-named
entry in `actions.py`'s registry, reachable generically:

```
kiln_call(name="list_buttons")                          # every action name + params + description
kiln_call(name="press_button", args={"name": "IO: Set Relay", "params": {"relay": 1, "on": true}})
kiln_call(name="press_button", args={"name": "Thermo: Read All"})
```

(`list_buttons`/`press_button` are, like every other tool below, reachable
through `kiln_call` — the server no longer publishes them directly. See
**[`../../docs/MCP_SERVERS.md`](../../docs/MCP_SERVERS.md)**.)

Action names match the GUI's own button/menu-item labels 1:1, so a future GUI
button gets MCP coverage the moment it's added to `actions.py`, without a
matching `@mcp.tool()` having to be hand-written. `gui.py`'s own widget
callbacks are untouched -- this is a second, generic front door onto the same
underlying `devices.py` calls, not a replacement.

## Query channels and unsolicited pushes

`UART_TASK_ID_INFO` was the only query channel on the old fixture; this board
has five. In all of them the request DATA frame is ACKed like any other
(delivery only), and the answer comes back as a **separate DATA frame** from
`(ESP, that task)` addressed to whoever asked -- so the PC has to be registered
on that task itself. All five clients register at app construction, not lazily
when a window opens.

Two differences worth knowing:

* INFO replies carry **no** subcommand byte, so `parse_info_response()`
  classifies structurally. Every other task echoes its subcommand in byte0.
* Three tasks push **unsolicited** frames: INFO (the once-per-boot version
  push, which is how a reboot is detected), THERMO and IO (auto-reports). A
  push is byte-identical to a query answer, so "nobody asked" is the only thing
  that distinguishes them -- which is why each client owns its task's inbox
  with a single consumer thread instead of letting callers drain the queue.

The MCP server buffers the two auto-report streams into `thermo_get_reports()`
and `io_get_reports()`, since an MCP client has nowhere to receive a push.

## Protocol version compatibility

`GET_FW_VERSION` carries `UART_PROTOCOL_VERSION` (see `uart_task_ids.h`) at a
*fixed* byte offset that never moves across versions, so it can always be read
and compared before trusting the rest of the payload. **This board is version
4** (task_ids 8-11 -- CONTROL/PROFILES/AUTOTUNE/WIFI -- plus
`SYSTEM_CMD_FACTORY_RESET`, additive to versions 2/3's task set). Version 1 is
the unit-test fixture, and the two are not merely different:
they reuse the same task ids for different hardware, so a v1 firmware would
accept a thermocouple command on task 1 and interpret it as an MCP4728 DAC
write. Hence a hard equality check, not `>=`.

`InfoClient.compatible` is `None` until a version has actually been observed
(query reply or boot push), `True`/`False` after. Both the GUI and the MCP
server refuse to send **any** device command -- blocking on `None` too, not
just `False` -- until compatibility is confirmed; INFO queries themselves are
always allowed, since that's how compatibility gets discovered in the first
place. The GUI also pops an error dialog once per connection on a confirmed
mismatch and turns the status-bar FW line red.

There's no automatic way to tell whether an arbitrary firmware change would
actually break the wire format, so `UART_PROTOCOL_VERSION` is a manually
maintained integer (bump policy documented next to it in `uart_task_ids.h`),
not a hash of the header -- a hash would flag harmless edits (comments,
reordering) as incompatible just as readily as a real break.

## Connecting does not reset the board

Two things had to be right for this, and both were wrong before:

* `UartLink.connect()` configures DTR/RTS **while the port is still closed**,
  then opens it. Passing `port=` to `serial.Serial()` opens with those lines
  at their driver defaults, which pulses the board's auto-reset circuit --
  deasserting them afterwards is too late.
* The starting `MSG_INDEX` is randomized per connection. The firmware's dedup
  ring outlives any host session, so restarting the host at index 0 made its
  first sends look like retransmits: re-ACKed, never delivered. See
  [`../../firmware/KilnFW/docs/UART_PROTOCOL.md`](../../firmware/KilnFW/docs/UART_PROTOCOL.md).

Together these are why the first query after a connect used to fail with
"ACKed but no reply arrived", recovering only when the firmware's
once-per-boot version push happened to land right after.

## About window / pinout diagram

**About -> Pin Configuration...** shows the live `GET_PIN_CONFIG` reply drawn
over `assets/pinout.png`, plus a legend of what each pin does. The image is a
stock ESP32-S3-DevKitC-1 pin list, which is the module this board carries, so
it did not need replacing -- what changed is which badges get highlighted
(nineteen GPIOs here against the fixture's eight) and the function-id table
behind them, which gained `SPI_MISO`, `THERMO_FAULT`, `EXPANDER_IRQ`,
`EXPANDER_RST`, `SAFETY_TX`, `SAFETY_RX` and `SAFETY_FAULT`.

Only real ESP32-S3 GPIOs appear there. The relay drives, the DRDY inputs and
the display's D/C and `~RESET` are SX1509 pins, reported through the I/O page
instead. See `pin_overlay.py` for which badge coordinates were measured
against the PNG and which were derived from the row pitch.

## Saleae Logic 2

Logic 2 runs two local servers, both toggled from **Preferences** and both
already enabled here (`%APPDATA%\Logic\config.json`:
`automationServerEnabled`, `mcpServerEnabled`):

| server | endpoint | used by |
| --- | --- | --- |
| automation (gRPC) | `127.0.0.1:10430` | `logic_capture.py` / `logic2-automation` |
| MCP (HTTP) | `127.0.0.1:10530/mcp` | registered as `saleae` in the repo's `.mcp.json` |

Logic 2 must already be running — neither server can launch it. Prefer the MCP
server for interactive, agent-driven capture; use `logic_capture.py` when the
capture has to be interleaved with UART traffic from a single process (arm the
analyzer, drive the DUT, export the decode).

```powershell
uv run --project tools/PcTools python -m kilnctrl.logic_capture devices
uv run --project tools/PcTools python -m kilnctrl.logic_capture rates --channels 0,1
uv run --project tools/PcTools python -m kilnctrl.logic_capture capture --channels 0,1 --seconds 2 --out logs/saleae
```

`SALEAE_AUTOMATION_HOST` / `SALEAE_AUTOMATION_PORT` override the endpoint.

Two device quirks the API doesn't surface as queries, both handled in
`logic_capture.py`:

- the sample rate must be one of a fixed set that depends on the enabled
  channel count — hence the `rates` subcommand, which recovers the legal set
  from the backend's rejection message (default is 25 MS/s);
- the attached Logic 16 exposes threshold *ranges* (1.8–3.6 V, 3.6–5.0 V) and
  rejects any explicit value, so `--threshold` is unset by default and Logic's
  own setting is used.

## Session logs

Both the GUI and the MCP server write one log file per session to
`tools/PcTools/logs/session_*.log`. A new file starts when a connect succeeds or
when a device reboot is detected — both semantic triggers, hence a
hand-swapped `FileHandler` rather than one of `logging`'s size/time rotating
handlers. **Logs → Keep Logs...** sets how many files to retain (persisted in
`src/kilnctrl/settings.json`, along with the last-used serial port, which is
preferred over the autodiscovered recommendation the next time a port is still
present); **Logs → Show Log...** is the in-app view.

## Known limitation

For everything that is not a query subcommand, `uart_bridge.c` only ACKs/NACKs
at the protocol layer; it never sends an application-level status frame back. A
send result of `ok` therefore means "delivered to the ESP task's inbox",
**not** that the underlying SPI/I2C transfer to the MAX31856, SX1509 or ILI9488
succeeded. Device-level failures surface only as firmware log lines — the GUI's
**Device Console** window, or the MCP server's `get_device_log()`.

## Self-check

```powershell
uv run --project tools/PcTools python tools/PcTools/selfcheck.py
```

Runs framing/CRC/payload-layout checks for all seven tasks and cross-wires two
`UartLink`s over a fake port with stub bridge tasks, so every query flow, both
auto-report pushes, the boot push, the blit stream and the action registry's
version gate are exercised without hardware.
