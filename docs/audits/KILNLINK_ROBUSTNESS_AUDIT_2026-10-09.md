# kilnlink robustness audit (2026-10-09)

Scope: the ESP32-S3 <-> RP2040 kilnlink UART link, both ends, audited at
origin/dev `48e1ba8a`. Audit only; no code was changed.

- CommonFW: `kilnlink_frame.c` (framing, SLIP-style stuffing, CRC) and the
  per-command codecs under `firmware/CommonFW/src/kilnlink_*.c`.
- Pico: `firmware/SaftyFW/src/tasks/link_task.c` (RX assembly, dispatch, every
  handler), `link_frame.c` (pure decisions), and the downstream
  `config_params.c`, `config_store*.c`, `ct_amps_cal.c`, `current_sense.c`,
  `safety_core.c` and `update_task.c` paths a link command reaches.
- ESP: `firmware/KilnFW/App/drivers/safety/safety_link*.c` (inbox drain,
  FW_VERSION/DIAG/TRIP_EVENT/PARAM/CONFIG_PAGE parsers, command senders, the
  link-down peer-info reset).

Questions asked: is length and CRC validated before any field is read; are
out-of-range opcodes, param ids and indexes refused; does sequence and dedup
state survive a reboot of either side correctly (the reset-one-side class,
CLAUDE.md lists four past instances); trip_seq and boot_id handling;
GET_PARAM/SET_PARAM bounds; what a corrupted or replayed frame can make the
Pico do (it must never weaken a guard or clear a trip); and whether a
malformed frame can starve the heartbeat or the trip watchdog.

Threat model note: kilnlink has no authentication. CRC16/CCITT-FALSE is the
only integrity check, so random line corruption passes undetected about 1 in
65536 frames, and a replayed or duplicated frame is indistinguishable from a
fresh command. The findings below are about what the protocol and the
handlers do with such frames, and about state that drifts out of step.

Frame notation: `7E | type msg_index(BE,2) src_dev src_task dst_dev dst_task
len | payload | crc16(BE,2) | 7E`, with stuffing omitted. The Pico acts only on
`type = 04` (BROADCAST). `..` marks bytes the handler does not inspect.

## Summary

| Severity | Count |
|----------|-------|
| H | 0 |
| M | 4 |
| L | 3 |

No finding lets a single malformed (CRC-failing, wrong-length or
unknown-opcode) frame weaken a guard, clear a trip, or stall the heartbeat.
Every finding needs either a CRC-valid frame that is replayed, duplicated or
stale, or a legitimate command sequence whose state goes out of step.

## Findings

### M1. Pico boot_id is an 8-bit time sample, yet the ESP relies on it for reboot detection (reset-one-side) -- FIXED in bcc75d61

- Fix: `boot_id` now folds `get_rand_32()` (ROSC, unique id and time), so
  consecutive boots no longer repeat it by construction. The 8-bit id still
  collides 1 in 256, so the ESP also treats a backwards step in the Pico's
  DIAG `uptime_ms` (wrap-safe, `safety_pico_uptime_regressed()`) as a reboot.
  Both signals run the same bookkeeping in one helper,
  `safety_note_pico_reboot_locked()`. Tests: `test_safety_link_compile.c`
  (same-boot_id reboot, rising uptime, wrap, baseline reset).

- Pico: `firmware/SaftyFW/src/tasks/link_task.c:3292`,
  `s_boot_id = (uint8_t)(time_us_64() ^ (time_us_64() >> 8));`. It is set in
  `link_task_start()`, before the scheduler, at a near-fixed point of a
  deterministic boot. The comment at lines 3286-3291 says it is "diagnostic
  identity only ... not a security or safety value, so true entropy is not
  required."
- ESP: `firmware/KilnFW/App/drivers/safety/safety_link_frames.c:270-313`
  relies on it in exactly the role that comment rules out. A boot_id change
  is the only Pico-reboot signal that resets `cached.trip_last_seq` (line
  301), resets `safety_relay_state_known`, and sets `reannounce_pending`
  (line 313). `safety_link.c:276-311` clears `pico_boot_id_known` only once
  the link reads down, and the link-up window is
  `SAFETY_LINK_UP_PERIODS * 500 ms = 1500 ms` (`safety_link.h:450`).
