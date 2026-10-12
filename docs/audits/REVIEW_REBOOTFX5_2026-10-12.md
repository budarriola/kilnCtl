# Review: REBOOTFX5 (origin/dev b2890bfd5, 4c2f7b994, 713c474d8) -- receive-stamp follow-ups

Reviewer: opus, 2026-10-12. Scope: `b2890bfd5` "Safety link: FW_VERSION note
window and DIAG freshness age use the receive stamp; stamp helper pinned
(REBOOTFX4 LOW-1..3, INFO-1/2)", `4c2f7b994` (source pin requires the exact
initializer ending in a comma), `713c474d8` (doc). Reviewed at origin/dev
`a6d475e8b`.

Counts: HIGH 0, MED 0, LOW 2, INFO 5.

## What changed

- New helper `uart_proto_rx_stamp_ms(TickType_t)`
  (`firmware/hwAbstraction/esp/uart/uart_protocol.h:118-121`):
  `(uint32_t)(ticks * (TickType_t)portTICK_PERIOD_MS)`. Both construction
  sites in `handle_raw_frame()` use it (BROADCAST `uart_protocol.c:286`, DATA
  `:341`).
- `SafetyLinkClass::diag_tick` is now a `uint32_t` ms receive stamp
  (`safety_link.h:1439`), written only at `safety_link_frames.c:1176`
  (`link->diag_tick = msg->rx_ms`).
- `safety_link_get_status()` computes `diag_frame_age_ms` as
  `now_ms - diag_tick` with `now_ms = (uint32_t)(xTaskGetTickCount() *
  portTICK_PERIOD_MS)` (`safety_link.c:702`), under `state_lock`, the same
  lock `safety_apply_diag()` writes it under.
- The FW_VERSION reboot-note window compares `msg->rx_ms` against
  `reboot_noted_by_uptime_esp_ms` (`safety_link_frames.c:427-428`) instead of
  the drain tick.
- Tests: `test_stalled_inbox_fw_version_does_not_double_count_reboot`,
  `test_diag_frame_age_uses_receive_time` (includes a wrap case),
  `test_rx_ms_units_pinned_at_uptime_consumer` (test_safety_link_compile.c), and
  `test_rx_stamp` in test_uart_protocol_link_delegate.c (helper values plus a
  source-text pin on `uart_protocol.c`).

## Answers to the review questions

**Tick wrap and units.** Correct on target. `CONFIG_FREERTOS_HZ=100` (IDF
default, not overridden in `sdkconfig.defaults`), so `portTICK_PERIOD_MS` is
10 and the stamp has 10 ms resolution. For any period that divides 1000,
`((t mod 2^32) * p) mod 2^32 == (t * p) mod 2^32`, so the ms stamp is
continuous across the 32-bit tick wrap. It wraps every 2^32 ms (49.7 days),
and every consumer subtracts modulo 2^32, so the wrap is harmless. The host
test pins the wrap (stamp `UINT32_MAX - 99`, now 300, age 400). Units are
consistent at every site today, but host tests cannot detect a units error;
see LOW-1.

**Other consumers of diag_tick / rx_ms.** None missed. `rx_ms` is read at
exactly three places (`safety_link_frames.c:428`, `:1129`, `:1176`), and
`diag_tick` at exactly one (`safety_link.c:702`). `diag_frame_age_ms` feeds
`relay_authority_pico_diag_fresh()` (`relay_authority.h:370`), the profile
executor's resume/start gate (`profile_executor.c:2340`) and
`dashboard_status_http.c:759`, all against `SAFETY_LINK_DIAG_FRESH_MS` (4500).
It also feeds `relay_authority_pico_tripped()` (INFO-1).

**Every caller, including the recovery image.** `safety_apply_diag()` and
`safety_apply_fw_version()` are called only from `safety_drain_inbox_ex()`
(`safety_link_inbox.c:304`, `:313`). `uart_proto_message_t` is constructed
only at the two sites in `handle_raw_frame()`. Every other occurrence is a
whole-struct copy (`xQueueSend`/`xQueueReceive`) or a test builder.
`firmware/KilnFW_recovery` does not compile `uart_protocol.c` or any
`safety_link*` source. Its only mention is a comment in
`recovery_pico_proto.h`, so the recovery image is unaffected. `UnitTestFw`
carries its own fork of the UART protocol and is outside this change.

**Did any freshness or guard decision become more permissive?** No.
- DIAG freshness: the age is now measured from receive rather than drain.
  Receive is never later than drain, so the age can only grow and the gates
  refuse more often. A negative age is impossible (`rx_ms <= now`); if one
  occurred, it would wrap to a huge value and read stale, which is
  fail-closed.
