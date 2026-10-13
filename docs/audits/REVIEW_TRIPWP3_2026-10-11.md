# Review: TEST_TRIP WP3 (KilnFW side), 2026-10-11

Scope: origin/dev `9a085e44f..6fb350b55`, five commits: `cbc65ef43`, `d88d1414a`,
`445f29402`, `6abfb4591`, `6fb350b55`. Plan: `docs/TEST_TRIP_PLAN.md`. This is a
read-only review in a fresh worktree at `6fb350b55`. Nothing was fixed.

## Verdict

No HIGH and no MED findings. **A dual flash of dev (KilnFW plus SaftyFW, both at
protocol 18) to the bench is safe for WP4**, with the usual dual-reflash S6a
procedure from CLAUDE.md. The trip mask is `1 << (6 - 1) = 0x0020`; check that
before `safety_clear_trip()`. WP2-review HIGH-1 is resolved for that pairing:
- the ESP parses the 32-byte DIAG;
- the ESP builds the 5-byte CLEAR_TRIP V3 once it has cached the boot id and trip_seq;
- the boot-time S6a auto-clear waits for a V3 DIAG when the peer is protocol 18 or higher.

So trips are clearable after a dual flash. The LOW items below are worth fixing,
but none of them can leave heat on or clear a real trip.

## Findings

| ID | Sev | One line |
|---|---|---|
| LOW-1 | LOW | A reboot can be counted twice across two DIAGs: a 30-byte DIAG counts it by uptime, then the first V3 DIAG counts it again because the boot id differs from the stale cached id |
| LOW-2 | LOW | The 200 ACCEPTED reply hardcodes `trip_reason 4` / `trip_mask 8`, with no read-back; a real guard that wins on the same tick is reported over HTTP as a test trip |
| LOW-3 | LOW | `trip_is_test` comes from the TRIP_EVENT cache (`ds->trip_reason`), not the live DIAG; it stays true after the clear, and true while a later real trip whose TRIP_EVENT was lost is latched |
| LOW-4 | LOW | Route mapping of DUPLICATE (409 duplicate) is untested; negtest mutating it to 200 is MISSED (see Negative tests) |
| LOW-5 | LOW | `safety_test_trip`'s read-back mask comparison is unpinned: dropping `or after.trip_mask != expected` passes `test_safety_test_trip.py` (negtest MISSED) |
| INFO-1 | INFO | `s_test_trip_request_id` restarts at ESP boot while the Pico's dedup state lives on; a request within 10 s of an ESP reboot can collide with the Pico's last id (409 duplicate, or the WP2 LOW-1 ACCEPTED replay) |
| INFO-2 | INFO | The DIAG boot-id reboot path logs nothing, unlike the FW_VERSION path's "rebooted" line |
| INFO-3 | INFO | REBOOTFX2 LOW-1 (missed fast second reboot) still open; the 3 s bound narrows it, V3 does not close it |
| INFO-4 | INFO | The stack budget of the new httpd path was not measured (no ELF, no target build allowed); its locals match sibling commands |
| INFO-5 | INFO | The 0x2F case in the stats-counting switch (`safety_link_inbox.c:239`, `cmd_test_trip_result_count`) is untested (negtest MISSED); the stashing dispatch case is covered |

## Focus 1: safety

- **Can it leave heat on?** No. The route only asks the Pico to latch a trip.
  The Pico drops heat on the trip. The executor faults the run on a TRIPPED
  DIAG. `test_safety_watchdog.c` now has a reason-4 case:
  `test_test_trip_reason4_faults_in_words` checks FAULT and the "test trip"
  wording. The route is not mode-gated, which is correct: aborting a firing
  is the intended effect.
- **Can it mask a real trip?** No:
  - `send_test_trip` refuses locally when the cached DIAG is TRIPPED.
  - The Pico refuses `REFUSED_ALREADY_TRIPPED` (409).
  - If a real guard trips after the test trip latched, the Pico owns the
    mask/reason (WP2 scope).
  - The ESP clear builds its mask from the cached DIAG.
- **Can a TEST trip be cleared as if real, or the reverse?** Clears still go
  through the unchanged Pico rules: mask, trip_seq, boot_id (F6), and the
  `trip_verify_s` hold for a test trip. The ESP S6a boot auto-clear still only
  fires for a MAIN_FAULT trip (`test_boot_clear_never_fires_for_a_non_s6a_trip`).