- Trigger: a Pico watchdog reset caused by a task other than link_task. The
  link task keeps sending STATUS until the 1000 ms watchdog
  (`main.c:57`) fires. The board reboots and resumes STATUS inside the
  1500 ms window, so the link never reads down. If the new boot draws the
  same 8-bit value, the ESP's boot_id test at line 279 sees no change. That
  chance is 1 in 256 even if the value were uniform, and it is likely worse
  for a fixed-point time sample.
  Concrete sequence: FW_VERSION reply `7E 04 .. .. .. .. .. .. len | 02 ..
  boot_id=0x5A ..` before the reset and the same `boot_id=0x5A` after it.
- Effect:
  - (a) The rebooted Pico restarts trip_seq. Its first TRIP_EVENT can equal
    the ESP's stale `trip_last_seq`, and the ESP then drops it as a dedup
    resend. The TRIPPED log line and its fault_sources snapshot are lost,
    although DIAG still shows the latched state.
  - (b) The relay-state edge baseline is kept across the reboot.
  - (c) The ESP never re-announces, so the Pico keeps
    `s_peer_protocol_version = 0` (line 3294). It then sends STATUS V1 only
    (no flags2, cj_valid or active-slot) and never sends ROLLBACK_RESULT.
- This is the reset-one-side class: the Pico-side reset of the pair is
  reliable, but the ESP-side reset depends on a value the Pico documents as
  non-load-bearing.
- Host tests: `test_safety_link_compile.c` covers the boot_id_changed branch
  when the id does differ. Nothing covers an equal-id reboot, and no ESP-side
  secondary signal exists (for example `diag_uptime_ms` going backwards at
  `safety_link_frames.c:981`).
- Suggested fix: draw boot_id from the RP2040 ROSC / `get_rand_32()`, and/or
  have the ESP also treat a `diag_uptime_ms` regression as a reboot.

### M2. COMMIT_CONFIG / APPLY_CONFIG_VOLATILE silently revert an earlier SET_CONFIG or SET_CT_CAL (two copies of one record) -- FIXED in 0e04c0a9

- Fix: staging is now a list of SET_PARAM edits (`link_staging.c`), not a
  record. COMMIT/APPLY build the candidate at commit time from the
  currently enforced record plus those edits, so an unstaged field always
  keeps its committed value. A successful SET_CONFIG/SET_CT_CAL also drops
  earlier staged edits of the fields it changed or targeted, so the later
  write wins. Tests: `test_link_staging.c`.

- `link_task.c:488-498`: `s_staged_config` is seeded from the committed
  record once per Pico boot (`s_staged_config_init`, reset only at line
  3325). It is re-baselined only by a successful COMMIT_CONFIG (line 2606)
  or APPLY_CONFIG_VOLATILE (line 2757).
- `link_task_handle_set_config()` (line 1816) and
  `link_task_handle_set_ct_cal()` (line 1929) write `config_store` directly
  from `config_store_get_full_record()` and never touch `s_staged_config`.
- Trigger, all frames CRC-valid and legitimate:
  1. `SET_PARAM` (`1C ...`) for any field. This seeds staging with
     tc_type = K.
  2. `SET_CONFIG` payload `16 03` (tc_type = 3) from the PC bridge path
     (`uart_bridge_safety.c:175`). It is written to flash and reapplied to
     the chip.
  3. Any later `COMMIT_CONFIG` payload `1D` (the commissioning page saving
     one field). The staged record still carries the old tc_type,
     `config_params_validate_ex()` accepts it, and `config_store_write_ex()`
     writes it while heat is safe. tc_type silently reverts and the chip is
     reapplied to the old type.
  
  SET_CT_CAL (`19 ch cal gain offset`) is reverted the same way, through the
  ct_cal gain/offset/calibrated fields.
- Safety relevance: a reverted tc_type makes the Pico linearize its
  thermocouple with the wrong curve. S1 then compares a wrong temperature
  against `abs_max_temp_c`, which can be a guard-weakening drift that nobody
  asked for. The commit is reported as accepted, so no COMMIT_CONFIG_REJECTED
  frame is sent. The ESP's post-commit read-back verifies only the params it
  staged, not tc_type.
- Host tests: none. `test_link_task_tc_type_gate.c` covers the heat-safe gate
  only, and no test interleaves SET_CONFIG/SET_CT_CAL with staging.
- Suggested fix: re-seed (or patch) `s_staged_config` after every successful
  direct `config_store_write*()` in link_task, or have both direct writers go
  through the staging path.

### M3. Staged SET_PARAMs outlive the ESP boot that sent them (reset-one-side across processors) -- FIXED in 0e04c0a9

