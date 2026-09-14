# Implementation: live per-task stack high-water reporting, SaftyFW -> ESP

Implements the design in `docs/audits/saftyfw_live_stack_reporting_design_2026-09-11.md`
(commit `5073eab9`), informed by `docs/audits/saftyfw_bare_minimum_stack_measurement_2026-09-11.md`
(commit `5b026798`). Both are commits, confirmed with `git cat-file -t`.

## Decision taken (design left two open)

- **New frame command pair**, not DIAG growth: `SAFETY_CMD_GET_STACK_MARGIN`
  (`0x2B`, ESP->Pico request) / `SAFETY_CMD_STACK_MARGIN` (`0x2C`, Pico->ESP
  reply). `firmware/CommonFW/include/kilnlink/kilnlink_get_stack_margin.h`,
  `firmware/CommonFW/include/kilnlink/kilnlink_stack_margin.h`.
- **`KILNLINK_PROTOCOL_VERSION` bumped 12 -> 13**
  (`firmware/CommonFW/include/kilnlink/kilnlink_version.h`). This pair is
  request-triggered in both directions (same shape as the unbumped
  REBOOT/REBOOT_RESULT precedent at protocol 12), so the letter of this
  project's own "does an old peer receive a byte it doesn't know" test does
  not strictly require a bump here either. It is bumped anyway, deliberately
  more conservative than that precedent, because a mismatched-pair skew is
  already this codebase's most common real hazard around a flash event, and
  a version-visible bump gives `flash_firmware()`'s verification step and
  `/api/status` readers a cheap, explicit "these two were flashed together"
  signal rather than relying on "the Pico silently never answers 0x2B." Full
  argument in `kilnlink_version.h`'s own "12 -> 13" entry. The separate UART
  link version (11) is unchanged -- only an application-level command pair
  was added, transport framing untouched.

## Wire format and units

`kilnlink_stack_margin_t` (47 bytes: `0x2C` + `rounds_completed` u8 + 9 x
(`task_id` u8, `high_water_words` u16 LE, `stack_total_words` u16 LE)).

**Units: WORDS, not bytes**, on both fields -- this port's
`uxTaskGetStackHighWaterMark()` and `xTaskCreate()` both operate in words
(vanilla FreeRTOS on RP2040/Cortex-M), the OPPOSITE convention from
ESP-IDF's own `get_stack_margin()`, whose FreeRTOS fork reports bytes. The
two processors' numbers are NOT directly comparable without converting one
of them; this is called out explicitly in `kilnlink_stack_margin.h`'s
"UNITS" section, in `stack_margin_poller.h`, and in
`safety_link_get_stack_margin()`'s own doc comment (`safety_link.h`) so the
mistake cannot be made silently at any of the three places a future reader
might look.

`KILNLINK_STACK_MARGIN_UNMEASURED` (`0xFFFF`) marks an entry the poller
has not sampled yet; `rounds_completed == 0` means at least one entry is
still that sentinel. Every surfaced value is a FLOOR, not a worst case (see
`kilnlink_stack_margin.h`'s own section on this, restated from the design
doc) -- it is the tightest margin observed since the Pico's last boot on
whatever code paths actually ran, never a ceiling.

## Timing-hazard design (the single most important acceptance criterion)

`stack_margin_poller.c` is a round-robin sampler: `stack_margin_poller_tick()`
measures exactly ONE of the nine tasks' `uxTaskGetStackHighWaterMark()` per
call, advances an index, and is invoked from `log_task_fn()`'s own loop
(`firmware/SaftyFW/src/tasks/log_task.c`), once per that task's existing
~500ms iteration -- never from `watchdog_task_checkin()`, never from every
task's own check-in. This is the shape the design doc recommends
specifically to avoid repeating the 2026-08-23 regression (a per-check-in
call from every task dropped watchdog cadence from ~9s to ~1.1s). A full
round-robin cycle over 9 tasks takes ~4.5s.

`link_task_handle_get_stack_margin()` never measures synchronously -- it
only reads `stack_margin_poller_snapshot()`'s cache and replies. The ESP
side (`safety_link_get_stack_margin()`) is meant to be polled slowly (this
data changes on the order of minutes/boots), not in the ~1Hz DIAG loop.

**Known limitation, stated rather than hidden:** `stack_margin_poller.c`
keeps its own hand-maintained table of each task's configured
`*_STACK_WORDS` (for `stack_total_words`), duplicated from each task's own
`#define` (relay_owner.c, safety_core.c, discrete_task.c, thermo_task.c,
current_task.c, link_task.c, log_task.c, update_task.c, watchdog_task.c).
This is the same "two sides of a fact, no single owning function" hazard
class CLAUDE.md's own "reset one side of a pair" writeup describes -- if a
task's stack size changes, this table does not change with it automatically.
No task-header currently exports its stack-depth constant (several are
isolation-restricted), so a mechanical single-source-of-truth fix was out of
scope for this pass; `check_saftyfw_task_stack_budgets.py`'s own static walk
remains the authoritative source and this table should be cross-checked
against it whenever either changes.

