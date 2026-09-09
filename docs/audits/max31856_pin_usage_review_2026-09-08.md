# Are we using the MAX31856 ~DRDY and ~FAULT pins correctly? — 2026-09-08

Review only. Nothing was changed by this pass — no code, no config, no test,
no board interaction (no flash, no reset, no firing). `thermo_task.c` is being
edited concurrently and was read, not touched.

**Four MAX31856 devices total**, not five: three on the main board's
thermocouple daughterboard driven by KilnFW (ESP32-S3, logical channels 0/1/2)
and one on the safety processor driven by SaftyFW (RP2040). CLAUDE.md's "five
MAX31856 (U10–U14)" is a stale schematic-era count and is being corrected
separately.

## 1. What the datasheet actually says

`hardware/datasheets/ThermocoupleBoard_Sensor_Temperature/MAX31856.pdf`, p.15,
verbatim:

> **DRDY**
> The DRDY output goes low when a new conversion result is available in the
> Linearized Thermocouple Temperature register. When a read-operation of the
> Linearized Thermocouple Temperature register or the Cold-Junction
> Temperature Register (if enabled) completes, DRDY returns high.

So `~DRDY` is a **level**, not a pulse. It asserts (low) on conversion
completion and de-asserts **only** when the host reads LTCB or CJT. Nothing
else clears it — not time, not the next conversion. A host that watches only
the *falling edge* and misses one is therefore permanently stuck: the pin
stays low, no further edge can occur, and the only thing that could release it
is the read that the missed edge was supposed to trigger. That is a
self-latching stall, and it is exactly what happened on the safety channel
(`docs/audits/safety_tc_drdy_stall_2026-09-08.md`, commit `512d3c33`).

Conversion mode, p.12 and p.19:

> The conversion mode can be either continuous or "normally off", as selected
> by bit 7 of the Configuration 0 register (00h). When in the normally off
> mode, a single "1-shot" conversion may be selected using bit 6 …

> **One-Shot Mode** … This causes a single cold-junction and thermocouple
> conversion to take place when Conversion Mode bit = 0 (normally off mode).
> The conversion is triggered when CS goes high after writing a 1 to this bit.
> … This bit self clears to 0.

DRDY behaviour itself does not change between the two modes; what changes is
*who decides when a conversion happens*. In one-shot the host knows exactly
when to expect DRDY (it just triggered it, so an elapsed-time proxy is sound);
in CMODE=1 the part free-runs on its own cadence, asynchronously to firmware
startup — which is what makes an edge-only design a race.

`~FAULT`, p.19 and p.26:

> **Fault Status Clear** … 1 = When in interrupt mode, returns all Fault
> Status bits [7:0] in the Fault Status Register (0Fh) to 0 and deasserts the
> FAULT output. This bit has no effect in comparator mode. … To prevent the
> FAULT output from reasserting, first set the Fault Mask bits.

> Overvoltage or Undervoltage Input Fault … 1 = The input voltage is negative
> or greater than VDD. **The FAULT output is asserted unless masked.**

Mask register (02h) reset value is `FFh` — every fault masked off the pin. A
design that never writes MASK has a `~FAULT` pin that can never assert.

## 2. Conversion mode, per channel

All four are in **automatic conversion mode (CR0 CMODE = 1)**.

- ESP ch0/1/2: `MAX31856_config_default()`
  (`firmware/KilnFW/App/drivers/hw/MAX31856.c:737`) sets `auto_convert` from
  `CONFIG_KILNCTL_THERMO_DEFAULT_AUTO_CONVERT`, and the live
  `firmware/KilnFW/sdkconfig:3733` has `=y`. (Read from `sdkconfig`, not the
  Kconfig default — that distinction has bitten this repo before.) The
  configure sequence is correct per datasheet: CMODE cleared first, notch and
  CR1/AVGSEL written while stopped, CMODE restored last
  (`MAX31856.c:874-921`).
- Safety: `max31856_configure()` (`firmware/SaftyFW/src/max31856.c:233-265`)
  does the same three-step dance and ends with `cr0_running = cr0_stopped |
  MAX31856_CR0_CMODE`.