- Fix: `link_task_handle_push_context()` discards staged edits when the
  ESP boot_id changes or PUSH_CONTEXT stopped for at least
  `LINK_TASK_CONTEXT_MAX_AGE_MS` (a same-id reboot, or link loss). The
  guard accumulators are still not reset there. The sweep repair in
  `zones_current_sweep_task.c` still covers the same-session case; its
  comment now says so. Tests: `test_link_staging.c`.

- `link_task.c:488-498` and `:3325`: staging is reset only when the Pico
  boots. The Pico computes an ESP boot_id change in
  `link_task_handle_push_context()` (line 1340) but deliberately discards it
  (`(void)boot_id_changed;`, line 1341). The reasoning there is sound for
  the guard accumulators, but it also means nothing else reacts to an ESP
  reboot, staging included. Link loss does not reset staging either.
- Trigger:
  1. The ESP sends `SET_PARAM` payload `1C 01 01 ..` (for example
     `abs_max_temp_c`, id 0x0101) as part of a commissioning edit.
  2. The ESP reboots, or the HTTP flow is abandoned, before `COMMIT_CONFIG`.
  3. Any later `COMMIT_CONFIG` (`1D`) from the new ESP boot commits the
     abandoned value together with whatever the new session staged.
  
  The value passes range validation, so it can be a looser but in-range
  threshold that the operator never confirmed.
- Known in part: `zones_current_sweep_task.c:512-535` documents the same
  "persistent baseline, no discard command" hazard and repairs its own sweep
  leftovers by re-staging the committed values. No other ESP writer does
  this, and nothing covers the ESP-reboot case.
- Host tests: none on the Pico side. The sweep repair has ESP-side tests
  only.
- Suggested fix: reset `s_staged_config_init` on an observed ESP boot_id
  change (staging is not a guard accumulator, so the "safe direction"
  argument at lines 1326-1339 does not apply to it), or add a
  DISCARD_STAGED command that the ESP sends before each staging session.

### M4. CLEAR_TRIP is not bound to a specific trip occurrence; a duplicated or stale frame can clear a later trip with the same reason -- FIXED in c2b151b5, 5ade853b

- Fix: kilnlink 16 -> 17, compatible (`KILNLINK_MIN_COMPATIBLE` stays 7).
  DIAG gains optional byte30 `trip_seq` (31 bytes), sent only to a peer whose
  ANNOUNCE named >= 17; send_diag reads the seq before the trip state, so a
  tear binds to an older seq and is refused. CLEAR_TRIP gains optional byte3
  `trip_seq` (4 bytes); the ESP echoes it whenever its cached DIAG carried
  one. The Pico refuses a 3-byte clear from a >= 17 peer
  (`LINK_CLEAR_TRIP_REFUSE_SEQ_REQUIRED`), and `safety_core` compares the seq
  with the latched one at dequeue, refusing a mismatch as
  refused-stale-occurrence (outcome 4) without running `try_clear`. Peer
  version 0 (before ANNOUNCE) and 16 keep the 3-byte behaviour, so the
  boot-time S6a clear still works. The PC wire and `safety_clear_trip` are
  unchanged. Tests: CommonFW `test_clear_trip.c`/`test_diag.c`/fuzz and
  vectors, SaftyFW `test_link_frame.c`, `test_safety_guards.c`,
  `test_safety_core_clear_trip_binding.c`, KilnFW
  `test_safety_link_compile.c`, PcTools `test_kilnlink_capture.py`.

- `link_task.c:1511-1580` and `link_frame.c:251-265`: the payload is
  `0A mask_lo mask_hi` (`kilnlink_clear_trip.h:24-25`). The Pico accepts it
  when `mask == 1 << (current_reason - 1)`, refuses NONE and S9, and queues
  it to safety_core (`safety_core_request_clear_trip()`, line 1566).
  safety_core then clears once the guard condition is no longer present.
  There is no trip_seq, nonce or boot binding. The Pico does not dedup
  broadcast frames by msg_index.
- Trigger: `7E 04 xx xx .. .. .. .. 03 0A 20 00 crc crc 7E` (mask 0x0020 =
  S6a). The same applies to `01 00` for S1 (reason 1, mask 0x0001).
  - Delivered twice, or replayed later, it clears a new S6a/S1 trip that
    latched after the first one was cleared, as soon as that trip's
    condition has gone.
  - The ESP side builds the mask from a cached DIAG up to
    `SAFETY_LINK_STALE_MS` old (`safety_link_commands.c` `send_clear_trip`).
    A clear the operator meant for occurrence N can therefore land on
    occurrence N+1 of the same reason.
  
  The ESP's admin-login gate for clearing (owner decision 2026-10-07) is
  invisible to the Pico.
