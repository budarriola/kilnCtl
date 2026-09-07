# Safety Model

> **Status:** planning · **Last reviewed:** 2026-08-19
> **Keep this file current.** If a guard, threshold or policy changes, update it
> in the same commit as the code. If this file and the code disagree, **the code
> wins** — fix this file and say so in the commit message. A completion
> checklist is at the bottom; tick items as they are built *and verified*, and
> keep the two distinct.

What the safety processor is for, what it will actually trip on, and — just as
important — what it will deliberately **not** trip on.

This is the design document the rest of `SaftyFW` answers to. If a guard in the
code and a guard in this file disagree, one of them is a bug; check the code
before assuming the file is right, the way `firmware/KilnFW/docs/SAFETY_MODEL.md` asks
you to.

---

## 1. The job

The main controller (ESP32-S3, `KilnFW`) runs the kiln: PID, profiles, ramps,
its own seven-guard `thermal_guard` suite. It is a sophisticated,
network-connected, feature-rich piece of software, and it is where bugs live.

The safety processor exists on the assumption that **the main controller is
wrong**. Its job is not to control the kiln, improve the firing, or duplicate
`KilnFW`'s guards. Its job is to notice that something has gone badly wrong and
open a mechanical contactor.

Three properties follow, and everything else is downstream of them:

**It must be independent.** The safety processor's primary trips use its *own*
thermocouple, its *own* current sensing, and its *own* E-stop input. It shares
no sensor, no bus and no ground with the main controller. Information from the
main controller makes some *secondary* checks smarter, and its **absence must
never, by itself, trip anything**.

Two deliberate, bounded exceptions, both of which must be understood rather than
glossed over:

- The frame codecs are shared code (`CommonFW`) — but they are pure,
  allocation-free, host-tested functions with no I/O and no state. Sharing a
  serializer is not sharing a failure mode; **re-implementing it twice and
  letting the two drift is a far larger risk**, and this project already has a
  documented instance of exactly that drift.
- `tc_source = BORROWED_ZONE` genuinely does trade away sensor independence, and
  §3 says so plainly rather than burying it.

**It must be simple.** Every line of code here is a line that can fail in the
one component whose failure is unmitigated. There is no PID, no profile engine,
no network stack, no filesystem, no display, and no dynamic allocation after
init. The guard evaluation is a pure function of a snapshot struct, testable on
a host with no hardware — the same pattern `KilnFW` already proved with
`thermal_guard.c` and `pid.c`.

**It must not cry wolf.** See below, because this is the requirement most
likely to be quietly violated.

---

## 2. The nuisance-trip doctrine

A safety system that trips spuriously gets bypassed. Not maybe — reliably, by a
reasonable person, at 2 a.m., eleven hours into a twelve-hour glaze firing, with
a jumper. A guard that fires on a healthy kiln is not "cautious"; it is a guard
that will be removed, taking with it the protection it was supposed to provide.

So the design rule is:

> **Every trip must clear two independent bars: a magnitude that correct
> operation cannot reach, and a duration that a transient cannot sustain.**
> One alone is never enough.

Applied consistently, that produces the following house rules.

**Thresholds sit outside the operating envelope, not at its edge.** The
absolute over-temperature limit is not "the profile's peak". It is the profile's
peak plus a margin large enough that a normal overshoot, a thermocouple
tolerance stack, and a cold-junction error together cannot reach it.

**Durations are measured in the units the physics happens in.** A kiln has a
thermal time constant of many minutes. Nothing thermally dangerous develops in
200 ms. Where a guard's evidence is thermal, its window is tens of seconds to
minutes — and it costs nothing, because the hazard is that slow too. The
exceptions are the two signals that are *electrically* unambiguous — the E-stop,
and an explicit fault assertion from the ESP — which are debounced only enough
to reject contact bounce and noise.

**Two classes of response, and most findings are not trips.**

| Class | Action |
|---|---|
| **WARN** | Reported over the link and in the diagnostic frame. **No relay action.** The operator and the main controller find out; the firing continues. |
| **TRIP** | K4 de-energizes, the contactor opens, and the condition **latches**. |

The default for a new guard is WARN. Promoting one to TRIP requires an argument
in this document about what physical harm it prevents. "It seemed unsafe" is not
that argument.

**A failed *sensor* is not the same as a failed *kiln*.** The safety
thermocouple going open-circuit means the safety processor is blind. Blind is
bad, but blind is not on fire, and a connector that wiggles for 900 ms should
not end a firing. Guard S5 handles this with a graduated response rather than
an instant trip.

**Absence of information is not evidence of danger — except when heat is on.**
This is the single most useful idea in the whole design. If the link to the main
controller drops and no current is flowing, nothing hazardous is happening;
warn and keep watching. If the link drops *while current is flowing*, then
something is heating the kiln with nobody in charge of it, and that is a trip.
Guard S6 is built on this distinction, and it is both less twitchy and strictly
safer than a flat "link lost ⇒ trip" timeout.

**Latching is not auto-recovery.** Once tripped, the safety processor stays
tripped until an operator explicitly clears it. There is no condition-cleared
auto-reset, because "the temperature came back down after I cut the power" is
not evidence that the fault is gone — it is evidence that the trip worked.
This mirrors `thermal_guard.h`'s "latching, always".

**Startup is not steady state.** No thermal or correlation guard arms until the
system has been running for `startup_grace_s` (default **60 s**) *and* has
accumulated enough valid samples to have an opinion. A guard evaluating a
half-filled rolling window is a guard evaluating noise.

---

## 3. What it is watching

| Input | Source | Owned by |
|---|---|---|
| Safety thermocouple temperature + cold junction + fault bits | MAX31856 on J7 daughterboard, SPI0 | `thermo_task` |
| Three current channels | ADC0/1/2, peak-hold front end | `current_task` |
| E-stop | GPIO9, active high = stop | `discrete_task` |
| `mainFault` from the ESP | GPIO10, active **low** | `discrete_task` |
| Zone setpoints, measured temps, relay commands | ESP, over the isolated UART | `link_task` |
| Link liveness | derived from frame arrival times | `link_task` |

The last two are **context**, not primary evidence. See §5.

### What the current channels are for — and are not

The three current channels exist to answer exactly two questions:

1. **Is the load actually drawing current right now?**
2. **Roughly how much power is going into the kiln?** — reported to the ESP for
   the GUI.

They are **not** an over-current or under-current protection device, and no
guard trips on a current *magnitude*. Fusing, breaker sizing and element
protection are the electrical installation's job, and they are much better at
it than a CT on a 12-bit ADC with a 1 s peak-hold in front of it.

Everything the current channels do is **presence/absence detection** against a
coarse `i_present_a` threshold, plus a reported estimate that no guard reads.
That is a deliberate scope limit and it makes the calibration burden much
lighter: "is there current" needs a threshold accurate to roughly a factor of
two, not a percent. See `CURRENT_SENSE.md` §2.

### Where the safety temperature comes from

Two commissioning decisions, both required, both statements about physical
reality rather than preferences. Full driver detail in `THERMOCOUPLE.md` §3.

#### `tc_source` — which sensor

| Value | Meaning |
|---|---|
| `OWN_J7` | The Pico's own MAX31856 on the J7 daughterboard. **Fully independent.** |
| `BORROWED_ZONE` | One of the **main board's** zone thermocouples, arriving in the context frame. `borrowed_zone_index` selects it |
| `BOTH` | `OWN_J7` is the primary trip sensor; the borrowed channel is a continuous cross-check. **Recommended where both exist** |

> ⚠️ **`BORROWED_ZONE` alone trades away the independence that justifies this
> board.** The reading is measured by the main board's MAX31856, read by the
> main board's SPI driver, packed by the main board's firmware, and delivered
> over a link the main board controls. Every one of those is a component the
> safety processor exists to distrust.
>
> It is a legitimate configuration — a board built without the J7 daughterboard
> is better off with a borrowed reading than with no temperature at all — but it
> must be a deliberate choice, and the system must not pretend otherwise. In
> `BORROWED_ZONE`:
>
> - The safety processor reports `SAFETY_FLAG_BORROWED` in every status frame,
>   and the GUI must label the temperature as borrowed.
> - **S6 (link dead) becomes the primary temperature protection**, because a
>   dead ESP now means no temperature at all, not merely no context.
> - **S13** exists specifically to catch the failure this mode introduces.
> - `TRIP_INEFFECTIVE` and the current guards are unaffected — they use the
>   Pico's own ADC and are independent in every mode.

