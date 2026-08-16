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
| [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) | Main firmware: web UI, profiles, PID, thermal protection, storage |
| [`firmware/KilnFW/docs/PROJECT_STATUS.md`](firmware/KilnFW/docs/PROJECT_STATUS.md) | What in `KilnFW` is built vs. verified — the honest ledger |
| [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) | Safety firmware, phases 0–10 |
| [`firmware/SaftyFW/docs/SAFETY_MODEL.md`](firmware/SaftyFW/docs/SAFETY_MODEL.md) | What trips, why, and the anti-nuisance doctrine |
| [`firmware/SaftyFW/docs/ARCHITECTURE.md`](firmware/SaftyFW/docs/ARCHITECTURE.md) | Tasks, priorities, core affinity, logging transports |
| [`firmware/SaftyFW/docs/HARDWARE.md`](firmware/SaftyFW/docs/HARDWARE.md) | The traced board, pin map, bench connections |
| [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md) | Shared `kilnlink` code, used by both firmwares |
| [`firmware/CommonFW/docs/LINK_PROTOCOL.md`](firmware/CommonFW/docs/LINK_PROTOCOL.md) | The wire, both ends — the contract neither side may break alone |
| [`firmware/CommonFW/docs/UPDATE_PROTOCOL.md`](firmware/CommonFW/docs/UPDATE_PROTOCOL.md) | Field updates for both processors: interlocks, one-password auth, ESP OTA partitioning |
| [`firmware/SaftyFW/docs/BOOTLOADER.md`](firmware/SaftyFW/docs/BOOTLOADER.md) | The RP2040 bootloader, flash layout and recovery mode |
| [`tools/PcTools/TODO.md`](tools/PcTools/TODO.md) | GUI, MCP, GPIO probe, debug and logging for **both** processors |
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

## M0 — Unblock the link · *the only milestone with no alternatives*

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 0.

- [ ] Tier 0 pin test settles ESP TX/RX by measurement (`HARDWARE.md` §1)
- [ ] `KILNCTL_SAFETY_TX_IO` = 4, `RX_IO` = 5, pull-up moved to GPIO5
- [ ] `firmware/KilnFW/docs/SAFETY_LINK.md` and `HARDWARE.md` corrected in the same change
- [ ] `UART_PROTO_MSG_BROADCAST = 0x04` added to `uart_protocol.{c,h}`
- [ ] `hardware/mainBoard/kiln.net` regenerated or deleted — it is stale and misleading
- [x] Bench path decided: Debug Probe SWD + probe UART bridge on GP16/GP17; no Pico USB
- [ ] DEBUG header and GP16/GP17 access provided before A1 is soldered down

System-wiring decisions that gate any bench trip test, and are not firmware work:

- [ ] K4 → line-contactor interlock topology confirmed; J10 NO vs NC identified
- [ ] E-stop confirmed normally-closed, or a deliberate jumper fitted

## M1 — Tooling that makes everything after it cheaper

Owned by [`tools/PcTools/TODO.md`](tools/PcTools/TODO.md). Worth doing early precisely
because it is what turns later hardware questions into a script instead of a
soldering session.

- [x] `KilnFW/pc_tools/` → `tools/PcTools/`, package still `kilnctrl`
- [ ] GPIO probe on the ESP, default off, deny-list including GPIO6
- [ ] GPIO probe on the Pico over SWD, GPIO6 never writable
- [ ] Coordinated two-board test script, reaching each side by a path that is
      **not** the link under test
- [ ] OpenOCD wrapper covering both chips: program, reset, halt, read/write memory
- [ ] Per-processor console windows and log files, plus an interleaved file

## M2 — `CommonFW`, before either firmware depends on it