- FW_VERSION note window: the delta is `msg->rx_ms` minus a DIAG receive
  stamp. The inbox is FIFO, so a FW_VERSION is never processed before the DIAG
  that set the note, and the delta is non-negative and no larger than the old
  drain-based one. This means suppression happens more often, which restores
  the pre-REBOOTFX4 drain-to-drain behaviour the window was designed around.
  It does not hide a reboot: a genuine second reboot is still counted by the
  DIAG uptime-regression path and by an independent boot_id change (pinned at
  `test_safety_link_compile.c:3617`). If `rx_ms` ever preceded the note, the
  delta would wrap huge and the reboot would be counted, which errs toward
  counting.
- `relay_authority_pico_tripped()`: see INFO-1. This moves in the
  fail-closed direction.

## Findings

### LOW-1: tick-versus-ms units are unpinned on host (M1, M6, M7 MISSED)

The host FreeRTOS stub (`test/stubs/freertos/FreeRTOS.h`) defines
`configTICK_RATE_HZ 1000u` and `portTICK_PERIOD_MS 1u`, so on host a tick
count and a ms count are the same number. Three units errors pass all 99 host
test executables:

- M1: the helper drops the period (`return (uint32_t)ticks;`). On target,
  `rx_ms` would be in ticks (10 ms each). The uptime-elapsed math in
  `safety_apply_diag()` would then see ESP time running 10 times slow,
  producing false or missed Pico reboot detection, and DIAG ages would mix
  bases.
- M6: the age computed as `safety_elapsed_ms((TickType_t)link->diag_tick)`,
  which treats the ms stamp as a tick count.
- M7: the age computed with the period dropped from `now`.

M6 and M7 would produce ages on target that are garbage, and mostly huge.
That is fail-closed (DIAG reads stale), but it permanently refuses heat and
resume. `test_rx_stamp`'s helper value checks compare the helper against the
same formula with `portTICK_PERIOD_MS == 1`, so on host they are tautological
except for the M2 "/1000" class.

The age formula at `safety_link.c:702` also re-spells the helper inline
instead of calling `uart_proto_rx_stamp_ms(xTaskGetTickCount())`, so the units
formula exists in two places.

Fix: call the helper at `safety_link.c:702`. Then either build one host-test
executable (`safety_link` plus `uart_protocol_link_delegate`) with the stub's
`portTICK_PERIOD_MS` overridable to 10 (`configTICK_RATE_HZ 100`), or add a
compile-time check in the stub path, for example
`_Static_assert(portTICK_PERIOD_MS != 1 || ...)` in a dedicated test TU that
defines period 10. With period 10, M1, M6 and M7 all change observable ages.

### LOW-2: the uart_protocol.c source pin is brittle and bypassable (M4 false CAUGHT, M5 MISSED)

`test_rx_stamp` reads `uart_protocol.c` as text, counts every `.rx_ms`
substring, comments included, requires exactly 2, and requires each one to
begin `.rx_ms = uart_proto_rx_stamp_ms(xTaskGetTickCount()),`.

- It is brittle. M4 replaces the DATA site with the semantically identical
  `(uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS)` and fails the pin.
  So does any new comment that mentions `.rx_ms`.
- It is bypassable. M5 comments out the DATA-site stamp, keeping the exact
  text inside `/* ... */`. The pin still counts 2 matching occurrences and
  passes, so the DATA frame now carries `rx_ms = 0` (designated-initializer
  zero), and nothing else in the host suite notices. A third construction
  site that never names `rx_ms` at all (zero-initialised) is also invisible
  to the pin.

Impact if such a regression shipped: DATA frames would carry `rx_ms = 0`.
DIAG ages would be huge (fail-closed, heat refused). The uptime baseline
would use ESP time 0, so reboot detection would degrade.

Fix: strip comments before matching, and anchor on construction rather than
on the field. Count `uart_proto_message_t <name> = {` initializers in
`uart_protocol.c` and require each one's brace body to contain the helper
call. Alternatively, move the stamp into a single
`uart_proto_message_init(&m, ...)` function that the host test calls
directly.

### INFO-1: mixed time basis in relay_authority_pico_tripped()

`relay_authority.h:359-361` compares `trip_event_age_ms <=
diag_frame_age_ms`. `trip_event_age_ms` is still drain-based
(`trip_event_tick = xTaskGetTickCount()`, `safety_link_frames.c:1401`, aged
through `safety_elapsed_ms()`). `diag_frame_age_ms` is now receive-based. The
DIAG age can only grow, so the predicate reads "tripped" more often.

After a stalled drain that carried both a TRIP_EVENT and a later DIAG, this
can report a spurious "tripped" for up to about one DIAG period. Before this
change, a same-pass drain gave equal ages and `<=` already answered
"tripped", so there is no practical regression. It is fail-closed.

### INFO-2: other drain-based stamps remain