- **Real trip mid-request:** the Pico answers ALREADY_TRIPPED (409), or
  ACCEPTED if the test frame won the race. The residual case where a real guard
  wins the same tick is LOW-2 here and WP2 LOW-1 on the Pico side. The MCP
  tool catches it through its DIAG read-back: it reports FAILED VERIFY unless
  reason == 4 and mask == 0x0008.
- **Recovery image:** not exposed. `firmware/KilnFW_recovery` does not
  compile or register the route. In the application, the route returns 409
  `recovery_mode` under `boot_guard_is_recovery_mode()` before touching the
  link (host-tested).

## Focus 2: wire compatibility

- **18 ESP with 17 Pico:** the 17 Pico sends 31-byte DIAG. The ESP caches
  trip_seq but no boot id, so it sends the 4-byte clear (tested).
  `send_test_trip` refuses locally with `LOCAL_PEER_TOO_OLD` (501, naming
  `pico_protocol`) and sends nothing (tested).
- **17 ESP with 18 Pico:** the Pico sends 32-byte DIAG only to a peer it
  knows is 18 or higher, so a 17 ESP keeps getting 31 bytes and sends 4-byte
  clears. That is WP2 behaviour, unchanged here.
- **Unknown peer version (announce lost):**
  - The ESP gets 30-byte DIAG and sends the 3-byte clear.
  - An 18 Pico refuses that clear until announce or the 30 s F6 fallback.
  - The ESP re-announces on 30- and 31-byte DIAGs from a peer 17 or higher,
    with a bounded budget.
- **CLEAR_TRIP V3 vs the 4-byte form:** `.has_boot_id = boot_id_known &&
  trip_seq_known`. The payload is sized for V3. A Pico reboot forgets the
  cached boot id, so a clear is never built from the old boot's id
  (negtested, see below).
- **0x2F dispatch:** a counted case plus an always-stashed case, with a new
  `s_compat_frames` row. Negtested: removing the stashing dispatch case
  (`safety_link_inbox.c:433`) is CAUGHT. Removing the stats-counter case
  (line 239) is MISSED (INFO-5).

## Focus 3: reset-one-side class

- `safety_note_pico_reboot_locked()` now clears `diag_boot_id_known` /
  `diag_boot_id` with `diag_trip_seq_known`. That is the third member of the
  pair (negtested).
- The link-down reset clears `pico_boot_id_known`, peer version,
  `reboot_noted_by_uptime_esp_ms` and the re-announce budget. The cached
  DIAG fields are left alone. Every DIAG refreshes them, and both senders
  gate on DIAG staleness first, so this is acceptable.
- The REBOOTFX2 skip in the FW_VERSION boot-id branch is now bounded to
  `SAFETY_REBOOT_NOTE_WINDOW_MS` (3 s). That closes REBOOTFX2 LOW-2. The
  stale-flag test goes red if it is removed.
- **LOW-1 below** is the remaining instance of this class.
- `s_test_trip_request_id` against the Pico's dedup state is INFO-1.

### LOW-1: double reboot count, 30-byte DIAG then V3 DIAG

`safety_apply_diag()` (`safety_link_frames.c` ~1142) counts a boot-id change
only when this same DIAG did not already count by uptime:

```c
if (link->pico_boot_id_ever_seen && p[31] != link->pico_boot_id && !pico_reboot_by_uptime)
    safety_note_pico_reboot_locked(link);
```

The guard is per DIAG. After a Pico reboot, the new boot does not yet know
the peer version, so it sends 30-byte DIAGs:
1. The first 30-byte DIAG counts the reboot by uptime regression.
2. That DIAG does not update `link->pico_boot_id`, which still holds the old
   boot's id.
3. The ESP re-announces, and the Pico switches to 32-byte DIAG.
4. If no FW_VERSION of the new boot arrived in between, the first V3 DIAG
   sees `p[31] != pico_boot_id` with `pico_reboot_by_uptime == false`, and
   counts the same reboot a second time.

The ESP does not request FW_VERSION on a reboot note. The Pico pushes only a
boot burst, and it drops roughly 70% of broadcasts on a full inbox.

