# REVIEW_REBOOTFX7 (2026-10-12)

Opus review of the REBOOTFX7 fix commits on origin/dev, `f85ad4678..fe430b9a7`:

- `16cda6d93` safety_link: latch DIAG stale (REBOOTFX6 LOW-1); period-10 `uart_proto_message_*` rx_ms units test (LOW-2)
- `9d18ea771` build_host_tests: obj dir replace for `uart_protocol_tick10`
- `af7b2bf19` check_host_test_verdict: totalExpected 101
- `fe430b9a7` LOW-2 test follows STACKFX1 (`uart_proto_message_init`)

Device bugs only. Result: **no HIGH, no MED, no LOW device findings in range.** One INFO note on
code outside the range.

## LOW-1 fix: DIAG age latch (`safety_link.c` `safety_link_get_status()`)

**Correct.** The latch is written under `state_lock` in `safety_link_get_status()` and cleared under the
same lock in `safety_apply_diag()` (`safety_link_frames.c`), in the same critical section that writes
`diag_tick`. Both halves of the pair (latch, stamp) move together, so the reset-one-side class does not apply.

Every consumer of `diag_frame_age_ms` reads it from `safety_link_get_status()`'s copy, so all of them see the
latched value: `relay_authority_pico_diag_fresh()` (`relay_authority.h`), `profile_executor.c`'s
`safety_diag_valid`, `dashboard_status_http.c`'s `trip_is_test`, and `dashboard_http.c`'s copy. Nothing
else reads `diag_tick`.

### The admitted residual is not reachable on device

The residual needs a DIAG gap with **no** `safety_link_get_status()` call anywhere between age 4.5 s and
2^32 ms (49.7 days; the wrap comes from the ms stamp `tick * 10` at `CONFIG_FREERTOS_HZ=100`).
`safety_poll_task` calls `ct_leak_alarm_service()` on every loop pass, before the `period == 0` branch, and
that service self-throttles to one `safety_link_get_status()` call per 500 ms
(`CT_LEAK_SERVICE_PERIOD_MS`). So the latch is set about 0.5 s after the age passes 4.5 s.

The service returns early only when `s_io` is NULL (no `kiln_io`), and then no relay can be driven. If
`safety_poll_task` itself stalls for 49.7 days, no STATUS is applied either, so `link_up` is false
throughout. Separately, heat-permitting callers (executor tick, start gates) call `get_status()` themselves
before acting.

**Boot before the first DIAG:** `diag_ever_received` is false, so `diag_frame_age_ms` is 0. The latch is
skipped, and `relay_authority_pico_diag_fresh()` and `safety_diag_valid` both require
`diag_ever_received`, so they fail closed. `safety_link_start()` memsets the link, so the latch starts false.

**Pico reboot / re-handshake / boot_id change:** `safety_note_pico_reboot_locked()` does not touch the
latch, and it does not need to. A reboot clears `diag_since_reboot`, which already blocks a permit in
`relay_authority_pico_diag_fresh()`. The new boot's first DIAG clears the latch and restamps `diag_tick` in
one locked step. A latch left set across a reboot only fails closed (it reports UINT32_MAX) until that DIAG.

**Inbox ordering:** a DIAG queued before a latching `get_status()` and applied after it clears the latch.
It also stamps `diag_tick` with its own `rx_ms`, so the next read reports its true age, and that age
re-latches if it is over 4.5 s. No path reports fresh.

**Side effect (cosmetic, not a finding):** while latched and aliased, the dashboard prints
`diag_frame_age_ms` as 4294967295. This happens only after 49.7 days of DIAG silence with STATUS flowing.

## LOW-2 fix: `uart_proto_message_init` rx_ms units at a 10 ms tick

**Correct.** `test_uart_protocol_link_delegate.c` includes the shipped
`firmware/hwAbstraction/esp/uart/uart_protocol.h`. Its `static inline uart_proto_message_init()` calls the
shared `uart_proto_rx_stamp_ms()` (`uart_proto_rx_stamp.h`). The stub `FreeRTOS.h` guards
`portTICK_PERIOD_MS` with `#ifndef`, so `/DportTICK_PERIOD_MS=10u` takes effect. The test asserts
`portTICK_PERIOD_MS == 10u`, so a silently failed `.Replace()` in `build_host_tests.ps1` fails loudly
instead of passing at period 1.

## Negative tests

`tools\negtest.ps1 -Preset kilnfw-host -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`
at `fe430b9a7`. Each CAUGHT below is a mutant that compiled, with a test assertion failing in the log.

Baseline (unmutated) passed. The real tree was unchanged afterwards. Verdict: `ALL_CAUGHT`.

| Mutation | Edit | Verdict | Failing assertion (mutant compiled) |
|---|---|---|---|
| M1 | `safety_link.c`: drop `link->diag_stale_latched = true;` | CAUGHT | `test_safety_link_compile.c:3990` "wrapped age still reads stale" |
| M2 | `safety_link_frames.c`: drop the latch clear in `safety_apply_diag()` | CAUGHT | `test_safety_link_compile.c:3993` "a new DIAG clears the latch and reads fresh", plus `:3182` "age is wrap-safe" |
| M3 | `safety_link.c`: drop the `UINT32_MAX` substitution | CAUGHT | `test_safety_link_compile.c:3990` "wrapped age still reads stale" |
| M4 | `uart_protocol.h`: `m->rx_ms = (uint32_t)now_ticks;` | CAUGHT | `test_uart_protocol_link_delegate.c:315` "stamps 3000 ms, not raw ticks" (`uart_protocol_tick10` exe) |
| M5 | `build_host_tests.ps1`: drop `/DportTICK_PERIOD_MS=10u` from the tick10 build | CAUGHT | `test_uart_protocol_link_delegate.c:314` "built with a 10 ms tick period" and `:315` |

## INFO (outside the range, no action)

`safety_link_send_clear_trip()` and the TEST_TRIP sender (`safety_link_commands.c`) still judge "fresh
DIAG" by `safety_elapsed_ms(link->cached_tick)`, which is the STATUS age, not `diag_tick`. This is the same
substitution F5HOLDERFX4 MED-1 removed from the executor. With STATUS flowing and DIAG silent, the local
refusal can pass on a stale `diag_state`/`diag_trip_mask`.

Impact: this is a command send, not a heat permit. The Pico validates the clear itself, and it is bound by
`trip_seq`/`pico_boot_id` when known, so a stale mask is refused there. It is not a fail-open on heat, so it
is recorded here only.

The STATUS age itself (`safety_elapsed_ms`, a tick delta with saturating 64-bit ms) wraps only at 2^32 ticks
(497 days at 100 Hz), and that is not reachable in practice.
