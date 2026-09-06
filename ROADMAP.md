# kilnCtl Roadmap — both processors

> **Status:** planning · **Last reviewed:** 2026-09-05, roadmap-upkeep audit
> (seventh sweep) — landed the cone-unrated bucket (`1501f0c`+`3b0c82e`), the
> `safety_cfg_http.c` PSRAM move (`541b357`, flashed and re-baselined —
> `af17e3d` plus the follow-up dram_margin.h/doc pass),
> S8's compiled default (`c43323a`+`ea69efa`, bench value still gated on a
> GRACE-window write), and the >62 °C ff_hold-infeasible confirmation
> (`94b1a2a`); fuzzy bands are live on the board. Also recorded eight owner
> decisions from 2026-09-05 (`abs_max_temp_c` closed at 80 °C by design, CTs
> deferred, `UnitTestFixture` kept, M8's field-update exercise approved) and
> absorbed the previous sweep's
> LCD/display session plus a batch of review-finding fixes and a stack-
> margin capture. Full detail for every closed item lives in
> `docs/COMPLETED_2026-09.md`, per this file's own upkeep rule; earlier
> sweeps' audit trail lives in that file's edit history, not here.
> **Bench status, 2026-09-05:** the thermocouple swap is fixed, heater power
> confirmed close to previous levels (if a tuning run doesn't match earlier
> measurements, recalibration may be needed), and the test fixture kiln is
> available for firing again.
> **Start here:** the [What is actually left](#what-is-actually-left) section
> immediately below is the short answer; the milestones are the detail.
> **Keep this file current.** This is the top-level dispatch board: the place to
> start a task from when you do not already know which plan owns it. It holds
> *ordering and cross-processor dependencies only* — the detail lives in the
> per-area plans linked below, and duplicating their content here guarantees the
> two will drift. When a milestone lands, tick it here **and** in the owning
> plan. When the shape of the work changes, edit this file rather than letting it
> describe a project that no longer exists.

The system is two firmwares that must agree with each other:

- **`KilnFW`** — ESP32-S3 main controller. Thermocouples, SSR heater outputs, PID,
  profiles, Wi-Fi, web GUI. Partly built and partly verified on hardware.
- **`SaftyFW`** — RP2040 safety processor (A1). Independent overheat and fault
  detection, owns the mechanical pilot relay K4. **Built and running on real
  silicon**; every guard input is now produced, and what remains is
  commissioning values plus the hardware-gated trip proofs.

They talk over an isolated UART (a digital isolator, U6, as of 2026-08-25;
previously an optocoupler pair). That link, and the rule that **the safety
processor must be alive for the main processor to heat**, is what makes this one
project rather than two.

---

## Index of what is left, by complexity

Every open item in this file, in one table, so the size of the remaining work
is visible without reading 1000 lines. **This is an index, not a second copy of
the plan** — each row points at the milestone that owns the detail, and when
the two disagree the milestone is right. Complexity is effort *once the item is
unblocked*; an XL that is blocked on a decision is still one sentence of your
time away from being startable.

| Size | Means |
|---|---|
| **S** | An hour or less. One file, or one number, or one question answered |
| **M** | A session. Several files, or a bench procedure with a known script |
| **L** | Multiple sessions. Touches persisted data, or a subsystem, or needs its own test pass |
| **XL** | A project. New hardware in the loop, or an unretired risk with no reproducer yet |

### Blocked on you — nothing in the code can answer these

| Size | Item | Where |
|---|---|---|
Six of the nine questions from 2026-08-28 became work rather than questions —
see [M12](#m12--commissioning-the-operator-can-actually-do--opened-2026-08-28).
Eight more were decided by the owner on 2026-09-05. What is still genuinely
open is short:

| Size | Item | Where |
|---|---|---|
| **XL** | CTs — deferred, 2026-09-05. Analysis lives in `docs/CONTACTOR_FEEDBACK_OPTIONS.md`. | `docs/CONTACTOR_FEEDBACK_OPTIONS.md`; M5 |
| **M** | **New CT hardware, 2026-09-05**: a real CT is now fitted on `Current3`/GPIO28-ADC2 (1A:1V, ~+59 mV DC offset, SUMMED across all heaters). (a) Add offset handling + calibration for this channel — find/extend `firmware/SaftyFW/src/ct_amps_cal.c`; HARDWARE.md §9 updated. (b) Re-evaluate which of S3/S4/S9/S11/S14 (`GUARD_TEST_MATRIX.md`, `safety_guards.c`) become usable with one SUMMED CT vs which still need per-zone current. No firmware changed yet. | M4/M5; `firmware/SaftyFW/docs/HARDWARE.md` §9, `GUARD_TEST_MATRIX.md` |
| **S** | **Safety TC display audit, 2026-09-05**: the safety processor's own thermocouple should show under "Thermocouple faults" on LCD/web/PcTools UI only when it is a SEPARATE physical TC (`tc_source == SAFETY_TC_SOURCE_OWN_J7` in `firmware/SaftyFW/src/safety_guards.h`) — hide/suppress it when safety is off or configured `BORROWED_ZONE`/`BOTH` (reusing a main TC). Audit every display site; no firmware changed yet. | `firmware/SaftyFW/src/safety_guards.h` (tc_source); `firmware/SaftyFW/docs/SAFETY_MODEL.md` §3 |
| S | S8 sanity rate — `c43323a`+`ea69efa` set compiled default 33.3 C/min (2x fastest shipped ramp), fields_set-gated. Bench commission of 14.85 C/min NOT yet applied: Pico refuses config writes while ARMED, write must land during the 60 s GRACE window after a Pico reset. | M3 |
| — | High-temperature validation firing — closed 2026-09-05, `94b1a2a` confirms ff_hold infeasible above 62 °C on hardware. | `PID_EXPANSION_PLAN.md` §3.6i |
| **S** | **Display items needing the owner's own hands/eyes, 2026-09-04.** Three separate (touch corner accuracy CLOSED `f028e2f` — see M1): (1) a residual blue tint on the ST7796 panel with every firmware cause eliminated by measurement — needs the owner's eye, or a colorimeter, or a second unit; (2) wake-on-touch, first-touch-swallow and error-dismissal behaviour on display power, which need a finger on the actual glass; (3) the STOP-block 5V I2C hazard measurement at meter-module pins 10/12, still not taken. | `DISPLAY_ST7796_PLAN.md` §4 |
| **M** | ~~Field-update hardware exercise~~ — **ESP half done 2026-09-05**: OTA into `ota_0` + rollback both verified on the bench (PID gains byte-identical before/after, no heat, no firing). **Pico half attempted 2026-09-06**: a raw `.bin` (`arm-none-eabi-objcopy -O binary` on `SaftyFW_slotA.elf`, no header-packaging step needed — the ESP builds `UPDATE_BEGIN`'s header itself) staged and the relay started, but the Pico refused `UPDATE_BEGIN` ("a safety trip is pending") before any flash write — a real Pico-side interlock the ESP's own cached status did not show. The actual over-the-wire transfer, and whatever the bootloader/metadata gap noted in `SaftyFW/TODO.md` Phase 10 implies for a completed one, remain unexercised. See `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` "Hardware exercise 2026-09-05". | M8 |
| XL | **Two accepted risks in `docs/SAFETY_CASE.md`, new 2026-09-04: nothing currently mitigates either.** (1) Whether the two processors' independently-"healthy" verdicts are actually *correct* rather than merely self-consistent — e.g. both could be reading a shared, physically-faulted thermocouple wire. (2) Whatever sits downstream of both relays (a mechanical failure past K4) has no mitigation beyond K4 itself. Not a code gap — no reproducer exists and none is proposed; owner decision on whether/how to mitigate | `docs/SAFETY_CASE.md` H5, H9 |
| — | **Guard evidence is mostly host-tested, not hardware-verified.** Of ~20 tracked guard-level claims, 19 are host-tested and only **3** are hardware-verified (S5's sensor fit/masking finding, KilnFW thermal_guard guard 6, the E-stop polarity fix) — everything else, including all of S1–S4/S6–S14's trip logic and KilnFW guards 1/2/3/4/5/7/9, has never been provoked on real silicon | `docs/SAFETY_CASE.md` §4 rollup; `GUARD_TEST_MATRIX.md` §3 |

### Software, doable now — no hardware, no decisions

| Size | Item | Where |
|---|---|---|
| L | **Every fault says what was detected and what to do** — a standing rule, not a closing milestone, so it never fully closes: applies to every fault surface added from here on. All of S6a's own checklist items landed 2026-08-28 | M13 |
| XL | **Source layering + hardware abstraction** — `drivers/` reorg applied in `9f18ca5` (2026-09-05); HAL Phase 1a still pending | M16; `docs/HW_ABSTRACTION_PLAN.md` |
| L | ~~**An uncommissioned safety processor must refuse heating enable.**~~ Landed `5cd56b6`. Resolved 2026-08-28 by making CTs **optional hardware**: `ct_installed` (param `0x0109`) is a new ASKED commissioning question, and answering *no* drops the CT-map requirement **and** switches S3/S4/S9/S14 off while reporting them off. Verified on the live board: `commissioned: true`, heat permitted | M12 |

### Blocked on hardware that does not exist yet

| Size | Item | Where |
|---|---|---|
| S | The CT coupling transformer (Hammond 140QEX): one look at the PDF before ordering — 10.62 H is quoted at 1 kHz and applied at 60 Hz | M5 |
| S | Time the link-staleness ceiling (1.5 s) and the firing abort (30 s) with a stopwatch. Code is flashed; nobody has held the link down | M6 |
| M | S9's welded-contactor escalation — by definition needs a welded contactor. **Checked 2026-09-03: SimFW cannot do this — SimFW itself no longer exists** (removed `8553244`, 2026-08-28; `firmware/UnitTestFw` took its place and is unrelated ESP32-S3 bench-instrument firmware — DAC/AD9833/OLED/PCF8575 — with no path to the safety processor's current-sense input at all). Even when SimFW existed, its own removal commit records that `ct_calibration` "needs the fixture to physically drive current into the CT" — S9 (`firmware/SaftyFW/src/safety_guards.c:363-389`) latches only on real `any_current_present`, gated by `in->context_valid`, `in->current_sensing_commissioned` and NOT `in->current_sensing_disabled`; that flag comes from the CT's analog current-transformer signal through `current_sense.c`, not a GPIO a simulator MCU could assert. What would actually be required: a fixture that injects genuine AC current through the CT sense loop while the K4 drive line is confirmed de-energized — i.e. a hardware jig, not firmware simulation — plus a CT actually fitted and commissioned (`ct_installed=yes`; on the bare bench today `ct_installed=no` switches S9 off and reports it off). | M4 |
| M | AP-fallback verified end to end (needs a router with correct *and* deliberately-wrong static config) | M6 |
| M | Per-channel CT-to-jack commissioning, plus a bench measurement of the ADC noise floor under the 25-count presence fallback | M5 |
| M | **HW changes:** relay status LEDs for K1–K4/S9, distinct connector types for the thermocouple daughterboards, I2C broken out on an expansion connector. (LCD backlight control's flying wire is fitted and confirmed — see M1, closed 2026-09-04.) | M1 |
| S | **Blocking, before the MSP4031 touches J2 at all**: meter module pins 10/12 (CTP_SCL/CTP_SDA) at 5V — confirms or clears a hazard that can back-feed the SX1509/ESP32-S3 through the shared I2C bus. `DISPLAY_ST7796_PLAN.md` §4 | M1 |
| M | DEBUG header and GP16/GP17 access before A1 is soldered down | M0 |
| L | Field updates exercised against real hardware — **ESP OTA into `ota_0` + rollback done 2026-09-05** (see `UPDATE_PROTOCOL.md`). **Pico bootloader over a live UART1**: staging/relay-start now proven 2026-09-06, but the Pico refused `UPDATE_BEGIN` on a pending safety trip before any bytes crossed the wire, and the bootloader/metadata scheme itself is still unflashed on this bench unit (`SaftyFW/TODO.md` Phase 10) — a real end-to-end transfer and a real version mismatch both remain open | M8 |
| L | `GUARD_TEST_MATRIX.md` §3 — every enabled guard's real trip, safe-state power-on, sensor open-circuit, current-mapping commissioning | M4 |

---

## Where each kind of task is planned

| Plan | Owns |
|---|---|
| [`ROADMAP.md`](ROADMAP.md) (this file) | Milestone order, cross-processor dependencies |
| [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) | Main firmware: web UI, profiles, PID, thermal protection, storage |
| [`firmware/KilnFW/docs/PROJECT_STATUS.md`](firmware/KilnFW/docs/PROJECT_STATUS.md) | What in `KilnFW` is built vs. verified — the honest ledger |
| [`firmware/KilnFW/docs/UI_PLAN.md`](firmware/KilnFW/docs/UI_PLAN.md) | LCD + web UI usability/cleanup plan — no-scroll LCD audit, phone/tablet web audit, prioritized fix queue |
| [`firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`](firmware/KilnFW/docs/PID_EXPANSION_PLAN.md) | Per-zone control-algorithm choice (Cohen-Coon rule, fuzzy-PID layer), cross-zone coupling measurement (RGA) and feedforward |
| [`firmware/KilnFW/docs/DISPLAY_ST7796_PLAN.md`](firmware/KilnFW/docs/DISPLAY_ST7796_PLAN.md) | Second LCD panel (ST7796/MSP4031) support, runtime panel auto-detection, display SPI async/DMA |
| [`firmware/KilnFW/docs/FLASH_BUDGET_PLAN.md`](firmware/KilnFW/docs/FLASH_BUDGET_PLAN.md) | The 16 MB flash: partition table, image size, what has been reclaimed |
| [`firmware/KilnFW/docs/DRAM_PSRAM_PLAN.md`](firmware/KilnFW/docs/DRAM_PSRAM_PLAN.md) | Internal SRAM reclamation — allocator threshold, stack sizing, PSRAM relocation |
| [`firmware/KilnFW/docs/WEB_UI_RESPONSIVE.md`](firmware/KilnFW/docs/WEB_UI_RESPONSIVE.md) | Browser UI across display sizes: token consolidation, shell layout, the responsive sweep |
| [`firmware/KilnFW/docs/ARCHITECTURE.md`](firmware/KilnFW/docs/ARCHITECTURE.md) | Tasks, priorities, owner-task queues, single-writer ownership doctrine |
| [`docs/HW_ABSTRACTION_PLAN.md`](docs/HW_ABSTRACTION_PLAN.md) | KilnFW `drivers/` layering into role directories, and the `firmware/hwAbstraction/` tree (interface/esp/pico/host) for both firmwares — M16 |
| [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) | Safety firmware, phases 0–10 |
| [`firmware/SaftyFW/docs/SAFETY_MODEL.md`](firmware/SaftyFW/docs/SAFETY_MODEL.md) | What trips, why, and the anti-nuisance doctrine |
| [`firmware/SaftyFW/docs/ARCHITECTURE.md`](firmware/SaftyFW/docs/ARCHITECTURE.md) | Tasks, priorities, core affinity, logging transports |
| [`firmware/SaftyFW/docs/HARDWARE.md`](firmware/SaftyFW/docs/HARDWARE.md) | The traced board, pin map, bench connections |
| [`firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md`](firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md) | Guard-by-guard test coverage and real-input reachability |
| [`firmware/SaftyFW/docs/CONFIG_REFERENCE.md`](firmware/SaftyFW/docs/CONFIG_REFERENCE.md) | Every safety tunable, its default, and whether getting it wrong is dangerous |
| [`firmware/SaftyFW/docs/COMMISSIONING.md`](firmware/SaftyFW/docs/COMMISSIONING.md) | How those values get set from the web GUI and persist on the safety processor across OTA |
| [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md) | Shared `kilnlink` code, used by both firmwares |
| [`firmware/CommonFW/docs/LINK_PROTOCOL.md`](firmware/CommonFW/docs/LINK_PROTOCOL.md) | The wire, both ends — the contract neither side may break alone |
| [`firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](firmware/CommonFW/docs/UPDATE_PROTOCOL.md) | Field updates for both processors: interlocks, one-password auth, ESP OTA partitioning |
| [`firmware/SaftyFW/docs/BOOTLOADER.md`](firmware/SaftyFW/docs/BOOTLOADER.md) | The RP2040 bootloader, flash layout and recovery mode |
| [`tools/PcTools/TODO.md`](tools/PcTools/TODO.md) | GUI, MCP, GPIO probe, debug and logging for **both** processors |
| [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) §§12–13 | M10's instrumentation: HTTP route-table cap, internal-DRAM low-water alarm, task-stack high-water reporting |
| [`docs/SAFETY_CASE.md`](docs/SAFETY_CASE.md) | The cross-processor safety argument: hazard list, guard/measure/accepted-risk mapping, residual risks, evidence classification (argued/host-tested/hardware-verified). Written 2026-09-04, still thin on hardware evidence — see its own §5 |
| [`docs/SYSTEM_ARCHITECTURE.md`](docs/SYSTEM_ARCHITECTURE.md) | The system level spanning both processors: end-to-end command/trip paths, the board-to-board interface, power domains and GND crossings, cross-processor boot/shutdown ordering. Written 2026-09-04 |
| [`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md) | The hardware/software reorganisation and its blockers |
| [`docs/SETUP.md`](docs/SETUP.md) | Fresh-clone setup: what is machine-specific, and how `tools/setup.ps1` handles it |

---

## The dependency spine

Four facts set the order. Everything else can be shuffled.

1. **The safety UART pins are swapped in `KilnFW`.** Until that is fixed the link
   cannot pass a byte in either direction, so every link-dependent task is
   blocked behind it.
2. **The wire contract is shared code.** Writing it twice guarantees the two
   copies diverge, so `CommonFW` comes before either firmware consumes it.
3. **The relay is the last thing to wire up.** Guards get built and host-tested
   against synthetic inputs before anything can physically open a contactor.
4. **The liveness rule cuts both ways.** Once the ESP refuses to heat without
   safety telemetry, a main board with no Pico fitted cannot heat either — so
   that switch is thrown deliberately, with a documented bench escape hatch.

---

## What is actually left

The milestones below are the detail. This is the honest short answer, because
after M0 cleared, "what remains" stopped being a code question and became
mostly a hardware-and-decisions question. Reviewed 2026-08-28.

**The bench changed on 2026-08-28 and several long-standing "theoretical"
items became live.** The heating elements are now PHYSICALLY CONNECTED. Every
statement anywhere in this repo that reasons from "relays may be activated but
nothing will get warm" is now false, and that assumption is load-bearing in
more places than it looks.

Two consequences that need to be read together:

- **The fixture is limited to 80 °C, and that is a property of the FIXTURE,
  not of the kiln.** It is currently enforced by `max_temp_c = 80` on all
  three zones (set live over `/api/zones`, previous config saved). This is a
  TEST THRESHOLD and must come out before a real kiln — `SaftyFW/TODO.md`
  phase 9 already carries "confirm no test threshold was left in place", and
  this is now a concrete instance of it, not a hypothetical.
- **Only ONE processor is enforcing that limit.** The safety processor's
  `abs_max_temp_c` reads `set: true, value: 0`, and 0 on that field means
  *never trip*. So the independent overtemperature guard is not armed, and the
  ceiling is enforced by the same processor that commands the heat. Note the
  reporting trap that hid this: `/api/readiness` says "all 58 safety
  parameters have values", which counts a zero as a value — the field next to
  it, `commissioned`, says `false`. Setting a Pico-side ceiling is offered and
  awaiting the owner's number.

**Blocked on you — mostly answered on 2026-08-28.** Six of the nine questions
that stood here were answered in one message, and the answers were largely
*"the operator should be able to enter that"*, which converted them from
questions into [M12](#m12--commissioning-the-operator-can-actually-do--opened-2026-08-28).
Summarised, because the answers matter beyond the milestone that implements
them:

- **An uncommissioned safety processor must NOT grant heating enable.**
  Unqualified NO. It grants it today, which is the only reason the bench can
  heat, so M12 lands this one **last** — see that milestone's ordering note.
- **Kiln maximum temperature** is entered on a safety web page, and feeds
  `abs_max_temp_c`. **Thermocouple maximum is inferred from the thermocouple
  type**, not asked for.
- **Every relay is rated for 100% duty and inrush is negligible — the board is
  designed for it.** This closes the SSR-vs-contactor-coil question and the
  2 A/125 VA duty-window check outright.
- **Breakers are assumed sized for the full kiln load at 100% duty.** Instead
  of per-zone element power, the operator enters maximum expected kiln power
  on the safety page, and the zones page gets a button that measures each
  zone's normal current by energizing them one at a time. That measurement is
  then used at runtime to check each CT is on the zone it is configured for,
  and to warn when it is not.
- The twelve stale `display_*` MCP tools were left to my judgement: delete.

All three items once listed here as still unanswerable are now closed: physical
zone arrangement (owner-confirmed 2026-09-03), the deferred S8 sanity rate
(decided 2026-09-05 — see the table above), and `hardware/UnitTestFixture/`
(owner said KEEP, 2026-09-05). **Scope firmed up 2026-09-05: `UnitTestFixture`
is a control device only** — a PcTools/MCP surface driving its I/O expanders
to flip relays, for shorting/opening thermocouples, opening heater
connections, and simulating SSR lock-ups. It stays excluded from the HAL
boundary and the `drivers/` reorg (M16/`HW_ABSTRACTION_PLAN.md`). See
`docs/UNIT_TEST_FIXTURE_PLAN.md` for the relay inventory, wire protocol, and
the MCP control surface.

**Blocked on hardware that does not exist yet.** All of this is scripted and
waiting, not unwritten:

- `GUARD_TEST_MATRIX.md` §3's trip rows — every enabled guard's real trip,
  safe-state power-on, sensor open-circuit, current-mapping commissioning.
- ~~The safety processor's own MAX31856~~ — **fitted 2026-08-24 and verified
  reading 30.2 °C.** ~~Still absent: any CT~~ — **2026-09-05: a CT is now
  fitted on `Current3` (GPIO28/ADC2) and confirmed working** — 1A:1V CT,
  ~+59 mV DC offset, reading the summed current of ALL heaters (not
  per-zone); `Current1`/`Current2` remain unpopulated. See the new items
  below and `firmware/SaftyFW/docs/HARDWARE.md` §9.
- S9's welded-contactor escalation, which by definition needs a welded
  contactor. **Checked 2026-09-03**: not a SimFW task — SimFW was removed
  (`8553244`, 2026-08-28) and its replacement, `firmware/UnitTestFw`, is
  unrelated ESP32-S3 bench-instrument firmware with no connection to the
  safety processor's current sense. S9 latches on real `any_current_present`
  from the CT's analog signal (`safety_guards.c:363-389`), which requires
  physically driving current through the CT — a hardware jig, not something
  any firmware simulator asserts over GPIO — plus a CT actually fitted and
  commissioned, since `ct_installed=no` switches S9 off on today's bare bench.
- The CT coupling transformer: the part is now selected (Hammond 140QEX) and
  ordering is unblocked, but its 10.62 H figure is quoted at 1 kHz and applied
  at 60 Hz — one look at the PDF before the order goes in.
- Link-staleness *timing* (the 1.5 s ceiling, the 30 s firing abort). The Pico
  is on the bench and the code is flashed; nobody has held the link down and
  timed it.

**Genuinely still software, and doable without you or the fixture.** This is
now a short list, which is the point:

- **Display power (brightness/idle-timeout/keep-on-while-firing/display-on-
  error), 2026-09-04.** New feature, not an M11 reopen — see
  `firmware/KilnFW/docs/UI_PLAN.md`'s "Display power" section for the full
  writeup. Pure decision core + persisted settings + HTTP API + settings-page
  UI are built and host-tested. **Brightness is no longer inert
  (`be02d34`):** the flying wire (GPIO15 → panel pin 8) is fitted, owner-
  confirmed by meter, `KILNCTL_BACKLIGHT_PWM_ENABLE` now defaults on, and ON
  duty is driven from the operator's brightness setting rather than a
  Kconfig constant — not yet confirmed by meter/eye that the panel actually
  dims, that's the next check. `keep_on_while_firing`/`display_on_error` now
  default **true** (`f028e2f`, owner decision from a real finger on the
  glass), so both are already right the day a timeout is chosen. The
  `screen_idle.c`/`lvgl_port.c` touch-swallow/timeout hookup landed the
  same day (`7fc17cc`), bound to real producers — the executor for
  `firing_active`, the RP2040 DIAG trip for `error_active`. Review then found
  both producers too narrow (`192eb7d`): keep-on-while-firing blanked the
  screen mid-autotune, since autotune holds relay authority with the executor
  IDLE, and display-on-error missed the ESP's own guard aborts, which never
  touch the RP2040's `diag_state`. Both widened. The 1-minute timeout is
  hardware-verified via `touch_get_state()` ("screen on" → "screen blanked"),
  as is NVS persistence across a reboot. Wake-on-touch, first-touch-swallow
  and error dismissal still need the owner's finger.
- ~~Touch was mirrored top-to-bottom on the ST7796 glass~~ **CLOSED
  (`f028e2f`)** — Y-invert was the wrong knob (X was fine); capacitive
  orientation knobs are now conditional on `KILNCTL_DISPLAY_PANEL_ST7796`.
- ~~LVGL's wake-edge invalidate reentered its own flush callback~~
  **CLOSED (`51e1ef5`) — killed a live firing on the bench before the fix.**
  This is the root cause behind the `safety_poll` panic/`configASSERT`
  chain; full postmortem in `CLAUDE.md` "Firmware gotchas".
- ~~LCD diagnostics 9 pages, profile-detail layout, builtin-catalogue
  browse-by-family, web display-settings styling, cone/firing-type
  verification~~ **all CLOSED 2026-09-04** (`1cf200f`, `445a78e`, `0470185`,
  `70ef683`, `9d73c8f`) — owner-feedback fixes on the real panel plus a
  source-checked correction of the builtin catalogue's cone metadata (3
  fixed, 15 confirmed). The remaining 10 unrated cones closed 2026-09-05
  (`1501f0c`+`3b0c82e`): `PROFILES_BUILTIN_CONE_UNRATED`, LCD catalogue
  "Unrated" bucket sorted last (owner decision).
  Full detail: `docs/COMPLETED_2026-09.md`. Not yet flashed.
- **`screen_idle` held its own lock across the producer reads, 2026-09-04
  (`7a8594d`).** The policy tick called `dashboard_get_status()` (five
  MAX31856 SPI bursts), `kiln_io_owner_command_read()` (blocks up to 200 ms on
  another task) and four interrupts-disabled heap walks at 20 Hz, all under a
  lock the LVGL task takes on every tick and touch — against the module's own
  documented invariant. Reads moved outside the lock and throttled to 1 Hz,
  policy now reads a cached snapshot; five mutation tests, all red.
- ~~**The LCD back buttons do not work**~~ **CLOSED (`1982ed6`).** Root cause
  was the topbar's z-order-first-match hit test: icons are built left-to-right
  (Back, Home, Prev, Next, Gear) so every icon except the last in a row was
  shadowed by whichever came after it, and Back was *always* shadowed since
  something always follows it. Fixed by capping the touch-area extension at
  `UI_THEME_PADDING_PX/2` per side (`ui_theme_apply_touch_area()`) and
  registering the icon row as a touch group (`ui_topbar.c`).
- ~~Diagnose the HTTP concurrency reset~~ — **stale, corrected 2026-09-04: this
  shipped and was already moved out.** Root cause was
  `CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE` defaulting to 6 — a fixed-size mailbox, not
  a heap failure or the backlog/socket-table limits three prior passes chased.
  Raised 6→16; reset rate 22.5%→0.0% at the same concurrency levels. This
  bullet itself was left behind when the finding moved to
  `docs/COMPLETED_2026-09.md#http-connection-resets-under-concurrency--root-cause-and-fix-2026-09-04`
  on 2026-09-04 — the upkeep rule says a finished item leaves this file, and
  the one-line pointer was missing until now.
- ~~A guard that every `src/**.c` is in its CMakeLists or explicitly
  excluded.~~ **Stale, corrected 2026-09-03: this shipped**, predating this
  roadmap's last review — `tools/check_c_files_in_cmakelists.ps1`, wired
  into `run_repo_checks`/`run_all_checks.ps1`. `tick_timing.c` (the incident
  that motivated it, 2026-08-28: added to the host-test list but not
  `SaftyFW/CMakeLists.txt`, so the host suite compiled it happily while the
  real target link failed) is itself already fixed too.
- ~~**PID Expansion Plan Phase 3b — cross-zone coupling feedforward.**~~
  **Stale, corrected 2026-09-02: this shipped.** The coupling matrix persists
  per-zone (`coupling_coeff[]`, `zones_config_accessors.c`/
  `zones_config_json.c`, config store) and `zone_coupling_solve.c` /
  `profile_executor_feedforward.c` apply it as a full 3x3 directed matrix —
  the asymmetric-pair schema concern this entry raised is already resolved by
  storing a full row per zone rather than one scalar coefficient. A re-solved
  asymmetric matrix was adopted onto the board 2026-09-02 — see
  `PID_EXPANSION_PLAN.md` §2/§3.2 for the coefficients and the caveats.
- ~~**New, scoped but not yet in a plan doc: per-zone enable/disable**~~ —
  **stale, corrected 2026-09-03 (second pass): the feature already existed.**
  `zones_cfg_t.thermo_count` is already exactly this: a contiguous-prefix
  `[0, thermo_count)` zone count, validated by `zones_config_json_validate()`
  and `parse_zone_fields()`. A zone dropped by shrinking the count keeps its
  stored config rather than losing it, so raising the count restores it.
  `profiles_http.c`'s `valid_zone_bits` already stops any profile targeting a
  zone outside the prefix, and guards, autotune and the coupling matrix are all
  already bounded by the same value. `zones_post_handler()` already refuses a
  structural change (409) while a firing is RUNNING/PAUSED **or** while any
  affected zone is still hot or has a relay commanded on — a stronger interlock
  than driving the relay off at toggle time.
  The scoping note above was doubly stale: `ZONES_CFG_VERSION` is at **15**, not
  11 — five further migrations have landed since that prerequisite was written.
  **Shipped 2026-09-03:** `zones_page.html` now offers per-zone checkboxes as
  UI sugar over `thermo_count` (checking zone *i* sets the count to *i+1*,
  unchecking sets it to *i*), so the contiguous-prefix rule holds by
  construction instead of relying on the operator editing a number by hand.
  No config-version bump and no migration were needed, and adding a second
  parallel "enabled" field would have duplicated `thermo_count`'s meaning
- ~~**New: Pico rollback from the OTA page**, plus a link-protocol reply
  frame so a Pico rollback refusal is visible~~ — **stale, corrected
  2026-09-03: this shipped.** `kilnlink_rollback_result.h` is exactly that
  reply frame (`link_task.c`'s `link_task_handle_rollback()` sends it instead
  of the old fire-and-forget path), and `ota_http.c`'s
  `ota_pico_rollback_post_handler()` / `ota_pico_rollback_status_get_handler()`
  drive it from `POST /api/ota/pico/rollback`. Not yet exercised against a
  live mismatch (M8)
- ~~**New: safety processor build identity** (commit + build date) shown on
  the OTA page~~ — **stale, corrected 2026-09-03: this shipped.**
  `saftyfw_build_info.h` is regenerated every build (`CMakeLists.txt`'s
  `saftyfw_build_info` target) and reported over the link; `dashboard_http.c`
  exposes `safety_build_commit`/`safety_build_datetime`/`safety_build_dirty`
  and `ota_page.html` renders them

**Closed 2026-08-27 through 2026-09-04, no longer open** — DRAM/stack
reclamation, PID Cohen-Coon/fuzzy-layer landing, SNTP sync, the first
hardware coupling-matrix/RGA measurements, the withdrawal of coupled/Ki
adaptive-tuning hardware clearance (harvest-layer defect), the shelving of
dynamics-from-ramps, ramp assist landing end to end (default OFF, gated on a
real cone-temperature firing — see the GATED table below), a round of
relay-autotune/measurement-tooling/web-UI hardening, and the fuzzy-PID A/B
campaign's discovery-and-fix-and-second-discovery (`8906686` then `778ad64`,
see [Blocked on you](#blocked-on-you--nothing-in-the-code-can-answer-these)
for the resulting owner decision) plus the 2026-09-04 safety-case/guard-
coverage hardening pass (27 payload-decoder fuzz targets, S3/S4/S10/S9
coverage, `SAFETY_CASE.md`/`SAFETY_MODEL.md` corrections, and
`wire_protocol_fingerprint_check.py`). Full postmortem detail for all of the
above:
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#what-is-actually-left-closeout-narratives-moved-2026-09-04).

**What is currently GATED, and on what** (the short answer for planning):

| Item | Gated on | Where |
|---|---|---|
| DRAM/PSRAM allocator-threshold work | A full soak (cold firing through cooldown) plus a Pico OTA relay-path measurement that has never been taken | `DRAM_PSRAM_PLAN.md` §5/§6/§9 |
| Second LCD panel (ST7796/MSP4031) | The physical panel, and its pre-power STOP-block 5V I2C hazard check before the module ever touches J2 | `DISPLAY_ST7796_PLAN.md` §0/§4 |
| Ramp assist default (OFF → ON) | A real firing at cone temperatures — everything measured so far is bench-range (0–80 °C), well below where the cone table's heat-work weighting matters | `PID_EXPANSION_PLAN.md` §7 |

**Two operational facts a future session must not miss (verified by READING
THE BOARD, 2026-09-04 — not inferred from commit messages):**
- **Both zone-config migrations are ALREADY on the live board and have already
  run against its real config.** `GET /api/zones` returns
  `ease_off_window_mult: 2.0` and `approach_rate_cap_c_per_hr: 0.0` on all
  three zones; `GET /api/status` reports `fw_build: Sep 4 2026 14:53:53`.
  This was NOT deliberate: an agent authorised to flash while diagnosing a
  watchdog reset built from a shared working tree carrying another session's
  in-progress schema work, and the v16→v17→v18 chain rode along. It landed
  correctly — v16→v17 carried the prior global `ease_off_window_mult` of 2.0
  verbatim to every zone (the intended default; had it been left at 3.0 by the
  withdrawn A/B, every zone would silently have inherited 3.0), and v17→v18
  added the cap at 0/uncapped. PID gains, plant models and the 80 °C ceilings
  are intact. Note `d800a60`'s own commit message says it stayed unflashed —
  true of *that agent*, and wrong about the board. **Read the board.**
- **The accidental flash briefly broke every preset apply**, because the
  firmware emitted `approach_rate_cap_c_per_hr` while `zones_http_client.py`
  had no POST mapping, so `load_config_preset` refused rather than risk
  silently zeroing it — correct behaviour, and the reason the board still sits
  on the `fuzzy_ab_strength50_20260903` arm preset rather than the baseline.
  The mapping landed in `d800a60` (`z%u_approachratecap` /
  `_PRESET_ZONE_OVERRIDE_FIELDS`), so the tree is whole; a long-running MCP
  server started before that commit will still refuse until restarted. The
  trap for the next flash: never flash a build carrying the GET half without
  the matching client POST half. `7afd2e6`'s flash-provenance guard now
  refuses when the dirty tree touches config-schema files, which is what
  should have stopped this.

**What is done and should not be reopened:** the link itself, the wire
contract and its two independent version numbers, the PC-link acknowledgement
convention, every guard's input plumbing, and the instrumentation that now
reports DRAM, stack and link health. See the decisions table at the foot of
this file before re-litigating any of them.

---

## M0 — Unblock the link · *the only milestone with no alternatives*

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 0.

- [x] **Link confirmed working end-to-end (2026-08-23).** The bench
      measurement found the byte actually died on baud mismatch, not idle
      level, framing or opto polarity: the TCMT1109 optocoupler pair then
      fitted could not switch fast enough for 115200. Walking the rate down
      settled on 9600, hardcoded on both sides at the time, and
      `safety_get_status()` returned live telemetry. **That optocoupler pair
      was replaced by a non-inverting digital isolator (U6) on 2026-08-25;
      the 9600 figure was a property of the retired parts, not of either
      firmware, and the baud sweep is now complete — see
      `KILNCTL_SAFETY_BAUD_RATE` in `KilnFW/App/drivers/Kconfig` for the
      committed value.** See `firmware/KilnFW/docs/SAFETY_LINK.md` "Transport"
      and `firmware/SaftyFW/docs/HARDWARE.md` §1.
- [x] Tier 0 pin test settles ESP TX/RX by measurement (`HARDWARE.md` §1) —
      moot as a separate step: the Pico is attached and the link carries live
      telemetry both ways (`safety_get_status()` returns fresh frames,
      `tx_dropped 0`), which settles TX/RX by observation rather than by
      probing pins
- [x] ESP TX/RX GPIO assignment and pull-up fixed in code, docs corrected,
      `UART_PROTO_MSG_BROADCAST` added, stale netlist removed, bench path
      decided (Debug Probe SWD + UART bridge on GP16/GP17) — all 2026-08-16
- [ ] DEBUG header and GP16/GP17 access provided before A1 is soldered down

System-wiring decisions gating any bench trip test (not firmware work) are
recorded in the decisions table below, with reasoning in
[`firmware/SaftyFW/docs/HARDWARE.md`](firmware/SaftyFW/docs/HARDWARE.md) §3/§5.

## M1 — Tooling that makes everything after it cheaper

Owned by [`tools/PcTools/TODO.md`](tools/PcTools/TODO.md). Worth doing early precisely
because it is what turns later hardware questions into a script instead of a
soldering session.

- [~] **Known-good config presets, so a test always starts from the same
      board** (owner request, 2026-08-28): factory-default then load a named
      config as one step, so a run is reproducible instead of depending on
      whatever the last session left behind. Presets live as DATA under
      `tools/`, never compiled into firmware — the bench fixture's 80 °C
      ceilings must never be capable of being left behind in a real kiln build.
      **No longer scoped to KilnFW-side config:** the safety-parameter write
      path is trustworthy now (SET_PARAM/COMMIT_CONFIG confirmed by a live
      read-back), so a preset carries a `"safety"` section written over
      `POST /api/safety/commissioning` by `safety_cfg_http_client.py`, with
      `load_config_preset(safety_host=...)` reaching it. Applied for real to
      the bench 2026-08-28: `tc_source`, `tc_placement_mode`,
      `abs_max_temp_c`, `mains_voltage_v`, `tc_type`, `max_rate_c_per_min`,
      `borrowed_zone_index` all committed and confirmed by read-back.
      **What is left is hardware, not software:** `ct_channel_map[0..2]`
      states which relay each CT is clamped around, and no CT is fitted on
      this bench (M5), so it has no measured truth. Committing a guess would
      clear `calibration_missing` and make the safety processor report itself
      COMMISSIONED on a mapping nobody verified, so the assumed identity map
      sits in a separate `"safety_ct_channel_map_backup"` preset section that
      applies only on an explicit `use_ct_map_backup=True`. Readiness still
      reports "Safety processor commissioned" as not-done, correctly, until
      the CTs are fitted and the zone current-sweep derives the real map
- [~] **Live-bench regression suite built on that preset** (2026-08-28):
      `tools/PcTools/tests/bench_fixture_session.py` +
      `tests/conftest.py` turn "start from `bench_fixture.json`, confirmed
      loaded" into a pytest fixture, and
      `tests/test_live_bench_firing.py` runs a real, bounded (45 °C, ≤80 °C
      ceiling checked four independent ways) firing attempt through the same
      `POST /api/profile_exec/start` the dashboard's Start button uses. Ran
      green against the live bench 2026-08-28, 4/4. **What it proves today:**
      the start endpoint accepts (the gate is downstream of
      `profile_executor.c`), and with the safety processor not commissioned
      no relay ever energizes and `safety_heating_enabled` stays false — a
      regression test for `commissioning_gate.c` working, not for it
      existing. The test branches on the board's own live verdict, so the day
      the CTs are fitted and the sweep commits a measured `ct_channel_map` it
      begins exercising the real heat path with no edit. Skips entirely
      unless `KILNCTRL_BENCH_HOST` is set; no mock stands in for the board
- [x] Live-bench **step and PID-tuning** regression tests
      (`tools/PcTools/tests/test_live_bench_tuning.py`), on the same harness —
      green against the live bench 2026-08-28, 4/4 in 6m10s. A closed-loop
      setpoint step (35 → 45 °C on zone 0) with the PV trace sampled at 2 s,
      and the open-loop step autotune (`autotune_engine.c`'s
      `AUTOTUNE_METHOD_STEP`) driven end to end through
      `/api/autotune/{start,abort}` under a 300 s budget. **What it proves
      today:** the start is accepted, the engine steps, and with heat refused
      PV is flat (−0.03 °C over 61 s, no relay ever closed) and the engine
      aborts itself with *"response too small to fit (trace flat or
      noise-dominated)"* — the correct verdict, said out loud. Also a
      negative test that a second concurrent autotune is refused
      (`begin_run_locked()`). `/api/autotune/accept` is never posted: a
      regression test must not retune the bench.
      **Superseded 2026-08-29 — the flat trace had two causes, and both are
      now fixed.** The first was K4: nothing in the firing path ever asked
      for heat enable (`heat_enable.c`, above). The second was found the
      moment the first was: zone 0's `heater_window_ms` was 2000 ms against
      the 10 s minimum on-time, so `heater_output_duty()` quantized every
      on-time a duty could compute to OFF. Autotune commanded `step_duty
      0.4` for 40 minutes and the relay never closed once — the "response
      too small to fit" verdict was correct about the trace and said nothing
      about why it was flat. With the window at 60 s the same run measures
      relay 1 closing for **24.0 s of a 60.9 s period (duty 0.394 against a
      commanded 0.40)** and PV climbing ~2 °C/min — and the step **completed
      with a model for the first time**: `state=done`, `model_valid=true`
      after 390 s / 39 samples, PV 31.9 → 45.0 °C, fitted `K = 32.95
      °C/duty`, `tau = 166.9 s`, `dead time = 36.9 s`, SIMC proposal
      `kp = 0.0343 / ki = 0.000206 / kd = 0.633`. The gains were **not**
      accepted, on this bullet's own rule. This bullet's "what it proves
      today" describes the old, heat-refused behaviour and is superseded by
      the run above. The relay-feedback method is
      **not runnable on this fixture at all** —
      `AUTOTUNE_RELAY_SETPOINT_HEADROOM_C` (50 °C) under an 80 °C ceiling
      admits only setpoints below the bench's own 35 °C ambient
- [x] **Live-bench verification pass, 2026-08-29** — step tuning, PID tuning,
      zone interaction and a full profile firing, each turned from an
      observation into an assertion, plus the cooldown gate that lets them run
      back to back honestly.
      **The gate:** `BenchSession.wait_for_cooldown()` +
      the `cold_bench` pytest fixture. The target is derived from the board's
      **lowest live cold-junction reading** plus a tolerance, not hardcoded —
      the room here runs around 100 °F and a 25 °C gate would never open. It
      raises on budget expiry rather than proceeding onto residual heat, and
      its policy is split into two pure functions with negative tests
      (`tests/test_cooldown_policy.py`): no cold junction refuses rather than
      defaulting, and **NaN is not cool**. Deliberately *not* an MCP tool — a
      multi-minute blocking wait does not fit `kiln_batch`'s one-round-trip
      contract. Measured in use: 50.4 → 37.5 °C in 646 s.
      **Two harness defects found by asserting instead of observing.** The
      step test's profile carried a 1-minute dwell against a 900 °C/h ramp, so
      the run ended at ~105 s with PV at 38.6 °C and the remaining ~380 s of
      the sweep sampled a *cooling* jig — the trace read 0.38 °C/min, four
      times too slow, for a reason that is not the plant. And
      `sample_response()` indexed channels positionally in a list already
      filtered to valid ones, which would have relabelled every channel after
      a gap — the exact failure that turns a cross-zone measurement into
      fiction. Both fixed; every sample now carries `channels_c` for all
      zones.
      **Results.** Step response from an enforced cold start: PV
      37.51 → 44.37 °C over 487 s, executor `running` throughout, relay 1 and
      K4 closed, **driving-phase rise 1.82 °C/min**, arrival at t=135 s,
      worst post-arrival deviation 4.48 °C. Autotune: `state=done` at 450 s,
      **K = 31.36 °C/duty, τ = 184.7 s, L = 46.1 s**, SIMC
      `kp = 0.03196 / ki = 0.00017 / kd = 0.73614` — an independent second fit
      agreeing with the first (32.95 / 166.9 / 36.9) to within 5 % on K, which
      is the first time this board's plant model has been *reproduced* rather
      than merely measured. Gains still not accepted.
      **Zone interaction — the RGA's first run on real data.**
      `autotune_engine.h` said "no input cell has ever been filled on hardware
      ... this has never run on measured data"; it has now. Two cold-start
      autotune runs (zones 0 and 1, same power cycle — the matrix is RAM-only)
      filled `K = [[30.181, 5.519], [11.822, 23.266]]`, and the board computed
      `Λ = [[+1.1024, −0.1024], [−0.1024, +1.1024]]`, det 636.9. Every row and
      column sums to 1.0000 — Bristol's identity, which is what the test
      asserts, so it is a check on the implementation rather than on a number
      someone typed. **Verdict: the loops interact mildly and independent
      per-zone PID is legitimate on this jig.** Raw thermal coupling, measured
      alongside and **asymmetric**: firing zone 0 raises ch1 by 17.5 % and ch2
      by 8.7 % of its own rise; firing zone 1 raises ch0 by **43.5 %** and ch2
      by 17.7 %. Zone 1 leaks into zone 0 about 2.5× as hard as the reverse,
      which matters for any multi-zone schedule on this enclosure.
      **Superseded 2026-08-30 by a full 3x3 coupling matrix and RGA over all
      three zones, and again 2026-09-02 by a re-solved matrix** — current
      numbers live in `firmware/KilnFW/docs/PID_EXPANSION_PLAN.md` §2, not
      restated here.
      **Full multi-segment profile.** 42 °C dwell 6 → 52 °C dwell 6 → down-ramp
      to 46 °C, run end to end through `POST /api/profile_exec/start`: all
      three segments entered in order, all dwelled, peak 56.30 °C, K4 closed on
      **112/113 samples**, no heat commanded during the down-ramp (PV
      56.16 → 54.18 °C with relays open), ramp-lock never engaged, and every
      relay plus K4 released by the completion path rather than by teardown.
      *(NOT a test of multi-zone ramp-lock coordination — that needs two zones
      in closed-loop control, and zones 1/2 are `control_mode 0` here.)*
- [x] **Guard 1 aborted a healthy firing for settling.** Found by the profile
      run above, on its second dwell: zone 0 holding 50.7 °C against a 52.0 °C
      setpoint at full duty — the steady-state offset any finite-gain PID
      leaves — and `thermal_guard.c` tripped with *"heating but rose only
      −0.2C in 1min"*, aborting the whole firing. Guard 1 treated `error > 0`
      as "still climbing toward setpoint"; those are different questions, and
      once a loop arrives, high duty + positive error + not rising **is** the
      correct state. Every sufficiently long dwell on a sufficiently lossy
      zone would eventually abort. Fixed with an arrival band
      (`progress_band_c`, default 3 °C): outside it guard 1 is unchanged, so a
      dead element on a ramp — errors of tens of degrees — is still caught;
      inside it the zone must not *fall*, which is guard 2's rate test applied
      to a case that previously had no test at all. Five host tests, proven
      load-bearing by forcing the band to 0 (two fail, both pass at 3).
- [x] `pc_tools` moved to `tools/PcTools/`; GPIO probes built for both chips
      (ESP: deny-list incl. GPIO6; Pico: over SWD, GPIO6 read-only). **Pico
      probe bench-tested 2026-08-19, PASS.** ESP probe bench-tested 2026-08-19
      but blocked: the PC↔ESP command UART (COM9) is unresponsive independent
      of the M0 isolated link — see bench state below
- [x] Coordinated two-board test script (`tools/PcTools/scripts/
      coordinated_gpio_test.py`) — reaches each board by a path independent of
      the link under test; run 2026-08-19 confirmed the Pico half works and
      the ESP half hits the same COM9 fault above
- [x] OpenOCD wrapper covering both chips (program/reset/halt/read/write) —
      2026-08-17, `kilnctrl.debug_probe`
- [x] Per-processor console capture + interleaved log file
      (`kilnctrl-console-capture`) — host-verified only; the SAFETY log-relay
      wire path is still unimplemented in firmware
- [x] **HW change: LCD backlight control — CLOSED 2026-09-04 (`be02d34`).**
      Flying wire (GPIO15 → module pin 8) is fitted, owner-confirmed by
      meter; `KILNCTL_BACKLIGHT_PWM_ENABLE` now defaults on and ON duty
      comes from the operator's brightness setting. Not yet confirmed by
      meter/eye that the panel actually dims — that's the next check, not a
      firmware gap. Full buildup history in `docs/COMPLETED_2026-09.md`.
- [~] **Second LCD panel (ST7796/MSP4031), auto-detection, display SPI
      async/DMA.** `firmware/KilnFW/docs/DISPLAY_ST7796_PLAN.md`, sequenced
      Phase 0 (bench facts/hazard measurement) through Phase 7 (UI). Phases 1
      (single-owner cleanup, bounded SPI-owner timeout — also closes
      `TODO.md`'s spi_owner unbounded-wait entry), 2 (panel codec extracted),
      3 (`panel_spi`/`st7796_panel` split, Kconfig-selectable) and 5 (FT6336U
      touch abstraction, unvalidated on hardware) landed 2026-09-01/02.
      Phase 4 (auto-detection) is done ahead of hardware: the detection logic
      is host-tested and wired into `panel_spi_blit.c` (`panel_spi.c`'s
      2026-09-04 split, see the 1500-line-rule item below), and is correctly inert
      today because both descriptors' `id_matches` stay NULL — the ILI9488's
      Sec.4 RDDID bytes were captured 2026-09-03 (`0x00 0x00 0x00`, MISO
      undriven, i.e. permanently unmatchable) and the ST7796's have never
      been read since the module has not touched J2. Phase 6 (SPI DMA) is
      also done ahead of hardware: 9.2/9.5/9.9 landed; 9.3 (PSRAM DMA), 9.4
      (hardware CS), 9.6 (async flush) and 9.7 (zero-copy flush) landed
      compiled-in but default-OFF behind their own Kconfig symbols; 9.1's
      instrumentation landed (`flush_last_us`/`flush_max_us`/`flush_count` on
      `GET /api/status`) though 9.1b's number is not yet recorded live; 9.8
      needed no code. **The owner connected the module to J2 on 2026-09-04 and
      it is now the panel in service** — the "has not yet touched J2" caveat
      above is superseded for everything except the STOP-block 5V I2C hazard
      measurement, which was never taken. Bring-up that day:
      `KILNCTL_DISPLAY_PANEL` was still pinned to ILI9488, so the board ran the
      wrong init table against real ST7796 silicon; and `main.c` only ever
      constructed an `NS2009Class`, so the fully written, host-tested FT6336U
      driver was never instantiated — a textbook consumer-without-producer.
      Both fixed (`16fe9ed`); touch answers at I2C 0x38. The real colour bug
      was neither inversion nor MADCTL: `panel_codec.c`/`panel_spi.c` sent
      LVGL's RGB565 little-endian where MIPI-DCS RAMWR wants MSB-first, and the
      code's own comment had the wire order backwards (`8174db5`). Every
      INVON/BGR experiment run before that fix was confounded and is not
      evidence. With byte order correct, MADCTL was re-measured and set to RGB
      `0x00` (`f493bf8`) and INVON is deliberately absent (vendor
      transcription, guarded by `test_st7796_panel.c`). Touch axis mapping now
      lives on `touch_dev_t`, so panel and touch controller select
      independently (`KILNCTL_TOUCH_FT6336U`). ST7796 RDDID reads `0x00 0x00
      0x00` like the ILI9488 — MISO undriven — so `id_matches` stays NULL
      permanently on both and Phase 4 is inert by design, not by omission.
      **Still open:** a residual blue bias, with every firmware cause
      eliminated by measurement (camera response refuted by a neutral
      off-screen bezel sample; RGB565 field boundaries unit-tested against
      known values; LVGL double-swap ruled out; blit paths compiled out; SPI
      clock swept 20/15/10 MHz with the blue *fraction* flat at 0.62/0.61/0.58
      — `07cad60`, table in `DISPLAY_ST7796_PLAN.md` §4). Treat as a module
      characteristic needing the owner's eye, a colorimeter or a second unit,
      not more firmware. Touch corner accuracy CLOSED 2026-09-04 (`f028e2f`
      — Y was mirrored, `KILNCTL_TOUCH_CAP_INVERT_Y` now defaults on).
      Still open: the 5V I2C hazard measurement. Rendering can now be
      checked without a person at the bench
      via `tools/PcTools/scripts/capture_lcd.ps1` (`5fd9761`) — sample pixels
      numerically, never by eye, and always include an off-screen reference.
      The only hardware change required remains a custom
      harness; the main board itself needs no modification
- [ ] **HW change: relay status LEDs** for K1–K4, S9
- [ ] **HW change: distinct connector types** for the thermocouple daughterboards
      vs. main-board connectors
- [ ] **HW change: I2C broken out on an expansion connector** (owner request,
      2026-08-28). For a future board revision, not the current one. Worth
      deciding alongside it: whether the expansion header carries power and at
      what rail, and whether the bus is the same one the SX1509 and the
      MCP23017 expanders sit on or a separate segment — an expansion connector
      that shares the relay expander's bus lets an add-on wedge relay control

**Bench state (2026-08-20):** ILI9488 LCD, ESP32-S3 JTAG, and Pico SWD all
verified working. Three MAX31856 ICs + thermocouples now fitted on the ESP32-S3
board (channels 0/1/2 reading correctly, `thermo_owner.c` unblocked); the
safety processor (RP2040) still has none fitted.

**Bench state (2026-08-24):** both UARTs work — the two blockers named above
are cleared. The isolated Pi↔ESP link is up and carrying telemetry at 9600
(M0; that figure was specific to the optocoupler pair fitted at the time and
does not describe the digital isolator that replaced it on 2026-08-25 — see
M0's note), and the PC↔ESP command UART on COM9 is responsive. Both firmwares were
flashed over JTAG/SWD today and verified running.

**Later the same day the safety processor's MAX31856 and thermocouple were
fitted** (this paragraph's earlier revision said "still not populated", which
was true when written and stopped being true a few hours later — the two
statements are hours apart, not a contradiction). Verified live:
`safety thermocouple valid | 30.20 C (CJ 28.08 C)`. It required a Pico reset,
because SPI init runs once at boot. The E-stop net measures low (a contact IS
fitted, contrary to `HARDWARE.md` §5's older note) and S7 is now genuinely
evaluated rather than masked by S5 — and correctly stays quiet, which is the
first real test of the `discrete_task.c` polarity fix (`642dd54`): with the
old inverted read this healthy board would now be latched on S7.

## M2 — `CommonFW`, before either firmware depends on it · *done, 2026-08-19*

Owned by [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md). `kilnlink` builds
under both pico-sdk and ESP-IDF, both firmwares consume the same codecs for
every `LINK_PROTOCOL.md` sec 4/6 command, `pc_tools` cross-checks the same
byte vectors, and a CI grep (`tools/check_no_duplicate_crc.ps1`) keeps a
second CRC/framing implementation from reappearing outside `CommonFW`. Detail
in `firmware/CommonFW/README.md` and `docs/LINK_PROTOCOL.md`.

## M3 — Safety processor to first trustworthy reading

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 2–4. Independent of the
link, so it can run in parallel with M1 and M2 once M0 is out of the way.

- [x] FreeRTOS SMP skeleton, GPIO6 driven low first, watchdog with latched trip
      reason — all done 2026-08-16, build-verified
- [x] **The SAFETY processor's MAX31856 and thermocouple are now fitted**
      (2026-08-24, by the user). Verified on hardware the same day:
      `link up; safety thermocouple valid | 30.20 C (CJ 28.08 C)`, status
      flags `0x21` = `LINK_UP | TEMP_VALID`. This unblocked the real-reading
      work below it and immediately changed guard reachability — S5 (sensor
      invalid) stopped latching, which had been masking every later guard
      because `safety_guards_tick()` early-returns while any trip is latched.
      **Bring-up trap worth keeping:** the IC must be present BEFORE the Pico
      boots, since its SPI init runs once at startup. Fitted under power it
      reads `safety TC invalid` with nothing pointing at the real cause; a
      `debug_reset` over SWD is the fix.
      **And it exposed a decision that needs you** — see
      `firmware/SaftyFW/TODO.md`, "An uncommissioned safety processor grants
      heating enable": with the sensor real and the stale S5 latch cleared,
      the board granted heating enable while still reporting
      `commissioned: false`, i.e. with S1's absolute temperature ceiling
      disabled for want of `abs_max_temp_c`
- [ ] ~~The SAFETY processor's MAX31856 is not populated~~ — superseded by the
      line above. Kept for one revision so anyone mid-task on the old wording
      sees why it changed.
      **Read the word "safety" carefully** — this item is about the RP2040's
      own thermocouple, not the main board's. `KilnFW`'s three channels ARE
      fitted and working: verified 2026-08-24 with all three reading ~35 °C,
      `SR 0x00`, and open-circuit detection confirmed genuinely enabled
      (`CR0 = 0x90`, so `OCFAULT[1:0] = 01`) — which is what makes "no fault"
      mean "a thermocouple is attached" rather than "detection is switched
      off". An absent TC on those channels would fault. The two sets of
      thermocouples are easy to conflate from this line alone, and doing so
      leads to "correcting" a true statement
- [~] MAX31856 driver + config plumbing (tc_type via flash-backed
      `config_store`, commissioned over `SAFETY_CMD_SET_CONFIG`) built and
      wired end-to-end in code (2026-08-19). ~~The part itself is not
      physically populated~~ — **fitted 2026-08-24 and reading correctly.**
      **Still open**: there is no LCD/web commissioning surface yet, and the
      four no-default section-1 fields remain unset, which is what keeps
      `commissioned: false` and leaves S1's ceiling disabled
- [x] 13 of 13 guards (`SAFETY_MODEL.md` §4) implemented as pure functions and
      host-tested against synthetic inputs. **S8 (rate-of-rise), the last
      holdout, gained its pure-module implementation and integration on
      2026-09-03** (`safety_guards.c`'s S8 block, `safety_core_load_guard_cfg()`
      wiring gated on `CONFIG_STORE_SET_MAX_RATE_C_PER_MIN`, `test_s8()` +
      `test_safety_core_s8_wiring.c`, `SaftyFW/docs/GUARD_TEST_MATRIX.md`).
      It ships deliberately off — `max_rate_c_per_min` defaults to 0.0f,
      the same "no default by design" shape S1 uses for `abs_max_temp_c` —
      so **commissioning `max_rate_c_per_min` is now what enables it**,
      superseding the earlier finding here that the field would enable
      nothing. **Input wiring now complete (2026-08-24):**
      `safety_core_build_input()` populates every field the guards read —
      `context_valid`, `any_current_present`, `relay_commanded_recently`/
      `_continuously`, `zone_count`, the setpoint/measured reductions,
      `sample_counter_advancing`, both discretes and `reboot_grace_active`.
      The older "only S5/S6b/S7/S12 are reachable, the other 8 have no
      producer" finding is superseded: what still holds a guard dormant is a
      missing **commissioning value** (S1's `abs_max_temp_c` defaults to 0 =
      never trip; S13 needs `tc_source`/`borrowed_zone_index`), which is a
      different kind of gap from a missing producer. Two real producer bugs
      were found and fixed on 2026-08-24 — the E-stop polarity was inverted
      (S7 could not fire) and `current_sense_set_cal()` was never called, so
      `any_current_present` was permanently false (S3/S9/S11 and S6b's
      current-gated trip could not fire). ~~The reachability count in
      `GUARD_TEST_MATRIX.md` predates both fixes; re-establish it rather than
      trusting the old number.~~ **Re-established 2026-09-03**
      (`GUARD_TEST_MATRIX.md` §6c): old count was 6 of 13 structurally
      reachable (computed before either fix and before S8 existed); S8
      itself gained its implementation and integration on 2026-09-03 and is
      now the same class as S1/S13 — implemented and integrated but
      deliberately configured off pending commissioning, not unreachable.
      See `SaftyFW/docs/GUARD_TEST_MATRIX.md` for the current reachable-count
      recomputation (S14 is new since 2026-08-28 and tracked separately). S3,
      S6a, S7, S9 and S11 moved from blocked to reachable — S6a's own
      `main_fault_asserted` wiring is a third fix in the same window, beyond
      the two named above. S1, S13 and S14 remain deliberately blocked by
      commissioning gaps, not producer bugs — §6c distinguishes that from an
      unreachable guard explicitly. One stale claim inside the matrix itself
      was also caught and corrected in the same pass: S13's row said
      `sample_counter_advancing` had no producer at all; it does now
      (`safety_core.c:957-960`), and `tc_source` alone is what still blocks
      S13. Host suite re-run in the same pass: 1983/1983 checks pass (the
      matrix's old "452/452" checklist line was itself stale, from before
      the suite grew ~4.4x).
- [x] CI grep: `safety_core.c` never includes the link header — 2026-08-16,
      `firmware/SaftyFW/tools/check_isolation.ps1`

## M4 — Relay authority

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 5. First milestone that can
physically stop a kiln, and the first that can nuisance-trip one.

- [x] Relay owner task is the sole writer of GPIO6 — confirmed by whole-tree grep
- [x] Trip latches and requires an explicit `CLEAR_TRIP` to clear, refused if
      the tripping condition re-fires on a one-tick retest
      (`safety_guards_try_clear()`); wired end to end PC→ESP→Pico and
      ESP-side send (`safety_link_send_clear_trip()`) plus web/UART surfaces —
      all built and host-tested (2026-08-19). **Known scope limit** (documented,
      not a bug): a graduated/windowed guard's elapsed-time accumulator resets
      on clear, so it re-trips on its own timescale rather than instantly.
      **Hardware-verified 2026-08-23/24**: the physical link has carried real
      CLEAR_TRIP frames. Refused correctly against a latched S5 with the
      safety thermocouple genuinely absent, accepted once the tripping
      condition cleared, and the board never rebooted in either case — which
      it originally did, via a `log_task` stack overflow on the refusal path
- [x] **K4 now has a real energize path.** The "zero callers" finding of
      2026-08-20 is stale: `safety_core_request_enable()` calls
      `relay_owner_command_energize()` (`safety_core.c`), reached from
      `link_task`'s enable handler, and it refuses ON when the safety
      thermocouple is declared absent or a trip is latched.
      **Enable is now GRANTED on real hardware (2026-08-24)** — the
      thermocouple is fitted, the stale S5 latch cleared, and the board
      reports `heating enable granted`. ~~The bench cannot demonstrate a
      genuine heat-enable until the safety thermocouple is populated~~ — that
      blocker is gone.
      **Observed closing, 2026-08-29.** Not with a meter and not with an LED
      — with the element. Until this date nothing in the normal firing path
      ever sent `SAFETY_CMD_REQUEST_ENABLE` at all: `profile_executor.c` and
      `autotune_engine.c` had *zero* calls to `safety_link_request_enable()`,
      so every firing and every autotune this firmware ever ran closed K1 and
      left K4 open. That is what "40 minutes of commanded heat moved this jig
      0.67 C" was actually measuring. With `heat_enable.c` wired in, the same
      jig on the same profile went 32.1 → 47.5 C, `safety_relay_energized`
      true on every poll of the run and false again the moment it stopped.
      A contact that passes enough current to move a thermocouple 15 C is
      closed. What is still unobserved is the *mechanical* state under a
      fault — a welded contact reading closed while the request is released
      — which is M1's LEDs, not this.
      And note what granting it exposed —
      `SaftyFW/TODO.md`'s "An uncommissioned safety processor grants heating
      enable": permission is given while S1's absolute ceiling is disabled for
      want of `abs_max_temp_c`
- [x] Rule engine drives relays through the existing owner arbitration —
      `rules_task.c` claims `RELAY_OWNER_RULE` and never writes the SX1509
      directly, so precedence is PROFILE/AUTOTUNE > RULE > MANUAL. Fails safe
      on safety fault, down link, OTA in progress, and on its own stale-tick
      watchdog. **Rules may never command a zone-assigned (PID/thermocouple)
      relay** — owner's scope rule, enforced in the evaluator, the task and
      the POST handler; heater relays stay readable as rule conditions.
      2026-08-22, verified on hardware before the bench was disassembled.
      **The rules engine was deleted on 2026-08-28 (M11)** — this bullet is
      kept because the arbitration it describes is still live and is what
      firing-profile relay/IO segments now claim through. The owner's scope
      rule survived the deletion intact: a segment may not command a
      zone-assigned relay either
- [x] Dashboard/readiness no longer report the safety link as up merely
      because the driver object exists — both call sites now consult the real
      staleness-gated `link_up`. This was a live false positive: the board
      reported "safety=up" with the UART unplugged. 2026-08-22
- [ ] S9 trip-ineffective escalation proven with a deliberately welded contactor
      (hardware-gated). **Not a SimFW task, checked 2026-09-03**: SimFW is gone
      (removed `8553244`); even the tool it was checked against for this exact
      question required "physically driv[ing] current into the CT," per that
      removal commit's own audit. S9's latch needs the CT's real analog
      current signal (see the M4 table row above for the code path), which
      only a hardware jig can supply, plus a CT fitted and commissioned
- [ ] Every guard exercised per
      [`GUARD_TEST_MATRIX.md`](firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md) —
      §2's host-provocation table is fully audited (452+/452+ checks pass);
      §3's hardware-trip rows remain open (no bench hardware attached)

## M5 — The link carrying real traffic

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 6–8, contract in
[`firmware/CommonFW/docs/LINK_PROTOCOL.md`](firmware/CommonFW/docs/LINK_PROTOCOL.md).

- [~] Current sensing: load-active detection and a power estimate (not an
      over/under-current trip) built and wired onto `SAFETY_CMD_POWER`.
      **2026-08-24: the calibration was never actually loaded** —
      `current_sense_set_cal()` had no caller anywhere, so `amps[]` was
      permanently 0 and `any_current_present` permanently false, silently
      disabling S3/S9/S11 and S6b's current-gated trip while S4 warned
      forever. Now loaded at boot and on every `COMMIT_CONFIG`, and presence
      detection is decoupled from `k_ct_v_per_a` (deciding whether current
      flows never needed a volts-per-amp scale). `CURRENT_SENSE.md` §5 and
      `CONFIG_REFERENCE.md` both claimed a wrong `k_ct_v_per_a` could not
      affect guard behaviour; that was false and is corrected. `context_valid`
      is also computed now — see M3's guard bullet. **Still open**: the
      per-channel CT-to-jack commissioning check on real hardware, and a bench
      measurement of the ADC noise floor to confirm the uncommissioned
      presence fallback margin (25 counts) sits above it
- [x] ESP → Pico context frames (`SAFETY_CMD_PUSH_CONTEXT`, incl.
      `relay_recent_mask`) built from live board state — 2026-08-18.
      **Hardware-verified 2026-08-24**: the Pico is on the bench, the link is
      up, and `context_valid` is computed from frames that actually arrive.
      The "no Pico on this bench" caveat this bullet used to carry is retired
- [~] Pico → ESP telemetry (status, diagnostics, firmware version, trip events,
      power) — all five frame types have working codecs, send paths, and
      `KilnFW`-side decode/dispatch, plus PC-facing `GET_DIAG`/`GET_TRIP_EVENT`
      subcommands (2026-08-18–19). **Now hardware-verified (2026-08-23/24)**:
      M0 is cleared, status/diag/power frames cross the real wire, and the
      diag/power applied counters were observed climbing at the expected
      0.5/s over a 90 s soak. Getting there needed a UART TX self-start fix —
      an edge-triggered TX interrupt that never re-armed stranded a frame in
      the ring silently, with no counter tripped
- [x] Pico never blocks on the link — all five no-wait rules audited clean
      2026-08-18 (no ACK/retry, bounded non-blocking TX, `safety_core.c` never
      calls into the link, correct task priority/core affinity)
- [x] Mutual version handshake: `ANNOUNCE_VERSION` both ways, `min_compatible`
      checked in both directions, both firmwares on the shared codec
- [x] TX ring reserves capacity for telemetry; log frames dropped above a 50%
      watermark and the drops counted
- [x] Borrowed-thermocouple staleness split correctly across S11/S13/S6 —
      audit-confirmed 2026-08-18, no code fix needed
- [x] `SAFETY_CMD_COMMIT_CONFIG_REJECTED` (0x20) — a refused commissioning
      commit used to be indistinguishable from an accepted one. The Pico now
      names the offending `param_id` and a reason code (RANGE /
      CONTRADICTION), and `safety_cfg_http.c` surfaces it in the page's error
      text instead of "sent, awaiting confirmation". 2026-08-22, host-tested
      both ends; not hardware-verified (M0)
- [x] `SET_LOG_LEVEL` (0x1B) reachable from the ESP — the codec and the Pico
      consumer existed with no caller, so the feature was dead. Now
      `POST /api/safety/log_level`, deliberately API-only (a bench knob, not
      an operator control). 2026-08-22
- [x] LCD stopped showing a raw `reason 0x%02X` where the web showed decoded
      words — the two same-language copies are one shared table
      (`safety_trip_words.h`). 2026-08-22

## M6 — Throw the liveness switch

The point at which the two processors become one system. Deliberately separate,
because it changes what a bare main board will do.

- [x] Link staleness → fault at a fixed 1.5 s ceiling, 30 s silence aborts a
      running firing, boot-time `FW_VERSION` request retried until answered,
      and a documented bench escape hatch (`safety_link_fault_on_link_loss`) —
      all built 2026-08-18/19. Code-verified and flashed. **The Pico is now on
      the bench (2026-08-24) so the "no Pico" caveat is gone, but the TIMING
      half is still unverified**: nobody has held the link down with a
      stopwatch to confirm the 1.5 s ceiling and the 30 s firing abort fire
      when they should. That is a bench procedure, not a code gap
- [~] GUI (web + LCD) surfaces safety temperature, enclosure temperature, and
      power — built and wired to the same status cache the wire frames land
      in. **The reason for the blanks changed on 2026-08-24 and the
      distinction matters**: frames now arrive every 500 ms, so this is no
      longer "no Pico has ever sent them". Safety temperature reads null
      because the safety MAX31856 is genuinely not populated (M3), and the
      three current channels read 0.00 A because no CT is fitted. Both are
      honest reporting of absent hardware, not a code gap and no longer an
      M0 consequence
- [ ] **AP-fallback fix unverified end to end** — needs a router with both
      correct and deliberately-wrong static config; see `KilnFW/TODO.md` Wi-Fi

**2026-08-20: a large batch of UI, Wi-Fi, and boot-stability bugs were found
and fixed during a full hardware test pass** — profile/readiness reporting,
the LCD no-scroll rewrite, captive-portal DNS hijack, gzip content
negotiation, the Digital Fire built-in schedules, a thermocouple-fault page,
web DHCP/static-IP toggle, and others. Full list, and the ongoing ledger, is
in `firmware/KilnFW/TODO.md` and `docs/UI_PLAN.md`; two hazards worth reuse
were promoted to the decisions table below (internal-SRAM exhaustion at task
creation, and the UART owner's per-transfer heap churn).

## M7 — Repo reorganisation · CLOSED 2026-09-05

Owned by [`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md). All items done,
including `hardware/UnitTestFixture` (owner decision: KEEP, 2026-09-05). Full
detail: [`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m7--repo-reorganisation-closed-2026-09-05).

## M8 — Field updates

Owned by [`firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](firmware/CommonFW/docs/UPDATE_PROTOCOL.md)
and [`firmware/SaftyFW/docs/BOOTLOADER.md`](firmware/SaftyFW/docs/BOOTLOADER.md);
tracked as `KilnFW/TODO.md` section 9 and `SaftyFW/TODO.md` phase 10.

Last, and genuinely last: a bootloader is new code in the component with nothing
behind it, and it is only defensible because SWD sits underneath as the recovery
path. Two facts set the shape of this milestone:

- **The RP2040 mask ROM has no UART bootloader.** Updating the Pico over the
  isolated link means writing one. It is written once over SWD and never
  updates itself.
- **You cannot OTA your way into being OTA-capable.** The ESP needs a new
  partition table with two app slots, and a partition table can only be written
  over a cable.

- [x] **Measure the isolated link's real error rate — done, and 115200 did
      not work at all under the old optocoupler pair.** The optocoupler pair
      capped the link at 9600 (see M0); that pair was replaced by a digital
      isolator (U6) on 2026-08-25 and the ceiling no longer applies, so the
      committed baud is being re-measured (`KILNCTL_SAFETY_BAUD_RATE` in
      `KilnFW/App/drivers/Kconfig`). The update transfer's error rate at
      whatever the current baud is, over a sustained multi-megabyte run, is
      still unmeasured. Retry cost is still 200 ms × up to 10
- [x] Real flash size established (N16R8, 16 MB/8 MB PSRAM) and declared in
      `sdkconfig` — 2026-08-17. **Both follow-ups are now done**: the
      two-app-slot table exists (`otadata`/`ota_0`/`ota_1`/`factory`/
      `pico_img`/`coredump`, and `factory` moved to 0x810000 on 2026-08-21),
      and the bootloader + partition table were flashed and verified on the
      physical board over JTAG on 2026-08-24. **Consequence worth knowing:**
      the sanctioned JTAG path writes the app into `factory`, so a
      bench-flashed build always boots `factory` and the rollback/boot-confirm
      machinery below never executes — it can only be exercised by a real OTA
      into `ota_0`/`ota_1`
- [x] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, app confirms itself only after
      NVS + safety link + web server are up — host-build-verified, not yet
      hardware-flashed
- [~] Pico flash layout/metadata frozen (reserved `signature[64]`, `sig_required`,
      pubkey region) and implemented in `metadata.c`/`flash_layout.h` — design
      and code done 2026-08-19; not flashed or hardware-verified (no RP2040
      attached)
- [~] Pico bootloader: GPIO6 low first, per-boot CRC, `boot_attempts` fallback,
      and a real recovery-mode UART1 receiver (not beacon-only) — all built
      and host-build-verified 2026-08-19; not flashed or exercised over a live
      UART1 link
- [x] Mutual protocol-version check landing with M5 — both firmwares on the
      shared `kilnlink_announce` codec, GUI shows both sides' versions and
      names which is older. Not verified against real mismatched hardware
- [~] Compatibility floor (frame ids `0x00`–`0x0F` reserved, never gated on
      `peer_version_compatible`) — frozen and implemented; not verified
      end-to-end against a live mismatch
- [x] Image header validated before the first erase (2026-08-17)
- [x] Challenge–response on the AP password, never crosses the wire, 3-failure
      lockout (2026-08-17)
- [x] Both update paths refused unless idle and cool, with the specific
      blocker named (2026-08-17)
- [ ] Link-loss heating block **not** bypassed during a Pico update — now
      pinned in CI on both sides (2026-09-04), still OPEN as a
      hardware-exercise item (a test suite is not a substitute for running a
      real update on a real board):
      - KilnFW side: `firmware/KilnFW/App/test/test_safety_link_compile.c`
        now links the REAL `relay_authority_on_blocked()` (App/drivers/
        relay_authority.c — previously stubbed everywhere else in the host
        suite) against a real `SafetyLinkClass`, and proves
        `safety_link_set_update_in_progress()` (the call `ota_pico_relay.c`'s
        relay task makes around a Pico relay) does not relax an asserted
        `SAFETY_FAULT_SRC_SAFETY_LINK` fault, and that a link going stale
        mid-update still denies heat. Negative-tested: temporarily made
        `safety_link_set_update_in_progress(true)` clear `fault_sources`,
        confirmed 4 checks fail by name, reverted.
      - SaftyFW side: `firmware/SaftyFW/test/test_update_task_relay_wiring.c`
        source-scans the real, non-host-compilable `update_task.c` (same
        precedent as `test_safety_core_s8_wiring.c`/
        `test_safety_core_polarity_wiring.c`) and fails closed if it cannot
        locate either file or `relay_owner_command_energize()`'s real
        signature; pins that `update_task.c` calls no relay_owner_* mutator
        and touches no relay GPIO. Negative-tested: added a call to
        `relay_owner_command_energize()` into `update_task.c`, confirmed the
        scan fails by name, reverted.
      - Still needed: an actual Pico OTA exercised on real hardware with the
        link deliberately dropped mid-update, confirming no relay ever
        energizes and the fault stays latched after the update ends.
- [x] Four MCP tools for OTA (challenge, ESP update, Pico update, status),
      plus explicit ESP and Pico rollback and a web `/ota` page — built and
      unit-tested against mocked HTTP (2026-08-18–19); not yet exercised
      against a physical board
- [x] `SAFETY_CMD_ANNOUNCE_REBOOT` sent before a routine ESP reboot so a
      firmware update doesn't trip S6(b) — 20 s grace window, host-tested;
      not hardware-verified (no board attached to confirm a real reboot
      suppresses the trip)

## M11 — The UI the owner actually asked for · *opened and CLOSED 2026-08-28*

Owned by [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) and
[`firmware/KilnFW/docs/UI_PLAN.md`](firmware/KilnFW/docs/UI_PLAN.md). **CLOSED
— all items landed and are hardware/migration-verified.** A batch of direct
owner requests that changed persisted data structures and deleted the rules
engine subsystem: relay control consolidated into the diagnostics Danger
Zone; board-health/thermocouple-fault pages folded into `/diagnostics`;
safety pages grouped into one nav group; shared per-zone timing profiles
(`ZONES_CFG_VERSION` 8→9); relay/IO segments added to firing profiles
(`PROFILE_VERSION` 2→3); the rules engine deleted in favor of segments; LCD
pages consolidated the same way, plus a planned-profile preview. Full
detail, including the durable `zones_cfg_t`/`profile_t` migration-hazard
note (persisted structs that embed arrays by value displace every element
after an insertion — read this before touching either struct again):
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m11--ui-consolidation-full-detail-moved-2026-09-04).

## M12a — The commissioning surface lied · *opened and closed 2026-08-28*

**CLOSED.** An opus audit found the safety commissioning page reported
`{"ok":true}` on writes the Pico had actually rejected (four independent
defects: an ACK that couldn't fail, a rejection race that got dropped, a
stale NVS cache presented as current, and an unconditional `"set": true`).
All fixed and hardware-verified the same day (`ddbd024`, `3149393`);
`abs_max_temp_c = 80` is committed and confirmed on the bench, and fixing it
exposed a second defect (config page 1 always timing out — the ESP had never
once fetched a config page in the project's history), also fixed the same
day. The write-window constraint this uncovered (config writes refused
outside the ~60s boot GRACE period) shaped M12's design below. Full
postmortem: [`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m12a--the-commissioning-surface-lied-full-postmortem--opened-and-closed-2026-08-28).

## M12 — Commissioning the operator can actually do · *opened 2026-08-28*

The owner answered six of the standing blocked-on-you questions in one message
on 2026-08-28. Most of the answers were not values to paste into a config —
they were *"the operator should be able to enter that"*, which turns a set of
questions into a milestone.

**The ordering constraint, and it is the important part of this milestone.**
The last item below makes an uncommissioned safety processor refuse heating
enable. Today the board grants enable while `commissioned: false`, and that is
the only reason the bench can heat at all. Land the refusal first and the bench
is locked out of heating until a full commissioning pass succeeds — including
`abs_max_temp_c`, which has no value yet. So the refusal goes **last**, after
the entry surface exists and a real commissioning pass has been completed on
the board. That is a sequencing decision, not a reason to soften the refusal.

- [x] **Kiln maximum temperature entered on the safety page**, and used to set
      `abs_max_temp_c` on the safety processor. Already landed — field id 260
      on `safety_commissioning_page.html`, `noDefault: true` so unset never
      reads as 0/no-limit (the M12a fix). Checkbox was stale; verified
      2026-08-28 by re-reading the page rather than re-implementing
- [x] **Thermocouple maximum inferred from the thermocouple type**, not
      entered. 2026-08-28: selecting `tc_type` now pre-fills `abs_max_temp_c`
      from `TC_MAX_C_BY_TYPE`, via `placeholder` rather than `.value` so the
      pre-fill is visible but never silently saved/committed on the
      operator's behalf — a real review finding on the first pass fixed
      before landing. `abs_max_temp_c` stays ASKED and independently
      editable per the spec (it's the kiln's own ceiling, not purely the
      sensor's); `checkTcMaxContradiction()`'s existing hard block (gates
      both Save and guided-flow commit) still prevents committing above the
      type's table max. One drifted row found between this table and
      SaftyFW's own `TC_RANGES` (type B: 1798 here vs 1820 there, two
      different datasheet pages) — noted in code, the lower/more
      conservative number kept on purpose
- [x] **Maximum expected kiln power entered on the safety page.** Already
      landed — `max_expected_power_w` (id 793, param 0x0319) on
      `safety_commissioning_page.html`. Checkbox was stale; verified
      2026-08-28
- [x] **Per-zone current measurement, from the zones page.** Already landed
      (`zones_page.html`'s "Measure Zone Normal Current" sweep,
      `zones_http.c`'s `ZONE_SWEEP_*` implementation) — one zone at a time,
      refuses during a running profile/autotune/link-down/trip-latched,
      Abort leaves every relay off. Checkbox was stale; verified 2026-08-28.
      **What it does NOT do**: write SaftyFW's per-CT-channel `ct_channel_map`/
      `i_normal_a[]` — it stores a per-ZONE result on the ESP side only
- [x] **`ct_channel_map` derived from the zone-normal-current sweep**, so S14
      can actually be armed. 2026-08-28. The sweep now samples all three CT
      channels separately per zone (not summed through `ct_mask`, which is
      the thing being derived); a channel is only accepted when it clears an
      absolute floor AND beats the runner-up by 4x, and a zone whose
      `relay_mask` isn't exactly its own relay bit, or that ties with another
      zone for the same channel, is refused rather than guessed — both
      refusal paths are host-tested. The commit is verified LIVE, not
      trusted from the ACK: the same class of bug `ddbd024` fixed for the
      commissioning page's own writes was caught by review here too — an
      ACKed, un-rejected COMMIT_CONFIG is not proof the Pico stored
      anything, so this reads the value back over the wire before persisting
      or reporting a channel as derived. A failed/rejected/unconfirmed
      commit also backs out anything already staged, so an unrelated later
      commit can't pick up a leftover partial map. `safety_commissioning_page.html`'s
      three manual-entry fields now show the derived value read-only with an
      explicit override, falling through to manual entry when the sweep
      hasn't run or was ambiguous
- [x] **Runtime CT-to-zone mapping check.** The comparison itself was already
      shipped (`zones_ct_mapping_mismatch()`/`zones_ct_mapping_warn_mask()`,
      `zones_http.c`, Task 2) and re-evaluated fresh against LIVE current on
      every `GET /api/zones` — but `zones_page.html` only ever called that
      endpoint once, at page load, so an operator who opened the page and
      walked away never saw a CT moved mid-firing. 2026-08-28: added
      `pollCtMapping()`, the same `setInterval` pattern this page already
      uses for `pollCtCurrents`/`pollAutotune`, deliberately touching only
      `#ctWarnings` rather than reusing the full-page load path (which
      overwrites every form field from the response — fine once, destructive
      on an interval while an operator might be mid-edit). Catches a CT
      moved to the wrong jack, exactly the mistake `CONFIG_REFERENCE.md`
      says a wrong `k_ct_v_per_a` cannot be distinguished from otherwise.
      Never a trip — `zones_http.h`'s own doc comment is explicit that this
      decision belongs to the safety processor, not this file
- [x] **Delete the twelve stale `display_*` MCP tools.** The owner left the
      choice open; deleting wins because `display_bridge_task` is confirmed
      dead code on real hardware, so "restore a minimal firmware handler" means
      writing a new consumer for tools nobody uses, not repairing a broken one.
      Shipped 2026-08-28: the removal itself rode in with `9838399` — fifteen
      tools in the end (`display_read_id`, `display_rgb565`, `display_reset`,
      `_set_power`, `_set_rotation`, `_set_invert`, `_clear`, `_fill_rect`,
      `_draw_rect`, `_draw_line`, `_set_text_cursor`, `_set_text_style`,
      `_print`, `_send_image`, `_test_pattern`) plus their `_display_mutating`
      helper. Deliberately kept: `devices.display_*` frame builders,
      `DisplayClient`, `actions.py`'s DISPLAY entries and `gui.py`'s Display
      panel — all still reached by the generic `press_button`/`list_buttons`
      path, which is a live front end, not part of this cleanup. This pass
      corrected the stale registered-tool counts left behind (135 -> 127 in
      `CLAUDE.md`, `docs/MCP_SERVERS.md`, `mcp_server.py`, `mcpkit/__init__.py`)
- [x] **An uncommissioned safety processor refuses heating enable** — the
      owner's answer was an unqualified NO. Shipped 2026-08-28 (`5cd56b6`),
      last, per the ordering note above. **Commissioned** now means two
      facts that must agree: the `calibration_missing` verdict `COMMIT_CONFIG`
      persisted, AND `config_params_all_required_set()` recomputed from
      `fields_set` (the eight no-safe-default fields — `tc_source`,
      `borrowed_zone_index`, `tc_placement_mode`, `abs_max_temp_c`,
      `ct_channel_map`, `max_rate_c_per_min`, `mains_voltage_v`, `tc_type`).
      Any disagreement, in either direction, refuses — a stored flag the bits
      do not back up, or a v1→v2-migrated record whose flag is forced true.
      Plausible values never count: only an explicit `SET_PARAM` +
      `COMMIT_CONFIG` sets a bit. **The guard lives on the safety processor**,
      not in the KilnFW UI: `firmware/SaftyFW/src/commissioning_gate.c` (pure,
      host-tested) consulted by `safety_core_request_enable()` on the ON
      direction only, beside the update interlock and the `safety_tc_installed`
      refusal — `SAFETY_CMD_REQUEST_ENABLE(1)` never reaches
      `relay_owner_command_energize()`, while de-energizing is never gated.
      This closes the 2026-08-24 bench finding: an uncommissioned board has
      `abs_max_temp_c == 0`, so S1 can never trip, and S8 ships disabled —
      heat was being granted with no absolute ceiling in force. **No new fault
      source or wire field**: the condition already travels as Frame B's
      `CALIBRATION_MISSING` bit, which KilnFW shows as `commissioned:false` on
      the commissioning page and as the FAIL of the "Safety processor
      commissioned" readiness item a firing start is already blocked on; the
      Pico logs `request_enable: refused: safety processor not commissioned`.
      Accepted cost, exactly as `SaftyFW/TODO.md` predicted: a never-
      commissioned bench board cannot close K4 until a real commissioning pass
      lands

Added 2026-08-28, same conversation — these are about making the commissioning
surface usable rather than merely correct:

- [x] **The commissioning page is far too complex** (owner's words). Shipped
      2026-08-28 (`64d0a8e`): the page now asks four questions (kiln maximum
      temperature, where the safety thermocouple sits, mains voltage,
      expected power), writing six parameters — the ones nothing else can
      derive. The full 58-parameter list stays reachable under a closed-by-
      default `<details>` Advanced view for anyone who needs it, but the
      guided four-question flow is what a landing operator sees.
      `firmware/KilnFW/docs/COMMISSIONING_UX.md` tracks the field-by-field
      DERIVED/ASKED/DEFAULTED classification and is kept in sync with the
      real page, not left as a stale proposal
- [x] **Mains voltage becomes a dropdown** — 120, 240, 380, 460 and any other
      distinct standard worth offering. `CONFIG_REFERENCE.md` §3 says unset
      means "report --, never assume", so an explicit unset option survives.
      Already landed — `MAINS_VOLTAGE_OPTIONS` on
      `safety_commissioning_page.html` (120/208/240/277/380/400/415/460V,
      `-1` "Other..." fallback, explicit unset). Checkbox was stale; verified
      2026-08-28
- [x] **Current-monitor calibration comes from the zones config**, not from the
      commissioning page — it consumes the per-zone normal-current measurement
      rather than asking for numbers. 2026-08-28, `17ae4d9`. The 16 read-only
      current-sense rows already mirrored the zones config; the one that did
      not have a producer was `k_ct_v_per_a[0..2]`, which asked for a CT
      datasheet figure nobody had (`COMMISSIONING_UX.md` OQ4) and so stayed at
      `config_store.c`'s `memset(0)` — not cosmetic, since
      `current_presence_policy.c` then abandons the configured `i_present_a`
      for a fixed counts-domain margin. The zone current-sweep now calibrates
      it: it already energizes one zone at a time with every other relay
      forced off, so summing each zone's dominant CT channel gives the
      whole-kiln current at full output, and `max_expected_power_w /
      mains_voltage_v` (Q4/Q3, both already answered) gives what it should be.
      Amps are inversely proportional to `k_ct`, so the correction is one
      scale factor, `k_new[c] = k_old[c] · (measured / expected)`. Refuses
      outright — with the reason on both pages — on an unresolved or
      shared-CT zone (the total would be short by that zone's share), an unset
      Q3/Q4, a `k_old` still at 0 (the link carries amps, not counts, so every
      reading was `0.0 A`), a total under 2 A, a correction outside 0.2×–5×,
      or a result outside 0.0005–0.5 V/A. Written over the same
      `SET_PARAM`/`COMMIT_CONFIG` path a typed value uses, confirmed by a live
      bit-exact read-back, and backed out of the Pico's staged buffer on every
      failure arm — the same discipline as `zone_sweep_push_ct_channel_map()`,
      and it refuses to run at all if that push left the shared staged buffer
      unrepaired. A clamp-meter override stays behind a checkbox on the
      commissioning page. `zones_http.c`, 14 new host tests, each guard
      re-run stubbed out to prove it fails without it
- [x] **The safety thermocouple and safety relay configuration shown on the
      zones config, NOT reassignable there.** 2026-08-28. `safetyTcType` on
      `zones_page.html` was a live editable `<select>` submitted back on
      Save; now `renderSafetyTcType()` renders it read-only (same pattern as
      the safety-relay display `renderSafetyWiring()` already used), removed
      from the Save payload, with a note pointing at the safety
      commissioning page's Advanced section — the actual assignment
      surface, and where it stays ASKED. No safety-relay editable field
      existed to fix; that side was already read-only. No firmware/C change
      needed: `zones_http.c` already re-echoes the stored value when a
      field is absent from a POST, the same "older client omits the field"
      convention every other optional field on this page already relies on
- [x] **An over-current guard to pair with the under-current guard**, set as a
      PERCENTAGE of the measured normal current. Specified symmetrically with
      the existing S3/S4/S11 family, and it must NOT trip on a zone whose
      normal has never been measured. Already landed — S14
      (`safety_guards.c:781-798`), `overcurrent_pct`/`overcurrent_time_s`,
      WARN-only, per-channel `i_normal_valid` gate so an unmeasured channel is
      skipped rather than tripped. `build_saftyfw_host_tests` passes 1891/1891
      including its coverage. Checkbox was stale; verified 2026-08-28. **Not
      the same as arming it**: S14 still can't be armed until `ct_channel_map`
      has a real producer — see the item above

**Answered and closed, recorded so they are not re-asked:** every relay is to
be rated for 100% duty cycle and inrush is negligible — the board is designed
for it, so the SSR-vs-contactor-coil question and the 2 A/125 VA duty-window
check are both settled and need no further hardware answer. Breaker capacity is
assumed sufficient for the full kiln load at 100% duty.

## M13 — Every fault says what was detected, and what to do · *opened 2026-08-28*

A standing requirement from the repo owner, not a one-off fix: **"all faults
reported to the user should come with instructions on how to fix them or more
importantly what was detected wrong."** Note which half he called more
important — the cause, not the remedy. Treat this as a rule that applies to
every fault surface added from here on, not a milestone that closes.

It came from S6a, which is the worst case and therefore the right example. S6a
is `mainFault` (GPIO10) LOW, debounced 200 ms. The safety processor sees ONE
BIT and cannot know why — that independence is deliberate and is not to be
traded away. But the ESP does know: `fault_sources` is a bitmask of MANUAL /
PC_LINK / THERMO / SAFETY_LINK / APP / THERMAL_SANITY (`safety_link.h:139-142`),
and `dashboard_http.c:261` already reads it. So the cause was measured, held,
and simply never shown next to the trip.

**Landed 2026-08-28, all of it — S6a's fault-source decode (shared
`safety_trip_words.h` table across web/diagnostics/LCD, captured AT TRIP
TIME not read live), real numbers on every `safety_trip_t` cause line,
KilnFW-side fault coverage, and an explicit non-clearable notice for S9.**
Full postmortem: [`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m13m14--fault-reporting-and-verification-findings-full-detail-moved-2026-09-04).

**The clearing semantics, recorded here because they were only discoverable by
reading `safety_guards.c`:** an S6a trip LATCHES. It does not clear on its own,
a new firing does not clear it, and an ESP reboot does not. Only an explicit
CLEAR_TRIP does — and that clear is REFUSED while the cause persists, because
`safety_guards_try_clear()` (`safety_guards.c:209`) clears the state and
immediately re-runs the guard, and an unwindowed guard with the line still LOW
re-trips on that same tick. So the operator sequence is: identify the source,
remove it, then clear. None of that is currently told to the operator, and the
owner had to ask.

## M14 — Verification you can trust · *opened and CLOSED 2026-08-28*

Not a feature milestone. It exists because on 2026-08-28 the sentence "tests
pass, build clean, flashed and verified" could be true and worthless, and
almost every defect found that day was something reporting success it had
not earned. **CLOSED, all findings landed** — a `build_kilnfw` wrapper that
reported OK on a failed build, a `flash_firmware` that didn't check the
binary matched HEAD, two stack overflows found via unregistered margins, and
two new CI guard scripts that both caught real violations on their first
run. Full postmortem: [`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m13m14--fault-reporting-and-verification-findings-full-detail-moved-2026-09-04).

## M10 — Instrumentation: make the board tell you when it is wrong · *CLOSED 2026-09-04*

Not a feature milestone. It exists because four separate defects in this
project were invisible for weeks not because they were subtle, but because
nothing on the board was counting the right thing — and in three of the
four, something *was* counting and reported the comfortable answer.
**All findings landed and are hardware-verified.** Full postmortem for each
(route-table overflow, safety-poll false timeouts, DRAM/stack-margin
instrumentation and its own blind spots, truncated-JSON readiness checks,
the heartbeat-contract guard, and the HTTP-concurrency-reset root cause):
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m10--instrumentation-findings-full-detail-moved-2026-09-04).

Owned by [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) §§12–13.

## M15 — Architecture hardening · *opened and CLOSED 2026-09-04*

**CLOSED.** All 22 findings from the four-agent architecture review landed
the same day (12 files over the 1500-line rule split move-only, five new CI
drift/lint checks added, the SX1509/DRAM/duty-struct/mode-state/JSONL/
quantize/mcpkit/frame-A findings all fixed) — `build_kilnfw` + 21/21 host
tests green throughout. Informational carry-forward: the Pico is still on
protocol v8, which makes S13's BORROWED-zone indicator unreachable in
practice (fails closed and visibly, not a defect). Full per-item detail,
file maps and the "patterns worth copying" list:
[`docs/COMPLETED_2026-09.md`](docs/COMPLETED_2026-09.md#m15-architecture-hardening-findings-full-detail).

---

## M16 — Source layering and hardware abstraction · *opened 2026-09-05, in progress*

Two related reorganisations of the firmware trees, planned in full in
[`docs/HW_ABSTRACTION_PLAN.md`](docs/HW_ABSTRACTION_PLAN.md). The six
upward-include untangles are DONE; the `firmware/hwAbstraction/` tree exists
with all interface headers, ESP backends (common/gpio/uart/spi/i2c/kv/time/
wdt/pwm/sysinfo), Pico backends (gpio/adc/uart/time/flash/scratch/wdt), and
host fakes for every interface; nothing is wired into CMakeLists yet
(Phase 1a, the actual move, has not started). The `drivers/` directory move
itself was applied in `9f18ca5` (2026-09-05).

1. **KilnFW `drivers/` layering** (KilnFW only) — DONE (`9f18ca5`,
   2026-09-05). `App/drivers/` reorganised into
   `App/drivers/{hw,owners,control,safety,persist,net,http,ui,bridge,sim,
   common}/`, 359 renames, CMakeLists SRCS rewritten and `check_*.ps1`
   scripts re-greped for old paths. Plan section "drivers/ layering".
2. **`firmware/hwAbstraction/{interface,esp,pico,host}`** — link-time
   backends for spi/i2c/uart/gpio/adc/kv/flash/scratch/time/wdt/pwm/sysinfo
   over ESP-IDF and pico-sdk, with host fakes replacing the stub-header
   include trick interface by interface. Phase 0 (headers, sizes, MSVC
   compile) through Phase 4 (include-direction check goes strict). Phase 1a
   moves `espInterfaces/` only after item 1 so paths move once.

Sequence: untangle includes (done), reorg (done, `9f18ca5`), then HAL Phase 1a.
All owner decisions are taken (2026-09-05): tree location/naming as above,
opaque-storage option a, hal_uart `send` + `send_blocking`.
`firmware/UnitTestFw` stays untouched throughout.

Gates: `build_kilnfw` + all 23 host executables green after every commit;
every check script proven able to go red after the move (nine of twelve
prior splits broke one silently); safety-link reply timing re-measured on
hardware after Phase 1b. Land-alone diffs — coordinate with any other
session on the tree, and rebase the `s14-cal-gate` worktree before any
SaftyFW move touching `safety_core.c`.

---

## Future work — KilnFW PC-link command acknowledgement

**Moved out of this file, 2026-08-24.** Per the upkeep rule at the top, the
detail now lives in [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md)
section 11, which owns it. In brief: the core defect is fixed — a truncated
payload, an out-of-range index and each relay refusal reason (owned / safety /
updating) now produce a reply the host can tell apart from success and from
each other, and `pc_tools` surfaces the reason instead of decoding it and
discarding it, which is what it used to do. The rule worth carrying forward is
recorded in [`firmware/KilnFW/docs/UART_PROTOCOL.md`](firmware/KilnFW/docs/UART_PROTOCOL.md):
on this hop an ACK means *queued*, not *done*, so a handler that rejects must
reply for itself.

**Closed out 2026-08-24 (`7b4c087`).** The three narrower instances this
paragraph used to leave open are done, and the audit behind them found more
than the item described: twelve IO subcommands plus DISPLAY's writes and three
TOUCH commands were fire-and-forget on the host side, so a refusal landed with
nothing pending and was logged at debug as "ignoring unsolicited response";
AUTOTUNE `ABORT`/`ACCEPT` and four WIFI writes did wait but discarded the
reason. On the firmware side `display_bridge_task` had never received any of
the original treatment at all — 13 guard failures replying zero bytes, plus 2
in TOUCH.

Two things are worth carrying forward rather than rediscovering:

- **A reasonless rejection is not a safe default.** `{subcmd, 0}` was
  byte-identical to `THERMO_CMD_READ_FAULTS`'s and `IO_CMD_SX_SCAN`'s honest
  empty-success reply, so "this firmware has never heard of your command"
  and "we ran it and found nothing" were the same two bytes — on the exact
  path an older build takes. Every rejection now carries a reason, enforced by
  [`tools/check_bridge_reject_reason.ps1`](tools/check_bridge_reject_reason.ps1).
- **The DISPLAY half is correct but unreachable.** `main.c` never starts
  `display_bridge_task` (LVGL owns the panel), so every DISPLAY frame is NACKed
  by the transport as "dst task 4 not registered". It changes no observable
  behaviour until `KilnFW/TODO.md` §10.1 decides between deleting the stale
  `display_*` tools and restoring a minimal handler. **That decision is open
  and is one of the few remaining items that needs a human.**

One gap remains open on purpose: `BLIT_DATA` stays raw fire-and-forget, because
a per-chunk wait would turn a ~1 minute image transfer into ~20 minutes.

This does NOT apply to the ESP↔Pico safety link, whose no-ACK doctrine is
deliberate and correct: `LINK_PROTOCOL.md` sections 1–2 forbid obliging the
Pico to reply, and telemetry already carries `config_crc`, the trip mask and
`boot_id` every 500 ms, so "poll the next frame" is both available and
sufficient there. The two files carry similar-looking comments that mean
different things; keep them distinct.

---

## Decisions taken, so they are not re-litigated

| Decision | Date | Where the reasoning lives |
|---|---|---|
| **A zone's time-proportioning window and its minimum on-time are one constraint, not two settings.** `heater_window_ms >= 3 * max(heater_min_on_ms, 10 s)`, enforced at every config door. Below that ratio no fractional duty can be rendered and a PID zone silently becomes a bang-bang one — this bench ran a 2 s window against the 10 s floor and every commanded duty rendered as OFF. | 2026-08-29 | `firmware/KilnFW/docs/PID_CONTROL.md` "The window and the minimum on-time are not independent" |
| Pico bench path is the Debug Probe: SWD plus its UART bridge on GP16/GP17. **The Pico's own USB is not used.** | 2026-08-16 | `firmware/SaftyFW/docs/ARCHITECTURE.md` §1 |
| **On-board relays K1-K4 are for galvanic isolation and switch other relays only — never element current.** They are EE2-12NUH signal relays (2 A, 125 VA max), so element switching was never physically possible. This retires the duty-window contact-life worry; the downstream device's own life and coil inrush now set the limit. | 2026-08-24 | `firmware/KilnFW/TODO.md` §6A.0, `hardware/datasheets/mainBoard_Relay/EE2-12NUH.pdf` p7 |
| ~~**PSRAM stays disabled** on the ESP32-S3~~ — **reversed 2026-08-17: PSRAM is ENABLED** (octal, 8 MB) and used for LVGL draw buffers + heap + several task stacks | 2026-08-16, reversed 2026-08-17 | `firmware/KilnFW/TODO.md` §9.1a |
| Library paths use `${KIPRJMOD}/../lib`, not a KiCad path variable | 2026-08-16 | `docs/REPO_LAYOUT.md` B1 |
| OTA authentication is challenge–response on the AP password, never a form POST | 2026-08-16 | `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` §2 |
| Update frames and the version handshake are a **frozen compatibility floor** | 2026-08-16 | `firmware/CommonFW/docs/LINK_PROTOCOL.md` |
| **A request and its reply may never share a command id.** Telling them apart by payload length structurally blocks a short refusal reply, which is why SAFETY/DISPLAY/TOUCH's driver-error paths stayed silent. `GET_CT_CAL`/`GET_PARAM`/`GET_CONFIG_PAGE` moved to `0x22`/`0x23`/`0x24`; `GET_FW_VERSION` keeps its shared `0x0B` as the documented exception, being inside the frozen floor where a refusal is never needed. | 2026-08-24 | `firmware/CommonFW/docs/LINK_PROTOCOL.md` |
| **The two links version independently.** `UART_PROTOCOL_VERSION` (PC↔ESP) was an alias of `KILNLINK_PROTOCOL_VERSION` (ESP↔Pico) behind a hard-equality gate, so an isolated-link bump refused every PC command until pc_tools moved. Bitten three times before being split. | 2026-08-24 | `firmware/KilnFW/App/drivers/common/uart_task_ids.h` |
| K4 → line-contactor interlock: J10 pin 1 = NO, pin 2 = COM, pin 3 = NC (read from the K4 symbol's rest position, not silkscreen) — still wants a continuity check against the physical part | 2026-08-16 | `firmware/SaftyFW/docs/HARDWARE.md` §3 |
| E-stop circuit is normally-closed by design. ~~No jumper fitted, so an as-built board reads permanent STOP~~ — **corrected 2026-08-24 by measurement**: `pico_gpio_read(9)` reads LOW on this bench, i.e. a contact IS fitted and S7 correctly stays quiet. Do not plan around needing to fit one; measure instead. The stale note also masked a real inversion — `discrete_task.c` had `!gpio_get()` on an active-HIGH pin, so this healthy reading decoded as *pressed*, hidden because S5 latched first and `safety_guards_tick()` early-returns while any trip is latched | 2026-08-16, corrected 2026-08-24 | `firmware/SaftyFW/docs/HARDWARE.md` §5, commit `642dd54` |
| ESP32-S3 boot-loop (repeating stack overflow in the main task, right after LVGL's boot banner) fixed by raising `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 3584→8192 | 2026-08-19 | `firmware/KilnFW/App/main.c`, `sdkconfig.defaults` |
| Internal SRAM exhaustion: `xTaskCreatePinnedToCore()` always takes TCB+stack from internal SRAM, and Wi-Fi/lwIP + LVGL had claimed nearly all of it by the time later tasks tried to start (caused the AUTOTUNE/WIFI UART-task registration failures). Fixed at the source — moved LVGL's allocator and the Wi-Fi/lwIP pools to PSRAM — not by shrinking the tasks that were failing | 2026-08-20 | `firmware/KilnFW/TODO.md` §1 |
| `uart_owner_transfer()` called `xSemaphoreCreateBinary()` (a heap alloc) on every single UART transfer; under real interactive load this exhausted internal SRAM (`ESP_ERR_NO_MEM` bursts every ~40s). Fixed with a static, stack-resident semaphore | 2026-08-18 | `firmware/hwAbstraction/esp/uart/uart_owner.c` |
| LVGL hit-testing cannot escape a parent that doesn't contain the touch point, and a non-`LV_OBJ_FLAG_FLOATING` child of a flex column silently joins the flow and eats the page's content budget | 2026-08-20 | `firmware/KilnFW/App/drivers/ui/ui_topbar.h` |
| benchproto returned a **stale reply from a different command** after every PC reconnect: the host's `msg_index` restarts at 0 per connection while the firmware's dedup ring and per-task ACK cache live for the MCU's boot lifetime. `FAULT_LIST` reported "0 faults" while 8 were armed — a clean-decoding wrong answer, diagnosed by reply *length* (3 bytes is `FAULT_SCHEDULE`'s shape, not an empty list's 2). Fixed with a `SYS_SESSION_RESET` handshake that must itself bypass dedup | 2026-08-23 | `firmware/CommonFW/src/benchproto_link.c` |

---

## How the work gets done — delegate to Sonnet subagents

Standing instruction from the repository owner, 2026-08-21. It applies to every
milestone below and to any new work filed against this roadmap.

**The coordinating session should not implement roadmap work itself.
Implementation is assigned to Sonnet subagents, and the coordinator oversees
them.** In practice that means:

- The coordinator reads enough of the code to write an accurate brief, splits
  the work into non-overlapping file scopes, dispatches Sonnet subagents, and
  then verifies what comes back. It does not sit down and write the feature.
- **Sonnet is the default worker model.** Opus and Haiku are available when a
  task genuinely calls for them; Fable coordinates and reviews only, and is
  never a worker.
- **Scopes must not overlap.** Two agents editing the same file collide
  silently and the loser's work is lost. Every brief names the files it owns
  and the files it must not touch.
- **Builds stay central.** Concurrent `idf.py` runs against one build
  directory clobber each other, so subagents write code and the coordinator
  compiles once. Briefs say "do not build" explicitly.
- **Verification is not delegated.** A subagent's report is a claim, not
  evidence. The coordinator builds, flashes, and exercises the change against
  the four levels in "What 'done' means" below before any item here is ticked.
- The narrow exception is shared scaffolding that encodes a hazard already paid
  for in a debugging session — the kind of thing a fresh agent reliably gets
  wrong. Writing that once, centrally, so every delegated task inherits the fix
  is cheaper than briefing the hazard into every agent.
  `firmware/KilnFW/App/drivers/ui/ui_topbar.h` is the worked example: it exists
  because LVGL hit-testing cannot escape a parent that does not contain the
  touch point, and because a non-`LV_OBJ_FLAG_FLOATING` child of a flex column
  silently joins the flow and eats the page's content budget. Both cost a
  session before they were understood.

---

## Working from the repository root

Claude and the editor are opened at `kilnCtl/` from 2026-08-16 onward. This is
now a load-bearing assumption rather than a preference:

- **Open `kilnCtl.code-workspace`, not the folder.** The ESP-IDF extension needs
  a `CMakeLists.txt` in the workspace folder it is pointed at, and the repository
  root does not have one. The multi-root workspace gives it `firmware/KilnFW`
  while keeping the root open alongside.
- `.mcp.json` uses root-relative paths, including `-C firmware/KilnFW` for the
  ESP-IDF server.
- `idf.py` needs `-C firmware/KilnFW`; `uv` needs `--project tools/PcTools`.

What it changes, and what still needs doing, is
[`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md) "Working from the repository root".

---

## Cross-processor invariants

Any change that touches these needs both plans read, not one:

- The safety processor **never blocks on the link**. A frozen ESP core must not
  be able to stall it.
- The safety processor is **RX-only while firing**; the telemetry relaxation
  applies outside firing and to unacknowledged broadcast frames.
- **The safety processor must be alive to start or continue any heater-on event.**
- Current measurement is **load-active detection and a power estimate only**.
- Guards clear two independent bars — a magnitude correct operation cannot reach,
  and a duration a transient cannot sustain.
- All board relays are **pilot relays**, driving external contactors and SSRs.
- **Neither processor is updatable while the kiln can heat**, and the Pico
  enforces that itself rather than trusting the ESP.
- **Each processor checks the other's protocol version**, in both directions, and
  neither trusts the other's data until both agree. The frames that establish
  that, and the ones that carry an update, are frozen so a mismatch can never
  lock out the fix.

## What "done" means

Same four levels everywhere, and they are not interchangeable:

**planned** → **built** (compiles, `-Wall -Wextra -Werror`) → **host-tested**
(synthetic inputs, negative paths) → **hardware-verified** (observed on the real
board). [`firmware/KilnFW/docs/PROJECT_STATUS.md`](firmware/KilnFW/docs/PROJECT_STATUS.md) keeps
built and verified distinct; every plan here is expected to do the same.

## Roadmap upkeep

- [ ] Milestone ticks mirrored into the owning plan, not only here
- [ ] `Last reviewed` date bumped whenever a milestone changes state
- [ ] New work filed under a milestone, or a new milestone added with its owner
- [ ] **A finished item leaves this plan.** Either it moves to a
      reference/doc file (a decision, hazard, or reasoning someone will re-hit
      — a one-line pointer here is enough) or it is deleted. Ticked boxes and
      completion narratives do not accumulate here; a milestone that is fully
      done collapses to one line
