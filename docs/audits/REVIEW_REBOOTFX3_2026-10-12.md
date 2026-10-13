# Review REBOOTFX3: fwd/back modular uptime-behind detector (2026-10-12)

Scope: dev commit `9a6debca9` (OLDPOOLA LOW/MED-2 + INFO-4 fixes in
`safety_pico_uptime_behind_expected()`, new tests in
`test_safety_link_compile.c`, fix-status note in
`REVIEW_OLDPOOLA_2026-10-12.md`). Adversarial review, no code changed.

Verdict: 0 HIGH, 0 MED, 2 LOW, 3 INFO. The fix is correct and closes the
24.9-49.7 day blind spot. The new boundary semantics match the original
intent. The one behaviour change outside the targeted bug is a very narrow new
false-positive corner at the Pico's own 32-bit wrap (INFO-1). The header
contract comment was not updated (LOW-1). The commit's reason for leaving
OLDPOOLA LOW-1 open ("not contained") does not hold up (LOW-2).

## What the function now decides

`firmware/KilnFW/App/drivers/safety/safety_link_frame.c:51-69`. With
`expected = baseline + elapsed` (mod 2^32), `tol = 2000 + elapsed/5000`:

- `fwd = now - expected`, `back = expected - now` (both mod 2^32). If either
  is `<= tol`, `now` is within `tol` of `expected` on the circle: **not a
  reboot** (continuity window, line 62).
- Otherwise it is a **reboot iff `now <= elapsed + tol`** (physical window,
  line 68): a rebooted Pico cannot report more uptime than the ESP has seen
  elapse since the baseline.

`tol < 2^19` and `elapsed < 2^31` (line 53), so the two continuity arcs never
overlap and `elapsed + tol` cannot overflow.

## Answers to the review questions

**(1) Boundary semantics.** `now == elapsed + tol` is a reboot,
`elapsed + tol + 1` is not (tests `:1254`, `:1256`). That is the REBOOTFX2
LOW-3 physical bound made inclusive, consistent with "a reboot's new uptime
is at most the ESP time since the baseline, plus jitter". It is the correct
direction: an uptime *behind* expected by more than `tol` is a reboot exactly
when the value is physically reachable by a fresh boot.

A genuine reboot is missed by the continuity window iff the new uptime lands
within `tol` of `expected` mod 2^32. For a reset `r` ms after the baseline
was sent, `back = baseline + r` = the Pico's uptime at the moment it reset.
So the miss condition is exactly "the old boot was `<= tol` (~2 s) old when
it reset" (mod 2^32, i.e. also "within `tol` after its own 49.7-day wrap").
Modelled: pre-reset uptime 1500 and 2000 ms missed, 2001 and 2500 ms caught,
**identical to the pre-commit signed-diff code** (`diff >= -tol`). This is
inherent to any uptime-based detector: a 2 s-old boot that resets is
indistinguishable from drain jitter. It is acceptable: the FW_VERSION boot_id
branch and the protocol-18 V3 DIAG boot_id (`safety_link_frames.c:1144-1161`)
are independent and catch it 255 times in 256, and a Pico that resets within
2 s of booting is normally still announcing (FW_VERSION) anyway. See INFO-3.

**(2) Does `back <= tol` accept a Pico that ran ahead?** No. `back` is the
behind distance; an ahead step is `fwd`. An ahead step beyond `tol` has both
`fwd > tol` and `back = 2^32 - fwd > tol`, so it falls to the physical test.
When `expected` has not wrapped, `now = baseline + elapsed + fwd >
elapsed + tol`, so it is never a reboot (same as before). It can only be
flagged when the step carries `now` across the 2^32 wrap to a small value,
which is INFO-1. Bounding the ahead side further is not needed: the old code
also accepted any ahead step up to 2^31.

**(3) Genuine 49.7-day wrap.** The Pico reports
`to_ms_since_boot(get_absolute_time())` (`SaftyFW/src/tasks/link_task.c:1095`),
a `uint32_t` that wraps cleanly mod 2^32. A Pico that keeps running lands
within jitter of `expected` mod 2^32, so `fwd` or `back` is small and it is
not flagged. Test `:1242` (wrap straddled by 900 s) still passes, and mutant
F1 shows the `fwd` term is what saves it there. Modelled: baseline 3 s before
the wrap, +300 ms jitter: not a reboot. The ESP side
(`esp_now_ms = tick * portTICK_PERIOD_MS`, `safety_link_frames.c:1129`) is
also continuous mod 2^32, including across the tick-count wrap, since 2^32
ticks times the period is 0 mod 2^32.