- Bound on harm: safety_core still refuses while the condition persists, so
  this never clears a trip whose cause is still active. It does remove the
  "every latched occurrence needs its own deliberate clear" property.
- Host tests: `SaftyFW/test/test_link_frame.c:369`
  (`test_decide_clear_trip`) and `CommonFW/test/test_clear_trip.c` cover the
  mask decision and codec. Nothing covers duplicate or stale delivery,
  because the protocol cannot express the distinction.
- Suggested fix: add the trip_seq (already in TRIP_EVENT/DIAG) to
  CLEAR_TRIP and refuse a mismatch on the Pico. This is a protocol version
  bump.

### L1. S6b link liveness accepts any CRC-valid frame, including the Pico's own echoed output -- FIXED in 59b0d6af

- Fix: the liveness refresh (and dispatch) in
  `link_task_handle_raw_frame()` now requires `src_device == ESP (0)` and
  `dst_device == SAFETY (2)` via the pure `link_frame_counts_for_liveness()`.
  The Pico's own frames are always the mirror image, so a TX-RX loopback no
  longer keeps S6b quiet. The message type is still not checked, so every
  genuine ESP frame counts. Tests: `test_link_frame.c` (predicate and
  link_task.c wiring).

- `link_task.c:2966-2967`: `s_last_valid_frame_tick` and
  `s_valid_frame_seen` are refreshed for every frame that passes
  unstuff/decode. That happens before the BROADCAST filter (line 2969), with
  no check of `src_device`/`dst_device` and regardless of whether the opcode
  is one the Pico handles. Line 3426 reads it for link-up.
- Trigger: a TX-to-RX loopback (solder bridge, probe clip, or a level-shifter
  fault while the ESP is dead) returns the Pico's own STATUS frame
  `7E 04 .. .. <pico src> .. .. .. 18 01 ... crc 7E`. It decodes, refreshes
  liveness, and is discarded by the switch, which has no 0x01 case. S6b
  ("link dead") never fires although no ESP is talking. The 5000 ms context
  staleness still expires correctly, so context-dependent guards fall back
  safely. Only the link-dead trip is masked.
- Host tests: none for liveness gating.
- Suggested fix: refresh liveness only for frames whose `src_device` is the
  ESP and whose opcode the Pico dispatches.

### L2. SET_CT_CAL / SET_PARAM accept any finite ct_cal gain or offset; gain 0 blinds the S14 over-current WARN -- FIXED in 50d2826b

- Fix: SET_PARAM 0x0310-0x0315 bounds ct_cal gain to [0, 10] and offset to
  |x| <= 50 A (finite). `config_params_validate_ex()` (COMMIT/APPLY) refuses a
  calibrated channel with gain <= 0, naming the field, and SET_CT_CAL refuses
  the same via `config_params_ct_cal_entry_ok()`. Gain 0 stays stageable for
  an uncalibrated channel (kiln_cfg re-push). Load-time
  `config_params_validate_ranges()` is unchanged, so a legacy record is never
  discarded wholesale. Tests: `test_config_store.c`.

- `config_params.c:498-503` (`CHECK_F32_FINITE` only) and `:733-738`
  (`RANGE_F32_FINITE` only), and `link_frame.c:288-299` (no value check).
  `ct_amps_cal.c:28` computes `gain * raw + offset`, clamped at 0.
  `safety_core.c:1266` feeds those corrected amps to S14/S15. Presence
  (S3/S6b/S9/S11) is computed separately from raw counts
  (`current_sense.c:385-389`) and is unaffected.
- Trigger, Pico not ARMED: `SET_CT_CAL` payload
  `19 00 01 00 00 00 00 00 00 00 00` (channel 0, calibrated, gain 0.0f,
  offset 0.0f), or `SET_PARAM` id 0x0310 with f32 0.0 followed by `1D`.
  From then on, corrected amps on channel 0 read 0. S14 (over-current) can
  never fire and S15 (under-current) nuisance-WARNs. A negative gain does
  the same.
- Severity is L because S14/S15 are WARN-only (`CT_COMMISSIONING_PLAN.md`,
  `CURRENT_SENSE.md`), the write is refused while ARMED (`config_store_decide_write_ex`),
  and the input must be a CRC-valid command.