**A borrowed channel is by definition `CHAMBER_AGREED`** and must be: it is one
of the sensors the zone guards use, measuring the same chamber. `tc_placement_mode`
is forced to `CHAMBER_AGREED` when `tc_source` is `BORROWED_ZONE`, and a
configuration that says otherwise is rejected rather than reconciled.

In `BOTH`, the two sources are compared continuously by S10 with the *own*
sensor as the reference — which is the strongest configuration available,
because it is the only one where a drifting or frozen sensor on either board is
visible from the other.

#### `tc_placement_mode` — where it is mounted

Applies to `OWN_J7`. The safety thermocouple is **not** assumed to agree with
the main board's zone thermocouples; whether it should is a per-kiln decision:

| `tc_placement_mode` | Meaning | Effect |
|---|---|---|
| `CHAMBER_AGREED` | Mounted in the chamber, measuring the same thermal space the zone TCs do. Expected to broadly track them | S1's ceiling may be tightened by the firing target. **S2 and S10 active** |
| `EXTERNAL_OVERHEAT` | An independent overheat sensor — kiln shell, exhaust, element chamber, enclosure, a different zone entirely. **No relationship to the zone readings is expected** | S1 uses a fixed, independently commissioned limit. **S2 and S10 disabled** |

This is not a tuning knob, it is a statement about physical reality, and getting
it wrong breaks guards in opposite directions:

- Declaring `CHAMBER_AGREED` for a shell-mounted sensor makes S10 trip
  constantly on a 700 °C disagreement that is entirely correct, and makes S2
  compare a shell temperature against a chamber setpoint.
- Declaring `EXTERNAL_OVERHEAT` for a chamber sensor silently discards the only
  cross-check the system has.

**There is no safe default, so there is no default.** `tc_placement_mode` is a
required commissioning field; until it is set, S2 and S10 stay off and S1 uses
the fixed limit — the conservative reading of an unanswered question.

> This was **false in the code until 2026-08-27**, and worth recording because
> the failure was invisible from either side alone. The enum cannot express
> "unset": `CHAMBER_AGREED` is 0, which is both the zero-initialised value and
> the value that *arms* S2 and S10. So every uncommissioned board ran both
> guards against a sensor whose placement had never been declared — the exact
> opposite of the paragraph above, and a nuisance-trip generator on any board
> whose safety thermocouple is shell- or exhaust-mounted. The fix is a separate
> `tc_placement_valid` flag rather than renumbering the enum, because those
> values are on the wire and in every already-commissioned board's flash
> record. **When a field has no safe default, the type must be able to say
> "unset" — a sentinel that collides with a real, guard-arming value is not a
> sentinel.**

---

## 4. The guard suite

Nine guards. Threshold names are configuration fields, defaults given.
Every one of them is a bench-tunable number, not a `#define` buried in a `.c`.

### S1 — Absolute over-temperature · **TRIP**

The reason the board exists.

```
CHAMBER_AGREED:     ceiling = min(abs_max_temp_c, firing_max_c + firing_margin_c)
EXTERNAL_OVERHEAT:  ceiling = abs_max_temp_c            ← fixed, always

safety_tc_c > ceiling   for  3 consecutive valid readings  (~300 ms)
```

`abs_max_temp_c` has **no default** and must be commissioned. In
`CHAMBER_AGREED` it is the highest temperature the kiln furniture and elements
can survive (of the order of 1300 °C for a cone-10 kiln — above cone 10's
~1285 °C with margin, and *not* the hottest profile). In `EXTERNAL_OVERHEAT` it
is whatever that particular sensor's location must never exceed — a shell
temperature limit, an exhaust limit, an enclosure limit — and it bears no
relation to any firing temperature at all.

`firing_margin_c` = **100 °C**.

**The firing-target tightening only applies in `CHAMBER_AGREED`.** `firing_max_c`
is the highest target the running profile will ask for, sent by the ESP at
profile start (`LINK_PROTOCOL.md` §4). In a chamber-mounted installation it is
genuinely useful: a single fixed ceiling protects the *hottest firing the kiln
will ever do*, so a 900 °C bisque otherwise runs with 400 °C of unprotected
headroom. Taking the profile's own peak into account tightens protection to each
firing's actual envelope with no threshold editing by anyone.

Applied to an externally-mounted sensor it would be nonsense — a shell
thermocouple reading 80 °C has no business being compared against a 1250 °C
firing target — so in `EXTERNAL_OVERHEAT` the field is ignored entirely.

**The ceiling can only ever tighten.** `min()` means a hostile or buggy ESP
sending `firing_max_c = 5000` gets clamped to `abs_max_temp_c`, not obeyed. The
main controller is permitted to ask for *more* protection and never for less —
that asymmetry is the only reason it is safe to accept this number from the
component under suspicion. When no firing is running, `firing_max_c` is absent
and the ceiling is simply `abs_max_temp_c`.

This is the one thermal guard with a short debounce, and deliberately so: an
absolute overtemp is never a transient, three consecutive MAX31856 conversions
already reject any plausible glitch, and the cost of waiting is measured in
element life.

**Independent of everything that matters.** Needs no link and no current data;
context can only make it stricter. If every other guard in this list were
deleted, S1 alone would justify the board.

### S2 — Sustained excess over setpoint · **TRIP** · *needs context* · **`CHAMBER_AGREED` only**

```
safety_tc_c > max(active zone setpoints) + overshoot_margin_c
  continuously for overshoot_time_s
```

Defaults: `overshoot_margin_c` = **75 °C**, `overshoot_time_s` = **120 s**.

Both numbers are deliberately generous. Real kilns overshoot at the end of a
ramp, thermocouples disagree with each other by tens of degrees depending on
placement, and the safety TC is in a different part of the chamber from any
zone TC. A 40 °C transient overshoot is normal operation. A sustained 75 °C
excess for two full minutes is a control loop that has lost the plot.

**Inactive when context is stale or absent**, and **inactive entirely in
`EXTERNAL_OVERHEAT`** — comparing a shell or exhaust reading against a chamber
setpoint is meaningless. S1 still covers the absolute case in both modes, so
this guard degrades to "off", never to "trip".

### S3 — Load active with no heat commanded · **TRIP** · *needs context*

The welded-SSR guard, and after S1 the most valuable thing here. A
**presence/absence** test, not a current-magnitude test.

```
any channel  I > i_present_a
  AND  no relay commanded on during the last  correlation_window_s
  sustained for  stuck_on_time_s
```

Defaults: `i_present_a` = **2.0 A** (well above the zero-drift floor, well
below any real element), `correlation_window_s` = **150 s**,
`stuck_on_time_s` = **20 s**.

`i_present_a` is a **load-active threshold, not a current limit.** It only has
to sit between "measurement noise" and "an element is conducting", which is a
gap of one to two orders of magnitude — so it tolerates a badly calibrated CT,
a wrong-ratio CT, and a mediocre ADC reference without any change in behaviour.

The 150 s window is not padding. `KilnFW` renders duty on a **60 s**
time-proportioned window (`profile_executor.c:27`), and the current front end
has a **1 s** peak-hold decay. So the window must cover at least two full
heater windows plus the decay tail before "no heat was commanded" means
anything at all. See `CURRENT_SENSE.md` §3.

This guard consumes `relay_recent_mask` — *"was any relay commanded on at any
point in the last N seconds"* — computed on the ESP, **not** the instantaneous
mask. Correlating against the instantaneous mask would trip on every healthy
low-duty firing.

Why this earns a TRIP where its inverse (S4) does not: current flowing with
nothing commanding it means a switching element has failed closed. The
temperature has not necessarily risen *yet* — which is exactly the point. This
catches the failure before `KilnFW`'s own runaway guard 3 would, and it catches
it even if the main board is the thing that has failed.

