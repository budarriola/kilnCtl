# Shared-state ("reset one side of a pair") review pass — 2026-09-17

RELEASE_HARDENING_PLAN.md section 9. A reading pass, not a mechanical check
(rejected for this class — see that section and CLAUDE.md's own writeup).
Scope: cross-processor state specifically — sequence numbers, dedup cursors,
boot IDs, config revision counters, seqlock generations, and the `cfg`-vs-NVS
revision comparison.

## Enumeration method

Scripted grep over `firmware/` and `tools/PcTools/src/` (and the `mykicadMcp`
submodule, empty for this class) for identifier tokens (`seq`, `cursor`,
`boot_id`, `revision`, `rev_`, `generation`, `seqlock`, `dedup`, `ring_next`,
`last_seq`, `trip_seq`, `msg_index`, `ack_cache`) filtered to lines that also
match a reset/clear/zero/init verb. 84 raw hits, hand-triaged (excluded LVGL
vendor code, test-file literals, and unrelated widget "cursor" hits). Each
surviving candidate was traced to both sides of its relationship and the
standing question ("who else holds a copy or a derived expectation of this?")
was answered explicitly, including the negative case.

## Pair table

| # | State | Side A | Side B | Relationship | Status |
|---|---|---|---|---|---|
| 1 | benchproto/UART dedup ring vs host `msg_index` | firmware dedup ring, boot-lifetime (`benchproto_link.h`) | PC `serial_link.py` `_next_tx_index`, per-session | equality (recent-tuple membership) | **Fixed by mitigation**: `serial_link.py:_random_msg_index()` randomizes the host's starting index every session instead of 0, per its own doc comment citing this exact class. `benchproto_link_reset_device()` also exists as a receiver-side clear but has no caller today (dead code, not wired to any reconnect event) — noted, not a defect: the randomization already closes the practical exposure. |
| 2 | SimFW `apply_reset()` `s_ring_next_seq` vs `telemetry.c` consumer cursor | — | — | monotonic derivation | **Moot** — SimFW and kilnsim were deleted 2026-08-28 (project memory); no such files exist on `origin/main`. |
| 3 | `fault_sched.c` `SET_SEED` vs `fault_engine_t.rng_state` | — | — | shared seed | **Moot** — same deletion; `fault_sched.c`/`fault_engine.c` do not exist on `origin/main`. |
| 4 | SaftyFW `s_trip_seq` (Pico) vs ESP `trip_last_seq` dedup cache | `safety_core.c:1341` increments `s_trip_seq`, restarts at 0 on Pico reboot | `safety_link_frames.c` `link->cached.trip_last_seq` | dedup-key equality across a reboot | **Fixed**, 2026-08-27 audit. `safety_link_frames.c`'s `boot_id_changed` branch (~line 279-306) explicitly clears `trip_event_ever_received`/`trip_last_seq` and `safety_relay_state_known` the moment a Pico boot_id change is observed, with a comment naming this exact bug class. Covered by `test_safety_link_compile.c` (boot_id-change tracking resets) and `test_safety_trip_decision.c`. |
| 5 | `web_auth_store` web administrator password vs LCD administrator PIN, on the physical four-corner reset gesture | `web_auth_store_clear_for_physical_reset()` — web password blob | LCD PIN blob (`WEB_AUTH_KEY_LCD`) | "physical reset recovers a forgotten credential" contract | **Already fixed on origin/main**, commit `1179e2d3` ("item 4a"). Both blobs are now cleared together; `test_web_auth_store.c:401` covers it. Single-processor (ESP-only), not cross-processor, but was in scope of the broader class this pass double-checked. |
| 6 | `ui_lcd_lock.c` policy `enabled` bit vs `s_lock.granted_role` session state | policy `enabled` toggled off then back on | `granted_role` (session grant) | "auth re-enable must not resurrect an old grant" | **Already fixed on origin/main**, commit `1179e2d3` ("item 4b"). The disabled-tick branch now force-locks (clearing the session) every tick while disabled, so a re-enable always finds the panel locked. Comment at `ui_lcd_lock.c` around line 108-124 names this class explicitly. |
| 7 | SaftyFW `peer_config_version`/`peer_config_crc` (ESP cache of Pico's live config identity) | `safety_link_frames.c:324-325`, overwritten from every FW_VERSION-carrying frame | `safety_cfg_store`'s own `cached_config_crc` | "is my cached commissioning data still what the Pico is enforcing" | **Nobody** (correct one-sided design) — `peer_config_crc` is refreshed unconditionally from every inbound frame regardless of boot_id change (it is not a derived counter, it is copied verbatim each time), so a Pico reboot cannot strand it. `safety_cfg_store.h`'s header comment documents that the CRC comparison, not a counter, is the authority, specifically to avoid this class. |
| 8 | ESP `kiln_cfg_store` NVS `rev` vs `cfg` LittleFS-partition `file_rev` | `kiln_cfg_store.c`/`kiln_cfg_store_cfg_fs.c` | same | tie-break on divergence | **Nobody new** — single-processor (both sides live on the ESP), and the tie-break (`kiln_cfg_store_cfg_fs.c:145-240`) already has its own negative-tested check, `check_cfg_fs_tie_break.ps1`, enforcing strict `>` between the two revs. Per plan section 10 this whole path is inert today (the `cfg` partition is unformatted/unmounted on the bench board), so it has never actually run outside host tests. Out of section 9's cross-processor scope strictly speaking, but checked since section 9 names "the `cfg`-versus-NVS revision comparison" explicitly. |
| 9 | SaftyFW `config_store` record `seq` (flash-persisted) vs anything ESP-side | `config_store.c`/`config_store_flash.c`, `seq == 0` sentinel for "never committed" | — | — | **Nobody** — this `seq` is a Pico-local flash-slot generation number (for its own A/B wear-leveled record selection), never mirrored or compared against any ESP-side counter. The ESP receives commissioning *values*, not this `seq`, over the link (see #7). |
| 10 | SaftyFW `link_task.c` `s_msg_index` (Pico's own outbound frame counter), reset in the Pico's own boot/reinit block (line ~3089) | Pico-local | ESP-side dedup of Pico frames | link-frame sequencing | **Nobody** — the ESP does not maintain a dedup ring keyed on this counter (unlike `trip_seq`); it is a framing sequence number only, not compared across reboots by the peer. |
| 11 | `safety_link_rollback_boot_id_changed()` / rollback-detection boot_id evidence | ESP-cached `pico_boot_id`/`peer_build_*` | Pico's actual boot_id/build identity | "is this a rollback or an ordinary reboot" | **Nobody, deliberately** — already reviewed (Opus review finding 2, "a reboot is not a rollback"); both helper functions explicitly refuse to fabricate evidence when either side's baseline is unknown, rather than resetting one side and guessing. No pairing hazard: the code declines to answer rather than answering wrong. |

## Findings requiring action

None. All four originally-confirmed instances (#1-4) are either fixed and
tested, or moot because the code they lived in (SimFW/kilnsim, `fault_sched.c`)
no longer exists on `origin/main`. The two web-auth instances found later
(#5, #6) are already fixed on `origin/main` (`1179e2d3`) with test coverage.
The additional cross-processor candidates surfaced by this pass's own
enumeration (#7-11) are all correct one-sided designs, several with their own
prior review comments citing this exact bug class by name — no new pair was
found where one side resets/reinits and the other does not.

## Disposition

No code changes made — no fix was warranted, so none of the "restore by hand
and force a rebuild" or negative-test machinery in the plan's Method section
was exercised. No narrow mirror-drift check was added: the standing rejection
of a general check stands, and this pass found no new *concrete, stable* pair
to pin a narrow check to — every open-looking candidate resolved to an
already-guarded or already-fixed relationship. Per the plan's own words,
this section's deliverable is the reading pass and its written findings, not
a mechanical artifact.

Suite run: not applicable — no source change was made in this pass, so
`tools/run_all_checks.ps1` was not run against a diff. (The last full-suite
baseline recorded elsewhere in project memory is 107 checks on an
unprovisioned bench, decomposed as prefix-glob + wired-by-name.)
