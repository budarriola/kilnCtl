# kilnCtl Roadmap — both processors

> **Status:** planning · **Last reviewed:** 2026-08-18
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

- [ ] **Link confirmed not working end-to-end (2026-08-18). Needs bench
      measurement, not more code guessing.** Scope pins/logic analyzer TX and RX
      lines at both opto boundaries during a coordinated GPIO test (`tools/PcTools`
      coordinated test, M1 below) to find where the byte actually dies — idle
      level, framing, baud mismatch, or opto polarity. Do not touch pin config
      again until the trace shows what's wrong
- [ ] Tier 0 pin test settles ESP TX/RX by measurement (`HARDWARE.md` §1) — still
      needs the Pico physically attached; the code-side fix below is not this
- [x] `KILNCTL_SAFETY_TX_IO` = 4, `RX_IO` = 5, pull-up moved to GPIO5 (2026-08-16,
      `Kconfig` + `sdkconfig`; `idf.py build` clean)
- [x] `firmware/KilnFW/docs/SAFETY_LINK.md` and `HARDWARE.md` corrected in the same change (2026-08-16)
- [x] `UART_PROTO_MSG_BROADCAST = 0x04` added to `uart_protocol.{c,h}` (2026-08-16,
      send/receive implemented; version not bumped, nothing consumes it yet)
- [x] `hardware/mainBoard/kiln.net` regenerated or deleted — was already gone; confirmed 2026-08-16
- [x] Bench path decided: Debug Probe SWD + probe UART bridge on GP16/GP17; no Pico USB
- [ ] DEBUG header and GP16/GP17 access provided before A1 is soldered down

System-wiring decisions that gate any bench trip test, and are not firmware work:

- [x] K4 → line-contactor interlock topology identified from the schematic
      (2026-08-16): J10 pin 1 = NO, pin 2 = COM, pin 3 = NC — wire the contactor
      coil to pins 1+2. Read from the K4 symbol's drawn rest position, not a
      silkscreen label, so **still wants a continuity check against the
      physical part** before final wiring (`firmware/SaftyFW/docs/HARDWARE.md` §3)
- [x] E-stop circuit confirmed normally-closed by design (2026-08-16) — no jumper
      is fitted anywhere on the `estop` net today, so an as-built board reads
      permanent STOP until a switch or a deliberate jumper is physically added
      (`firmware/SaftyFW/docs/HARDWARE.md` §5)

## M1 — Tooling that makes everything after it cheaper

Owned by [`tools/PcTools/TODO.md`](tools/PcTools/TODO.md). Worth doing early precisely
because it is what turns later hardware questions into a script instead of a
soldering session.

- [x] `KilnFW/pc_tools/` → `tools/PcTools/`, package still `kilnctrl`
- [x] GPIO probe on the ESP, default off, deny-list including GPIO6 (2026-08-16,
      `firmware/KilnFW/App/drivers/gpio_probe.{c,h}` + `tools/PcTools`; not yet
      bench-tested, see `tools/PcTools/TODO.md`)
- [x] GPIO probe on the Pico over SWD, GPIO6 never writable (2026-08-18,
      `tools/PcTools/src/kilnctrl/pico_gpio_probe.py`, no firmware agent
      needed -- RP2040 GPIO is memory-mapped, poked via the existing
      `debug_probe.py`/OpenOCD SWD connection; GPIO6 refused unconditionally
      for SET_MODE/WRITE, READ allowed; not yet bench-tested, see
      `tools/PcTools/TODO.md` §1b)
- [x] Coordinated two-board test script, reaching each side by a path that is
      **not** the link under test (`tools/PcTools/scripts/
      coordinated_gpio_test.py`; not yet run against real hardware)
- [x] OpenOCD wrapper covering both chips: program, reset, halt, read/write memory
      — done 2026-08-17, `kilnctrl.debug_probe` + `openocd_util` (`tools/PcTools/TODO.md`
      "Debug and programming"); flashed and verified `firmware/SaftyFW/build/SaftyFW.elf`
      to the Pico over SWD as the first real use