### S4 — Heat commanded but load inactive · **WARN only** · *needs context*

```
relay commanded on throughout the last  correlation_window_s
  AND  all channels  I < i_present_a
```

**This never trips, and that is a design decision, not an oversight.** A dead
element, a blown fuse, or a failed-open SSR ruins a firing and wastes a day. It
does not start a fire. Tripping the contactor in response would convert a
recoverable problem into an identical outcome plus an alarm.

It is reported prominently — it is genuinely useful diagnostic information, and
it is the earliest possible warning of an element approaching end of life — but
the decision of what to do about it belongs to the operator and to `KilnFW`,
which has guard 1 (heating-failed) for exactly this and much better context to
judge it with.

### S5 — Safety thermocouple invalid · **WARN, then TRIP** · graduated

Trips on: SPI transfer failure, `NaN`, `THERMO_FAULT_OPEN` / `OVUV` /
`TCRANGE` from the MAX31856's SR register, (2026-08-24) a linearized reading
outside the datasheet's per-`tc_type` plausibility band
(`max31856_tc_range_policy.c`, wired in `thermo_task.c`), or (also
2026-08-24) `max31856_tc_type_verified()` reporting that the last
`max31856_configure()` call could not confirm, via a CR1 readback, that the
part actually accepted the type it was asked to run.

The plausibility band catches config_store's commissioned `tc_type`
disagreeing with what the MAX31856's own CR1 register is actually running
(e.g. a failed `max31856_configure()` write leaving the part on a stale
type): a Type-S sensor decoded through a stuck Type-K LUT can land inside
K's own wide range and never set the part's own `TCRANGE` bit, but will
fail this config_store-anchored check the moment it disagrees with the
commissioned type by enough. Ranges are datasheet Table 1 (MAX31856.pdf
p.12), inclusive at both ends. **Updated 2026-08-24:** the band is no
longer applied unconditionally regardless of commissioning status —
`config_store`'s `tc_type` now carries a `fields_set` bit
(`CONFIG_STORE_SET_TC_TYPE`, `config_store.h`) distinguishing "operator
committed K" from "never touched, defaulted to K", which the old band could
not do. A genuinely commissioned type still gets its own exact band; a
never-commissioned board instead gets the union of all eight types' ranges
(-210..+1820 °C) as a pure garbage floor, wider than K's own -200..+1372 °C
— a deliberate widening, not a regression, since this check was never the
uncommissioned board's primary ceiling (S1's `abs_max_temp_c` gating and the
MAX31856's own `TCRANGE` bit are unaffected). See
`max31856_tc_range_policy.h`'s file header for the full argument.

The CR1 readback check is independent of the band above: it catches the
part not accepting the write at all (rather than accepting a different,
still-real type), including the specific case of a readback of `0x00` or
`0xFF`, which cannot be a byte this driver's own CR1 write ever produces for
any real type and is treated as a dead/shifted SPI bus rather than "some
other type" (`spi_owner.c`'s own baudrate-margin comment on a shifted burst
producing plausible-looking wrong numbers). The readback happens once, at
configure-time, not per sample; `thermo_task.c` checks the cached result on
every reading. Both additions feed S5's existing WARN→TRIP path; neither
changes S5's graduated response shape or timing below, and neither is a new
trip.

Deliberately **not** tripping on `TCHIGH` / `TCLOW` (those are threshold
comparators, which is S1's job) or `CJHIGH` / `CJLOW` / `CJRANGE` alone (a
cold-junction complaint means the *board* is too hot or too cold, which is worth
a WARN and is not a chamber emergency).

```
bad reads ≥ 10 consecutive AND ≥ 5 s        →  WARN, TEMP_VALID cleared
condition persists for  blind_grace_s        →  TRIP
```

Default `blind_grace_s` = **60 s**.

The graduated response is the whole point. A wiggled connector, a
thermally-induced intermittent, or a single noisy SPI transaction should cost a
warning and nothing else. But a safety processor that is *permanently* blind
must not silently preside over an unattended overnight firing while reporting
that everything is fine. Sixty seconds is long enough to ride out any plausible
intermittent and short enough that "blind for the whole firing" cannot happen.

While blind, the status frame reports **NaN** temperatures with
`SAFETY_FLAG_TEMP_VALID` clear — never 0, never the last good reading. An
explicit not-a-number is much harder to mistake for a cold kiln than a
plausible-looking stale value. (Same rule `firmware/KilnFW/docs/SAFETY_LINK.md` sets for
this field, and the ESP already parses it that way.)

### S6 — Main controller unhealthy · **TRIP**, conditionally

Two independent signals, treated independently. **They are not redundant
copies of each other and must never be collapsed into one.**

**(a) The ESP explicitly asserts fault** — `mainFault` (GPIO10) reads LOW.

```
mainFault asserted, debounced 200 ms   →  TRIP
```

Unambiguous: the main controller is telling us it has a fault and wants heat
gone. Cheap to act on, no reason to hesitate. The ESP asserts this on boot
failures and thermocouple faults (`SAFETY_FAULT_SRC_*` in
`safety_link.h:151`) unconditionally. PC-link loss used to be in that list
unconditionally too, and was the single biggest source of real-world S6a
trips — a bare board on the bench, powered with nothing plugged into the PC
link, asserted `mainFault` five seconds after boot and latched a trip for no
actual hazard. It is now gated behind `KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT`
(KilnFW `App/drivers/Kconfig`, default off), because this board is meant to
fire unattended from its own LCD/web UI and the PC's absence alone is not a
hazard. A deployment where the PC link is required equipment can still turn
that option on to get the old behavior back.

**Startup grace, 2026-09-07 (fc6d30f8 flash incident).** During a bench
reflash that resets BOTH processors together, a Pico reset that lands while
the ESP is still mid-boot can see `mainFault` read asserted for the whole
time the ESP takes to reach the point where its own firmware drives GPIO6
("Fault") to its healthy level — `HARDWARE.md` section 4 only characterizes
the "ESP absent" case (reads healthy); it says nothing about the "ESP
present but not yet initialized" case, and that gap is exactly what this
incident exposed. That window is comfortably longer than the 200 ms
consecutive-sample debounce `discrete_task.c` applies (sized for opto/switch
bounce, not an MCU boot), so the trip was real by the guard's own rules, not
a debounce failure — and it was cleared with `safety_clear_trip()` afterward,
which is exactly the kind of "just clear it" response this incident was
flagged to catch before it became routine.

The fix is qualification, not a weaker guard: `safety_guard_input_t::
s6a_startup_grace_active` (`safety_guards.h`) is true only for the first
`S6A_STARTUP_GRACE_MS` (5 s, `safety_core.c`) of **the Pico's own** boot,
computed from its free-running `to_ms_since_boot()` clock — deliberately a
different mechanism from S6b's `reboot_grace_active`/`ANNOUNCE_REBOOT`, which
only exists when the ESP deliberately announces an upcoming self-reboot (an
OTA restart) and is never sent ahead of a debug-probe-driven reset of both
boards together. Like `reboot_grace_active`, it gates only the S6a `trip()`
call, never the debounce, never re-arms mid-boot (the clock only counts up),
and grants no heating permission. Once the window closes, S6a is exactly as
unconditional as before — `test_s6a_startup_grace()`
(`test/test_safety_guards.c`) proves a mainFault still asserted the instant
the window closes trips immediately, with zero accumulated advantage from
the suppressed ticks before it.

**Clearing an S6a trip.** SaftyFW sees exactly one bit (`mainFault` LOW) and
by design cannot know why the ESP asserted it — that independence must never
be compromised by, say, having the ESP tell the safety processor "trust me,
I'm fine now." The reason lives entirely on the ESP side, as `safety_link.h`'s
`fault_sources` bitmask (`SAFETY_FAULT_SRC_MANUAL | PC_LINK | THERMO |
SAFETY_LINK | APP | THERMAL_SANITY`), which the web dashboard and the LCD's
Diagnostics → Trip Detail page now both decode into words and a suggested
remedy (`firmware/KilnFW/App/drivers/safety/safety_trip_words.h` — the one shared
table both surfaces read, so they cannot drift). 2026-08-27: the ESP now also
snapshots that mask **at the instant the trip latches**
(`safety_link_status_t.trip_fault_sources`, set in `safety_apply_trip_event()`
only on a genuinely new `trip_seq`) — distinct from the *live* mask
(`safety_link_get_fault_sources()`), because a source can assert, cause the
trip, and release again before anyone looks. Both are shown, separately
labeled, wherever a trip is reported.

