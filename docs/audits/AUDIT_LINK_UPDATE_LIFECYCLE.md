# Audit: ESP<->Pico safety link across the update lifecycle (2026-10-14)

Scope: kilnlink (`firmware/CommonFW`), the KilnFW `safety_link_*` driver, the
SaftyFW link/guard tasks, and the recovery image, followed through one GitHub
release update from fetch to the new application, plus Pico auto-update and a
protocol mismatch. Device bugs only. Source read at origin/dev `153c12cf5`. The
board was not touched.

For each phase there are three questions:
- (a) Does heat stay off when it must?
- (b) Can a benign update leave a latched trip that a human must clear?
- (c) Is every trip cleared only by the documented rules?

Counts: HIGH 0, MED 1, LOW 3, INFO 6.

## Rules this audit checks against

- The Pico latch is RAM-only. It exits only by CLEAR_TRIP, an E-stop cycle or
  a Pico reboot.
- `safety_guards_try_clear` refuses while the reason's condition still holds.
  For S6b it refuses while `!link_up` (`firmware/SaftyFW/src/safety_guards.c:376`).
  S9 is refused unconditionally.
- An 18+ peer's clear must carry the Pico boot_id.
- KilnFW has exactly one automatic clear: the boot-clean S6a release
  (`firmware/KilnFW/App/drivers/safety/safety_link_frames.c:1004-1265`). It
  sends CLEAR_TRIP only when all of these hold:
  - reason 6 (S6a) only;
  - within 30 s of an ESP boot whose bring-up raised no fault source
    (`main_control_bringup.c:116-123`, `SAFETY_LINK_BOOT_CLEAN_WINDOW_MS`
    at `safety_link_frames.c:1066`);
  - `fault_sources == 0` at the attempt (`:1224`);
  - at most 3 attempts.
- Everything else, S6b included, needs a person (or the bench owner rule of
  2026-10-02).

## Phase 1: GitHub fetch and stage (XIP, UART ISR not in IRAM)

- (a) Yes. The install and stage routes refuse while running or while relays
  are energized, so no heat is commanded through the fetch.
- (b) Only if the ESP poll loop sees no Pico telemetry for 1.5 s
  (`SAFETY_LINK_STALE_MS`, `safety_link.h:474`). Then
  `safety_update_health` asserts `SAFETY_FAULT_SRC_SAFETY_LINK`
  (`safety_link_poll.c:313`), GPIO6 rises, and after 200 ms the Pico latches
  S6a. Mid-fetch is outside the 30 s boot-clean window, so that latch would
  need a human.
- (c) Yes.

### INFO-1: the remaining cache-off windows are far below the 1.5 s stale bound

`docs/audits/LINK_CRC_DURING_FETCH.md` measured 150-400 ms stalls per 64 KB
erase on the pre-XIP build, with the ISR not in IRAM. Those stalls cost frames
(crc/length counters, 3 reply timeouts) but never reached link-down. Under XIP
from PSRAM the erase no longer freezes the cache. The windows that remain
(mmap, `esp_image_verify`, `esp_ota_end`, stage verify) are shorter.

One 128 B FIFO overrun drops a frame, not the link. Reaching S6a would need
about three consecutive 500 ms status pushes lost in a row.

No fix needed. The separate XIP cache-off audit owns those windows. If one
ever exceeds about 1 s, this finding becomes a benign-update S6a.

## Phase 2: reboot into recovery (Pico sees the ESP vanish)

- (a) Yes:
  - The install route refuses unless the authoritative relay read says off
    (`ota_http_recovery.c:311-317`).
  - The SX1509 keeps its outputs across the ESP reset.
  - Recovery pulses the SX1509 reset and then holds K1/K2/K3/K5 and IO5 low,
    verified (`recovery_main.c:41`, `recovery_io.c:105-115`).
  - K4 closes only on REQUEST_ENABLE, which recovery cannot send.
  - The S6b soft tier needs current, and there is none.
- (b) Yes. See MED-1.
- (c) Yes. Recovery never sends CLEAR_TRIP (`recovery_pico.c:18`, `:310`).

### MED-1: an unattended install's recovery dwell is guarded only by the 120 s S6b hard tier; a benign update can latch S6b that only a person can clear

