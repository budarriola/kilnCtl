# Review: REBOOTFX4 (origin/dev 44289aa41) -- DIAG receive-time stamp

Reviewer: opus, 2026-10-12. Scope: `44289aa41` "Pico reboot detector: stamp DIAG
at UART receive (rx_ms), rewrite header contract, pin continuity-window edges
(REBOOTFX3 LOW-1/2, INFO-1/2)".

Counts: HIGH 0, MED 0, LOW 3, INFO 4.

## What changed

- `uart_proto_message_t` gains `uint32_t rx_ms` as its last field
  (`firmware/hwAbstraction/esp/uart/uart_protocol.h:124`, host stub
  `firmware/KilnFW/App/test/stubs/uart_protocol.h:46`).
- Both construction sites in `handle_raw_frame()` stamp it with
  `xTaskGetTickCount() * portTICK_PERIOD_MS` (BROADCAST `uart_protocol.c:286`,
  DATA `:341`).
- `safety_apply_diag()` uses `msg->rx_ms` instead of the drain tick as
  `esp_now_ms` (`safety_link_frames.c:1130`). That value feeds the uptime
  regression detector, `reboot_noted_by_uptime_esp_ms` and
  `pico_uptime_baseline_esp_ms`.
- Header contract rewritten (`safety_link_frame.h:42-50`); new host test
  `test_pico_uptime_uses_receive_time_not_drain_time`; three INFO-2 edge pins.

## Answers to the review questions

**(1) Every construction/copy path sets rx_ms.** Yes, for every path that
reaches `safety_apply_diag()`. The struct is built in exactly two places, both
in `handle_raw_frame()` with designated initializers, and both now stamp it.
`xQueueSend` copies it by value into the inbox, `uart_protocol_receive()`
(`xQueueReceive`, `uart_protocol.c:770`) copies it out, and the only caller of
`safety_apply_diag()` is `safety_drain_inbox_ex()` (`safety_link_inbox.c:313`)
on a message from `uart_protocol_receive()`. The other locals and stashes
(`safety_link_commands.c`, the `stashed_*` members in `safety_link.h:1464-1570`,
the bridge tasks) are filled by whole-struct copies from that same receive call.
No path synthesizes a DIAG with an unset `rx_ms` in firmware. The stamp sites
are not host-compiled, so no test pins them (LOW-3).

**(2) Tick source, units and wrap.** The stamp expression is the same one the
consumer used before (`(uint32_t)(ticks * portTICK_PERIOD_MS)`). The other
`esp_now_ms` comparisons in `safety_apply_diag()` subtract two such stamps, and
the 32-bit tick counter and its ms product wrap at the same point mod 2^32, so
modular subtraction holds across the wrap. The cast differs cosmetically
(`portTICK_PERIOD_MS` is already `TickType_t` in ESP-IDF). One mixed-basis
comparison remains: LOW-1.

**(3) Other drain-time stamps where receive time is meant.**
- `safety_apply_fw_version()` compares the drain time against the now
  receive-stamped `reboot_noted_by_uptime_esp_ms` (LOW-1, a regression from
  this commit).
- `diag_tick` (`safety_link_frames.c:1177`) is still the drain tick. It feeds
  `diag_frame_age_ms` (`safety_link.c:702`) and the relay-authority/executor
  DIAG freshness gate (`relay_authority.h:359`, `profile_executor.c:2340`,
  `SAFETY_LINK_DIAG_FRESH_MS` 4500). That is LOW-2: pre-existing, but now
  directly fixable.

**(4) Struct growth.** 264 -> 268 bytes (4+1+1pad+2+1+253 = 262, padded to
264; `rx_ms` at offset 264). Inbox queues are PSRAM (`xQueueCreateWithCaps`
with `MALLOC_CAP_SPIRAM`, `uart_protocol.c:633`), so +4 B/slot costs no
internal RAM. `SafetyLinkClass` embeds eight `uart_proto_message_t` by value
(`safety_link.h:1464-1570`) inside `static main_boot_ctx_t ctx`
(`main.c:246`, `.bss`): +32 B of DRAM. Each stack-local message grows 4 B
(bridge tasks, `safety_link_commands.c` frames with two locals, the
uart_protocol rx task). That is negligible against measured margins, but the
ELF-based stack and DRAM budget checks were not run in this review (no target
build, per rules). They will see it on the next coordinator full run.

**(5) UnitTestFw copy.** `firmware/UnitTestFw/UnitTest/App/drivers/espInterfaces/uart_protocol.h`
is an independent fork (payload 128, its own `uart_protocol.c`). It does not
build `hwAbstraction` or any `safety_link*` code. `rx_ms` is a RAM-only field,
never on the wire, so nothing changes in the wire format or ABI.
`wire_protocol_fingerprint_check.py` reads only `UART_PROTO_MAX_PAYLOAD`, and
`stub_signature_drift_check.py` skips `uart_protocol.h` and checks prototypes
only. Neither mirrors the struct, so nothing drifts. INFO-3.

