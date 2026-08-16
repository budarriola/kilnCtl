# kilnCtl Roadmap — both processors

> **Status:** planning · **Last reviewed:** 2026-08-16
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
  detection, owns the mechanical pilot relay K4. **Nothing built yet.**

They talk over an opto-isolated UART. That link, and the rule that **the safety
processor must be alive for the main processor to heat**, is what makes this one
project rather than two.

---

## Where each kind of task is planned

| Plan | Owns |
|---|---|
| [`ROADMAP.md`](ROADMAP.md) (this file) | Milestone order, cross-processor dependencies |
| [`KilnFW/TODO.md`](KilnFW/TODO.md) | Main firmware: web UI, profiles, PID, thermal protection, storage |
| [`KilnFW/docs/PROJECT_STATUS.md`](KilnFW/docs/PROJECT_STATUS.md) | What in `KilnFW` is built vs. verified — the honest ledger |
| [`SaftyFW/TODO.md`](SaftyFW/TODO.md) | Safety firmware, phases 0–9 |
| [`SaftyFW/docs/SAFETY_MODEL.md`](SaftyFW/docs/SAFETY_MODEL.md) | What trips, why, and the anti-nuisance doctrine |
| [`SaftyFW/docs/ARCHITECTURE.md`](SaftyFW/docs/ARCHITECTURE.md) | Tasks, priorities, core affinity, logging transports |
| [`SaftyFW/docs/HARDWARE.md`](SaftyFW/docs/HARDWARE.md) | The traced board, pin map, bench connections |
| [`CommonFW/README.md`](CommonFW/README.md) | Shared `kilnlink` code, used by both firmwares |
| [`CommonFW/docs/LINK_PROTOCOL.md`](CommonFW/docs/LINK_PROTOCOL.md) | The wire, both ends — the contract neither side may break alone |
| [`PcTools/README.md`](PcTools/README.md) | GUI, MCP, GPIO probe, debug and logging for **both** processors |
| [`REPO_LAYOUT.md`](REPO_LAYOUT.md) | The hardware/software reorganisation and its blockers |

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

## M0 — Unblock the link · *the only milestone with no alternatives*

Owned by [`SaftyFW/TODO.md`](SaftyFW/TODO.md) phase 0.

- [ ] Tier 0 pin test settles ESP TX/RX by measurement (`HARDWARE.md` §1)
- [ ] `KILNCTL_SAFETY_TX_IO` = 4, `RX_IO` = 5, pull-up moved to GPIO5
- [ ] `KilnFW/docs/SAFETY_LINK.md` and `HARDWARE.md` corrected in the same change
- [ ] `UART_PROTO_MSG_BROADCAST = 0x04` added to `uart_protocol.{c,h}`
- [ ] `mainBoard/kiln.net` regenerated or deleted — it is stale and misleading
- [x] Bench path decided: Debug Probe SWD + probe UART bridge on GP16/GP17; no Pico USB
- [ ] DEBUG header and GP16/GP17 access provided before A1 is soldered down

System-wiring decisions that gate any bench trip test, and are not firmware work:

- [ ] K4 → line-contactor interlock topology confirmed; J10 NO vs NC identified
- [ ] E-stop confirmed normally-closed, or a deliberate jumper fitted

## M1 — Tooling that makes everything after it cheaper

Owned by [`PcTools/README.md`](PcTools/README.md). Worth doing early precisely
because it is what turns later hardware questions into a script instead of a
soldering session.

- [ ] `KilnFW/pc_tools/` → `PcTools/`, package still `kilnctrl`
- [ ] GPIO probe on the ESP, default off, deny-list including GPIO6
- [ ] GPIO probe on the Pico over SWD, GPIO6 never writable
- [ ] Coordinated two-board test script, reaching each side by a path that is
      **not** the link under test
- [ ] OpenOCD wrapper covering both chips: program, reset, halt, read/write memory
- [ ] Per-processor console windows and log files, plus an interleaved file

## M2 — `CommonFW`, before either firmware depends on it

Owned by [`CommonFW/README.md`](CommonFW/README.md), gating items repeated in
[`SaftyFW/TODO.md`](SaftyFW/TODO.md) phase 1.