Like every other guard trip in this firmware, S6a **latches**: it does not
clear on its own, an ESP reboot does not clear it (`safety_link_mark_boot_
clean()`'s one narrow exception is a *clean* boot finding nothing wrong, not
a magic auto-clear), and **starting a new firing does not clear it** — the
GUI banner says so explicitly now. The only way out is an explicit
`SAFETY_CMD_CLEAR_TRIP`, and `mainFault` is not in `guard_condition_still_
immediate()`'s list (`safety_guards.c`), so it is **unwindowed**: `safety_
guards_try_clear()` clears state and immediately re-runs `safety_guards_
tick()`, and if the GPIO10 line is still LOW on that very tick, S6a re-trips
before the clear can take effect. In practice this means: **identify the
asserted source (above), remove it, then Clear Trip** — a clear attempted
while the line is still LOW is refused. `SAFETY_FAULT_SRC_SAFETY_LINK` is the
one source an operator standing at the kiln typically cannot act on directly
(it means this board's own UART link to the safety processor is stale, not a
firing-side condition) — the GUI says that plainly rather than implying a
Clear Trip alone will fix it.

**CLEAR_TRIP has no wire-level reply.** `link_task_handle_clear_trip()`
(`src/tasks/link_task.c`) never ACKs — `LINK_PROTOCOL.md` does not ask it to
— so `link_frame_decide_clear_trip()`'s named refusal reasons
(`LINK_CLEAR_TRIP_REFUSE_NOTHING_TRIPPED` / `_MASK_MISMATCH` /
`_INEFFECTIVE`) only ever reach `log_task_log()`, i.e. SWD/serial, never the
ESP or the operator's browser/LCD. The ESP's own `/api/safety/clear_trip`
400 response ("no trip currently latched, or Pico diagnostics are stale") is
a *different*, purely local check made before sending anything — it does not
cover, and cannot know about, a still-asserted-line refusal that happens
after the frame reaches SaftyFW. The operator's only signal that a
still-asserted-source refusal happened is indirect: the next `/api/status`
poll still shows `diag_state == TRIPPED` with the live `heat_block_sources`
matching (or overlapping) `trip_fault_sources` — which is exactly what the
web banner now surfaces as "Still asserted right now." There is no
plan-of-record to add a wire ACK for CLEAR_TRIP; this document records the
gap so it is not rediscovered as a mystery later.

**(b) The link has gone quiet** — no valid frame within `link_timeout_s`.

Here is where a naive design becomes a nuisance generator, and where the doctrine
in §2 earns its keep:

```
no valid frame for  link_timeout_s (default 10 s)
  AND  any channel I > i_present_a          →  TRIP  (heat with nobody in charge)

no valid frame for  link_dead_hard_s (default 120 s)
                                            →  TRIP  (unconditional backstop)

no valid frame for  link_timeout_s, no current
                                            →  WARN, keep watching
```

A quiet link with a cold kiln is not an emergency. A quiet link with current
flowing is the definition of one. Keying on the hazard rather than on the
symptom makes this guard both quieter *and* faster than a flat timeout: it
responds in 10 s when it matters, and never at all when it does not.

The 120 s unconditional backstop exists because "no current *right now*" is a
weak statement given the 60 s heater window — the ESP could have died mid-window
with the SSR off, and come back to life it will not. Two minutes of silence
means the main controller is gone, current or no current.

> **`mainFault` cannot detect a dead ESP.** With the main board unpowered,
> U1's LED is dark, R8 pulls GPIO10 high, and it reads *healthy*
> (`HARDWARE.md` §4). Signal (b), the UART timeout, is the *only* detector of a
> dead main controller. This is why the two signals are kept separate.

**A routine ESP reboot looks identical to a dead ESP, from here.** An OTA
self-update necessarily stops the context stream for several seconds while
the board resets, re-inits Wi-Fi and reaches its first poll cycle -- Signal
(b) has no way to tell that apart from a crash on its own. `SAFETY_CMD_
ANNOUNCE_REBOOT` (0x18, **built and host-tested 2026-08-19**) closes that
gap: the ESP sends this unsolicited, no-payload frame immediately before
calling `esp_restart()`, and `link_task.c` records the local receive time
(`reboot_announce.c`). `safety_core.c` compares that timestamp against "now"
every tick and passes a plain `reboot_grace_active` bool into `safety_guards.
c` -- the guard itself does no clock arithmetic, matching every other input
in `safety_guard_input_t`.

This is a **narrow, time-boxed exception to S6b's trip condition only**:

```
in->reboot_grace_active == true
  -> the hard-backstop and soft-path trip() calls above are withheld
  -> state->s6b_link_down_elapsed_s keeps accumulating regardless
  -> in->link_up is never touched, no other guard reads this field
