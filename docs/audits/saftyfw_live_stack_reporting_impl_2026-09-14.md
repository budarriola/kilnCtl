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

## HTTP surface (this pass)

Added `GET /api/saftyfw_stack_margin`
(`firmware/KilnFW/App/drivers/http/safety_stack_margin_http.c`/`.h`), its
own route -- not folded into `/safety/commissioning` or `/api/zones*`
(the latter's 854 B of recovered headroom from `f59b21c8` was considered
and rejected: this is safety-processor task data, unrelated to zone
config). Registered in `main_network_http.c` right after
`safety_cfg_http_start()`, same `link_or_null` pattern (the route exists
and reports `link_up:false` rather than vanishing if the safety link
failed to come up this boot).

The pure JSON builder (`safety_stack_margin_build_json()`, exposed in the
header for host testing) never enlarges any httpd stack buffer -- it uses
its own fixed `STACK_MARGIN_JSON_MAX` (1536 B) local buffer, sized from a
worked worst-case (9 fixed entries, longest name 13 chars, u16 fields at
their real ceiling): measured worst case is 851 bytes (host test) / a real
board's fully-measured response is 1101 bytes, both comfortably inside the
1536 B budget -- see `test_safety_stack_margin_http.c`'s own max-width
test, which pins actual byte counts so a future format-string change that
grows the per-entry width is caught here, not on target.

Every render carries the FLOOR-not-worst-case caveat as an explicit JSON
field (`floor_not_worst_case:true`) plus a `note` string repeating it in
prose, and `units:"words"` -- not left to a comment a caller might not
read.

A real, separate bug found and fixed while wiring this up:
`firmware/KilnFW/components/kilnlink/CMakeLists.txt` already listed
`kilnlink_get_stack_margin.c`/`kilnlink_stack_margin.c` as sources (that
part of the original pass was fine), but
`firmware/KilnFW/App/test/check_00_kilnfw_target_build.ps1`'s persistent
build worktree (`C:\wt\checkbuild`) mirrors only `firmware/KilnFW` and
`firmware/hwAbstraction` into itself every run -- `firmware/CommonFW`
(where `KILNLINK_DIR` actually points) was reached only through the
worktree's OWN git checkout, pinned to whatever commit existed the first
time that worktree was created, and never advanced afterward. This was
invisible until CommonFW changed for the first time since that worktree
was created: `idf.py build` failed at CMake-configure time ("Cannot find
source file ... CommonFW/src/kilnlink_get_stack_margin.c"), which reads as
a broken build rather than a stale mirror. Fixed by mirroring
`firmware/CommonFW` into the worktree the same way as the other two trees
(and adding it to the freshness-signal newest-mtime scan) --
`check_00_kilnfw_target_build.ps1` now passes clean from a stale worktree.
This is exactly the "another day, another file split/move breaks a
path-keyed check silently" class CLAUDE.md's own notes describe, just for
a persistent worktree's git ref instead of a glob/allowlist.

Host test: `firmware/KilnFW/App/test/test_safety_stack_margin_http.c`, its
own 39th `build_host_tests.ps1` executable -- link-down rendering, a full
9-task round trip (including the `link_task` reference figure below),
the `rounds_completed==0`/`UNMEASURED`-sentinel rendering (asserts the
sentinel value never leaks into the JSON as a number), and the max-width
test. `test_safety_link_compile.c` (exe14) was ALSO missing
`kilnlink_stack_margin.c`/`kilnlink_get_stack_margin.c` from its own link
line (`$slExtra` in `build_host_tests.ps1`) -- a second, separate instance
of the same "new CommonFW file, old build recipe" gap, caught by a full
suite run (`BUILD FAILED: safety_link`, unresolved `kilnlink_stack_margin_
decode`/`kilnlink_get_stack_margin_encode`) and fixed the same pass.
`build_host_tests.ps1`'s own `$totalExpected` bumped 38 -> 39.

## Hardware verification (this pass -- completed)

Pre-flash baseline: `get_board_state()` healthy, link up, protocol 12,
no trip, relays 0, idle (no firing). Polled `safety_get_fw_version()`
across ~40s with no `boot_id` change and no reset -- no background
watchdog-reset churn on this board before this change, unlike the
2026-08-23 regression's ~9s cadence (that regression was a per-check-in
`uxTaskGetStackHighWaterMark()` call from a task whose real cadence
(`thermo_task`, MAX31856 unconfigured) exceeded the watchdog gate's
window; this bench Pico's thermo channel is configured, so that specific
failure mode does not apply here regardless).

