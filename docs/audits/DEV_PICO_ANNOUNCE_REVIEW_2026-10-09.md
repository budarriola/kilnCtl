# Review: Pico ANNOUNCE state struct and bounded ESP re-announce (2026-10-09)

Reviewer: opus. Base: origin/dev `a4240461`. Commits reviewed:

- `e6a0ff34` (SaftyFW): `link_peer_announce_t {known, boot_id, version}`, `link_peer_announce_record/clear`,
  `link_staging_apply_context_session()` takes the struct; `link_task.c` `s_peer_announce`.
- `e0a038d6` (KilnFW): bounded DIAG-driven re-announce, `safety_diag_reannounce_consider_locked()`,
  `SAFETY_DIAG_REANNOUNCE_MAX 3`, `SAFETY_DIAG_REANNOUNCE_GAP_MS 2000`.

No code was changed by this review.

## Verdict

Both commits are correct for what they claim. No HIGH findings. `e6a0ff34` is a faithful refactor: every
reader and resetter of the old `s_peer_protocol_version` / `s_peer_version_boot_id(_known)` now goes through
`s_peer_announce` (status v2/v3 gates `link_task.c:893,900`, DIAG `has_trip_seq` `:1199`, push_context `:1389`,
ANNOUNCE record `:1461`, clear-trip decision `:1591`, rollback result `:2252`, boot clear `:3420`). No old
identifier remains in source. Every access is on `link_task` itself, so dropping `volatile` is safe.
One MED finding is a gap that predates this refactor and that the refactor keeps.

## Findings

### MED-1: a stale ESP version survives the Pico's first PUSH_CONTEXT (pre-existing, kept by e6a0ff34)

`firmware/SaftyFW/src/tasks/link_staging.c:130-136,166`. `link_staging_new_esp_session()` returns false when
`prev_known` is false (the first PUSH_CONTEXT after a Pico boot). `link_staging_apply_context_session()` then
returns before its `peer->boot_id != boot_id` check. So a version announced under a different ESP boot_id
is kept.

Failure scenario:
1. The Pico boots. ESP boot A (boot_id X, protocol 17) sends its ANNOUNCE, and the Pico records {known, X, 17}.
2. The ESP reboots, for example by rolling back to a protocol 16 image with boot_id Y, before its first
   PUSH_CONTEXT reaches the Pico. The new image's ANNOUNCE burst (4 frames, 250 ms apart) is lost.
3. The Pico gets its first PUSH_CONTEXT with boot_id Y. Because `prev_known` is false, nothing is reset, and
   from then on `prev_boot_id` is Y, so no later context resets the version either.
4. The Pico keeps treating this ESP as protocol 17. It sends 31-byte DIAGs, which a protocol 16 ESP drops as
   frame errors because it accepts only the exact 30-byte length. It also refuses the ESP's unbound 3-byte
   CLEAR_TRIP with `SEQ_REQUIRED` (`link_frame.c:279`).
5. The trip cannot be cleared from the ESP until either processor reboots.

This needs two unlikely events together: an ESP reboot inside the gap between the ANNOUNCE and the first
PUSH_CONTEXT, and a fully lost burst. The consequence is a trip-clear lockout, though, and it is a path where
a previous ESP boot's version is honoured. The test `test_apply_context_session` asserts "first context keeps
the version" (`test/test_link_staging.c:292-295`) only for an unannounced version (`known=false`), so it does
not cover this case.

Suggested fix: on the first context (`!prev_known`), also zero `peer->version` when
`peer->known && peer->boot_id != boot_id`. This is independent of the staging reset.

### LOW-1: the boot-clear branch re-announces without a bound and bypasses the new budget

`firmware/KilnFW/App/drivers/safety/safety_link_frames.c:1143-1150`. During the 30 s boot-clean window
(`SAFETY_LINK_BOOT_CLEAN_WINDOW_MS`, `:1002`), if a stale S6a is latched, `fault_sources == 0`, the peer is
protocol 17 or later, and the DIAG is 30 bytes, every DIAG sets `reannounce_pending`. Pico DIAGs come every
2000 ms (`link_task.c:211`), so this is at most about 15 bursts. Each burst is 4 frames, and
`safety_link_send_announce_version_burst()` spends about 750 ms in `vTaskDelay` on `safety_poll_task`.

- **UART load:** not a flood. It is about 2 frames per second, and the 30 s deadline bounds it.
- **Poll task:** it runs at roughly 40-50% duty inside announce bursts for up to 30 s. During that time it
  services `boot_clear_pending`, deferred fault deasserts and heat-enable releases late.
- **Redundancy:** this branch's condition is a subset of what `safety_diag_reannounce_consider_locked()` (`:1084`)
  already evaluates on the same DIAG.
- **Budget bypass:** the branch also ignores the new budget, so in this window the bound does not hold.

It could be dropped in favour of the bounded helper. Alternatively, keep it as a deliberate window-limited
override for the boot-clear case and say so in a comment, because today the two mechanisms do not reference
each other.

### LOW-2: once the budget is spent, nothing recovers until a reboot or link-down (accepted degradation, undocumented)

`safety_link_frames.c:315-317`. The three re-announces fire about 0 s, 2-4 s and 4-8 s after the first
30-byte DIAG. The GAP equals the DIAG period, so with jitter a DIAG at 1999 ms is skipped and the next one at
about 4 s takes the slot. All three can be lost if the ESP-to-Pico direction is impaired while Pico-to-ESP
frames still arrive. ESP link-down is driven by Pico frames, so it never fires in that case. After that the
budget only returns on a Pico reboot, an ESP reboot, or a link-down.

