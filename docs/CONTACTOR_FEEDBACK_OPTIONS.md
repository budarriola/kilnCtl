# Welded-contactor detection: CTs vs. relay/contactor feedback — an options assessment

Status: **owner decision, no recommendation implied.** Written 2026-09-04 in
response to `docs/SAFETY_CASE.md` §3 item 4a (the K4/contactor single-failure
finding) to answer one narrower question: is there a cheaper or complementary
way to detect a welded line contactor than fitting current transformers?

Everything below is either (a) read directly off the schematic via the KiCad
MCP server this pass, cited by tool call and net name, or (b) already written
down in `firmware/SaftyFW/docs/HARDWARE.md` / `firmware/KilnFW/docs/HARDWARE.md`
and cited as such. Where the answer depends on a physical part actually screwed
into a terminal block, that is flagged as an owner question, not assumed.

## Headline finding

**No feedback path exists today from the contactor or any SSR back to either
processor.** But the board is not silent by accident of a single missing
part — it has more spare capacity for this than the safety case's current
write-up suggests, on two independent fronts:

1. Every one of the five pilot relays (K1–K5, including K4 itself) is a DPDT
   part with **one entire Form-C contact set wired to nothing.**
2. The ESP32 already has a **spare opto-isolated input, broken out to an
   external screw terminal, doing nothing** — no board rework needed to wire
   an external dry contact into it, if the physically fitted contactor has
   one.

Neither of these was reachable from the CT-gated guards (S3/S4/S9/S14) already
documented as inert in the safety case; both are genuinely new information
about the board's own spare capacity, not proposed additions.

## 1. What the schematic actually shows

### 1a. K1–K5 each have a spare, unconnected contact set

`SSD.kicad_sch` defines the K1–K5 pilot relay as **one symbol instanced five
times** (`kicad_call(get_kicad_component_connections, reference="K4")` shows
`sheetname: /SaftyProcessor/SaftyRelay/`, and the same lib symbol's
`instances` block in `SSD.kicad_sch` lines 2682–2703 lists all five
references — K1, K2, K3, K5, K4 — against the one drawn part). The part is
`Kemet EE2-12NUH`, a DPDT ("2 Form C") relay: two independent common/NO/NC
contact sets sharing one coil.

Tracing K4's eight non-coil pins with `get_kicad_pin_position` (schematic net
per pin, not the PCB netlist — see §4 for why the PCB netlist route came back
empty):

| K4 pin | Net | Used for |
|---|---|---|
| 1 | `12v_Safty` | coil + |
| 12 | `Net-(D9-A2)` (flyback diode) | coil − |
| 8 | `Net-(J10-Pad1)` | contact set 2: NO → J10 pin 1 |
| 9 | `Net-(J10-Pad2)` | contact set 2: COM → J10 pin 2 |
| 10 | `Net-(J10-Pad3)` | contact set 2: NC → J10 pin 3 |
| 3 | `unconnected-(K4-Pad3)` | contact set 1 — **floating** |
| 4 | `unconnected-(K4-Pad4)` | contact set 1 — **floating** |
| 5 | `unconnected-(K4-Pad5)` | contact set 1 — **floating** |

`firmware/SaftyFW/docs/HARDWARE.md` §3 already documents the J10 side of this
(K4 pin 8=NO/1, 9=COM/2, 10=NC/3, traced 2026-08-16) and instructs wiring the
**contactor coil** through J10 pins 1+2 (NO+COM) — that pairing is the
*load-drive* circuit, not a sense line: it fails safe (coil de-energizes when
K4 opens) and is what actually operates the physical contactor. That
documentation is corroborated, not contradicted, by this pass.

What is **not** documented anywhere, because nothing uses it: K4's *other*
Form-C set (pins 3/4/5) is wired to nothing at all — same instantiated symbol,
same board, spare. The same pattern holds for K1/K2/K3/K5 against their own J3
/J4/J8/J11: one contact set drives the SSR-side load, the other set is
unpopulated. This is true in every one of the five instances (confirmed from
the single shared symbol definition rather than five separate checks).

**What this means:** the physical part that would sense "did this relay's
armature actually move" is already on the board, on every relevant relay,
including K4 itself. Using it would need copper (a trace from K4 pin 3, 4, or
5 out to a connector or directly to a GPIO with the right isolation/pull-up)
and possibly a rev of the PCB — not a new relay, not a new footprint.