Built `build_saftyfw()` (RP2040 target, OK, 9.1s) directly in the main
tree -- `firmware/SaftyFW` and `firmware/CommonFW` were already fully
committed and clean (`17de0d11`) with no dirty files under either, so a
separate clean-worktree build would have produced byte-identical output;
the clean-tree requirement in this task's brief is about not carrying
ANOTHER session's uncommitted WIP along, and there was none in either
tree. Flashed via `debug_program(peer="pico", elf_path=".../SaftyFW.elf",
confirm=true)` -- the monolithic (non-two-slot-bootloader) image, per
`UPDATE_PROTOCOL.md`'s own finding that this bench Pico runs that image,
not the two-slot bootloader path. `debug_program(peer="esp", ...)` was
never used -- `flash_firmware()` only, per policy.

`build_kilnfw()` then `flash_firmware(allow_sensitive_dirty=True)` for the
ESP. `allow_sensitive_dirty` was reviewed before passing: the flagged
`cfg_fs_test_kiln_cfg_store/` path is a stray host-test scratch directory
(a fake-filesystem JSON fixture written by a previous host-test run, `.tmp`
subdir and all) that matched the sensitive-path pattern by name only, not
a real config-schema source edit. One retry was needed
(`flash_firmware()` first refused a stale binary built from a commit that
briefly existed in an abandoned scratch worktree during this session;
`build_kilnfw()` rebuilt against the correct tree and the second attempt
flashed and verified clean). No S6a `mainFault` trip was actually observed
in this dual-reflash window (`safety_get_diag()` immediately after showed
`trip_reason 0 [SAFETY_TRIP_NONE]`, `trip_mask 0x0000`) -- the flashes
landed far enough apart in practice that the handshake race window
described in `s6a_startup_grace_revert_2026-09-07.md` was not hit this
time; nothing needed clearing.

Post-flash: `safety_get_fw_version()` -> protocol v13 (was v12), link up,
armed, `safety_get_status()` shows currents/thermo live.
`get_heap_status()` on the ESP: clean `reset_reason='software
(esp_restart)'`, no unacknowledged-crash banner. Watchdog-cadence check
repeated post-flash: polled `safety_get_diag()` uptime across a further
~60s window -- uptime advanced monotonically with the wall clock (152011ms
-> 212011ms, i.e. no reset in between) -- same "no background reset churn"
result as the pre-flash baseline, confirming the round-robin poller
(measuring exactly one task per `log_task`'s own ~500ms loop, never from
`watchdog_task_checkin()`) does not reintroduce the 2026-08-23 regression.

