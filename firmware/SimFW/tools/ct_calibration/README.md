# CT calibration runner

Closes `firmware/SimFW/docs/PLAN.md` section 3.3's milestone **M-D**: turns
"simulate 12 A" from a meaningless 0..1 PWM-duty fraction into a number that
means 12 A *as `SaftyFW`'s current-sense channel measures it*. Also performs
`firmware/SaftyFW/docs/CURRENT_SENSE.md` section 5's commissioning check
("one relay commanded -> exactly one channel responds", adapted here to
driving one CT channel at a time instead of a relay) as a precondition,
exactly as PLAN.md 3.3 says it should:

> This makes "simulate 12 A" mean 12 A as the DUT measures it, which is the
> definition that matters, and it doubles as the CURRENT_SENSE.md §5
> commissioning check (one relay commanded → exactly one channel responds).

## What it does

One command, three steps:

1. **Crosstalk check.** Drives each of the 3 CT channels alone (others held
   at zero) and confirms the DUT reports exactly one channel above a
   threshold, and it's the right one. This gates everything after it — per
   CURRENT_SENSE.md §5, "a correlation guard fed by a mis-mapped CT is worse
   than no guard," and the same logic applies to a calibration table: one
   built on an unproven channel mapping would silently swap two channels'
   gain/offset, which is worse than no calibration at all.
2. **Sweep + fit.** For each channel, sweeps the commanded amplitude across
   `--points` values (default 10, per PLAN.md 3.3: "~10 points"), reads back
   what the DUT reports at each point, and fits `measured = gain * commanded
   + offset` by ordinary least squares. Reports R² and the max residual so a
   bad fit is visible, not just a number. A fit is **rejected** (not stored)
   if the gain is non-positive/near-zero (flat or inverted readback) or R²
   falls below `--min-r2` (default 0.98).
3. **Persist.** Writes a versioned JSON calibration table — see
   "Persistence decision" below for why this is a file, not fixture flash.

## Running it

```powershell
# Bench day: real fixture on COM5, real DUT over the existing kilnctrl link
python calibrate_ct.py --port COM5 --dut-serial-port COM7 --out calibration_table.json

# Today, no hardware: exercise the whole procedure against a compiled
# virtual_simfw with a synthetic stand-in DUT
python calibrate_ct.py --virtual 127.0.0.1:<port> --mock-dut --out /tmp/cal.json
```

Run `python calibrate_ct.py --help` for the full flag list (sweep range,
point count, settle time, thresholds, crosstalk test amplitude, etc.).

Exit codes: `0` success (table written), `1` crosstalk check failed (nothing
written), `2` one or more channel fits rejected (nothing written), `3`
connection failure, `4` table-save failure.

## What a good result looks like

- Crosstalk step: every channel line reads `PASS: channel N responded as
  expected, no crosstalk`.
- Sweep step: `r2` close to 1.0 (>= `--min-r2`, default 0.98), `gain`
  positive and in a plausible range for the fixture's amplitude scale,
  `max_abs_residual` small relative to the readings.
- The tool prints `Calibration table written: <path>` and the JSON table,
  and exits 0.

## What a bad result means, and what to do about it

- **Crosstalk FAIL, "no channel responded"** — the fixture's amplitude
  command isn't reaching that channel's transformer/RC path, or the DUT
  isn't reading it. Check the wiring/coupling before doing anything else;
  do not lower the threshold to make this pass.
- **Crosstalk FAIL, "channels [...] all responded"** — genuine crosstalk
  between CT channels (shared conductor, leaky transformer windings, or an
  ADC mux/wiring fault on the DUT side). Fix the hardware; a calibration run
  on top of this would silently corrupt every current-based test downstream.
- **Crosstalk FAIL, "channel X responded when channel Y was driven"** — a
  swapped CT jack or a mis-mapped ADC channel. CURRENT_SENSE.md §5 calls
  this out explicitly: "this is the only way to discover a CT plugged into
  the wrong jack." Fix the physical connection, don't just relabel channels
  in software.