**Important limit of this contact set, even if wired:** it senses **K4's own
armature**, not the external contactor's. K4 pin-3/4/5 would tell you "did the
Pico's pilot relay open," which the software mirror `relay_owner_is_energized()`
already claims to know (see §2) — except this would be a *real* hardware
confirmation instead of a command echo. It still would **not** tell you
whether the external line contactor downstream of K4's coil-drive contacts
actually opened. Sensing the contactor itself requires a signal from the
contactor's own auxiliary contact (see §1c), not from K4's spare pole.

### 1b. The ESP already has a spare isolated I/O pair, brought out to a connector

`firmware/KilnFW/docs/HARDWARE.md`'s SX1509 pin table (read via `Read`, not
inferred):

| SX1509 pin | Net | Function |
|---|---|---|
| IO4 | `IO_1` | Opto-isolated **input** from J24 (through U13), 2.2k pull-up to 3.3V |
| IO5 | `IO_2` | Drives an opto-isolated **output** to J25 (through U14), 390R |

Traced this pass with `get_kicad_pin_position`: `U13` pin 1 sits on
`Net-(R104-Pad2)`; `R104` pin 1 sits on `Net-(J24-Pad1)` — i.e. `IO_1`'s
isolated input side terminates at **J24**, a 2-way Phoenix screw terminal
(`1935161`, the same generic terminal-block part used for J1/J12/J14/J16/J18
–J25), sitting on `MainControler.kicad_sch`. `IO_1` is not connected, anywhere
in the schematic, to K4, K1–K5, or any of their spare contacts (checked: only
J24 feeds it). It is a **generic, currently-unused, already-isolated external
input**, wired straight to a screw terminal, waiting for something to be
plugged into it.

This means: if the physical line contactor fitted to this kiln has an
auxiliary contact, wiring it to J24 today would get its state into the ESP32
(as `IO_1`) with **zero PCB changes** — the isolation (opto U13), pull-up
(2.2k to 3.3V), and connector are already populated. The only unknowns are (a)
whether the fitted contactor actually has an aux contact to wire there (owner
question, §5), and (b) that this lands on the **ESP**, not the RP2040 — see
§3 for why that matters.

### 1c. `mainFault`/GPIO10 is unrelated — do not conflate it

`U1` (`SaftyProcessor.kicad_sch`) carries `/MainControler/Fault` (ESP GPIO6,
software-driven) across isolation to `/SaftyProcessor/mainFault` (Pico GPIO10).
`firmware/SaftyFW/docs/HARDWARE.md` §4 already documents this at length: it is
an ESP-software heartbeat/fault flag, **not** a sense line from any relay or
contactor, and its own documentation calls out its fail-danger behavior (a
dead ESP reads identically to a healthy one). It is unrelated to the
contactor-weld question and is not proposed as part of either option below.

### 1d. No contactor auxiliary contact is wired anywhere on this board today

Between §1a–§1c, every signal this pass could find that touches K4, K1–K5, or
crosses the main/safety isolation boundary has been accounted for. None of
them is a sense line from the physical line contactor's own auxiliary
contact. **If such a contact exists on the physically fitted part, nothing on
this board reads it yet.**

## 2. `relay_owner_is_energized()` is commanded, not sensed — stated plainly

`firmware/SaftyFW/src/tasks/relay_owner.c` (read only, another session owns
`safety_guards.*`, this file is not touched):

```c
case RELAY_OWNER_CMD_ENERGIZE:
    if (s_state == RELAY_OWNER_STATE_ARMED) {
        gpio_put(SAFTYFW_PIN_RELAY, cmd.energize);
        s_energized = cmd.energize;      // <-- set from the command, not read back
    } else {
        gpio_put(SAFTYFW_PIN_RELAY, 0);
        s_energized = false;
    }
    break;
...
bool relay_owner_is_energized(void) { return s_energized; }
```