**(6) Tests prove what they claim.** Partly. The new receive-time test fails
when the consumer is reverted to the drain tick (N4) and when the stamp is
forced to zero (N6 is also caught, by the older `:3555` LOW-1 pin). The
INFO-2 pins kill their three boundary mutants (N7-N9). A seconds/ms slip at
the consumer (N5) is caught, but only incidentally by the older `:3555` pin,
not by the new test (INFO-1). None of the stamp-site mutants (N1-N3) is
caught (LOW-3).

## Findings

### LOW-1: FW_VERSION note window now mixes a receive stamp with the drain tick (regression)

`safety_link_frames.c:427-429` (`safety_apply_fw_version()`) tests
`drain_now - reboot_noted_by_uptime_esp_ms <= SAFETY_REBOOT_NOTE_WINDOW_MS`
(3000 ms). Since this commit, `reboot_noted_by_uptime_esp_ms` is set from
`msg->rx_ms` (`:1138`), while the left side is still
`xTaskGetTickCount() * portTICK_PERIOD_MS` at FW_VERSION drain. Before, both
were drain ticks of frames taken from the same FIFO in one pass, so an inbox
stall cancelled out. Now the window shrinks by the full inbox latency.

Failure scenario: the Pico reboots, and the inbox stalls (the LOW-2 premise
this commit fixes). The new boot's DIAG is received at t=20000 and its
FW_VERSION at t=20100, both drained at t=23500. The DIAG counts the reboot by
uptime and notes it at 20000. The FW_VERSION then computes 23500-20000=3500 >
3000 and counts the same reboot a second time. That means a second
`pico_reboot_seq` bump, `diag_since_reboot=false`, and
`pico_uptime_baseline_known=false`. That is the double count the firing audit
2 MED-1 window exists to prevent. `heat_enable.c:754-771` now ORs
`reboot_was_tripped` and never downgrades `reboot_fatal_latched`, so the
consequence is a spurious extra classify/hold cycle and a K4 counter reset.
The T3 latch is not lost, hence LOW.

I confirmed this with a scratch host test, not committed: at 44289aa41 it
fails "FW_VERSION must not double count". With the consumer reverted to the
drain tick it passes.

Fix: stamp the FW_VERSION side with its own `msg->rx_ms`. Add a test with
stalled-drain stamps (DIAG rx 20000, FW_VERSION rx 20100, fake tick 23500)
that requires one bump.

### LOW-2: DIAG freshness age is still drain-stamped

`safety_link_frames.c:1177` sets `diag_tick = xTaskGetTickCount()` at drain.
`safety_link.c:702` turns that into `diag_frame_age_ms`, which gates relay
authority (`relay_authority.h:359`), the executor (`profile_executor.c:2340`)
and the dashboard (`dashboard_status_http.c:759`) against
`SAFETY_LINK_DIAG_FRESH_MS` (4500).

Failure scenario: a DIAG that waited 4 s in a stalled inbox is reported with
age 0 when it is drained. The gate is fail-open by the inbox latency: it can
accept a DIAG whose content is up to ~8.5 s old. This is pre-existing and not
introduced here, but `rx_ms` now makes it directly fixable.

Fix: derive `diag_frame_age_ms` from `msg->rx_ms` (store a ms stamp, not a
`TickType_t`) and update `test_tripfx3_lowa...`'s
`diag_tick == 2000u` pin accordingly.

### LOW-3: the receive stamp itself is unpinned; most test DIAG builders leave rx_ms = 0

`uart_protocol.c` is not compiled by any host test. Mutants N1 (BROADCAST site
unstamped), N2 (DATA site unstamped) and N3 (BROADCAST site in seconds) all
survive the full KilnFW host suite.

Separately, only `apply_diag_uptime[_rx]` stamps `rx_ms`
(`test_safety_link_compile.c:1211-1225`). These builders `memset` the message
and leave `rx_ms = 0` whatever `s_fake_tick_count` says:
`drive_one_diag_and_service` (`:3015`), `m4_apply_diag` (`:3035`),
`v3_apply_diag` (`:3121`), `v3_apply_diag_uptime` (`:3163`),
`diag_with_reason` (`:3425`) and `apply_diag_v3` (`:3444`). Tests that advance
the fake tick between those calls no longer advance the DIAG clock. For
example, `:3193-3194` ("A genuine later reboot (outside the 3 s note window)",
`s_fake_tick_count += 10000u`) is now inert for the DIAG side. The test still
passes only because the note was already consumed. Mixed-basis tests like this
are why LOW-1 was not caught.

