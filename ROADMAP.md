# kilnCtl Roadmap — both processors

> **Status:** planning · **Last reviewed:** 2026-09-04 (M15 opened: architecture
> review findings) (reconciled against
> the five KilnFW plan docs; M11 closed; M12a opened
> and closed the same day; M12/M13 in progress; `DISPLAY_ST7796_PLAN.md`
> Phases 1/2/3/5 landed, Phases 4/6 in progress; ramp assist landed end to end
> default OFF; board reflashed 2026-09-03 07:36:20 and a coupling-matrix A/B
> is running on it now)
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
Six of the nine questions in this section were answered on 2026-08-28 and have
become work rather than questions — see [M12](#m12--commissioning-the-operator-can-actually-do--opened-2026-08-28).
What is still genuinely open is short:

| Size | Item | Where |
|---|---|---|
| S | Physical zone arrangement — which element is where | [What is actually left](#what-is-actually-left) |
| S | The deferred sanity rate for S8 — S8 gained a pure-module implementation and is integrated (2026-09-03, `safety_guards.c`/`safety_core.c`, `SaftyFW/docs/GUARD_TEST_MATRIX.md`), but ships deliberately off (`max_rate_c_per_min` defaults to 0, same "no default by design" shape as S1), so commissioning that field is what will enable it | M3 |
| S | `hardware/UnitTestFixture/` — delete or keep | M7 |

### Software, doable now — no hardware, no decisions

| Size | Item | Where |
|---|---|---|
| L | **The HTTP reset is not a heap failure** — proven, not argued. Remaining candidates are lwIP or `esp_http_server`'s accept/select loop under `max_open_sockets=13`, which needs a different instrumentation surface | M10 |
| L | **HTTP connection resets under concurrency.** TCP-layer instrumentation built and live; 188 requests across varied burst sizes reproduced nothing (rate appears lower than the original 9/80 measurement, unconfirmed why). Still unreproduced under instrumentation, not root-caused, not closed — an absence of failure is not a fix, see M10 for the honest accounting | M10 |
| L | **Every fault says what was detected and what to do** — a standing rule, not a closing milestone, so it never fully closes: applies to every fault surface added from here on. All of S6a's own checklist items landed 2026-08-28 | M13 |
| L | ~~**An uncommissioned safety processor must refuse heating enable.**~~ Landed `5cd56b6`. Its predicted side effect arrived exactly as the ordering note warned — the bench, having no CT fitted, could not satisfy `ct_channel_map` and was locked out of heating. Resolved 2026-08-28 by making CTs **optional hardware** rather than by relaxing the gate: `ct_installed` (param `0x0109`) is a new ASKED commissioning question, and answering *no* drops the CT-map requirement **and** switches S3/S4/S9/S14 off while reporting them off (`SaftyFW/docs/GUARD_TEST_MATRIX.md` §9). Verified on the live board: `commissioned: true`, heat permitted, relay commanded with `heat_blocked: false` | M12 |
| ~~L~~ | ~~**Sustained heat is impossible on the bench: S6a fires within ~1 s of every firing start.**~~ **CLOSED 2026-08-29 (63cc741).** Root cause was not the wire, not the Pico, and not the consumer-cannot-keep-up shape this entry guessed at — nothing was ever dropped: 0 CRC errors, 0 resyncs, 0 discards throughout, the frames were only ever *late*. `KilnFW`'s `uart_protocol_rx_task` read with `uart_read_bytes(port, chunk, sizeof(chunk), 200 ms)` where `chunk` had been sized to the worst-case stuffed frame (528 B) by 3149393. That call re-blocks until `length` bytes arrive or the timeout expires, and on a link whose frames are ~40 B at ~10 frames/s, 528 B never arrive — so every read held its bytes the full 200 ms, against a ~345 ms reply budget. By the time it was characterised it was timing out on **100 %** of polls (42/42), not 20 %. Fixed by reading only what `uart_get_buffered_data_len()` reports with a zero timeout, and blocking for a single byte (bounded 100 ms) only when nothing is buffered. Measured after: **0 timeouts in 340+ polls**, request/reply back to 1:1, config fetch converging in 3 page requests instead of 25. A bounded firing run now holds `running` with no heat block for a full 60 s sweep (regression-asserted in `test_live_bench_tuning.py`). DMA was evaluated and rejected as the fix — see `LINK_PROTOCOL.md` §3 "Never wait on a receive buffer filling", which states the rule both ends must hold and leaves the mechanism open. ~~**Remaining bench limit is physical, not firmware: no heating element is fitted to the zone-0 relay, so PV does not move and autotune still cannot fit a response.**~~ **Wrong, corrected 2026-08-29.** Every zone has a real heater. PV did not move for two firmware reasons, both since fixed: K4 was never requested (`heat_enable.c`), and zone 0's 2 s time-proportioning window could not render any fractional duty against the 10 s minimum on-time. "It must be the hardware" was the third wrong diagnosis this one symptom attracted. | M6 |

### Blocked on hardware that does not exist yet

| Size | Item | Where |
|---|---|---|
| S | The CT coupling transformer (Hammond 140QEX): one look at the PDF before ordering — 10.62 H is quoted at 1 kHz and applied at 60 Hz | M5 |
| S | Time the link-staleness ceiling (1.5 s) and the firing abort (30 s) with a stopwatch. Code is flashed; nobody has held the link down | M6 |
| M | S9's welded-contactor escalation — by definition needs a welded contactor. **Checked 2026-09-03: SimFW cannot do this — SimFW itself no longer exists** (removed `8553244`, 2026-08-28; `firmware/UnitTestFw` took its place and is unrelated ESP32-S3 bench-instrument firmware — DAC/AD9833/OLED/PCF8575 — with no path to the safety processor's current-sense input at all). Even when SimFW existed, its own removal commit records that `ct_calibration` "needs the fixture to physically drive current into the CT" — S9 (`firmware/SaftyFW/src/safety_guards.c:363-389`) latches only on real `any_current_present`, gated by `in->context_valid`, `in->current_sensing_commissioned` and NOT `in->current_sensing_disabled`; that flag comes from the CT's analog current-transformer signal through `current_sense.c`, not a GPIO a simulator MCU could assert. What would actually be required: a fixture that injects genuine AC current through the CT sense loop while the K4 drive line is confirmed de-energized — i.e. a hardware jig, not firmware simulation — plus a CT actually fitted and commissioned (`ct_installed=yes`; on the bare bench today `ct_installed=no` switches S9 off and reports it off). | M4 |
| M | AP-fallback verified end to end (needs a router with correct *and* deliberately-wrong static config) | M6 |
| M | Per-channel CT-to-jack commissioning, plus a bench measurement of the ADC noise floor under the 25-count presence fallback | M5 |
| M | **HW changes:** LCD backlight control (fix scoped in `DISPLAY_ST7796_PLAN.md` §3.4.1: one flying wire, GPIO15/16 to module pin 8 — needs the second-panel harness to carry it), relay status LEDs for K1–K4/S9, distinct connector types for the thermocouple daughterboards, I2C broken out on an expansion connector | M1 |
| S | **Blocking, before the MSP4031 touches J2 at all**: meter module pins 10/12 (CTP_SCL/CTP_SDA) at 5V — confirms or clears a hazard that can back-feed the SX1509/ESP32-S3 through the shared I2C bus. `DISPLAY_ST7796_PLAN.md` §4 | M1 |
| M | DEBUG header and GP16/GP17 access before A1 is soldered down | M0 |
| L | Field updates exercised against real hardware: Pico bootloader over a live UART1, an actual OTA into `ota_0`/`ota_1` (a JTAG flash boots `factory` and never runs the rollback machinery), a real version mismatch | M8 |
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

Still genuinely unanswerable by the code:

1. Physical zone arrangement (which element is where).
2. The deferred sanity rate (S8's rate-of-rise ships disabled until a real
   kiln's ramp is measured — S8 itself gained a pure-module implementation
   and integration on 2026-09-03, so this is now a commissioning gap, the
   same class as S1's `abs_max_temp_c`, not a missing guard).
3. `hardware/UnitTestFixture/` — delete it or keep it. `firmware/UnitTestFw`
   went on 2026-08-23; its embedded KiCad project was out of that change's
   scope. Board files are off-limits without your say-so.

**Blocked on hardware that does not exist yet.** All of this is scripted and
waiting, not unwritten:

- `GUARD_TEST_MATRIX.md` §3's trip rows — every enabled guard's real trip,
  safe-state power-on, sensor open-circuit, current-mapping commissioning.
- ~~The safety processor's own MAX31856~~ — **fitted 2026-08-24 and verified
  reading 30.2 °C.** Still absent: any CT, so `0.00 A` on all three channels
  remains correct reporting of absent hardware (M5).
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

- ~~**M11's remaining items are now the LCD consolidation only.**~~ **Stale,
  corrected 2026-09-03: M11 is fully closed.** The LCD consolidation items
  (temperature-page relay toggle, diagnostics/safety/thermocouple-fault
  paging, planned-profile preview) also landed and are checked off in
  [M11](#m11--the-ui-the-owner-actually-asked-for--opened-2026-08-28) itself.
- ~~**The LCD back buttons do not work**~~ **CLOSED (`1982ed6`).** Root cause
  was the topbar's z-order-first-match hit test: icons are built left-to-right
  (Back, Home, Prev, Next, Gear) so every icon except the last in a row was
  shadowed by whichever came after it, and Back was *always* shadowed since
  something always follows it. Fixed by capping the touch-area extension at
  `UI_THEME_PADDING_PX/2` per side (`ui_theme_apply_touch_area()`) and
  registering the icon row as a touch group (`ui_topbar.c`).
- Diagnose the HTTP concurrency reset above — it has a reproducer and two
  ruled-out mechanisms, so the next step is instrumenting the failing
  allocation, not more black-box testing.
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

**Closed 2026-08-27/28, no longer open:** the task stacks were re-read after a
real firing and 4 kB of internal DRAM reclaimed; `rules_task`'s callees were
audited (nothing writes flash — the real finding was a cache-disabling *read*
via `dashboard_get_status()`) and its stack moved to PSRAM; the guard scripts
now run from `tools/run_all_checks.ps1` and the `run_repo_checks` MCP tool.

**Closed 2026-08-30, no longer open:** `PID_EXPANSION_PLAN.md` Phases 1-4 are
landed — Cohen-Coon is a selectable, reachable tuning rule alongside SIMC, and
the fuzzy-PID layer (`pid_fuzzy.c`) is wired into `profile_executor.c` and
selectable from `zones_page.html`. SNTP/NTP time sync landed. The first
autotune runs ever to complete on real hardware fitted all three zones, and
the first full 3x3 cross-zone coupling matrix and RGA were measured — see that
plan's §2 for the current numbers (that matrix has since been superseded). Proposed gains reviewed, not accepted.

**PID/adaptive tuning, as of 2026-09-02** (detail owned by
`PID_EXPANSION_PLAN.md`, not duplicated here): coupled identification and the
diagonal model refine are built and hardened, but their hardware clearance was
**withdrawn 2026-09-02** — the solve is sound, the harvest layer feeding it
records non-steady duties as DC-gain observations (fired on 12 of 12 joint
observations on the `coupid6` capture). Never yet run on the kiln. The Ki diagnosis layer is built with
all code blockers closed — also never yet run on the kiln. Dynamics-from-ramps
was tried and **shelved**: its two-point fit reduces analytically to
`0.524·K·Δduty/ramp_rate`, an artifact of the commanded ramp rate with no plant
content. A real board-wide deadlock was found and fixed — pressing Accept on
an autotune result over the UART bridge re-entered the flash worker and hung
it permanently, taking down every UART bridge, `safety_cfg_store`'s deferred
NVS flush, and `adaptive_tune`'s persistence with it. All adaptive layers stay
per-zone opt-in, default OFF.

**Closed 2026-08-31/09-03, no longer open — ramp assist, landed end to end**
(detail owned by `PID_EXPANSION_PLAN.md` §7, not duplicated here): a pyrometric
cone table (`cone_table.c`/`.h`, Orton 022-14 incl. half-cones, Arrhenius
heat-work weighting), sustained-lag detection and auto-stretch in the
executor, and a dwell heat-work credit that shortens the following dwell when
it was earned lagging — the SPEND is gated on `ramp_assist_enabled` so
disabled behaviour is bit-identical to before this landed (`profile_executor_
ramp_assist.c`'s accrual/report side is unconditional; only the spend checks
the flag). Web banner, event log, and LCD lag notice all wired to the same
richer sustained-lag fields. Control surface (`GET`/`POST /api/ramp_assist`,
diagnostics-page toggle, persisted kiln-wide) is done and **defaults OFF** —
it stays off until a real cone-temperature firing validates it (see GATED,
below). A same-day defect sweep found and fixed a duplicated band-width
formula, a band-cliff bug present in `cone_table.c` itself (not just its
caller), and a dwell-credit crash, plus ten wrong cone temperatures in the
Orton table (mirror test added so a future table edit can't repeat it).
**Further hardening since, still pre-real-firing:** dwell credit was found
**unreachable** — its gate was tied to the 25 °C ramp-lock band instead of
schedule lag, so it could never fire — and fixed (`cf3763c`); accrual was
then extended to keep crediting past the nominal ramp end (`0402ecb`,
KilnFW + simulator). Separately, three ramp-lock/guard interaction bugs
surfaced and were fixed: a hot-start stall (one-sided lock + a guard-4
arming backstop, `8f12449`), a false guard-4 trip during autotune's
SETTLING phase that the backstop itself introduced (`7911f26`), and guard 4
mistaking autotune's synthetic setpoint for a real one (`1bfd5ee`). None of
this changes the GATED verdict below — a real cone-temperature firing is
still the only thing that flips ramp assist's default to ON — but it removes
failure modes that would otherwise have surfaced mid-firing.

**Also closed in the same window, no longer open:**
- Relay-autotune thermal guards 1/2 now use an amplitude discriminator
  (`relay_min_swing_c`) in place of a directional test that could never fire
  mid-limit-cycle — `df3b31b`.
- A coupled-solve `use_measured_diag_k_dc` flag shipped, **default OFF**.
- Measurement tooling: `noise_floor.py` gained a start-temperature covariate
  and cooldown-sidecar refusal; `pid_ab_compare` now gates on a start-temp
  confound instead of ignoring it.
- `run_queue.py` hardening: capture opens before the start POST, a real
  wait-until-actually-finished replaces the old "first idle sample = done"
  logic (`0a0ccd7`; the once-proposed `docs/patches/run_queue_idle_stop_fix.*`
  patch was superseded by this different fix and has been deleted), campaigns
  are resumable via a durable state file, a clobber is refused rather than
  silently overwritten, and a capability preflight fails a stale-firmware
  preset before the campaign starts rather than mid-run.
- Web UI: firing-flow profile feasibility warnings, a live flash partition
  table on the diagnostics page, a new-profile segment preview graph, the
  duty axis in percent with a rotated label, a falling-behind-schedule
  banner, and RGA/status cells that no longer rely on colour alone (plus an
  extended colour-only checker guard script).
- `PID_EXPANSION_PLAN.md`'s fuzzy-layer hardware-run blocker (a
  `zones_http_client` field-mapping bug) is resolved (`b1ea749d`) — the run
  itself still has not happened, see GATED below.

**What is currently GATED, and on what** (the short answer for planning):

| Item | Gated on | Where |
|---|---|---|
| DRAM/PSRAM allocator-threshold work | A full soak (cold firing through cooldown) plus a Pico OTA relay-path measurement that has never been taken | `DRAM_PSRAM_PLAN.md` §5/§6/§9 |
| Second LCD panel (ST7796/MSP4031) | The physical panel, and its pre-power STOP-block 5V I2C hazard check before the module ever touches J2 | `DISPLAY_ST7796_PLAN.md` §0/§4 |
| Ramp assist default (OFF → ON) | A real firing at cone temperatures — everything measured so far is bench-range (0–80 °C), well below where the cone table's heat-work weighting matters | `PID_EXPANSION_PLAN.md` §7 |
| Fuzzy-PID layer's first above-zero hardware run | Nothing named now — its last blocker (a field-mapping bug) is fixed and the run is available; it just hasn't been run yet | `PID_EXPANSION_PLAN.md` §3.6 |

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
- [ ] **HW change: LCD backlight control.** The firmware side landed
      2026-09-03 (commit `ad35720`): `App/drivers/backlight_pwm.c/.h`, an
      LEDC PWM driver polling `screen_idle_get_state()` and mapping
      screen-on/idle to duty, wired into `App/main.c`, host-tested
      (`App/test/test_backlight_pwm.c`), behind default-OFF
      `CONFIG_KILNCTL_BACKLIGHT_PWM_ENABLE` — same anticipatory/default-off
      posture as `DISPLAY_ST7796_PLAN.md`'s 9.3/9.4/9.6/9.7. What remains is
      purely hardware: the flying wire (GPIO15 or GPIO16 to module pin 8),
      scoped in `DISPLAY_ST7796_PLAN.md` §3.4.1 to ride along with the
      second-panel harness, is still **not fitted**, and stays gated on the
      STOP-block 5V I2C hazard measurement in that plan's §4 before any
      harness is connected. Pin-by-pin wiring sheet, plus the R4/R6 module
      rework touch requires: `firmware/KilnFW/docs/DISPLAY_ST7796_WIRING.md`.
      Firmware is **not flash-verified** — it was written ahead of the
      hardware, so first enable must confirm the panel actually dims.
- [~] **Second LCD panel (ST7796/MSP4031), auto-detection, display SPI
      async/DMA.** `firmware/KilnFW/docs/DISPLAY_ST7796_PLAN.md`, sequenced
      Phase 0 (bench facts/hazard measurement) through Phase 7 (UI). Phases 1
      (single-owner cleanup, bounded SPI-owner timeout — also closes
      `TODO.md`'s spi_owner unbounded-wait entry), 2 (panel codec extracted),
      3 (`panel_spi`/`st7796_panel` split, Kconfig-selectable) and 5 (FT6336U
      touch abstraction, unvalidated on hardware) landed 2026-09-01/02.
      Phase 4 (auto-detection) is done ahead of hardware: the detection logic
      is host-tested and wired into `panel_spi.c`, and is correctly inert
      today because both descriptors' `id_matches` stay NULL — the ILI9488's
      Sec.4 RDDID bytes were captured 2026-09-03 (`0x00 0x00 0x00`, MISO
      undriven, i.e. permanently unmatchable) and the ST7796's have never
      been read since the module has not touched J2. Phase 6 (SPI DMA) is
      also done ahead of hardware: 9.2/9.5/9.9 landed; 9.3 (PSRAM DMA), 9.4
      (hardware CS), 9.6 (async flush) and 9.7 (zero-copy flush) landed
      compiled-in but default-OFF behind their own Kconfig symbols; 9.1's
      instrumentation landed (`flush_last_us`/`flush_max_us`/`flush_count` on
      `GET /api/status`) though 9.1b's number is not yet recorded live; 9.8
      needed no code. Phase 0's blocking hardware measurements (STOP-block 5V
      I2C hazard, ST7796 RDDID bytes) are still open, and the module has not
      yet touched J2. The only hardware change required remains a custom
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

## M7 — Repo reorganisation · *done 2026-08-16, two items open*

Owned by [`docs/REPO_LAYOUT.md`](docs/REPO_LAYOUT.md). Tree split into
`hardware/`/`firmware/`/`tools/`/`docs/`, library tables and submodule paths
fixed, fresh-clone `mainBoard` open confirmed 2026-08-19.

- [x] `pdfMcp/` moved under `tools/` — 2026-08-28. Its own running `pdf-mcp.exe`
      process blocked a plain rename the same way `mykicadMcp`'s would, so
      `tools/pdfMcp/` is a copy, not a move; `.mcp.json` updated to the new
      path. The stale root-level copy cleans up on the next session restart,
      once nothing holds it open
- [x] `mykicadMcp/` moved under `tools/` — 2026-08-28, as its own dedicated pass
      per the plan above: stopped the `kicad` server (`mcp_servers.ps1 stop
      -Server kicad`), moved the submodule (a directory-rename `git mv` hit
      the same "Permission denied" this repo's original hardware/firmware
      split ran into — worked around the documented way, pre-creating the
      destination and moving children individually; one stale abandoned
      `.claude/worktrees/` leftover from an unrelated old session couldn't be
      moved and was left behind, harmless debris, not part of the submodule's
      tracked content), fixed the submodule's own `.git` gitdir pointer and
      `core.worktree` for its new depth (the actual cause of a first attempt
      silently re-adding it as 43 individual file blobs instead of one
      gitlink — caught by `git ls-files -s` showing `100644` entries instead
      of a single `160000`, not assumed away), updated `.gitmodules`,
      `mcp_servers.ps1`, and all 8 of the 9 `.claude/settings.json` allowlist
      entries with an unambiguous path (the 9th, `../mykicadMcp/...`, has no
      recoverable original working directory to translate against and was
      left to simply stop matching — the safe direction, a future prompt
      rather than a silently wrong grant). Verified: `git submodule status`
      resolves all three submodules, the `kicad` server restarted clean from
      the new path and answered a real `kicad_call`, and the submodule's own
      110-test suite passed unchanged from its new location
- [ ] `hardware/UnitTestFixture` KiCad project still unopened (the other three
      projects were confirmed clean 2026-08-16)

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
- [ ] Link-loss heating block **not** bypassed during a Pico update — correct
      by code inspection on both sides (`relay_authority.c` untouched since
      2026-08-13; `update_task.c` only reads output/thermo status to gate
      `UPDATE_BEGIN`, never writes relay/GPIO state), but not yet exercised on
      real hardware
- [x] Four MCP tools for OTA (challenge, ESP update, Pico update, status),
      plus explicit ESP and Pico rollback and a web `/ota` page — built and
      unit-tested against mocked HTTP (2026-08-18–19); not yet exercised
      against a physical board
- [x] `SAFETY_CMD_ANNOUNCE_REBOOT` sent before a routine ESP reboot so a
      firmware update doesn't trip S6(b) — 20 s grace window, host-tested;
      not hardware-verified (no board attached to confirm a real reboot
      suppresses the trip)

## M11 — The UI the owner actually asked for · *opened 2026-08-28*

Owned by [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) and
[`firmware/KilnFW/docs/UI_PLAN.md`](firmware/KilnFW/docs/UI_PLAN.md). A batch of
direct requests from the repository owner, recorded here because several of
them change data structures and one of them deletes a subsystem — this is not
cosmetic work and should not be filed as such.

The through-line: **stop making the operator repeat themselves, and stop
spreading one subject across several pages.**

- [x] Manual relay-control web page removed; the diagnostics Danger Zone is
      the sanctioned hand-control. `POST /api/relay` went with it — grep found
      the page was its only caller, and both the LCD and the Danger Zone call
      `dashboard_set_relay()` in-process, so the shared ownership/safety gate
      is untouched
- [x] Board health and Thermocouple faults folded into `/diagnostics`; both
      standalone routes deleted. Board health turned out to be a byte-for-byte
      duplicate of a card already there; the thermocouple-fault content was
      entirely unique and was carried over whole, including the
      ABSENT-vs-FAULTED distinction that is the entire point of that page
- [x] Safety timings / Safety processor / Safety commissioning are now an
      expanding nav group, not three flat entries and not a landing page
- [x] Firing profiles is the first menu item
- [x] Shared **safety timing profiles**: the nine per-zone timing fields moved
      into named profiles that zones point at, so identical zones are
      configured once. `ZONES_CFG_VERSION` 8→9, lossless migration that
      de-duplicates identical value sets into one shared profile
- [x] **Relay/IO segments in firing profiles**, blocking or non-blocking, with
      a per-segment choice of whether the relay is left in its last state at
      run end. `PROFILE_VERSION` 2→3 (`73c03c0`). Migration verified against
      the owner's live board: both saved profiles came back byte-identical
      with the five new fields defaulting to the old ramp behaviour
- [x] **Rules engine deleted** (`56dfa07`) — `rules_http.c`, `rules_task.c`,
      `rules_eval.c`, `rules_page.html` and their two test files, plus
      PcTools' "Relay Rules (HTTP)" popup, which would have 404'd on first
      use. Sequenced deliberately AFTER segments so there was never a window
      with no way to drive a non-zone relay. The one thing that had to be
      *replaced* rather than dropped is `rules_task`'s own watchdog:
      `profile_executor.c`'s guard-9 watchdog now force-offs every segment via
      `io_segs_force_all_off(false)`, where the `false` means a segment's
      `leave_on_at_end` is ignored and the contacts always open.
      `RELAY_OWNER_RULE` stays in the enum unrenumbered and marked retired, and
      the stored `rules_cfg` NVS blob is left orphaned — both so a stale value
      is never silently reinterpreted as something else. Flash-verified: relay
      4, which rule R0 had been holding closed at ambient, came up open
- [x] Names for relays not assigned to a zone (`bc3f7ad`) — a separate NVS key
      rather than more bytes in `zones_cfg_t`, which had none to give
- [x] LCD: temperature page stops offering a manual toggle for zone-assigned
      relays (visible, not hidden — removing the control, not the reading),
      keeps it for non-zone relays
- [x] LCD: safety-processor, board-health and thermocouple-fault pages folded
      into the LCD diagnostics page and removed; kiln setup, thermocouple types
      and kiln config pages removed; profiles moved to the top-left of the main
      menu and the whole menu scaled to one page. Diagnostics is now six
      Prev/Next-paged screens under one nav item — the honest way to combine
      three pages' content without scrolling or silently dropping any of it
- [x] LCD: a planned-profile preview. The LCD could only draw a planned curve
      for a profile ALREADY RUNNING (`ui_page_home.c:843` gates it on
      `state != IDLE`), so a profile could never be previewed before firing it
      the way `/profiles` allows on the web. The preview went on the
      profile-detail page rather than the home chart, which stays coupled to the
      running executor's snapshot and keeps its IDLE guard intact

**The constraint that shapes most of this**: `zones_cfg_t` is 500 bytes
against a hard 512-byte `ZONES_CONFIG_BLOB_MAX_SIZE`, and the timing-profile
work spent the slack getting there (it had to reorder `zone_cfg_t` to kill
alignment padding and cut the profile-name length to 7). Anything that wants
to persist more per-zone or per-relay state now has to find the bytes or take
its own NVS key. That cap is also `kiln_cfg_store.c`'s buffer size, so it is
not a free knob.

**And the hazard every item here shares**: these structs are persisted, and
two of them embed arrays by value (`zones_cfg_t.zones[]`,
`profile_t.segments[]`), so adding one field to an element displaces every
element after the first. Each migration needs a FROZEN snapshot struct of the
old layout and a field-by-field walk. `profiles_http.c` has already shipped
the version of this that returns `sizeof` the *current* struct for the *old*
version — it rejected every profile on the owner's board and marked them
unused, and only a hardware flash caught it.

## M12a — The commissioning surface lied · *opened and closed 2026-08-28*

Found by an opus audit on 2026-08-28, triggered by a routine attempt to set
`abs_max_temp_c = 80` on the bench. **Three `POST /api/safety/commissioning`
requests returned `{"ok":true}` and not one value changed**, including a control
write to a harmless parameter. This is not a UI defect. It means the safety
processor's commissioning surface reports success it has not earned, while
displaying values that did not come from the safety processor.

The decisive evidence, all live: `live_config_crc` never moved (a successful
`config_store_write()` bumps `seq` and therefore the CRC, so **nothing was
written**), while the Pico's own histogram read `commit_config_rejected=2` —
it refused, it said so, and the ESP discarded the refusal.

Four defects, each verified against source:

1. **`ok` cannot fail.** `apply_pairs()` returns true when the send returns
   `ESP_OK` (`safety_cfg_http.c:434/446/472`), but SET_PARAM and COMMIT_CONFIG
   both go out as broadcasts (`safety_link.c:2944/3027`) and
   `uart_protocol.c:795` returns `ESP_OK` for "the local UART accepted the
   bytes" — its own comment says "No ack wait, no retry". The doc comment above
   `safety_link_send_commit_config()` still claims `ESP_OK` means the Pico
   ACKed; that has been untrue since the broadcast change.
2. **The rejection is caught in a ~144 ms race and then thrown away.**
   `safety_link.c:3045-3059` waits `SAFETY_LINK_REPLY_TIMEOUT_MS`; a late
   REJECTED frame reaches `safety_drain_inbox_ex()` (`:1367-1373`) and is
   counted and dropped. `CONFIG_PAGE`, two cases above (`:1353-1364`), stashes
   an unclaimed frame — REJECTED has no stash. **Silence is currently defined
   as acceptance** (`:3044`).
3. **The GET is an ESP-local NVS cache presented as current.** Refreshed only
   when the CRCs disagree (`safety_cfg_store.c:674-675`); they agree, so there
   were zero fetches this boot (`cmd_config_page_count: 0`). `fetched_ms_ago`
   is board uptime, not fetch age (`:441-443`) — it says "fetched 10 minutes
   ago" about bytes read off flash, possibly written by a different Pico image
   days earlier.
4. **`"set": true` is unconditional** — the Pico emits every field without
   consulting `rec.fields_set` and the ESP sets `set = 1` for every entry
   received (`safety_cfg_store.c:609`). So `abs_max_temp_c {set:true,
   value:0}` is the page asserting a *commissioned ceiling of 0 °C* for a field
   the Pico has never had set, and 0 on that field means NEVER TRIP. This makes
   `safety_cfg_http.c:136-139`'s deliberate "omit the value rather than print a
   misleading zero" branch unreachable. **Fifth instance** of a report that
   structurally cannot be false.

- [x] Stash an unclaimed REJECTED frame the way CONFIG_PAGE is stashed
- [x] **Confirm commits positively by reading `config_crc` back.** The audit was
      right that a stash alone leaves `ok` meaning "no rejection seen"; the
      read-back compares every submitted field against what the Pico returns
- [x] Refetch after commit and report the CONFIRMED value, not the sent one
- [x] Carry `fields_set` through the CONFIG_PAGE codec so `set` can be false —
      protocol 7→8, floor held at 7, and the ESP now refuses to call anything
      "set" when the peer is older than 8 **or its version is unknown**
- [x] `fetched_ms_ago` reports real fetch age; an NVS load no longer stamps it
- [x] Surface the ARMED/GRACE write window in the page itself

**Closed 2026-08-28, verified on hardware (`ddbd024`, `3149393`).** The page now
reports `abs_max_temp_c {set: false}` where it used to report a commissioned
0 °C, `cached_config_crc == live_config_crc` with `stale: false`, and a refused
write says so instead of returning `{"ok":true}`.

**And fixing it immediately exposed an older defect it had been hiding.** With
the read-back real for the first time, page 0 of the config fetch arrived and
**page 1 timed out on every attempt** — so the processor still could not be
commissioned. `cmd_config_page` had been **0** for the life of this project:
the ESP had never once fetched a page, so a page-1 timeout had nothing to
surface through. The cause was not the Pico, which answered in under 1 ms and
dropped no frame: `uart_protocol_rx_task` read UART bytes 32 at a time, so a
~157-253 byte CONFIG_PAGE needed 5-8 scheduler round trips against a ~145 ms
budget while the small STATUS/DIAG/POWER frames that always worked needed 1-2.
Fixed by reading a whole frame in one go and giving the reply 300 ms of margin
— the 2000 ms multi-page ceiling is untouched, because growing *that* is what
caused an earlier panic-reboot regression. Two more bugs surfaced in the same
instrumentation: `link_task` polled its RX ring once per 100 ms (now 10), and
the pre-send drain discarded a CONFIG_PAGE for the wrong index instead of
stashing it, so a late page-1 reply could never be rescued.

**`abs_max_temp_c = 80` is now committed and confirmed on the bench — S1's
absolute ceiling is armed for the first time in this project.** The board still
reports `commissioned: false`, honestly: `tc_source`, `borrowed_zone_index` and
`tc_placement_mode` remain unset. The dangerous half is closed; the descriptive
half is what M12's four-question flow collects.

**And the constraint this uncovered, which shapes M12's whole design:** config
writes are refused whenever the relay owner is `ARMED` (`config_store_flash.c:279`),
and ARMED is the steady state ~60 s after boot. **The only write window is the
boot GRACE period.** Commissioning today means resetting the Pico and
committing within 60 seconds, and nothing in the UI, the API, or the error text
says so. That is not a workflow an operator can be handed.

**Net effect: S1's absolute overtemperature ceiling cannot be commissioned
through the shipping web surface**, and every attempt outside the window
reports success. The heating elements are physically connected to this bench.

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

- [x] Decode the fault-source bitmask wherever an S6a trip is reported — web,
      diagnostics, LCD. Through a shared table: `safety_trip_words.h` exists
      because the LCD and web had already drifted into showing different things
      for the same trip, and a second copy of the names would repeat that.
      Verified 2026-08-28 by reading both surfaces directly rather than
      trusting the landed list below: `dashboard_http.c:741-744` (web) and
      `ui_page_diagnostics.c:675-722` (LCD, `ds.trip_reason == 6u` branch)
      both decode via `safety_fault_source_words()`
- [x] Distinguish "asserted right now" from "this is what tripped it". The
      sources are the ESP's CURRENT state; the trip is a LATCHED past event, so
      a source released after the latch would otherwise misreport the cause.
      Capturing the mask at trip time is part of this — see the landed item
      below ("captured AT TRIP TIME")
- [x] Give every `safety_trip_t` reason (S1–S13) a real cause line **with the
      numbers the firmware has** — the temperature and the ceiling it passed,
      the current and its threshold, the elapsed time and the window — plus a
      real remedy line. Where the firmware cannot currently say what was
      detected, record that rather than filling the slot with a vague sentence.
      See the landed item below ("Cause lines carry the NUMBERS")
- [x] Cover the KilnFW-side faults too. A thermocouple reporting a raw SR
      bitmask is the same defect as a bare S6a. See the landed items below —
      closed further 2026-08-28 by the two `fault sources 0x%02X` refusals
      the original sweep missed (autotune/profile-executor "heat is blocked")
- [x] Say plainly when a fault is NOT operator-clearable. S9 means a possibly
      welded contactor and the required response is "remove power at the
      breaker" — offering a Clear button that will refuse is worse than saying
      so. Verified 2026-08-28: `safety_trip_words.h:244` (S9/case 10) reads
      "NOT clearable from here. Remove power at the breaker and inspect the
      contactor before touching anything else."

**The clearing semantics, recorded here because they were only discoverable by
reading `safety_guards.c`:** an S6a trip LATCHES. It does not clear on its own,
a new firing does not clear it, and an ESP reboot does not. Only an explicit
CLEAR_TRIP does — and that clear is REFUSED while the cause persists, because
`safety_guards_try_clear()` (`safety_guards.c:209`) clears the state and
immediately re-runs the guard, and an unwindowed guard with the line still LOW
re-trips on that same tick. So the operator sequence is: identify the source,
remove it, then clear. None of that is currently told to the operator, and the
owner had to ask.

## M13 landed so far · *2026-08-28*

- [x] S6a decodes its fault source, captured AT TRIP TIME and flagged invalid
      when it cannot be trusted — an ESP reboot with a trip still latched would
      otherwise present this boot's sources as the cause of an older trip. An
      empty mask reads "not captured", never "none": for S6a a zero mask is
      impossible if capture worked
- [x] Cause lines carry the NUMBERS — S1's temperature and the ceiling it
      passed, S3's three channel currents and threshold, S6b's elapsed silence,
      S11's reading and window. **No wire format widened**: the values were
      already arriving on Frame D and were simply never copied
- [x] Where a number does not exist the sentence says so. The negative test
      caught S12 about to print the safety thermocouple where the enclosure
      reading belongs — a different sensor, and entirely plausible-looking
- [x] The thermocouple SR bitmask, the web last-run banner and the per-zone
      diagnostics subrow stopped printing bare codes
- [x] The trip decision moved to its own translation unit so it can be
      host-tested; nothing could link `safety_link.c` off-target
- [x] `profile_executor`'s `fault_guard` swept: every emission is consumed
      only by already-decoded call sites. A repo-wide `reason 0x%02X` sweep
      found the remaining hex-coded surfaces were all `ESP_LOGW`/`ESP_LOGE`
      serial log lines, except two operator-facing ones missed by the first
      pass — `ui_page_temperature.c`'s relay-refusal LCD message and
      `zones_http.c`'s zone-sweep refusal reason — fixed in the same push as
      this checkbox
- [x] A host harness that links `safety_link.c` itself
      (`test_safety_link_compile.c`, 23/23): `SAFETY_FLAG_TEMP_VALID` as sole
      NaN authority and peer-sent LINK_UP/FAULT bits being dropped are both
      pinned against the real file, not a stub
- [x] **Two more operator-facing raw hex values, missed by the earlier
      `reason 0x%02X` sweep because these read `fault sources 0x%02X`, a
      different phrase.** 2026-08-28: `autotune_engine.c`'s and
      `profile_executor.c`'s "heat is blocked" refusals — the message a
      `POST /api/autotune/start` or `POST /api/profile_exec/start` returns
      when the safety link is down — now decode via
      `safety_fault_source_words()`, same first-source-plus-"(+more)"
      shortening `zones_http.c`'s zone-sweep refusal already uses. Verified a
      repo-wide grep for `0x%02X` in `firmware/KilnFW/App/drivers/*.c`:
      everything remaining is an `ESP_LOGW`/`ESP_LOGE`/`ESP_LOGI` line
      (`profile_executor.c`'s stray-relay and config-reload-generation logs,
      `uart_bridge.c`'s IO-refusal logs) or a genuinely unrelated code —
      `ota_http.c`'s bad-image-magic byte, `profile_executor.c`'s
      claimed-relay-mask value — not a fault-source mask. Negative-tested:
      reverted the fix, confirmed the new
      `test_run_decodes_fault_sources_instead_of_hex` host test fails with
      exactly the "must not fall back to a bare hex value" message, then
      restored it

## M14 — Verification you can trust · *opened and largely closed 2026-08-28*

Not a feature milestone. It exists because on 2026-08-28 the sentence "tests
pass, build clean, flashed and verified" could be true and worthless, and
almost every defect found that day was something reporting success it had not
earned.

- [x] **`build_kilnfw` reported OK for builds that failed.** PowerShell does not
      propagate a native command's exit code as its own without an explicit
      `exit $LASTEXITCODE`, so `powershell.exe` returned 0 whatever ninja did.
      Caught live: "OK in 4.8s" printed over a log containing
      `ninja: build stopped: subcommand failed`. **The most expensive instance
      of the structurally-unfailable check in this repo, because it sat above
      all the others** — every guard script, host suite and review funnelled
      through a tool that could not say no. Every sibling tool was checked
      rather than assumed to share the bug; only this one used `-Command`
- [x] **`flash_firmware` would flash a stale binary and report success.** It
      checked only that the `.bin` files existed. Composed with the above into
      something worse than either: edit, build, flash, then verify behaviour
      that had nothing to do with the edit. Now compares the RECORDED build
      commit against HEAD — exact, and right in the case timestamps get wrong
      (a dirty worktree flashing a clean-tree binary). It caught a real
      staleness on its first run, and refused a flash minutes later
- [x] **Two stack overflows, one after it crashed and one before.**
      `safety_poll` died twice with `IllegalInstruction` — a canary trip, not a
      watchdog — holding 1192 B of 4096 because the config refetch put ~1070 B
      of locals on its frame this week and the stack did not move.
      `safety_proto_rx` was then found at 25% by *reading the margins* rather
      than by a second crash, having lost ~500 B to the enlarged RX chunk the
      same day. Both tasks were unregistered with `stack_margin`: the
      measurement existed, the tasks were not in it
- [x] Two new guard scripts (12 → 14): every `src/**.c` in its CMakeLists, and
      no relay write outside `kiln_io_owner`. **Both found real violations on
      their first run**, one of which three rounds of opus review had read past
      because reviews read the diff and it was not in the diff
- [x] A guard for the "setter with no caller" shape of this class —
      `check_unused_setters.ps1` (2026-08-28), which caught its own first
      version blind to multi-line prototypes before it ever ran for real. The
      wider class isn't closed: see M10's heartbeat-contract item for what's
      still uncovered, and `check_guard_input_producers.ps1` still only proves
      a field is assigned, not that the assignment carries a real measurement

## M10 — Instrumentation: make the board tell you when it is wrong

Not a feature milestone. This exists because four separate defects in this
project were invisible for weeks not because they were subtle, but because
nothing on the board was counting the right thing — and in three of the four,
something *was* counting and reported the comfortable answer.

Owned by [`firmware/KilnFW/TODO.md`](firmware/KilnFW/TODO.md) §§12–13.

- [x] **HTTP route table silently overflowed.** `max_uri_handlers` was 84
      against 85 real routes, so `POST /api/safety/commissioning/bench_preset`
      never registered and 404'd with no visible cause. The cap had fallen
      behind four times, each time surfacing as "one page is broken"; a comment
      saying "keep this ahead of the count" had already failed three times.
      Raised to 95 (44 bytes: `esp_http_server` allocates an array of
      *pointers*, `httpd_main.c:430`) and made durable by
      [`tools/check_uri_handler_cap.ps1`](tools/check_uri_handler_cap.ps1),
      which recounts from source and fails if the cap is lower. Verified over
      HTTP: the route now answers 200. 2026-08-24
- [x] **A quarter of every safety poll was logged as a timeout on a healthy
      link.** `timeouts 3616` against `diag applied 3618` / `power applied
      3618`, zero CRC errors, STATUS count equal to the send count.
      `safety_drain_still_waiting()` could speak for CT_CAL, CONFIG_PAGE and
      COMMIT_CONFIG_REJECTED but not for a STATUS, so the poll abandoned its
      remaining budget the moment an unrelated push arrived first — the same
      bug a 2026-08-23 fix had closed for the other three callers, with the
      fourth left out. **No deadline was widened.** Measured after: 310 polls
      carrying 77 DIAG and 77 POWER pushes, zero timeouts. `80f473d`
- [x] **Internal-DRAM low-water alarm**, split into a standing WARN and a
      `DRAM REGRESSION` ERROR, because an alarm that fires on every boot is
      one everybody learns to scroll past. Corrected the trough itself while
      building it: the real minimum is `app_main_done`, ~1 kB below the
      `uart_bridges_1` figure everyone had been quoting, and it sits *below*
      the one documented real failure (free=11903, largest=8704 — `/app.js`
      truncated, pages stuck on "Loading..."). `8d1b015`
- [x] **First real task-stack measurement this project has ever taken**, and
      it found `rules_task` — the rule evaluator that gates heating — at 336
      bytes of 3072, 10.9% headroom, on an idle board. Raised to 4096.
      It was invisible because the instrumentation had inherited vanilla
      FreeRTOS's word units; ESP-IDF returns **bytes** (`task.h:1509`), so a
      stray ×4 reported that task as 45.3% and OK. `d90986c`
- [x] **A safety checklist could come back short and look like a pass.**
      `/api/readiness` appended items with the usual "stop rather than
      corrupt" overflow rule, so a dropped item was simply absent -- and this
      is the one endpoint where absent reads as approval, since the list
      exists to say what is NOT ready before a firing. It could also open the
      array `[,` from a single dropped first item, and skip its own closing
      `]}`, both of which are invalid JSON that the page's fetch throws on,
      leaving an operator on "Loading" with no reason why. Now a reserve is
      held back for the terminator, `first` only clears when an item really
      landed, and any drop is reported AS an item. `GET /api/zones` got the
      matching treatment: a truncated body is now a 500 that names the cause
      instead of a half-document. Negative-tested on the board with the
      buffer cut to 900 bytes -- four items fit and the response carried the
      "Checklist incomplete" entry. 2026-08-26/27, `4e8e1f1`, `88f12e0`
- [x] **Re-read the stack margins after a real firing.** Done 2026-08-27.
      The first run tripped guard 1 at 60 s and never reached a segment
      advance, so a second was built to walk the whole state machine WITHOUT
      heat: a three-segment profile whose targets sit just below ambient, so
      the executor ramps, reaches temperature, dwells, advances segment,
      and completes on a board with the heaters disconnected. Rules were
      configured and driving a relay throughout. Every margin held --
      `rules_task` 1372 B free of 4096 (33.5%), against 1368 idle
- [x] **Audit whether anything `rules_task` calls writes NVS or flash.**
      Done 2026-08-27, transitively over every callee: nothing writes NVS or
      flash. The audit did find one real reach, and not the kind this note
      predicted — not a write but a cache-disabling *read*.
      `dashboard_get_status()`, called every tick by this task, performed
      `esp_flash_get_size()` and `esp_image_get_metadata()` lazily behind a
      first-caller-wins static, so a PSRAM stack was safe only because the
      httpd task happened to get there first: a race, not a guarantee. Those
      reads are now primed at startup on the app_main task, and the stack
      moved to PSRAM. **The standing DRAM regression is gone** — `dram_free`
      at the `uart_bridges_1` trough went 6771 → 10675 and the largest free
      block 4608 → 7680, back to baseline, with no `DRAM REGRESSION` line at
      any stage. `47b004c`
- [x] **Reclaim internal DRAM from the healthy stacks.** Done 2026-08-27
      (`8ad7d5b`) — and the delay was justified by what the coverage work
      turned up. `UART_OWNER_STACK_SIZE` sizes **four** tasks, not two:
      main.c's PC-link `uart_owner` pair was registered for high-water
      reporting, while `safety_link.c`'s pair — the one carrying the telemetry
      that gates all heating — was registered by nobody. Trimming on the two
      visible numbers would have resized two tasks that could not be seen:
      this project's recurring "two instances, one identifier" trap, the same
      shape as the shared UART log tags. Both are now registered
      (`safety_owner_task` / `safety_owner_evt`), the worst of the four had
      used 904 B of 4096, and the shared size dropped to 3072 — ~70% headroom
      on all four and **4 kB of internal DRAM back** (free 22471 → 26895).
      Deliberately not cut closer: a Pico OTA relay transfer streams through
      the safety-link pair and is still unmeasured, so the margin covers a
      path the numbers do not. `rules_task`, `rules_watchdog`, `uart_proto_rx`
      and `system_uart_bridge` are left alone — healthy, and the DRAM they
      would return is no longer needed
- [x] **The cross-language PC-tool heartbeat contract had no guard.** 2026-08-28,
      `tools/check_heartbeat_contract.ps1`. Every existing "consumer with no
      producer" guard (`check_guard_input_producers.ps1`,
      `check_unused_setters.ps1`) is C-only, and this pair's producer
      (`link_hub.py`'s `_heartbeat_loop`) is Python — invisible to both. This
      is the fourth instance of that class this project has shipped
      (`current_sense_set_cal`, `sample_counter_advancing`, `i_normal_a`, and
      the heartbeat itself, `e3e8ec6`); the other three now have guards, this
      is the heartbeat's. Regex-over-source across both files, checking three
      things a future edit could break without either language's compiler
      noticing: the producer call site (`LinkHub.start()` actually starting
      the thread, not commented out), the timing margin (interval + ack
      timeout must stay under `UART_BRIDGE_LINK_TIMEOUT_MS/2`, the bound
      `link_hub.py`'s own comment already promises), and task-id isolation
      (`_HEARTBEAT_TASK_ID` must not collide with a real `UART_TASK_ID_*`).
      Negative-tested by inducing each of the three failures in turn on the
      real files (commented-out start call, interval widened 1.5s→3.0s,
      task id collided with 14/UI_TEST) and reverting — all three caught
- [ ] **HTTP connection resets under concurrency — rate dropped sharply since
      the original characterization; still not root-caused.** 2026-08-28:
      built `GET /api/debug/lwip_stats` (`diagnostics_http.c`,
      `CONFIG_LWIP_STATS=y`) to read lwIP's own TCP-layer counters
      (drop/memerr/err) from a live burst without a JTAG halt that would
      perturb the timing. Two dead ends on the way, kept in the code's own
      comments so the next pass doesn't re-walk them: `stats_display()`'s
      output routes through a bare `printf()` to the console UART unless
      `CONFIG_LWIP_DEBUG_ESP_LOG` is also on, which entirely bypasses
      `uart_log_bridge` (the thing `get_device_log()` reads) — and even with
      that on, the call is hardcoded to `ESP_LOG_DEBUG`, stripped at compile
      time by this project's `CONFIG_LOG_MAXIMUM_LEVEL=3` (INFO). Reading
      `lwip_stats.tcp` fields directly into the JSON response sidesteps both.
      `lwip_stats.mem` does not exist on this port at all — `MEM_STATS` is
      unconditionally 0 whenever `MEM_LIBC_MALLOC == 1`, which ESP-IDF's
      lwipopts.h sets, so there is no separate lwIP heap arena here to have a
      counter for (found by the build refusing to compile it, not assumed).
      With this live: **188 requests, 8/12/20-way parallel, both near-boot
      and steady-state, zero failures**, `tcp.drop`/`memerr`/`err` all held
      at 0 throughout. The one failure seen this session (2/8, the very
      first burst) ran during a window this session's own Pico-recovery
      sequence was generating "PC link lost" churn on the isolated UART — a
      real confound, not present in any of the 180 clean follow-up requests.
      Most likely explanation for the drop: the DRAM-reclaim work already
      landed this session (`c8e10f0`/`8ad7d5b`/`47b004c`) moved the
      fragmentation trough from 8704 → 13824 bytes, clear of `/app.js`'s
      ~9490 B, which is exactly the largest-contiguous-block hypothesis this
      item's previous text flagged as resting on two numbers being close —
      those two numbers no longer are. **Not closing this**: absence of
      failure across 188 requests is evidence the rate dropped, not proof
      the mechanism is gone or was ever confirmed; the original 9/80 rate
      was measured before that DRAM work, and there is no green run of the
      *original* reproducer script to compare against directly. If it
      resurfaces, `/api/debug/lwip_stats` is now in place to catch it live
- [x] **Wire the guard scripts into something that runs them.** Done
      2026-08-27: `tools/run_all_checks.ps1`, plus a `run_repo_checks` tool on
      both MCP servers. Discovery is by glob rather than a list, because a list
      that falls behind is this repository's single most repeated defect; the
      floor below which it refuses to report success exists because the
      opposite trap — a glob matching nothing and reporting green — looks
      exactly like a pass. Its first run found 24 scripts where the repo has
      12, because `.claude/worktrees/` holds abandoned full-tree copies, and
      found `check_link_impl_isolation.ps1` red on twelve false positives plus
      one real hit. Twelve checks, all green
- [x] **`safety_poll` crashed twice with `IllegalInstruction`, self-recovered
      by rebooting.** `c8e10f0`, 2026-08-28. Root cause was a real stack
      overflow, not the timing/blocking shape of the two earlier reverted
      attempts (checked before changing anything, not assumed from the
      symptom): `safety_cfg_store_maybe_refetch()`'s scratch (~680 B) and page
      (~390 B) locals landed on this task's frame the week the refetch became
      real, and the stack was never resized with it — measured at 1192 B free
      of 4096 under ordinary traffic. Raised to 8192, PSRAM-backed, and
      registered with `stack_margin` for the first time (why 29% headroom had
      looked fine for weeks: nobody was reading it). Confirmed live on
      hardware after this session's own commissioning-flow test — which
      exercises exactly the refetch path that crashed it — with no panic:
      4336 B free of 8192 (52.9% headroom) every check added here was made to
fail on purpose before being trusted. That caught two checks that would
otherwise have shipped useless — a slack constant expressed in terms of itself,
and a Python test that could never fail on a C regression — and one that was
actively dangerous: a string-literal fix to `check_isolation.ps1` that blinded
its own `#include` rule while still printing "Isolation check passed". A check
nobody has watched fail is not evidence.

## M15 — Architecture hardening · *opened 2026-09-04*

Findings from a four-agent architecture review, coordinator spot-verified.
Owner: unassigned. Everything below is an open suggestion, nothing is done.

- [x] **`SX1509.h` is public by accident.** `App/drivers/CMakeLists.txt:316`'s
      `INCLUDE_DIRS "."` exposes it to all 8 `uart_bridge*.c` files instead of
      just the owner module. Split the header, move the rest to
      `PRIV_INCLUDE_DIRS`. M — **CLOSED 2026-09-04**: `PRIV_INCLUDE_DIRS`
      can't fence sibling `.c` files in a flat single-component directory
      (quote-`#include` always searches the including file's own directory
      first), so the write/config API moved to `SX1509_internal.h`, gated by
      a `#error` unless the including file `#define`s `SX1509_OWNER_BUILD`
      first — only `kiln_io.c`/`kiln_io_owner.c`/`SX1509.c`/`main.c`'s bring-up
      do. `SX1509.h` now carries only the struct, constants, and read/status
      calls. All 8 `uart_bridge*.c` files turned out to need none of it
      directly (their `#include "SX1509.h"` was dead — verified by grepping
      each for `SX1509_` symbol use) and had it removed; `uart_bridge_io.c`
      keeps `kiln_io.h` for `SX1509_PIN_COUNT`/`SX1509_SENSE_FOR_PIN`.
      Negative-tested: added `#include "SX1509_internal.h"` to
      `uart_bridge_thermo.c` and confirmed `build_kilnfw` fails on the
      `#error`, then reverted. Runtime behavior unchanged — declarations
      moved, nothing rewritten. `build_kilnfw` and all 21/21 host test
      executables pass.
- [x] **`GET /api/status` allocates from internal DRAM.** `dashboard_http.c:582`
      uses plain `malloc` while sibling handlers in the same file use
      `heap_caps_malloc` SPIRAM; same gap in `backup_http.c:178`. This is the
      most-polled handler against the tightest heap. S — **CLOSED 2026-09-04**:
      both converted to `heap_caps_malloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`
      (`dashboard_http.c:582`, `backup_http.c:178` and `:1679`); swept the rest
      of the HTTP handler files and found no other plain `malloc` of a
      response/scratch buffer.
- [ ] **12 files exceed the 1500-line rule.** `autotune_engine.c` (4120),
      `wifi_prov.c` (2820), `dashboard_http.c` (2572), `ota_http.c` (2508),
      `ui_page_home.c` (2279), `panel_spi.c` (2174), `profiles_http.c` (2097),
      `zones_config_json.c` (1867), `main.c` (1820), `backup_http.c` (1756),
      `uart_bridge_ext.c` (1727), `zones_http_handlers.c` (1598). Split
      `wifi_prov.c` and `autotune_engine.c` first — riskiest per
      `ARCHITECTURE.md`, and autotune has four separable concerns, on the
      `profile_executor` 8-file split's precedent. M-L
- [x] **Stand-in stubs sit above the polarity/decode layer.**
      `SaftyFW/src/tasks/discrete_task.c:91-97` documents the shipped E-stop
      polarity bug that 378/378 host checks could not see because
      `virtual_dut` stands in above translation, not below it. Audit the
      remaining stand-ins (current sense, TC SPI) and push stubs below the
      decode layer. S to audit, M per stub — **AUDIT PART CLOSED 2026-09-04**,
      full table in `SaftyFW/docs/GUARD_TEST_MATRIX.md` §10: no `virtual_dut`
      module exists (that name is inherited from the deleted `SimFW`/
      `kilnsim`); the pattern was audited against every `safety_guard_input_t`
      field instead. Current sense (`current_presence_policy.c`,
      `ct_amps_cal.c`) and every already-existing decode/policy layer
      (`discrete_pin_policy.c`, `max31856_*_policy.c`, `snapshots.c`, the
      `kilnlink_*_decode()` codecs) were already relocated below the stub in
      prior sessions and are host-tested directly. **One new relocation this
      pass:** `safety_core_build_input()`'s `.relay_deenergized =
      !relay_owner_is_energized()` line (S9) had a bare, single `!` with zero
      coverage — the same shape as the shipped S7 bug — closed with a new
      source-text-scan test, `test/test_safety_core_polarity_wiring.c`
      (same technique `test_safety_core_s8_wiring.c` uses, since
      `safety_core.c` itself is not host-compilable), which also pins
      `.estop_pressed`/`.main_fault_asserted`'s required non-negation.
      Negative-tested: each of the three lines' negation was flipped one at a
      time and the matching check failed (2067/2068), then restored (2068/2068).
      **Documented, not touched:** the TC SPI raw-decode layer inside
      `max31856.c` — another agent is extracting its fault-pin polarity
      concurrently, so left alone per instruction; the
      `current_sensing_disabled` forcing gap was already recorded honestly in
      GUARD_TEST_MATRIX.md §9 before this pass. `test/test_safety_guards.c`
      itself (the by-design injection seam for the whole
      `safety_guard_input_t` contract) is owned by another concurrent session
      and was not touched.
- [ ] **`safety_link.h` hand-mirrors CommonFW frame constants.** POWER/DIAG/
      UPDATE_STATUS flag blocks (`safety_link.h:243-249,257-280,632-654`, 9
      "mirrored here" comments) duplicate `kilnlink_power.h`/`kilnlink_diag.h`
      by hand because KilnFW cannot `#include` SaftyFW's headers. Either call
      the CommonFW codecs directly or add a CI diff against the source-of-
      truth headers. M
- [x] **No shared bounded-wait/unknown-outcome helper.** The discipline behind
      `safety_link_rollback_boot_id_changed()`'s rollback path (`safety_link.c`
      ~262, "a rollback that fully succeeded into a permanent
      UNKNOWN_TIMEOUT") exists only there. Extract a shared await-reply-or-
      unknown helper for the new-ESP/old-Pico skew case generally. S-M —
      **CLOSED 2026-09-04**: `safety_link_await_or_unknown()` (declared
      `safety_link.h`, cross-TU decl `safety_link_internal.h`, defined
      `safety_link.c`) is a generic "poll at an interval until a callback
      reports ACKED/UNKNOWN or the timeout elapses" helper, documented
      against `LINK_PROTOCOL.md`'s "a timeout must never be misreported as
      success" skew rule, with a doc comment requiring future Pico-bound
      commands to route through it rather than hand-roll a poll loop.
      `safety_link_send_rollback_ex()`'s boot_id-reconnect watch
      (`safety_link_commands.c`) ported onto it: the old inline `for(;;)`
      loop body became `rollback_boot_watch_poll()`, a
      `safety_link_await_poll_fn`, with a `rollback_boot_watch_ctx_t`
      closure carrying what used to be captured locals — same per-iteration
      decisions, same log lines, zero behavior change. `build_kilnfw` and
      21/21 host test executables pass (one pre-existing, unrelated
      `test_safety_link` failure noted below, present before this change and
      untouched by it).
- [x] **`KILNLINK_MIN_COMPATIBLE` is prose-argued per bump.**
      `kilnlink_version.h:20-24` documents this is a human judgement call, not
      a hash or a check. Add a synthetic-old-peer host test that asserts
      dispatch-table coverage per historical version. M — **CLOSED
      2026-09-04**: `test_safety_link_compile.c` gained a table (one row per
      Pico->ESP frame, cited against `kilnlink_version.h`'s own per-bump
      history comments) driving `safety_drain_inbox_ex()`'s real dispatch
      switch (`safety_link_inbox.c`) for every protocol version from
      `KILNLINK_MIN_COMPATIBLE` (7) to `KILNLINK_PROTOCOL_VERSION` (10): a
      frame that falls through to the switch's `default:` now fails the
      build instead of reading as a silently dropped/dead link. Reverse
      direction also asserted (a frame gated above a version, e.g.
      `ROLLBACK_RESULT` at protocol 9, is not expected from an older-but-
      still-compatible peer). KilnFW side only — SaftyFW's own dispatch
      (`link_task.c`) needs real RP2040/pico-sdk headers this tree has no
      off-target harness for; documented as out of reach in the test file's
      own header comment. Negative-tested: commenting out the
      `KILNLINK_ROLLBACK_RESULT_CMD` case failed the new test naming the
      exact frame and both affected protocol versions (9 and 10), then
      passed again once restored. `build_kilnfw` and all 21 host test
      executables green.
- [x] **Duty composition has no single breakdown struct.** Four stages —
      `profile_executor_feedforward.c:291-515`,
      `profile_executor_pid_tick.c` PID clamp then load-cap boost
      (~78-125, ~139-174, boost applied AFTER the clamp so duty can exceed
      1.0 invisibly to `pid_terms_t`, `pid.h:144-149`), and `heater_output.c`
      quantization — with nothing recording the breakdown. Add a
      `zone_duty_breakdown_t` populated through the pipeline and exposed on
      `/api/control`. M — **CLOSED 2026-09-04**: `zone_duty_breakdown_t`
      (`profile_executor.h`) added with one field per real transform
      (ff_hold/ff_climb/coupling_correction, pre/post-taper rate, effective
      kp/ki/kd, post_clamp_total, load_cap_boost, final_commanded — p/i/d/ff
      and pre_clamp_total deliberately not duplicated, already on
      `pid_terms_t`/derivable from it). Populated read-only in
      `profile_executor_feedforward.c` (`zone_feedforward()`) and
      `profile_executor_pid_tick.c` (`pid_family_zone_tick()`) — no control
      math changed. Wired onto `/api/control` in `dashboard_json.c`
      (`append_zone_status_json()`, not `dashboard_http.c` — the actual
      per-zone JSON now lives in the split-out file); `DASHBOARD_JSON_
      CONTROL_BUF_SIZE` raised 448->900/zone with the byte math in
      `dashboard_json.h`. Host tests: `test_dashboard_json.c`'s worst-case
      render extended, plus a new internal-consistency test in
      `test_profile_executor_prestart.c` with a negative-test mutation
      proving the check has teeth. 21/21 host test executables and
      `build_kilnfw` pass.
- [x] **Mode-state sprawl.** >=5 independent enums/booleans describe system
      mode; the dwelling/ramp-lock caveat is re-derived identically at
      `profile_executor_feedforward.c:243-244` and `:566-576`. Document a
      legal-state table or add a runtime assertion — not a forced single enum.
      M — **CLOSED 2026-09-04**: legal/illegal-state table documented as a
      comment block in `profile_executor_internal.h` (6 illegal rules, 3
      called-out legal-but-tricky rows: ramp-lock stall without dwelling,
      PAUSED mid-dwell, autotune SETTLING with no_setpoint). `exec_mode_
      state_check()` (`profile_executor.c`) asserts the illegal rows every
      control tick under the existing lock; host build has no NDEBUG
      convention to plug an on-target assert into (verified by grep — only
      compile-time `_Static_assert` exists elsewhere in `App/drivers`), so
      the call site uses plain `assert()`, live on host, log-only-if-Kconfig-
      disables-it on target — documented as such rather than invented.
      9 new host tests in `test_profile_executor_prestart.c` (5 illegal
      combinations incl. the 3 required, 4 legal-but-tricky including the
      two named in the ROADMAP item). Negative-tested: disabled rule 4's
      check, confirmed 2 of its tests failed, restored — 21/21 host test
      executables and `build_kilnfw` pass.
- [x] **No lint against flash/NVS writes outside the flash worker.** Direct
      writes bypassing `kiln_cfg_store.c`'s worker dispatch (`nvs_set_blob` at
      `kiln_cfg_store.c:356`, `kiln_cfg_store_apply()` at `:681`) have panicked
      hardware 3x and host tests cannot see the hazard (no lock in the stub).
      Add a grep-based CI lint to the host-test script. S — **CLOSED
      2026-09-04**: `App/test/flash_worker_lint.py` greps `drivers/*.c` for
      `nvs_set_*`/`nvs_commit`/`esp_partition_write`/`esp_partition_erase_range`
      outside a 19-file allowlist (each entry justified inline against one of
      three sanctioned patterns: worker dispatch, local
      `caller_stack_is_external()` guard, or init-time-only from `app_main`
      before the scheduler starts). `check_flash_worker_lint.ps1` wires it
      into `tools/run_all_checks.ps1`'s `check_*.ps1` discovery glob.
      Negative-tested: added a bare `nvs_set_u8()` call to `MAX31856.c` (not
      allowlisted), lint failed naming `drivers\MAX31856.c:1374`; reverted,
      confirmed clean again.
- [x] **`zones_http_client.py` hand-types its field table instead of reading
      it live.** `zones_http_client.py:320-350`'s `_TOP_FIELD_FORM_KEY` /
      `_TOP_INT_FIELDS` maps drift from firmware JSON keys by hand;
      `safety_cfg_http_client.py:136-213`'s `params_by_name()` +
      `build_post_body()` already use a live-GET lookup instead. Port zones to
      that pattern, or extend `selfcheck.py` (already parses
      `UART_PROTOCOL_VERSION` from firmware headers, `selfcheck.py:74-89`) to
      diff the dict against `zones_http_handlers.c` literals. S-M — **CLOSED
      2026-09-04**: `selfcheck_zones_fields.py` extracts both `zones_get_handler`'s
      top-level JSON keys (walks the concatenated `APPEND()` string-literal
      template with a brace/bracket-depth tracker, so a `"key":` only counts
      at depth 1 -- nested `safety_wiring`/per-zone/per-profile keys are
      excluded without hand-listing them) and `zones_post_handler`'s literal
      top-level POST field names, and diffs both against
      `_TOP_FIELD_FORM_KEY`/`_TOP_READONLY_OR_STRUCTURAL_KEYS`. Wired into
      `selfcheck.py`'s `main()`. Negative-tested: removed
      `safety_tc_type` from `_TOP_FIELD_FORM_KEY`, both new checks failed
      naming it, reverted, confirmed clean again.
- [x] **No stub-vs-real-IDF signature check.** `App/test/stubs/*.h` can drift
      from the real ESP-IDF headers they stand in for with nothing catching
      it. Add a signature-diff script. M — **CLOSED 2026-09-04**:
      `App/test/stub_signature_drift_check.py` extracts name+arity for every
      stub prototype, matches each stub header to its real counterpart under
      the configured IDF root (`--idf-root`, else `$IDF_PATH`, else
      `build/project_description.json`'s `idf_path`, else common install
      paths — SKIPS gracefully, exit 0, when none resolve), and reports any
      arity mismatch as stub-file:line. 2026-09-04 audit: 26/26 stub headers
      matched, tree clean, so `check_stub_signature_drift.ps1` (wired into
      `tools/run_all_checks.ps1` the same way) passes `--fatal-on-clean`.
      Negative-tested: added a bogus second argument to `stubs/driver/ledc.h`'s
      `ledc_timer_config()`, check failed naming
      `stubs/driver/ledc.h:43`; reverted, confirmed clean again.
- [x] **Campaign runner has no board-config restore on abnormal exit.**
      `run_queue.py` has atomic state and resume, but the `finally` path
      (~1203-1213) only closes the capture file and removes a stray empty
      log — it never re-applies a safe preset via `_apply_preset_http_only`
      (~836). Add a restore-on-exit hook plus a verify-arms-differ preflight
      as built-ins. M — **CLOSED 2026-09-04**: `run_queue()`'s entry loop is
      wrapped in `try/except BaseException` — `_handle_abnormal_exit` skips
      restore if the board was never touched, re-applies the campaign's
      baseline preset (last entry's, or `--baseline-preset`) once
      `_board_is_idle()` confirms `profile_exec` is not running/paused
      (never races an active firing), and records
      attempted/succeeded/error into the state file either way; a failed
      restore logs an unmissable banner naming the preset and fields to
      check by hand. `_check_arms_differ`, wired into `_preflight_campaign`
      before any HTTP probe, compares every pair of distinct local preset
      payloads (excluding `name`) and refuses naming the identical pair.
      `tools/PcTools/tests/test_run_queue_restore_and_arms.py` (10 tests).
      Negative-tested both: disabling `_check_arms_differ`'s call site
      failed the identical-arms test; disabling the `except` block's
      restore call failed 4/6 restore tests; both reverted, full
      `test_run_queue*.py` suite (119 passed, 2 skipped) confirmed clean
      again.
- [x] **Four hand-rolled JSONL parse loops disagree on malformed-line
      handling.** `coupling_pair_log.py:187-192,239-245`,
      `http_capture_log.py:67-72`, `link_hub.py:197-202,588-595`,
      `relay_ku_tu_check.py:97-101`. Factor a shared `iter_jsonl` helper. S —
      **CLOSED 2026-09-04**: `kilnctrl/jsonl_util.py`'s `iter_jsonl(source,
      on_error=..., with_line=...)` accepts either a path or an already-open
      line iterable (e.g. `link_hub.py`'s socket `makefile()`), and preserves
      each caller's prior malformed-line behaviour explicitly per call site:
      `on_error="skip"` (coupling_pair_log/http_capture_log/link_hub's second
      loop), `on_error="raise"` (relay_ku_tu_check, which never caught
      `json.loads()` before), `on_error=<callable>` (link_hub's first loop,
      which logs before skipping). `with_line=True` covers
      `load_thermo_samples_any_format`'s two-format fallback, which needs the
      raw line when the primary `{"t","s"}` parse fails. All five call sites
      ported, no behaviour change. Negative-tested: made the "raise" path a
      no-op, `test_jsonl_util.py`'s raise test failed as expected; reverted.
- [x] **`tuning_campaign.py`'s `make_plant` generates continuous floats.**
      (~126-268) No MAX31856 0.0078125 C quantization, unlike the real
      sensor path. Add a quantize pass, or document explicitly why continuous
      is intentional. S — **CLOSED 2026-09-04**: `run_step_test`/
      `run_relay_test` gained a `_quantize()` helper (rounds to
      `plant_sim.MAX31856_QUANTUM_C`, 0.0078125 C = 1/128 C) applied to every
      measured-temperature output, on by default via a new `quantize=True`
      parameter (`quantize=False` restores the old continuous behaviour).
      Negative-tested: made `_quantize()` a no-op, `test_tuning_campaign_
      quantize.py` failed (3 of 6 tests), reverted, confirmed clean again.
- [x] **Vendored `mcpkit_registry.py` has no drift guard.** Currently
      byte-identical to `tools/PcTools/src/mcpkit/registry.py`, the source of
      truth it's vendored from — add a one-line diff check to `selfcheck.py`
      so it stays that way. S — **CLOSED 2026-09-04**: `pytest`-side coverage
      already existed (`tests/test_mcpkit_vendored_copy.py`), but nothing
      covered it in `selfcheck.py`, which runs in contexts pytest doesn't
      (per this item's own request). Added `_mcpkit_vendored_copy_check()` --
      same byte-identical (line-ending normalized) comparison, skips cleanly
      when the `mykicadMcp` submodule isn't checked out.
- [ ] **Frame A's field layout is hand-duplicated across firmwares.**
      `SaftyFW/src/tasks/link_frame.h:1-16` (pack side) says it is
      "byte-for-byte the layout `safety_link.h` already parses" against
      `KilnFW/App/drivers/safety_link_frames.c:600-641`'s independent
      hand-written offset table — an offset mismatch passes CRC and silently
      misdecodes temperatures. Lift the offsets into a shared CommonFW header,
      the way `kilnlink_rollback_result.h` already does for that result type.
      M
- [x] **The drift test for the item above is itself a third hand-copy.**
      `SaftyFW/test/test_link_frame_wire.c:93` `mirror_apply_status()` is a
      transcription of `safety_apply_status()`/`safety_parse_fw_version()`
      (KilnFW `safety_link.c`), not a link to them — it can drift green
      exactly like the two functions it's meant to catch drifting from each
      other. Add a CI check that diffs the `p[N]` offset lists between the
      mirror and the real function. M — **CLOSED 2026-09-04**:
      `App/test/frame_a_offset_drift_check.py` extracts the ordered
      `p[N]`/`out[N]`/`&x[N]` offset table for Frame A's six numeric fields
      (`tc_temp_c`/`cj_temp_c`/`tc_fault`/`amps1-3`) from all three copies —
      `SaftyFW/src/tasks/link_frame.c`'s `link_frame_pack_status()` (pack),
      `KilnFW/App/drivers/safety_link_frames.c`'s `safety_apply_status()`
      (parse), and `SaftyFW/test/test_link_frame_wire.c`'s
      `mirror_apply_status()` (the drift test itself) — and fails naming the
      exact file/field/offset on any disagreement.
      `check_frame_a_offset_drift.ps1` wires it into
      `tools/run_all_checks.ps1`'s `check_*.ps1` discovery glob, same
      pattern as `check_flash_worker_lint.ps1`. Negative-tested: changed
      `safety_link_frames.c`'s `tc_fault` read from `p[10]` to `p[9]`, check
      failed naming `safety_link_frames.c: 'tc_fault' read from offset 9,
      expected 10`; reverted, confirmed clean again.
- [x] **MAX31856 fault-pin polarity is inline and host-untested.**
      `SaftyFW/src/max31856.c:231`,
      `out->fault_pin_asserted = (s_fault_gpio >= 0) && (gpio_get(s_fault_gpio) == 0)`
      — same class as the shipped S7 e-stop polarity bug. `discrete_task.c`
      already shows the fixed pattern: pure, host-tested
      `discrete_pin_policy_*_asserted()` helpers
      (`discrete_task.c:98-100`). Extract the same pattern for the MAX31856
      fault pin and host-test it; feeds S5. M — **CLOSED 2026-09-04**: split
      into `max31856_fault_pin_policy.h/.c`
      (`max31856_fault_pin_asserted(fault_gpio_high)`), same convention as
      `max31856_tc_type_policy.h`/`max31856_tc_range_policy.h`; `max31856.c:231`
      now calls it instead of the inline `== 0` check. Confirmed asserted-low
      against KilnFW's `MAX31856.c:531`/`MAX31856.h:482` ~FAULT comments
      (open-drain, active-low). Added `test_max31856_fault_pin_policy.c`
      pinning both directions, feeding S5. Negative-tested: inverted the
      predicate, 2/2064 host checks failed (exactly the two new ones), then
      reverted. Zero behaviour change; full SaftyFW host suite 2064/2064
      green.
- [x] **Dead blocking fixed-length `uart_read_bytes` branch stays loaded.**
      `KilnFW/App/drivers/espInterfaces/uart_owner.c:139`'s `rx_buffer`/
      `rx_length` branch is the exact pattern behind the 100%-timeout
      incident. No current caller passes `rx_buffer` (grep across
      `App/drivers/*.c` turns up nothing), so it's dead today, but nothing
      stops a future caller reintroducing the hazard. Delete the branch, or
      assert it unreachable once a `uart_protocol_t` is attached. S —
      **CLOSED 2026-09-04**: the struct's `rx_buffer`/`rx_length`/
      `rx_length_out` fields and public `uart_owner_transfer()` signature are
      used by test stubs, so full deletion was awkward; the branch now fails
      loudly (`ESP_LOGE` + `ESP_ERR_NOT_SUPPORTED`) instead of blocking, per
      `LINK_PROTOCOL.md` §3.
- [x] **`LINK_PROTOCOL.md` section 10's completion checklist is stale.**
      TRIP_EVENT dedup, the POWER/DIAG frames, and the 30 s firing-abort are
      all listed unchecked (`CommonFW/docs/LINK_PROTOCOL.md` sec 10) though
      implemented — `safety_link_frames.c:280-295` (TRIP_EVENT dedup),
      `safety_apply_power()`/`safety_apply_diag()` (same file, POWER/DIAG),
      and `profile_executor.c:1112-1147` plus
      `test_safety_link.c:77-92`/`:86-92` (30 s abort, tested and pinned).
      `CommonFW/docs` is owned by CommonFW, not KilnFW — someone with edit
      access there needs to tick these. S (doc-only) — **CLOSED 2026-09-04**:
      each verified directly in code before ticking —
      `safety_apply_trip_event()` (`safety_link_frames.c:883-928`) dedups on
      `trip_seq` via `safety_trip_decision.c`, with the boot-reboot dedup
      reset at `:280-295`; `safety_apply_power()` (`:685`) and
      `safety_apply_diag()` (`:793`) both exist and apply their frames;
      `profile_executor.c:1201-1236`'s `safety_link_silent_30s` /
      `SAFETY_LINK_FIRING_ABORT_SILENCE_MS` implements the 30 s abort,
      pinned by `test_safety_link.c:77-92`
      (`test_firing_abort_ms_constant_is_30000`). `LINK_PROTOCOL.md` sec 10
      updated: TRIP_EVENT, DIAG, POWER, and the 30 s firing-abort line items
      ticked with file:line citations; the two still-genuinely-open liveness
      items (pre-first-frame-down, bench-escape doc) left unchecked.

Informational: the Pico is still on protocol v8, which makes S13's
BORROWED-zone indicator unreachable in practice today — it fails closed and
visibly, so not a defect, but a concrete reason to prioritize bringing the
Pico build current.

**Patterns worth copying, not just avoiding:** `thermal_guard_tick`'s explicit
input-struct interface; `kiln_cfg_store`'s interlock kept inside the module
that owns the write path; `safety_cfg_http_client`'s live-lookup field table
(the fix for the zones item above); and `kilnlink_version.h`'s deliberate
independence of the two version constants — documented reasoning, do **not**
propose re-tying them.

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
| **The two links version independently.** `UART_PROTOCOL_VERSION` (PC↔ESP) was an alias of `KILNLINK_PROTOCOL_VERSION` (ESP↔Pico) behind a hard-equality gate, so an isolated-link bump refused every PC command until pc_tools moved. Bitten three times before being split. | 2026-08-24 | `firmware/KilnFW/App/drivers/uart_task_ids.h` |
| K4 → line-contactor interlock: J10 pin 1 = NO, pin 2 = COM, pin 3 = NC (read from the K4 symbol's rest position, not silkscreen) — still wants a continuity check against the physical part | 2026-08-16 | `firmware/SaftyFW/docs/HARDWARE.md` §3 |
| E-stop circuit is normally-closed by design. ~~No jumper fitted, so an as-built board reads permanent STOP~~ — **corrected 2026-08-24 by measurement**: `pico_gpio_read(9)` reads LOW on this bench, i.e. a contact IS fitted and S7 correctly stays quiet. Do not plan around needing to fit one; measure instead. The stale note also masked a real inversion — `discrete_task.c` had `!gpio_get()` on an active-HIGH pin, so this healthy reading decoded as *pressed*, hidden because S5 latched first and `safety_guards_tick()` early-returns while any trip is latched | 2026-08-16, corrected 2026-08-24 | `firmware/SaftyFW/docs/HARDWARE.md` §5, commit `642dd54` |
| ESP32-S3 boot-loop (repeating stack overflow in the main task, right after LVGL's boot banner) fixed by raising `CONFIG_ESP_MAIN_TASK_STACK_SIZE` 3584→8192 | 2026-08-19 | `firmware/KilnFW/App/main.c`, `sdkconfig.defaults` |
| Internal SRAM exhaustion: `xTaskCreatePinnedToCore()` always takes TCB+stack from internal SRAM, and Wi-Fi/lwIP + LVGL had claimed nearly all of it by the time later tasks tried to start (caused the AUTOTUNE/WIFI UART-task registration failures). Fixed at the source — moved LVGL's allocator and the Wi-Fi/lwIP pools to PSRAM — not by shrinking the tasks that were failing | 2026-08-20 | `firmware/KilnFW/TODO.md` §1 |
| `uart_owner_transfer()` called `xSemaphoreCreateBinary()` (a heap alloc) on every single UART transfer; under real interactive load this exhausted internal SRAM (`ESP_ERR_NO_MEM` bursts every ~40s). Fixed with a static, stack-resident semaphore | 2026-08-18 | `firmware/KilnFW/App/drivers/espInterfaces/uart_owner.c` |
| LVGL hit-testing cannot escape a parent that doesn't contain the touch point, and a non-`LV_OBJ_FLAG_FLOATING` child of a flex column silently joins the flow and eats the page's content budget | 2026-08-20 | `firmware/KilnFW/App/drivers/ui_topbar.h` |
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
  `firmware/KilnFW/App/drivers/ui_topbar.h` is the worked example: it exists
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
