# Review: Pico reboot detection fix 2 (2026-10-10)

Reviewer: opus (rvrebootfx2). Scope: origin/dev commits 72d4f683f, 6ab706684,
a2c7080be and 66aab5fe9. They fix REVIEW_REBOOTDET_2026-10-10.md MED-1 (a DIAG
that arrives before FW_VERSION double-counted one Pico reboot, and the second
count erased the T3 fatal latch) and LOW-1..3. Base reviewed: origin/dev
66aab5fe9. No code was changed.

## Verdict

MED-1 is fixed. The fatal latch is now OR-preserved across reboot seq bumps and
is consumed only by the first operator acquire or by `heat_enable_init()` at
ESP boot. I found no sequence that silently clears it. The FW_VERSION skip
removes the double count in the MED-1 order. The expected-uptime comparison is
arithmetically sound across both the tick wrap and the Pico uptime wrap.

The new comparison and the skip flag add three LOW findings. One of them
(LOW-1) is a narrow fail-unsafe regression: a second Pico reboot within about
2 s of the first can go uncounted, so a fatal second boot can be read as the
benign first one and heat resumes. No HIGH or MED findings.

## Findings

### LOW-1: a fast second reboot after an uptime-counted one can be missed (fail-unsafe)

Where: `safety_link_frames.c` FW_VERSION boot_id-changed branch (~418, the
`reboot_noted_by_uptime` skip) together with
`safety_pico_uptime_behind_expected()` (`safety_link_frame.c`).

Scenario:
1. Pico reboot A (benign reason, for example a deliberate reset). All four
   boot-burst FW_VERSION frames are lost. This is not rare: the ESP inbox is 4
   deep and the 2026-08-25 measurement in `SAFETY_INBOX_LEN`'s comment shows
   about 70% of Pico broadcasts dropped. With no link-down, the ESP never
   re-requests FW_VERSION (`!peer_version_known` only).
2. A's first DIAG counts the reboot by uptime and sets
   `reboot_noted_by_uptime`. heat_enable resolves the held claim as benign and
   F1 re-requests heat.
3. Pico reboot B follows within about 2 s (for example a brownout after the
   power blip that caused A). B's burst FW_VERSION arrives. Its boot_id differs
   from the cached (pre-A) id, the flag is set, so the branch skips it as A's
   own FW_VERSION, then clears the flag.
4. B's first DIAG is compared against A's last DIAG. Example: A's DIAG at
   uptime 1000 ms at ESP tick E, B boots at E+1000, B's first DIAG at uptime
   1000 ms arrives at E+2000. expected = 3000, diff = -2000, tol = 2000. Not
   counted.

Result: no seq bump for B. `diag_boot_reason` now reads BROWNOUT, but
heat_enable only looks at the reason when a new seq arrives, so the verdict
stays "benign" and heat stays requested after a fatal reboot. The pre-fix code
counted B through its FW_VERSION, which was safe. B's TRIP_EVENT seq 1 is also
deduped against A's `trip_last_seq`, and `diag_trip_seq` is A's.

Fix (any one):
- Treat a change of `diag_boot_reason` between two consecutive DIAGs with no
  seq bump in between as a reboot. The Pico latches its boot reason once per
  boot (`boot_reason.c` `s_cached`, written before the scheduler starts), so a
  change is proof of a new boot. A same-reason miss leaves the heat verdict
  unchanged anyway, so this closes the unsafe half.
- Time-bound the skip (see LOW-2).
- Once DIAG V3 lands (TEST_TRIP WP3), compare the DIAG's `pico_boot_id` with
  the boot_id the last count was made for, and drop the FW_VERSION skip.

### LOW-2: `reboot_noted_by_uptime` can stay set indefinitely

Where: the flag is set in `safety_apply_diag` (~1122) and cleared only by a
FW_VERSION that carries a boot_id (~430) or by
`safety_reset_stale_peer_info_if_link_down()` (`safety_link.c` ~321).

Scenario: an uptime count that is not followed by a FW_VERSION. That happens
when the burst is lost (LOW-1 step 1), or when the count was a false positive
(LOW-3). With the link up, `peer_version_known` stays true and nothing requests
FW_VERSION, so the flag stays set for hours. The next genuine reboot whose
FW_VERSION arrives before its first DIAG (the normal order) is then skipped. Its
count is delayed to its first DIAG, about 2 s later, or missed entirely if that
DIAG is within tolerance (LOW-1). During the delay:
- the old boot's ARMED DIAG still counts as `diag_since_reboot`, so F1 can
  re-request heat from the new boot on old-boot evidence;
