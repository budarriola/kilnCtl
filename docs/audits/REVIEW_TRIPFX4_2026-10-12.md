# Review: TRIPFX3 review fixes, 2026-10-12

Scope: origin/dev `7f213c002`, the code fixes for `docs/audits/REVIEW_TRIPFX3_2026-10-12.md`
LOW-A, LOW-B, LOW-C, INFO-A and INFO-C. `893859644`, the requested sha, is the doc-only
Fix status table for that review and was read as part of the scope. This was an
adversarial, read-only review in a fresh worktree at origin/dev `893859644`. Nothing in
firmware or tools was changed. All mutations ran in `tools/negtest.ps1` throwaway copies.

Safety lens (owner rule): TEST_TRIP is admin-only, and a test trip must never be mistaken
for a real trip or mask one. TEST is `SAFETY_TRIP_TEST = 4`, mask `0x0008`.

## Verdict

There are no HIGH or MED findings. The five claimed fixes are present and correct. The
new `diag_tick` is written and read under `safety_lock` and needs no reset of its own
(Q3). The stricter readback cannot fail a legitimate POST (Q4). Nothing found can leave
heat on, clear a real trip, or hide the real trip reason: `trip_reason` and `trip_mask`
still come from TRIP_EVENT and are reported next to `trip_is_test`. Today `trip_is_test`
is an API-only field. A repo grep finds no web page, LCD page or PcTools consumer.

There are two new LOWs. `trip_is_test` is derived from the DIAG frame alone, so it can
still read `true` for up to one DIAG period while the Pico is latched on a newer, real
trip (Q1, LOW-1).

The second is a test gap (Q5, LOW-2). Only the `diag_tick` stamp is pinned. The rest of
the chain behind LOW-B and LOW-C passes the full KilnFW host suite under mutation:

- the age computation in `safety_link_get_status()`;
- the `dashboard_http.c` copy of the two new fields;
- the `diag_since_reboot = false` clear on a Pico reboot.

One example: forcing `diag_frame_age_ms` to 0, so every DIAG reads fresh forever, is
MISSED. TRIPFX3's own fix-status mutations all hit the stamp or the fake-status layer,
which is why that table read all CAUGHT.

## Findings

| ID | Sev | One line |
|---|---|---|
| LOW-1 | LOW | `trip_is_test` never consults TRIP_EVENT or `trip_seq`. Clear TEST, then a real guard trips before the next DIAG: the cached DIAG still says TRIPPED reason 4, so `trip_is_test` reads true for up to about 2 s (up to 4.5 s if a DIAG is lost) while the real trip is non-TEST |
| LOW-2 | LOW | Only the `diag_tick` stamp is pinned. Three links pass the full suite under mutation: the age computation (`safety_link.c:702`; M3 unit, M8 age always 0), the `dashboard_http.c:352-353` copy (M1 dropped, M2 STATUS age) and the reboot clear `diag_since_reboot = false` (`safety_link_frames.c:280`; M6), which LOW-C now depends on |
| INFO-1 | INFO | The 4500 ms bound is `2 x LINK_DIAG_TX_PERIOD_MS + 500`. It tolerates exactly one lost DIAG and fails safe (false) after two. Nothing pins it to the Pico's period |
| INFO-2 | INFO | `readback:false` is now the common answer of a successful `POST /api/safety/test_trip`, because the readback is immediate and the TRIPPED DIAG lags by up to one DIAG period. This is correct, but `safety_test_trip`'s 2.0 s poll window equals the DIAG period and can report a spurious FAILED VERIFY |
| INFO-3 | INFO | The readback binds `diag_trip_seq` but not `diag_boot_id`. Covered in practice by `diag_since_reboot` |
| INFO-4 | INFO | A DIAG exactly 2^32 ticks old (about 49.7 days at 1 kHz) aliases to a fresh age. This needs a dead link for 49 days with the cache kept, and the same aliasing already exists for `trip_event_age_ms` |
| INFO-5 | INFO | `safety_link_status_t` grows by 4 B and `dashboard_status_t` by about 5 B. Both are trivial |

## Q1: can `trip_is_test` be true while the real trip is not TEST?

Yes, briefly. This is LOW-1.

`trip_is_test` (`dashboard_status_http.c:757-759`) is

    TRIPPED && diag_trip_reason == 4 && diag_since_reboot
            && diag_frame_age_ms < SAFETY_LINK_DIAG_FRESH_MS

and every term comes from the last DIAG (Frame B). The Pico sends DIAG every 2000 ms
(`LINK_DIAG_TX_PERIOD_MS`, `SaftyFW/src/tasks/link_task.c:214`) and sends no immediate
DIAG on a trip. A trip is announced right away by TRIP_EVENT, which updates
`trip_reason`, `trip_mask` and `trip_seq`, but `trip_is_test` reads none of them.

