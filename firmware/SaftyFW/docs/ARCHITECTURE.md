# Firmware Architecture

> **Status:** planning · **Last reviewed:** 2026-08-16
> **Keep this file current.** Task priorities, core affinity and the boot order
> are safety properties, not implementation detail — if they change, this file
> changes in the same commit. If it disagrees with the code, **the code wins.**
> Checklist at the bottom.

FreeRTOS on the RP2040, structured the way `KilnFW` structures the ESP32-S3:
**every hardware interface is owned by exactly one task**, and everything else
talks to that owner rather than to the peripheral.

---

## 1. Toolchain

| Choice | Value | Why |
|---|---|---|
| SDK | `pico-sdk` (2.x) | The RP2040 baseline. Vendored as a submodule or via `PICO_SDK_PATH` |
| RTOS | `FreeRTOS-Kernel`, **SMP port** | Both cores usable, and core affinity is a real isolation tool — see §4 |
| Build | CMake + `arm-none-eabi-gcc` | pico-sdk's native flow |
| Flash/debug | **SWD via OpenOCD + a debug probe** | Not USB. See `HARDWARE.md` §7 — USB contends with the back-fed 3V3 rail. Matches this project's existing OpenOCD workflow |
| Warnings | `-Wall -Wextra -Werror` | Same bar `KilnFW` holds |
| Host tests | MSVC, no SDK | Same pattern as `firmware/KilnFW/App/test/` |

### Logging has three transports, in this order

| Path | Needs | When |
|---|---|---|
| **`kilnlink` LOG frames, task id 5, relayed by the ESP** | nothing extra | **Primary.** Works in the deployed system; the ESP already forwards every `LOG` frame to the PC |
| **RTT over SWD** | the debug probe | **Secondary** — and the only one that works when the link is down or the Pico will not talk, which is when a log matters most |
| **USB CDC (`stdio_usb`)** | a USB cable | **Bench only.** `HARDWARE.md` §7: A1's 3V3 is back-fed, so USB puts the module regulator in contention with IC3. Usable only with `12v_Safty` removed |

#### Why `stdio_usb` is not the default, on firmware grounds

Independent of any board-level power or grounding question, USB costs more than
it returns here:

- **It does not replace the probe.** SWD is needed for flashing and debug
  regardless, and the Debug Probe already carries a UART bridge on the same
  cable (`HARDWARE.md` §7b). USB is a second cable for a console you have.
- **It links TinyUSB into the safety processor** — a device stack, descriptors,
  a CDC class driver and an IRQ that must be serviced on schedule. That is a
  large amount of code inside the one component whose design argument is that
  every line in it can fail with nothing to catch it (`SAFETY_MODEL.md` §1).
- **`stdio_usb` blocks by default** when the host is not reading. A blocking
  `printf` in this firmware directly violates no-hang rule 3. It can be made
  droppable, but that is one more thing to get right and keep right.
- **It is absent exactly when it is wanted.** USB CDC exists only once the
  firmware is running and the stack is up — so a crash before USB init, an early
  hang, or a watchdog loop produces no console at all. RTT works earlier; SWD
  works with the CPU halted.
- **It drops on every reset**, re-enumerating and disconnecting terminals —
  precisely the event worth observing on a processor that may watchdog-reset.
- **It complicates core affinity.** USB becomes another task to pin and
  prioritise inside a design whose main property is decoupling (§4).

So: `SAFTYFW_ENABLE_USB_STDIO`, **compile-time, default off, debug builds
only.** Note this is a build flag and *cannot* be a runtime GUI toggle —
TinyUSB is either linked in or it is not.

#### Isolated-path log emission: runtime, GUI-configurable, default quiet

This one *is* runtime-configurable, and should be exposed per-peer in the GUI
(`tools/PcTools/TODO.md`, "Logging and consoles"). Default: **errors and warnings
only**, with verbose levels enabled on demand.

The reason to default it quiet is not bandwidth. At 115200 baud the link moves
~11.5 KB/s and telemetry uses roughly 5 % of it, so there is ample room. The
reason is the **TX ring**:

> **A log frame must never be able to displace a telemetry frame.**

Telemetry dropping is what the ESP interprets as a possibly-dead safety
processor (`firmware/CommonFW/docs/LINK_PROTOCOL.md` §8) — it blocks heating at 1.5 s and
aborts a firing at 30 s. A burst of verbose logging that fills the TX ring could
therefore stop a firing, which is a spectacular way for a debug feature to cause
an outage.

Two requirements follow, and they are not optional:

1. **Reserve TX ring capacity for telemetry.** Log frames may only use space
   that remains after the telemetry budget; when the ring is above the reserve
   watermark, log frames are dropped at the point of enqueue.
2. **Count the drops and report them** in the diagnostic frame, so "the log went
   quiet" is distinguishable from "nothing was logged".

Log emission is **best-effort and droppable in every transport** — never
blocking the task that logged (no-hang rule 3). `log_task` is the lowest
priority in the system for the same reason.

---

## 2. Layering

```
                    ┌──────────────────────────────────┐
                    │  safety_core_task                │  decides
                    │   └─ safety_guards.c  (PURE)     │
                    └───────┬──────────────────┬───────┘
                            │ verdict          │ reads snapshots
                    ┌───────▼──────┐   ┌───────┴────────────────────┐
                    │ relay_owner  │   │ thermo │ current │ discrete│
                    │  (GPIO6/K4)  │   │  task  │  task   │  task   │
                    └──────────────┘   └───┬────┴────┬────┴────┬────┘
                                           │         │         │
                                      ┌────▼───┐ ┌───▼───┐ ┌───▼────┐
                                      │spi_own │ │adc_own│ │ GPIO   │
                                      └────────┘ └───────┘ └────────┘

   link_task ──> publishes context snapshot ──> (read by safety_core)
       │         never called BY safety_core
       └─ uart_owner + uart_frame (framing/CRC)
```

**The arrows are the design.** `safety_core_task` *pulls* snapshots; nothing
pushes into it, and nothing it calls can block on the outside world.
`link_task` is a producer of data and never a service that anyone waits on.

### The one rule that matters

> **`safety_core.c` does not `#include` the link header, and `link_task` never
> touches the relay.**

This is checkable by grep, and it should be checked by grep in CI. It is what
makes "a frozen ESP cannot hang the safety processor" a structural property
rather than a promise.

---

## 3. Modules

### Hardware owners — one task per interface

| Module | Owns | Notes |
|---|---|---|
| `spi_owner` | SPI0 (GPIO0/2/3) + `CS0` (GPIO1) | Request-queue owner, mirroring `esp_spi_owner`. One device today; the pattern keeps J7 expansion cheap. CS driven as a plain GPIO so a register burst stays in one frame |
| `adc_owner` | RP2040 SAR ADC, channels 0/1/2 | Genuinely shared — one SAR muxed across three inputs. Round-robin, 16× oversample. No DMA (see `CURRENT_SENSE.md` §4) |
| `uart_owner` | UART1 (GPIO4/5) | RX ring + **non-blocking** TX ring. **Drops on full, never blocks** |
| `relay_owner` | **GPIO6 only** | The safety actuator. Highest priority. The *only* code in the build that writes GPIO6 |
| `i2c_owner` | GPIO7/8 | **Not initialised.** No device on the bus. Left as a documented no-op, not a half-init |

### Drivers

| Module | Purpose |
|---|---|
| `max31856.c` | Port of `firmware/KilnFW/App/drivers/MAX31856.c`. Same part, same registers, same conversions — **port it, do not rewrite it.** `firmware/KilnFW/docs/MAX31856.md` is the reference |
| `uart_frame.c` | `0x7E` framing, `0x7D` stuffing, CRC16/CCITT-FALSE. Byte-compatible with `uart_protocol.c`. **Parse only + emit; no ACK, no retry, no dedup** |

### Application