**(4) Young-Pico step back (`:1251`, `(1000, 0, 500)`).** Uptime steps back
without a reboot whenever the current DIAG is drained later than the baseline
DIAG was (`elapsed` is stamped at drain, `safety_link_frames.c:1129`, so it
over-reads real time by `d1 - d0`). The Pico also samples uptime at frame
build time, not send time. Steps back of up to `tol` are normal jitter, and
the pre-commit code accepted them too (`diff = -500 >= -2000`). The test
matters only because for a young baseline (`baseline <= ~tol`) the physical
window also matches, so without the `back` term the function would call
jitter a reboot. Mutant F1 is caught only by this test. Accepting it is
correct.

**(5) OLDPOOLA LOW-1: is receive-time stamping contained?** Yes; see LOW-2.

## Findings

### LOW-1: header contract comment still describes the signed-diff algorithm

`firmware/KilnFW/App/drivers/safety/safety_link_frame.h:45-49` still says
"the signed modular difference now - expected must not be below -(2 s +
200 ppm...)" and "the 49.7-day wrap needs no special band". The code now
uses a circular +/-tol window plus the physical bound `now <= elapsed + tol`.
The REBOOTFX2 bound is also missing from the header. The in-function comment
(`safety_link_frame.c:58-59`, `:65-67`) is accurate, but its `diff` wording
at `:66` names a variable that no longer exists.

Failure scenario: a maintainer who trusts the header and "simplifies" the
body back to a signed diff reintroduces the 24.9-49.7 day blind spot (mutant
F2 below, which the new test `:1248` now catches, so this is documentation
risk only). Fix: rewrite the header paragraph to state the two windows and
the inclusive bound, and rename `diff` at `:66`.

### LOW-2: LOW-1 left open on a wrong premise; receive-time stamping is contained

The fix-status note in `REVIEW_OLDPOOLA_2026-10-12.md` says stamping at UART
receive "would mean carrying a timestamp through the DIAG message/inbox path
... not a contained change". I read the path, and it is contained:

- The inbox item is `uart_proto_message_t`
  (`firmware/hwAbstraction/esp/uart/uart_protocol.h:118-124`), copied by
  value into the queue. That struct already *is* the carrier from the
  receive task to `safety_apply_diag`
  (`safety_link_inbox.c:280`, `:313`), so a timestamp needs no new path.
- It is built at exactly two sites, both in the protocol's receive context
  and both using designated initializers:
  `uart_protocol.c:281` (BROADCAST) and `:335` (DATA). Adding `.rx_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS)`
  to each is two lines.
- Size: 264 to 268 bytes. Inboxes are PSRAM-backed and sized with
  `sizeof(uart_proto_message_t)` (`uart_protocol.c:631`). The eight by-value
  stashes in `SafetyLinkClass` (`safety_link.h:1464-1570`) grow 32 B in total.
  Nothing asserts `sizeof(uart_proto_message_t)`.
- Consumer: `safety_link_frames.c:1129` uses `msg->rx_ms` instead of reading
  the tick count. The note-window stamps at `:1137`/`:1153` stay consistent,
  because queue order is receive order.
- Host side: add the same field to the test stub
  (`firmware/KilnFW/App/test/stubs/uart_protocol.h:40-46`) and set it in the
  DIAG test helpers. A late-drain pair (baseline drained 3.5 s late, reboot
  100 ms after send) then pins OLDPOOLA LOW-1 itself.

Estimate: 1-2 hours, about 5 files and 40-80 lines including tests.
The remaining delay (UART driver ring buffer to protocol task) is
milliseconds, far below `tol`.

Failure scenario if left: OLDPOOLA LOW-1 as written: a multi-second inbox
stall on the baseline DIAG plus a 1-in-256 boot_id repeat lets an early
reboot pass unnoticed, so the reboot resync (`safety_note_pico_reboot_locked`,
`safety_link_frames.c:270-310`: trip dedup, relay-state and trip_seq
resync) does not run. Leaving it as a documented trade-off is defensible at
this probability. The fix is cheap, though, and also removes INFO-1, so the
stated "not contained" reason should be corrected whichever way it is
decided.

### INFO-1: new false-positive corner at the Pico's own 32-bit wrap

`safety_link_frame.c:62-68`. A Pico that ran *ahead* of `expected` by `j`,
with `tol < j <= tol + (time from baseline to its 2^32 wrap)`, and that
crosses its wrap between the two DIAGs, gives a small `now <= elapsed + tol`
and is now counted as a reboot. The signed-diff code returned "not a reboot"
because the diff was positive. "Ahead by more than `tol`" requires the
*baseline* DIAG to have been drained over 2 s later than the current one,
which is the same inbox stall precondition as OLDPOOLA LOW-1.
Modelled: baseline 1 s before the wrap, elapsed 5 s, baseline drained 2.5 s
late: `now = 6500`, new code `true`, old code `false`.