`s_energized` is written in the same branch that calls `gpio_put()`, from the
caller's own request (`cmd.energize`) — it is a **software mirror of what was
commanded**, never a read of the pin, never anything derived from a contact
closing. `safety_core.h`'s own comment on the function (`relay_energized comes
from relay_owner_is_energized() (is GPIO6 actually high right now)`) is
accurate about GPIO6 (the Pico is driving its own output pin, so "is GPIO6
high" and "did I just tell it to be high" really are the same fact for that
one pin) but that equivalence stops at the pin — it says nothing about
whether K4's armature moved, whether the coil circuit downstream is intact, or
whether the external contactor closed. **A guard built on this function
compares a command against itself and can never detect a weld** — which is
exactly why S9 (the guard that could catch it) is built on CT current instead,
not on this function.

## 3. The honest comparison

| | Fitting CTs (S9 unblocked, S3 sharpened) | Contactor auxiliary feedback |
|---|---|---|
| **What it directly detects** | Current still flowing through an element when no heat is commanded — i.e., the *downstream symptom* of any weld in the chain (contactor **or** an SSR) | The contactor's own contacts not matching K4's command — i.e., the *specific* upstream cause this document is about |
| **What it does NOT detect** | *Which* device welded (contactor vs. a specific SSR) — it sees current, not which relay is stuck. Also detects nothing until the ESP has actually stopped commanding heat, i.e. it is a post-hoc confirmation, not a pre-emptive one | A welded **SSR** downstream of a healthy contactor — the aux contact only reports the contactor's own state; K1–K5/SSR welds are invisible to it (as they are today) |
| **Where it lands** | RP2040 (`current_task.c` reads the on-board CT ADCs — `CurrentSense.kicad_sch`); this is squarely SaftyFW's own domain, independent of the ESP | Depends which processor: RP2040 needs a **new** input (no CT-analogous slot exists there today, see §4); ESP already has a free isolated input (`IO_1`/J24) but landing it there means the *independent* RP2040 veto is not the one that gets the confirmation — a compromise worth naming honestly |
| **Hardware needed** | 3 CT cores + burden resistors + wiring through the CT loop per phase (`CurrentSense.kicad_sch` already has ADC0/1/2 provisioned — the board expects this, it is just not populated/commissioned) | An auxiliary contact on the **physically fitted** contactor (owner question, §5) + a debounce/isolation path. If the contactor has no aux contact, this option does not exist without swapping the contactor itself |
| **Commissioning needed** | `ct_installed` flag + per-phase calibration + the existing S9/S3 threshold work already scoped in the guard matrix | A one-time "confirm this input tracks commanded state across a manual open/close" bench check per relay wired, plus (if landed on the ESP) a link-frame field and RP2040-side guard logic to consume it meaningfully |
| **Software already exists?** | Guard math (S9/S3) is already written and just gated inert (`ct_installed=0`) — this is described in `docs/SAFETY_CASE.md` §2/§3 already | None — no guard, no link field, no debounce wiring exists for a contactor-state input; would be new work end to end |
| **Are they alternatives or complementary?** | **Complementary, not alternatives.** CTs see "current flowing wrongly," which is the only thing that can catch a welded *SSR*; aux feedback sees "the contactor didn't move," which is the only thing that can catch a welded *contactor* specifically and can do so *before* any current-based symptom appears. Fitting one does not make the other redundant — together they would close both halves of the H3 single-failure table in `docs/SAFETY_CASE.md` §3 item 4a; either alone leaves the other half's failure mode uncovered. |

## 4. Why this more naturally belongs on the RP2040, and why that is not free

The RP2040/SaftyFW side is the "independent veto" — it is the side that is
supposed to be trustworthy even if the ESP is compromised or crashed. A
contactor-state confirmation is only as valuable as the guard's independence
from what it is checking: **feeding a contactor-aux signal into the ESP
(`IO_1`) and trusting the ESP to report it honestly re-introduces exactly the
single point of trust (the ESP) that K4/S9 exist to route around.** For the
signal to mean anything as an independent check, it should land on the
RP2040 — which currently has **no spare GPIO already broken out to an
isolated external connector** the way the ESP's `IO_1`/J24 pair is (this pass
did not find one; `firmware/SaftyFW/docs/HARDWARE.md`'s own GPIO table would
need re-checking by whoever owns that file for a genuinely free pin, since
`safety_guards.*`/pin-table edits are out of scope for this pass). Landing it
on the ESP instead is possible with zero board changes, but the reader should
know that choice trades away the property that makes S9-style detection
valuable in the first place. This tradeoff, not a hardware limitation, is the
real reason a same-side (RP2040) aux input is worth a rev if this path is
pursued at all — it is a design question for the owner, not something this
assessment resolves.

## 5. Questions only the owner can answer

1. **Does the physically fitted line contactor have an auxiliary contact at
   all?** Many industrial contactors (and cheap SSR-adjacent relay modules)
   do not. Nothing in this repo can answer this — it is a fact about a part
   number this pass did not have access to and was constrained not to touch
   (a live thermal campaign is running; no contactor/SSR was inspected or
   tested).
2. **If it has one, is it NO or NC, and what is it currently wired to (if
   anything)?** This determines the polarity logic on the receiving side
   (active-high "closed" vs. active-low "open") — get this wrong and a
   feedback guard fails in exactly the fail-danger way `mainFault` already
   does (§1c).
3. **Same two questions for whichever SSRs are fitted**, since some SSR
   modules also expose a status output; this document's schematic search
   only covers what is already on the kilnCtl PCB, not properties of the
   externally sourced load-switching devices themselves. (`docs/SAFETY_CASE.md`
   §3 item 4a already flags "what contactor and SSR model numbers are
   actually wired at J8/J3/J4/J11/J10" as an open owner question — this is
   the same gap.)
4. **Is a PCB rev acceptable**, or does any fix need to work through the
   existing spare I/O only (J24/`IO_1` on the ESP, accepting the ESP-trust
   tradeoff in §4), or via re-wiring K4/K1–K5's already-present but unrouted
   spare contact set (§1a), which needs new copper regardless?
5. **Is CT fitment already planned/budgeted** (it is flagged blocked on a
   hardware jig per `docs/SAFETY_CASE.md`'s ROADMAP.md M4 reference) — if so,
   the honest complementary relationship in §3 argues for treating aux
   feedback as an addition to that plan, not a substitute being weighed
   against it.

## 6. What was and was not verified

**Verified this pass, by tool call:**
- K4's second Form-C contact set (pins 3/4/5) is unconnected in the schematic
  (`get_kicad_pin_position`, three calls, all returning `unconnected-(K4-PadN)`).
- The same relay symbol is instanced as K1/K2/K3/K5/K4 from one definition in
  `SSD.kicad_sch` (read directly, lines 2559–2706), so the same "one contact
  set wired, one spare" pattern applies to all five by construction, not by
  five independent checks.
- `IO_1` (ESP SX1509 IO4) traces through `U13` to `J24` pad 1, an external
  2-way terminal, with no schematic connection to any K-relay contact
  (`get_kicad_pin_position` on U13, R104; cross-referenced against
  `firmware/KilnFW/docs/HARDWARE.md`'s own pin table).
- `relay_owner_is_energized()` returns a value set from the command, not read
  back from any pin (`firmware/SaftyFW/src/tasks/relay_owner.c`, read only).
- K4↔J10 pin mapping (8=NO/1, 9=COM/2, 10=NC/3) matches
  `firmware/SaftyFW/docs/HARDWARE.md` §3's own 2026-08-16 trace.

**Not verified — inferred or explicitly out of reach:**
- Whether the physical line contactor or any SSR fitted to this specific rig
  has an auxiliary/status contact at all (§5, owner question).
- Whether the RP2040 side has a genuinely free GPIO for a same-side feedback
  input — this pass did not do a full free-pin audit of SaftyFW's own pin
  table, which belongs to `safety_guards.*`'s current owner.
- The PCB (as opposed to schematic) netlist for K4/J10/J24 — `get_kicad_net`
  and `find_kicad_components_by_net` returned "not found" for these
  auto-generated net names, consistent with the stale/unbuilt PCB netlist
  already noted in `docs/SAFETY_CASE.md` §3 item 4a; this document relies on
  the schematic-derived pin-to-pin tracing (`get_kicad_pin_position`) instead,
  which is direct schematic evidence, not a PCB continuity measurement.
- No physical hardware was touched, reset, flashed, or tested — read-only
  investigation only, per this task's constraints, with a thermal campaign
  live on the bench rig throughout.

## 7. Correction: no coupling transformer, part number was mis-recorded

The Hammond 140QEX referenced in the earlier version of this section, and
the datasheet analysis built on it (1 kHz vs. 60 Hz inductance, burden
loading, saturation/distortion margin), do not apply to this hardware. The
part number was a mis-record. There is no CT coupling/isolation transformer
in this design.

The actual current-sense hardware is split-core current transformer probes
with a 1 V output at full scale, where the amp rating equals the full-scale
value (the bench unit is a 1 A probe: 1 A : 1 V, with a ~+59 mV idle
offset). A summed-heater CT is fitted on RP2040 GPIO28 (2026-09-05). The
calibration model is `amps_at_full_scale` plus a zero offset. See
`firmware/SaftyFW/docs/CURRENT_SENSE.md` and `CT_COMMISSIONING_PLAN.md`
for the full model and commissioning procedure. The 60 Hz inductance
question this section previously raised is moot and has been dropped.