If that happens, the Pico stays at version 0 for the rest of its boot. Degradation:
- **Trip clear still works.** A version 0 Pico accepts the unbound clear (`link_frame.c:279`), and the ESP
  sends the unbound form whenever `diag_trip_seq_known` is false (`safety_link_commands.c:122,146`). There is
  no lockout.
- **The M4 race protection is lost.** The trip_seq binding that stops a stale clear from releasing a newer
  trip of the same mask is not used.
- **Status and rollback reporting are reduced.** STATUS falls back to v1, so the v2/v3 fields (tx_dropped,
  flag2 such as TC_CONFIG_REASSERTED) are missing, and ROLLBACK_RESULT stays silent.
- **The LOW-2 boot-clear gate (`:1126-1128`) keeps blocking,** but only inside the 30 s boot window, where
  LOW-1's unbounded branch is still re-announcing.

This is acceptable, but no log line says "re-announce budget exhausted, Pico still on 30-byte DIAG", so the
degraded state is invisible. Suggestion: one `ESP_LOGW` when the count reaches MAX, and/or a stats counter.

### LOW-3: the uptime-regression reboot path spends one slot at the instant of the reboot

`safety_link_frames.c:1064` calls `safety_note_pico_reboot_locked()`, which sets count to 0 and
`reannounce_pending`. Then `:1084` sees the new boot's 30-byte DIAG and takes slot 1 at the same moment, so
the two collapse into one burst. Likewise, the first DIAG after ESP boot can be one the Pico packed before it
processed the boot burst, which spends a slot on a race rather than a loss. Either way about two useful
retries remain, not three. This is harmless, but the "3" in the commit text overstates the margin.

### LOW-4: a stale Pico version after an uptime-detected reboot (informational)

`safety_note_pico_reboot_locked()` does not clear `peer_version_known`. A Pico that reboots into a protocol 16
image (detected by DIAG uptime, same boot_id) is treated as protocol 17 or later until its FW_VERSION arrives.
That costs up to 3 harmless extra ANNOUNCEs (a protocol 16 Pico understands ANNOUNCE), and the boot-clear
LOW-2 gate stays blocked meanwhile. No unsafe effect.

### LOW-5: an 8-bit boot_id collision keeps the version (pre-existing, informational)

`link_staging.c:166`. An ESP reboot that draws the same 8-bit boot_id (1/256) and whose ANNOUNCE burst is lost
keeps the old version, because the gap triggers a new session but `boot_id == prev_boot_id`. The consequences
are the same as MED-1. It is inherent to an 8-bit boot_id and not introduced here.

## Items checked and found correct

- **Version zeroing:** `version` is zeroed only on an ESP boot_id change where the recorded announce is for a
  different boot_id or is unknown (`link_staging.c:166`). A context gap with the same boot_id keeps it. An
  ANNOUNCE for the new boot_id that arrives before its PUSH_CONTEXT is kept (the test
  "announce B then context B"). `link_task_start()` clears all three fields.
- **Counter resets (reset-one-side class, ESP half):** the counter resets on a 31-byte DIAG (`:308-310`, which
  runs after `diag_trip_seq_known` is updated at `:1081`), on a Pico boot_id change or uptime regression
  (`:283-284` via `safety_note_pico_reboot_locked`), and on link-down (`safety_link.c:316-317`). An ESP reboot
  zero-initialises it.
- **Protocol 16 Pico:** the `peer_protocol_version < 17u` guard means a protocol 16 Pico is never misdetected,
  apart from the stale case in LOW-4.
- **Time wrap:** `now_ms` is `tick * portTICK_PERIOD_MS` truncated to 32 bits, and the difference is computed
  unsigned (`:319`), so it is wrap-safe mod 2^32 even if the product overflows. `last_ms` is only compared
  when `count != 0`.
- **Locking:** the helper is called inside `safety_apply_diag()`'s `state_lock` section (`:1051`..`unlock`),
  and every other writer (`safety_note_pico_reboot_locked`, `safety_reset_stale_peer_info_if_link_down`) holds
  the same lock. The send is deferred to `safety_poll_task` through `reannounce_pending`, never made inline.

## Tests run

- `firmware\KilnFW\App\test\build_host_tests.ps1 -Only safety_link`: built 2/2 executables, all passed. The
  safety_link executable passed 631/631 checks, including "residual -- 30-byte DIAGs from a >= 17 Pico...".
- `firmware\SaftyFW\test\build_host_tests.ps1`: all host tests passed (the last executable reports 72/72).
- `tools\negtest.ps1`, using `-Command` with build_host_tests `-Only "kilnctl_host_tests_safety_link\.exe"`
  rather than `-Preset kilnfw-host`, so that only this executable is rebuilt. Each mutation has its own
  `FAIL .*test_safety_link_compile\.c:\d+: <message>` pattern. The baseline passed. Verdict: ALL_CAUGHT.
  - Gap check removed (`< SAFETY_DIAG_REANNOUNCE_GAP_MS` changed to `< 0u`): **CAUGHT**, `:3002`
    "DIAGs inside the gap owe a single re-announce".
  - Reboot-time counter reset removed (`safety_note_pico_reboot_locked`): **CAUGHT**, `:2992`
    "Pico boot_id change resets the counter".
  - Link-down counter reset removed (`safety_link.c`): **CAUGHT**, `:3020` "link-down clears the counter".
- The reboot-reset test drives the boot_id-change entry (`:383`). The uptime-regression entry (`:1064`) calls
  the same function, so it is not separately mutation-tested.