What the second count does:
- bumps `pico_reboot_seq` again;
- resets `trip_last_seq` (a duplicate TRIPPED log or event if a trip is latched);
- resyncs the relay-edge baseline;
- spends a fresh re-announce budget.

In `heat_enable_note_pico_boot()` the verdict inputs survive this:
- `reboot_was_tripped` is OR-preserved while pending, or re-derived from `last_pico_tripped`;
- `reboot_fatal_latched` is OR-preserved;
- a benign verdict does not clear `reboot_hold`.

So this does not turn a fatal reboot benign. The code's own comment says a
second bump "would reset heat_enable's verdict inputs and erase the T3
lost-trip latch". That holds only after a verdict has already been taken
between the two counts.

Suggested fix: when the uptime path notes a reboot, forget the cached boot
id, so the next V3 DIAG adopts the new id without counting. For example,
clear `pico_boot_id_ever_seen` for the comparison, or keep a
"reboot noted, id pending" flag that the V3 block honours within
`SAFETY_REBOOT_NOTE_WINDOW_MS`. Add a test:
30-byte DIAG with uptime regressed, then V3 DIAG with a new id, then
`pico_reboot_seq` bumped exactly once.

### LOW-2: ACCEPTED reply is not read back

`safety_test_trip_post_handler()` (`dashboard_exec_http.c` ~996) reports
`"trip_reason":4,"trip_mask":8` from constants. If a real guard latched on
the same Pico tick (WP2 LOW-1 residue), an HTTP caller other than the MCP
tool sees a test trip. The web banner reads the live `diag_trip_reason`, so
the operator UI is right. Fix: read the cached DIAG after the reply. Either
report what it shows, or omit reason/mask and leave verification to the
caller. WP4 judges must verify with the DIAG, as the MCP tool does.

### LOW-3: `trip_is_test` follows the TRIP_EVENT cache

`dashboard_status_http.c:780` emits `trip_is_test` inside the
`trip_event_ever_received` block, from `ds->trip_reason`. That cache is only
cleared by a Pico reboot note. So the field stays true after the test trip is
cleared, and stays true while a later real trip is latched whose TRIP_EVENT
(Frame D) was lost. Plan 5.5 calls it a convenience derived from reason ==
4, which it is, but its name reads as "the latched trip is a test". Fix:
derive it from the live DIAG (`diag_state == TRIPPED && diag_trip_reason == 4`
on a fresh DIAG), or rename it. No clear path reads it. WP4 judges should key
on `diag_trip_reason`.

### LOW-4: DUPLICATE status untested

`test_dashboard_exec_http_handlers.c` maps every outcome except
`SAFETY_LINK_TEST_TRIP_DUPLICATE`. A mutation making DUPLICATE answer
`200 OK` with `ok:true` passes the suite. Add the case.

### LOW-5: MCP read-back mask check unpinned

`mcp_server_safety.py` fails verify when `after.trip_reason != 4 or
after.trip_mask != expected`. The only mismatch test, `test_readback_mismatch_fails`,
feeds reason 6 / mask 0x20, so the reason half alone fails it. A mutation
deleting the mask half passes the file. Add a case with reason 4 and a wrong
mask (for example 0x0028, a real guard latched alongside).

## Focus 4: route tier, CSRF, URI cap

- `route_tier_table.h:295`: `POST /api/safety/test_trip` is `ROUTE_TIER_ADMIN`.
  `test_web_auth_safety_interaction.c` pins it (negtested).
- CSRF: the global cross-origin refusal for state-changing requests covers
  it. The body requires `confirm=1` (400 otherwise, link never asked).
- `check_uri_handler_cap.ps1`: 176 routes, cap 184, 8 spare. PASS.
- A missing link returns a plain-text 500. The PcTools client raises on
  non-JSON, which the tool reports. Acceptable.

## Focus 5: tests and the author's gaps

The targeted KilnFW host tests pass: `build_host_tests.ps1 -Only
"safety_link|dashboard_exec|dashboard_status|safety_trip_words|safety_watchdog|web_auth_safety|kilnlink"`
built and ran 9/9 executables, all PASS. `pytest tests/test_safety_test_trip.py`
gives 9 passed.

### Negative tests (tools\negtest.ps1, throwaway copies)