**Where:**
- `ota_http_recovery.c:249` sends the one ANNOUNCE_REBOOT.
- `firmware/SaftyFW/src/tasks/safety_core.c:172` sets the grace window,
  `REBOOT_GRACE_WINDOW_MS 20000`.
- `safety_guards.c:702-749` is the S6b block. Grace gates only the `trip()`
  calls, never the elapsed accumulator.
- `firmware/KilnFW_recovery/main/recovery_main.c:88-128` brings up the
  recovery image: Wi-Fi, then HTTP start with retries and up to
  `RHEALTH_HTTP_MAX_RESTARTS` full restarts, and only then
  `recovery_install_boot()`.
- `recovery_apply_esp.c:174-175` waits 1500 ms, then calls `esp_restart()`.

**Scenario:**
1. POST /api/update/install.
2. The app waits 500 ms, sends ANNOUNCE_REBOOT, and resets.
3. The recovery image sends nothing on UART1 unless a Pico upload is running.
   GPIO6 is never driven (`recovery_pico.c:19-20`), and no frame resets the
   Pico's `s_last_valid_frame_tick` (`link_task.c:3266`).
4. The Pico's `s6b_link_down_elapsed_s` counts the whole dwell: recovery boot,
   Wi-Fi, HTTP, erasing and copying about 3 MB from stage to app, hashing and
   verifying, the 1.5 s wait, then the new app's boot up to its first valid
   frame.
5. The 20 s grace expires long before that. Only `link_dead_hard_s` (120 s
   default) stands between the update and an S6b latch.

**Evidence of the margin:**
- benchupd4 and benchupd5 completed with no trip.
- A dwell of about 105 s, measured another time, did not trip.
- No unattended install has been timestamped from reset to the first valid
  frame.

**What eats the margin:**
- an HTTP start retry or an HTTP-failure restart before the install even
  begins;
- a larger image;
- a slower flash part or a degraded flash;
- a lowered `link_dead_hard_s` in commissioning.

Any of these turns a benign install into a latched S6b.

**Consequence:**
- Heat stays off. The latch is fail-safe.
- KilnFW's only automatic clear handles reason 6, not 7
  (`safety_link_frames.c:1004`, `:1227`). So a field user with no bench
  standing rule must clear S6b by hand after every update that ran long.
- `docs/RECOVERY_IMAGE_PLAN.md:190-192` and `docs/OTA_SINGLE_SLOT.md:126` call
  an S6b latch "expected" after any recovery stay.
- `docs/GITHUB_RELEASE_UPDATE_PLAN.md` describes the install as unattended and
  never mentions S6b.

**Fix:** any one of these.
1. Have the recovery image send a kilnlink frame on UART1 every few seconds
   while `recovery_install_boot()` runs. Any cleanly decoded frame resets the
   Pico's tick, for example the ANNOUNCE_VERSION it already packs
   (`recovery_pico_proto.c:242`) or a GET_STATUS. The cost is a UART install
   for the length of the apply. `recovery_pico.c` already does this for a
   transfer.
2. Call `recovery_install_boot()` before Wi-Fi and HTTP when an install
   request is pending, to cut the dwell.
3. At minimum, timestamp the dwell on the bench (reset to first valid frame)
   and state the margin and the S6b outcome in `GITHUB_RELEASE_UPDATE_PLAN.md`.

## Phase 3: the recovery image's own link behavior

- (a) Yes. This is the same as phase 2: there is no arm or heat command in
  the image.
- (b) Yes, through MED-1. A Pico upload from recovery is refused while any
  trip is latched (`firmware/SaftyFW/src/tasks/update_task.c:843`, `:893`).
  So once S6b has latched during a long dwell, recovery can no longer update
  the Pico until the app clears it. Clearing it means leaving recovery.
- (c) Yes.

### LOW-1: recovery leaves the isolated fault line undriven for the whole dwell

**Where:**
- `recovery_pico.c:19-20` says "GPIO6 is never driven".
- The application's own bring-up comment at
  `firmware/KilnFW/App/drivers/safety/safety_link.c:425-430` reads: "a
  floating gate on U1 is an undefined fault state at the safety processor".

