# Project Status

What is actually done, what is verified vs. merely built, and what is left,
as of this writing. This file is the one to update whenever something here
changes — check dates and specifics against the code before trusting them,
the way you would any status doc.

## Scope

KilnCtrl retargets a unit-test-fixture firmware/PC-tools pair onto the real
kilnCtl main board: an ESP32-S3 driving three MAX31856 thermocouple channels
(via the daughterboard on J6), an SX1509 I/O expander (relays, digital I/O,
display control), an ILI9488 TFT on J2, and an opto-isolated link to an
RP2040 safety processor that is a separate, not-yet-started firmware project.

## Done and verified

- **Board wiring traced from the schematics**, not assumed — `docs/HARDWARE.md`.
  Caught three things that would have cost real bench time: the safety link's
  pin assignment was swapped in `KilnFW`'s `Kconfig` defaults relative to the
  board (`DataToSafty`/GPIO4 is the ESP's TX, `DataFromSafty`/GPIO5 is its RX —
  fixed 2026-08-16, see `../SaftyFW/docs/HARDWARE.md` §1), both isolated data
  directions are logically inverted by the optocouplers (fixed via
  `uart_set_line_inverse`; the RP2040 side needs **no** inversion of its own —
  see `docs/SAFETY_LINK.md`), and the isolated `Fault` line is an ESP
  **output**, not an input.
- **Wire protocol v2** — `App/drivers/uart_task_ids.h` — replaces the
  fixture's DAC/AD9833/OLED/PCF8575 tasks with THERMO/IO/DISPLAY/SAFETY.
  Framing, CRC, ACK/retry/dedup are unchanged from the fixture.
- **Five drivers**, each with its own doc: `MAX31856.c` (`docs/MAX31856.md`),
  `SX1509.c` + `kiln_io.c` (`docs/SX1509.md`), `ILI9488.c` (`docs/ILI9488.md`),
  `safety_link.c` (`docs/SAFETY_LINK.md`).
- **UART bridge layer and `app_main` bring-up** rewritten for this board's
  device set, with a documented safe boot-failure path
  (`kiln_enter_safe_state()`) and a link-loss watchdog — see
  `docs/SAFETY_MODEL.md` for exactly what that does and does not cover.
- **PC tooling renamed and rewritten**: `pc_tools/src/kilnctrl` (was
  `uart_control`) — GUI pages, actions and MCP tools for THERMO/IO/DISPLAY/SAFETY,
  live data via the firmware's auto-report push rather than polling, PNG→panel
  blit via Pillow.
- **A hardening pass** across firmware and Python: untrusted-input validation
  on every wire parser (both directions), the fail-safe relay/fault-line
  behavior on link loss, lock-timeout/deadlock/resource-lifecycle fixes in
  the drivers, and negative-path tests added to `pc_tools/selfcheck.py`
  (492 checks, currently all passing). Real bugs found and fixed during that
  pass are listed in the individual driver docs and in git history/PR
  descriptions where this was committed — this file does not repeat them.
- **The safety-wins relay gate** (`io_relay_on_blocked` in
  `App/drivers/uart_bridge.c`): a PC command to turn a relay ON is refused,
  not queued or partially applied, while any safety fault source is
  asserted or if the safety subsystem never came up at all. Turning a relay
  OFF is never refused. Full detail, including what this gate does **not**
  yet cover, is `docs/SAFETY_MODEL.md` — read it before assuming "safety
  wins" is fully implemented; it is implemented for the cases listed there
  and nowhere else yet.
- **Datasheets curated** to comm-interface parts only (I2C/SPI/UART) in
  `firmware/KilnFW/Datasheets/`.
- **Build**: `idf.py -C firmware/KilnFW build` from a full clean, ESP-IDF
  v6.0.2, target `esp32s3`, succeeds under this project's
  `-Wall -Wextra -Werror`. Re-verified 2026-08-16 after the repository
  reorganisation, which needed an `idf.py fullclean` first — the CMake cache
  records the project directory and refuses to build once it moves.

  **`KilnCtrl.bin` is 0x1237A0 bytes (1167 KB), 22% of the app partition free.**
  This entry previously read 0x497e0 and ~71% free; that was roughly four
  builds' worth out of date and materially wrong for anything sizing a
  partition. Measure before quoting.

  This machine's `export.ps1` is broken (looks for a venv path the EIM
  installer doesn't create); the working activation is
  `& 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'`. Invoking
  the ESP-IDF python directly also works but needs `ESP_IDF_VERSION`,
  `IDF_PATH`, `IDF_TOOLS_PATH` and `IDF_PYTHON_ENV_PATH` all set, plus cmake
  and ninja on `PATH` — the profile script is the shorter route.