Every mutation ran in a throwaway copy, rebuilt from scratch (`-OutDir {OUT}`).
KilnFW mutations used `build_host_tests.ps1 -Only` with the targeted regex above
and `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
Baselines passed. (The first KilnFW run reported REAL TREE CHANGED only because
this review file was written into the worktree during it; the copies were removed
and the per-mutation verdicts stand.)

| Mutation | Target | Result |
|---|---|---|
| CLEAR_TRIP never bound (`.has_boot_id = false`) | `safety_link_commands.c` | CAUGHT |
| Pico reboot keeps the cached DIAG boot id | `safety_link_frames.c` note_reboot | CAUGHT |
| Drop `!pico_reboot_by_uptime` (count twice per DIAG) | `safety_link_frames.c` V3 block | CAUGHT |
| V3 DIAG boot id not adopted | `safety_link_frames.c` V3 block | CAUGHT |
| TEST_TRIP_RESULT request_id not matched | `safety_link_commands.c` | CAUGHT |
| Route tier ADMIN to USER | `route_tier_table.h` | CAUGHT |
| 0x2F stashing dispatch case removed (`s_compat_frames` row's coverage) | `safety_link_inbox.c:433` | CAUGHT |
| 0x2F stats-counter case removed | `safety_link_inbox.c:239` | **MISSED** (INFO-5) |
| DUPLICATE answers 200 ok | `dashboard_exec_http.c` | **MISSED** (LOW-4) |
| DIAG V3 encoder writes 0 for the boot id (test_diag 32-byte vector) | CommonFW `kilnlink_diag.c`, check `check_commonfw_diag_vectors.ps1` | CAUGHT |
| Python codec drops the `pico_boot_id` byte | `kilnlink_codec.py`, pytest `test_safety_test_trip.py` | CAUGHT |
| MCP verify ignores the mask | `mcp_server_safety.py`, same pytest | **MISSED** (LOW-5) |

This closes the author's open negtest gaps: the Python pins (one of two is
caught), the test_diag 32-byte vector (caught), and the `s_compat_frames` 0x2F
row (caught, through the dispatch case it guards).

## Not checked

- The stack budget of the httpd path (INFO-4). It needs a target ELF, and
  this review may not do target builds. The two 253-byte-payload
  `uart_proto_message_t` locals match `safety_link_get_stack_margin()` and
  the other commands.
- The full host suite after the rebase. Only the targeted subset above was run.
- SaftyFW (WP2) code, beyond the wire facts quoted here.

## Fix status

Fixed in the commit "TEST_TRIP WP3 review fixes: single reboot count across 30/32-byte DIAG, route
read-back, live trip_is_test, tests" (2026-10-11, on origin/dev).

| ID | Status | Fix |
|---|---|---|
| LOW-1 | fixed | `safety_apply_diag()` V3 block honours the uptime path's reboot note for `SAFETY_REBOOT_NOTE_WINDOW_MS` (as the FW_VERSION branch does), so one reboot is counted once; the new boot id is adopted either way. Test: `test_wp3_low1_reboot_counted_once_across_30_then_32_byte_diag` |
| LOW-2 | fixed | The 200 reply reads the live DIAG: `readback:true` with the observed `trip_reason`/`trip_mask` and `is_test`; otherwise `readback:false` with `requested_trip_reason`/`requested_trip_mask` only. A real guard that won shows its own reason and `is_test:false` |
| LOW-3 | fixed | `trip_is_test` is emitted in the DIAG block from `diag_state == TRIPPED && diag_trip_reason == 4` on a DIAG fresher than `SAFETY_LINK_STALE_MS`; clears with the trip |
| LOW-4 | fixed | `test_safety_test_trip` pins DUPLICATE as 409 `ok:false` |
| LOW-5 | fixed | New `test_readback_right_reason_wrong_mask_fails` (reason 4, mask 0x28); the old mismatch test is renamed `test_readback_wrong_reason_fails` |
| INFO-1 | fixed | `s_test_trip_request_id` is seeded once per ESP boot from `hal_sysinfo_random_u32()` |
| INFO-2 | fixed | The DIAG boot-id reboot path now logs a WARN line |
| INFO-3 | open | Unchanged (REBOOTFX2 LOW-1; V3 does not close it) |
| INFO-4 | open | Stack budget still unmeasured; needs a target ELF measurement on the bench-flashed build |
| INFO-5 | fixed | `test_wp3_info5_test_trip_result_counted_in_stats` covers the 0x2F stats case |