Sequence:

1. TEST is latched. A DIAG arrives with TRIPPED, reason 4.
2. The operator clears the trip. The Pico is ARMED.
3. Within the same DIAG period a real guard (S1-S8) trips. TRIP_EVENT arrives with
   reason != 4 and a new `trip_seq`.
4. Until the next DIAG, `trip_is_test` is `true` with `trip_reason` != 4 in the same
   response. With one DIAG lost, this lasts up to 4.5 s.

The reverse case also exists: after step 2, if nothing trips, `trip_is_test` stays true
for the same window while the Pico is not tripped at all.

Impact is limited. No UI or tool consumes `trip_is_test`, `trip_reason` is correct in
the same JSON, and the window is one DIAG period. It still breaks the owner rule as
written ("never mistaken for a real trip") for any future consumer that trusts the
flag alone, such as a TEST badge.

Suggested fix: make `trip_is_test` false whenever a TRIP_EVENT newer than the DIAG
exists and disagrees. For example, require `trip_reason == SAFETY_TRIP_TEST` when
`trip_event_age_ms < diag_frame_age_ms`, or require `diag_trip_seq == trip_seq` when
both are known (the same binding the INFO-A readback fix already uses). A cross-check
against the STATUS frame's tripped flag would also close the reverse case.

## Q2: is 4500 ms the right bound?

It is defensible. See INFO-1.

4500 = 2 x 2000 + 500. A healthy link refreshes the DIAG every 2000 ms, so the age stays
under about 2000 ms plus UART and task latency. One lost DIAG raises it to about
4000 ms, which still passes. Two lost DIAGs in a row make the flag drop to false. That
fails safe (it under-reports a test trip, never over-reports one) and has no UI or log
effect today.

Weakness: the constant lives in `safety_link.h` and nothing ties it to
`LINK_DIAG_TX_PERIOD_MS` on the Pico. If the Pico period is raised past 2250 ms, the
flag flaps false on a healthy link. If it is lowered, the bound stays loose and
lengthens the LOW-1 window. Suggest a cross-reference comment on both sides, or a
narrow mirror-drift check like the existing ones.

## Q3: lock and reset consistency of `diag_tick`

There is no defect.

- Write: `safety_apply_diag` sets `link->diag_tick = xTaskGetTickCount()`
  (`safety_link_frames.c:1176`) under `safety_lock`, in the same critical section that
  sets `diag_ever_received` and `diag_since_reboot`.
- Read: `safety_link_get_status` computes
  `out->diag_frame_age_ms = out->diag_ever_received ? safety_elapsed_ms(link->diag_tick) : 0u`
  (`safety_link.c:702`) under `safety_lock`, right after `*out = link->cached`. This is
  the only copy site. `safety_elapsed_ms` uses unsigned tick subtraction, so it is
  wrap-safe apart from INFO-4.
- Init: `link` is zeroed in `safety_link_start` (`safety_link.c:359`).
- Reboot: `safety_note_pico_reboot_locked` (`safety_link_frames.c:270`) clears
  `diag_since_reboot`, `diag_trip_seq_known` and `diag_boot_id_known` but leaves
  `diag_tick` alone. That is correct. `trip_is_test` and the readback both require
  `diag_since_reboot`, so a stale age from the old boot is never consulted, and the next
  DIAG sets both again together.
- Link down: `safety_reset_stale_peer_info_if_link_down` (`safety_link.c:301`) clears no
  DIAG cache. That is acceptable, because the age grows past the bound by itself.
- "Never received": the age is 0, but `trip_is_test` sits inside
  `if (ds->diag_ever_received)`, so the 0 is never read as "fresh". Mutation M7 (compute
  the age unconditionally) is harmless for the same reason.

Read against the "reset one side of a pair" checklist in `CLAUDE.md`: `diag_tick` pairs
with the `diag_ever_received` latch (never cleared) and the `diag_since_reboot` gate
(cleared on reboot). Both consumers check the gate, so neither side can be left
inconsistent.

LOW-A check: the V3 branch now clears `reboot_noted_by_uptime` on every V3 DIAG. That is
sound for in-order UART delivery, because the note only describes the previous
STATUS-derived reboot and the V3 boot id supersedes it.

## Q4: does the stricter readback break a legitimate POST?

No. The readback (`dashboard_exec_http.c:1003-1007`) now needs `link_up`,
`diag_ever_received`, `diag_since_reboot`, TRIPPED, `diag_trip_seq_known` and
`diag_trip_seq == trip_seq`. When it fails, the handler still answers 200 `ok:true` with
`readback:false` and the `requested_*` fields. The POST result never depends on it.