```

The window is 20s (`REBOOT_GRACE_WINDOW_MS`, `safety_core.c`) -- a software
timeout with generous margin over a plausible ESP32-S3 boot-to-first-
PUSH_CONTEXT time, in the same "reasonable timeout, not a measured physical
constant" category as `SAFETY_LINK_STALE_MS`/`SAFETY_LINK_FIRING_ABORT_
SILENCE_MS` (`firmware/KilnFW/App/drivers/safety/safety_link.h`), *not* the same
category as S8's rate-of-rise threshold, which this document explicitly
forbids guessing. 20s sits comfortably below `link_dead_hard_s`'s 120s
unconditional backstop, so a reboot that genuinely fails to come back is
still caught shortly after the window closes.

**The one property that must never be compromised: this grants no
permission to heat.** The elapsed-silence accumulator is never reset by the
grace window -- only the two `trip()` calls are gated -- so if the ESP is
still silent once the window expires, S6b trips on the very next tick
exactly as if `ANNOUNCE_REBOOT` had never arrived; there is no second grace
period hiding behind the first. `relay_owner`'s energize/ARM logic has no
path to `reboot_grace_active` at all (`safety_guards.c` has no link/flash/
GPIO access, by this module's own design), and no other guard reads it --
`test_s6b_reboot_grace()` (`test/test_safety_guards.c`) covers the
suppression, the no-accumulated-advantage-on-expiry property, S1 firing
unaffected, and `link_up == true` making the field a provable no-op.

### S7 — E-stop · **TRIP**

```
GPIO9 HIGH (contact open), debounced 50 ms   →  TRIP
```

Fastest guard in the set, minimal debounce, no conditions. Requires
normally-closed wiring so that a pressed button, a cut cable and an
unterminated input all read identically as *stop* — see `HARDWARE.md` §5, which
also explains why this must not be "fixed" by inverting it in firmware.

### S8 — Implausible rate of rise · **TRIP** · **ships disabled**

```
d(safety_tc_c)/dt > max_rate_c_per_min   sustained for  rate_window_s
```

Defaults: `max_rate_c_per_min` = **0 (disabled)**, `rate_window_s` = 60 s.

Intended to catch a full-power runaway before it reaches S1's absolute limit —
the difference between stopping at 900 °C and stopping at 1300 °C is the
difference between a recoverable event and replacing the elements and furniture.

**It ships off** because the correct threshold depends on the kiln's mass,
element power and insulation, and nobody has ever measured this kiln's
maximum legitimate ramp rate. A small test kiln on full power can genuinely
exceed 15 °C/min; a large one struggles past 5. Hard-coding a plausible number
and calling the guard done is the mistake `thermal_guard.h` explicitly refuses
to make for its guard 8, and the same discipline applies here — `0.0f` here
is not a "substitute a default" zero, it is the same "no default exists"
convention `abs_max_temp_c` uses for S1, and `config_store_default()` ships it
that way.

Enable it after a full-power ramp has been logged and the real maximum rate is
known — set the threshold at roughly 2 × that, and the guard becomes genuinely
useful. `TODO.md` phase 6 tracks this.

**Built and host-tested, 2026-09-03.** `safety_guards.c` now implements the
pure guard: rather than an instantaneous per-tick derivative — which at this
module's ~100 ms tick would amplify a fraction of a degree of ordinary
MAX31856 read noise into a triple-digit apparent °C/min, exactly the
nuisance-trip generator §2 forbids — it measures the **average rate over one
whole `rate_window_s` window** using a baseline-sample-and-hold (remember the
reading at the window's start, compare against the reading `rate_window_s`
later, then slide to a fresh window). A single glitchy sample landing on a
window boundary can inflate one window's average, so the trip condition
additionally requires **two consecutive over-threshold window evaluations**
before latching — the same "magnitude and duration, one alone is never
enough" doctrine applied across windows instead of within one: a real
runaway keeps climbing every window and clears both bars with margin, while a
glitch that reverts by the next sample becomes the *starting* value of the
following window and measures back down, never compounding into a second
consecutive over-threshold window. An invalid/stale/NaN reading cannot poison
either endpoint: S5's bad-read check already returns before this guard's
block runs on any tick that is not `tc_valid`, so the window is left
**paused**, not corrupted, across a bad-read burst, and resumes from its
original baseline once the sensor recovers.

**Honest scope limit on the "legitimate full-power ramp" margin:** this repo
has no logged full-power ramp on any real kiln to read a number from — this
section's own preceding paragraph says so plainly. The host test suite
therefore proves the margin against the number *this document itself*
already commits to (a small kiln's stated worst case, "can genuinely exceed
15 °C/min") rather than a measured one: `max_rate_c_per_min` commissioned at
2× that (30 °C/min) does not trip on a 30-minute ramp held exactly at
15 °C/min. That is a proof of the *design's* margin, not a substitute for
the bench measurement this section still calls for before the threshold is
ever actually commissioned on a real board.

**Integrated, 2026-09-03.** `safety_core.c`'s `safety_core_load_guard_cfg()` —
the function that copies commissioned `config_store` fields into
`s_guard_cfg`, the same one a 2026-08-27 audit found missing for S1 — now
copies `max_rate_c_per_min` (0x0204, fields_set-gated, same "0 = never
commissioned" idiom as `abs_max_temp_c`) and `rate_window_s` (0x0205,
ordinary "0 → documented default" field) into the guard config it hands the
pure module. `test_safety_core_s8_wiring.c` proves the whole chain: a value
staged through `config_store`'s real `SET_PARAM` path reaches
`safety_guards_tick()` and changes its verdict, and the same test proves the
converse — an uncommissioned board (config_store's shipped default, the bit
clear) runs the identical implausible ramp and stays silent. **Still ships
inert by default**: nothing about this wiring pass changes
`config_store_default()`'s `max_rate_c_per_min = 0.0f`, so S8 remains off on
every board until an operator actually commissions a threshold from a
measured full-power ramp, per this section's own guidance above.

### S9 — Trip ineffective / contactor welded · **TRIP-ESCALATE** · loudest thing here

After a trip, the current must go away. If it does not, the trip did not work.

```
tripped (K4 de-energized) for  trip_verify_s
  AND  any channel  I > i_present_a
        →  latch TRIP_INEFFECTIVE, and say so as loudly as the system can
```

Default `trip_verify_s` = **10 s** (comfortably past the 1 s peak-hold decay and
any contactor drop-out delay).

**This is the single most valuable guard after S1, and it costs nothing to
build** — the sensors are already there. `SAFETY_MODEL.md` §7 lists "welded line
contactor" as an unclosable gap, because no firmware on this board can open a
contactor whose contacts have fused. That is true. But there is an enormous
difference between *unclosable* and *undetected*:

- Undetected, the operator sees a kiln that "stopped safely" and walks away
  from a chamber that is still heating at full power with no controller.
- Detected, the operator gets an unambiguous "**POWER IS STILL FLOWING — REMOVE
  IT AT THE BREAKER**" and a system that will not stop shouting about it.

So S9 does not prevent the failure. It converts a silent, lethal one into a
loud one, which is the whole of what is achievable here. Every channel it has —
the diagnostic frame, the trip event frame, the log — carries a distinct
`SAFETY_TRIP_INEFFECTIVE` reason, and the ESP should escalate it in the GUI
differently from every other trip, because the required operator action is
different: this one means *go to the breaker*, not *investigate the kiln*.

It also catches the much more mundane version: **K4 or Q4 failed, or the
interlock was wired to the wrong J10 contact.** That last one passes every bench
test and fails dangerous, and S9 is what finds it on the first real trip.

### S10 — Safety TC disagrees with every zone TC · **WARN** · *needs context* · **`CHAMBER_AGREED` only**

```
| safety_tc_c − nearest valid zone measured_c |  >  tc_disagreement_c
  continuously for  tc_disagreement_time_s
```

Defaults: `tc_disagreement_c` = **200 °C**, `tc_disagreement_time_s` = **300 s**,
and it **ships as WARN**.

**This guard is entirely off in `EXTERNAL_OVERHEAT`, and that is the normal
case unless the installation says otherwise.** A safety thermocouple mounted on
the kiln shell, in the exhaust, in the element chamber, or simply in a part of
the chamber the zone sensors do not represent has *no obligation whatsoever* to
agree with them. A 600 °C standing disagreement there is the correct reading,
not a fault, and a guard that fires on it is precisely the nuisance generator §2
exists to prevent.

Where it *is* declared `CHAMBER_AGREED`, this becomes the only cross-check a
single-sensor safety processor can have, and it is free — the zone temperatures
are already on the wire for S2. Comparing against the **nearest** valid zone
reading, not the mean, is deliberate even then: kilns stratify by well over
100 °C top to bottom. Only a disagreement with *every* zone means a sensor has
left the building.

What it catches that nothing else does: a chamber-mounted safety thermocouple
that has fallen out, been installed in the wrong port, or drifted badly with
age. All three leave S1 reading a plausible, comfortable number forever while
the kiln does whatever it likes.

**WARN, never TRIP by default**, because the honest answer to "which sensor is
wrong?" is unknowable from here. Promote it to TRIP only after a real firing's
stratification has been logged and `tc_disagreement_c` set from measured data
rather than from this paragraph.

**Related commissioning option worth offering in the GUI**: in `CHAMBER_AGREED`,
a one-shot "capture expected offset" at a soak — record the steady-state
difference between the safety TC and its nearest zone, and compare against
*that* rather than against zero. A consistent 120 °C offset from mounting
position is then normal, and a 200 °C *change* in it is the signal. Strictly
better than a raw difference, and cheap.

### S11 — Frozen safety reading · **TRIP**

Applies to **whichever source is active** — the Pico's own MAX31856, or a
borrowed zone channel, or both in `BOTH` mode.

```
active safety reading identical (to full resolution) for  frozen_window_s
  AND  current flowing or heat commanded during that window