- **Flash and PSRAM — resolved 2026-08-17.** Confirmed against the LonelyBinary
  product page for the board in hand (variant 43784065712285): it is an
  **N16R8 — 16 MB flash, 8 MB PSRAM**. The buy lists (`N8R8`, 8 MB / 8 MB) and
  the footprint library's 3D model (`N8R2`, 8 MB / 2 MB) are both stale and
  still need fixing at the source (BOM / 3D model), which is separate,
  unstarted work.

  The firmware's sdkconfig now declares **16 MB**
  (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`, was wrongly `_2MB`). `partitions.csv`
  itself is unchanged and still only maps the first 2 MB (single `factory`
  slot) — see ROADMAP.md M8 for the still-open two-app-slot OTA partition
  table work; `../../CommonFW/docs/UPDATE_PROTOCOL.md` §3 is the design doc
  for that.
- **PSRAM: ON since 2026-08-17** (`TODO.md` 9.1a). `CONFIG_SPIRAM=y`,
  `CONFIG_SPIRAM_MODE_OCT=y` at 40 MHz on the N16R8 module's 8 MB,
  `CONFIG_SPIRAM_USE_MALLOC=y`, `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`,
  `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`. In use by: LVGL's two draw
  buffers (`lvgl_port.c`), the LVGL heap (`lvgl_mem_psram.c`),
  `ui_page_touch_test.c`'s canvas, and several task stacks allocated with
  `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)` in
  `uart_bridge.c`, `uart_bridge_ext.c`, `gpio_probe.c` and `uart_protocol.c`.
  **This paragraph said "deliberately off, and staying off" until 2026-08-20
  — it was stale by three days and is corrected here.** It was off by
  decision on 2026-08-16 for four reasons (no framebuffer needed, cache-miss
  stalls against real-time deadlines, a new boot failure mode, a DMA audit);
  section 10's LVGL GUI was the named trigger that reversed it, and
  `TODO.md` 9.1a re-examines all four objections against the actual use.
  Note the DMA one specifically: `ILI9488_blit_data()` stages into the
  driver's own internal DMA-capable scratch buffer, so the PSRAM LVGL buffer
  is never a DMA target.
- **Python**: `python -c "import kilnctrl"` succeeds. `selfcheck.py` **fails 16
  of its checks** as of 2026-08-16; they fail identically on the
  pre-reorganisation source, so they are pre-existing and unrelated to the move.
  The previous claim here that all 492 pass is stale — see `docs/REPO_LAYOUT.md`.
- **PC-link UART baud raised to 921600** (was 115200), to speed up the
  DISPLAY blit path — changed consistently in `App/drivers/Kconfig`
  (`KILNCTL_UART_BAUD_RATE` default), `sdkconfig`
  (`CONFIG_KILNCTL_UART_BAUD_RATE`), and
  `pc_tools/src/kilnctrl/protocol.py` (`DEFAULT_BAUD_RATE`).
  Verified (2026-08-11): rebuilt, reflashed via OpenOCD, `get_fw_version()`
  and `get_wifi_status()` both succeed at the new rate. Note: the DISPLAY
  blit is still stop-and-wait (one ACK round-trip per 128-byte
  `UART_PROTO_MAX_PAYLOAD` frame), so fixed per-frame round-trip latency,
  not raw bit rate, is likely still the dominant cost for a full-frame
  blit — pipelining/windowing the wire protocol would address that but is
  a bigger change against the frozen v2 wire format, not done here. No
  RTS/CTS flow control is wired on this board's PC-link UART
  (`App/drivers/settings.h` only defines `UART_OWNER_TX_IO`/
  `UART_OWNER_RX_IO`).
- **First slice of the PID/thermal-protection design (TODO.md section 6A)
  built and live-verified (2026-08-11)**: `App/drivers/pid.c` (positional
  PID, derivative-on-measurement, low-pass filter, conditional-integration
  anti-windup, functional-range blending, bumpless transfer),
  `App/drivers/thermal_guard.c` (guards 1–7 of the Klipper/Marlin-class
  protection suite), and `App/drivers/heater_output.c` (time-proportioned
  and bang-bang relay rendering) are new; `App/drivers/profile_executor.c`
  was substantially rewritten to dispatch OFF/BANGBANG/PID control modes
  per zone and run the guard suite every tick, escalating trips through
  either the global safety-fault mask or the new
  `relay_authority_zone_blocked()` per-zone mask
  (`App/drivers/relay_authority.c`). Live-verified end to end on the real
  board (no thermocouple/relay hardware attached): guard 6 (sensor invalid)
  correctly tripped after 3 consecutive bad reads, latched the executor to
  `"faulted"`, kept the relay off throughout, and cleared correctly via
  `profile_executor_halt()`. Concurrent multi-zone execution (6A.5) is
  explicitly NOT part of this slice (the step-test half of autotune landed
  later the same day, see below) — see TODO.md section 6A for the itemized
  checklist of what did and didn't land.
- **Host-side test harness for pid.c/thermal_guard.c/heater_output.c
  (TODO.md 6A.8) built and run (2026-08-11)**: `App/test/` — a minimal
  MSVC-built host executable (`build_host_tests.ps1`, no ESP-IDF), a
  single-zone `sim_plant.c` thermal model (first-order lag + sensor
  transport delay/lag), and 71 checks across the three pure control
  modules plus a closed-loop sim+pid+guard+heater_output integration run.
  All passing. Writing these tests found and fixed a real bug in
  `heater_output_bangbang()`: it re-checked its min-on/min-off debounce
  timer only on the exact call where the raw comparator decision changed,
  not on every call while a transition was still pending — a `want_on`
  that stopped flip-flopping and just held its new value could leave the
  relay stuck at the old state forever, no matter how much time passed.
  Fixed to gate on the relay's actual state instead; rebuilt, reflashed,
  verified. Not yet built: the firmware-integrated N-zone/fault-injection
  simulator and `KILNCTL_SIM_PLANT` Kconfig path TODO.md 6A.8 also
  describes, and the written test-matrix doc.
- **Coupled multi-zone simulator + per-guard fault provocations
  (2026-08-12)**: the "N coupled zones / injectable faults" half of TODO.md
  6A.8, closing most of the gap the bullet above ends on. `sim_plant.{c,h}`
  gained a `sim_kiln` layer — up to 3 zones with an inter-zone conductance
  matrix (elements integrated simultaneously, so coupling isn't
  integration-order dependent), an optional per-zone radiative loss term
  (the reason plant gain falls with temperature and one tuning can't cover a
  whole firing), a read-time sensor map for swapped connectors, deterministic
  sensor noise, and injectable faults: dead element, welded relay, detached
  thermocouple (electrically fine, physically out of the kiln body), frozen
  sensor, open sensor. The single-zone `sim_plant_step()` API is unchanged.
  `App/test/test_sim_kiln.c` takes the host suite from 71 to **120 checks,
  all passing**, and changes what those checks mean: every implemented guard
  is now provoked by a *stated physical failure* rather than hand-fed
  numbers (dead element and detached TC → guard 1, swapped connectors →
  guard 2, welded relay → guard 3, element dying at low duty after settling
  → guard 4, low ceiling → guard 5, open TC → guard 6 at exactly 3 reads,
  frozen TC → guard 7 at its 600 s window), plus a negative control (1 °C of
  sensor noise on a healthy zone trips nothing). It also exercises TODO.md
  6A.5(b)'s cross-gain identification for the first time against a known
  non-zero coupling: a duty step on one zone, both zones fitted with the same
  `pid_autotune_fit_fopdt()` the on-target matrix capture calls, asserting a
  positive cross gain, smaller than the direct gain, over a slower path.
  This half is host-side only — no firmware change.
- **`CONFIG_KILNCTL_SIM_PLANT` — the control stack can now run against the
  simulated kiln on the board itself (2026-08-12)**, closing the last
  unbuilt piece of TODO.md 6A.8. `App/drivers/sim_backend.{c,h}` plus a new
  Kconfig menu (default **off**) compile the same `App/test/sim_plant.c`
  model into the firmware, and `profile_executor.c`, `autotune_engine.c`,
  and `dashboard_http.c` read simulated channels instead of MAX31856 ones.
  Relay commands reach the model from the post-safety-gate decision in both
  `apply_relay()` implementations, so the simulated kiln only heats when a
  real one would have. Faults are injectable on the running board over
  `POST /api/sim` (`element_dead`, `relay_welded`, `tc_detached`,
  `tc_frozen`, `tc_open`); `GET /api/sim` shows each zone's true element
  temperature beside what its thermocouple claims. The model advances on
  wall-clock time (`esp_timer`), sub-stepped at ≤1 s with a 60 s catch-up
  cap, so the 1 Hz executor, the 2 s dashboard poll, and the autotune engine
  can all read it without making simulated time run fast. Every entry point
  is a `static inline` no-op when the option is off — the production build
  is unchanged and `/api/sim` does not exist in it.
  **Both configurations build clean** (production default; sim-enabled build
  in a separate build dir, costing ~7 KB of app image). **Nothing was
  flashed** — the sim path has never run on the board, and no guard has yet
  been provoked on real silicon through it. Note for whoever does flash it:
  a sim build reports fabricated temperatures, and relay writes still go out
  to a real expander if one is attached, so it must not be flashed to a
  board wired to elements.
- **Relay contact-cycle accounting, persisted and operator-visible
  (2026-08-12)** — TODO.md 6A.1. New `App/drivers/relay_cycles.{c,h}` keeps
  lifetime on/off transition totals per relay in its own NVS key
  (`kiln_cfg`/`relay_cyc`, deliberately *not* folded into the `zones_cfg`
  blob, whose loader treats a size change as corrupt and would have wiped
  every user's zone setup). `profile_executor.c` feeds it the per-tick delta
  from `heater_output`'s RAM counter and lets it write at most once per 10
  minutes; `profile_executor_halt()` forces a flush. Counts saturate instead
  of wrapping. Shown as a table on Settings → Relays & Rules via a new
  `relay_cycles` array on `GET /api/status`, with the ~100k-operation
  contact budget explained and values past 80k highlighted — the point being
  that an electromechanical relay switching a kiln element has a finite
  contact life and a controller that eats it silently fails mid-firing.
  Built and flashed; **not exercised** (no relay hardware, no firing has
  run), so every count is still 0.
- **First on-target guard walk against the simulated plant, and a real
  false-positive bug it found (2026-08-12)** — TODO.md 6A.8's whole point.
  With `CONFIG_KILNCTL_SIM_PLANT` flashed and the PC joined to the fallback
  AP, guards were provoked over `POST /api/sim` and watched through
  `GET /api/profile_exec`:
  - **Guard 6 (sensor invalid)**: `tc_open` → tripped in **3 s** with
    `fault_guard: 7`, `"sensor invalid for 3 consecutive reads"`. Matches
    `docs/GUARD_TEST_MATRIX.md` exactly.
  - **Guard 3 (runaway / welded contact)**: `relay_welded` at a satisfied
    setpoint → tripped at **120 s**, correctly identifying heat arriving with
    the relay commanded off.
  - **Bug found: guard 3's rate was computed over mismatched intervals.** Its
    message read *"rose 4.3C (rate 257.74C/min)"* — the rise was measured from
    the start of the off-window but divided by the time since the *settle*
    window ended, so the first tick past the window divides by one `dt`. The
    consequence is worse than a wrong number: any zone idling at duty 0 and
    drifting up slightly (a dwell, a neighbour's heat arriving through the
    chamber, ordinary coasting after a ramp) trips a welded-contact fault.
    The guard 1 test case hit exactly that and tripped guard 3 instead.
    Fixed in `thermal_guard.c` by latching a second baseline when the settle
    window ends so rise and elapsed time cover the same interval; the 20 °C
    margin check still references the original baseline, since "20 °C above
    where it was when heat was commanded off" is the absolute claim. Two host
    regression tests added (125 checks now, all passing): a 0.2 °C/min drift
    at duty 0 must stay quiet for 15 simulated minutes, and a genuine
    welded-contact rise must still trip while reporting a plausible rate.
  - **Guard 7 (frozen sensor) is preempted by guard 1, by design.** Injecting
    `tc_frozen` on a heating zone tripped **guard 1 at 481 s**, not guard 7 at
    600 s — correctly: a frozen reading is flat while heat is commanded, which
    is precisely guard 1's no-progress signature, and guard 1's 300 s window
    is shorter. Guard 7's reachable window is therefore narrow: a zone holding
    at setpoint with duty above 0 but below `PROGRESS_DUTY_MIN` (0.5), where
    guard 1 does not engage. This is defence in depth working, not a defect,
    but it means guard 7 is hard to observe on-target and easy to over-claim.
    The same reasoning applies to guard 4 (drift): the physical scenarios that
    would produce a sustained excursion — dead element, welded relay — are
    caught first by guards 1 and 3. Both remain unobserved on hardware and are
    marked as such rather than assumed working.
  - **Sensor-swap injection added** (`POST /api/sim` with `swap=a,b`,
    sim builds only) so a miswired kiln — the failure guard 2 exists for — can
    be modelled on the board at all; the per-zone fault list could not express
    it. **Result: a miswire is caught, but by guard 1, at 478 s.** With zones
    0 and 1 reading each other's thermocouples, zone 0's element climbed to
    33.3 °C while zone 0 read zone 1's untouched 20.1 °C sensor — flat under
    commanded heat, i.e. guard 1's signature. Guard 2 needs the reading to be
    *above setpoint and falling*, which a PID loop makes rare by construction
    (above setpoint it commands zero duty). So guard 2 is a backstop for a
    narrower case than "miswired kiln", and the failure it is usually credited
    with catching is actually caught by guard 1. Guard 2 remains unobserved.
  - **Also learned**: a sim build could not command heat at all until two
    boot faults were scoped to `CONFIG_KILNCTL_SIM_PLANT` — the missing SX1509
    (`SAFETY_FAULT_SRC_APP`) and the missing RP2040 peer
    (`fault_on_link_loss`). Both correctly block relay-on globally on a real
    board; in a sim build the model *is* the actuator, so both are demoted,
    loudly logged, and impossible to reach outside a sim build. Separately,
    `SAFETY_FAULT_SRC_PC_LINK` blocks relay-on whenever the UART link is
    idle, so the test harness has to hold that link up the way an operator's
    GUI session would — the firmware was right, the first test run was wrong.
- **Guard 8 (cross-zone plausibility) implemented, shipped disabled
  (2026-08-12)**, closing the last unimplemented guard in TODO.md 6A.3.
  `thermal_guard.c` compares a zone's raw reading against every other zone's
  from the same tick snapshot — worst disagreeing peer, not an average, since
  with three zones an average lets one badly-wrong channel hide behind a
  healthy one — and trips only after a sustained window (600 s default)
  outside the band. It is per-zone: it blocks its own zone through the
  existing `relay_authority_zone_blocked()` path and asserts no global fault
  bit. **`cross_zone_max_delta_c` has no default and `profile_executor.c`
  sets it to 0, so the guard is inert as shipped**: TODO.md 6A.5 is explicit
  that this threshold should come from a measured cross-gain matrix, and no
  such matrix has ever been captured on hardware. The logic exists and is
  host-tested (122 checks now, all passing — a detached thermocouple in a
  coupled chamber trips it, a healthy coupled kiln at equal duty does not);
  only the number is missing.
  Writing the negative control produced a useful data point: two *healthy*
  zones driven at 1.0 and 0.6 duty settle more than 100 °C apart in the
  model, which is exactly why this cannot be a constant.
- **Guard 8 armable from config (2026-08-12)**: `cross_zone_max_delta_c` is
  now a per-zone field (*Cross-zone plausibility*) on Settings →
  Thermocouples & Zones, persisted in the `zones_cfg` blob, read back with
  `zones_config_get_cross_zone_delta()` and handed to the guard by
  `profile_executor.c`. The default is still 0 = disabled, for the same
  reason as before — the number belongs to a measured kiln, not to the
  firmware — but an operator who has measured one can now switch the guard
  on without a rebuild. `cross_zone_period_s` stayed a firmware constant
  (600 s) on purpose: one knob arms the guard, a second is easier to get
  wrong than to get value from. `z<i>_xzone` is the one *optional* field in
  `POST /api/zones`, so the MCP path and the existing test harnesses (which
  post the original 14 fields) are unaffected. Note the `zones_cfg` blob
  grew, and this module treats a wrong-sized blob as untrustworthy — the
  first boot after this change starts unconfigured and any saved zone config
  has to be re-entered.
  **Then observed firing on the board, 2026-08-12** — the first time guard 8
  has ever tripped on hardware. Two zones on one profile, threshold set to a
  deliberately tight 2 °C, `fault=tc_detached` on zone 1: trip at 600 s,
  `fault_guard: 9`, `"28.5C differs from zone 1's 20.5C by 8.1C (>2.0C) for
  600s"`. The run exposed two behaviours no host test showed: the comparison is
  symmetric, so on a *two*-zone kiln both zones trip on the same tick and the
  run escalates to `FAULTED` through the "every active zone individually
  faulted" path (isolating the bad channel needs three or more zones); and the
  threshold was latched at run start, so arming the guard mid-firing did
  nothing until the next start — which is what motivated the config-reload
  work below, landed the same day.