- Host tests: `SaftyFW/test/test_ct_amps_cal.c` covers the calibrated and
  uncalibrated paths. Nothing tests a non-positive gain.
- Suggested fix: refuse `gain <= 0` (and bound `|offset|`) in
  `config_params_set()` and `config_params_validate_ranges()`.

### L3. Flash-writing commands are not idempotent or rate-limited; a stale comment says a no-op write cannot happen -- FIXED in 0795908c

- Fix: after the write decision, `config_store_write_ex()` compares the
  packed candidate (with the persisted seq) against the current slot's
  flash bytes and, when they match, updates RAM and returns OK with no
  erase or program. An identical write is still refused while ARMED. Real
  writes keep the erased-slot check and read-back. The comment is
  corrected. Not rate-limited: a changed record still writes every time.
  Tests: `test_config_store_flash.c` (three `audit L3` cases).

- `config_store_flash.c:1380-1390`: the comment says "a no-op write is not
  on any call path here". Yet a COMMIT_CONFIG with nothing staged (`1D`), a
  repeated `SET_CONFIG 16 <same tc_type>`, or a repeated identical
  `SET_CT_CAL` all reach `config_store_write_ex()` with a record identical to
  the persisted one.
  - While ARMED this is refused, because neither narrow-change comparator
    matches an identical record (`config_store.c:1321-1326`, `:1444`). That
    outcome is safe.
  - While idle, every replay programs a new slot and erases a sector at
    rollover, under `flash_safe_execute()`, which stalls both cores for the
    erase.
- Trigger: a burst of `7E 04 .. 01 1D crc 7E` frames from a misbehaving ESP
  or bridge. Each one costs a flash program, so there is wear and periodic
  multi-tens-of-ms stalls. No guard is weakened, and the 1000 ms watchdog
  margin covers a single 4 KB erase.
- Host tests: `test_config_store.c` covers the write decision but has no
  identical-record case.
- Suggested fix: skip the write (report success) when the candidate record
  equals `s_persisted_record`, and correct the comment.

## Areas checked and found clean

- **Frame decode order** (`kilnlink_frame.c:116-156`): the decoder checks the
  minimum raw length, then `length <= 253`, then the exact
  `header + length + crc` match, then CRC, then the type enum. Fields are
  filled only after all of that. Unstuffing (lines 26-70) requires
  `out_cap >= in_len` and rejects a trailing escape, and stuff/encode check
  capacity. Covered by `CommonFW/test/test_frame.c`, `test_fuzz.c`, and
  `test_fuzz_payloads.c` (ASan runner `run_fuzz_payloads_asan.ps1`).
- **Pico RX assembly** (`link_task.c:3126-3159`): bounded to
  `KILNLINK_FRAME_STUFFED_MAX`, resyncs on 0x7E, and drops an overlong
  frame.
- **Unknown opcodes**: both ends discard them through `default:`. The Pico
  ignores non-BROADCAST types and zero-length payloads (line 2969). The ESP
  counts them into `unmatched_cmd_count` (`safety_link_inbox.c:449-458`).
- **Per-command length gates**: every Pico codec decode is exact-length plus
  cmd-byte. FW_VERSION/GET_CT_CAL/GET_STACK_MARGIN/GET_CT_AUTO_ZERO require
  len 1, GET_PARAM len 3, and GET_CONFIG_PAGE a fixed length. The PUSH_CONTEXT
  unpack (`link_frame.c:190-235`) checks min length, `zone_count <= MAX` and
  exact length before writing `*out`. On the ESP, DIAG, TRIP_EVENT, CT_CAL,
  COMMIT_REJECTED, ROLLBACK_RESULT, REBOOT_RESULT, STACK_MARGIN and
  CT_AUTO_ZERO_STATUS use exact lengths. PARAM is bounded
  `[HDR_LEN, MAX_LEN]` and CONFIG_PAGE is `>= HDR_LEN` with a full decode
  later. Covered by the per-codec `CommonFW/test/test_*.c` files.
- **ESP FW_VERSION parser** (`safety_link_frame.c:57-149`): every
  variable-length tail field is bounds-checked against `len` before it is
  read. commit/datetime copies are truncated to their maxima while the
  cursor advances by the wire length, and `config_crc` needs `i + 1 < len`.
  Covered by `test_safety_link_compile.c:1036`.
