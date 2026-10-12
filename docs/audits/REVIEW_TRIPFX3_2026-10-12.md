# Review: TEST_TRIP WP3 review fixes, 2026-10-12

Scope: origin/dev `57c4e0454` (code fixes for `docs/audits/REVIEW_TRIPWP3_2026-10-11.md`
LOW-1..LOW-5, INFO-1, INFO-2, INFO-5) and `95cf5e6a7` (that review's Fix status table).
Adversarial, read-only review in a fresh worktree at origin/dev `f230b49a4`. Nothing in
firmware or tools was changed; all mutations ran in `tools/negtest.ps1` throwaway copies.

## Verdict

No HIGH and no MED findings. Every claimed fix is present and pinned by a test that
fails when the fix is reverted (table below), except the two that cannot be
host-tested: INFO-1 (random seed) and INFO-2 (log line). Nothing found can leave heat
on, clear a real trip, or hide a real trip reason: the Pico's trip mask is
single-bit and derived from the reason, and S9 escalation still overwrites reason 4
with 10 while TEST is latched.

Two new LOW items. One is a narrow gap the LOW-1 fix left in the reboot count
(LOW-A). The other is that the "fresh DIAG" bound behind `trip_is_test` is actually
measured against the STATUS frame clock (LOW-B).

## Findings

| ID | Sev | One line |
|---|---|---|
| LOW-A | LOW | The V3 DIAG path honours the uptime note but never consumes it. A second fast Pico reboot inside the 3 s note window, seen only through a new V3 boot id, is not counted |
| LOW-B | LOW | `trip_is_test`'s "fresh" bound uses `diag_age_ms`, which is `sl.age_ms`, the age of the last STATUS (Frame A) frame and not of the DIAG |
| LOW-C | LOW | `trip_is_test` and the readback ignore `diag_since_reboot`. After a Pico reboot detected by FW_VERSION while TEST was latched, the old boot's reason-4 DIAG still reads as a test trip until the new boot's first DIAG |
| INFO-A | INFO | The 200 readback does not bind to the trip it reports: there is no `diag_trip_seq == trip_seq` or age check. A DIAG from before a just-cleared real trip can be reported as `readback:true` with that old reason |
| INFO-B | INFO | Pico design, stated for the record: while TEST is latched, every other guard's debounce is frozen, so `trip_is_test` / the TEST badge stay true while a real S1-S8 condition develops. Only a refused clear signals it. S9 is not masked |
| INFO-C | INFO | An orphaned comment ("TEST_TRIP_PLAN 5.5: derived from reason == 4 ... Mirrored in ...") still sits in the trip_event block, above unrelated code |
| INFO-D | INFO | The `/api/status` worst case is 5792 / 5888 B, leaving 96 B of headroom. `trip_is_test` moved, so nothing grew, but the margin is thin |
| INFO-E | INFO | The test_trip handler's frame grew: `json[256]` plus a `safety_link_status_t` held across the send chain. The INFO-4 stack measurement is still outstanding |

## Fix status

Fixed in `9f311dfd9` (rebased sha in the landing note). Negtest: `tools/negtest.ps1 -Preset kilnfw-host`, targeted `-Only "test_safety_link_compile|test_dashboard_status_http|test_dashboard_exec_http_handlers"`, 8 mutations, all CAUGHT, baseline PASS. Full KilnFW host suite: 98/98 pass. `check_00_kilnfw_target_build.ps1`: PASS.

| Item | Status | Fix | Mutation |
|------|--------|-----|----------|
| LOW-A | fixed | V3 boot-id branch clears `reboot_noted_by_uptime`; test `test_tripfx3_lowa_second_quick_reboot_within_note_window_counted` (the reviewer's probe) | M1 drop the clear: CAUGHT |
| LOW-B | fixed | new `diag_tick` stamped in `safety_apply_diag`, exported as `diag_frame_age_ms`; `trip_is_test` bounded by it (`SAFETY_LINK_DIAG_FRESH_MS` 4500) | M2 use `diag_age_ms`: CAUGHT; M8 drop the stamp: CAUGHT |
| LOW-C | fixed | `trip_is_test` and the test_trip readback require `diag_since_reboot` | M4 (status) and M5 (readback): CAUGHT |
| INFO-A | fixed | readback requires `diag_trip_seq_known && diag_trip_seq == trip_seq` | M6 drop the bind: CAUGHT |
| M3 gap | fixed | boundary tests at `FRESH_MS-1` (true) and `FRESH_MS` (false) | M3 `<` to `<=`: CAUGHT |
| M4 gap | fixed | `link_up=false` readback case | M7 drop `st.link_up`: CAUGHT |
| INFO-C | fixed | orphaned comment removed | n/a |
| INFO-D | unchanged | JSON key set unchanged; worst-case mirror in `test_dashboard_json.c` still within 5888 B (host suite passes) | n/a |
| INFO-E | measured | `check_httpd_task_stack_budget` against the fresh target ELF: OK, deepest handler is `autotune_start_post_handler` at 4528 B (ceiling 4528 B of 8192 B); the test_trip handler is not among the deepest 5 | n/a |
| INFO-B | left | Pico design decision, out of scope | n/a |

Disclosure: the plumbing `safety_link_get_status` -> `diag_frame_age_ms` has no host-test pin of its own; M8 pins the `diag_tick` stamp and the status/readback tests pin the consumers.

### LOW-A -- the second reboot inside the note window goes uncounted

`firmware/KilnFW/App/drivers/safety/safety_link_frames.c:1150-1161`. `counted_by_uptime` is
true for `SAFETY_REBOOT_NOTE_WINDOW_MS` (3000 ms) after any uptime-counted reboot. The V3
branch adopts `p[31]` but never clears `link->reboot_noted_by_uptime`. The FW_VERSION branch
always clears it, at `:440`. So for the rest of the window, every boot-id change on a V3 DIAG
is assumed to be the same reboot.

How it fails:
1. Boot A is counted by uptime regression at t=0.
2. Its first V3 DIAG adopts id 43.
3. Boot A crashes young. Boot B's first V3 DIAG arrives at t=2 s with id 44 and uptime 1800 ms.
   The expected uptime is 3600 ms, so B is 1800 ms behind, inside the 2000 ms tolerance and not
   an uptime regression. B is not counted either.

The results:
- `pico_reboot_seq` does not move;
- `trip_last_seq` is not reset, which is the reset-one-side class;
- heat_enable's verdict inputs keep boot A's state.

This needs a young-boot reboot loop on a link that stays up across the gap. A link-down
clears the note in `safety_link.c:323`, so the window is narrow, but a fast reboot loop is
exactly when the count matters.

Fix: clear `link->reboot_noted_by_uptime` when the V3 branch adopts the id. After that the
cached id is current, so the FW_VERSION of the same boot sees no change and needs no note.

Evidence: a probe test (`test_probe_second_quick_reboot_within_note_window`, applied with
`negtest -Diff`) FAILS on origin/dev, which proves the gap. The same probe plus the one-line
fix PASSES, including the existing LOW-1 test. P1 (probe alone): CAUGHT, `test_safety_link_compile.c:3154: PROBE: boot B (new id 44) counted`. P2 (probe + fix): MISSED, all checks pass, including the LOW-1 test.

### LOW-B -- "fresh" is measured against the STATUS frame clock

Where it is computed:
- `dashboard_status_http.c:757-759` and `main_page.html:1982` both use `diag_age_ms < SAFETY_LINK_STALE_MS`.
- That value comes from `dashboard_http.c:351`, `out->diag_age_ms = sl.age_ms;`.
- `sl.age_ms` is `safety_age_ms_locked()`, which reads `link->cached_tick`.
- `cached_tick` is set only in `safety_apply_status()` (`safety_link_frames.c:898`).

So the bound limits how recently a STATUS frame arrived, not how old the cached DIAG is. The
Pico sends a DIAG every 2000 ms, which is longer than the 1500 ms stale bound.

What goes wrong:
- After a clear, `trip_is_test` and the banner can stay true for up to about 2 s.
- If DIAGs are dropped (`diag_tx_frames_dropped`) or rejected (frame_errors) while STATUS keeps
  flowing, they stay true indefinitely on a trip that has cleared.

The comment at `:753` and the fix table's "fresh DIAG" overstate the guarantee. This is
display-only, because heat never reads `trip_is_test`. No web, LCD, MCP or readiness code
consumes the JSON key; the dashboard computes its badge from `reason === 4` itself.

Fix: stamp a `diag_tick` in `safety_apply_diag()` and expose `diag_age_ms` from it. Bound it
by about two DIAG periods, not by the STATUS stale bound.

Evidence: dropping the age term is CAUGHT (M1), so the term itself is pinned. Changing `<` to
`<=` is MISSED: the 1500 ms boundary is untested.

### LOW-C -- `diag_since_reboot` is not consulted

Where: `dashboard_status_http.c:757` and `dashboard_exec_http.c:1003`.

What happens:
1. A FW_VERSION-detected reboot runs `safety_note_pico_reboot_locked()`, which sets
   `diag_since_reboot=false`.
2. That reboot leaves the cached DIAG fields as they were.
3. So a TRIPPED/reason-4 DIAG from the previous boot keeps `trip_is_test:true` until the new
   boot's first DIAG, which is at most one DIAG period.

This is display-only. Fix: AND in `diag_since_reboot` (copy it into `dashboard_status_t`).

### INFO-A -- the readback is not bound to the trip occurrence

Where: `dashboard_exec_http.c:1001-1010`. `readback` requires `get_status OK`, `link_up`,
`diag_ever_received` and `TRIPPED`. It does not compare `diag_trip_seq` against the reply's
`trip_seq`, and it does not check age.

How it can mislead:
1. Real trip reason 5.
2. Clear.
3. TEST_TRIP within 2 s, before the next DIAG.
4. The reply reads `readback:true, trip_reason:5, is_test:false`. That is identical to the
   "real guard won" case and wrong.

Within about 2 s of a fresh accept the readback is usually `false`, because the new TRIPPED
DIAG has not arrived yet. So it is rarely useful and occasionally misleading.
`safety_test_trip` (MCP) ignores these fields and polls DIAG itself, so the tool is unaffected.

Suggestion: require `st.diag_trip_seq_known && st.diag_trip_seq == trip_seq`, or drop the
readback and keep only `requested_*`.

Evidence: dropping `st.link_up` is MISSED. No test has `link_up=false`.

## Fix verification

| Claim | Where | Mutation | Result |
|---|---|---|---|
| LOW-1 one count across 30/32-byte DIAG | `safety_link_frames.c:1150` | M7 `counted_by_uptime` = this DIAG only | CAUGHT (`test_safety_link_compile.c:3129`, `:3135`) |
| LOW-1 note window bounded | same | M8 drop the 3000 ms bound | CAUGHT (`test_safety_link_compile.c:3135`) |
| LOW-2 live readback | `dashboard_exec_http.c:1003` | M5 drop TRIPPED term | CAUGHT (`test_dashboard_exec_http_handlers.c:328`) |
| LOW-2 is_test from live reason | `:1010` | M6 is_test always true | CAUGHT (`test_dashboard_exec_http_handlers.c:323`) |
| LOW-2 link_up gate | `:1003` | M4 drop `st.link_up` | MISSED |
| LOW-3 live, fresh trip_is_test | `dashboard_status_http.c:757` | M1 drop age term | CAUGHT (`test_dashboard_status_http.c:340`) |
| LOW-3 TRIPPED required | same | M2 drop TRIPPED term | CAUGHT (`test_dashboard_status_http.c:333`) |
| LOW-3 stale boundary | same | M3 `<` to `<=` | MISSED |
| LOW-5 mask half pinned | `mcp_server_safety.py:429` | drop `or after.trip_mask != expected` | CAUGHT (`test_readback_right_reason_wrong_mask_fails`) |
| INFO-1 seed | `safety_link_commands.c:1067` | M9 remove seed | not run (seed value, not host-observable) |
| INFO-2 log | `safety_link_frames.c:1269` | M10 remove log | not run (log line, not host-observable) |

LOW-4 (DUPLICATE answers 409) and INFO-5 (0x2F stats) are covered by new tests and were not
re-mutated.

Answers to the brief:
- **Can `trip_is_test` read true for a real trip?** Not by reason: the live DIAG reason must
  be 4, and the Pico's mask is single-bit and derived from the reason. It does stay true while
  a real condition develops under a TEST latch (INFO-B), and an S9 escalation flips it to
  false.
- **Can it stay true after a clear?** Yes, for up to about one DIAG period, or longer if DIAGs
  are lost while STATUS flows (LOW-B, LOW-C).
- **Is "fresh" bounded, and against what clock?** It is bounded against the STATUS clock, not
  the DIAG clock (LOW-B).
- **Is the readback racy?** It is a snapshot under the link lock, so it is torn-free, but it is
  not bound to this trip (INFO-A).
- **Does the boot-id adoption break S6a/S6b or the reset-one-side rule?** It does not double-count, and
  `trip_last_seq` is reset on every counted reboot. It can under-count, as LOW-A shows.
- **Can request_id be 0 or a duplicate, and is it thread-safe?** It is seeded and incremented
  under `xact_lock`, so it is thread-safe. `0` (seed 255) is harmless, because the Pico dedups
  on equality with the last id and treats 0 like any other id. A collision after an ESP reboot
  inside the 10 s window is 1/256 and replays the stored ACCEPTED.
- **Are short or odd DIAG lengths safe?** Yes. Only exactly 30, 31 or 32 bytes with
  `payload[0]==SAFETY_CMD_DIAG` are accepted; anything else counts as a frame error, and
  `p[31]` is read only for 32-byte frames.
- **Is the JSON worst-case mirror correct?** Yes. The mirror emits the key inside the diag
  block with `"false"`, the longer value, and the render is 5792 B against 5888 B.

## test_update_fetch.c:469 / :489 under load

Not reproduced. Ten clean-baseline runs of `build_host_tests.ps1 -Only test_update_fetch\.c` (four, then six, concurrently) while three kilnfw-host negtest workers and other sessions' builds ran (build-gate queue depth 25-42): every run reported `test_update_fetch: 232 checks, 0 failed`. The two checks (`:469`, `:489`) read `g_fr_heap_live` right after `WAIT_FOR(g_fr_live_tasks <= 1)`; if a task drops its live count before freeing its buffers, that is a plausible timing race under heavier load, but it did not show here. A separate review of the update_fetch test LOWs is in flight.

Negative-test setup: `tools/negtest.ps1 -Preset kilnfw-host -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"` (full suite, baseline PASS in 1330 s) for M1-M6 and M8; targeted runs limited to `test_safety_link_compile` for M7, P1 and P2; `-Preset pytest` for LOW-5. M9 and M10 were stopped unrun at the deadline; neither is observable from a host test.