- [ ] `kilnlink` target consumable by both pico-sdk and ESP-IDF
- [ ] `KILNLINK_PROTOCOL_VERSION` the single source; `UART_PROTOCOL_VERSION` an alias
- [ ] Codecs pure and bounds-checked; host tests and `test/vectors/`
- [ ] `pc_tools` consuming the same vectors as the third implementation
- [ ] `KilnFW` delegating framing and CRC, proven byte-identical **before** the
      old code is deleted
- [ ] CI grep: no CRC or byte-stuffing implementation outside `CommonFW`

## M3 — Safety processor to first trustworthy reading

Owned by [`SaftyFW/TODO.md`](SaftyFW/TODO.md) phases 2–4. Independent of the
link, so it can run in parallel with M1 and M2 once M0 is out of the way.

- [ ] FreeRTOS SMP skeleton, tasks at the planned priorities and core affinities
- [ ] `main()` drives GPIO6 low as its first statement
- [ ] Watchdog with the trip reason latched in scratch registers
- [ ] MAX31856 on J7, with per-thermocouple type configuration
- [ ] Guards implemented and **host-tested against synthetic inputs**, no relay yet
- [ ] CI grep: `safety_core.c` never includes the link header

## M4 — Relay authority

Owned by [`SaftyFW/TODO.md`](SaftyFW/TODO.md) phase 5. First milestone that can
physically stop a kiln, and the first that can nuisance-trip one.

- [ ] Relay owner task is the only writer of GPIO6
- [ ] Trip latches; clearing requires an explicit command
- [ ] S9 trip-ineffective escalation proven with a deliberately welded contactor
- [ ] Every guard exercised per [`GUARD_TEST_MATRIX.md`](SaftyFW/docs/GUARD_TEST_MATRIX.md)

## M5 — The link carrying real traffic

Owned by [`SaftyFW/TODO.md`](SaftyFW/TODO.md) phases 6–8, contract in
[`CommonFW/docs/LINK_PROTOCOL.md`](CommonFW/docs/LINK_PROTOCOL.md).

- [ ] Current sensing: load-active detection and a power estimate — **not** an
      over/under-current trip
- [ ] ESP → Pico context frames, including `relay_recent_mask`
- [ ] Pico → ESP telemetry: status, diagnostics, firmware version, trip events, power
- [ ] Pico never blocks on the link — all five no-wait rules honoured
- [ ] TX ring reserves capacity for telemetry; log frames dropped above the
      watermark and the drops counted
- [ ] Borrowed-thermocouple staleness split correctly across S11 / S13 / S6

## M6 — Throw the liveness switch

The point at which the two processors become one system. Deliberately separate,
because it changes what a bare main board will do.

- [ ] `SAFETY_FAULT_SRC_SAFETY_LINK` redefined as "no telemetry within 1.5 s"
- [ ] 30 s silence aborts a firing
- [ ] Boot-time version request with retry, surfaced in the GUI
- [ ] Bench escape hatch documented: `safety_link_fault_on_link_loss(link, false)`
- [ ] GUI shows safety temperature, enclosure temperature and power

## M7 — Repo reorganisation

Owned by [`REPO_LAYOUT.md`](REPO_LAYOUT.md). Explicitly scheduled **after** the
safety firmware is written (blocker B6) — moving paths mid-bring-up buys nothing
and breaks tooling at the worst moment.

- [ ] All of B1–B5 resolved
- [ ] Fresh `git clone` into a scratch directory opens `mainBoard` — the only
      test that catches the absolute-path breakage

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

## What "done" means

Same four levels everywhere, and they are not interchangeable:

**planned** → **built** (compiles, `-Wall -Wextra -Werror`) → **host-tested**
(synthetic inputs, negative paths) → **hardware-verified** (observed on the real
board). [`KilnFW/docs/PROJECT_STATUS.md`](KilnFW/docs/PROJECT_STATUS.md) keeps
built and verified distinct; every plan here is expected to do the same.

## Roadmap upkeep

- [ ] Milestone ticks mirrored into the owning plan, not only here
- [ ] `Last reviewed` date bumped whenever a milestone changes state
- [ ] New work filed under a milestone, or a new milestone added with its owner