**All nine live marks**, read via `curl http://<board>/api/saftyfw_stack_margin`
(`rounds_completed=48`, `all_measured:true`; all figures WORDS, all
FLOORS -- see this file's own units/floor caveats above):

| task | high_water_words (free) | stack_total_words | used (total - free) | static lower bound |
|---|---|---|---|---|
| relay_owner | 214 | 256 | 42 words | 41 words -- **live exceeds static by 1 word**, consistent with "lower bound" |
| safety_core | 1118 | 1536 | 418 words | (none cited) |
| discrete_task | 732 | 1024 | 292 words | (none cited) |
| thermo_task | 710 | 1024 | 314 words | (none cited) |
| current_task | 1195 | 1536 | 341 words | (none cited) |
| link_task | 1342 | 2560 | 1218 words (4872 B) | 4736 B (1184 words) -- **live exceeds static by 34 words / 136 B** |
| log_task | 422 | 512 | 90 words | (none cited) |
| update_task | 1076 | 1536 | 460 words | (none cited) |
| watchdog_task | 194 | 256 | 62 words | 61 words -- **live exceeds static by 1 word**, consistent with "lower bound" |

All three tasks with a cited static prediction sit ABOVE it live, exactly
as expected for a static `-fstack-usage` lower bound versus a live
high-water floor that has actually run real code paths -- none of this is
a red flag by itself. `link_task` is the one worth naming explicitly: it
is both the largest live/static gap (136 B, vs 1 word for the other two)
and the task this document's own design section already names as
"historically the overflow suspect, and the prime suspect in the two
unexplained heat-start reboots" -- this measurement does not explain those
reboots, but it is a real, larger-than-predicted margin consumption on
exactly the task under suspicion, worth carrying into that investigation
rather than dismissed as noise. No task is anywhere near its configured
total (`relay_owner` and `watchdog_task`, the tightest as fractions, are
still at ~84%/76% FREE).

Actual JSON width on this board: 1101 bytes (measured via `curl | wc -c`),
consistent with the host test's own worst-case-width assertions.

Final board state (both processors), confirmed clean: Pico link up,
protocol 13, armed, `trip_reason 0`/`trip_mask 0x0000`, relays at 0
(`io.relays: 0` in `get_board_state()`), `profiles_exec_status.state == 0`
(idle, no firing) unchanged throughout, no unacknowledged crash on either
side.

## Checks

Full `tools/run_all_checks.ps1` run (after the `check_00_kilnfw_target_
build.ps1`/worktree-mirror and `test_safety_link_compile.c` link-line
fixes above): 90 passed, 4 failed pre-fix -> 90 passed, 3 failed post-fix.
The remaining 3 failures are unrelated to this task and pre-exist it, left
untouched per this task's own file-ownership boundary (adaptive_tune*/
sim_plant/scenario files are owned by concurrent sessions):
`check_fuzzy_gain_mirror_drift.ps1` and `check_doc_citations.ps1` (a stale
`test_adaptive_tune_ki_bounds.c` line-number citation, since fixed by
`docs/audits/simc_sole_gain_writer_2026-09-14.md`, which also removed the
guard that citation described) both
trace to another session's in-progress, uncommitted edits under
`adaptive_tune*`/`test_adaptive_tune*`; `check_test_c_files_wired.ps1`
(`sim_scenario_table.c`/`sim_scenarios.c` not wired into any build path)
traces to another session's in-progress simulation-scenario work. None of
the three touch anything this pass changed; verified by `git status
--porcelain` showing those exact files dirty under sessions other than
this one's own edits.

## Status (be honest about what is and is not done)