- **Config reload while running (TODO.md 6A.7) built and verified on-target
  (2026-08-12)**, closing the last structural gap in 6A.7. Settings-page edits
  used to be snapshotted at run start and ignored for the rest of a
  multi-hour firing. Now `zones_http.c` exposes a monotonic
  `zones_config_generation()` (bumped only where the in-RAM config really
  changes — a rejected POST does not move it), and `profile_executor.c`
  compares it once per tick, after that tick's readings are stored and before
  any control math so an edit can never be half-applied across the
  decide/apply split. On the unchanged path the comparison is the entire cost.
  Tuning lands bumplessly (`pid_seed_bumpless()` off the last commanded duty,
  or `pid_reset()` when the reading isn't valid — a bad measurement must not
  be baked into the integral); mode and relay-mask changes force relays off
  first (the old mask before adopting the new one, or those contacts stay
  closed under a mask no zone can name); heater timing applies at the next
  window with `heater_output_state_t` intact, so a `window_ms` edit can't
  machine-gun a contactor through the min-on/min-off timers; guard thresholds
  apply immediately and each is logged at WARN with old → new as an operator
  action — 6A.7 offered "require idle or log loudly" and loud logging is the
  right call for an operator correcting a ceiling for the ware in the kiln
  right now. A reload never touches `thermal_guard_state_t`: editing a
  threshold must not become an undocumented way to clear a latched trip.
  **Verified on the board**: editing `kp` mid-run left the run undisturbed,
  and dropping `max_temp_c` below the zone's current reading tripped guard 5
  on the next tick (`"20.0C >= max_temp_c 19.0C"`) — an edit that did nothing
  at all the day before.
- **Unowned-relay sweep (2026-08-12)**, a hole the reload work exposed: the
  only code that can *open* a relay is code that can still *name* its mask,
  so a stranded contact (a failed force-off, a mask edited away) had nothing
  to retry it but guard 9's stale-tick path. `profile_executor.c` now forces
  off, once per tick, the intersection of the expander's shadow (what is
  actually still closed), `claimed_relay_mask` (what *this run* ever
  commanded), and the complement of everything that may legitimately hold a
  relay — active zones whatever their fault state, plus any zone autotune is
  running on, since the two engines are mutually exclusive per zone and not
  globally. The `claimed` term is what keeps a "safety" feature from
  chattering off an operator's manually held damper or blower once a second;
  manual `/api/relay` and the UART bridge can still energize any relay
  mid-firing with no arbitration at all, which is now recorded in TODO.md as
  its own item.
- **Relay-feedback (Åström–Hägglund) identification, math only
  (2026-08-12)**: `pid_autotune_fit_relay()` recovers `{Ku, Tu}` with the
  hysteresis correction, fitting only trailing complete cycles and rejecting
  a trace that never reached a limit cycle; `pid_autotune_tune_from_relay()`
  adds the ZN and Tyreus-Luyben rules the enum has always carried. Host
  suite 125 → **154 checks, all passing**. Writing the test found the
  interesting part: the limit-cycle period is *not* the plant's ultimate
  period — the relay's own `-asin(h/a)` phase lag lengthens it by 28 % on
  this plant, so the expected value is solved from the same
  describing-function condition the fit assumes. No on-target relay test
  exists; nothing drives the relay in that pattern yet.
- **Feedforward from the identified plant model (TODO.md 6A.2) built
  2026-08-12** — the item that section calls the highest-value one for
  control quality, unblocked by the model persistence above.
  `u_ff = (T_sp - T_amb)/K_dc + (dT_sp/dt)*tau/K_dc`, with the rate taken
  from the segment's commanded ramp rather than a tick-to-tick difference of
  the target (at 1 Hz that difference is mostly timing noise, and it gets
  multiplied by tau on the way to the duty). Ambient is the MAX31856 cold
  junction sampled **once** at firing start — it warms with the board, so
  re-sampling would drift the hold term through a firing. Feedforward stays
  exactly 0 without an identified model, on a non-positive or non-finite
  `K_dc`/`tau`, outside PID mode, and on a tick with no trustworthy reading.
  **Building it exposed a real defect**: `pid_seed_bumpless()` solves
  `integral = (u_desired - P)/Ki`, which was exact only while feedforward was
  always zero — with it live the seed came back one whole ff term high, and
  on a hot kiln that is the largest term in the sum. Fixed at both call
  sites; the cleaner fix (moving the subtraction into `pid.c`) is recorded in
  TODO.md. Untested against a real kiln: a wrong `K_dc` from a bad fit now
  reaches the duty directly, bounded only by the clamp and the guards.
- **On-target relay-feedback autotune (TODO.md 6A.4) built 2026-08-12**:
  `AUTOTUNE_METHOD_RELAY` adds `RELAY_APPROACH -> RELAY_CYCLING` to the
  existing engine, reusing its lock, tick task, trace buffer,
  relay-authority claim and abort paths rather than growing a second set.
  Recording starts at the first switch, not on arrival, so the trace has no
  approach ramp to bias the midline; the relay law runs at 1 Hz so switch
  instants are not quantised to the 10 s sample period. The step test remains
  the default, and accepting a relay result writes gains but never a plant
  model — a relay test measures one frequency-response point, and a model
  from a previous step test must survive. Guard 3 has no off-window during
  cycling (the low branch is duty 0.15); guards 4 and 5 cover that case, and
  the alternative full-off branch would fire guard 3 on the ordinary case.
  **Never run on hardware** — with no thermocouples attached, every on-target
  autotune of either method aborts on guard 6 first.
- **Wi-Fi settings moved to their own flash partition, verified on hardware
  (2026-08-12)**: new `wifi_nvs` (0x187000, 24 KB) carved from free flash
  above the app, with `nvs`/`phy_init`/`factory` kept byte-identical so
  nothing already stored moves. The motivating bug was in our own code:
  `wifi_prov.c` answered an NVS recovery condition with a blanket
  `nvs_flash_erase()`, which wipes the whole default partition — so a corrupt
  profile blob could take with it the network access needed to fix it. Each
  partition now recovers independently.
  **What this session actually established about the board**: it had valid
  credentials all along. The boot log said "no saved credentials" and fell
  back to the AP, yet the one-time migration read the same default-partition
  namespace and found a working SSID *and* password, joining on the first
  attempt — so the two read paths disagree about what counts as provisioned.
  That inconsistency is now a TODO item; it is the actual reason the board
  appeared to be missing from the network.
  Persistence across a flash was then verified directly: reflashed
  bootloader + partition table + app, and the next boot joined straight from
  `wifi_nvs` with no migration line. The board answers at `kilnctl.local`
  (192.168.1.156). Stated plainly because it is the one case people assume
  wrongly: **a full-chip `esptool erase_flash` still destroys everything,
  including this partition.**