`cached_tick` (STATUS `age_ms`/`link_up`, `safety_link_frames.c:897`),
`trip_event_tick`, `update_status_tick` and the stash ticks are still taken
at drain. These are pre-existing, and a late drain only makes them look
fresher by the inbox stall. They are outside REBOOTFX4's scope. If the
receive-stamp contract is meant to be uniform, `cached_tick` (link_up) is the
next candidate.

### INFO-3: deframe-time stamp, 10 ms resolution

The DATA site stamps after `xSemaphoreTake(tasks_lock, portMAX_DELAY)`, so
`rx_ms` is the deframe time, not the wire time (as REBOOTFX4 INFO-4 noted).
On target the resolution is 10 ms. Both are negligible against the 3000 ms
note window and the 4500 ms freshness bound.

### INFO-4: recovery image and UnitTestFw unaffected

See "Every caller" above. No change is needed there.

### INFO-5: the M12 log shows one environmental build failure

The M12 run lists `zones_http` under BUILD FAILURES, caused by `fatal error
C1083: ... cfg_fs_status.obj: Permission denied`. That is a transient file
lock in the per-mutant output directory, not a compile error from the
mutation, because `relay_authority.h` compiled into every other executable.
M12's CAUGHT verdict rests on assertion failures in
`profile_executor_prestart`, which compiled and ran.

## Negative tests

`tools\negtest.ps1 -Preset kilnfw-host -ExpectPattern "(?m)^\s+FAIL |FAIL
.*\.c:\d+|RUN FAILURES|BUILD FAILURES" -Parallel 2` on worktree
`C:\wt\rvrebootfx5_olzt3i` (`a6d475e8b`).
- The unmutated baseline PASSED (454 s).
- "REAL TREE CHANGED" was not printed, and the worktree was clean afterwards.
- Every CAUGHT below was confirmed from its log as a compiled mutant with a
  named failing assertion.

| Mutant | File | Change | Verdict | Evidence |
|---|---|---|---|---|
| M1 helper drops period | uart_protocol.h | `return (uint32_t)ticks;` | MISSED | all 99 executables built and passed (LOW-1) |
| M2 helper in seconds | uart_protocol.h | `... * portTICK_PERIOD_MS / 1000u` | CAUGHT | test_uart_protocol_link_delegate.c:213 "rx stamp is ticks * period in ms, not seconds", :215 wrap |
| M3 BROADCAST site unstamped | uart_protocol.c:286 | `.rx_ms = 0u,` | CAUGHT | test_uart_protocol_link_delegate.c:245 "both ... construction sites stamp rx_ms via the helper" |
| M4 DATA site equivalent inline | uart_protocol.c:341 | `(uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS)` | CAUGHT (false positive) | :245 pin fails on a semantically identical rewrite (LOW-2, brittle) |
| M5 DATA site commented out | uart_protocol.c:341 | stamp moved inside `/* ... */` | MISSED | all 99 passed; the pin counts the comment (LOW-2, bypass) |
| M6 age on tick basis | safety_link.c:702 | `safety_elapsed_ms((TickType_t)link->diag_tick)` | MISSED | all 99 passed; period 1 on host (LOW-1) |
| M7 age drops period | safety_link.c:702 | `(uint32_t)xTaskGetTickCount() - diag_tick` | MISSED | all 99 passed; period 1 on host (LOW-1) |
| M8 age reversed | safety_link.c:702 | `diag_tick - now_ms` | CAUGHT | test_safety_link_compile.c:3176 "age counts from receive", :3182 "age is wrap-safe" |
| M9 diag_tick at drain | safety_link_frames.c:1176 | `diag_tick = now_ms` | CAUGHT | :3172 "diag_tick is the receive stamp", :3176, :3177 "stalled-inbox DIAG is stale", :3182 |
| M10 FW_VERSION window at drain | safety_link_frames.c:428 | `now_ms - reboot_noted_by_uptime_esp_ms` | CAUGHT | :3158 "FW_VERSION must not double count a stalled-inbox reboot" |
| M11 FW_VERSION window never matches | safety_link_frames.c:428 | `... < 0u` | CAUGHT | :3158, :3609 "MUST GO RED if the FW_VERSION counts the same reboot again", :3610-3617 |
| M12 freshness gate +1000 ms | relay_authority.h:370 | `<= SAFETY_LINK_DIAG_FRESH_MS + 1000u` | CAUGHT | test_profile_executor_prestart.c:2359 "resume refused on a stale DIAG", :13791 "diag age == FRESH_MS+1 -> refused", :13894 (INFO-5) |

Summary: 8 CAUGHT (one of them, M4, is a brittleness false positive) and 4
MISSED. The MISSED mutants form two classes: units errors invisible at host
period 1 (LOW-1), and a pin bypass through a comment (LOW-2). Every
behavioural mutant of the REBOOTFX4 fixes themselves (M8 to M12) is caught.