Consequence: one spurious `safety_note_pico_reboot_locked()`.
`pico_reboot_seq++` resets heat_enable's verdict inputs and the T3 lost-trip
latch (comment at `safety_link_frames.c:1141-1143`). That is not purely
conservative, but it needs a >2 s stall on the exact baseline frame within
seconds of a once-per-49.7-days event. Receive-time stamping (LOW-2) removes
it. No action needed beyond that.

### INFO-2: continuity-window boundaries and the 200 ppm term are unpinned

My mutants R1 (`back <= tol` to `back < tol`), R2 (drop the
`elapsed / 5000` term) and R3 (`fwd <= tol` to `fwd < tol`) all compile, and
all 903 checks pass (MISSED). The continuity window only decides the outcome
where the physical window also matches: a young baseline (`baseline <~ 2*tol`)
or the wrap. The old-baseline tests therefore never reach it. Low value, but
three one-line cases would pin it (verified against a Python model of
the function):

- `!f(2000u, 0u, 0u)`: `back == tol` is not a reboot (kills R1)
- `!f(0u, 1000000u, 1000000u - 2100u)`: tol 2200 at 1000 s elapsed (kills R2)
- `!f(UINT32_MAX - 999u, 1000u, 2000u)`: `fwd == tol` across the wrap
  (kills R3)

### INFO-3: inherent miss for a reboot within `tol` of the old boot's start

Covered under (1): unchanged from the pre-commit code and inherent to the
method. boot_id covers it. Recorded so a later reviewer does not re-open it
as a regression.

## The commit's own claims

- LOW/MED-2 fixed: confirmed. Modelled 24.9 d, 30 d, 49 d and 49.7 d
  baselines with a reboot to 500 ms are now caught, and were missed before.
  20 d and 24.85 d are caught by both.
- INFO-4 fixed: confirmed (F3/F4 caught at `:1255`/`:1257`).
- Test changes are additions only. No existing case was altered.

## Negative tests

`tools\negtest.ps1 -Command "powershell -File
firmware\KilnFW\App\test\build_host_tests.ps1 -OutDir {OUT} -Only
test_safety_link_compile"`, ExpectPattern
`(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`, base `9a6debca9`.
Baseline passed (903/903). `real_tree_unchanged: true`, copies removed. Logs:
`%TEMP%\negtest_logs\20261012_074433_t6sy`. Every CAUGHT log has 0
`BUILD FAILURES`, 0 compiler errors, and exactly one `FAIL file:line`
assertion.

| Id | Mutation (`safety_link_frame.c`) | Verdict | Assertion |
|----|----------------------------------|---------|-----------|
| F1 | drop `\|\| back <= tol` | CAUGHT | `:1252` young-Pico step back |
| F2 | signed diff `(int32_t)fwd >= -(int32_t)tol` | CAUGHT | `:1248` 31-day baseline reboot |
| F3 | bound `<=` to `<` | CAUGHT | `:1255` now == elapsed + tol |
| F4 | bound `elapsed + 2u * tol` | CAUGHT | `:1257` now == elapsed + tol + 1 |
| R1 | `back <= tol` to `back < tol` | MISSED | INFO-2 |
| R2 | tol without the 200 ppm term | MISSED | INFO-2 |
| R3 | `fwd <= tol` to `fwd < tol` | MISSED | INFO-2 |

## Findings summary

| Id | Severity | Where |
|----|----------|-------|
| LOW-1 | LOW | `safety_link_frame.h:45-49` stale contract; `safety_link_frame.c:66` stale `diff` |
| LOW-2 | LOW | OLDPOOLA LOW-1 "not contained" is wrong; `uart_protocol.c:281`/`:335`, `safety_link_frames.c:1129` |
| INFO-1 | INFO | `safety_link_frame.c:62-68` new FP at the Pico 2^32 wrap with a late-drained baseline |
| INFO-2 | INFO | `test_safety_link_compile.c:1226-1259` R1/R2/R3 MISSED |
| INFO-3 | INFO | inherent miss when the old boot was `<= tol` old, unchanged |

## Fix status

LOW-1, LOW-2, INFO-1 and INFO-2 fixed (REBOOTFX4): header contract rewritten; `uart_proto_message_t.rx_ms` stamped at both UART receive sites and used as the DIAG timestamp (`safety_link_frames.c`); new host test `test_pico_uptime_uses_receive_time_not_drain_time`; three INFO-2 edge pins added. INFO-3 is inherent, unchanged.