**Scenario:** Recovery sits for tens of seconds to minutes with GPIO6 at its
reset default. Whether the Pico reads mainFault as asserted then depends on
the U1 drive circuit and on leakage, not on firmware.

**Consequence:**
- If it reads as asserted, the Pico latches S6a during the dwell. This is
  benign: the new app's boot-clean release clears it within 30 s, and a
  latched S6a also masks a later S6b because the first trip wins.
- If it floats intermittently, the trip reason is non-deterministic.
- benchupd4/5 show no trip, so this board reads it as released.

**Fix:** In `recovery_main.c`, drive GPIO6 low as an output next to
`recovery_io_hold_relays_off()`. This is the same known-good level the app
latches first. Alternatively, record in HARDWARE.md that U1's input is
defined when undriven.

## Phase 4: reboot into the new application

- (a) Yes:
  - `safety_link_start` drives GPIO6 low before anything else
    (`safety_link.c:425-440`).
  - Heat is then blocked until the peer version is known and compatible
    (`safety_link_poll.c:185-188`).
- (b) Only S6a, which is released automatically within the documented
  boot-clean window. S6b from MED-1 is not released.
- (c) Yes. The boot-clean release is the one documented automatic clear and
  is bounded as described in "Rules this audit checks against".

### INFO-2: the new boot may assert GPIO6 itself before FW_VERSION lands, and the boot-clean release covers it

The poll loop does GET_STATUS, then a FW_VERSION request, then health
(`safety_link_poll.c:605-660`).

If the FW_VERSION reply misses the first tick, the following happens:
1. `version_mismatch` is true because the version is unknown.
2. The fault asserts and is held for at least 300 ms
   (`SAFETY_FAULT_MIN_HOLD_MS`, `safety_link.h:877`).
3. The Pico latches S6a.
4. Once the version is known and the hold has passed, `fault_sources` returns
   to 0.
5. The first DIAG that shows TRIPPED/MAIN_FAULT triggers the release.

This is the same mechanism as the documented dual-reflash S6a. It depends on a
DIAG arriving within 30 s of `mark_boot_clean()`, which is about 2 s DIAG
cadence once the link is up.

### INFO-3: reset-one-side pairs are handled across both reboots

- **ESP side.** `safety_link_start` memsets the whole link and draws a fresh
  random `esp_boot_id` (`safety_link.c:361`, `:378`).
- **Pico reboot as seen by the ESP.** `safety_note_pico_reboot_locked`
  (`safety_link_frames.c:270-300`) clears all of these:
  - `trip_last_seq`
  - `trip_event_ever_received`
  - the cached DIAG `trip_seq`
  - the cached DIAG `boot_id`
  - the re-announce budget
- **ESP boot_id change as seen by the Pico.** The Pico forgets the peer
  version and resets the context session
  (`firmware/SaftyFW/src/tasks/link_task.c:1412-1442`, `:1504`).
- **Boot-clean retry budget.** It is anchored to the ESP boot, not to the
  Pico's boot_id, by design (`safety_link_frames.c:1052-1060`).

No unpaired reset was found.

## Phase 5: Pico auto-update at boot

### INFO-4: the auto-update path is compiled out in every build

`PICO_AUTO_UPDATE_ASSUME_BOOTLOADER_PRESENT` defaults to 0
(`firmware/KilnFW/App/drivers/net/pico_auto_update_boot.c:302-304`). No
CMake, Kconfig, sdkconfig or script sets it. The task logs, clears its
blocking state and exits, so the phase is inert today.

### LOW-2: a Pico relay (manual today, automatic once enabled) leaves an S6a that the boot-clean window cannot release

**Where:**
- `ota_pico_relay.c:405-411` marks the update in progress, but that mark is
  text only. The fault keeps asserting on `!up`, as its own comment and
  `safety_link_poll.c:236-238` say.
- `safety_link_frames.c:1066` is the 30 s window.
- `pico_auto_update_boot.c:48` is the 20 s identity wait.

**Scenario:**
1. The Pico erases (the bootloader can be silent for tens of seconds,
   `recovery_pico.c:11-12`) and reboots into the new slot.
2. The ESP sees no telemetry for more than 1.5 s. It asserts GPIO6 and clears
   `peer_version_known` (`safety_reset_stale_peer_info_if_link_down`,
   `safety_link.c:304-330`).
