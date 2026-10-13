# Review: TRIPFX4 review fixes, 2026-10-12

Scope: origin/dev `03228b65b`, the fix for `docs/audits/REVIEW_TRIPFX4_2026-10-12.md` LOW-1,
LOW-2, INFO-1 and INFO-2. It touches 10 files: `dashboard_http.c`/`.h`,
`dashboard_status_http.c`, `safety_link.h`, SaftyFW `link_task.c`, three KilnFW host tests,
`mcp_server_safety.py` and `test_safety_test_trip.py`. This was a read-only review in a fresh
worktree at origin/dev `8b8eccf1c`. Nothing in firmware or tools was changed. All mutations
ran in `tools/negtest.ps1` throwaway copies.

Safety lens (owner rule): TEST_TRIP is admin-only, and a test trip must never be mistaken for
a real trip or mask one. TEST is `SAFETY_TRIP_TEST = 4`, mask `0x0008`.

## Verdict

There are no HIGH or MED findings. The forward case of TRIPFX4 LOW-1 is closed: a real trip
after a cleared TEST now drops `trip_is_test` as soon as its TRIP_EVENT arrives, because the
flag needs the cached TRIP_EVENT to say reason 4 and carry the same `trip_seq` as the DIAG.
The binding is reset-consistent across a Pico reboot. The MCP tool's 6 s window and its
TRIP_EVENT confirm refuse correctly. Four of the five re-run host mutants and all four pytest mutants
compiled and failed a named assertion.

There are two new LOWs. The reverse case that TRIPFX4 also named is still open, and the fix's
code comment says it is closed (LOW-1). The `trip_reason == 4` term of the new binding has no
test that fails without it (LOW-2). The claimed CAUGHT for `L1_drop_event_reason` must have
dropped `trip_event_ever_received` too.

## Findings

| ID | Sev | One line |
|---|---|---|
| LOW-1 | LOW | Reverse case still open. After a TEST trip is cleared, the cached DIAG still says TRIPPED reason 4 and the TRIP_EVENT cache (never cleared by CLEAR_TRIP) still says reason 4 with the same seq, so `trip_is_test` stays `true` for up to one DIAG period (4.5 s with one lost DIAG) while the Pico is ARMED. The new comment in `dashboard_status_http.c` claims a cleared trip "cannot read as a test trip while the DIAG is stale" |
| LOW-2 | LOW | Dropping only `ds->trip_reason == 4u` from `trip_is_test` passes the full KilnFW host suite. Every test case that changes the TRIP_EVENT reason also changes the seq, so the seq term alone decides them. The term is logically redundant while seq identity means "same trip" (one reason per trip on the Pico), so no behaviour is at risk today. The fix claim "L1_drop_event_reason CAUGHT" overstates the pin. Add a case with equal seqs and `trip_reason = 2` -> `false` |
| INFO-1 | INFO | Residual forward window: a real trip still reads as a test trip until its first TRIP_EVENT copy arrives, and for up to one DIAG period if all 4 burst copies (`LINK_TRIP_REPEAT_COUNT`, 250 ms apart) are lost |
| INFO-2 | INFO | `safety_test_trip`'s docstring still says it verifies by GET_DIAG reason and mask only; the new TRIP_EVENT path confirms by seq and reason, with no mask check. Safe, because the Pico latches one reason per trip, but the docstring is stale |
| INFO-3 | INFO | The reboot-clear pin (`test_tripfx4_low2_...`) calls `safety_note_pico_reboot_locked()` directly instead of driving a reboot detection path. It catches M6, but not a detection path that stops calling the helper |
| INFO-4 | INFO | Scope note: the commit touches 10 files, not 11 |

## Q1: can a real trip be reported or cleared as a test trip?

Reported: only in the INFO-1 window. `trip_is_test` (`dashboard_status_http.c:760-764`) now
requires

    DIAG TRIPPED && diag_trip_reason == 4 && trip_event_ever_received && trip_reason == 4
      && diag_trip_seq_known && diag_trip_seq == trip_last_seq && diag_since_reboot
      && diag_frame_age_ms <= SAFETY_LINK_DIAG_FRESH_MS

A real trip after a cleared TEST makes the Pico send a TRIP_EVENT with a new seq and a
non-TEST reason. `safety_apply_trip_event()` refreshes `trip_reason` and `trip_last_seq` on
every copy, deduplicated or not (`safety_link_frames.c:1382-1383`), so either term drops the
flag. The Pico latches only one reason per trip (first wins, `safety_guards.c`), and
`trip_seq_next()` wraps 255 -> 1 and never reaches 0. So a seq can only alias after 255 trips,
which the 10 s rate limit rules out inside one DIAG period.

Cleared: no. Nothing consumes `trip_is_test`. CLEAR_TRIP is bound to `diag_trip_seq`
(`safety_link_commands.c:125`), and `safety_clear_trip()` judges the mask, not this flag.

