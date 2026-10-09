# System Architecture — both processors and the boards under them

> **Status:** written · **Last reviewed:** 2026-09-04
> Scope is deliberately narrow: only what is true of the *system* — both
> processors, the wire between them, the power domains, cross-processor
> ordering. Task/driver structure lives in each firmware's own
> `ARCHITECTURE.md`; the wire format lives in
> [`../firmware/CommonFW/docs/LINK_PROTOCOL.md`](../firmware/CommonFW/docs/LINK_PROTOCOL.md);
> hazard/risk argument lives in [`SAFETY_CASE.md`](SAFETY_CASE.md). See the
> stub history at the bottom of this file for why it exists at all.

## 1. The two processors, in one sentence each

- **`KilnFW`** (ESP32-S3, `mainBoard`) — Wi-Fi, web/LCD UI, thermocouples,
  PID, profile execution. **Commands** heat. Cannot itself guarantee a relay
  is safe to energize.
- **`SaftyFW`** (RP2040, A1) — independent overtemp/fault detection, owns the
  mechanical pilot relay K4. **Vetoes** heat. Never asked to compute a
  setpoint, run a PID loop, or trust anything it cannot check itself.

They are two independently-clocked, independently-flashed programs. Nothing
below is true of one firmware in isolation — see each firmware's own
`docs/ARCHITECTURE.md` for that.

## 2. End-to-end command path: "start a firing"

Concrete hop-by-hop trace, ESP web UI (LCD is the same handler, different
caller) to a heating element closing.

| # | Where | What happens |
|---|---|---|
| 1 | Browser / LCD `ui_page_home_actions.c` | `POST /api/profile_exec/start` |
| 2 | `dashboard_exec_http.c:636` `profile_exec_start_post_handler()` | Registered in `dashboard_http.c:614`; refuses if `danger_mode_active()` (manual override in progress) |
| 3 | `profile_executor.c` / `profile_executor_run.c` | State machine enters `RUNNING`; on every tick asks `heat_enable_init()`'s holder to request `SAFETY_CMD_REQUEST_ENABLE` (`heat_enable.c:129`, *"safety processor asked to permit heating (K4)"*) |
| 4 | `safety_link_commands.c` / `safety_link.c` | Encodes the request as an unacknowledged `BROADCAST` (§3 below), and separately builds `SAFETY_CMD_PUSH_CONTEXT` every 500 ms carrying zone setpoints/measured/relay masks (`safety_link_frames.c`) |
| 5 | UART1, 230400 baud, through U6 (ADuM1201WT digital isolator) | Frame crosses `GND_Main` → `GND_Safty` |
| 6 | `SaftyFW` `link_task.c` | Decodes the frame, publishes a snapshot; `safety_core_task` never touches the link directly (§3) |
| 7 | `safety_core.c` builds guard input, `safety_guards.c` evaluates every guard | If none tripped **and** `commissioning_gate.h` is satisfied **and** `enable=1` was requested, `relay_owner_command_energize()` is issued |
| 8 | `relay_owner.c` | Drives K4 (mechanical pilot relay) — the RP2040's own GPIO, physically isolated from the ESP's own relay-driving path |
| 9 | Back on the ESP, in parallel | `profile_executor_relay_io.c` calls `kiln_io_owner_command_set_relay_mask_authorized()` (the ESP-local element relays K1–K3, via the SX1509 expander), gated on `relay_authority_on_blocked()` — refuses if `safety_link_get_fault_sources()` is nonzero or the link has never come up |