| Module | Purpose |
|---|---|
| `safety_guards.c/.h` | **Pure function.** `(config, snapshot, state) → verdict`. No RTOS, no SDK, no logging, no I/O, no time source of its own — `dt_s` comes in as an argument. Host-testable |
| `safety_core.c` | Assembles snapshots, calls `safety_guards_tick()`, commands `relay_owner`, owns latch/clear |
| `link_task.c` | Frame RX → context snapshot; telemetry TX (**required** — the ESP will not heat without it); version/clear/ceiling request handling |
| `build_info.h` | Generated every build: git commit, dirty flag, build timestamp. **Unknown must map to `dirty = 1`** |
| `config_store.c` | Thresholds + calibration in the last flash sector. CRC'd, versioned, with compiled-in safe defaults on corruption |
| `watchdog_task.c` | Hardware watchdog, fed only when every task has checked in |

`safety_guards.c` being pure is not stylistic. It is what let `KilnFW` find a
real bug in `heater_output_bangbang()` on the host before it ever reached a
board (`PROJECT_STATUS.md`), and the guard thresholds here are far harder to
provoke on real hardware than that one was.

---

## 4. Tasks, priorities, affinity

Higher number = higher priority.

| Task | Prio | Core | Period | Job |
|---|---|---|---|---|
| `relay_owner` | **7** | 1 | event | Drive GPIO6. Must always be able to drop |
| `watchdog_task` | 6 | 1 | 250 ms | Feed the HW watchdog iff all tasks checked in |
| `safety_core` | 5 | **1** | 100 ms | Run guards, decide, command the relay |
| `discrete_task` | 4 | 1 | 10 ms | Debounce E-stop + `mainFault` |
| `thermo_task` | 3 | 1 | `~DRDY` IRQ | Read the MAX31856 |
| `current_task` | 3 | 1 | 50 ms | Sample ADC0/1/2 |
| `link_task` | **2** | **0** | event | Frame assembly, context publish, telemetry |
| `log_task` | 1 | 0 | event | Drain the log ring into the link. Lowest, always droppable |
| `update_task` | 1 | 0 | event | Phase 10: UPDATE_BEGIN/DATA/END/ABORT flash I/O, UPDATE_STATUS replies |

