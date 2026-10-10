# Safety link review (ESP32-S3 <-> RP2040), 2026-10-09

This is a findings-only review of the kilnlink safety link (protocol v17) on origin/dev at `0dd056c6`. Nothing was changed.

## Scope

- **CommonFW link code.**
- **KilnFW safety link:**
  - `safety_link_*`: frames, inbox, commands and poll.
  - `heat_enable.c`.
  - `safety_cfg_write.c` and `safety_cfg_store.c`.
  - The step-13 flash fallback in `kiln_cfg_swap.c`.
- **SaftyFW:**
  - `link_task.c` and `link_frame.c`.
  - `link_staging.c`.
  - The trip latch in `safety_core.c`.
  - `relay_owner.c`.
  - `config_store_flash.c`.

### Prior reviews not repeated

These four reviews are already covered and their findings are not repeated here:

- `DEV_PICO_ANNOUNCE_REVIEW_2026-10-09.md`
- `DEV_HOSTRULE_LINKCOV_REVIEW_2026-10-09.md`
- `DEV_LINKCOV_PROFSEQ_REVIEW_2026-10-09.md`
- `KILNLINK_ROBUSTNESS_AUDIT_2026-10-09.md`. Its M1-M4 and L1-L3 are fixed, and its "checked and clean" list stands.

Every finding below was confirmed by reading both ends of the exchange.

## Findings

### F1 (MED): the heat grant is sent once; a Pico reboot, a lost frame or a Pico refusal loses it silently

**ESP side**

- `send_enable()` (`heat_enable.c:270-277`) sets `s_he.granted = true` as soon as `safety_link_request_enable()` returns `ESP_OK`.
- `REQUEST_ENABLE` is a fire-and-forget BROADCAST: there is no ACK and no reply (`safety_link.c:700`, and `heat_enable.c:24-36` explains this).
- So `ESP_OK` means only that the local UART accepted the bytes while `link_up` was set.
- `heat_enable_reconcile()` retries only when `pending && held_mask && !granted` (`heat_enable.c:674`).
- `heat_enable.h:240-247` states this on purpose: "a granted request is sent once per run, not once per tick".
- Nothing on the ESP ever compares `granted` with the Pico's reported K4 state (`SAFETY_FLAG_RELAY`). Its only consumers are the dashboard, the CT sweep, danger mode and the CT auto-zero precheck.
- `safety_note_pico_reboot_locked()` (`safety_link_frames.c:270-293`) resets the trip, DIAG, version and relay-state bookkeeping. It does not touch `heat_enable`.

**Pico side**

- `relay_owner_task` (`relay_owner.c:89-108`) applies ENERGIZE only while ARMED.
- In GRACE the command is "accepted/tracked but never actually energizes". GPIO6 is driven low and the request is not remembered.
- The GRACE to ARMED transition (`relay_owner.c:149-153`) re-applies nothing.
- `safety_core_request_enable()` (`safety_core.c:1821-1890`) also returns false for an ON request in four cases: during a Pico update transfer, during a tc_type apply, while `safety_tc_installed == 0`, or while uncommissioned. The ESP never sees that false.

**Ways the grant is lost while the ESP still holds `granted = true`**

1. The Pico reboots mid-firing without latching a trip (a watchdog reset or a brownout). It comes back in a fresh 60 s GRACE (`SAFTYFW_STARTUP_GRACE_MS`), with K4 open and no grant. The ESP's executor watchdog faults the run only on a DIAG TRIPPED state or 30 s of link silence (`profile_executor.c:2144-2154`), and a quick reboot produces neither.
2. A firing or autotune is started within 60 s of a Pico boot, for example straight after a dual reflash or a `POST /api/sw_reset`. The start path does not check for GRACE (no `DIAG_STATE_GRACE` use under `drivers/control`), so the one `REQUEST_ENABLE` lands in GRACE and is dropped.
3. The single broadcast frame is corrupted on the wire (a CRC drop). It is not repeated, unlike `TRIP_EVENT` (`LINK_TRIP_REPEAT_COUNT`) and the rollback burst.
4. The Pico refuses for any of the transient reasons above, such as a tc_type apply that happens to be in progress.

**Consequence**

- The run keeps advancing its schedule and zone relays keep switching, but K4 stays open, so no element current flows.
- The home page's "heat enabled" indicator (`ui_page_home_actions.c:603`, through `heat_enable_is_granted()`) keeps reading true.
- This fails in the safe direction (no heat). It is still the "run looks normal and heats nothing" shape that `heat_enable.h:249-251` and the executor's all-OFF refusal exist to prevent.
- Any detection is indirect, through a no-temperature-rise guard if one fires.