- **GET_PARAM / SET_PARAM bounds**: `config_params_set()` refuses an unknown
  param_id or a type mismatch and range-checks per field. COMMIT and
  APPLY_VOLATILE re-run `config_params_validate_ex()` on the whole record and
  write nothing on refusal. GET_PARAM of an unknown id answers `found = 0`
  and never reads outside the table. On the ESP, `safety_link_get_param()`
  checks that the reply's param_id matches the request
  (`safety_link_commands.c:1083-1099`) and ages out stale stashes.
- **GET_CONFIG_PAGE index**: any `page_index` 0-255 is safe. The page walk
  is bounded by `offset < total` and `all[72]` is capped by
  `config_params_count()` (`link_task.c` `send_config_page`).
- **ARMED write refusal**: `config_store_decide_write_ex()` refuses every
  persistent write while ARMED except the narrow tc_type-only (heat-safe
  gated) and single-channel zero_counts/k_ct-only shapes, and
  `config_store_write_volatile()` refuses a loosening while ARMED. So no
  replayed config frame can loosen a threshold mid-firing.
- **REQUEST_ENABLE**: len 2, forwarded to safety_core. relay_owner enforces
  TRIPPED/GRACE/ARMED, and a replay cannot arm past a latched trip.
- **CLEAR_TRIP refusals**: NONE and S9 (INEFFECTIVE) are refused
  unconditionally, a wrong mask is refused, and safety_core refuses while
  the condition persists (but see M4).
- **SET_FIRING_CEILING**: finite and `> 0` or treated as absent. It has been
  inert since 2026-09-24 (S1 uses `abs_max_temp_c` only,
  `safety_core.c:908-929`), so a replay cannot loosen S1.
- **SET_CLOCK**: plausibility window 2020-2100, and no guard reads the
  clock.
- **SET_LOG_LEVEL**: `level > LOG_LEVEL_VERBOSE` is refused.
- **SET_CONFIG tc_type**: `tc_type > MAX31856_TC_TYPE_T` is refused, and the
  heat-safe gate applies (but see M2).
- **INJECT_TC**: refused unless `safety_tc_installed == 0`, so it cannot
  override a real sensor.
- **REBOOT / ROLLBACK / UPDATE_BEGIN**: refused while the relay is energized
  (`update_task.c:1440`, `:1544-1584`). A replayed REBOOT while idle reboots
  the Pico, which is visible and safe. UPDATE_* frames are queued with a
  zero-timeout `xQueueSend` (`update_task.c:1623`), so they never block
  link_task.
- **ANNOUNCE_REBOOT**: it only opens the S6b grace window that already
  exists. No other guard is affected.
- **ESP reboot as seen by the Pico**: guard accumulators are deliberately
  not reset on an ESP boot_id change. That is the safe direction, documented
  at `link_task.c:1326-1341`, and context expires after 5000 ms.
- **ESP view of a Pico reboot**: when boot_id does change,
  `trip_last_seq`, `trip_event_ever_received`, `safety_relay_state_known`
  and `reannounce_pending` are reset together (past instance #4 is still
  fixed). TRIP_EVENT dedup goes through the pure `safety_trip_decision.c`,
  covered by `test_safety_trip_decision.c` (including the first event after
  boot and the 255 to 0 wrap).
- **ESP boot-time S6a auto-clear** (`safety_link_frames.c` DIAG path):
  limited to a 30 s window, 3 attempts 2 s apart, MAIN_FAULT only, with
  `fault_sources == 0`. This is designed and bounded.
- **Pico peer state on its own reboot**: `s_peer_protocol_version`,
  `s_msg_index`, the RX assembly, the context and the ceiling are all reset
  in `link_task_start()` (lines 3292-3325). The ESP's link-down reset clears
  its peer-version, boot_id and build flags.
- **Heartbeat and watchdog starvation by malformed input**: a bad frame costs
  one unstuff plus one CRC over at most 515 bytes. The loop reads at most 64
  bytes per 10 ms iteration (`LINK_RX_POLL_BUF`, line 269), then always runs
  the STATUS/DIAG/POWER schedule and `watchdog_task_checkin`. Sustained
  input above about 6.4 kB/s (below the 23 kB/s line rate) backs up and
  overflows the UART RX buffer and drops bytes. It does not delay the
  heartbeat or the trip-event repeat burst (`poll_trip_event`, lines
  3169-3224). The only blocking handlers are the flash writers (see L3) and
  REBOOT's 10 ms TX drain.
- **ESP stash correlation**: CONFIG_PAGE/PARAM/COMMIT_REJECTED stashes are
  cleared before each request and aged out afterwards, and a PARAM reply is
  matched by id.