- **Reboot breadcrumb (TODO.md 6A.3's "no auto-resume") built and verified
  on-target (2026-08-12)**: new `App/drivers/run_state.{c,h}` persists a
  104-byte fixed record — profile, zone mask, segment, target, elapsed, phase,
  and the fault guard/reason if it ended badly — to its own NVS key, never
  inside the `zones_cfg` blob. Written on every meaningful transition plus a
  300 s refresh (~200 writes over a 12-hour firing, against ~43,000 if written
  per tick). A clean end is recorded as ended, so "I stopped it" is
  distinguishable from "the power went out"; a run left PAUSED is deliberately
  still interrupted. Served at `GET /api/profile_exec`'s `last_run` key with a
  dismissible dashboard banner and an ack endpoint. **Nothing acts on it** —
  there is no code path from a stored record to a run or a relay.
  Verified end to end on the bench unit: a run faulted on guard 6, the board
  was rebooted, and the record came back intact (profile name, segment,
  target, `fault_guard: 7`, reason, `uptime_at_write_s: 70` — uptime, not a
  fabricated wall-clock time, since the board has no RTC), then acknowledged
  cleanly. Costs +240 bytes of `.bss`.
  Building it also turned up a **pre-existing JSON truncation bug**:
  `/api/profile_exec` sized its buffer at 224 bytes per zone, but a fully
  escaped 95-char `fault_reason` needs ~190 B plus keys, so a multi-zone fault
  could truncate mid-object into invalid JSON — exactly when the dashboard
  most needs to render. Raised to 320/zone, and `last_run` is appended before
  the zones array so a verbose fault can never be what drops the breadcrumb.
- **Relative Gain Array computed and displayed (TODO.md 6A.5(c), 2026-08-12)**:
  `pid_autotune_rga()` (pure, host-testable) computes RGA = K .* (K^-1)^T for
  2x2/3x3 over the largest principal sub-block whose every cell is measured,
  refusing an incomplete matrix, a scale-aware singular one, or any non-finite
  entry — a wrong RGA would tell an operator their zones are independent when
  nobody has measured whether they are. Served on `/api/autotune/matrix` and
  rendered on the zones page as a verdict keyed to the worst actual diagonal
  element, in a potter's terms rather than a control engineer's. Host suite
  154 → **215 checks**. **Never fed real data**: no cross-gain cell has ever
  been filled, because every on-target autotune aborts on guard 6 with no
  thermocouples attached; on the board only the refusal path has been seen
  answering.
- **Fitted plant model persisted per zone (2026-08-12)**: autotune fit
  `{K, tau, L}` and discarded it at acceptance; it now goes to
  `zone_cfg_t` through `zones_config_set_model()` at the same point the gains
  go through `zones_config_set_pid()`, so `zones_http.c` stays the sole NVS
  owner. This is the prerequisite for 6A.2's feedforward term, the highest
  -value control-quality item left. Note this grew the `zones_cfg` blob
  again, so the stored zone config is discarded on the next boot — the second
  such wipe today, and worth batching any future zone-config field with it.
- **On-target session, 2026-08-12: sim build flashed and booting; the web
  UI could not be reached, and that is NOT caused by this session's work.**
  What was established, in order:
  - A `CONFIG_KILNCTL_SIM_PLANT` image was built and flashed over JTAG
    (`flash_firmware()`, verify OK) and the board boots and runs it.
  - **The board's PC-link log was 100 % saturated** by the safety link
    retrying an absent RP2040 peer: one `ESP_LOGW` per retry, ten retries per
    message, ~2.5 messages/s — about 97 warning lines every 5 s, with
    `uart_log_bridge` reporting a "log line(s) dropped (queue full)" notice
    between nearly every pair. Every other log line on the board, including
    the entire `app_main` bring-up sequence, was being discarded.
    **Fixed** in `App/drivers/espInterfaces/uart_protocol.c`: the retry
    warning is now rate-limited to one line per 5 s per protocol instance,
    carrying a `(+N more suppressed)` count. Retry *behavior* is untouched —
    only how often it is reported. **Live-verified**: the log went from
    unreadable to one clean line per 5 s (`+96 more suppressed`).
  - With the log readable, the console UART showed
    `E app_main: Failed to start thermo uart bridge task` at boot. The old
    message could not distinguish a task-registration refusal from
    `ESP_ERR_NO_MEM`, so `App/main.c` now logs the actual `esp_err_t` plus
    free heap and largest free block. **Not yet observed with the new
    message** — see the log-bridge note below.
  - **The web UI is unreachable on this bench unit.** The fallback AP
    (`kilnCtl`) beacons and a client associates and gets a DHCP lease
    (192.168.4.2, gateway 192.168.4.1), but ICMP gets no reply and TCP :80
    never completes a handshake; the AP also deauthenticates the client every
    ~15–20 s. **This reproduces identically on a production (sim-off)
    build**, so it is a pre-existing condition, not a regression from the
    simulated-plant work — and it is the likely real identity of the "Wi-Fi
    flapping" logged on 2026-08-11. Root cause is still open. The board is
    NOT cycling station joins while this happens (no `wifi_prov` activity in
    the log at all), so "AP+STA channel contention" is ruled out.
  - **The log bridge now tees to the console UART**
    (`CONFIG_KILNCTL_LOG_TEE_CONSOLE`, default y). It previously *replaced*
    console logging, so any line its queue dropped was gone for good — and it
    reliably drops ~40 lines during the boot burst, which is exactly where
    the diagnosis lived. This is the change that made the root cause visible;
    turn it off only if a logging-heavy path needs the microseconds back.
  - `UART_LOG_BRIDGE_QUEUE_LEN` 64 → 192 was tried (the boot backlog reliably
    overruns 64, losing exactly the bring-up lines) and **reverted the same
    session**: the PC link stopped answering afterward, and since the link is
    also the only way to observe the log, there was no way to tell whether
    the bump caused it. The reasoning and the retry conditions are recorded
    in a comment at that constant.
  - Board left on a **production (sim-off) image** with the two logging fixes
    above; the PC link answers `get_fw_version` normally.
  - **RESOLVED the same day — root cause was DRAM exhaustion, and the fix is
    live.** With the console tee above in place, the previously-dropped line
    read: `E app_main: Failed to start thermo uart bridge task:
    ESP_ERR_NO_MEM (free heap 7176 B, largest block 4608 B)`. The board was
    reaching `app_main` with **7 KB of free heap**, so `xTaskCreate` failed,
    `esp_http_server` could not allocate, and the Wi-Fi driver could not hold
    a client association — the "AP deauthenticates every 15–20 s" symptom was
    the same starvation, not RF.
    `idf.py size-files` named the two culprits: `autotune_engine.c` held
    **70 KB** of `.bss` (a `[3][2880]` trace of 8-byte samples) and
    `profile_executor.c` **59 KB** (2880 × 20-byte history entries) — ~130 KB
    of a ~196 KB DRAM budget, claimed before Wi-Fi started. Both are now
    packed, with the unpacked shapes kept at the API boundary so no caller
    changed:
    - history: 20 B → **8 B** per sample (elapsed in 30 s units, temperatures
      as deci-degC `int16`, duty as whole percent), 57.6 KB → 23 KB;
    - autotune trace: 8 B → **2 B** per sample (time implicit in the index,
      deci-degC `int16`) and the sample period 5 s → 10 s (a kiln's time
      constant is ~10,000 s; a FOPDT fit cannot see the difference), 69 KB →
      ~8.6 KB. `finalize_fit()` unpacks one zone's row into a heap scratch
      buffer for the duration of a fit, which is affordable precisely because
      the packing freed far more than it borrows.
    **Verified live over the fallback AP**: boot log now shows
    `wifi_prov_http: provisioning HTTP server up` and every API coming up; all
    five pages return 200; `/api/status`, `/api/zones`, `/api/profiles`,
    `/api/profile_exec`, `/api/control`, `/api/rules`, `/api/history.csv`,
    `/api/autotune`, `/api/autotune/matrix` all answer correctly with no
    hardware attached; `/api/sim` correctly 404s in a production build; and
    the association held with 8/8 successful requests over 40 s with no
    deauthentication.
  - **Consequence for TODO.md 6A.8**: the on-target guard-matrix run — flash
    a sim build, provoke each guard over `POST /api/sim`, watch it trip — is
    now unblocked but has still not been performed. Nothing in
    `docs/GUARD_TEST_MATRIX.md` has been observed on hardware yet.
- **`docs/GUARD_TEST_MATRIX.md` written (2026-08-12)**, TODO.md 6A.8's
  "test matrix, written down" artifact: one row per guard 1–9 with what it
  catches, how it is provoked, threshold/window, expected time to trip,
  whether it escalates globally or per-zone, and the exact
  `fault_reason`/`fault_guard` an operator sees — plus an explicit coverage
  section separating what a host test asserts from what is only read off the
  source (escalation, guard 9, and anything involving real hardware are in
  the latter group). Writing it surfaced a real reporting wart, documented
  there and not yet fixed: `/api/profile_exec` exposes the raw
  `thermal_guard_trip_t` enum as `fault_guard`, so the numbers do not match
  the guard numbers used everywhere else — `fault_guard: 7` means guard 6,
  guard 7 reports 8, and guard 5 reports 5 or 6 depending on which limit was
  crossed.
- **PID autotune, step-test path (TODO.md 6A.4) built and live-verified
  (2026-08-11)**: `App/drivers/pid_autotune.c` (pure FOPDT two-point fit +
  SIMC tuning-rule math, host-validated against `sim_plant.c`'s ground
  truth) and `App/drivers/autotune_engine.c` (on-target
  IDLE->SETTLING->STEPPING->DONE/ABORTED state machine, 1Hz, owns the
  zone's relay through `relay_authority_zone_blocked()`, full thermal_guard
  suite armed, 4h time budget, RAM trace + CSV download). New endpoints
  `/api/autotune`, `/api/autotune/start|abort|accept`,
  `/api/autotune/trace.csv`; a control panel added to `/settings/zones`.
  Results are proposed only -- `POST /api/autotune/accept` is the only path
  that writes gains, through a new `zones_config_set_pid()` setter that
  keeps `zones_http.c` the sole NVS owner. Mutual exclusion with the
  profile executor is enforced both directions. Live-verified: all pages
  still serve 200, `GET /api/autotune` returns a clean idle status, `POST
  /api/autotune/start` correctly refuses with "zone has no relay mask
  configured" against the unconfigured board (no thermocouple/relay
  hardware attached). NOT built: relay-feedback (Astrom-Hagglund)
  identification, gain-scheduling bands, cross-zone coupling logging during
  a run -- see TODO.md 6A.4 for the itemized list.
