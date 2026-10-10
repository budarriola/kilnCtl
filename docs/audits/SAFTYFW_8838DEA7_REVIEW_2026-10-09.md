# Review: 8838dea7 (SaftyFW peer-version reset on ESP boot_id change), 2026-10-09

Read-only Opus review of origin/dev commit `8838dea7` ("SaftyFW: boot_id change
forgets peer protocol version; tested push_context session trigger; metadata
buffer assert; commit-readback text names staging expiry"). Code cited at
origin/dev `f293f196`; no file touched by the commit has changed since.

Summary: items 2-4 are fine. Item 1 has one HIGH ordering defect. On every
ordinary ESP reboot the Pico forgets the ESP's protocol version after it has
already learned it, and it never learns it again that session. While the version
is forgotten, the M4 trip_seq binding is off and V2/V3 status and ROLLBACK_RESULT
are not sent, for as long as the ESP keeps running.

## HIGH-1: the reset runs AFTER the new boot's ANNOUNCE burst, so the version stays 0 for the whole ESP session

Where:
- `firmware/SaftyFW/src/tasks/link_staging.c` `link_staging_apply_context_session()` (sets `*peer_protocol_version = 0` on a boot_id change)
- `firmware/SaftyFW/src/tasks/link_task.c:1383-1390` (call site in `link_task_handle_push_context()`)
- ESP order: `firmware/KilnFW/App/drivers/safety/safety_link_poll.c:555` sends `safety_link_send_announce_version_burst()` (4 copies, 250 ms apart, blocking, `safety_link_frames.c:213`) BEFORE the loop. The first `safety_build_and_send_context()` runs at the end of the first loop pass (`safety_link_poll.c:682`).

Sequence on any ESP reboot (OTA, sw_reset, recovery_exit, watchdog, brown-out)
while the Pico keeps running:
1. ESP boots with a new `esp_boot_id` and sends ANNOUNCE_VERSION x4. The Pico sets `s_peer_protocol_version = 17` (`link_task.c:1460`).
2. About 1 s later the ESP sends its first PUSH_CONTEXT, carrying the new boot_id. `link_staging_apply_context_session()` sees `boot_id != prev_boot_id` and sets the version back to 0.
3. Nothing re-announces. The ESP sends a re-burst only when it sees a Pico boot_id change (`safety_note_pico_reboot_locked()`, `safety_link_frames.c:293`), or in one narrow case: an S6a boot-clear blocked on a missing trip_seq (`safety_link_frames.c:1110-1117`). A healthy ESP reboot hits neither case. The version stays 0 until the Pico itself reboots.

What runs on version 0 for the rest of the session:
- **M4 is defeated.** `link_task_send_diag()` sends the 30-byte DIAG with no trip_seq (`link_task.c:1196`). The ESP then has `diag_trip_seq_known=false` (`safety_link_frames.c:1050`), so every operator clear is sent unbound (`safety_link_commands.c:143-147`). `link_frame_decide_clear_trip()` accepts it because `link_frame_trip_seq_supported(0)` is false (`link_frame.c:279`). This brings back exactly the case M4 closed: a delayed, duplicated or stale CLEAR_TRIP with a matching mask clears a NEW trip of the same reason that the operator never saw. The mask check is the only remaining guard, and it cannot tell two occurrences of the same guard apart.
- V2/V3 Frame A are not sent (`link_task.c:890,897`). On every V1 frame the ESP clears `borrowed_known`, `cj_valid_known`, `tc_config_reasserted_known` and `pico_active_slot_known` (`safety_link_frames.c:764-773`). `pico_auto_update_boot.c` `wait_for_wire_active_slot()` runs right after the ESP boots and can lose the wire slot to this. If so, it spends its 3 s budget and falls back to the persisted alternation guess. Whether it loses the slot depends on the race between the burst and the first context, but the slot is unknown for the rest of the session either way.
- ROLLBACK_RESULT is suppressed (`link_task.c:2247`), so `ota_rollback(pico)` after any ESP reboot gets no result frame.

This is not "no weaker than a fresh Pico boot". After a fresh Pico boot the
version is 0 for about one burst: the ESP sees the Pico's new boot_id in
FW_VERSION and re-announces. Here the version is 0 indefinitely. The case the
commit targets (the whole 4-copy burst lost, with an older ESP behind it) is
rarer than the case it breaks (every ESP reboot).

Before this commit the Pico kept the previous boot's version until the new
burst overwrote it. That was stale only when all 4 copies were lost.

**Fix (recommended):** tie the cached version to the boot_id that announced it.
ANNOUNCE_VERSION already carries the ESP boot_id (`kilnlink_announce_t.boot_id`).
The ESP fills it from the same `link->esp_boot_id` it puts in PUSH_CONTEXT
(`safety_link_frames.c:183` and `:457`).
- In `link_task_handle_announce_version()`, record `s_peer_version_boot_id = msg.boot_id` and set `s_peer_version_boot_id_known = true`. Do this together with `s_peer_protocol_version`.
- In `link_staging_apply_context_session()`, pass in the announced boot_id. Zero the version only when `snap.boot_id != announced_boot_id`, rather than on any change from the previous context's boot_id. An ESP that announced and then sent context keeps its version. An ESP that rebooted and lost its entire burst still drops to 0.
- Also reset `s_peer_version_boot_id_known` in `link_task_start()` next to `s_peer_protocol_version = 0` (`link_task.c:3415`).

A cheaper alternative, if wire-carried boot_id is unwanted: on a boot_id change,
zero the version only if no ANNOUNCE has arrived since the previous context
frame. Set a flag in the announce handler and clear it in push_context. This is
less exact, because a stale announce from the old boot can be in flight.

Optional defence in depth on the ESP: in `safety_apply_diag()`, if
`peer_version_known && peer_protocol_version >= 17` and the DIAG is the 30-byte
form, set `reannounce_pending` regardless of trip state. Today that exists only
inside the S6a boot-clear branch (`safety_link_frames.c:1110-1117`). With this,
any version loss on the Pico (including the "burst lost" case the commit
targets) heals within one poll period.

## MED-1: the tests pin the pure function, not the ordering that breaks it

Where: `firmware/SaftyFW/test/test_link_staging.c:258-284` (`test_apply_context_session`).

The test is not vacuous for what it checks. Mutating the
`boot_id != prev_boot_id` condition or the reset would fail it. But it only
models "context, then context". It never models the real wire order
ANNOUNCE -> PUSH_CONTEXT (new boot_id), where the expected result is "version
preserved". As written, HIGH-1 fails no test. The call site
(`link_task.c:1383-1390`) is still in the untested FreeRTOS file, and that is the
place where the version and the announce interact.

Fix: with the HIGH-1 fix, add a case that announces boot_id B and then applies
context B after context A. The expected result is version 17 kept. Add the
inverse too: announce A, then context B, expecting version 0. Negative-test the
pair with `tools\negtest.ps1 -Preset saftyfw-host`.

## LOW-1: `s_degraded_no_context` is not reset with the version (paired state)

Where: `link_task.c:1467` (written only by ANNOUNCE) against the new reset of
`s_peer_protocol_version`.

After a boot_id change the version becomes 0 but `s_degraded_no_context` keeps
the previous ESP's verdict. That is the "reset one side of a pair" shape.
- Previous ESP compatible: the state matches a fresh boot (false, 0). Benign.
- Previous ESP incompatible: the state becomes (true, 0), which a fresh boot never produces. That is the conservative direction, because degraded only blocks things, for example `link_task_heat_is_safe_for_tc_type_change()` at `link_task.c:1774`. So it is not a safety loosening.

Fix: either document at the call site that degraded is deliberately kept
(conservative), or bind it to the announcing boot_id together with the version
under the HIGH-1 fix. Do not reset it to false, because that would loosen.

## LOW-2: confirm_commit_landed text says "link silence"; the trigger is PUSH_CONTEXT silence

Where: `firmware/KilnFW/App/drivers/safety/safety_cfg_write.c:291-292`.

The staging reset fires on a gap of at least `LINK_TASK_CONTEXT_MAX_AGE_MS`
(5000 ms) between PUSH_CONTEXT frames. Other link traffic, such as GET_STATUS,
does not stop it. The text also appears on every read-back mismatch, including
ones with other causes, so "staged edits are discarded ..." reads as the
diagnosis rather than one possible cause. Suggested wording: "(possible cause:
staged edits are discarded after 5 s without PUSH_CONTEXT or on an ESP reboot
before the commit)". Cosmetic.

## Items checked with no finding

- Item 4: `UPDATE_METADATA_MAX_RECORD` now lives in `update_task_metadata_write.h` (included by `update_task.c:112`). `_Static_assert(BOOTLOADER_METADATA_RECORD_LEN <= UPDATE_METADATA_MAX_RECORD)` at `update_task.c:518` holds (256 <= 256). The runtime `record_len > sizeof(buf)` refusal in `update_task_metadata_write.c` is unchanged. Correct.
- Call-site ordering apart from HIGH-1: the reset runs before `s_last_context_boot_id`/`s_context_boot_id_known` are updated and before `s_last_context_rx_tick` is refreshed. That is the order `context_gap` and the session test need. `staged_before` is read before the reset, so the discard log count is right. The first context after a Pico boot (`prev_known=false`) resets nothing. That is correct, because `link_task_start()` already zeroed the version.
- A same-boot_id context gap keeps the version. Correct, because the same image is still talking.
- Guard accumulators are still untouched by a boot_id change. That matches the existing rationale comment at `link_task.c:1339-1365`.