3. The new Pico image boots with mainFault asserted. Its guards run from boot,
   with no S6a startup grace (reverted 2026-09-07). So it latches S6a about
   200 ms later.
4. The ESP deasserts only after the new Pico's FW_VERSION and the 300 ms hold.
5. For an operator-started relay, the ESP boot was long ago. For the future
   boot-time auto-update, the 20 s identity wait plus the erase and transfer
   alone exceed 30 s. In both cases `s_boot_clean` has expired, and the S6a
   stays latched.

**Consequence:**
- Heat stays off.
- Every successful Pico update ends with a latched S6a that a person must
  clear. The bench procedure already expects this
  (`docs/PICO_AUTO_UPDATE.md:222`).
- For an unattended boot-time auto-update this answers (b) yes, every time.

**Fix:** Before setting `PICO_AUTO_UPDATE_ASSUME_BOOTLOADER_PRESENT=1`, give the
relay a bounded release of the same shape as the boot-clean one. Arm a
one-shot, reason-6-only, `fault_sources == 0`-gated release when the relay
reports COMPLETE and the new Pico boot_id is first seen. Anchor its budget to
that relay event, not to the Pico boot_id (reset-one-side rule).

## Phase 6: protocol version mismatch between a new ESP image and the running Pico

`KILNLINK_PROTOCOL_VERSION` is 18 and `KILNLINK_MIN_COMPATIBLE` is 7
(`firmware/CommonFW/include/kilnlink/kilnlink_version.h:375`, `:444`). A
17-protocol Pico is therefore compatible with today's 18-protocol ESP, and
nothing in this phase is reachable today.

### LOW-3: a future MIN_COMPATIBLE bump wedges the pair: S6a latched and unclearable, Pico update refused

**Where:**
- `safety_link_poll.c:188` and `:313` treat an incompatible peer exactly like
  a dead link, so GPIO6 stays asserted.
- The boot-clean release requires `fault_sources == 0`
  (`safety_link_frames.c:1224`).
- S6a cannot clear while mainFault holds.
- The Pico application refuses UPDATE_BEGIN while any trip is latched
  (`update_task.c:843`, `:893`).
- The app relay additionally gates on a protocol mismatch, overridable with
  force (`ota_http_pico.c:312-331`).

**Scenario:** A release raises MIN_COMPATIBLE above the field Pico's version.
1. The new ESP boots and asserts GPIO6 permanently.
2. The Pico latches S6a.
3. The latch cannot be cleared.
4. Because the latch is held, the Pico refuses the link update that would
   resolve the mismatch.

**Consequence:**
- Heat stays off. This is correct, but the board stays dead until someone
  downgrades the ESP through stage, recovery and apply, or flashes the Pico by
  SWD or the bootloader path.
- `firmware/SaftyFW/docs/ARCHITECTURE.md:538-560` says a mismatch latches no
  trip by itself. That is true of the Pico's own logic, but the ESP's GPIO6
  policy produces an S6a anyway, and the doc does not say so.

**Fix:**
- Make any MIN_COMPATIBLE bump ship the Pico image first. This means a release
  gate in the update path that refuses to stage an ESP image whose
  MIN_COMPATIBLE exceeds the running Pico's reported version.
- Or let `update_task.c` accept UPDATE_BEGIN while only S6a is latched,
  since S6a with relays off is the expected state here.
- Add one sentence to ARCHITECTURE.md about the S6a.

## Cross-phase notes

### INFO-5: S6b grace semantics fail safe

The ANNOUNCE_REBOOT window suppresses only S6b's `trip()`; the accumulator keeps
counting (`safety_guards.c:713-728`). It cannot extend heat. The window also
cannot exceed 20 s because it is a local compile-time constant on the Pico, not
something the ESP sends. No defect.

### INFO-6: no automatic clear other than boot-clean S6a exists on either side

There is no CLEAR_TRIP in the recovery image (`recovery_pico.c:18`). The ESP
side has no other sender of CLEAR_TRIP except the operator routes. Every clear
observed in this lifecycle goes through `safety_guards_try_clear` and its
boot_id/trip_seq rules.