`MAX31856_trigger_one_shot()` exists on the ESP side but is a debug/manual
path only; no periodic caller. Nothing in either firmware assumes one-shot
timing while running auto-convert.

## 3. `~DRDY` handling, per channel

### ESP channels 0/1/2 — **correct**, and correct for the right reason

The three `~DRDY` lines land on the SX1509 expander (IO8/IO9/IO10), not on
ESP GPIOs. KilnFW does **not** interrupt on them at all. Instead
`main_boot_early.c:68` installs `main_kiln_drdy_provider()` via
`MAX31856_set_drdy_provider()`, which calls `kiln_io_get_drdy()`
(`kiln_io.c:424`) — a **level read** of the expander's RegData, inverted for
active-low. `MAX31856_read()` samples it *before* the six-register burst
(`MAX31856.c:1182-1189`), with a comment citing the exact datasheet reason:
the burst read is what releases `~DRDY` high, so sampling afterward would
always report "no new data".

This is a level-polling design against a level signal. It has **no edge to
miss**, so it is structurally immune to the safety channel's race — this is
not timing luck. If a DRDY assertion happens before firmware is ready, the
level is simply still there when the first read runs, which is the whole point
of the part's latching behaviour. The only degradation mode is `drdy_known ==
false` (I2C to the expander failing), which falls back to the elapsed-time
`max31856_result_due()` heuristic and only affects the `fresh`/`stale` flag,
never whether a read happens.

Two supporting details also check out:

- The per-channel DRDY pin table (`kiln_io.c:71`) is rotated one position to
  match the CS/fault rotation in `MAX31856_start_all()` (`MAX31856.c:785-790`)
  — logical channel *i* reads the DRDY of the same physical chip its CS talks
  to. An un-rotated table would have made a dead channel look fresh off its
  neighbour's DRDY. Already found and fixed on the bench 2026-08-27; verified
  still consistent.
- The DRDY expander pins get internal pull-ups (`KILN_IO_PULLUP_MASK`,
  `kiln_io.c:44`), so an absent/unpowered daughterboard reads high = "not
  ready" — the safe direction.

One latent trap worth naming: `kiln_io_init()` **does** configure a
falling-edge sense on the three DRDY pins (`kiln_io.c:218-219`) and enables
the SX1509 interrupt for them. Nothing consumes that for thermocouple
scheduling today — it only feeds the diagnostic snapshot / `~INT` state — so
it is harmless now. But it is the seed of the safety channel's bug: if anyone
later wires thermo reads to that edge, they inherit the same "armed on an
already-low pin" failure. Recommend a comment there saying the edge sense is
diagnostic only and that the read path is deliberately level-based.

### Safety channel — **wrong** (known, fix in flight)

`thermo_task_fn()` (`firmware/SaftyFW/src/tasks/thermo_task.c`) arms
`gpio_set_irq_enabled_with_callback(SAFTYFW_PIN_THERMO_DRDY,
GPIO_IRQ_EDGE_FALL, …)` as the first thing the task body does — deliberately
late, because arming it pre-scheduler double-faulted the board once (the
comment at that call documents this at length and the reasoning is sound).
Meanwhile `main.c`'s pre-scheduler `max31856_configure()` has already set
CMODE=1, so the part is free-running before the IRQ exists. If the first
conversion completes in that window, the edge is lost and — per the datasheet
passage above — no further edge is possible, because only a host read clears
it and the only reader is gated behind the notification. That is precisely the
observed state: GPIO12 stuck LOW across three samples, `s_snapshot` showing
the DRDY-silence branch's output.

This is an **edge-triggered design against a level signal**, which is the
underlying category error. The already-in-tree fix
(`thermo_task_drdy_missed_edge()` plus the `gpio_get()` consult on timeout) is
the right shape: it reinstates a level check as the backstop, and it correctly
refuses to fabricate a reading when the pin reads HIGH, so a genuinely dead
part still falls through to sensor-invalid and S5. I did not touch it.

Two follow-ups I would ask of whoever owns that fix:

1. The recovery currently only runs **after** a full `timeout_ms` (~302 ms)
   has elapsed, once per iteration. That is fine steady-state, but it means
   the boot-time race costs one wasted timeout and one bogus sensor-invalid
   snapshot before recovery. A single `gpio_get()` immediately after arming
   the IRQ — before entering the wait loop the first time — closes it with no
   invalid publication at all. That was the recommendation in the stall audit
   and it is still the cleaner form.
2. `thermo_task_drdy_missed_edge()` is `static` in `thermo_task.c` and has
   **no host test** as of this review, despite its own doc comment citing
   host-testability as the reason it was extracted. Its three-input truth
   table (notifications, assume_ready, pin level) is exactly the kind of thing
   that should be pinned before it is trusted. I deliberately did not add one:
   the file is mid-edit in another session and a test landing on it now would
   collide.

## 4. `~FAULT` handling, per channel

**All four**: the pin is configured as an input with a pull-up, **polled at
read time**, and **never interrupt-driven**.

- ESP: `MAX31856.c:587-606` configures `intr_type = GPIO_INTR_DISABLE` with
  the internal pull-up (the daughterboard has no external one), and the
  comment explains the choice — the level is sampled during `MAX31856_read()`
  into `out->fault_pin_asserted` (`MAX31856.c:1179`) and is always corroborated
  by the SR byte read in the same burst. For a signal that only changes at
  conversion rate, that is a defensible and I would say correct choice.
- Safety: `max31856_init()` (`max31856.c:178-185`) does the same via
  `hal_gpio_init_in(..., HAL_GPIO_PULL_UP)`, and `max31856_read()` samples it
  through `max31856_fault_pin_asserted()` — a split-out, host-tested polarity
  predicate. Polarity is right: LOW = asserted.

**Mask register is configured on all four**, which is the important half —
the reset default of `FFh` would have left every `~FAULT` pin permanently
idle, and both firmwares call that out explicitly as a known trap.
`MAX31856_DEFAULT_FAULT_MASK` unmasks exactly OPEN and OVUV
(`MAX31856.h:143`; SaftyFW `max31856.h:110` mirrors it, plus the reserved
`0xC0`). Those are the two hard "this reading is meaningless / the part has
suspended conversions" faults, so the pin asserts for what matters and stays
quiet for the threshold faults whose limits are at full scale anyway. ESP
writes it in `MAX31856_start_all()` (`MAX31856.c:817`); safety writes it as
step 3 of `configure()` (`max31856.c:256`). Correct on all four.

Comparator vs interrupt fault mode: both firmwares use **comparator mode**
(CR0 bit 2 clear — ESP `cfg->interrupt_fault_mode = false`, and no caller
overrides it; safety hardcodes it). That is the right pairing with a polled
pin: fault bits and the pin self-clear when the condition goes away, so
nothing needs to issue a FAULTCLR that comparator mode would ignore anyway.
No code anywhere writes `MAX31856_CR0_FAULTCLR` on a schedule, which would
have been the tell of a mode confusion. None found.

**One real defect found, documentation-level but the misleading kind.**
`firmware/SaftyFW/src/max31856_fault_pin_policy.h` states that its output
"feeds `max31856_reading_t.fault_pin_asserted`, which safety_guards.c's S5
(thermocouple fault) reads", and the header's whole rationale is that a
flipped polarity would silently disable S5. That is not true today: S5
(`safety_guards.c:132`) reads `in->fault_bits` — the **SR register** — and
`fault_pin_asserted` is never propagated into `thermo_snapshot_t` or any guard
input. Grepping SaftyFW, the field is written in `max31856.c:317` and read
nowhere outside its own unit test. So the safety channel's `~FAULT` **pin** is
currently a diagnostic that no one reads, while the header claims it is a
safety input. The behaviour is not unsafe — SR carries the same fault bits and
S5 does act on them — but a reader is being told a guard depends on a signal
it does not depend on, and it would make a future polarity or wiring
regression look harmless. On the ESP side the same field is honestly scoped:
it reaches only the diagnostics HTTP page, the LCD diagnostics page and the
UART bridge, and nothing claims otherwise.

Recommend: correct that header comment to say what actually consumes the
field, **or** wire `fault_pin_asserted` into `thermo_snapshot_t` and let S5
treat pin-asserted as an additional bad-read condition. The second is the
stronger design — the pin is an independent hardware path to the same
information, so an SPI bus that reads plausible-looking garbage would still be
caught — but it is a safety-behaviour change and belongs to the owner, not to
a review pass.

## 5. Chip select and bus sharing (ESP, three devices)

Correct. Each channel gets its own CS pin and its own `hal_spi_device`
(`MAX31856.c:608-640`), attached to the shared SPI host that
`main_boot_early.c` already brought up for the ST7796 panel. Serialization is
two-layer: a per-channel FreeRTOS mutex around every register access
(`max31856_lock()`, bounded timeout, failure returns the pre-staged
NaN/`spi_failed` answer rather than blocking), and `spi_owner`'s bus ownership
underneath every `hal_spi_transfer_polling()` call — so a display blit cannot
interleave inside a thermocouple burst. Mode 1 (CPOL=0/CPHA=1) matches the
part. Under the bit-banged-CS build, CS is driven high the moment it becomes
an output, before anything can clock, so a MAX31856 cannot latch the display's
traffic (`MAX31856.c:563-579`); under `KILNCTL_SPI_HARDWARE_CS` the peripheral
owns the pin and the code deliberately does not also drive it as a GPIO.

The six-register burst (CJTH…SR in one transaction) is also the right call for
DRDY-driven reads: split single-byte reads would let a conversion land
mid-sequence and pair a cold-junction value from one conversion with a
hot-junction value from the next.

The DRDY read is on a *different* bus (I2C to the SX1509), so sampling DRDY
does not disturb the SPI transaction it precedes — and because DRDY is a
per-chip latched level, there is no cross-talk risk between channels: reading
channel 1's registers releases only channel 1's DRDY. The rotation fix (§3)
is what makes that mapping true; without it the channels *would* have been
reading each other's ready lines.

The one weakness is inherent to the expander: DRDY sampling costs an I2C round
trip and, if the SX1509 is unreachable, silently degrades to elapsed-time
freshness. That is already handled, logged, and is the documented reason the
safety channel's direct-GPIO DRDY was considered worth doing at all.

## 6. Verdict

| Channel | `~DRDY` | `~FAULT` |
|---|---|---|
| ESP ch0 | **correct** — level-polled, no edge dependency | **correct** — polled, mask configured, honestly scoped as diagnostic |
| ESP ch1 | **correct** — same path | **correct** |
| ESP ch2 | **correct** — same path | **correct** |
| Safety | **wrong** — edge-armed against a latching level; known, fix in flight | **correct** in hardware handling and mask config; **header misstates its consumer** (claims S5 reads it; S5 reads SR) |

Stated plainly, since a clean verdict is the useful answer: the three ESP
channels are not "working by luck". They never arm an edge on `~DRDY` at all,
so the race that stalls the safety channel cannot occur there by construction.
Conversion mode is auto on all four and every DRDY consumer is written to auto
semantics. The fault mask — the single most common way to get `~FAULT` wrong —
is correctly written away from its `FFh` reset default on all four, in
comparator mode, matching the polled-pin design.

What I would change, in priority order:

1. Move the safety channel's missed-edge check to **immediately after arming
   the IRQ**, not only after the first timeout, so the boot race costs no
   invalid snapshot. (Owner of the in-flight `thermo_task.c` edit.)
2. Add the host test for `thermo_task_drdy_missed_edge()`'s truth table once
   that edit settles.
3. Fix `max31856_fault_pin_policy.h`'s claim about S5, or wire the pin into
   S5 for real — decide which, do not leave the comment as-is.
4. Add a one-line comment at `kiln_io.c:218` that the DRDY falling-edge sense
   is diagnostic only and the read path is deliberately level-based, so the
   safety channel's bug is not re-imported into KilnFW later.

`tools/run_all_checks.ps1`: 74 passed, 0 skipped, 0 failed.