- **Fit REJECTED, low R²** — noisy or nonlinear readback. Check for a loose
  connection, an unstable bench supply, or a channel near its clipping
  point (CURRENT_SENSE.md §2: "a channel reading within ~50 mV of the rail
  ... is at least this much current, and the measurement is no longer
  valid" — lower `--amps-max` if the sweep is running into clipping).
- **Fit REJECTED, gain <= 0** — the readback didn't move with the command,
  or moved the wrong way. Same causes as a crosstalk "no channel responded"
  failure, or an inverted wiring polarity.
- **A run refuses to write a table at all if only some channels pass.**
  This is deliberate — see `calibrate_ct.py`'s "ONE OR MORE CHANNEL FITS
  REJECTED" message. A partial table would silently leave the failed
  channel(s) uncalibrated with no obvious signal that anything is wrong.

## Readback path — what was found, and what wasn't

PLAN.md 3.3 says to read back "what `SaftyFW`'s own `current_task`/ADC
reports (over the existing kilnctrl MCP path or SWD)". Both were checked.

- **The kilnctrl MCP/UART path exists and is the one this tool uses.**
  `tools/PcTools/src/kilnctrl/safety.py`'s `SafetyClient.get_status()`
  decodes `SAFETY_CMD_GET_STATUS` (`firmware/uart_task_ids.h`'s wire layout)
  into a `SafetyStatus` whose `current_a` field is exactly `SaftyFW`'s
  current-sense reading for all three channels, already zero-subtracted and
  clamped at 0 per `CURRENT_SENSE.md` §2/§5. This is the ESP's cache of the
  Pico's last poll, reachable from the PC over the existing kilnctrl USB
  link with no SWD probe needed — the least invasive of the two options
  PLAN.md names, so it's the one `readback.py`'s `KilnctrlSafetyReadback`
  wraps. `tools/PcTools/scripts/current_sense_commissioning.py` (an earlier,
  separate bench script covering CURRENT_SENSE.md §5's *relay*-driven
  commissioning check) already uses this exact same client/link for the
  same reason, which is corroborating evidence this is the intended path,
  not a guess.
- **What's missing: nobody has run it against real current-sense hardware
  yet.** `current_sense_commissioning.py`'s own module docstring says
  plainly that no RP2040 firmware in this repo answers
  `SAFETY_CMD_GET_STATUS` with real current data yet — no MAX31856/CT
  wiring exists on the bench at the time of writing. `KilnctrlSafetyReadback`
  in this package is in the same state: read, not run, against real
  hardware. This is not a gap in this tool's plumbing, it's the honest
  state of the bench.
- **No MCP tool exists to push calibration constants (`k_ct_v_per_a`, etc.)
  to the RP2040's own flash** — `current_sense_commissioning.py` already
  found this while covering CURRENT_SENSE.md §5 step 3. Irrelevant to this
  tool specifically (this calibration lives on the fixture side, not
  `SaftyFW`'s own `k_ct_v_per_a`), but worth knowing it's a real gap in the
  broader "commission the current-sense system" story if a future task
  needs to write `SaftyFW` calibration constants from a script.
- SWD (`debug_probe.py`, `mcp__kilnctrl__debug_read_memory`) was not used.
  It could, in principle, read `current_sense.c`'s live state directly out
  of RAM, but that means halting the target (or reading racy live memory)
  and coding against internal struct layout instead of the stable wire
  protocol GET_STATUS already exposes. Strictly more invasive for no
  accuracy benefit here, so it was not pursued.

## Persistence decision: a PC-side file, not fixture flash

PLAN.md 3.3 says "store the table in fixture flash keyed by channel." This
tool does **not** do that, deliberately. `firmware/SimFW/src/` has no
config/flash-persistence subsystem at all today — grepped for at the time
this was written, zero hits, unlike `firmware/SaftyFW/src/config_store.c` +
`config_store_flash.c`, the established pattern this codebase already uses
elsewhere. Building an equivalent for SimFW is a real firmware subsystem
(flash sector layout, a write-count/wear budget, a load-at-boot path, and a
new wire command to push a table down) — out of scope for a PC-tooling task,
and explicitly excluded by this task's constraint against touching
`firmware/SimFW/src/**`.

Instead, `calibration_table.py` writes a versioned JSON file
(`ct_calibration_table.json` by default, next to this script) that:

- is useful **today**, with zero firmware risk — a human or another script
  can read `gain`/`offset`/`r2` straight off disk;
- is a strict subset of what a future flash format would need to hold, so
  none of this is wasted once that firmware work exists;
- refuses to be written at all if the crosstalk check failed or any
  channel's fit was rejected (`CalibrationTable.save()`) — there is no
  "partial, trust it anyway" mode.

### Remaining firmware work

`firmware/SimFW/src/tasks/wave_owner.c`'s `ct_wave_amps_to_pwm_scale()` is
today an **identity placeholder** (`TODO(M-D calibration)`, per its own
header comment): `pwm_scale = clamp(amps, 0, 1)`. To consume this tool's
table, that function needs to become, per channel:

```c
pwm_scale = clamp(gain[channel] * amps + offset[channel], 0.0f, 1.0f);
```

using `ChannelCalibration.to_command()`'s exact inverse-fit arithmetic
(`(target_amps - offset) / gain`, algebraically identical to the form
above once rearranged). Where `gain[channel]`/`offset[channel]` come from
is exactly the flash-persistence question above — the smallest change that
needs no new firmware subsystem is a compiled-in table generated from this
tool's JSON output (checked in, regenerated whenever a new bench
calibration run is done), which gets the *behavior* PLAN.md 3.3 wants
without inventing flash storage; a real per-unit-flashable table is the
Phase-9-style follow-up once SimFW gains its own `config_store`.

This file was **not** modified by this task (constraint: no changes under
`firmware/SimFW/src/**`) — the above is the precise, minimal change a
follow-up firmware pass needs to make.

## Be honest about limits

**Nothing in this tool has been run against real current-sense analog
hardware**, and nothing here can prove it will work correctly against real
hardware. What has been verified, against a compiled, unmodified
`virtual_simfw.exe` (`firmware/SimFW/tools/virtual_simfw/`) speaking the
real `benchproto` wire protocol over TCP, with a synthetic stand-in DUT
(`readback.py`'s `SyntheticDutReadback` — see its module docstring for
exactly what it does and does not model):

- the full command path — real `CT SET_MODE`/`SET_AMPS` frames reach a real
  compiled SimFW core and are acknowledged;
- the sweep procedure, least-squares fit, and R²/residual reporting are
  arithmetically correct (`tools/PcTools/tests/test_ct_calibration.py`'s
  `FitLinearTest`/`EvaluateFitTest`/`LinearFitInvertTest`, synthetic
  numbers, no I/O);
- the crosstalk decision logic correctly identifies "no response," "wrong
  channel," and "multiple channels" failure modes
  (`CheckCrosstalkTest`/`AllChannelsCleanTest`);
- the calibration table's save/load round-trips, and **refuses to save**
  an unvalidated table (`CalibrationTableTest`);
- end to end, against the live virtual fixture: a clean synthetic DUT
  produces a valid table and exit code 0; a synthetic DUT with induced
  channel-2-into-channel-1 leakage is caught by the crosstalk gate and
  produces exit code 1 with **no file written**; a synthetic DUT with one
  flat channel (crosstalk skipped via `--skip-crosstalk` to isolate the
  fit gate) is caught by the R² gate and produces exit code 2 with **no
  file written** (`TestCalibrateCtAgainstVirtualSimfw`).

What none of this proves: that a real CT, transformer, AD8542 rectifier
stage, and ADC actually behave linearly across the swept range, that the
threshold/R²/gain defaults chosen here are the right ones for real noise
levels, or that the kilnctrl `SAFETY_CMD_GET_STATUS` readback path performs
correctly against a real RP2040 running real `SaftyFW` current-sense code
(nothing in this repository has exercised that path with real current
flowing, per `current_sense_commissioning.py`'s own docstring). Only a real
bench run, with real hardware, can establish that — this tool makes that
run one command instead of an afternoon of spreadsheet work, nothing more.

## Files

| File | Purpose |
|---|---|
| `fit.py` | Pure least-squares fit + accept/reject gate. No I/O. |
| `crosstalk.py` | Pure one-channel-responds decision logic. No I/O. |
| `calibration_table.py` | Versioned JSON persistence, with the save-time gates. |
| `readback.py` | DUT readback: real (`KilnctrlSafetyReadback`) and synthetic (`SyntheticDutReadback`) implementations of one shared interface. |
| `fixture.py` | Thin wrapper over `kilnsim`'s real `CT` command group. |
| `calibrate_ct.py` | CLI orchestrator — the one command. |

Tests: `tools/PcTools/tests/test_ct_calibration.py`.