- **`SX_WRITE_REG`/`SX_SET_DIR` relay-authority bypass closed (2026-08-11)**:
  the last open gap in `docs/SAFETY_MODEL.md`. These raw-register debug
  subcommands wrote straight to the SX1509 through `kiln_io->exp`, bypassing
  `kiln_io_set_relay_mask()` and therefore `relay_authority` entirely.
  Closed with a per-pin guard in `uart_bridge.c` (not a blanket refusal --
  the other 12 expander pins stay reachable for debug): `SX_WRITE_REG` is
  refused when it would set a relay pin's RegData bit high while
  `relay_authority_on_blocked()` says no; `SX_SET_DIR` is refused
  unconditionally when it would flip a relay pin to an input (an input
  relay pin can't be commanded off either, so this one isn't fault-gated).
  New `kiln_io_relay_pin_mask()` getter keeps the pin map's one owner in
  `kiln_io.h`. Rebuilt, reflashed, all pages re-verified live.
- **pc_tools GUI gains a "Firing Status" popup (2026-08-11)**: read-only view
  of `/api/profile_exec` and `/api/autotune` (control mode, target/actual
  temp, duty, guard fault, autotune fit/proposed gains), polled every 3s the
  same way the existing Safety popup polls over UART, but here over HTTP to
  the board the way the Wi-Fi Settings popup already does. Deliberately
  read-only -- starting/stopping a firing or autotune run, and accepting
  autotune's proposed gains, stays a web-dashboard action; this closes the
  "the Python GUI shows nothing about the new zone control_mode/duty/guard
  telemetry" gap for *viewing* it. `pc_tools/src/kilnctrl/gui.py` compiles
  clean (`python -m py_compile`); not yet exercised against a live board
  from this GUI (same hardware-absence caveat as everything else this
  session).
- **TODO.md 6A.9 history/telemetry slice built and live-verified
  (2026-08-11)**: the history ring buffer section 0 designed but never
  built (`{elapsed_s, actual_c, desired_c, duty, guard}`, 30s/sample, 24h,
  single-zone) now lives in `profile_executor.c`, exported as
  `GET /api/history.csv` and graphed on the dashboard (`main_page.html`'s
  new canvas chart, no external library, guard trips marked on the
  timeline). `pid.c` grew `pid_update_terms()` (P/I/D/FF breakdown,
  `pid_update()` now a thin wrapper over it) backing a new
  `GET /api/control` endpoint. **A real bug was found and fixed while
  live-testing this**: the first version of both new CSV endpoints
  (`/api/history.csv` and `/api/autotune/trace.csv`) allocated one large
  buffer up front and `/api/history.csv` came back `500 out of memory`
  against the actual board's runtime-free heap -- a pre-flash size-report
  check of static DIRAM headroom looked fine and didn't catch this, since
  the failure was against the separate runtime heap already carrying
  Wi-Fi/lwIP/httpd. Fixed by paginating the underlying getters
  (`profile_executor_get_history()`, new `autotune_engine_get_trace()`)
  and streaming both CSV responses via `httpd_resp_send_chunk()` in small
  batches instead. Rebuilt, reflashed, all pages and both CSV endpoints
  re-verified live returning `200`. **Not built**: per-zone history (still
  single-zone, matching 6A.5's unbuilt concurrent execution), ramp-lock
  markers on the graph (ramp-lock itself doesn't exist), and per-relay
  `window_ms`/`min_on_ms`/`min_off_ms` becoming page-configurable -- this
  landed too, see below.
- **Per-zone relay timing config (2026-08-11)**: `zone_cfg_t` grew
  `heater_window_ms`/`heater_min_on_ms`/`heater_min_off_ms` (0 = use the
  firmware default), a `zones_config_get_heater_cfg()` getter, and both
  `profile_executor.c` and `autotune_engine.c` now substitute a zone's
  configured values instead of the fixed 60000/2000/2000ms constants. UI on
  `zones_page.html` (not `rules_page.html` -- heater timing is applied per
  zone, not per individual relay, in the actual control code, so that's
  where it was wired up; noted as a deviation from TODO.md 6A.9's literal
  wording in the itemized note). Rebuilt, reflashed, `/api/zones` verified
  live carrying the three new fields.
- **Concurrent multi-zone execution + ramp-lock (TODO.md 6A.5(a)+(d)) built
  and live-verified end to end (2026-08-11)**: `profile_executor.c` rewritten
  from a single `zone_index` run to a `profile_t.zone_mask`-driven array of
  `MAX31856_CHANNEL_COUNT` independent per-zone control/guard runtimes
  (`zone_runtime_t`), sharing one ramp/dwell schedule (`target_c`,
  `segment_elapsed_s`) that only advances while every active, not-yet-
  faulted zone is within `PROFILE_EXECUTOR_RAMP_LOCK_BAND_C` (25C) of it
  (ramp-lock, 6A.5(d)). A per-zone guard trip (guards 1/2/4/7) drops only
  that zone and the run continues for the rest; a global trip (guards
  3/5/6), or every active zone individually faulting, faults the whole run
  -- same escalation policy as before this pass, now applied per-zone
  within one run instead of being the whole run's only zone.
  `profiles_http.c`'s `profile_t.zone_index` became `zone_mask` (a profile
  can now target more than one zone); `autotune_engine.c`'s mutual-exclusion
  check against the profile executor became per-zone
  (`profile_executor_zone_is_active()`) instead of a blanket "any profile
  running" refusal. `/api/profile_exec` and the new `/api/control` both grew
  a `zones` array plus shared `ramp_lock_held`/`ramp_lock_lagging_mask`
  fields; `main_page.html`'s exec card, `profiles_page.html`'s zone selector
  (checkboxes now, not a single `<select>`), and the pc_tools Firing Status
  popup were all updated to match.
  **Found and fixed a second real bug while live-testing this**: the HTTP
  server's `max_uri_handlers` (bumped to 24 earlier this session) turned out
  to be exactly one shy of the true total once this pass's new
  autotune/control/history routes were counted -- registration order is
  wifi_prov(5) -> dashboard_http(14) -> zones_http(3) -> rules_http(3) ->
  profiles_http(5) = 30, so `profiles_http.c`'s routes (registered last)
  all failed silently (logged, not fatal) and `/profiles` + `/api/profiles`
  came back `404` against the live board. Bumped to 40. Live end-to-end
  verification after the fix: configured 2 zones, created a profile with
  `zone_mask=3`, started it, watched both zones run concurrently and
  independently trip guard 6 (sensor invalid -- no thermocouple hardware
  attached) on their own schedules, confirmed the whole run correctly
  escalated to `FAULTED` only once *both* zones had individually faulted,
  and confirmed `profile_executor_halt()` recovered cleanly to `idle`.
  **NOT built at this point**: coupling-matrix capture during autotune
  (6A.5(b) -- landed next, see below), the RGA display (c), the static
  decoupler (e), electrical load staggering, and guard 8 (cross-zone
  plausibility).
- **Cross-zone coupling-matrix capture during autotune (TODO.md 6A.5(b))
  built (2026-08-11)**: `autotune_engine.c` already called
  `MAX31856_read_all()` (whole-bus read) every control tick but only kept
  the reading for the zone under test -- the fix records every configured
  zone's reading into its own trace row (`zone_trace[MAX31856_CHANNEL_COUNT][AUTOTUNE_ENGINE_MAX_SAMPLES]`)
  at the same tick, at zero extra SPI cost. When a run's fit succeeds,
  `finalize_fit()` now also fits every other zone's row against the same
  duty step (`pid_autotune_fit_fopdt()` -- the existing pure two-point-method
  function, called once per zone, no new identification math) and writes
  the results into a new `autotune_coupling_matrix_t` global, one row per
  tested zone, RAM-only (lost on reboot, matching the trace's own
  lifetime). A zone whose baseline wasn't valid when STEPPING started (no
  sensor, or outside `thermo_count`) is left `valid:false` in that row
  rather than fit against garbage; a momentary bad read on a non-tested
  zone mid-run carries forward its last valid value so it can't shift that
  zone's trace out of alignment with the tested zone's `t_s`. Exposed as
  `GET /api/autotune/matrix` (all cells, small enough to send as one JSON
  object -- no pagination needed unlike the trace/history endpoints) and a
  table on `zones_page.html`, polled every 5s. Host tests (88/88, unchanged)
  re-run and confirmed passing -- the coupling logic lives entirely in
  `autotune_engine.c`, which is deliberately impure/on-target-only per this
  module set's own design (see `docs/PID_CONTROL.md`'s module table), so
  there was no new pure math for the host harness to cover; it reuses
  `pid_autotune_fit_fopdt()`, which `test_pid_autotune.c` already validates
  against `sim_plant.c` ground truth.
  Rebuilt, reflashed via OpenOCD (first attempt hit the usual benign
  "Verify Failed", second attempt succeeded), `GET /api/autotune/matrix`
  and `/settings/zones` both verified live returning `200`. Ran a real
  1-zone step test against the bench unit (2 zones configured via
  `POST /api/zones`, `POST /api/autotune/start`) to confirm the new code
  path doesn't crash or otherwise misbehave: it aborted on guard 6 (sensor
  invalid) after 1s, as expected with no thermocouple hardware attached,
  and `GET /api/autotune/matrix` correctly stayed all-`valid:false`
  afterward (an aborted run never reaches `finalize_fit()`). Config reset
  to `thermo_count=0`/`relay_count=0` afterward; dashboard confirmed still
  responding.
  **What "built" does NOT mean**: this has never actually filled a matrix
  cell against a real cross-gain, since no thermocouple daughterboard is
  attached to the bench unit and every live autotune run aborts before a
  fit happens. The capture code, the per-zone fit call, and the JSON/UI
  plumbing are all logic-verified and exercised by the same guard-trip path
  (a) already proved works; the actual numbers a real coupled kiln would
  produce remain unverified until sensor hardware exists.
  **Still NOT built**: the RGA display (c) and static decoupler (e) --
  both need real matrix data, not just the capture code, so they stay
  blocked on hardware, not on more firmware work -- and guard 8's proper
  threshold (same hardware block).