```

Default `frozen_window_s` = **600 s**, matching `thermal_guard.c`'s
`FROZEN_WINDOW_S` for the same failure on the main board.

A stuck reading is the failure mode that quietly disables S1, S2 and S8 all at
once — a frozen 400 °C never crosses any ceiling, so a blind spot masquerades as
a healthy kiln. The MAX31856 reports to 19 bits; a genuinely static value at
that resolution, for ten minutes, while energy is going in, does not happen in a
real thermal system.

**The "and heat is happening" qualifier is what keeps this from being a nuisance
guard.** A cold, idle kiln legitimately sits at a constant reading for hours.

On a **borrowed** channel this guard is necessary but not sufficient: it catches
a frozen *value*, and S13 catches a channel that has stopped producing values at
all. The two failures look identical from here and are distinguished only by the
context frame's `sample_counter`.

### S12 — Cold junction / enclosure over-temperature · **WARN, then TRIP**

```
cj_c > cj_warn_c    →  WARN
cj_c > cj_max_c     sustained for  cj_time_s   →  TRIP
```

Defaults: `cj_warn_c` = **60 °C**, `cj_max_c` = **85 °C**, `cj_time_s` = **60 s**.

The MAX31856's cold-junction sensor measures the temperature at the terminal
block — which is to say, **inside the electronics enclosure**. It is already
read on every conversion, so this guard is free, and it covers two real
problems at once:

- **A cooking enclosure is a fire-adjacent condition in its own right.** A
  blocked vent, a failed fan, or an enclosure mounted somewhere it should not be
  will get there long before anything else notices.
- **Cold-junction compensation is only as good as the CJ reading.** Above the
  part's specified range every thermocouple reading on this board is wrong, in
  an unknown direction. A safety processor whose sensor has silently gone out of
  spec is worse than one that admits it.

`THERMO_FAULT_CJRANGE` from the part is treated as an immediate WARN by S5, and
S12 is the graduated numeric version that acts before the part gives up entirely.

### S13 — Borrowed channel not updating · **WARN, then TRIP** · **`BORROWED_ZONE`/`BOTH` only**

The guard that makes a borrowed thermocouple usable at all.

```
context frames arriving  AND  zone sample_counter has not advanced
  for  borrowed_stale_s                    →  WARN, reading treated as invalid
  for  borrowed_stale_trip_s               →  TRIP
```

Defaults: `borrowed_stale_s` = **10 s**, `borrowed_stale_trip_s` = **60 s**.

`sample_counter` is a per-zone byte in the context frame that the ESP increments
**only when it actually consumes a fresh conversion** from that channel — never
merely because it built a frame (`firmware/CommonFW/docs/LINK_PROTOCOL.md` §4).

Without it, the failure is undetectable. A MAX31856 on the main board that stops
converting keeps returning its last value; the ESP forwards it faithfully every
500 ms; and from the Pico's side **that is indistinguishable from a kiln holding
a steady soak** — which is precisely when it is holding still for hours and
precisely when being blind is most dangerous.

Note the division of labour, which is why both S11 and S13 exist:

| `sample_counter` | value | Verdict |
|---|---|---|
| advancing | changing | healthy |
| advancing | frozen, heat on | **S11** — the sensor is stuck |
| not advancing | anything | **S13** — the channel stopped converting |
| frames not arriving at all | — | **S6** — the link, not the sensor |

Three different faults, three different fixes, and collapsing them into one
"temperature is stale" condition would lose the diagnosis every time.

**A borrowed reading whose `sample_counter` is stale is treated as invalid
immediately** (feeding S5), not merely warned about — it is not a slightly-old
number, it is a number of unknown age.

### S14 — Zone current above its measured normal · **WARN only** · *needs context* · NEW, 2026-08-28

The owner asked for "an over current guard to go with the under current guard,
use a percentage of the normal to set this." This is that guard, and it does
**not** contradict §2/§3/§7's over-current position below — see "Scope" at the
end of this section for why.

```
for each CT channel ch in 0..2:
  i_normal_a[ch] is measured (i_normal_a fields_set bit for ch is set)
    AND that channel's mapped relay is commanded on right now
    AND amps[ch] > i_normal_a[ch] * overcurrent_pct / 100
      continuously for overcurrent_time_s               →  WARN