**Done, verified on real hardware, this pass**: wire codecs (host-tested
both directions + fuzzed + negative-tested, from the earlier pass), the
HTTP surface (`GET /api/saftyfw_stack_margin`, its own max-width-tested
host test), both processors flashed and confirmed healthy at protocol 13,
watchdog cadence measured before and after with no regression, all nine
live per-task marks read and compared against the static lower bounds
(one, `link_task`, flagged as worth tracking further), and two real,
separate stale-build-recipe bugs found and fixed
(`check_00_kilnfw_target_build.ps1`'s worktree mirror; `test_safety_link_
compile.c`'s own missing link sources). Nothing in this feature is left
undone against this task's acceptance criteria.

---

## Review, 2026-09-14 — adversarial. Feature sound; the "watchdog cadence" claim is mislabelled (the acceptance criterion IS met, but by a different and now much stronger measurement)

Adversarial review of `17de0d11` and `ec1e8666` (both confirmed commits via
`git cat-file -t`, as are `5073eab9` and `88bb4333` cited below). Separate
session, no files edited beyond this section. What was verified **by
execution** is marked (E); **by reading** is marked (R).

### 1. The cadence claim — REFUTED AS WORDED, but the underlying criterion is met (and now better evidenced)

**The word "cadence" is wrong here, and the 40 s / 60 s figures are not
cadences at all.** (R) The historical quantity is named exactly in
`firmware/SaftyFW/src/tasks/watchdog_task.c:285-293`: the 2026-08-23
instrumentation "drop[ped] measured uptime between watchdog resets from ~9 s
to ~1.1 s". That is a **mean time between watchdog resets on a board that was
already reset-looping**. A healthy board has no such quantity — it is
undefined/infinite.

What this pass actually measured, per its own "Hardware verification (this
pass -- completed)" text, is two **observation windows** in which no reset
occurred: ~40 s pre-flash (instrument: `safety_get_fw_version()`'s `boot_id`)
and ~60 s post-flash (instrument: `safety_get_diag()`'s `uptime`, 152011 ms ->
212011 ms). So:

- **40 and 60 are not the same quantity measured before and after.** They are
  window lengths, and they used two different instruments. The apparent
  "50% change" is an artifact of the observer polling longer the second time,
  nothing more. Reading them as a cadence lengthening 40 s -> 60 s is a
  misreading the document's own headline wording invites.
- **Neither figure is comparable to the design doc's ~9 s** for the same
  reason: ~9 s was a reset interval on a broken board, not a nominal.
- **The acceptance criterion — "does not reintroduce the 2026-08-23
  regression" — is nevertheless genuinely met.** A 1.1 s reset interval would
  be unmissable inside either window. The evidence supports the conclusion;
  only the label on it is wrong.

**Stronger evidence, obtained in this review (E):** the bench Pico is
currently running `88bb4333`, whose `stack_margin_poller.c`, `log_task.c` and
`kilnlink_stack_margin.c` are **byte-identical to `17de0d11`**
(`git diff --stat 17de0d11 88bb4333 --` over those three paths returns empty).
Two `safety_get_diag()` reads ~11 minutes apart showed `uptime` advancing
monotonically 1 496 006 ms -> 2 160 006 ms with no `boot_id` change — i.e.
**~36 minutes of continuous uptime with the poller live and zero watchdog
resets**, roughly 36x the window this document claims. The poller is confirmed
not to reintroduce the regression.

**Recommended wording fix (not applied here — this section is the correction):**
say "no watchdog reset observed in a ~40 s pre-flash and ~60 s post-flash
window", never "watchdog cadence ~40 s / ~60 s".

### 2. `log_task` headroom for the poller — adequate, but the doc omits the expensive half of the call

(R) `log_task` is 512 words / 2048 B (`LOG_TASK_STACK_WORDS`, `log_task.c`),
live floor 422 words free = 90 words (360 B) used. `stack_margin_poller_tick()`
is called **sequentially** in `log_task_fn()`'s loop, not nested inside the
log-send path, so it never stacks on top of that path's own ~1130-1180 B
worst case (`log_task.c`'s own arithmetic). Even at that estimated peak,
~870 B remains for the poller's own frame, which is a handful of scalars.
**Headroom is sufficient.**

Two caveats the document does not state:

- **The 90-words-used floor is not evidence of comfort.** It means the
  log-send branch (`xQueueReceive` returning `pdTRUE`) has essentially never
  run since boot on this board — `LOG_LEVEL_WARN` default, quiet system. The
  floor understates `log_task`'s real peak by roughly an order of magnitude.
  This is the document's own floor-not-worst-case caveat applying to its own
  tightest-looking number, and it should be said where that number is read.
- **`stack_margin_poller_tick()` is not "a single bounded FreeRTOS API call"**
  as `log_task.c`'s new comment asserts. It is **two**, and the one the
  comment omits is the costly one: `xTaskGetHandle(slot->task_name)` calls
  `vTaskSuspendAll()` and linearly walks all `configMAX_PRIORITIES` ready
  lists plus the delayed, suspended and terminated lists doing name compares
  before `xTaskResumeAll()`
  (`C:\pico-tools\FreeRTOS-Kernel\tasks.c:4370-4400`, read directly (R)).
  That is a **scheduler suspension every ~500 ms, forever**, on the safety
  processor — precisely the category of cost this whole design exists to
  avoid. `uxTaskGetStackHighWaterMark()` itself takes no lock; it is the name
  lookup that does.
  It is bounded and short (9 tasks, ~36 min of live evidence above shows it
  is not hurting anything today), so this is a **quality finding, not a
  defect** — but it is trivially avoidable: resolve each slot's
  `TaskHandle_t` once, cache it, and skip `xTaskGetHandle()` on every
  subsequent tick. Recommended.

No interference with `link_task` mid-frame (R): the poller touches only its
own file-static `s_slots[]`; `link_task` reads it solely through
`stack_margin_poller_snapshot()`; there is no shared UART, buffer or lock
between them, and the reply payload (47 B) does not change
`link_task_send_broadcast_to()`'s stack shape — its `raw[263]`/`stuffed[528]`
buffers are fixed-size regardless of payload.

### 3. Units — no error found

(R) Traced every boundary: `uxTaskGetStackHighWaterMark()` (words, vanilla
FreeRTOS port) -> `s_slots[].high_water_words` -> `kilnlink_stack_margin_encode`
(u16 LE, words) -> wire -> `kilnlink_stack_margin_decode` -> ESP
`kilnlink_stack_margin_t` -> `safety_stack_margin_build_json()` ->
`"units":"words"`, field names `high_water_words`/`stack_total_words`.
**There is no conversion anywhere, and no display path that divides or
multiplies by 4** — the words value travels end to end and is labelled at the
only place it surfaces. That is the correct design given the deliberate
ESP/Pico unit split, and it means the classic silent units defect cannot
occur on this path. The document's own table's byte conversions check out
arithmetically (link_task 1218 words = 4872 B; 4872 - 4736 = 136 B).

### 4. Protocol bump — moved together; a v12 peer does NOT "fail loudly", by deliberate design

(R/E) `KILNLINK_PROTOCOL_VERSION` is 13 at `kilnlink_version.h:279`, a single
source of truth consumed by both firmwares and `CommonFW`; a repo-wide grep
found **nothing still asserting 12** (`test_dashboard_protocol_version.c`'s
comment was updated 12 -> 13 in `17de0d11`). The PC<->ESP
`UART_PROTOCOL_VERSION` is correctly untouched at 11 — confirmed live (E):
`get_board_state()` reports `fw_version.protocol_version: 11` and
`safety_fw_version.protocol_version: 13` simultaneously, the two-numbers
invariant holding on real hardware.

**Correction to the review brief's premise:** a v12 peer meeting a v13 peer
does **not** fail loudly, and is not supposed to.
`KILNLINK_MIN_COMPATIBLE` is **7** (`kilnlink_version.h:332`), deliberately
not raised, so `link_frame_versions_compatible()` passes the pair and the link
comes up normally. The old peer simply has no dispatch case for `0x2B`, never
replies, and `safety_link_get_stack_margin()` returns `ESP_ERR_TIMEOUT` with
an explicit log line naming that exact cause — **silence, never a fabricated
reading, never a misparse**. The "+121 crc/framing errors in 3 s" loud-failure
behaviour belongs to a `MIN_COMPATIBLE`-raising (breaking) bump; this one is
additive and correctly is not that. Both directions are additive: the ESP only
ever *sends* `0x2B`, the Pico only ever *sends* `0x2C`, so no byte an old peer
cannot handle is ever pushed at it unsolicited.

**Fingerprint manifest — refreshed genuinely, but with one real gap (E).**
`wire_protocol_fingerprints.json` gained four new entries and moved
its fingerprint/version pair (a SHA-256 prefix of the wire-relevant defines,
NOT a commit hash) from the old value at version 12 to a new one at version
13 — a real recomputation, not a hand-edited number, and `check_wire_protocol_fingerprint.ps1` passes (E).
**However:** the extraction regex for the kilnlink link is
`^KILNLINK_.*(_LEN|_OFF)\w*$`. Running `extract_defines()` directly against
`kilnlink_stack_margin.h` (E) returns only `KILNLINK_STACK_MARGIN_ENTRY_LEN`
and `KILNLINK_STACK_MARGIN_LEN` — **`KILNLINK_STACK_MARGIN_NUM_TASKS` is not
captured**, yet `KILNLINK_STACK_MARGIN_LEN`'s recorded *text* is
`(1u + 1u + KILNLINK_STACK_MARGIN_NUM_TASKS * KILNLINK_STACK_MARGIN_ENTRY_LEN)`.
Changing `NUM_TASKS` 9 -> 10 changes the wire frame length 47 -> 52 while
leaving every fingerprinted string byte-identical, so the check **would pass a
breaking layout change** — exactly the failure class its own docstring
("Instance 1: the reply grew two bytes... with no version bump") exists to
catch. This is the first kilnlink `_LEN` macro defined in terms of a symbol
outside the captured set (`PARAM_MAX_LEN` and `STATUS_LEN` both reference
macros that ARE captured), so the gap is newly introduced by this pass even
though the regex is pre-existing. **Recommended fix:** widen the kilnlink
source pattern to also capture `_NUM_`/`_COUNT` macros, and negative-test it
by bumping `NUM_TASKS` and confirming a FAIL.

### 5. Frame decode path — no input escapes validation

(R) `kilnlink_stack_margin_decode()` rejects `len != 47` before touching the
buffer, then rejects a wrong cmd byte; every subsequent read is at a fixed
offset in `[0,46]` of a buffer proven to be exactly 47 bytes. There is no
length-derived index, no caller-supplied count, no variable-length field —
truncated, oversized and malformed inputs are all rejected by the same single
equality test. `kilnlink_get_stack_margin_decode()` is the same shape at
`len != 1`. **I could not construct a length or index that escapes
validation.**

Defence in depth on both ends is real, not nominal: the Pico gates
`frame.length == 1` in `link_task_handle_raw_frame()` *before* calling the
handler, which decodes again; the ESP gates `msg.length == KILNLINK_STACK_MARGIN_LEN`
in `safety_drain_inbox_ex()` before stashing, and decodes again in
`safety_link_get_stack_margin()`.

Fuzz registration is genuine (R): both decoders appear in
`test_fuzz_payloads.c`'s `k_cases[]` with canary-armed adapters and
`MAX_LEN = LEN + 32`, so over-long inputs are actually generated.

One nit, **not a new defect**: neither decoder null-checks `payload`, so
`decode(NULL, 47, out)` would fault. This is the house convention across
every kilnlink decoder (`kilnlink_ct_cal.c`, `kilnlink_reboot_result.c`
verified (R) — `ct_cal` does not even guard `out`), the length contract is
caller-side, and nothing reachable passes NULL. Flagged for completeness only.

### 6. Partial rounds — cannot be misread as complete. Staleness — cannot be detected at all

(R) **The partial-round guarantee holds, and the ordering is correct by
construction.** `stack_margin_poller_snapshot()` reads
`s_rounds_completed` *first*, then the entries. `s_rounds_completed` is only
incremented after all nine slots have been visited, and a slot's value only
ever transitions sentinel -> measured. So `rounds_completed >= 1` implies every
slot was written before that counter moved; the only possible torn read is the
*conservative* direction (`rounds_completed` read as 0 while entries are in
fact all measured), which under-claims. **A partial round cannot be reported as
complete.** Single-word aligned accesses on Cortex-M0+ make the individual
field reads atomic, as the module comment claims.

**Two real weaknesses the document does not state:**

- **`all_measured` can lie in one reachable case.** The poller advances its
  index and eventually increments `s_rounds_completed` even when
  `xTaskGetHandle()` returns NULL (task never created, or — far more likely —
  the hand-maintained `task_name` string drifted from the `xTaskCreate()`
  name). That yields `rounds_completed > 0` with a slot still at
  `UNMEASURED`, and `safety_stack_margin_http.c` derives
  `"all_measured": rounds_completed > 0` **unconditionally**, so the JSON
  would read `"all_measured":true` alongside that entry's own
  `"measured":false`. Self-contradictory output. `kilnlink_stack_margin.h`'s
  contract ("0 means at least one entry is still that sentinel") implies the
  converse, which does not hold. **Recommended fix:** derive `all_measured`
  by scanning the nine entries for the sentinel, not from `rounds_completed`.
  This is the same hand-maintained-duplicate-table hazard the document already
  names as a known limitation, surfacing a second time in a place it did not
  anticipate.
- **There is no freshness signal whatsoever.** `rounds_completed` saturates at
  255 — confirmed live (E): the board reports `"rounds_completed":255`, so
  after ~2 minutes of uptime the field is a constant and carries no
  information beyond ">= 1". If `log_task` ever hung, the cache would freeze and
  the endpoint would keep serving the last values with `all_measured:true` and
  no indication they are minutes or hours stale. **A consumer cannot
  distinguish a fresh set from a stale one.** For a diagnostic endpoint whose
  entire purpose is to catch a task that is about to die, that is the wrong
  failure mode. **Recommended:** carry a per-round timestamp or a free-running
  tick alongside the snapshot (a u32 `last_tick_ms` would fit; the frame is
  fixed-length and would need a version bump, so this is a follow-up, not a
  correction).

### 7. Incidental fixes — independently verified

(R) `check_00_kilnfw_target_build.ps1` now calls `Mirror-Tree` on
`firmware\CommonFW`, and `Mirror-Tree` uses `robocopy /MIR` — a true purging
mirror, so a *removed* CommonFW file is removed from the worktree too, not
just added ones copied. `firmware\CommonFW` is also added to
`Get-NewestSourceTime`'s freshness scan. The stale-worktree failure mode is
genuinely closed: CommonFW is now re-mirrored on **every** run rather than
reached through the worktree's own pinned git checkout, so it cannot go stale
at all — the original bug (worktree git ref frozen at first-creation commit)
is structurally eliminated rather than papered over. (E) `check_00` is among
the 94 passing checks below.

(R) `test_safety_link_compile.c`'s link line gained the two missing
`kilnlink_*stack_margin.c` sources in `build_host_tests.ps1`'s `$slExtra`, and
its `s_compat_frames[]` gained a `min_version 13` row for
`KILNLINK_STACK_MARGIN_CMD` — which is what actually proves
`safety_drain_inbox_ex()` has a dispatch case for `0x2C`.

Note for future readers: `$totalExpected` is now **40**, not the 39 this
document records — another session's `sim_scenarios.c` landed as the 40th
executable after `ec1e8666`.

### 8. Live board — serving, plausible, correctly labelled (E)

`curl http://192.168.1.156/api/saftyfw_stack_margin` returns **1102 bytes**
(this document says 1101; a one-byte measurement difference, immaterial, and
well inside the 1536 B budget). All nine tasks present, all `measured:true`,
`units:"words"`, `floor_not_worst_case:true` and the prose `note` all present
as claimed. Values are plausible and consistent with this document's table —
`relay_owner` 214/256, `log_task` 422/512, `watchdog_task` 194/256,
`thermo_task` 710/1024, `current_task` 1195/1536, `update_task` 1076/1536 are
**identical**; `discrete_task` reads 728 (doc: 732) and `link_task` reads
**1526** (doc: 1342) — both differences are in the safe direction (more free)
and are expected: this is a different boot of a different build
(`88bb4333`, boot_id 115), and a high-water floor is per-boot.

Caveat worth recording: **`link_task`'s headline number in this document —
1342 words free, 1218 used, "136 B above its static lower bound" — is a
single-boot observation and does not reproduce on the current boot** (1526
free / 1034 used, i.e. *below* the 1184-word static bound). The
document's own framing of it as "worth carrying into that investigation" is
still right, but it should not be quoted as a stable figure.

Board left untouched and healthy: armed, `trip_reason 0`, `trip_mask 0x0000`,
relays 0, `profiles_exec_status.state == 0`, no firing, no unacknowledged
crash. Read-only access throughout; nothing was flashed by this review.

### Verdict

The feature is **sound and correctly built**. No units error, no frame input
that escapes validation, no way to misread a partial round as complete, and
the protocol bump is coherent across both firmwares with the UART version
correctly left alone. Three things should be corrected in this document or in
follow-up work, in order of importance:

1. **The "watchdog cadence 40 s -> 60 s" framing is wrong** and should be
   restated as two no-reset observation windows. The criterion is met; the
   label is not. (Now superseded by ~36 min of continuous poller-live uptime
   measured in this review.)
2. **`all_measured` is derived from `rounds_completed` and can contradict its
   own per-entry `measured` field**; and the endpoint carries no freshness
   signal at all once `rounds_completed` saturates at 255.
3. **The wire fingerprint check does not cover `KILNLINK_STACK_MARGIN_NUM_TASKS`**,
   so a 9 -> 10 change would alter the frame length while the manifest still
   passes.

Quality follow-up: cache the nine `TaskHandle_t`s instead of calling
`xTaskGetHandle()` — a scheduler suspension — every 500 ms forever.

**Checks: `tools/run_all_checks.ps1` -> 94 passed, 0 skipped, 0 failed (E).**
The three failures this document records (`check_fuzzy_gain_mirror_drift.ps1`,
`check_doc_citations.ps1`, `check_test_c_files_wired.ps1`) have since been
resolved by their owning sessions; all three pass now.