Owned by [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md), gating items repeated in
[`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 1.

- [ ] `kilnlink` target consumable by both pico-sdk and ESP-IDF
- [ ] `KILNLINK_PROTOCOL_VERSION` the single source; `UART_PROTOCOL_VERSION` an alias
- [ ] Codecs pure and bounds-checked; host tests and `test/vectors/`
- [ ] `pc_tools` consuming the same vectors as the third implementation
- [ ] `KilnFW` delegating framing and CRC, proven byte-identical **before** the
      old code is deleted
- [ ] CI grep: no CRC or byte-stuffing implementation outside `CommonFW`

## M3 — Safety processor to first trustworthy reading

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 2–4. Independent of the
link, so it can run in parallel with M1 and M2 once M0 is out of the way.

- [ ] FreeRTOS SMP skeleton, tasks at the planned priorities and core affinities
- [ ] `main()` drives GPIO6 low as its first statement
- [ ] Watchdog with the trip reason latched in scratch registers
- [ ] MAX31856 on J7, with per-thermocouple type configuration
- [ ] Guards implemented and **host-tested against synthetic inputs**, no relay yet
- [ ] CI grep: `safety_core.c` never includes the link header

## M4 — Relay authority

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 5. First milestone that can
physically stop a kiln, and the first that can nuisance-trip one.

- [ ] Relay owner task is the only writer of GPIO6
- [ ] Trip latches; clearing requires an explicit command
- [ ] S9 trip-ineffective escalation proven with a deliberately welded contactor
- [ ] Every guard exercised per [`GUARD_TEST_MATRIX.md`](firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md)

## M5 — The link carrying real traffic

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 6–8, contract in
[`firmware/CommonFW/docs/LINK_PROTOCOL.md`](firmware/CommonFW/docs/LINK_PROTOCOL.md).

- [ ] Current sensing: load-active detection and a power estimate — **not** an
      over/under-current trip
- [ ] ESP → Pico context frames, including `relay_recent_mask`
- [ ] Pico → ESP telemetry: status, diagnostics, firmware version, trip events, power
- [ ] Pico never blocks on the link — all five no-wait rules honoured
- [ ] Mutual version handshake: `ANNOUNCE_VERSION` both ways, `min_compatible`
      checked in both directions, compatibility floor reserved at ids `0x00`–`0x0F`
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

## M7 — Repo reorganisation · *done 2026-08-16, three items open*

Owned by [`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md). Done **before** `SaftyFW`
phase 1, which is the timing blocker B6 preferred: the paths in the new
documents are correct from the start rather than being rewritten later.

- [x] Tree split into `hardware/`, `firmware/`, `tools/`, `docs/` — 380 files,
      all byte-identical renames
- [x] B1 library tables, B2 TFT35-SPI submodule, B3 `CLAUDE.md`, B5 clean tree
- [x] Everything else the move broke: `.mcp.json`, both `.vscode`
      `compile-commands-dir` settings, two `PcTools` modules that derived paths
      from `__file__`, 3D model paths in 17 footprints, twelve broken doc links
- [x] B4 — the stale `mainBoard/kiln.net` deleted
- [ ] `mykicadMcp/` and `pdfMcp/` moved under `tools/` — blocked at the time by
      running MCP server processes holding the directories open
- [x] **Three of four KiCad projects opened, no missing libraries (2026-08-16).**
      Includes `mainBoard`, which is the one B1 applied to, so the relative
      library path is confirmed working. `UnitTestFixture` still unopened
- [ ] Fresh `git clone` into a scratch directory opens `mainBoard` — the only
      test that catches the absolute-path breakage for someone who is not this
      user on this machine

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

- [ ] **Measure the isolated link's real error rate at 115200 first.** Nobody has
      characterised the optocouplers; retry cost is 200 ms × up to 10
- [ ] **Establish the real flash size, then switch to it.** Three records in the
      repo disagree — buy lists say N8R8 (8 MB), the 3D model says N8R2 (8 MB),
      the board in hand is an N16R8 (16 MB) — while the build says 2 MB. The real
      image is 1167 KB, so two copies do not fit in the 1500 KB app region. The
      layout is sized for 8 MB and puts everything above `0x200000` where nothing
      exists, so no live data moves. Bootloader must be reflashed
- [ ] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, and the app confirms itself only
      after NVS, safety link and web server are up
- [ ] Pico flash layout and metadata format frozen before the first board is
      programmed; signature field and key space reserved even though signing is off
- [ ] Pico bootloader: GPIO6 low first, active slot CRC'd every boot, recovery
      mode over UART1 with no timeout out of it
- [ ] **Mutual protocol-version check** (lands with M5, gates this): each side
      verifies the other, a mismatch blocks heating on the ESP and puts the Pico
      in `DEGRADED_NO_CONTEXT` without latching a trip
- [ ] **Compatibility floor** frozen so a version mismatch can never disable the
      update path itself — otherwise every mismatch needs a debug probe
- [ ] Image header validated before the first erase, so a wrong-target upload
      cannot erase a slot
- [ ] Challenge–response on the AP password, so it never crosses the wire;
      lockout after 3 failures
- [ ] Both paths refused unless the kiln is idle and cool, with the specific
      blocker named
- [ ] Link-loss heating block **not** bypassed during a Pico update — alarm text
      suppressed, never the block
- [ ] Four MCP tools, since most development updates will be agent-driven

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