Reverse case (LOW-1). TRIPFX4 Q1 named it ("after step 2, if nothing trips, `trip_is_test`
stays true for the same window") and suggested a cross-check against the STATUS frame's
tripped flag (`SAFETY_FLAG_ENABLED`, byte1 bit4, "relay_owner is ARMED / not tripped").
The fix did not add it. The cleared-trip test case only covers a fresh DIAG that already says
ARMED. Impact matches TRIPFX4: no consumer, a bounded window, and the error is in the
over-report direction for "test", never a real trip hidden. Fix: AND in the STATUS-side
not-ENABLED (tripped) flag when STATUS is fresh, and add a test with DIAG TRIPPED reason 4, a
matching TEST TRIP_EVENT and STATUS ENABLED -> `false`. At minimum, correct the comment.

## Q2: seq/reason binding across a Pico reboot

Consistent. `safety_note_pico_reboot_locked()` (`safety_link_frames.c:270-290`) clears all
three inputs of the new binding together: `trip_event_ever_received`, `trip_last_seq`,
`diag_trip_seq_known`/`diag_trip_seq`, plus `diag_since_reboot`. Each needs a fresh frame
from the new boot before the flag can be true again. Walked against the reset-one-side
checklist in `CLAUDE.md`:

- Old boot TEST seq 1, the new boot trips for real with seq 1, and the TRIP_EVENT arrives
  before the reboot is noted: the cache still takes reason R, so the flag is false.
- The reboot is noted after that TRIP_EVENT: `trip_event_ever_received` drops, so the flag is
  false until a resend. That is fail-safe (it under-reports).
- A V3 DIAG with a new boot id notes the reboot and then latches `diag_since_reboot` and
  `diag_trip_seq` in the same apply. The flag still needs a new-boot TRIP_EVENT.

Every ordering errs toward `false`. There is no pair left with one side reset.

## Q3: the MCP tool's window and seq check

Correct. `_TEST_TRIP_VERIFY_WINDOW_S = 6.0` exceeds `SAFETY_LINK_DIAG_FRESH_MS` (4.5 s). The
TRIP_EVENT confirm needs `ever_received`, `last_seq == trip_seq` and `trip_reason == 4`, with
`trip_seq` an `int`. The POST's `trip_seq` comes from the Pico's TEST_TRIP_RESULT, sent only
once the latch is seen, and only as ACCEPTED when the latch is TEST
(`link_task_poll_test_trip_reply`). The timeout path answers seq 0, which no TRIP_EVENT can
carry, so it falls back to the DIAG path. The DIAG path still breaks on reason 4 alone, but the
pre-POST read refused any nonzero reason or mask, and the ESP's DIAG cache only moves forward.
So a reason-4 DIAG seen afterwards is newer than the POST, and the mask is still checked after
the loop.

## Negative tests (re-run)

All runs used `tools/negtest.ps1` with private mutation JSON. Host runs used `-Preset
kilnfw-host -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.
Each CAUGHT below was checked in its log: the mutant compiled (no BUILD FAILURES) and a named
assertion failed.

Host run: baseline PASS (473.3 s), real tree unchanged, every copy removed.

| Mutation | File | Change | Result |
|---|---|---|---|
| M2_copy_status_age | `dashboard_http.c` | copy `sl.age_ms` into `diag_frame_age_ms` | CAUGHT (`test_dashboard_safety_ready.c:259`, `:263`) |
| M6_reboot_keeps_since_reboot | `safety_link_frames.c` | drop `diag_since_reboot = false` in the reboot note | CAUGHT (`test_safety_link_compile.c:3355`) |
| M8_age_always_zero | `safety_link.c` | invert the age ternary | CAUGHT (`test_safety_link_compile.c:3343`, `:3346` and older age pins) |
| L1_drop_seq_bind | `dashboard_status_http.c` | drop `diag_trip_seq == trip_last_seq` | CAUGHT (`test_dashboard_status_http.c:350`) |
| L1_drop_event_reason | `dashboard_status_http.c` | drop only `trip_reason == 4u` (keep `trip_event_ever_received`) | **MISSED** (exit 0, suite green; see LOW-2) |

Pytest run (`-Preset pytest -PresetArg tests/test_safety_test_trip.py`): baseline PASS, 4/4
CAUGHT, real tree unchanged.

| Mutation | Change | Result |
|---|---|---|
| P1_event_seq_unchecked | drop `ev.last_seq == trip_seq` | CAUGHT (`test_trip_event_wrong_seq_or_reason_does_not_confirm`) |
| P2_event_reason_unchecked | drop `ev.trip_reason == 4` | CAUGHT (same test) |
| P3_window_2s | window 6.0 -> 2.0 s | CAUGHT (`test_verify_window_covers_diag_period`) |
| P4_no_event_path | never set `via_event` | CAUGHT (`test_late_diag_confirmed_by_trip_event`) |

## Fix status

| Item | Status |
|---|---|
| LOW-1 | open |
| LOW-2 | open |
| INFO-1 | open, no action proposed |
| INFO-2 | open |
| INFO-3 | open, no action proposed |
| INFO-4 | no action |