**Who commands, who vetoes.** The ESP drives K1–K3 (the element relays) and
*asks* for K4. The RP2040 drives K4 itself and never asks anyone; it is the
only processor that can make K4 close, and every guard trip goes straight to
"de-energize K4" with nothing the ESP can do to override it (`safety_core.c`
comments at lines 1108/1156/1202/1340 — "de-energizes K4 first, before
anything else, before logging"). **Heat requires both**: K1–K3 closed *and*
K4 closed. Either processor withholding its half is enough to stop current
flowing — this is the asymmetric veto the rest of this document keeps
coming back to.

## 3. End-to-end safety event: a guard trip reaching the operator

| # | Where | What happens |
|---|---|---|
| 1 | `safety_guards.c` | A guard (e.g. S1 absolute overtemp) evaluates true; `safety_core.c` latches `trip_mask`, de-energizes K4 immediately |
| 2 | `link_task.c` | Pushes `SAFETY_CMD_TRIP_EVENT` (Frame D, `0x0D`) **immediately**, not on the 500 ms cadence — repeated a few times since there is no ACK — carrying the deciding temperature/threshold/current values captured *at the trip instant* |
| 3 | UART1 → U6 → ESP | `safety_link_frames.c`'s `safety_apply_trip_event()` decodes it, dedups on `trip_seq` |
| 4 | `safety_link.c` | Also learns the trip from the 500 ms `DIAG` frame's `trip_mask`/`trip_reason` fields (Frame B) as a backstop if Frame D was lost entirely |
| 5 | `dashboard_status_http.c` / `GET /api/status` | Exposes trip reason + deciding values |
| 6 | `safety_page.html` / `main_page.html` | Renders the "SAFETY TRIP" banner; `SAFETY_TRIP_INEFFECTIVE` (S9 — power still flowing) gets distinct wording pointing at the breaker, not the kiln |
| 7 | ESP relay-on paths | `relay_authority_on_blocked()` is not directly wired to a *trip*, only to the *fault-source* mask and link-down — a pure Pico-side trip with the link otherwise healthy blocks new heat because `profile_executor` reads the trip mask on its own poll and faults itself (`profile_executor.c:1219` region), not because K1–K3 are separately vetoed by the trip frame |

**Independent of the ESP entirely.** Steps 1–2 above happen whether or not
the ESP is listening, running, or even powered — K4 drops the instant the
guard trips, before any frame is built. The frame exists so the *operator*
finds out why; it plays no role in the safety action itself.

## 4. The board-to-board interface

Full contract: [`LINK_PROTOCOL.md`](../firmware/CommonFW/docs/LINK_PROTOCOL.md).
Summary of what a system-level reader needs:

| Property | Value | Note |
|---|---|---|
| Physical crossing | Two UART lines through U6 (ADuM1201WT digital isolator, non-inverting), one fault line through U1 (TCMT1109 optocoupler) | U6 replaced a TCMT1109 pair (U2/U3) on 2026-08-25 |
| Baud | **230400** | Was 9600 under the retired optocoupler pair — that ceiling was the part, not either UART peripheral (`LINK_PROTOCOL.md` §3) |
| Frame model | `BROADCAST`: fire-and-forget, no ACK, no retry, dropped silently on a full TX ring | The Pico **never** waits on the ESP — §2's governing constraint |
| Fault line | ESP GPIO6 → U1 → Pico GPIO10, active low | **Cannot detect a dead ESP**: unpowered main board reads as healthy on this line alone (`LINK_PROTOCOL.md` §5) |
| Link-down thresholds (ESP side) | >1.5 s silence: block new heat-on, self-healing. >30 s: abort a running firing | `LINK_PROTOCOL.md` §8 |
| Link-down thresholds (Pico side, S6b) | 10 s soft (warns if current is flowing with no context) / 120 s hard (trips) | `safety_guards.c:27-28`, `LINK_TIMEOUT_S_DEFAULT`/`LINK_DEAD_HARD_S_DEFAULT` |
| Who commands heat | ESP (K1–K3 element relays) | Refused by `relay_authority_on_blocked()` if the safety link is down or faulted |
| Who vetoes heat | RP2040 (K4 pilot relay) | Never blockable by the ESP; K4 is the RP2040's own GPIO |
| Protocol version check | Mutual, both directions (`ANNOUNCE_VERSION`) | A mismatch makes the ESP treat the link as dead; the Pico enters `DEGRADED_NO_CONTEXT` **without** latching a trip — a version skew during development must not read as a dangerous kiln |
| Who owns the Pico's configuration | The Pico (its own 4 K record store). The ESP holds only a *snapshot* of it inside each saved kiln package | Applying a kiln package pushes both halves in one transaction (`kiln_cfg_swap.c`), on its own task — never on an httpd worker |
| If the two halves disagree | Standing **config divergence**: alarm, heaters disabled, latched with no operator dismiss control until the halves match again | Compared by format version + hash identity, not field-by-field. Surfaced by `/api/status`'s `safety_diverged` (boolean only) and, with the reason text, by `/api/readiness`'s `safety_ceiling_match` item |

**Ground-bonding hazard, stated once because it matters at the bench, not
just on paper:** a debug probe or USB cable on the Pico ties `GND_Safty` to
the PC's ground; if the ESP is *also* plugged into the same PC, `GND_Main`
and `GND_Safty` are bonded through the PC and the isolation barrier is
bypassed for the duration (`firmware/SaftyFW/docs/HARDWARE.md` §"Bench
state"/around line 600–605).

### A gap this document found, not previously written down anywhere

`SAFETY_CMD_ANNOUNCE_REBOOT` (`0x18`) is implemented and load-bearing on both
sides — `KilnFW`'s `safety_link_commands.c` sends it before a self-initiated
reboot; `SaftyFW`'s `link_task.c` (`link_task_handle_announce_reboot()`) and
`reboot_announce.c`/`safety_core.c:170` (`REBOOT_GRACE_WINDOW_MS = 20000`)
consume it to suppress an S6b nuisance trip during a routine ESP OTA
reboot — but it is **not documented in `LINK_PROTOCOL.md`** at all, whose
last review predates this frame. `LINK_PROTOCOL.md`'s own header rule ("if it
disagrees with the code, the code wins — fix this file and say so") applies:
this is an omission there, not a disagreement to arbitrate, and is flagged
for that document's own next pass rather than fixed here, since fixing the
wire contract doc is out of this document's scope.

## 5. Power domains and ground crossings

From `firmware/KilnFW/docs/HARDWARE.md` §"Power" and
`firmware/SaftyFW/docs/HARDWARE.md` §1/§6, cross-checked against each other —
they agree.

| Rail | Domain | Feeds |
|---|---|---|
| 12 V input | Both — **separately regulated per domain**, not shared | Relay coils, each domain's own 5 V switcher |
| 5 V | `GND_Main` and `GND_Safty` independently (each side "has its own 5 V switcher and 3.3 V rail") | Logic supply headroom, display (main side only) |
| 3.3 V | `GND_Main` and `GND_Safty` independently | ESP32-S3 / RP2040 I/O, sensors |

**The two ground domains share no connection except through the isolation
barrier** (U6 for the UART pair, U1 for the fault line) — this is the single
sentence both `HARDWARE.md` files use verbatim-in-spirit, and the schematic
sheet `/SaftyProcessor/` is where `GND_Safty` is defined as its own net.
Every other net crossing the two domains (there should be none outside U1/U6)
would be a hazard by definition; this document did not find one, but did not
independently re-derive the full netlist either — see §7's honesty note.

## 6. Cross-processor boot and shutdown ordering

**Neither side waits for the other to exist.** Both firmwares boot
independently; "the safety processor must be alive to heat" (§4's link-down
table) is enforced entirely by the ESP refusing to command heat, never by
either boot sequence blocking on the other's presence.

### ESP boot order (`firmware/KilnFW/App/main.c`, split 2026-09-04 by boot phase)

`app_main()` calls, in this fixed order (order is semantics — tuned against
real hardware failures, per the file's own top comment):

1. `main_boot_early()` — reset-reason/coredump diagnostics, Wi-Fi+mDNS, I2C
   bus, SX1509/`kiln_io_init()` (**relays latched off first**), `boot_guard_init()`
   (must run *after* relays are off, *before* anything that could hang),
   MAX31856 thermocouples, display/touch.
2. `main_control_bringup()` — `safety_link_start()` (comes up whether or not
   the RP2040 answers — "a silent far side is `link_up = 0`, not a startup
   failure"), `heat_enable_init()`, `kiln_io_owner_start()`,
   `profile_executor_start()`, `autotune_engine_start()`.
3. `main_network_http_bringup()` — every HTTP route, the PC-link UART owner.
4. `main_bridges_bringup()` — UART bridge tasks, LVGL, the fail-safe link
   watchdog.

### RP2040 boot order (`firmware/SaftyFW/src/main.c`, header comment)

Steps 1–7 bring up FreeRTOS/GPIO6-low/watchdog/tasks; step 8 enters **GRACE**
for `startup_grace_s` (default 60 s) with the relay held de-energized; step 9
transitions GRACE → **ARMED**, at which point K4 may be energized **iff no
guard is tripped**. The RP2040 does not know or care whether the ESP has
booted yet — GRACE is a fixed timer from the RP2040's own boot, not gated on
receiving a first frame.

### The boot guard's RECOVERY MODE — a real incident, same day as this doc

`boot_guard.h` counts consecutive un-confirmed-healthy ESP boots; at
`RECOVERY_MODE_BOOT_THRESHOLD` (3) the **next** boot enters recovery mode:
relays latched off, only Wi-Fi + OTA HTTP routes + read-only pages start.
`main_control_bringup.c` explicitly skips `profile_executor_start()` and
`autotune_engine_start()` in this mode, and `main_bridges_bringup.c` skips
starting LVGL.

**A consumer that ignored this bricked the board on 2026-09-04.**
`screen_idle.c`'s policy tick called `profile_executor_get_status()`,
`dashboard_get_status()` and `autotune_engine_is_active()` unconditionally —
`screen_idle_start()` runs in `main_boot_early()`, *before*
`main_control_bringup.c`'s recovery-mode skip even executes — and after
495+ consecutive unconfirmed-healthy boots the board hit exactly this path
and asserted on the executor's uninitialized mutex. The fix was two-layered:
every public entry point in `profile_executor.c`/`autotune_engine.c` was
hardened to answer cleanly when its mutex is `NULL` (host-tested in
`test_*_prestart.c`), **and** `screen_idle.c` gained its own caller-side gate
that returns before reaching those calls at all when
`boot_guard_is_recovery_mode()` is true (`screen_idle.c:150`,
`test_display_power_wiring.c:417` pins the source-order requirement). The
lesson generalizes: **recovery mode is not a mode any new subsystem gets to
assume is "someone else's problem" — every poll loop that reaches into
executor/autotune state must itself be recovery-mode-safe**, because
recovery mode is decided once at boot and does not retroactively stop code
that was already running from calling in.

### The reboot-announce grace window

Before a self-initiated ESP reboot (OTA apply, e.g.), `safety_link_commands.c`
sends `SAFETY_CMD_ANNOUNCE_REBOOT` (`0x18`, see §4's gap note — undocumented
in `LINK_PROTOCOL.md` but implemented and consumed). `SaftyFW`'s
`reboot_announce.c` records the local (Pico) uptime at receipt;
`safety_core.c` computes `age_ms < REBOOT_GRACE_WINDOW_MS` (20 000 ms) as a
pure fact fed to S6b, which suppresses *only* S6b's own link-down trip for
that window — it does not touch `link_up` itself, does not affect any other
guard, and grants no heating permission. Without this, a routine ~10–20 s ESP
reboot (OTA apply, or any reset) would look identical to a dead main
controller and trip S6b's link-loss guard on a healthy, expected event.

### Unexpected reset of either side

- **ESP resets unexpectedly** (no `ANNOUNCE_REBOOT` sent): the Pico sees
  telemetry stop; S6b's ordinary 10 s soft / 120 s hard timers apply with no
  grace suppression — a crash is indistinguishable from a hang by design,
  which is the point of a hard timeout that does not depend on the crashing
  side announcing itself.
- **RP2040 resets unexpectedly**: `boot_id` changes (a fresh random value
  per boot, `safety_link.c:358` — see §7's note below on the doc/code
  deviation there), which the ESP uses to reset every correlation window and
  re-request `GET_FW_VERSION`. Until the ESP's own `SAFETY_LINK_UP_PERIODS`
  (3 × 500 ms = 1.5 s) elapses with no frame, new heat-on is blocked; the ESP
  does **not** reboot alongside the Pico, so an S6a fault the ESP asserted
  before the Pico reset can outlive the Pico's own reboot (`safety_link.c`'s
  `safety_link_mark_boot_clean()` doc comment) — a stale latch from a
  *previous* ESP boot only clears once the ESP explicitly says "this boot's
  own bring-up found nothing wrong."

## 7. Honesty notes — gaps, deviations, and what this document did not check

- **§4's `ANNOUNCE_REBOOT` gap** is real and named above, not silently fixed.
- **`boot_id` is randomized, not incremented**, contrary to `LINK_PROTOCOL.md`
  §4's literal text ("increments on every ESP boot"). `LINK_PROTOCOL.md`'s
  own completion checklist already flags this as a deliberate, documented
  deviation ("ticked on behavior, not on the literal wording") — repeated
  here because it is exactly the kind of doc/code mismatch this document's
  brief asked to be checked, and it was already caught by the other
  document's own 2026-09-04 pass, not invented here.
- **This document did not independently re-derive the full net-crossing list**
  between `GND_Main` and `GND_Safty` from the KiCad netlist — it relies on
  `firmware/SaftyFW/docs/HARDWARE.md`'s claim that U1/U6 are the only
  crossings. That claim is schematic-derived in that document, not
  re-verified here against `hardware/mainBoard/kiln.kicad_pcb` directly.
- **§2/§3's traces are current as of the code cited**, but this system has
  had several "shipped, then found unreachable/inert" features (see
  `ROADMAP.md`'s decisions table) — a future reader should re-check the
  cited line numbers rather than trust them indefinitely; they will drift.
- **No claim in this document was restated from another doc without checking
  it against code.** Where a claim originates in another doc (e.g. the power
  rail table, the fault-line polarity), it was cross-checked against the
  current file, not assumed current from the other document's own "Last
  reviewed" date.

## 8. What does not belong here (unchanged from the stub)

- Anything true of only one firmware → that firmware's own `docs/`.
- The wire format itself → [`LINK_PROTOCOL.md`](../firmware/CommonFW/docs/LINK_PROTOCOL.md).
- Which faults trip and why → [`SAFETY_MODEL.md`](../firmware/KilnFW/docs/SAFETY_MODEL.md) / `SaftyFW`'s equivalent.
- Task and driver structure → the per-firmware `ARCHITECTURE.md` files.

---

## Completion checklist

- [x] Written, or deliberately deleted — **written**, 2026-09-04
- [x] Referenced from [`../ROADMAP.md`](../ROADMAP.md) if it survives — 2026-09-04