```

Defaults: `overcurrent_pct` = **150 %**, `overcurrent_time_s` = **30 s**.

**Per-channel, and never guessed.** `i_normal_a[ch]` is the current recorded
while that channel's zone was the only one energized (the same per-zone
measurement `CURRENT_SENSE.md` §5 step 2 already requires, ROADMAP M12). A
channel whose normal has never been measured is **skipped entirely** — no
accumulation, no WARN, reported inactive, exactly like a context-dependent
guard with no context. `0` is never substituted for a missing normal: this is
a magnitude comparison against a real measurement, or it does not run at all.

**WARN, never TRIP**, for three reasons:

1. §2's rule: the default for a new guard is WARN. Promoting one to TRIP needs
   a written argument about what physical harm it prevents, and none is made
   here.
2. It does not change §3's or §7's position (below): breakers, sized for full
   load at 100 % duty, remain the over-current protection. S14's job is
   different — catching a CT on the wrong jack, or an element/wiring change
   that shifted a zone's draw — which is a *commissioning-integrity* check, not
   an electrical-protection one.
3. The measurement chain (a 12-bit ADC behind a 1 s peak-hold, calibrated by a
   self-service zones-page button) is not yet evidence anyone should open a
   contactor on.

**Scope, precisely:** this guard compares a channel's current against *its own
measured normal*, as a percentage, never against an absolute amp figure and
never against another channel — three zones on one kiln can legitimately
differ 2× in element draw, so a shared absolute threshold would be either
useless or a nuisance generator. It exists alongside S3/S4 (presence/absence)
without changing their scope statement below: S3/S4 still never look at
magnitude, and S14 still never protects against a short or a genuine
over-current fault — see §3/§7.

### Runtime configuration integrity · continuous

Not a guard, a background check: the in-RAM threshold/calibration set is
re-CRC'd against its flash copy every 10 s. A mismatch means RAM corruption,
which on a safety processor is not something to discover during a trip
evaluation — reload from flash, report `calibration_missing`, and if it
recurs, trip.

This is cheap paranoia and it is warranted here specifically because a
corrupted `abs_max_temp_c` fails silent: a threshold that has quietly become
`0x7FFFFFFF` never trips, and nothing else in the system would ever notice.

---

## 5. Context from the main controller

The ESP pushes setpoints, measured temperatures and relay state over the
isolated link (`LINK_PROTOCOL.md` §4). Guards S2, S3 and S4 consume it.

The rules governing it are short and absolute:

1. **Context is never primary evidence.** No guard trips *because of* something
   the ESP said. S1, S5, S5, S6, S7, S8 do not read it at all.
2. **Stale context is no context.** Older than `context_max_age_s`
   (default **5 s**, i.e. 10 poll periods) and the context-consuming guards
   go inactive, not pessimistic.
3. **An ESP restart invalidates it.** The context frame carries a boot counter;
   when it changes, all correlation windows reset. A relay history from before
   a reboot describes a different program's intentions.
4. **The ESP is not trusted to be correct, only to be honest about what it
   commanded.** The Pico uses "which relays did you turn on" — a statement of
   fact about the ESP's own outputs — and never "is this safe", which would be
   asking the component under suspicion to grade itself.

---

## 6. Trip semantics

**On trip**, in this order:
1. `relay_owner` de-energizes K4 — first, before anything else, before logging.
2. The trip reason and a snapshot of the deciding inputs are latched.
3. The status frame's `SAFETY_FLAG_ENABLED` clears and `SAFETY_FLAG_RELAY`
   goes low; the diagnostic frame carries the reason.
4. It is logged.

**A trip latches.** `SAFETY_CMD_REQUEST_ENABLE` from the ESP is refused while
latched — it is advisory and the Pico's interlocks always win, which
`firmware/KilnFW/docs/SAFETY_LINK.md` already documents and the ESP already handles.

**Clearing requires a deliberate operator act**: `SAFETY_CMD_CLEAR_TRIP` (0x0A)
over the link. **Correction, 2026-08-27 audit:** an earlier version of this
section additionally claimed "an E-stop assert-then-release cycle (a physical
action, at the machine, by someone who has looked at the kiln)" as a second,
independent clear path. No such path exists in code — `discrete_task.c`
publishes `estop_pressed` as a plain debounced level
(`discrete_task_estop_pressed()`), nothing anywhere tracks its previous value,
and `safety_core.c` has no code path that clears a trip on its own initiative;
the only way a trip's `is_tripped` ever goes back to false is through
`safety_guards_try_clear()`, and the only caller of that function is
`link_task.c`'s handling of an incoming `CLEAR_TRIP` frame
(`link_task.c:1304`). Releasing the E-stop button changes nothing about
`is_tripped` by itself.

What releasing the E-stop *does* do, for an S7 trip specifically: it is a
**precondition** the operator must satisfy before `CLEAR_TRIP` will succeed,
not a substitute for sending it. `safety_guards_try_clear()` resets the guard
state and re-evaluates one tick against the current input
(`safety_guards.c:242-243`); `safety_guards_tick()` checks the live
`in->estop_pressed` level unconditionally near the top of every tick
(`safety_guards.c:461-464`, "the fastest guard in the set", checked before
any windowed guard). If the button is still pressed, that recheck re-trips
within the same call and the clear is refused; only once the button has
actually been released does the recheck pass. So in practice an operator
does still have to walk to the machine, look at it, and release the E-stop —
but they *also* have to send `CLEAR_TRIP` afterward. Silence after release
leaves the trip latched forever, matching §2's "latching is not
auto-recovery": there is no timer, no edge watcher, and no code that decides
on its own that a released button means it is safe to re-arm K4.

A clear is refused while the tripping condition is still true — otherwise
"clear" becomes a way to spam past a real fault.

**What an operator should actually do to recover from an E-stop trip:**
release the physical E-stop, confirm the kiln is actually safe, then issue
`SAFETY_CMD_CLEAR_TRIP` (0x0A) from the ESP UI / `kiln_call`. Both steps are
required; neither alone clears the trip.

`CLEAR_TRIP` is code-complete on both sides of the check as of 2026-08-19:
`link_task.c` decodes the frame, refuses locally (never calling into
`safety_core`) if nothing is latched or if the wire `trip_mask` doesn't match
the currently-latched reason, and otherwise calls
`safety_core_request_clear_trip()`, which wraps the pure `safety_guards_try_clear()`
below. Host-tested only — **the Pi↔ESP link is bench-confirmed dead this
session**, and a separate PC↔ESP UART fault was found this session too, so the
frame has never crossed real wire.

**`safety_guards_try_clear()` has a documented, intentional scope limit.** It
resets guard state and immediately re-evaluates one tick against fresh input,
refusing the clear only if that single retick re-trips. That catches
**unwindowed** guards reliably (S7 E-stop, S6a `mainFault`, S6b's hard
backstop) — if the condition is still true, the very next tick trips again.
It does **not** reliably catch a still-present condition on a **windowed /
graduated** guard (S1, S2, S3, S5, S9, S11, S12, S13): the clear call resets
the same elapsed-time accumulator the guard needs to re-arm, so one retick
sees an empty window and reports healthy. The condition is not missed
forever — the guard re-trips on its own normal timescale once the window
rebuilds — but the clear is not an *instant* refusal for those guards the way
it is for the unwindowed ones. This is a stated design trade-off, not a gap
left silent: rejecting every clear until a full window re-accumulates would
make `CLEAR_TRIP` effectively unusable for any graduated guard.

**Power-on state is de-energized.** K4 is off before `main()` runs, on every
reset, watchdog or otherwise. Heating is permitted only after every task has
reported healthy, the startup grace has elapsed, and no guard is tripped.

---

## 6a. Version disagreement between the two processors

The protocol version check is **mutual**: the ESP verifies the safety
processor's, the safety processor verifies the ESP's, and neither trusts the
other's data until both agree
([`../../CommonFW/docs/LINK_PROTOCOL.md`](../../CommonFW/docs/LINK_PROTOCOL.md),
`ANNOUNCE_VERSION`). One-directional checking would leave this processor parsing
setpoints, the relay mask and a borrowed thermocouple reading out of a frame
whose format it had never confirmed — and it is the side that must not guess.

**On a mismatch this processor does not trip.** It sets `DEGRADED_NO_CONTEXT`
(`ARCHITECTURE.md`) and carries on with the guards that need no context. The
reasoning is the doctrine in §2 applied honestly:

- A version mismatch is evidence that two builds were flashed out of step. It is
  not evidence of a hot kiln. It fails the *magnitude* bar outright.
- Tripping would also be redundant: the ESP treats the same mismatch as a link
  fault and refuses every heater-on. The kiln is already prevented from heating
  by the processor that commands the heat.
- During development, mismatches will be frequent. A guard that fires on every
  mismatched pair of builds is a guard people learn to work around, and that
  habit is the actual hazard.

What does **not** relax: S1 absolute over-temp, S5 sensor invalid, S7 E-stop,
S11 frozen reading and S12 cold junction all keep running and keep authority
over K4. Those are the guards that matter when the main controller is an unknown
quantity, and they need nothing from it.

The context-dependent guards — S2, S3, S4, S10, S13, S14 — report as **disabled**,
never as passing. A guard that cannot evaluate must not look like a guard that
evaluated and found nothing wrong; that distinction is the difference between a
safety case and a green light.

## 6b. Update mode

Field updates over the isolated link are planned
([`../../CommonFW/docs/UPDATE_PROTOCOL.md`](../../CommonFW/docs/UPDATE_PROTOCOL.md),
[`BOOTLOADER.md`](BOOTLOADER.md)). They matter here because **while its flash is
being rewritten the safety processor is not watching anything.**

Three rules follow, and none of them are negotiable:

1. **The Pico enforces its own preconditions.** Relay open, no trip pending,
   every reading below a configured ceiling (default 100 °C). It does not take
   the ESP's word for any of them, for the same reason this processor exists at
   all — the ESP is the thing that might be wrong.
2. **GPIO6 stays low for the entire update**, including through the bootloader
   and recovery mode. A block erase stops both cores for hundreds of
   milliseconds at a time, during which no guard is running; the relay being
   already open is what makes that survivable.
3. **The link going quiet during an update must still block heating on the ESP.**
   `SAFETY_FAULT_SRC_SAFETY_LINK` asserting at 1.5 s is the correct behaviour,
   not a bug to be papered over. The GUI may say "updating" instead of "not
   responding"; `relay_authority_on_blocked()` must not learn the difference.

An update is also the one moment when the safety processor's own code changes,
so the rollback bar is deliberately higher than "it booted": configuration CRC
verified, a plausible thermocouple reading, ADC sampling, every task checked in,
and one acknowledged telemetry frame. An image that boots but cannot read its
thermocouple is worse than the one it replaced.

## 7. What this does NOT protect against

Stated plainly, because a safety case that only lists successes is not a safety
case.

| Failure | Covered? |
|---|---|
| Welded/shorted SSR | **Yes** — S3, and K4 is upstream in a different technology |
| Runaway with the main controller crashed | **Yes** — S1, S6(b) |
| Main controller commanding nonsense | **Yes** — S1, S2 |
| Main controller absent, unprogrammed, or dead at boot | **Yes** — the ESP refuses to heat without Pico telemetry, and the Pico refuses to arm without context |
| Safety TC fallen out of the chamber / wrong port / drifted | **Only in `CHAMBER_AGREED`** — S10 detects it (WARN); S8 and S11 catch some cases in both modes. In `EXTERNAL_OVERHEAT` there is no cross-check at all |
| Element short / over-current | **No, by design.** Fuses and breakers own this — see §3. S14 (§4, added 2026-08-28) WARNs on a channel drawing well above its own measured normal, but that is a commissioning-integrity check (wrong CT jack, changed element), not electrical protection, and it never trips |
| Safety TC frozen at a plausible value | **Yes** — S11 |
| Enclosure overheating / CJ out of spec | **Yes** — S12 |
| Corrupted safety threshold in RAM | **Yes** — periodic CRC check |
| **Welded line contactor** | **Not preventable — but now loudly detected.** S9 escalates to `TRIP_INEFFECTIVE`; the current keeps flowing until someone opens the breaker |
| **Safety TC and zone TCs all wrong the same way** | **No.** One safety sensor; S10 only catches *disagreement*, not common-mode error |
| **Pico hardware failure** | **Partly.** Watchdog + fail-safe relay polarity cover hang and reset; a shorted Q4 or welded K4 is caught by S9 only after a trip is attempted |
| **Fire from a non-electrical cause** | **No.** Not a fire detection system |
| Loss of `12v_Safty` | **Yes, inherently** — K4 de-energizes, contactor opens |

Two rows still say **No**, and they are the honest limits of a single-channel
safety processor with one sensor:

- **Common-mode sensor error.** If the safety TC and the zone TCs are all wrong
  in the same direction, nothing here notices. Only a genuinely independent
  second sensor fixes that — and in `EXTERNAL_OVERHEAT` mode, where S10 is off,
  there is no sensor cross-check of any kind. That is an acceptable trade when
  the external sensor is measuring a genuinely independent physical limit (a
  shell or exhaust temperature that *cannot* be wrong in the same way a chamber
  TC is), and a poor one if it is just a chamber TC declared external to silence
  S10. Choose the mode for the physics, not for the quiet.
- **A welded contactor.** S9 turns this from silent to loud, which is the whole
  of what firmware can achieve. Actually *clearing* it needs a second series
  contactor, or a mirror contact plus an operator who acts on the alarm.

Both want board changes — a **second independent safety thermocouple** and a
**contactor mirror/feedback contact** — and both are worth having before this
system is trusted to run unattended overnight. They are tracked in `TODO.md`
phase 8 rather than quietly omitted.

One further wiring suggestion, free and worth taking: **put the kiln's lid/door
switch in series with the E-stop's normally-closed loop.** There is no spare
isolated input for a lid switch, but S7 already treats an open loop as a stop,
so a series lid switch gets door interlocking for the price of a wire.


---

## Completion checklist

Tick **built** and **verified on hardware** separately — they are not the same
claim, and `firmware/KilnFW/docs/PROJECT_STATUS.md` is the model for keeping them apart.
Provocation methods are in [`GUARD_TEST_MATRIX.md`](GUARD_TEST_MATRIX.md).

### Guards

> **"Built" and "host-tested" describe the pure module in `safety_guards.c`.
> They do NOT promise the integration hands the guard a threshold it can act
> on.** An audit on 2026-08-27 found that distinction was load-bearing and
> undocumented: `s_guard_cfg` in `safety_core.c` was never populated from
> `config_store`, so **S1 could not fire at any temperature on any board**,
> commissioned or not, while this table showed it built and host-tested. The
> pure function was correct and well tested the whole time; the value never
> arrived. Fixed by `safety_core_load_guard_cfg()`, and the "Integrated"
> column below now tracks that question separately. When adding a guard, tick
> Integrated only after confirming a commissioned value actually reaches it
> on target — a host test cannot see this class of defect.

| | Guard | Class | Built | Host-tested | Integrated | Hardware-verified |
|---|---|---|---|---|---|---|
| S1 | Absolute over-temperature | TRIP | [x] | [x] | [x] *(fixed 2026-08-27; was NOT integrated — see note above)* | [ ] |
| S2 | Sustained excess over setpoint | TRIP | [x] | [x] | [x] *(gated on `tc_placement_valid` since 2026-08-27; previously armed on uncommissioned boards)* | [ ] |
| S3 | Load active, no heat commanded | TRIP | [x] | [x] | [x] | [ ] |
| S4 | Heat commanded, load inactive | WARN | [x] | [x] | [x] | [ ] |
| S5 | Safety thermocouple invalid | WARN→TRIP | [x] | [x] | [x] | [ ] |
| S6 | Main controller unhealthy | TRIP | [x] | [x] | [x] | [ ] |
| S7 | E-stop | TRIP | [x] | [x] | [x] | [ ] |
| S8 | Implausible rate of rise | TRIP, off by default | [x] *(2026-09-03; two-window average-rate design, see S8's own section above)* | [x] | [x] *(2026-09-03; `safety_core_load_guard_cfg()` now copies `max_rate_c_per_min`/`rate_window_s` from config_store -- `test_safety_core_s8_wiring.c` proves the whole chain and the still-inert default, see S8's own section above)* | [ ] |
| S9 | Trip ineffective / contactor welded | ESCALATE | [x] | [x] | [x] | [ ] |
| S10 | Safety TC vs zone TC disagreement | WARN | [x] | [x] | [x] *(same `tc_placement_valid` gate as S2)* | [ ] |
| S11 | Frozen safety reading | TRIP | [x] | [x] | [x] | [ ] |
| S12 | Cold junction / enclosure over-temp | WARN→TRIP | [x] | [x] | [x] | [ ] |
| S13 | Borrowed channel not updating | WARN→TRIP | [x] | [x] | [x] `sample_counter_advancing` now has a real producer (`context_borrowed_sample_counter_advancing()`, `src/snapshots.h`, called from `safety_core_build_input()`) -- `tc_source` may be commissioned to BORROWED_ZONE/BOTH once `borrowed_zone_index` is also set | [ ] |
| — | Runtime config integrity | TRIP | [ ] | [ ] | [ ] | [ ] |

S2/S3/S4/S6/S9/S10/S13 built and host-tested 2026-08-18: `src/safety_guards.c`
now implements 12 of 13 guards (everything but S8, which ships disabled by
design per its own section above -- no threshold to build until a real
kiln's ramp rate is measured). The context/current-sense facts these seven
need (relay/setpoint/zone data, current presence, link liveness, mainFault,
borrowed-channel sample counter) are flattened into
`safety_guard_input_t` as plain scalars with their own validity flags
(`context_valid`, etc.) rather than by including `link_task`'s or
`current_task`'s real snapshot types -- `link_task`/`current_task` don't
publish real producers yet (Phase 6/7), and this keeps the module exactly as
link-header-free as the original five-guard version (ARCHITECTURE.md
section 2's "the one rule that matters"). `test/test_safety_guards.c` adds
48 new checks (320/320 total), nuisance cases first per this file's own
doctrine, for all seven. **Not hardware-verified** -- no RP2040/MAX31856
attached to the build machine, so nothing above claims more than "host-tested
against synthetic inputs." Runtime config integrity is out of scope here --
it is a `config_store` (Phase 9) concern, not a per-tick guard.

**S11/S13/S6 borrowed-staleness split audited 2026-08-18** (ROADMAP.md M5):
confirmed already correct, no code change needed. `safety_guards_tick()`'s
S6b block reads only `in->link_up`; its S13 block reads only
`in->context_valid` and `in->sample_counter_advancing`; S11 reads neither --
three disjoint facts with no cross-reads between the blocks, exactly matching
this section's own S13 table (`sample_counter` not advancing -> S13; frames
not arriving at all -> S6; frozen value while advancing -> S11). The split's
correctness ultimately rests on the caller (`safety_core`, not yet built)
collapsing `context_valid` to false the moment frames stop arriving, per
section 5 rule 2 ("stale context is no context") -- this module's own
boundary is now independence-tested directly: `test_s6_s13_split()` in
`test/test_safety_guards.c` covers link-dead-not-channel-stale,
channel-stale-not-link-dead, and both-facts-on-one-tick (verifying S6's
earlier check order wins and the trip latches so S13 cannot re-attribute it
on a later tick). 378/378 host checks pass.

### Policy

- [ ] Every trip clears **both** bars: magnitude *and* duration
- [ ] WARN is the default class; each TRIP has a written argument here
- [ ] Trips latch; no condition-cleared auto-reset anywhere
- [ ] `CLEAR_TRIP` refused while the condition holds, and on a `trip_mask` mismatch
- [ ] E-stop release is honored as a *precondition* for `CLEAR_TRIP` to
      succeed on an S7 trip (release alone does not clear it -- see section 6's
      2026-08-27 correction)
- [ ] GRACE state evaluates and reports but never energizes K4
- [ ] Context-consuming guards go **inactive** on stale context, never pessimistic
- [ ] `boot_id` change resets every correlation window
- [ ] `SIM_PLANT` flag disables S2/S3/S4 and warns persistently
- [ ] Guards with no defensible default ship **disabled**, and say so in telemetry
- [ ] `tc_placement_mode` and `tc_source` required at commissioning, no defaults

### Honest-gaps register (§7)

- [ ] Summary table re-checked against the code, with per-row verification state
- [ ] Common-mode sensor error still recorded as **uncovered**
- [ ] Welded contactor still recorded as **detected, not preventable**
- [ ] Second independent thermocouple proposed for the next board revision
- [ ] Contactor mirror/feedback contact proposed for the next board revision
- [ ] Lid/door switch in series with the E-stop loop suggested in the build docs