**Fix directions** (not implemented)

- Treat a grant as confirmed only once `SAFETY_FLAG_RELAY` reads energized within a bounded window after an ARMED DIAG. Otherwise fall back to `pending` so reconcile retries.
- Or re-send `REQUEST_ENABLE(true)` on a Pico `boot_id` change and on a GRACE to ARMED transition seen in DIAG while a claim is held.

### F2 (LOW-MED): an ESP reboot mid-firing leaves K4 energized with no owner

**Pico side**

- The Pico's grant is RAM state in `relay_owner`. Nothing de-energizes it when the ESP session changes.
- `link_task_handle_push_context()` (`link_task.c:1321-1420`) runs `link_staging_apply_context_session()` on a boot_id change or a gap of at least 5000 ms. That call drops staging and the peer version only.
- `CONTEXT_FLAG_HEAT_OWNER_ACTIVE` is read only by the tc_type heat-safe gate (`link_task.c:1807`). It is never used to drop K4.
- The only Pico-side `relay_owner_command_energize(false)` outside a trip is the unconfigured-ARMED backstop (`safety_core.c:1166`).
- S6b (link dead) trips only after 120 s (`safety_guards.c:27`). An ESP reboot is much faster than that.

**ESP side**

- A fresh boot starts with `held_mask = 0` and `granted = false` (`heat_enable_init()`, `heat_enable.c:158-170`).
- `REQUEST_ENABLE(false)` is sent only when a claimant lets go (`heat_enable.c:600`), on orphan compensation (`:336`), or when danger mode exits (`danger_mode.c:285`, `:333`).
- No boot-time release exists, so the K4 closed by the previous ESP boot stays closed while the ESP idles.

**Consequence**

- The zone relays are dropped on the ESP reboot, so no current flows.
- What is lost is the second, independent interlock pole: K4 remains closed with no heat owner on either processor until the next firing's release. A later fault that closed a zone relay would have only one pole to stop it.
- This is not a reset-one-side bug in the classic sense, because the ESP correctly forgot its claim. It is the same shape, though: the grant has an owner on one processor and storage on the other, and only the owner's lifecycle event ends it.

**Fix directions:** send one `REQUEST_ENABLE(false)` when the ESP link comes up, before any claim, or have the Pico drop the grant when a fresh context arrives with `HEAT_OWNER_ACTIVE` clear.

### F3 (LOW-MED): a refused persistent commit can be read back as landed

**ESP side**

- `confirm_commit_landed()` (`safety_cfg_write.c:178-299`) decides that a COMMIT_CONFIG took effect by refetching the Pico's config pages and comparing each submitted value.
- **Exception:** a `COMMIT_CONFIG_REJECTED` frame was seen in the reply window, or found in the stash.

**Pico side**

- `GET_CONFIG_PAGE` and `GET_PARAM` serve `config_store_get_full_record()` (`config_store_flash.c:1018`). That is the RAM record, which includes an unpersisted volatile install.
- `config_store_write_ex()` (`config_store_flash.c:1397+`) refuses a persistent write while ARMED. On refusal it leaves that RAM record alone.
- So when the submitted values equal an earlier volatile install, the read-back matches whether or not flash was written.

**Concrete path**

- `persist_pico_flash_fallback()` (`kiln_cfg_swap.c:651-676`, swap step 13) re-commits the same `target_pico` values that the swap has just volatile-installed. Its own comment says this "is expected to fail on essentially every armed board".
- If the `COMMIT_CONFIG_REJECTED` frame is lost, or the ceiling-reconcile path consumes the stash first (the code admits this at `safety_cfg_write.c:195-205`), the read-back passes.
- The swap then logs "landed on Pico flash -- will survive a Pico reboot". The next Pico reboot reverts to the old flash values.

**Notes**

- The Pico already exposes the missing fact: `config_store_is_volatile_dirty()` is reported as DIAG flags bit 7. `confirm_commit_landed()` never consults it.
- Impact is limited to a false log and operator belief. NVS and the ESP side are unaffected, and the reverted values are the previously persisted ones, which `abs_max same or looser` also covers.

**Fix directions:** for a persistent commit, also require DIAG `volatile_dirty == 0` after the refetch, or have the read-back serve `s_persisted_record`.

### F4 (LOW): the 256th trip in one Pico boot is never announced