- `trip_last_seq` dedup is not reset, so the new boot's first TRIP_EVENT (seq
  1) can be deduped;
- a clear in that window uses the old `diag_trip_seq`.

The `&& link->pico_uptime_baseline_known` half of the skip condition does not
limit this: the baseline is always known while the flag is set.

Fix: record the ESP tick of the uptime count and skip only when the FW_VERSION
arrives within a short window of it (one DIAG period plus the boot-burst span,
about 3 s). Optionally also require the current uptime baseline to be young
(below a few seconds), since the skip is only meant for a boot that has just
started.

### LOW-3: ESP-side drain delay or a frozen DIAG now reads as a reboot

Where: `safety_apply_diag` stamps `esp_now_ms` when the frame is drained from
`link->inbox`, not when the UART task received it.

The comparison is `diff = (uptime_1 - uptime_0) - (esp_1 - esp_0)`. Each ESP
stamp includes that frame's queue delay, so diff = d0 - d1 where d is the
drain delay of each DIAG. If one DIAG waits more than 2 s longer in the inbox
than its predecessor, it is counted as a reboot. That needs the poll task and
every `xact_lock` holder to stop draining for more than 2 s: poll-task
starvation, a long locked exchange, or the never-reproduced 2026-08 multi-surface
stall. The old regression test (`now < prev` outside a wrap band) had no such
false positive.

A frozen DIAG (the 2026-08-23 bench bug, `link_task.c` ~580: uptime stuck for
90+ s while DIAGs kept arriving) now counts a reboot whenever two DIAGs are
more than 2000 ms apart, which is most of them, because the 2000 ms tolerance
equals the DIAG period.

Consequence during a firing: a seq bump that heat_enable resolves at once with
the current boot's reason. It pauses with `pico_fatal_reboot` if the current
Pico boot reason is fatal (the operator already resumed after that boot) or the
last DIAG was TRIPPED. Both are fail-safe nuisances. Otherwise it costs a k4
episode reset, trip dedup and relay-edge resync, a reannounce, an inflated
`pico_reboot_by_uptime_count`, and it sets the flag behind LOW-2.

Fix: add the physical bound. A reboot happens after the baseline, so the new
boot's uptime cannot exceed the ESP time elapsed since the baseline. Count only
when `diff < -tol` and `(int32_t)(now_ms - esp_elapsed_ms) <= tol` as well,
or simply `now_ms <= esp_elapsed_ms + tol`. Drain-delay and frozen-DIAG cases
have a large `now_ms` and fail that bound. The LOW-1 case (long outage) and
the wrap case still pass. Stamping the receive tick in the UART task would
remove the drain-delay half too.

### INFO

- Stale references to the removed `safety_pico_uptime_regressed()`:
  `safety_link.h` ~1638 and `firmware/SaftyFW/src/tasks/link_task.c` ~3435
  (the boot_id comment). The 2026-10-09 kilnlink audit mention is historical.
- `test_pico_reboot_detected_by_uptime_regression` LOW-1 case b (baseline
  20 s, ESP elapsed 300 s, new uptime 310 s) is physically impossible: a new
  boot's uptime cannot exceed the ESP time since the baseline. It passes today
  and would correctly start failing under the LOW-3 bound, so rewrite it with
  an uptime below 300 s when that lands.
- An outage of 24.8 to 49.7 days counts a reboot through the
  `>= 0x80000000u` guard (fail-safe). At 49.7 days or more the elapsed time
  aliases. Harmless in practice.

## Questions answered

**Can any sequence still lose the fatal latch?** No. A new seq ORs the old
latch into the claim-less verdict (`|| s_he.reboot_fatal_latched`) and no
longer clears it. `reboot_was_tripped` is ORed into a still-pending verdict.
Release keeps `verdict_pending`, `fatal_latched` and `was_tripped`.

**Can acquire or init clear it silently?** No. `heat_enable_init()` is called
only from `main_control_bringup.c` at ESP boot, where nothing is running.
`acquire_since` consumes the latch only on the first claim (`was_empty`), and
turns it into `reboot_hold`, which the executor acts on at
`profile_executor.c` ~2540: it pauses the profile and aborts autotune with
`pico_fatal_reboot`. The four acquire callers (profile start, resume, both
autotune starts) are operator actions; none re-acquires automatically. A
stale-epoch acquire does not consume.

**Can a reboot be missed, or heat auto-resume after a fatal reboot?** Yes,
only in the narrow LOW-1 window. Otherwise every reboot is counted at least
once (the uptime check fires when the burst is lost, and a FW_VERSION counts
whenever the flag is clear).