## Tests

Host-only, both directions, run via MSVC (`vcvars64.bat` + `cmake`/`ninja`,
`firmware/CommonFW/build`):

- `firmware/CommonFW/test/test_get_stack_margin.c` -- request codec: round
  trip, NULL-msg encode, buffer-too-small, length-mismatch decode, wrong-cmd
  decode.
- `firmware/CommonFW/test/test_stack_margin.c` -- reply codec: round trip
  with all 9 entries fully measured, round trip with the UNMEASURED sentinel
  and `rounds_completed == 0`, a hand-computed little-endian wire vector
  (byte-for-byte, printed and diffed), length-mismatch decode (too long, too
  short, empty), wrong-cmd decode, buffer-too-small encode, NULL-msg encode.
- Both registered in `firmware/CommonFW/test/test_fuzz_payloads.c`'s
  `k_cases[]` (`build_host_tests.ps1` enforces every kilnlink decoder is
  fuzzed -- this caught the omission on the first run and named both
  functions explicitly); fuzzed cleanly (17216 calls, 34 decoders, no crash/
  hang/canary corruption).
- `firmware/KilnFW/App/test/test_safety_link_compile.c`'s synthetic-old-peer
  compatibility table got a new row for `KILNLINK_STACK_MARGIN_CMD`
  (min_version 13), proving `safety_drain_inbox_ex()`'s dispatch has an
  explicit case for it.
- `firmware/KilnFW/App/test/wire_protocol_fingerprints.json` regenerated via
  `wire_protocol_fingerprint_check.py --update` (the manifest's own
  deliberate-refresh gate; it failed loud on the stale manifest before this,
  exactly as designed).

**Full suite result:** all 40 `firmware/CommonFW/build` ctest cases pass;
`build_saftyfw_host_tests()` reports 2497/2497 + 56/56 + 220/220, all
passed; `build_saftyfw()` (RP2040 target) and `build_kilnfw()` (ESP32-S3
target) both report OK.

### Negative test (production code broken by hand, then restored)

Changed `kilnlink_stack_margin_decode()`'s `high_water_words` field offset
from `base + 1u` to `base + 2u` (an off-by-one into the wrong byte lane) in
`firmware/CommonFW/src/kilnlink_stack_margin.c`. Rebuilt
(`test_stack_margin` target only, then confirmed): **11 checks failed**,
including every per-entry `high_water_words round-trips` assertion and both
UNMEASURED-sentinel assertions, exit code 1 -- confirming the test suite
actually detects this class of bug rather than passing vacuously. Restored
the original line by hand, deleted `firmware/CommonFW/build` entirely, and
rebuilt clean from scratch (`cmake -S . -B build -G Ninja` +
`cmake --build build`): all 40 tests pass again, including
`test_stack_margin`.

## Hardware verification

Board state before touching hardware: `get_board_state()`/`get_heap_status()`
confirmed healthy -- `reset_reason='other watchdog'` from a prior, unrelated
uptime (31625s), relays at 0, `profiles_exec_status.state == 0` (idle, no
firing), no unacknowledged-crash banner, safety link up
(`crc_errors=17`/`timeouts=112` out of 94645 deframed frames -- pre-existing
background rate, not from this change). ESP-side kilnlink was still
protocol 12 pre-flash (Pico's own `safety_fw_version.protocol_version: 12`).

[FLASH AND ON-HARDWARE VERIFICATION NOT YET PERFORMED IN THIS PASS -- see
"Status" below.]

## Status (be honest about what is and is not done)

Done and verified: wire codecs (host-tested both directions + fuzzed +
negative-tested), SaftyFW-side poller and link_task dispatch (compiles
clean, `build_saftyfw()` OK), ESP-side consumer
(`safety_link_get_stack_margin()`, compiles clean, `build_kilnfw()` OK),
protocol version bump with full history-comment justification, compat-table
test row, wire-protocol-fingerprint manifest refreshed, full existing test
suites (CommonFW ctest, SaftyFW host tests via both the repo script and the
MCP tool) all still green.

**Not yet done as of this writing:** flashing both processors together and
the live hardware verification this task's acceptance criteria center on --
confirming the link comes up at version 13, reading real per-task marks for
all nine SaftyFW tasks, and (the single most important acceptance criterion)
measuring watchdog reset cadence before/after on real hardware to confirm
the round-robin poller design does not repeat the 2026-08-23 regression. No
GET /api/... HTTP route or kilnctrl MCP tool has been added yet to surface
`safety_link_get_stack_margin()` to a human/tool caller (the ESP-side
driver function exists and compiles; nothing calls it yet). This section
will be wrong the moment that work lands -- treat any claim elsewhere in
this document about hardware results as unperformed until this paragraph is
removed or replaced with real numbers.