- `s_trip_seq` is a `uint8_t` that wraps (`safety_core.c:566-573`; the comment calls this an "acceptable, undocumented edge").
- At the 256th trip it reads 0, `safety_core_get_trip_event()` returns false, and `link_task_poll_trip_event()` (`link_task.c:3297`) sends no `TRIP_EVENT` burst at all.
- So the event is suppressed, not just given a duplicate number.
- The ESP still sees TRIPPED through DIAG and STATUS, and a bound clear with seq 0 still matches. Only the forensic trip event is lost.
- This needs 255 trip-and-clear cycles in one boot, so it is informational.

### F5 (LOW, informational): trip-event fields are published without a barrier

- `safety_core.c` latches the trip fields (reason, uptime, tc, threshold) and then `s_trip_seq++` as plain non-volatile statics, with no `__dmb()`.
- `link_task` reads them lock-free, and on the RP2040 it can run on the other core (`SAFTYFW_CORE_LINK_PATH`).
- The documented "seq is written last, so never torn" ordering is therefore something the compiler may legally reorder, and it holds only by codegen accident.
- The consequence would be one `TRIP_EVENT` with a stale reason or temperature, corrected by the next DIAG. Marking the fields `volatile`, or adding a release/acquire pair, would make the ordering real.

### F6 (LOW): CLEAR_TRIP binding carries no boot identity

- The bound token is `0x100 | seq` (`link_frame.c:263+`, `safety_guards.c:283`).
- The ESP forgets its cached `diag_trip_seq` on a Pico `boot_id` change (`safety_link_frames.c:275-281`, robustness L1).
- The Pico itself, however, accepts any bound clear whose seq equals the current trip's seq.
- A delayed or replayed clear from a previous Pico boot with the same seq (both boots number their first trip 1) and the same mask can therefore clear the new trip, provided the condition has gone away.
- An unbound clear is also accepted while the Pico's peer version is unknown, which happens right after a new ESP `boot_id` (`link_frame_decide_clear_trip` applies `SEQ_REQUIRED` only to a peer at version 17 or later).
- Both need a stale frame on a wired point-to-point link, so this is LOW.

## Checked, no issue found

- **Trip delivery:** the `TRIP_EVENT` burst repeats `LINK_TRIP_REPEAT_COUNT` (4) times. The ESP dedups through `safety_trip_decision.c`. DIAG TRIPPED is an independent path to the executor fault (`profile_executor.c:2153`). A lost event therefore delays only the log, never the relay cut, which is local to the Pico (`relay_owner.c` `RELAY_OWNER_CMD_TRIP`, de-energized before latching).
- **Relay cut cannot be lost to a queue:** `relay_owner` command sends use a 0-tick queue send, backed by the `s_trip_command_owed` and `s_clear_command_owed` retry latches. A trip always wins over a clear.
- **Clear drain:** the clear is re-checked against the current occurrence under `safety_guards_clear_trip_occurrence_matches()`. While the condition holds it is refused in `safety_core`. On the ESP side, `safety_link_send_clear_trip()` refuses when its DIAG is stale or not TRIPPED.
- **Staging across ESP sessions:** staged `SET_PARAM` edits are dropped on a boot_id change or a context gap of at least 5000 ms (`link_staging_apply_context_session()`). COMMIT re-validates the whole record and writes nothing on refusal.
- **Bounded waits:** the `heat_enable` flush is bounded at about 5.2 s (`xact_lock` 5000 ms plus 200 ms of polling). `safety_cfg_store_refetch()` takes its lock with `portMAX_DELAY`, but every holder is bounded by its own 2 s budget, and a nonblocking variant exists for the reconcile path. No unbounded wait was found on either side of the link.
- **Pico peer reset on its own reboot:** `link_task_start()` resets `s_peer_protocol_version`, `msg_index`, RX assembly, the context and the ceiling. It draws a fresh `boot_id` from `get_rand_32`.
- **Pico state on an ESP reboot:** guard accumulators are deliberately not reset, which is the safe direction. Context expires after 5000 ms.
- **Danger mode and firing exclusion:** `danger_mode` refuses to start while a firing is RUNNING or PAUSED (`danger_mode.c:54-68`), so its unconditional `REQUEST_ENABLE(false)` on exit cannot cancel a firing's grant.
- **Orphaned and reordered grant:** `send_enable()` undoes a grant that lands after its last claimant let go, and queues a failed compensating release. `he_flush_release_blocking()` prevents an enable from overtaking an in-flight release.

## Tests

None run; this is a review only.