**Is the flag reset on every path?** The FW_VERSION and link-down paths clear
it, and the struct is zeroed at init. No path clears it on time, which is
LOW-2. The baseline is deliberately kept across link-down, so the elapsed time
includes the outage; that is correct.

**Tick wrap and signed arithmetic.** `xTaskGetTickCount() * portTICK_PERIOD_MS`
is continuous mod 2^32, so `esp_now_ms - baseline_esp_ms` is exact while the
gap is below 2^31 ms; the guard counts a reboot beyond that. `expected` and
`diff` wrap correctly across the Pico uptime wrap (case covered by the test).
`tol` is at most 2000 + 2^31/5000 and fits in int32.

**Locks.** Every new field (`pico_uptime_baseline_esp_ms`,
`reboot_noted_by_uptime`) is read and written under `state_lock`, in
`safety_apply_diag`, `safety_apply_fw_version` and the link-down reset.
`xTaskGetTickCount()` inside the lock is fine. heat_enable state stays under
its own lock as before.

**TEST_TRIP WP2/WP3 interaction.** DIAG V3 (32 bytes) adds `pico_boot_id`, a
third reboot signal. KilnFW still accepts only 30 and 31 byte DIAGs today. When
WP3 lands it should:
- count at most once per DIAG across all three signals (uptime, DIAG boot_id,
  FW_VERSION), with `safety_note_pico_reboot_locked()` as the only counter;
- update the cached boot_id from the DIAG too, which makes the FW_VERSION skip
  and its flag unnecessary (fixes LOW-1 and LOW-2);
- reset the boot_id-bearing DIAG cache inside
  `safety_note_pico_reboot_locked()`, not only in the FW_VERSION branch. In the
  skip case the cache already holds the new boot's DIAG, so that is safe.

**Are the tests vacuous?** No, apart from the unphysical case noted in INFO.
See the negative tests below.

## Tests run

Host tests in the worktree at origin/dev 66aab5fe9,
`build_host_tests.ps1 -Only "^(safety_link|main)( |$)"`: main 14631/14631,
safety_link 816/816, exit 0. (`test_heat_enable.c` is built into the `main`
executable, so `-Only heat_enable` selects nothing.)

## Negative tests

`tools\negtest.ps1 -Command "build_host_tests.ps1 -OutDir {OUT} -Only '^(safety_link|main)( |$)'"`
with `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`,
baseline passing, one throwaway copy per worker.

All 10 mutations were CAUGHT. The baseline passed (exit 0). The run's overall
verdict read ERROR only because this review doc was written into the worktree
while the run was going, which negtest counts as a real-tree change. No tracked
file changed, and every throwaway copy was removed.

| Mutation | File | Verdict | First failing assertion |
|---|---|---|---|
| a: drop `\|\| reboot_fatal_latched` from the claim-less verdict | heat_enable.c | CAUGHT | test_heat_enable.c:1230 "newer seq erases the unconsumed T3 lost-trip latch" |
| b: clear `reboot_fatal_latched` on a new seq (pre-fix behaviour) | heat_enable.c | CAUGHT | test_heat_enable.c:1230 |
| c: FW_VERSION skip never taken | safety_link_frames.c | CAUGHT | test_safety_link_compile.c:3200 "FW_VERSION counts the same reboot again" |
| d: link-down reset keeps the flag | safety_link.c | CAUGHT | test_safety_link_compile.c:3229 "link-down clears the flag" |
| e: `expected = baseline` (no ESP elapsed term) | safety_link_frame.c | CAUGHT | test_safety_link_compile.c:1228 (LOW-1 case) |
| f: remove the `>= 0x80000000u` guard | safety_link_frame.c | CAUGHT | test_safety_link_compile.c:1235 |
| g: tolerance base 2000 -> 0 | safety_link_frame.c | CAUGHT | test_safety_link_compile.c:1222 |
| h: DIAG path never sets the flag | safety_link_frames.c | CAUGHT | test_safety_link_compile.c:3198 |
| i: FW_VERSION never clears the flag | safety_link_frames.c | CAUGHT | test_safety_link_compile.c:1284, 3203 |
| j: acquire never consumes the latch into `reboot_hold` | heat_enable.c | CAUGHT | test_heat_enable.c:1175, 1210, 1358 |

The tests do not cover LOW-1..3. Each fix should arrive with a test that fails
on the current code: a boot-reason change with no seq bump (LOW-1), a stale flag
followed by a later boot_id change (LOW-2), and a DIAG drained more than 2 s
late or a frozen uptime (LOW-3).