- **Electrical load staggering, phase-offset half only (2026-08-11)**:
  `heater_output_seed_phase()` added to the pure `heater_output.c` module
  (truncates a zone's first time-proportioned window and forces it OFF so
  the window boundary lands a fixed offset earlier, permanently -- every
  window after the first keeps the normal period, so the shift isn't a
  one-time transient). Wired into `profile_executor.c`: each PID-mode zone
  in a run gets `phase_offset_ms = rank * window_ms / n_active_zones`
  (rank = 0-based position among *this run's* active zones, not raw zone
  index). Host-tested, 96/96 pass -- and writing the new tests caught a
  real pre-existing bug in the test file itself: several
  `heater_output_state_t` locals were declared without zero-initializing,
  relying on undefined behavior (reading `state->cycle_count` before
  `heater_output_reset()`'s memset, to preserve it) that happened to be
  harmless by luck until this pass's new test blocks shifted the stack
  layout enough to flip that luck. Fixed by zero-initializing every
  declaration. **NOT done**: the `max_simultaneous_relays` cap -- the
  harder half of this TODO item, deliberately not rushed (capping
  correctly means either changing what the pure module computes or
  overriding its output in a way that would desync its internal
  `relay_on`/`cycle_count` bookkeeping from the real hardware state;
  neither was worked out with confidence this session, so it's left for a
  dedicated pass). **Live hardware round-trip not completed**: the bench
  board's Wi-Fi was flapping badly for the second half of this session
  (up for seconds, unreachable for tens of seconds, repeatedly).
  Diagnosed via a GDB backtrace taken mid-flap: every task, including
  `httpd` and `wifi`, was parked in a normal blocking wait -- no crash, no
  deadlock -- and `wifi_prov.c`'s disconnect/reconnect handler was
  re-read and is correct. Conclusion: environmental/RF, not a firmware bug
  from this session's diff (nothing here touches Wi-Fi/HTTP infrastructure).
  Also found and fixed a real **tooling** bug while chasing this: repeated
  raw `System.IO.Ports.SerialPort` opens against the board's console UART
  (COM3) left DTR/RTS asserted, which this board's auto-program circuit
  reads as GPIO0 held low -- every JTAG reset issued after that stranded
  the chip in UART download mode instead of booting the app (matches
  several serial captures this session that printed exactly one ROM
  banner line then went silent). Fixed by explicitly setting
  `DtrEnable=false`/`RtsEnable=false` before every reset; boot has
  completed cleanly every time since. Firmware builds clean and is
  host-tested; phase-offset itself remains logic-verified only, not yet
  exercised against a real multi-zone PID run over HTTP.
- **`wifi_provision_http.c`'s shared httpd worker task stack raised
  4096 -> 8192** (2026-08-11), found while live-testing the larger
  `POST /api/zones` body this pass added: the request was intermittently
  hanging rather than crashing outright, the usual signature of stack
  pressure on `esp_http_server`'s shared worker task. Root cause not
  conclusively proven (no live crash backtrace was captured), but the bump
  made repeated live POST testing reliably stable afterward — a low-risk,
  well-precedented fix given this codebase already sizes other task stacks
  generously, but flagged here as unverified-root-cause/confirmed-effective.
- **Bootloader-wipe incident and recovery (2026-08-11), and a new
  `flash_firmware()`/`kill_openocd_sessions()` MCP tool pair to prevent a
  repeat.** While chasing an unrelated Wi-Fi flakiness issue, a manual
  `flash erase_sector 0 0 last` (intended to clear one region) erased the
  *entire* chip — bootloader and partition table included — and the
  follow-up reflash only rewrote the app image at 0x10000, leaving the
  board unable to boot (`invalid header: 0xffffffff` on the console UART,
  confirmed live, not a stale buffer). Recovering it took: a full
  bootloader+partition-table+app reflash (still failed verify identically
  across several attempts, including at a deliberately slowed JTAG clock);
  discovering via `flash info`/`flash erase_address unlock` that flash
  sectors 0-8 reported as write-protected and OpenOCD's own unlock request
  failed outright (`failed setting protection for blocks 0 to 8`); and
  finally a full USB power cycle of the board (not just a JTAG/soft reset),
  after which a clean erase + 3-image reflash succeeded immediately and the
  board booted normally. Conclusion: a soft/JTAG reset does not reliably
  clear a SPI-flash write-protect state stuck from a botched low-level
  flash operation; only removing power does.
  To make sure nobody (agent or human) improvises that raw command sequence
  again, added `flash_firmware()` and `kill_openocd_sessions()` tools to
  `pc_tools/src/kilnctrl/mcp_server.py` — the existing UART-control MCP
  server, now also exposing a JTAG-only flash tool (still never
  esptool/`idf.py flash`, per this project's standing rule). It always
  writes bootloader (0x0) + partition table (0x8000) + app (0x10000) via
  `program_esp ... verify` (which only erases/rewrites a region that
  doesn't already match — never a bare full-chip erase), auto-retries once
  on a failed verify (the documented benign single-retry-fixes-it quirk
  seen many times this session), kills any stray `openocd.exe` first (a
  second instance can't open the JTAG USB device, which looks identical to
  "board not responding"), and on a genuine persistent failure returns the
  real openocd output tail plus an explicit warning against a manual
  `erase_sector` recovery, rather than a generic error. Found and fixed a
  real bug in the tool itself while smoke-testing it: the first version
  required the literal string "Verify OK" in the output to call a run
  successful, which false-negatived on a legitimate success where a
  region's content already matched the file (that path skips straight to
  "Resetting Target" without ever printing "Verify OK") — fixed to key off
  `returncode == 0` and the absence of `"Verify Failed"`/`"Error:"`
  instead. Smoke-tested standalone (not through the MCP transport, calling
  the underlying functions directly via the venv's `python -c`) and
  confirmed both a real reflash and the fixed success-detection path work.

## Explicitly NOT done — do not assume otherwise

- **Most of the board's actual sensing/driving hardware has never run.**
  This is narrower than it used to be: as of 2026-08-10/11, the bare
  ESP32-S3 plus Wi-Fi and the safety-link opto-isolator *have* run on real
  silicon and been verified live — Wi-Fi provisioning (AP fallback, station
  join, mode persistence across reflash, see TODO.md section 1; the
  `local_only` flag this originally shipped with was replaced 2026-08-11 by
  an explicit home/AP mode toggle, see section 1's note),
  the dashboard's `GET /api/status`/`POST /api/relay` correctly reporting
  hardware-absent and refusing rather than crashing (TODO.md section 2),
  and, as of 2026-08-11, all five web page routes plus the
  zones/rules/profiles JSON APIs returning correct hardware-absent stub
  responses (`/`, `/profiles`, `/settings/zones`, `/settings/relays`,
  `/wifi` — see TODO.md section 2's 2026-08-11 note). What has genuinely
  **not** run, because the parts are physically absent from this bench
  build: the MAX31856 thermocouple daughterboard, the SX1509 relay/IO
  expander, the ILI9488 display, and the RP2040 safety-processor peer (the
  safety *link* on the ESP side degrades to "no peer" correctly, but
  nothing has ever answered it). The IO bridge's GPIO-ISR/queue-set path,
  the auto-report timers for real sensor data, and the SPI-bus-sharing
  arrangement between MAX31856 and ILI9488 remain unverified against real
  silicon until that hardware is reattached.
- **The RP2040 safety-processor firmware does not exist.** The ESP-side
  `safety_link.c` and the wire contract for it (`uart_task_ids.h`, `SAFETY`
  task) are written and documented so a Pico firmware has something concrete
  to implement against (see the payload contract at the top of
  `App/drivers/safety_link.h`), but nothing runs on the Pico today. The link
  degrades to "no peer" correctly (fail-safe fault assertion, not a hang),
  which is the one part of this gap that's already handled well.
- **Live thermocouple faults do not gate the relays — partially closed
  2026-08-11, for one specific case.** Previously, only a boot-time failure
  of the whole SPI bus or every channel gated anything. As of TODO.md
  section 6A's first slice, **while `profile_executor` is actively running
  a zone**, `App/drivers/thermal_guard.c`'s guard 6 trips on
  `spi_failed`/`NaN`/`THERMO_FAULT_OPEN`/`OVUV`/`TCRANGE` after 3 consecutive
  bad reads and blocks relay-on globally via a live-broadened
  `SAFETY_FAULT_SRC_THERMO` — live-verified end to end on the real board
  (see the build note above). **The general case is still open**: a channel
  that opens or goes out of range while no profile is running that zone —
  including a direct `THERMO_CMD_READ` over the PC/MCP UART link, or the
  dashboard's `/api/status` view outside of a running profile — is still
  only reported, not acted on. See the updated "What does NOT enforce it
  yet" section in `docs/SAFETY_MODEL.md` for the precise scope of what
  closed and what didn't.
- **The safety processor cannot influence the main board's relays.** An
  E-stop or fault reported back from the (nonexistent) Pico firmware over
  the isolated link is visible in `SAFETY_CMD_GET_STATUS` and does not
  currently drop anything on the main board. Isolated-fault-line traffic
  today is one-directional, ESP → Pico only.
- **Raw expander debug commands (`IO_CMD_SX_WRITE_REG`, `SX_SET_DIR`) bypass
  the relay-on gate.** They write the SX1509 directly and are not routed
  through `kiln_io_set_relay`. See `docs/SAFETY_MODEL.md`.
- **Display connector pin identity is unresolved.** `docs/HARDWARE.md` and
  `docs/ILI9488.md` document a real disagreement between this board's
  schematic net names and every found BIGTREETECH TFT35 SPI pinout over
  which J2 pin is D/C vs. the touch IRQ, and whether SDA/SCL are swapped.
  Handled with a menuconfig switch (`KILNCTL_DISPLAY_SWAP_DC_RESET`)
  defaulting to the module's reading, not resolved. Also unresolved: the
  module's touch controller is more likely an NS2009 (I2C) than the XPT2046
  (SPI) commonly claimed for it — moot for this firmware, which drives the
  panel only and does not support touch.
- **`SX_SET_INT_MASK`'s sense field cannot express per-pin edge selection.**
  The frozen wire contract specifies 16 bits (2 per pin *pair*); the part
  wants 32 (2 per pin). The bridge expands each pair's mode onto both its
  pins, which is well-defined but coarser than the hardware supports.
  `SX_WRITE_REG` against 0x14–0x17 is the workaround; fixing it properly
  needs a wire-protocol v3 field, not a firmware change.
- **No refusal signal on the wire.** A safety-refused relay command produces
  no reply, success or failure — see `docs/UART_PROTOCOL.md`. The GUI's
  relay state is read back from live auto-report so a refusal is visible
  within one report period, but there is no explicit "refused, here's why"
  the UI could turn into a clear message.
- **No *on-target* automated test suite.** There is now a host-side one
  (`App/test/`, 120 checks as of 2026-08-12, covering the pure control
  modules against a coupled multi-zone sim with injected faults — see the
  entry above), plus `selfcheck.py` on the PC side (492 checks) and the
  ESP-IDF build itself. What does not exist: any test that runs on the
  ESP32-S3, and any pytest config in `pc_tools`. A guard that passes in the
  sim has been shown to be *logically* right, not that it fires against real
  silicon — only guard 6 has ever tripped on the actual board.

## Suggested order for what's next

1. Flash a `CONFIG_KILNCTL_SIM_PLANT` build and walk a whole firing on the
   board with no kiln attached: start a profile over HTTP, watch the ramp
   and the dashboard graph, then inject each fault over `POST /api/sim` and
   confirm the right guard trips, escalates, blocks the right scope, and
   clears. This is the cheapest way to turn `docs/GUARD_TEST_MATRIX.md`'s
   "reasoned about" rows into observed ones, and it needs no hardware that
   isn't already on the bench. Reflash a production (sim-off) image
   afterward — a sim build reports fabricated temperatures.
2. Flash the board and validate the boot sequence, the SPI bus sharing, and
   the relay-on gate end to end (command a relay on, unplug USB, watch it
   drop and the fault line assert) before trusting any of this near an
   actual kiln.
3. Decide the design questions in `docs/SAFETY_MODEL.md` for live
   thermocouple-fault gating, then implement it — this is the gap most
   worth closing before unattended operation.
4. Write the RP2040 safety-processor firmware against the contract in
   `App/drivers/safety_link.h`, using a PIO UART or plain hardware UART per
   `docs/SAFETY_LINK.md` (no inversion needed on that side).
5. Decide whether a safety-processor-reported fault/E-stop should drop the
   main board's relays, and if so, add the isolated-link path for it.
6. Confirm the J2 pin identity against the physical connector and drop the
   now-unneeded `KILNCTL_DISPLAY_SWAP_DC_RESET` ambiguity from the docs.

## 2026-08-13 update

This doc had drifted well behind TODO.md's own dated notes (which cover
2026-08-13's NVS partitioning/versioning work, the readiness wizard, and the
saved-networks list) — a full re-sync is its own task, not attempted here.
This entry only covers what changed in the same pass that did the sync,
found and fixed while reconciling TODO.md's checklist against the actual
code:

- **`esp_wifi_set_config(AP) failed: ESP_ERR_WIFI_MODE`, observed on every
  boot since 2026-08-12, root-caused and fixed.** `wifi_prov_start()` (and
  two more call sites with the identical pattern, `ap_fallback_timer_cb()`
  and `wifi_prov_set_mode()`'s AP branch) called `apply_ap_config()` —
  `esp_wifi_set_config(WIFI_IF_AP, ...)` — before `esp_wifi_set_mode()` had
  ever put the driver into a mode that includes the AP interface. Fixed by
  reordering all three: mode first, config only on success. Boot log
  confirmed clean (no `ESP_ERR_WIFI_MODE` line) after the fix.
- **A real NVS persistence bug found and fixed in `zones_http.c`.** Every
  module using the 8.2 version/size-check pattern checked blob size *before*
  version, which made the version-based migration path dead code for the
  one case it exists for (a struct that grows). Fixed in `zones_http.c`
  (version decides first now); the same latent bug still exists unfixed in
  `rules_http.c`/`relay_cycles.c`/`profiles_http.c` — flagged, not touched,
  since nothing in this pass grew any of those three structs.
- **`pid_seed_bumpless()` now takes `ff_u` and subtracts it internally**
  (previously the caller pre-subtracted it, per TODO.md 6A.2's own
  "move it into pid.c" item). Host tests updated and passing (216/216).
- **Two new PID/executor diagnostics**: `cooling_limited` (per zone, in
  `/api/control` — a PID-mode zone stuck at duty 0 while still hot, TODO.md
  6A.2's "report cooling-limited" bullet) and a WARN log when
  `max_ramp_c_per_hr` is lowered mid-firing below what the running segment
  needs (TODO.md 6A.7).
- **New run-level policy, `continue_on_zone_trip`** (`zones_cfg_t`, default
  `false`): a per-zone thermal guard trip now aborts the whole multi-zone
  firing by default, matching TODO.md 6A.3's explicit request; the previous
  behavior (other zones kept running) is preserved only as an explicit
  opt-in. `ZONES_CFG_VERSION` bumped 1→2, migration verified by inspection
  (a v1 blob reads the new field as its zero/abort default with no explicit
  conversion step).
- **Status**: all of the above build-verified (`ninja -j 24`, clean) and
  flashed to the bench unit over OpenOCD/JTAG; the board boots and answers
  the UART protocol correctly after each flash. None of it has been
  exercised against real behavior — no thermocouple daughterboard or relay
  expander is attached to this bench unit, so nothing here has actually
  tripped a guard, run a firing, or been watched cooling-limited on real
  hardware. `docs/SAFETY_MODEL.md` was updated in the same pass to match
  (relay-authority's real caller count, `relay_authority_zone_blocked()`,
  and this policy).
- **TODO.md 6A.3's remaining named guard thresholds promoted to per-zone
  override (2026-08-16)**: `thermal_guard_cfg_t` grew 8 fields (wrong-dir
  rate/window, off-settle, runaway rate/margin, drift period, sensor
  debounce count, frozen window), each following the existing
  `sanity_rate_c_per_min` convention — 0 substitutes `thermal_guard.c`'s own
  firmware-wide default (`effective_f()`/`effective_ticks()`), not the
  opposite disable-on-zero convention `cross_zone_max_delta_c` uses.
  `zone_cfg_t` stores them (`ZONES_CFG_VERSION` 2→3, same auto-migrating
  zero-fill as every prior blob growth), a new bundled getter
  `zones_config_get_guard_thresholds()` reads them, and both
  `profile_executor.c` (run start, and the loud per-field mid-firing reload
  path) and `autotune_engine.c` (an autotune run arms the full guard suite
  too) wire them in. `zones_page.html` gained a `<details>` "Advanced guard
  thresholds" disclosure per 6A.9's own note about the page needing to
  become collapsible for this reason.
  Also found while wiring this: `relay_authority`'s per-relay ownership
  arbitration (TODO.md section 0's "does a manual command fight a running
  profile over the same relay") was already fully implemented —
  `relay_authority_claim_mask()`/`_release_mask()` at profile
  start/pause/resume/halt, checked by
  `relay_authority_manual_blocked_by_owner()` in both `uart_bridge.c` and
  `dashboard_http.c` — but TODO.md's checkbox for it was stale, still
  reading unbuilt. Corrected the checkbox rather than re-doing the work.
  **Status**: host-tested (`test_thermal_guard.c` grew 3 checks covering the
  debounce/frozen-window/runaway-margin overrides, 221/221 passing) and
  `idf.py -C firmware/KilnFW build` clean. **Not flashed, not exercised on
  hardware** — no thermocouple/relay hardware attached to observe an
  override actually change what trips a real firing.

## 2026-08-19 update — single-writer ownership tasks, and the PC-link UART found dead

**New bench fact that changes how to read almost every "verified" claim
above and below: the PC↔ESP command UART link was found dead this
session.** This is a *different* break from the already-known Pi↔ESP
safety-link fault (ROADMAP.md M0) — it is the USB-serial link `pc_tools`/MCP
use for THERMO/IO/DISPLAY/SAFETY/SYSTEM commands and for the console log
tee. With the board present, powered, and answering fine over JTAG/OpenOCD
(chip examines, halts, reports PC normally), every UART command
(`get_fw_version`, `gpio_probe_read_all`, etc.) timed out with "no reply
after all retries," even after `disconnect`/`connect` and a JTAG
`reset(run)`. See `ROADMAP.md` M1 for the bench log. Practical effect: this
session could build, flash, and boot-smoke-test firmware, but could not
exercise almost anything over the wire — no console log, no
`get_fw_version`, no live Wi-Fi status, no THERMO/IO round-trip. Read every
"flashed" claim below as build-verified plus, at most, a JTAG-observed
liveness check — not as a UART-confirmed round-trip, regardless of how
earlier entries in this file used that word.

Also true this session, restated because it changes what "not built" means
for the thermocouple driver work: **the MAX31856 thermocouple ICs are
physically not connected** on this bench unit (the daughterboard itself,
not just the main board) — a narrower and more specific fact than "no
thermocouple hardware attached," since even a bus without conversions would
exercise the SPI transaction path differently than a bus with nothing on
it at all.

**Four pieces of ownership/single-writer architecture landed
(`firmware/KilnFW/TODO.md` section 10.14, all four phases, commits
`6aa4adf`/`ea3dbc8`/`1a36155`/`35fcbf2`):**

- **`kiln_io_owner.c`/`.h` (built, Phase 1, commit `6aa4adf`)** — a single
  task + bounded queue (depth 8) now owns every relay/digital-IO/raw-SX1509
  write and read. This replaces three previously uncoordinated writers
  (`uart_bridge.c`'s `io_bridge_task`, `dashboard_http.c`'s
  `dashboard_set_relay()`, and — found mid-implementation — the
  `profile_executor.c` control loop and `autotune_engine.c` writing relay
  state directly) and centralizes a safety/ownership gate that used to be
  duplicated independently in two places. `idf.py build` clean. **Not
  hardware-verified**: no board was attached when this landed, and even now
  the dead PC-link UART blocks confirming SET_RELAY from all three surfaces
  concurrently or measuring the added queue-hop latency against a real
  firing.
- **`thermo_owner.c`/`.h` (built, Phase 2, commit `ea3dbc8`)** — same shape,
  a single task + bounded queue owning every `MAX31856_*` config/read/write
  call, migrated off `uart_bridge.c`'s `thermo_bridge_task` and
  `safety_link.c`'s context-frame builder. Explicitly **not** a bug fix the
  way Phase 1 was — `MAX31856.c`'s per-channel lock already serialized
  multi-transfer sequences, so this is architectural consistency (one
  owning task per hardware subsystem) and a choke point for later
  system-mode gating. **Not hardware-verified, and cannot be from this
  bench even later**: the thermocouple daughterboard is physically
  disconnected, so every channel is expected to fail its own bring-up
  regardless of this change — the owner's fail-closed behavior on a
  missing channel is reasoned about, not observed.
- **Phase 3 (`profile_executor` command queue) — deliberately skipped, not
  built.** Reviewed and rejected: all four of `run()`/`halt()`/`pause()`/
  `resume()` already wrap their bodies in a single mutex
  (`s_exec.lock`), which is a correct mutex-guarded API with no
  Phase-1-style lost-update bug to fix. Converting a safety-critical state
  machine to a drop-on-full-queue command path would trade a well-understood
  failure mode (caller waits) for a worse one (command silently dropped),
  for no safety gain. Recorded as a decision, not deferred work — revisit
  only if a later system-mode gate genuinely needs a choke point here.
- **`wifi_prov.c`'s owning task (built, Phase 4, commit `1a36155`)** — the
  riskiest phase, done last per the approved plan. Closes a real bug: `s_wifi`
  previously had zero locking across four independent writers (the Wi-Fi
  driver's own event-loop task, the esp_timer service task, `lvgl_port_task`
  via `ui_page_network.c`, and esp_http_server's worker via
  `wifi_provision_http.c`). Every public `wifi_prov_*()` entry point now
  posts to a bounded queue drained by one new owner task; every signature is
  unchanged, so no caller module needed editing. The Wi-Fi driver's and
  timers' own callbacks (`on_wifi_event`/`on_ip_event`/
  `ap_fallback_timer_cb`/`rescan_timer_cb`) are also rerouted through the
  same queue as fire-and-forget posts — the part that makes this materially
  riskier than "add a queue in front of an existing task," since it touches
  the Wi-Fi stack's own event delivery.
  **Verification reached, and its real limit, stated plainly**: `idf.py
  build` clean; flashed over JTAG/OpenOCD (bootloader + partition table +
  app all **verified OK**); the board **boots and stays running** —
  confirmed by a subsequent OpenOCD resume reporting the core running
  freely (not halted in a panic), and the one boot log line that did arrive
  (an unrelated, pre-existing IDF warning) never repeated, which a
  crash/reboot loop would have made it do. **That is the entire extent of
  verification.** No actual Wi-Fi behavior was observed — not the owner
  task announcing itself, not an AP coming up, not a station join — because
  the console log and `wifi_get_status` both depend on the same dead PC-link
  UART described above. This is boot-smoke-tested, not Wi-Fi-verified;
  treat it as **built**, not **hardware-verified**, until the UART link is
  fixed and a real status/log round-trip is observed.
- **`safety_link.c`'s `ANNOUNCE_VERSION` migrated onto the shared
  `kilnlink_announce` codec (built, commit `35fcbf2`)** — the send side
  (`safety_build_announce_version_payload()`) now calls
  `kilnlink_announce_encode()` instead of hand-writing the byte layout;
  same fields, same burst cadence. `firmware/KilnFW/components/kilnlink/CMakeLists.txt`
  now also compiles `kilnlink_announce.c`. `idf.py build` verified. The
  receive side (`safety_parse_fw_version()`) stays hand-rolled on purpose —
  it parses the Pico's distinct `FW_VERSION` (`0x0B`) frame, not an inbound
  `ANNOUNCE_VERSION`, so the codec's fixed layout doesn't apply there. Not
  independently hardware-verified beyond the existing build/host-test
  coverage this bullet inherits from — no Pico exists on this bench to
  receive a real frame (ROADMAP.md M0).

**Net effect on this file's own claims**: none of the four items above
were previously mentioned in this doc at all — the "Done and verified"
section above predates all of them (last entry 2026-08-16). None reaches
**hardware-verified** by this doc's convention; Phases 1–2 and the codec
migration are **built** only (compiles clean, not exercised on real
silicon), and Phase 4 is the sole exception that gets a genuine **partial**
hardware data point (boots and stays running) short of full verification.

## 2026-08-20 update — the three MAX31856 thermocouple ICs are fitted

**Supersedes the 2026-08-19 entry above and every other place in this repo
that says "the MAX31856 thermocouple ICs are physically not connected" for
the `KilnFW`/ESP32-S3 side.** Three MAX31856 ICs and their thermocouples are
now populated on this bench board (via the daughterboard on J6). Verified
live: channels 0/1/2 read ~31-32 °C, cold junctions tracking ~0.3 °C below,
no faults; `CR1` reads back `0x03` (averaging 1 sample, TC type K) where it
previously read `0x00` before the parts were fitted. `thermo_owner.c`'s
bench verification is unblocked by this. **Unchanged**: the safety
processor (RP2040) still has none fitted — see `ROADMAP.md` M3 and
`../SaftyFW/docs/THERMOCOUPLE.md`.

Also this session: the mDNS host was renamed `kiln.local` -> `kilnctl.local`
(see the correction just above, this file's own earlier `kiln.local`
reference already updated) and a CMSIS-DAP probe was confirmed wired to the
safety processor's SWD, giving an agent-usable debug/program path there too
— see `../SaftyFW/README.md`.