`update_task` (added Phase 10, TODO.md's field-update work) is tied with
`log_task` at the lowest priority, also on core 0 -- its work (parsing
frames link_task hands it over a bounded queue, and the flash writes those
imply) is link-adjacent, not trip-adjacent, and it never touches
`relay_owner`. Note core placement does not shield core 1 from an update's
actual flash erase/program calls: `flash_safe_execute()`'s multicore lockout
halts BOTH cores for that duration regardless of which one issues the call
-- see `src/tasks/update_task.c`'s header comment. What core 0 placement
buys is that `update_task`'s ordinary (non-flash-write) scheduling only ever
competes with `link_task`/`log_task`, never with the guard-evaluation path.

**The affinity split is deliberate and it is how the no-hang constraint becomes
structural.** Everything that can trip the relay lives on **core 1**; everything
that touches the link lives on **core 0**. A link task that spins, deadlocks, or
faults cannot starve the guard path, because it is not competing for the same
core at all. This is a stronger guarantee than priority alone, and the RP2040
gives it away for free.

**`link_task` is the second-lowest priority in the system.** A busy, hostile,
or noise-flooded link must never delay a trip. If frames are arriving faster
than they can be parsed, they are dropped — newer context is strictly more
useful than older context.

### Watchdog

- Hardware watchdog, **1 s** timeout, fed only by `watchdog_task`.
- `watchdog_task` feeds only if every registered task has checked in within its
  own deadline. **A hung `thermo_task` therefore reboots the chip**, and the
  reboot de-energizes K4. Fail-safe by construction.
- `watchdog_enable_caused_reboot()` is read at startup and reported in the
  diagnostic frame's `boot_reason` (`LINK_PROTOCOL.md` §6). A safety processor
  silently watchdog-looping presents as a working system with mysterious
  intermittent trips — that bit is how you find it.
- **`watchdog_task` must not depend on `safety_core`.** A control loop cannot
  be its own watchdog — the same reasoning `thermal_guard.h:19-22` gives for
  keeping `KilnFW`'s guard 9 in a separate task.

---

## 5. Boot sequence

Order matters; this one is chosen so that no failure can leave heat on.

```
1.  GPIO6 → output, driven LOW.        ← FIRST, before anything else
2.  Watchdog enabled (1 s).
3.  Read boot_reason; latch it.
4.  Load config from flash; on CRC failure use safe defaults + set a flag.
5.  Bring up spi_owner, adc_owner, uart_owner (non-blocking TX).
6.  Probe the MAX31856. Failure is logged, not fatal — S5 handles a blind
    processor; refusing to boot would leave the E-stop and current guards
    unserved too.
7.  Start tasks in priority order.
8.  Enter GRACE for startup_grace_s (60 s): guards evaluate and report, but
    the relay stays de-energized.
9.  Enter ARMED. K4 may now be energized, iff no guard is tripped.
```

**Step 1 is not a formality.** GPIO6 powers up as a floating input; a floating
gate on Q4 is an undefined coil state. Driving it low before anything else runs
is the same discipline `KilnFW` applies to its own fault line and to
`kiln_io_init`'s relay-bits-before-direction ordering.

**Step 8 exists because arming instantly is wrong.** Thermocouple readings need
a few conversions to settle, the current channels need their zero re-checked,
and a rolling window with three samples in it has no opinion worth acting on.
Sixty seconds of watching before permitting heat costs nothing on a firing
measured in hours.

Only `uart_owner` failing to initialise aborts the boot; everything else is
logged and stepped over. Same policy as `firmware/KilnFW/App/main.c`.

---

## 6. Data flow and snapshots

Each producer publishes a **complete, self-consistent, timestamped snapshot**
into a double-buffered slot. Consumers take the newest whole snapshot; there is
no partial read and no lock held across a bus transaction.

```c
typedef struct {
    uint32_t timestamp_ms;
    bool     valid;
    float    tc_c, cj_c;      /* NaN when !valid */
    uint8_t  fault_bits;      /* THERMO_FAULT_* */
    bool     spi_failed;
} thermo_snapshot_t;

typedef struct {
    uint32_t timestamp_ms;
    float    amps[3];
    bool     clipped[3];
    bool     calibrated;
} current_snapshot_t;

typedef struct {
    uint32_t timestamp_ms;    /* when the last valid frame arrived */
    bool     valid;           /* false = never received, or stale */
    uint8_t  boot_id, relay_now_mask, relay_recent_mask, recent_window_s;
    uint8_t  zone_count, flags;
    uint32_t seq;
    struct { uint8_t index, flags; float setpoint_c, measured_c; } zones[3];
} context_snapshot_t;
```

`safety_core` reads all three plus the debounced discretes, builds one
`guard_input_t`, and calls the pure evaluator. **Every guard in a given tick
sees one consistent picture** — the same reason `profile_executor.c` reads every
channel once per tick.

**Staleness is checked by the consumer, not the producer.** A producer that has
stopped updating cannot mark its own data stale, which is precisely the case
that matters.

---

## 7. Configuration

Thresholds (`SAFETY_MODEL.md` §4) and calibration (`CURRENT_SENSE.md` §5) live
in the last flash sector.

- **Versioned and CRC'd.** On mismatch or corruption, fall back to compiled-in
  defaults **and set `calibration_missing`** in the diagnostic frame. Silently
  running on defaults that were never commissioned is the failure this flag
  exists to make visible.
- **Guards with no defensible default ship disabled** — `max_rate_c_per_min`
  (S8) defaults to 0 = off, and the four commissioning fields
  (`tc_placement_mode`, `abs_max_temp_c`, `tc_type`, `ct_channel_map`) have no
  defaults at all. Same discipline `thermal_guard.h:67-75` applies to guard 8's
  `cross_zone_max_delta_c`. Full table: `CONFIG_REFERENCE.md`.
- Written over the link, or over SWD during commissioning. **A config write is
  refused while ARMED** — retuning a safety threshold during a firing is not a
  supported operation.

---

## 8. RP2040 specifics that will bite

Platform behaviours that are easy to get wrong and expensive to debug. All of
these are RP2040/pico-sdk facts, not design choices.

### Flash writes stop the world — and there are two cores

Writing flash on an RP2040 requires **XIP to be disabled**, which means any code
executing from flash faults while the write is in progress. On a FreeRTOS **SMP**
build, the *other* core is executing from flash too.

So `config_store` cannot simply call `flash_range_erase()`:

- Use **`flash_safe_execute()`** (pico-sdk 1.5.1+), which performs the multicore
  lockout and parks the other core in RAM for the duration.
- Any **interrupt handler that may fire during the write must be in RAM**
  (`__not_in_flash_func`), or it faults. The `~DRDY` and UART handlers are the
  candidates here.
- A 4 KB sector erase plus program is on the order of tens of milliseconds —
  comfortably inside the **1 s watchdog**, but not inside a 10 ms task deadline.
  `watchdog_task` must not be the thing that is blocked.

**`SAFETY_MODEL.md`'s "config writes are refused while ARMED" is therefore not
only a safety rule, it is a platform requirement.** GPIO state persists across a
flash operation, so K4 stays de-energized throughout — but a write during a
firing would stall the guard evaluation for tens of milliseconds, which is
exactly the moment not to.

### Latch the trip reason in the watchdog scratch registers

The RP2040's watchdog block has **eight 32-bit scratch registers that survive a
watchdog reset**. Use two of them: trip reason and a magic word.

Without this, a watchdog reset loses the reason the system was tripping, and the
board comes up looking merely freshly-booted. With it, `boot_reason` can report
"watchdog reset **while tripped on S1**", which is the difference between a
diagnosable fault and a mystery.

Validate the magic word before trusting the contents — the registers are
uninitialised after a true power-on.

### `watchdog_enable(ms, pause_on_debug)`

The second argument pauses the watchdog while a debugger has the core halted.
**True for development builds, false for release.** A release build that pauses
on debug can have its watchdog silently disabled by an attached probe, and this
is the one component where that matters.

Read `watchdog_caused_reboot()` **before** re-arming, and distinguish it from a
power-on reset (the chip-reset block records whether the last reset was a POR).
Those two produce very different diagnoses.

### The ADC

- **`adc_gpio_init()` disables the digital functions** on GPIO26/27/28. Call it,
  and do not also configure them as GPIOs somewhere.
- **`adc_set_round_robin()`** cycles the mux across the three channels in
  hardware. Use it rather than switching the mux by hand between reads.
- The first conversion after a mux change is the one to distrust; discard it.
  This matters more than it sounds with 16× oversampling per channel.
- ADC4 is the **internal die temperature sensor**. It is not the enclosure
  temperature (S12 uses the MAX31856's cold junction for that), but it is a free
  sanity check that the chip is in a sane thermal state, and it costs one more
  round-robin slot.
- ADC3 reads `VSYS/3` on a standard Pico — **useless here**, because VSYS is
  unconnected (`HARDWARE.md` §2). Do not sample it and do not report it.

### FreeRTOS SMP configuration

Core affinity (§4) needs `configNUMBER_OF_CORES 2` and
**`configUSE_CORE_AFFINITY 1`**, then `vTaskCoreAffinitySet()` per task. Without
the second define the affinity calls silently do nothing and the isolation
property in §4 quietly evaporates — **assert it at build time** rather than
trusting it.

Use a **1000 Hz tick**. The 10 ms `discrete_task` deadline wants it, and nothing
here is power-constrained.

---

## 9. States and trip reasons

```
   POWER-ON / RESET
         │  GPIO6 driven LOW before anything else
         ▼
   ┌───────────┐  config loaded, tasks started
   │   INIT    │
   └─────┬─────┘
         ▼
   ┌───────────┐  guards evaluate and report,
   │  GRACE    │  relay stays de-energized (startup_grace_s = 60 s)
   └─────┬─────┘
         ▼
   ┌───────────┐  K4 may be energized
   │  ARMED    │◄──────────────┐
   └─────┬─────┘               │ condition clears
         │ guard WARN          │
         ▼                     │
   ┌───────────┐───────────────┘
   │   WARN    │  K4 stays energized; reported only
   └─────┬─────┘
         │ guard TRIP
         ▼
   ┌───────────┐  K4 de-energized. LATCHED.
   │  TRIPPED  │  exit only via CLEAR_TRIP or E-stop cycle,
   └─────┬─────┘  and only once the condition is false
         │ current still flowing after trip_verify_s
         ▼
   ┌────────────────────┐  the loudest state. No exit except
   │ TRIP_INEFFECTIVE   │  power removal at the breaker.
   └────────────────────┘
```

`WARN` is a reporting state, not a relay state — it exists so the GUI and the
diagnostic frame can distinguish "everything is fine" from "something is off but
not dangerous", which is most of what this system will ever say.

### `DEGRADED_NO_CONTEXT` — an orthogonal flag, not a state

The version handshake is mutual: each processor checks the other's protocol
version and neither trusts the other until both agree
(`../../CommonFW/docs/LINK_PROTOCOL.md`, `ANNOUNCE_VERSION`). When the ESP's
version is missing or incompatible, the safety processor cannot parse context
frames, so setpoints, the relay mask and any borrowed thermocouple reading are
all unavailable.

This is deliberately **a flag on top of the state above, not a state of its
own**, because it is orthogonal to everything the state machine tracks: a
processor can be in `GRACE`, `ARMED` or `TRIPPED` and simultaneously have no
usable context. Modelling it as a state would mean duplicating the trip
transitions inside it, which is how a state machine acquires the bug where a
trip is missed in one branch.

While the flag is set:

- context frames are **discarded unparsed** — no best-effort decode of a layout
  this build does not recognise;
- context-free guards (S1, S5, S7, S11, S12) run unchanged and still command the
  relay;
- context-dependent guards (S2, S3, S4, S10, S13) are **reported as disabled**,
  never as passing;
- **no trip is latched merely because of the mismatch.** Two processors flashed
  out of step is a development event, not a dangerous kiln, and the ESP is
  already refusing to heat. Tripping here would be the nuisance trip that §2 of
  `SAFETY_MODEL.md` exists to prevent.

Telemetry keeps flowing throughout, which is what lets the ESP display the
mismatch and push the update that fixes it.

```c
typedef enum {
    SAFETY_TRIP_NONE = 0,
    SAFETY_TRIP_OVERTEMP        = 1,  /* S1  */
    SAFETY_TRIP_OVER_SETPOINT   = 2,  /* S2  */
    SAFETY_TRIP_LOAD_STUCK_ON   = 3,  /* S3  */
    /* 4 reserved: S4 is WARN-only          */
    SAFETY_TRIP_SENSOR_INVALID  = 5,  /* S5, after blind_grace_s */
    SAFETY_TRIP_MAIN_FAULT      = 6,  /* S6a */
    SAFETY_TRIP_LINK_DEAD       = 7,  /* S6b */
    SAFETY_TRIP_ESTOP           = 8,  /* S7  */
    SAFETY_TRIP_RATE            = 9,  /* S8  */
    SAFETY_TRIP_INEFFECTIVE     = 10, /* S9 -- escalation, not a cause */
    /* 11 reserved: S10 is WARN-only        */
    SAFETY_TRIP_FROZEN_SENSOR   = 12, /* S11 */
    SAFETY_TRIP_ENCLOSURE_TEMP  = 13, /* S12 */
    SAFETY_TRIP_BORROWED_STALE  = 14, /* S13 */
    SAFETY_TRIP_CONFIG_CORRUPT  = 15,
    SAFETY_TRIP_SELF_TEST       = 16, /* a task failed to check in */
} safety_trip_t;
```

**The numbering follows the guard numbers and leaves gaps** — 4 and 11 are
reserved, because S4 and S10 are WARN-only and can never produce a trip code.
Once this ships, a trip code is a wire value that ends up in logs and
screenshots; renumbering it later to close a gap silently reinterprets every
historical record. Same reasoning `uart_task_ids.h` gives for never reusing a
task id lightly. **Guards may still be renumbered freely until the first
release** — nothing is built yet.

`trip_mask` in the diagnostic frame is a **bitmask indexed by guard number**, not
by this enum — several guards can be tripped at once, and the enum only names
the *first* one that fired.

---

## 10. Testing without hardware

Straight from `KilnFW`'s playbook, which found real bugs this way.

**Host tests (MSVC, no SDK).** `safety_guards.c` is pure, so every guard is
testable directly: feed synthetic snapshot sequences, assert the verdict.
The cases worth writing first are the *nuisance* cases, not the trip cases —
a 60 s heater window at 15 % duty must not trip S4; a 40 °C ramp-end overshoot
must not trip S2; a 900 ms sensor dropout must not trip S5.

**Reuse `firmware/KilnFW/App/test/sim_plant.c`.** It already models a kiln with element
lag, sensor transport delay and radiative loss. Driving `safety_guards` from it
gives realistic thermal traces for free.

**Assert the independence invariant in tests**, not just in prose: run the guard
suite with the TX path stubbed out entirely and assert the verdict stream is
bit-identical to a run with it live. Telemetry is required for the *ESP* to
permit heating, but no guard verdict here may ever depend on whether anything
is listening.

**Link testing with no Pico attached** already works from the ESP side —
`firmware/KilnFW/docs/SAFETY_LINK.md` §"How to test this without a Pico" describes
loopback and a PC-side stub on the safety UART. The stub is the right way to
develop the context-broadcast format before either firmware is finished, and it
works in reverse too: a PC-side stub *emitting* context frames is how
`SaftyFW`'s parser gets exercised before `KilnFW` can send any.


---

## Completion checklist

Phase 2 ("Skeleton") items below were done and **build-verified under the real
toolchain** 2026-08-16 (arm-none-eabi-gcc 14.2.1, pico-sdk 2.1.1,
FreeRTOS-Kernel's RP2040 SMP port, Ninja) — a from-scratch `cmake --build`
produces `SaftyFW.elf` with zero warnings under `-Wall -Wextra -Werror`. **Not
run on hardware** — no RP2040 or debug probe was attached to the build
machine, so nothing below claims more than "compiles clean." See `TODO.md`
Phase 2 for the itemised done/not-done breakdown this summarizes.

**Structure**
- [x] pico-sdk + FreeRTOS-Kernel SMP, CMake, `-Wall -Wextra -Werror`
- [x] `CommonFW` linked as `kilnlink`; **no protocol code duplicated here** —
      linked but not yet called from any SaftyFW source, same as `KilnFW`'s
      own component wrapper
- [x] One task per hardware interface, per §3 — task **shells** exist at the
      right priority/affinity; the interface-owning bodies (SPI/ADC/UART
      request queues) are Phases 3/6/7's job, not done here
- [x] `safety_guards.c` pure: no RTOS, no SDK, no I/O, `dt_s` passed in —
      done by a parallel Phase 4 effort (`src/safety_guards.c`), verified
      here only in that it includes nothing but `<math.h>`/`<stdarg.h>`/
      `<stdio.h>`/`<string.h>`. **Not yet wired into any SaftyFW task** —
      integrating it into `safety_core.c` is explicitly out of this phase's
      scope
- [x] **CI grep: `safety_core.c` does not include the link header; `link_task.c` does not touch GPIO6** —
      `tools/check_isolation.ps1`, comment-aware so the rule can be documented
      in the files it checks without self-triggering. Verified against both
      the real files and a deliberately-introduced violation. Not wired into
      an actual CI pipeline (none exists in this repo yet)

**Runtime**
- [x] Priorities and core affinity as §4; link work on core 0, trip path on core 1
- [x] `configUSE_CORE_AFFINITY` asserted at build time — `#error` in
      `src/task_priorities.h`, both it and `configNUMBER_OF_CORES` also set
      correctly in `FreeRTOSConfig.h` (the assert only compiles because they are)
- [x] Boot order per §5, **GPIO6 low as the first statement of `main()`** —
      steps 1–3 and 7 of the nine-step sequence are real; steps 4–6 and 8–9
      are TODO-commented in `src/main.c` for their respective later phases
- [x] GRACE → ARMED after `startup_grace_s` — built 2026-08-16
      (`src/tasks/relay_owner.c`): the `INIT`/`GRACE`/`ARMED`/`TRIPPED` state
      machine lives in `relay_owner`, with the GRACE timer starting the
      moment `relay_owner_task()` begins running rather than via a separate
      call from `main.c` (a judgement call documented in that file's header
      comment). `relay_owner_command_energize()` never drives GPIO6 high
      while `GRACE` or `TRIPPED`; only `ARMED` honours it.
      `safety_core.c` now calls `relay_owner_command_trip()` (de-energize +
      latch) on a new guard trip. Build- and host-test-verified; **not
      hardware-verified** — no RP2040 attached to the build machine
- [x] Watchdog fed only when every task has checked in — real
      bitmask-of-registered-tasks gate in `src/tasks/watchdog_task.c`, not a
      stub that always feeds
- [x] Trip reason latched in watchdog scratch registers, magic-word validated —
      `src/boot_reason.c` (scratch[0]/[1], deliberately not scratch[4], which
      pico-sdk's own `watchdog_enable_caused_reboot()` uses). The
      read/validate/clear path runs every boot. **The latch-on-trip call is
      now wired**, 2026-08-16: `safety_core.c` calls `boot_reason_latch_trip()`
      immediately after `relay_owner_command_trip()` whenever
      `safety_guards_tick()` returns a new trip. Build- and
      host-test-verified that the wiring compiles and the untouched
      `safety_guards.c` host tests still pass; **not hardware-verified** —
      no RP2040 attached, so the scratch-register write itself has never
      run on real silicon
- [ ] `pause_on_debug` true in dev builds, **false in release** — true is
      hardcoded (`main.c`); there is no release/debug build distinction in
      this CMake project yet to switch on, so the "false in release" half is
      not implemented

**RP2040 specifics (§8)**
- [ ] `flash_safe_execute()` for every config write — **not started**, no
      config store exists yet (Phase 9)
- [ ] ISRs that can fire during a flash write are `__not_in_flash_func` —
      **not started**, no such ISR exists yet (the `~DRDY`/UART ISRs are
      Phase 3/7)
- [x] `adc_gpio_init()` called; ADC3/VSYS **not** sampled — `adc_gpio_init()`
      is called for ADC0/1/2 in `src/tasks/current_task.c`; `src/current_sense.c`
      only ever selects channels 0/1/2, ADC3 is never sampled. Build-verified
      2026-08-16, not hardware-verified.
- [~] Round-robin across ADC0/1/2 — built via manual `adc_select_input()` +
      single-shot `adc_read()` polling in `src/current_sense.c`, **not**
      `adc_set_round_robin()`/hardware free-running capture. Deliberate
      deviation, documented in `current_sense.c`'s header comment:
      `docs/CURRENT_SENSE.md` §4's "no DMA, no free-running capture" is more
      specific to this module than this section's general round-robin
      guidance, and the two read as being in tension until you notice that.
      Marked partial rather than checked because the letter of this bullet
      (`adc_set_round_robin()`) is not what got built, even though the
      three-channel round-robin behaviour it describes is.

**Tests (§10)**
- [ ] Host harness building without the SDK
- [ ] `sim_plant.c` reused for thermal traces
- [ ] Guard verdicts bit-identical with the TX path stubbed out
