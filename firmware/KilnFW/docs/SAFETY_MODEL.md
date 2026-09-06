# Safety Model

This document states, in one place, what actually stops a KilnCtrl board from
leaving a heating element energized when it shouldn't be — and, just as
important, what still does not stop that today. If a claim here and a claim
in another doc disagree, this file is not automatically right; check the code
(`App/drivers/bridge/uart_bridge.c`, `App/drivers/owners/kiln_io.c`, `App/drivers/safety/safety_link.c`,
`App/main.c`) and fix whichever one is wrong.

## The rule

**The PC (GUI or MCP) can ask a relay to turn on. The firmware can refuse.**
When a request to energize a relay and a real safety condition disagree, the
safety condition wins — the request is refused, not queued, not partially
applied, not overridden by a "force" flag, because there is no force flag.
Nothing in `pc_tools` can turn a relay on against a firmware-side refusal;
the only way to change the outcome is to clear the underlying fault.

Turning a relay **off** is never refused, by anything, for any reason. The
safe direction must always be reachable — including as the way to recover
from the very fault that is blocking "on".

## What enforces it today

### 1. The relay-on gate (`App/drivers/bridge/uart_bridge.c`, `io_relay_on_blocked`)

`IO_CMD_SET_RELAY` and `IO_CMD_SET_RELAY_MASK` are refused outright — logged,
no state change, no reply — if `safety_link_get_fault_sources()` is nonzero,
or if no `SafetyLinkClass` was successfully started at boot at all (absence
of the safety subsystem is treated as "cannot prove safe", not "assume
safe"). See the wire-level note in `docs/UART_PROTOCOL.md` under `IO` task 2.

**Update (2026-08-13): no longer the only caller, and the gate itself moved.**
The check this section describes was extracted into
`App/drivers/relay_authority.{c,h}` (`relay_authority_on_blocked()`) so every
caller shares one implementation instead of each reimplementing the same
fault-source read; `uart_bridge.c`'s `io_relay_on_blocked()` is now a thin
wrapper over it. Two more real callers exist beside the UART bridge:
`diagnostics_http.c`'s `POST /api/diagnostics/danger/relay` (the web UI's
manual relay control, TODO.md section 2 — `dashboard_http.c`'s old
`POST /api/relay` was removed 2026-08-27, see `docs/WEB_UI.md`) and
`App/drivers/control/profile_executor.c` (the profile
execution engine, TODO.md section 6/6A) — both gate every relay-ON command
through the same function, so the rule stated above ("the safety condition
wins") holds identically for all three. `profile_executor.c` additionally
introduced a second, narrower gate, `relay_authority_zone_blocked()`
(TODO.md 6A.6): some guard trips (thermal_guard's per-zone-physics guards)
block only the tripping zone's relays rather than the whole board, layered
*on top of* the global gate above, never replacing it. Everything below
still describes the global gate correctly; treat "the only place" in the
paragraph above as historical.

**Danger mode is a deliberate, explicit bypass of this gate.**
`App/drivers/owners/kiln_io_owner.c`'s `relay_on_blocked()` — the single choke point
every manual relay-ON command reaches — checks `danger_mode_active()`
(`App/drivers/safety/danger_mode.c`) first, and if the operator has entered that
mode (an explicit accept-the-risk action from the diagnostics page, time-
boxed and auto-released on idle timeout — see `danger_mode.h`'s top comment)
it skips both `relay_authority_on_blocked()` and the OTA heat-interlock
check entirely, logging a warning whenever one of them would otherwise have
blocked. This is intentional, not a gap: the mode exists so an operator can
bench-test a relay/contactor with nothing fighting the test, and it bypasses
only this ESP's own relay-on gate — it never reaches, and cannot touch,
SaftyFW's independent contactor authority on the RP2040, which stays outside
this board's reach either way. Confirmed current as of 2026-09-04 (`kiln_io_owner.c:179-209`'s
`relay_on_blocked()`, `danger_mode.c:174-209`'s `danger_mode_active()`).

### 2. The fault-source mask (`App/drivers/safety/safety_link.c`)

`safety_link_set_fault_source(mask, assert)` OR's named reasons into
`GPIO6`, the isolated line into the RP2040 safety processor:

| Source | Set by | Meaning |
|---|---|---|
| `SAFETY_FAULT_SRC_MANUAL` | `SAFETY_CMD_SET_FAULT_OUT` from the PC | Operator or GUI explicitly asserted it |
| `SAFETY_FAULT_SRC_PC_LINK` | the link watchdog (`uart_bridge.c`), gated by `KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT` (default off) | No frame from, or ACK from, the PC within `UART_BRIDGE_LINK_TIMEOUT_MS` (5000 ms) |
| `SAFETY_FAULT_SRC_THERMO` | `App/main.c` at boot only — see gap below | SPI bus or all three thermocouple channels failed to come up |
| `SAFETY_FAULT_SRC_SAFETY_LINK` | `safety_link.c`, `fault_on_link_loss` policy (default on) | The RP2040 hasn't answered a poll recently |
| `SAFETY_FAULT_SRC_APP` | `App/main.c` at boot; `io_relay_on_blocked` when `safety` is NULL | Catch-all for "something upstream of this gate isn't there" |

No single source can clear another's bit — the mask is closed
(`SAFETY_FAULT_SRC_ALL`) and `safety_link_set_fault_source` refuses any bit
outside it. The relay gate reads this same mask, so **any** asserted source
blocks "on", not just the ones that are obviously about relays.

### 3. Link-loss watchdog (`App/drivers/bridge/uart_bridge.c`)

A dedicated task at priority 6 (above every bridge task, so it keeps running
even if a bridge is stuck waiting on a dead host's ACK) checks every 250 ms
whether a frame or ACK has arrived from the PC within the last 5000 ms.
Before the PC has ever spoken — including at boot with nothing attached — the
link counts as lost. On loss, relays are dropped and **retried every tick
until the write actually succeeds** (one failed I2C transfer must not be the
reason an element stays on); this half is unconditional
(`KILNCTL_SX1509_RELAYS_OFF_ON_LINK_LOSS`, default on).

**Update (2026-08-2x): asserting `SAFETY_FAULT_SRC_PC_LINK` on the same loss
is now a Kconfig option, `KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT`, default OFF.**
The original reasoning for asserting the fault line unconditionally still
holds as the *rationale for the option's existence*: the PC link is the one
channel `pc_tools`/MCP use to command relays, and a controller that has gone
silent while something is calling for heat is exactly the case this doc's
"fail-safe, not fail-open" stance is built around — turning the option on is
the right call for a deployment where the PC is expected to stay attached.
But this board also has its own LCD and web UI and is designed to fire with
no host attached at all, so the PC's absence by itself is not a hazard on a
standalone board — and the old unconditional behavior asserted the fault
line five seconds after every boot with nothing plugged in, tripping the
safety processor's S6a (`SAFETY_TRIP_MAIN_FAULT`) on a perfectly healthy
kiln. That trip latches on the RP2040 and does not clear when the link comes
back, so the board was left showing a permanent fault for no real hazard —
which is why the default flipped to off. Turning `KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT`
on is still the right choice for any deployment that wants the PC treated as
required equipment; it just is not this board's default. Either way, the
relay drop above still happens.

### 4. Boot-time fault assertion (`App/main.c`)

If the SX1509 expander doesn't come up, relay state is unknown and
uncommandable, and `SAFETY_FAULT_SRC_APP` is asserted. If the shared SPI bus
or every MAX31856 channel fails, `SAFETY_FAULT_SRC_THERMO` is asserted. Both
early-return paths in `app_main` call `kiln_enter_safe_state()` first —
relays off, fault line up — before giving up, because `app_main` returning
does not stop FreeRTOS; a task that already started stays running with
whatever state was last set unless something explicitly changes it.

### 5. Payload validation (`App/drivers/bridge/uart_bridge.c`)

Independent of the above: every relay/IO/thermo/display/safety subcommand is
length- and range-checked before it reaches a driver, and rejected — never
guessed at — on a short or malformed frame. This stops a corrupted or
malicious frame from being *misinterpreted* as a valid command; it is not
itself a safety-condition check. See `docs/UART_PROTOCOL.md`.

## What does NOT enforce it yet — real gaps, not just untested code

**Update (2026-08-11): partially closed, for a running profile executor
only.** `App/drivers/control/profile_executor.c` and `App/drivers/control/thermal_guard.c`
now exist (TODO.md section 6A) and, while a profile is actively running a
zone, guard 6 (sensor validity — `spi_failed`, `NaN`, `THERMO_FAULT_OPEN`/
`OVUV`/`TCRANGE`) trips after 3 consecutive bad reads and asserts a **live**
`SAFETY_FAULT_SRC_THERMO` (broadened from its previous boot-only meaning),
which blocks relay-on everywhere through the unchanged
`relay_authority_on_blocked()`. This was live-verified end to end on the
real board (no thermocouple attached, so every read is invalid): the trip
fired after exactly 3 bad reads, relay stayed off throughout, and
`profile_executor_halt()` was the one way to clear it back to idle.
**This does not close the general case below** — it only covers the window
while `profile_executor` is actively driving a zone. The other paths that
read a thermocouple (`THERMO_CMD_READ` over the UART bridge, the dashboard's
`/api/status` outside of a running profile) still do not gate on a live
fault; a channel that opens with no profile running is still silently
unenforced. The design questions this doc originally posed are now answered
*for the profile-executor case*: any-channel-is-actually-per-zone (guard 6
blocks that zone's relay via the new `relay_authority_zone_blocked()`, layered
on top of the unchanged global gate — see the summary table below),
debounced at 3 consecutive bad reads (not first-bad-read), and `OPEN`/`OVUV`/
`TCRANGE` are the tripping bits (a single `TCHIGH`/`TCLOW` still does not
trip — left as the control loop's job, matching the original open question's
own reasoning). See `App/drivers/control/thermal_guard.c` and TODO.md section 6A.3
for the full detail, including guards 1–5 and 7 (also implemented, not yet
live-tested) and guards 8–9 (not yet implemented).

**A live thermocouple fault during operation still does not block relay-on
outside of a running profile.** `SAFETY_FAULT_SRC_THERMO` was, before
2026-08-11, only ever asserted once at boot if the bus or every channel
failed to *come up*. That boot-time behavior is unchanged. If a channel
later reports `THERMO_FAULT_OPEN` or `THERMO_FAULT_TCRANGE` mid-session while
no profile is running — for instance, a direct `THERMO_CMD_READ` over the PC/
MCP UART link, or the dashboard's live status view with no profile active —
the relay gate still does not see it and does not react; only
`profile_executor`'s own guard 6, while a profile is actively running, does.
`THERMO_CMD_READ` already reports the fault byte and, since the
driver-hardening pass, `NaN` for an invalid reading (see `docs/MAX31856.md`);
nothing downstream of that read outside `profile_executor` currently *acts*
on it. Closing this for the general case remains open, and needs a design
decision this document still does not make on its own for that broader case.

**Nothing on the main board reacts to a safety-processor-reported E-stop or
fault.** `safety_link_get_status()` surfaces `SAFETY_FLAG_ESTOP` and
`SAFETY_FLAG_FAULT` from `SaftyFW`'s replies — the link now carries real
telemetry (see `docs/SAFETY_LINK.md`, "Transport") — and the isolated fault
line is currently one-directional — an ESP *output* into the Pico (see
`docs/SAFETY_LINK.md`, "Trap 3"). Whether the Pico's own interlocks (its relay
K4, its own thermocouple) are sufficient on their own, or whether an
E-stop/fault reported back over the isolated UART should also drop the *main*
board's relays, is a system-design call above what this firmware currently
implements. Today: no.

**The RP2040 safety-processor firmware now exists and the link works, but
`SAFETY_FAULT_SRC_SAFETY_LINK` still means link staleness, not a real safety
opinion from the Pico.** With no peer, or before this firmware was written,
`fault_on_link_loss` (default on) kept the fault line correctly asserted,
which was arguably the single best thing about the earlier state of the
safety story: the failure mode of "nobody wrote the other half yet" was
fail-safe, not fail-open. That reasoning still applies to a genuinely dead
link; it no longer excuses the claims above about acting on a *live* Pico's
own E-stop/fault opinion, which remain unimplemented on the main board today.

**Refusal is invisible on the wire.** As documented in
`docs/UART_PROTOCOL.md`, a refused `SET_RELAY` produces no reply — the PC
finds out only by reading state back. `pc_tools`' relay indicators are
already driven from the firmware's live auto-report push rather than an
assumed command echo (see `docs/UART_PROTOCOL.md`, `IO` task `SET_AUTO_REPORT`),
so a refused command shows up as "stayed off" within one report period —
but there is no explicit "refused, and here is why" signal a GUI could turn
into a clear message instead of an unexplained non-response. A future wire
version could add one; today it does not exist.

**`SX_WRITE_REG` / `SX_SET_DIR` bypassing the relay gate — closed
(2026-08-11).** These raw-register debug subcommands write straight to the
SX1509 through `kiln_io->exp`, not through
`kiln_io_set_relay`/`kiln_io_set_relay_mask`, so they never passed through
the same gate. Fixed with a per-pin guard in `uart_bridge.c`
(`sx_write_reg_touches_relay_on()`/`sx_set_dir_touches_relay()`), reading the
relay-pin bitmap from a new `kiln_io_relay_pin_mask()` getter rather than
duplicating the board's pin map — the other 12 expander pins stay fully
reachable for debug. `SX_WRITE_REG` is refused exactly when it would set a
relay pin's `RegData` bit high while `relay_authority_on_blocked()` says no
(fault-gated, same rule as `SET_RELAY`/`SET_RELAY_MASK`). `SX_SET_DIR` is
refused unconditionally whenever it would flip a relay pin to an input
(independent of fault state — an input relay pin can't be commanded off
either, so a fault-gated check alone would miss the "already-on relay gets
stuck on" failure mode). See `TODO.md` section 6A.6 for the implementation
note.

**RESOLVED, docs previously lagged the fix.** A prior revision of this
section claimed `SAFETY_FAULT_SRC_APP` latches forever once guard 9 sets it
for a transient control-task stall, clearing only on reboot, and separately
claimed this was "found on the bench 2026-08-25" — that bench-discovery claim
was already retracted as unsupported (no commit or `PROJECT_STATUS.md`/
`ROADMAP.md` entry near that date records a genuine bench-provoked
control-task stall; 2026-08-25's actual bench work, `8c2bba6`, was the
safety-link 230400 baud sweep). Re-checking the underlying "nothing ever
clears it" claim against the current tree (2026-09-04) shows it is also
stale: the fault-clear path was added in `0d85dbb` (2026-08-27, "Report the
channel that failed, not the one after it") — **two days after** the paragraph
above was originally written — and this file was never revisited once the
fix landed.

As implemented today: guard 9's stale-tick branch
(`profile_executor.c:1386` for the `tick_stale` check,
`WATCHDOG_TICK_DEAD_MS` = 10 s at `profile_executor.c:61`) calls
`guard9_assert_stale_tick_fault()` (`profile_executor_relay_io.c:600`), whose
own doc comment cites this exact defect ("Audit 2026-08-27 item 2 ... a
single stale control-task tick left `SAFETY_FAULT_SRC_APP` latched
board-wide until reboot"). It now ORs `SAFETY_FAULT_SRC_APP` into
`s_exec.global_fault_source` (not a bare, unpaired
`safety_link_set_fault_source(..., true)` call) and transitions the run to
`PROFILE_EXEC_FAULTED`. `profile_executor_halt()` (`profile_executor_status.c:76`)
calls `clear_this_runs_faults()`, which deasserts the *entire*
`global_fault_source` mask — `SAFETY_FAULT_SRC_APP` included — via one
`safety_link_set_fault_source(..., false)` call. Host-tested directly:
`App/test/test_profile_executor_prestart.c`, section "`guard9_assert_stale_tick_fault()`
then `profile_executor_halt()` -- the operator's halt deasserts
`SAFETY_FAULT_SRC_APP`, closing the loop this defect left open" (also proves
the OR-not-overwrite behavior against a pre-existing global fault).

So the remaining behavior is: a stalled control task drops every relay and
raises `SAFETY_FAULT_SRC_APP` immediately (unchanged, and correct — this is
guard 9 doing its job), and clearing it requires an operator
acknowledgment (Stop / halt) rather than clearing itself the instant the
tick resumes. That is the same acknowledgment-required pattern every other
`thermal_guard` trip uses (`clear_this_runs_faults()` is the shared clear
path), not a defect specific to guard 9 — GPIO6 does not stay driven forever
short of a reboot; it stays driven until the operator halts the run, then
drops. No hardware trial of this path is on record (see the summary table
below), so the trip-and-recover sequence itself is host-tested + argued
against real functions, not hardware-verified — but the "nothing ever clears
it" defect this section used to describe does not exist in the current
code.

## Summary table

Verification-state vocabulary, shared with `docs/SAFETY_CASE.md` §4 and
`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` §4: **argued** (code inspection
only), **host-tested** (a specific host test exercises the real function, not
a stub), **hardware-verified** (exercised on real silicon; commit/date
given). **inert/dormant** marks a guard that is real but cannot fire in the
shipped default configuration (a threshold defaulting to "never trip", an
input nothing produces). A row may carry more than one state when different
aspects differ — e.g. detection host-tested but the relay-drop side never
exercised on hardware.

| Failure | Detected? | Relay-on blocked? | Existing relays dropped? | Verification state |
|---|---|---|---|---|
| PC link lost (cable, crash, USB) | Yes, 5 s | Only if `KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT` (default off) | Yes, automatically | **inert/dormant** for the fault-assert side (default off — see "3. Link-loss watchdog" above); **host-tested** for the underlying staleness/fault logic (`test_safety_watchdog.c::test_pc_link_sustained_loss_faults_running`, `test_pc_link_brief_loss_does_not_fault`, `test_pc_link_already_faulted_retries_relay_off`); the unconditional relay-drop itself is **argued only** — no bench record of a real cable pull found in `PROJECT_STATUS.md` or `GUARD_TEST_MATRIX.md` |
| Expander fails at boot | Yes | Yes (`APP`) | N/A — nothing to drop yet | **argued only** — `App/main.c`'s early-return path is code-reviewed; no host test or bench record of a real failed-SX1509 boot found |
| SPI bus / all thermo channels fail at boot | Yes | Yes (`THERMO`) | No — only blocks new "on" | **argued only** — same as above, no test/bench record found for this specific boot-time path |
| One thermo channel faults during operation, **a profile is actively running that zone** | **Yes** — `thermal_guard.c` guard 6, 3-read debounce | **Yes** (`THERMO`, live-broadened) | Yes — `profile_executor.c` retries the drop every tick | **hardware-verified**, 2026-08-11 — real board, no TC attached, trip fired after exactly 3 bad reads, relay stayed off throughout, `profile_executor_halt()` the only way to clear (this doc, above, and `docs/PROJECT_STATUS.md`); also **host-tested** against the real `thermal_guard_tick()` function, not a stub (`App/test/test_thermal_guard.c` "Guard 6" cases, e.g. `"3rd consecutive bad read trips guard 6"`) |
| One thermo channel faults during operation, **no profile running that zone** (raw UART/MCP `THERMO_CMD_READ`, dashboard status view) | Reported on `READ`, not acted on | **No** | **No** | **not a guard — open gap.** No mechanism exists to cite; this is the general case `SAFETY_MODEL.md` itself still leaves open (see "What does NOT enforce it yet" above) |
| Zone heating but not progressing (guard 1) — disconnected/fallen-out TC | **Yes**, while a profile runs that zone | Yes, per-zone (`relay_authority_zone_blocked()`) | Yes, that zone only | **host-tested** against the real `thermal_guard_tick()` (`App/test/test_thermal_guard.c`, e.g. `"guard 1 trips when commanded heat produces far less than sanity_rate_c_per_min"`, plus the `progress_rise_check_relaxed`/`progress_duty_min`/`progress_window_s`/`progress_band_c` override cases); **not hardware-verified** — no bench provocation of guard 1 on record (`docs/SAFETY_CASE.md` §4 lists it "argued + code-reviewed only, not yet live-tested", which understates the host-test coverage that exists — see disagreement note below) |
| Zone temperature moving the wrong direction (guard 2) — miswired/swapped TC | **Yes**, while a profile runs that zone | Yes, per-zone | Yes, that zone only | **host-tested** (`App/test/test_thermal_guard.c`, `"guard 2 trips when heating commanded but temperature falls fast at/above setpoint"`, and the scope check proving guard 1's relaxation does not leak into guard 2); **not hardware-verified** |
| Relay welded on / shorted SSR, heat reads off but rising (guard 3) | **Yes**, while a profile runs that zone | Yes (`THERMAL_SANITY`, global) | Yes, every zone | **host-tested**, including a named hardware-motivated regression case (`App/test/test_thermal_guard.c`, `"guard 3 tolerates a slow 0.2C/min drift at duty 0"`, comment: "found on hardware 2026-08-12 against the simulated…"); the trip case itself and the `runaway_margin_c` override are also host-tested; **not hardware-verified** against a genuine welded contact — the only hardware contact this guard has had is the false-positive it was tuned against, not a positive trip |
| Zone drifted from setpoint and stayed drifted (guard 4) | **Yes**, while a profile runs that zone | Yes, per-zone | Yes, that zone only | **host-tested**, including the 2026-09-03 hot-start arming fix and the `no_setpoint` two-producer contract case (`App/test/test_thermal_guard.c`, `"guard 4 trips on a sustained excursion after having settled"`, `"guard 4 eventually trips a zone that starts hot and never settles, once armed by run duration"`); **not hardware-verified** |
| Zone reading exceeds configured `max_temp_c`/`min_temp_c` (guard 5) | **Yes**, while a profile runs that zone | Yes (`THERMAL_SANITY`, global) | Yes, every zone | **host-tested** (`App/test/test_thermal_guard.c`, `"guard 5 max_temp trips on the very first over-limit tick"`, `"guard 5 min_temp trips"`); **also inert/dormant per-zone whenever that zone's `max_temp_c`/`min_temp_c` is left at its 0 default** — same test file proves it explicitly (`"max_temp_c==0 disables guard 5's ceiling"`) — an uncommissioned zone has no ceiling in force, the KilnFW-side twin of `SaftyFW`'s S1; **not hardware-verified** |
| Frozen/stuck sensor reading while duty > 0 (guard 7) | **Yes**, while a profile runs that zone | Yes, per-zone | Yes, that zone only | **host-tested**, including the `frozen_window_s` override (`App/test/test_thermal_guard.c`, `"guard 7 trips when the reading never changes while duty > 0"`, `"frozen_window_s override=20 trips guard 7 well before the 600s default would"`); **not hardware-verified** |
| Cross-zone plausibility (guard 8) | No — needs concurrent multi-zone execution (TODO.md 6A.5), unbuilt | **No** | **No** | **not built** — no code exists to argue, host-test, or hardware-verify |
| Profile-executor control task itself stalls (guard 9) | Yes — independent `profile_exec_wdt` task, 10 s staleness | Yes (`APP`) | Yes, every zone | **host-tested** — the tick-stale fault path and its priority over other reasons are real-function tests, not stubs (`App/test/test_safety_watchdog.c::test_tick_stale_still_faults_running_and_takes_priority`); the fault-clear path (`guard9_assert_stale_tick_fault()` → operator `profile_executor_halt()` → `clear_this_runs_faults()`) is likewise a real-function host test, not a stub (`App/test/test_profile_executor_prestart.c`, "the operator's halt deasserts `SAFETY_FAULT_SRC_APP`, closing the loop this defect left open") — the section above previously claimed this never clears; that was true only before `0d85dbb` (2026-08-27) landed the fix, and this row is now corrected to match. Treat guard 9's trip-and-clear mechanism as **host-tested + argued**, not hardware-verified, until a real stall is provoked on the bench |
| Safety processor link down | Yes (once Pico firmware exists) | Yes (`SAFETY_LINK`) | No — only blocks new "on" | **host-tested** for the staleness/liveness logic itself (`firmware/CommonFW/docs/LINK_PROTOCOL.md` §8, `test/test_safety_link.c:77-92`, pinned 2026-09-04); the link genuinely exists and has carried real traffic (`docs/SAFETY_LINK.md`, 2026-08-23) but **nobody has held the link down on the bench and watched the fault assert with a stopwatch** — `docs/SAFETY_CASE.md` §4/§2 and `ROADMAP.md` both say so explicitly; do not read "Yes (once Pico firmware exists)" as hardware-verified |
| Safety processor reports E-stop/fault | Surfaced in `GET_STATUS`, not acted on | **No** | **No** | **not a guard — open gap**, one-directional link (see "Nothing on the main board reacts…" above); nothing to cite beyond that design gap |
| Manual assert via `SET_FAULT_OUT` | Yes | Yes (`MANUAL`) | No — only blocks new "on" | **argued only** — mask-OR logic is simple and code-reviewed; no host test or bench record specifically provoking `SET_FAULT_OUT` was found |
| Raw `SX_WRITE_REG`/`SX_SET_DIR` at a relay pin | N/A | Yes (per-pin guard, 2026-08-11) | N/A | **argued only** — `sx_write_reg_touches_relay_on()`/`sx_set_dir_touches_relay()` are code-reviewed (see "closed (2026-08-11)" above); no host test or bench record found exercising the refusal path itself |
| Malformed/truncated frame | Yes | Rejected outright | N/A | **argued + host-tested** — length/range checks are simple and code-reviewed (`docs/UART_PROTOCOL.md`); the framing layer itself (`kilnlink_frame_decode`, `kilnlink_unstuff`) is fuzz-covered by the pre-existing `firmware/CommonFW/test/test_fuzz.c`, and the payload layer one level up (the ~27 `kilnlink_*_decode` codecs) is fuzz-covered by `ccb23ac` (`firmware/CommonFW/test/test_fuzz_payloads.c`) — neither exercises this file's own per-subcommand range checks by name |

**Disagreement found while adding this column (2026-09-04), reported rather
than silently harmonised:** `docs/SAFETY_CASE.md` §4 lists KilnFW's
`thermal_guard` guards 1, 2, 4, 5, 7 as "**argued + code-reviewed only** …
implemented and code-reviewed but not yet live-tested (no thermocouple/relay
hardware attached to provoke them)", quoting this very document's own
2026-08-11 update note below. That quote is about *hardware* verification and
is still accurate on that narrower point — but `App/test/test_thermal_guard.c`
demonstrably host-tests all five of those guards (and guards 3, 5, 6) against
the real `thermal_guard_tick()` function, including override/arming/regression
cases, not a stub. "Not yet live-tested" was read by `SAFETY_CASE.md` as "not
tested at all" for these rows; the table above corrects that to **host-tested,
not hardware-verified**, which is the precise and stronger-than-"argued"
state these guards are actually in. `SAFETY_CASE.md` should be corrected to
match (out of scope for this pass — its own edit lane belongs to a different
task today).

Rows marked **No** in bold are the open work. **Updated 2026-08-11**: the
"one thermo channel faults during operation" row is now split in two — it
flips to detected/blocked/dropped only for the case where `profile_executor`
is actively running that zone (TODO.md section 6A; live-verified end to end
on real hardware, see `docs/PROJECT_STATUS.md`); the general case (no
profile running) is unchanged and still fully open. The new guard 1–5/7/9
rows are new capability from the same pass, not flips of prior rows — guards
1, 2, 4, 5, 7 are implemented and code-reviewed but **not yet live-tested**
(no thermocouple/relay hardware attached to provoke them); guard 6 and guard
9's mechanism are the only ones exercised on real hardware so far, and guard
9 only via code inspection of its trigger path, not by actually stalling the
control task. Guard 8 remains fully unbuilt. Contrary to TODO.md 6A.6's own
expectation of "flip the two rows... into Yes", only the thermo-channel row
actually flips this pass — the "safety processor reports E-stop/fault" row
is untouched, since it needs the RP2040 firmware (which still doesn't exist)
and an isolated-link path this pass didn't add. `SX_WRITE_REG`/`SX_SET_DIR`
also remain unaddressed (TODO.md 6A.6's own "should land before the first
unattended firing" item, not done here). Everything else is implemented and
covered by the ESP-IDF build (`idf.py build`, clean under
`-Wall -Wextra -Werror` as of this writing) but **untested on real hardware**
unless stated otherwise above — see `docs/PROJECT_STATUS.md` for what "done"
means across the whole project.

**Added 2026-08-13, run-level policy (not a relay-authority change)**:
`zones_config_get_continue_on_zone_trip()` (`zones_http.c`, default `false`)
governs whether a per-zone thermal guard trip (one that only blocks that
zone's relays, per `relay_authority_zone_blocked()` above) also aborts every
other active zone in a multi-zone firing. Default is abort-the-whole-firing,
per TODO.md 6A.3's explicit rationale (a partially-blind, still-heating
chamber is worse than stopping); an operator can opt into
"continue with the healthy zones" per-board. This sits entirely inside
`profile_executor.c`'s `escalate_guard_trip()` and does not touch either
relay-authority gate — it decides which zones the executor *chooses* to
fault, not whether a relay command is permitted.