The readback runs immediately after the command is sent, with no wait. The TRIPPED DIAG
lags by up to one DIAG period, so `readback:false` is now the normal answer of a
successful test trip (INFO-2). Before the fix, a stale DIAG could produce a
`readback:true` naming an older trip (TRIPFX3 INFO-A), so the change is correct.

PcTools `safety_test_trip` (`mcp_server_safety.py:376`) ignores the HTTP readback. It
polls GET_DIAG for `_CLEAR_TRIP_READBACK_WINDOW_S = 2.0` s at 0.25 s steps and checks
reason 4 and the mask, but not `trip_seq`. A 2.0 s window equals the DIAG period, so a
test trip sent just after a DIAG can miss the next one by link and task latency and
report FAILED VERIFY on a trip that happened. Suggest a window of at least
`SAFETY_LINK_DIAG_FRESH_MS` (4.5 s). Optionally also check `diag_trip_seq` against the
TRIP_EVENT `trip_seq`, as the firmware now does.

INFO-3: the readback could also require `diag_boot_id == boot_id`. `diag_since_reboot`
already covers the realistic case.

## Q5: plumbing gap, pinned with negtest mutations

`dashboard_http.c` is not built by any host test. The only coverage is a source-text
scan in `test_dashboard_safety_ready.c`, which does not look at the two new lines.

The mutations also show that `test_safety_link_compile.c`, which does build
`safety_link.c` and `safety_link_frames.c`, checks `link.diag_tick` directly and never
reads `diag_frame_age_ms` back through `safety_link_get_status()`. No test asserts that a
Pico reboot clears `diag_since_reboot`. That clear predates this commit (`5fa28ed3b`),
but LOW-C's fix now relies on it. Together this is LOW-2.

`tools/negtest.ps1 -Preset kilnfw-host` (full KilnFW host suite), 8 mutations,
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`.

Run: baseline PASS (792.9 s), log `negtest_logs/20261012_025250_uo4f`. The run ended
with verdict `ERROR` only because this review doc was written into the worktree while it
ran (`real_tree_unchanged: false`, the only change being the untracked
`docs/audits/REVIEW_TRIPFX4_2026-10-12.md`). Every throwaway copy was removed.

| Mutation | File | Change | Result |
|---|---|---|---|
| M1_copy_dropped | `dashboard_http.c` | drop `out->diag_frame_age_ms = sl.diag_frame_age_ms` | MISSED |
| M2_copy_status_age | `dashboard_http.c` | copy `sl.age_ms` (STATUS age, the TRIPFX3 LOW-B bug) | MISSED |
| M3_unit_100ms | `safety_link.c` | age `/ 100u` | MISSED |
| M4_no_stamp | `safety_link_frames.c` | drop the `diag_tick` stamp | CAUGHT (`test_safety_link_compile.c:3158`) |
| M5_stamp_cached_tick | `safety_link_frames.c` | stamp `cached_tick` instead | CAUGHT (`test_safety_link_compile.c:3158`) |
| M6_reboot_keeps_since_reboot | `safety_link_frames.c` | drop `diag_since_reboot = false` in `safety_note_pico_reboot_locked` | MISSED |
| M7_age_computed_when_never | `safety_link.c` | compute the age even when no DIAG was ever received | MISSED (harmless: the consumer is gated on `diag_ever_received`) |
| M8_age_always_zero | `safety_link.c` | invert the ternary, so the age is 0 whenever a DIAG was received | MISSED |

Suggested pins for LOW-2:

- `test_safety_link_compile.c`: apply a DIAG at tick T, advance the fake tick by N, and
  assert `safety_link_get_status()` reports `diag_frame_age_ms == N` (catches M3 and M8).
- Same file: latch `diag_since_reboot` with a DIAG, trigger a Pico reboot (boot id or
  uptime path), and assert it reads `false` until the next DIAG (catches M6).
- Extend the `test_dashboard_safety_ready.c`-style source scan of `dashboard_get_status()`
  to require `out->diag_frame_age_ms = sl.diag_frame_age_ms` and
  `out->diag_since_reboot = sl.diag_since_reboot` (catches M1 and M2). Better still,
  build `dashboard_http.c` with fakes in a host test.

## Fix status

| Item | Status |
|---|---|
| LOW-1 | open |
| LOW-2 | open |
| INFO-1 | open |
| INFO-2 | open |
| INFO-3 | open |
| INFO-4 | open, no action proposed |
| INFO-5 | no action |