Failure scenario: a refactor that drops the stamp from the BROADCAST
initializer leaves `rx_ms = 0` on every DIAG. Elapsed is then always 0 and
tol 2 s. A reboot is caught only if the new uptime is <= 2 s at its first
DIAG. The V3 note window (`:1154`, 0-0) is always open, so a boot-id reboot
inside it is suppressed. Every host test still passes.

Fix: make every test DIAG builder default `rx_ms` to the fake tick, as real
receive does. Add a small host-compiled seam (e.g. a static inline
`uart_proto_stamp_rx()` used at both sites) or a source check that both
designated initializers in `handle_raw_frame()` name `.rx_ms`.

### INFO-1: the units contract is pinned only incidentally

N5 (`esp_now_ms = msg->rx_ms / 1000u`) is CAUGHT, but only by the older
`test_safety_link_compile.c:3555` pin ("uptime >= old baseline hides the
reboot", LOW-1 of an earlier review). The new receive-time test
(`:1249-1254`) does not fail on N5. Its stamps are small enough that dividing
them by 1000 still lands inside tolerance. The units contract therefore
depends on a test written for a different purpose. A direct pin would close
this: a large receive gap with an on-schedule uptime, e.g. rx 0 -> 600000 and
uptime +600000, must not count as a reboot.

### INFO-2: stale comments

`safety_link.h:1680` still says `pico_uptime_baseline_esp_ms` is "ESP tick
(ms) when the baseline DIAG was applied". It is now the receive stamp.
`safety_link_frame.c:65-66` still names "a late-drained DIAG" as a reason for
a large `now_ms`. With receive stamps that case is a frozen or late-generated
uptime, not a late drain.

### INFO-3: UnitTestFw keeps its own struct

`firmware/UnitTestFw/UnitTest/App/drivers/espInterfaces/uart_protocol.h:41`
(payload 128, no `rx_ms`) is a separate fork with no safety-link consumer.
`rx_ms` never goes on the wire, so there is no wire or ABI drift and no
mirror check to update.

### INFO-4: "receive" means deframe time

The stamp is taken in `uart_protocol_rx_task` after deframing, not at UART
arrival. Backlog in the UART driver ring buffer still lags it. That lag is
bounded by buffer size at 230400 baud (well under the 2 s base tolerance),
so it is acceptable.

## Negative tests

`tools\negtest.ps1 -Preset kilnfw-host -Parallel 3`, ExpectPattern
`(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`. Each CAUGHT was
checked in its log: the mutant compiled, and a named `FAIL file:line`
assertion failed.

| Mutant | File | Change | Verdict | Evidence |
|---|---|---|---|---|
| N1_bcast_site_unstamped | `uart_protocol.c` | drop `.rx_ms` from BROADCAST initializer | MISSED | 99/99 host executables pass (LOW-3) |
| N2_data_site_unstamped | `uart_protocol.c` | drop `.rx_ms` from DATA initializer | MISSED | 99/99 pass (LOW-3) |
| N3_bcast_site_seconds | `uart_protocol.c` | BROADCAST stamp `/ 1000u` | MISSED | 99/99 pass (LOW-3) |
| N4_consumer_drain_time | `safety_link_frames.c` | `esp_now_ms` from drain tick | CAUGHT | `FAIL test_safety_link_compile.c:1249`, `:1254` |
| N5_consumer_seconds | `safety_link_frames.c` | `msg->rx_ms / 1000u` | CAUGHT | `FAIL :3555` (older pin, INFO-1) |
| N6_consumer_zero | `safety_link_frames.c` | `esp_now_ms = 0u` | CAUGHT | `FAIL :3555` |
| N7_back_strict | `safety_link_frame.c` | `back <= tol` to `<` | CAUGHT | `FAIL :1291` |
| N8_fwd_strict | `safety_link_frame.c` | `fwd <= tol` to `<` | CAUGHT | `FAIL :1295` |
| N9_no_ppm | `safety_link_frame.c` | tol = base only | CAUGHT | `FAIL :1293` |

Every CAUGHT log shows all host executables compiled (no BUILD FAILURES) and
`RUN FAILURES (1)` from a runtime assertion. The baseline passed,
`real_tree_unchanged: true`, and copies were removed. Base 44289aa41.

The LOW-1 scratch test (stalled-drain FW_VERSION double count) was run by hand
in a separate scratch worktree. It fails at 44289aa41 and passes with a
drain-time consumer. It is not committed.

## Host suite

The negtest baseline is the full KilnFW host suite (`build_host_tests.ps1`, no
filter) at 44289aa41: "all 99 host test executables built and passed", plus
18/18 and 97/97 checks.