- [x] Per-processor console windows and log files, plus an interleaved file —
      `tools/PcTools/src/kilnctrl/console_capture.py` (`kilnctrl-console-capture`
      CLI, not a Tk window: reuses the existing ESP `LogClient`/`get_device_log`
      plumbing, adds a raw-serial reader for the Pico's probe UART console),
      writing `esp_*.log` / `safety_*.log` / `interleaved_*.log` under
      `tools/PcTools/logs/console/`. Host-verified: the interleave/merge and
      file-writing logic, with synthetic events (no hardware attached this
      session). Not verified against real ESP or Pico console traffic; the
      wire-protocol LOG-relay path for SAFETY (task 5, device SAFETY) is still
      unimplemented in firmware, so today's SAFETY capture only covers the
      probe-UART bench path — see `tools/PcTools/TODO.md` "Logging and consoles"

**Bench hardware now present, confirmed 2026-08-17:**
- [x] Raspberry Pi Debug Probe connected to A1 (Pico), SWD + UART bridge —
      programmed and verified `SaftyFW.elf` over it this session
      (`firmware/SaftyFW/docs/HARDWARE.md` §7b bench path)
- [x] Main board LCD (ILI9488, `DISPLAY` task) connected, boots, and shows
      "kilnCtl Ready"
- [ ] **HW change needed: backlight control.** LCD backlight currently has no
      GPIO/PWM control path from the board — add one (dim/off on idle,
      touch-driven wake). Owns: schematic net + pin assignment in
      `hardware/mainBoard/`, then `firmware/KilnFW/App/drivers/screen_idle.{c,h}`
      wiring once the pin exists
- [ ] **HW change: relay status LEDs.** Add indicator LED for each relay (K1–K4,
      S9), driven from the GPIO that controls the relay coil. Schematic + layout in
      `hardware/mainBoard/`
- [ ] **HW change: thermocouple board connectors.** Use different connector types
      for thermocouple boards (`ThermocoupleBoard`, `SafyThermocoupleBoard`) to
      distinguish from main board connectors. Schematic + footprints in respective
      board projects

## M2 — `CommonFW`, before either firmware depends on it

Owned by [`firmware/CommonFW/README.md`](firmware/CommonFW/README.md), gating items repeated in
[`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phase 1.

- [~] `kilnlink` target consumable by both pico-sdk and ESP-IDF — builds
      clean under MSVC and, as of 2026-08-16, under `KilnFW`'s real
      xtensa-gcc build too (`components/kilnlink/` wrapper, auto-discovered,
      linked-but-unused so far — `KilnCtrl.bin` size unchanged). **Not yet
      tried under pico-sdk/arm-none-eabi-gcc**, since `SaftyFW` has no CMake
      project; **not yet actually called** by `uart_protocol.c`
- [x] `KILNLINK_PROTOCOL_VERSION` the single source (2026-08-16); `KilnFW`'s
      `UART_PROTOCOL_VERSION` **not yet** switched to alias it — that edit
      belongs with the migration item below, not before it
- [~] Codecs pure and bounds-checked; host tests and `test/vectors/` — **the
      framing layer (`kilnlink_frame`/`kilnlink_crc`) and two of the payload
      codecs are done** (2026-08-18): `kilnlink_context.{c,h}` (ESP→Pico
      `SAFETY_CMD_PUSH_CONTEXT` 0x07, `LINK_PROTOCOL.md` sec 4, including
      `relay_recent_mask`) and `kilnlink_status.{c,h}` (Pico→ESP Frame A
      `SAFETY_CMD_GET_STATUS` 0x01, sec 6, the existing 23-byte layout). Host
      tests (`test_context.c`, `test_status.c`) and byte-exact vectors
      (`test/vectors/context_vectors.json`, `status_vectors.json`) pass under
      MSVC+CMake+Ninja. **Still not done**: sec 4's other ESP→Pico commands
      (`SET_FIRING_CEILING`, `CLEAR_TRIP`, `GET_FW_VERSION`, `SET_CLOCK`,
      `ANNOUNCE_VERSION`) and sec 6's other Pico→ESP frames (`DIAG`,
      `FW_VERSION`, `TRIP_EVENT`, `POWER`) -- their layouts are concrete in
      `LINK_PROTOCOL.md` too but weren't coded this pass. Not wired into
      either firmware yet (`uart_protocol.c`/`SaftyFW` migration is a
      separate item below)
- [x] `pc_tools` consuming the same vectors as the third implementation
      (2026-08-16, `selfcheck.py`, framing layer only) — **not yet extended**
      to `context_vectors.json`/`status_vectors.json`, left for a follow-on
      pass since `selfcheck.py` wasn't touched this session
- [ ] `KilnFW` delegating framing and CRC, proven byte-identical **before** the
      old code is deleted
- [x] CI grep: no CRC or byte-stuffing implementation outside `CommonFW` --
      `tools/check_no_duplicate_crc.ps1` (2026-08-18), matching
      `firmware/SaftyFW/tools/check_isolation.ps1`'s conventions. Greps for
      the kilnlink CRC-16/CCITT-FALSE polynomial (`0x1021`) outside
      `firmware/CommonFW`. Not registered in any CI pipeline (none exists
      yet for this check to join). Scoped to catch *new* untracked
      duplicates only: the 4 known pre-migration copies (`KilnFW`'s
      `uart_protocol.c` and its `UnitTestFw` mirror, `pc_tools`'
      `protocol.py` and its mirror) are an explicit, documented allowlist
      tied to the two items above -- deleting an allowlist entry is part of
      finishing each migration, so it can't go stale silently

## M3 — Safety processor to first trustworthy reading

Owned by [`firmware/SaftyFW/TODO.md`](firmware/SaftyFW/TODO.md) phases 2–4. Independent of the
link, so it can run in parallel with M1 and M2 once M0 is out of the way.

- [x] FreeRTOS SMP skeleton, tasks at the planned priorities and core affinities —
      done 2026-08-16, build-verified clean under the real arm-none-eabi-gcc/pico-sdk
      toolchain (task bodies are still TODO shells; Phases 3/6/7 fill them in)
- [x] `main()` drives GPIO6 low as its first statement — done 2026-08-16
- [x] Watchdog, fed only when every task checks in, trip reason latched in
      scratch registers — done 2026-08-16; the latch call itself is unwired
      until a guard exists to trigger it (Phase 4/5)
- [ ] **MAX31856 thermocouple ICs not yet connected on the bench (2026-08-18).**
      Blocks all real-reading work below it — SPI bus verify, per-channel type
      config, and fault-read all need the parts populated/wired first, not just
      the schematic net
- [ ] MAX31856 on J7, with per-thermocouple type configuration
- [x] Guards implemented and **host-tested against synthetic inputs**, no relay
      yet — 2026-08-18, `firmware/SaftyFW/src/safety_guards.c`. 12 of the 13
      guards in `SAFETY_MODEL.md` section 4 are built as pure functions and
      host-tested (320/320 checks, MSVC, `/W4 /WX`): S1/S5/S7/S11/S12 landed
      2026-08-16; this pass adds S2/S3/S4/S6/S9/S10/S13, fed by synthetic
      context/current-presence/link-liveness facts rather than real
      `link_task`/`current_task` producers (neither exists yet — Phase 6/7).
      S8 (rate-of-rise) is the one guard intentionally not built: it ships
      disabled per `SAFETY_MODEL.md` until a real kiln's ramp rate is
      measured on the bench, and building a placeholder threshold now is the
      mistake that section explicitly refuses to make. Build-verified under
      the real arm-none-eabi-gcc/pico-sdk toolchain too (`safety_guards.c`/
      `safety_core.c` compile clean, zero warnings under
      `-Wall -Wextra -Werror`) — the full `SaftyFW.elf` link currently fails
      on an unrelated, pre-existing, uncommitted bug in
      `firmware/CommonFW/src/kilnlink_status.c` (untracked in git, not
      touched by this pass; see `firmware/SaftyFW/TODO.md` Phase 4 for
      detail). No relay/GPIO6 wiring touched — guards remain evaluated but
      not commanding hardware, matching this milestone's scope
- [x] CI grep: `safety_core.c` never includes the link header — done 2026-08-16,
      `firmware/SaftyFW/tools/check_isolation.ps1`

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
- [x] **Establish the real flash size, then switch to it (2026-08-17).** Confirmed
      against the LonelyBinary product page for the board in hand
      (variant 43784065712285): it is an **N16R8 — 16 MB flash, 8 MB PSRAM**,
      resolving the three-way disagreement (buy lists said N8R8/8 MB, the 3D
      model said N8R2/8 MB). `firmware/KilnFW/sdkconfig` now declares
      `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` / `CONFIG_ESPTOOLPY_FLASHSIZE="16MB"`
      (was wrongly `_2MB`/`"2MB"`). **Scope of this pass: the sdkconfig
      flash-size declaration only.** `partitions.csv` is UNCHANGED — it still
      maps only the first 2 MB (single `factory` app slot, no OTA), with a
      header comment now noting the remaining ~14 MB is unmapped headroom for
      the item below. The real
      image is 1167 KB, so two copies do not fit in the 1500 KB app region. The
      new two-app-slot OTA partition table this milestone describes is still a
      separate, unstarted item — see the next checklist entries. The bootloader
      must still be reflashed for the corrected header flash-size to take
      effect on the physical board; this pass is config/doc-only and has not
      been flashed or build-verified against real hardware
- [x] `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, and the app confirms itself only
      after NVS, safety link and web server are up (2026-08-17, already on
      `main` before this pass — `partitions.csv` two-app-slot table +
      `ota_rollback_confirm_task()` in `App/main.c` gating on
      `nvs_ok && web_ok && link_up`; host-build-verified, not yet
      hardware-flashed)
- [ ] Pico flash layout and metadata format frozen before the first board is
      programmed; signature field and key space reserved even though signing is off
- [ ] Pico bootloader: GPIO6 low first, active slot CRC'd every boot, recovery
      mode over UART1 with no timeout out of it
- [ ] **Mutual protocol-version check** (lands with M5, gates this): each side
      verifies the other, a mismatch blocks heating on the ESP and puts the Pico
      in `DEGRADED_NO_CONTEXT` without latching a trip. `ANNOUNCE_VERSION`
      itself still unbuilt (`KilnFW/TODO.md` 9.0)
- [ ] **Compatibility floor** frozen so a version mismatch can never disable the
      update path itself — otherwise every mismatch needs a debug probe
- [x] Image header validated before the first erase, so a wrong-target upload
      cannot erase a slot (2026-08-17, already on `main` — `ota_esp_do_transfer()`
      checks `esp_image_header_t` magic + chip ID before `esp_ota_begin()`)
- [x] Challenge–response on the AP password, so it never crosses the wire;
      lockout after 3 failures (2026-08-17, already on `main` —
      `GET /api/ota/challenge`, PSA Crypto HMAC, `App/drivers/ota_http.c`)
- [x] Both paths refused unless the kiln is idle and cool, with the specific
      blocker named (2026-08-17, already on `main` — `ota_interlock_check()`
      names the zone/reason, e.g. `"zone 2 is at 340 C"`)
- [ ] Link-loss heating block **not** bypassed during a Pico update — alarm text
      suppressed, never the block. **Correctly untouched, not yet verifiable**:
      `relay_authority.c` is unmodified (right, per design) but there is no
      Pico-update transfer path wired up yet to exercise this end-to-end
      (`KilnFW/TODO.md` 9.4)
- [x] **Four MCP tools (2026-08-18).** `tools/PcTools/src/kilnctrl/ota_http_client.py`
      (pure HTTP client: challenge fetch, HMAC derivation, streamed push,
      status poll) plus four `mcp__kilnctrl__` wrappers in `mcp_server.py`:
      `ota_get_challenge(host)`, `ota_update_esp(image_path, password, host)`,
      `ota_update_pico(image_path, password, host)`, `ota_status(host)`.
      Board discovery mirrors `gui.py`'s `_wifi_default_host()` (UART
      `wifi_get_status()`'s station IP, falling back to the AP default
      `192.168.4.1`; an explicit `host` always wins). Request construction /
      HMAC signing / response parsing unit-tested against mocked HTTP
      (`tools/PcTools/tests/test_ota_http_client.py`, 15 tests, no live
      board). **Real gap, surfaced honestly rather than papered over**:
      `ota_status()` can only report the Pico relay's progress
      (`GET /api/ota/pico/status`) — there is no HTTP route for the ESP
      self-update's own progress counter or the persisted `ota_record` "last
      update" blob, both of which exist only as in-process C getters today.
      **Not yet exercised against a physical board** — no hardware attached
      in this pass's environment; live-board verification (real interlock
      refusals, real lockout, a real Pico relay) is still outstanding.

---

## Decisions taken, so they are not re-litigated

| Decision | Date | Where the reasoning lives |
|---|---|---|
| Pico bench path is the Debug Probe: SWD plus its UART bridge on GP16/GP17. **The Pico's own USB is not used.** | 2026-08-16 | `firmware/SaftyFW/docs/ARCHITECTURE.md` §1 |
| **PSRAM stays disabled** on the ESP32-S3. Not an oversight — nothing needs it, and it costs determinism, a boot failure mode and a DMA audit | 2026-08-16 | `firmware/KilnFW/TODO.md` §9.1a |
| Library paths use `${KIPRJMOD}/../lib`, not a KiCad path variable | 2026-08-16 | `docs/REPO_LAYOUT.md` B1 |
| OTA authentication is challenge–response on the AP password, never a form POST | 2026-08-16 | `firmware/CommonFW/docs/UPDATE_PROTOCOL.md` §2 |
| Update frames and the version handshake are a **frozen compatibility floor** | 2026-08-16 | `firmware/CommonFW/docs/LINK_PROTOCOL.md` |

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
