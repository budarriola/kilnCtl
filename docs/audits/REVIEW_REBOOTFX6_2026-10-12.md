# Review: REBOOTFX6 (origin/dev afc6a1479, 94b8c3c42, 150e8f176, b5dbb8f60) -- shared rx-stamp helper, 10 ms tick host test

Reviewer: opus, 2026-10-12. Reviewed at origin/dev `50ea87f0b`. Scope is device
firmware behaviour only (owner rule 2026-10-12); tooling and PC-side nits are
left out. Motivating findings: `docs/audits/REVIEW_REBOOTFX5_2026-10-12.md`
LOW-1 (tick-versus-ms units unpinned on host) and LOW-2 (brittle, bypassable
source pin on `uart_protocol.c`).

Counts: HIGH 0, MED 0, LOW 2, INFO 3.

## What changed

- `uart_proto_rx_stamp_ms()` moved to its own header,
  `firmware/hwAbstraction/esp/uart/uart_proto_rx_stamp.h:12-15`. The real
  `uart_protocol.h:119` and the host stub `test/stubs/uart_protocol.h:23` both
  include it, so host and target share one definition.
- `safety_link_get_status()` now computes the DIAG age as
  `uart_proto_rx_stamp_ms(xTaskGetTickCount()) - link->diag_tick`
  (`safety_link.c:705`). That is the same formula as the stamp written into
  `diag_tick` (`safety_link_frames.c:1178`, `link->diag_tick = msg->rx_ms`).
- `uart_proto_message_make()` (`uart_protocol.h:133-145`) is now the only
  constructor for a received `uart_proto_message_t`. Both `handle_raw_frame()`
  sites call it with `xTaskGetTickCount()`: BROADCAST at `uart_protocol.c:281-282`
  and DATA at `:331-332`.
- Host tests:
  - The `test_rx_stamp` source pin now strips comments, requires exactly two
    make calls that are each fed `xTaskGetTickCount()`, and requires no
    hand-built `uart_proto_message_t x = {` initializer.
  - `test_message_make` is new.
  - `kilnctl_host_tests_safety_link_tick10.exe` is new. It rebuilds
    `test_safety_link_compile.c`, which `#include`s the real `safety_link*.c`,
    with `/DportTICK_PERIOD_MS=10u /DKILN_TICK10_ONLY`. Under that build it
    runs only `test_diag_frame_age_tick_period_10`, which receives at
    300 ticks, reads at 500 ticks and expects a 2000 ms age.

## Answers to the review questions

**(1) Correct age on the device across tick rollover, Pico reboot and link drop.**

- **Tick rollover.** Correct. `TickType_t` is 32 bits and the period is 10 ms
  (`CONFIG_FREERTOS_HZ=100`). The stamp and "now" use the same
  `(ticks * 10) mod 2^32` formula. A modulo-2^32 difference is therefore the
  true elapsed ms modulo 2^32, both across the 32-bit tick wrap (497 days) and
  across the ms-stamp wrap (49.7 days). One exception: the result is correct
  only while the true age is below 2^32 ms. See LOW-1.
- **Pico reboot.** Correct. The reboot path `safety_note_pico_reboot_locked()`
  clears `diag_since_reboot` (`safety_link_frames.c:281`), and
  `relay_authority_pico_diag_fresh()` requires it (`relay_authority.h:372`).
  An old-boot DIAG therefore never permits heat, whatever its age, until the
  new boot's DIAG arrives. That DIAG writes `diag_tick` and sets
  `diag_since_reboot` together under the lock (`:1176-1178`). The inbox is
  FIFO, so an old-boot DIAG cannot be drained after the reboot is noted.
- **Link drop.** Correct. Nothing resets `diag_tick` or `diag_ever_received`
  except `safety_link_init()`'s `memset` (`safety_link.c:362`). After a drop the
  age therefore grows from the last DIAG's receive stamp and reads stale after
  4500 ms (fail-closed). A DIAG that sat in the inbox through a stall is aged
  from its receive time, not its drain time, which also errs toward stale.

**(2) Can any path report fresh data that is actually stale?** Only one:

- The 2^32 ms alias in LOW-1. The DIAG age is now a raw 32-bit ms
  difference, so a DIAG that is a multiple of about 49.7 days old reads as
  fresh for a 4.5 s window.
- Every other path errs stale. The receive stamp is never later than the
  drain, `diag_tick` is written only from a DIAG's own `rx_ms`, and the
  reboot and link-drop handling is described above.
- A `TRIPPED` cache is honoured at any age (`relay_authority_pico_tripped()`),
  so the alias can only hide an *absence* of DIAG, never a trip the ESP
  already knows about.

**(3) Does the tick10 test exercise the shipped code?** Yes, for the DIAG
age path:

- The executable compiles the real `safety_link.c`, `safety_link_frames.c`
  and the rest (by `#include`) with period 10.
- It feeds the DIAG through the real `safety_apply_diag()`
  (`apply_diag_uptime_rx`) and reads the age through the real
  `safety_link_get_status()`. The helper is the real header, not a copy.
- The negtests N1 to N4 below are caught by its assertions at
  `test_safety_link_compile.c:3958` and `:3968`.
- It does not cover `uart_proto_message_make()`. `uart_protocol.c` is never
  host-compiled, and `test_message_make` runs only at period 1. See LOW-2.

## Confirmation of the REBOOTFX5 fixes

- **LOW-1 (units unpinned): FIXED** for the DIAG age.
  - REBOOTFX5's M1, M6 and M7 were re-run here as N1, N3 and N2. All three
    are now CAUGHT, by the tick10 executable's assertions.
  - The two spellings of the units formula in the DIAG age and the stamp are
    now one helper.
  - One part is not fixed: the same units error placed inside
    `uart_proto_message_make()` is still MISSED (N6, LOW-2).
- **LOW-2 (brittle/bypassable pin): FIXED** for the cases REBOOTFX5 named.
  - A commented-out stamp can no longer pass. Comments are stripped, and the
    stamp now lives inside the make call, so there is nothing to comment out
    without dropping the call.
  - A hand-built initializer is refused (N8 CAUGHT).
  - A make call fed 0 instead of the tick count is refused (N7 CAUGHT).
  - An equivalent rewrite no longer false-fails, because the pin anchors on
    the call rather than on the `.rx_ms` text.
  - A post-construction overwrite still passes (N9, INFO-1).

## Findings

### LOW-1: DIAG age aliases to "fresh" after 2^32 ms without a DIAG (regression of the SAFETYFX10 MED-1 saturation)

`safety_link.c:705` computes the age as `(uint32_t)(now_ms - diag_tick)`.
Before `b2890bfd5` (REBOOTFX4) it was `safety_elapsed_ms(link->diag_tick)`.
That function computes in 64 bits and saturates at `UINT32_MAX` precisely
so that "a very old age [cannot] read as a fresh one" (its own comment,
`safety_link.c:212-216`, SAFETYFX10 MED-1). The tick delta inside it still
wraps at 2^32 ticks (497 days), but the ms receive stamp wraps 10 times
sooner.

**Effect on the device.** Suppose the link stays up (STATUS frames keep
`link_up` true) but no DIAG is accepted for 2^32 ms, about 49.7 days. Then for
4.5 s in every 49.7-day cycle:

- `diag_frame_age_ms` reads 0 to 4500.
- `relay_authority_pico_diag_fresh()` returns true, which opens
  `relay_authority_on_blocked()`'s DIAG gate (`relay_authority.h:403`).
- The profile executor's `safety_diag_valid` (`profile_executor.c:2339`)
  accepts the 49.7-day-old cached DIAG as OK.

Reaching this needs a Pico or link defect that silences DIAG alone, which is
exactly what the freshness gate exists to catch. The Pico's own hardware
interlock is still authoritative. Severity is LOW, but the direction is
fail-open.

**Fix.** Keep the receive stamp, but bound the age by a saturating clock as
well. Two options:

- Store the DIAG's drain tick alongside `diag_tick` and report
  `safety_elapsed_ms(drain_tick) + (uint32_t)(drain_ms - rx_ms)`, saturating.
  This works because `drain - rx` is always small.
- Or latch a `diag_stale` flag in `safety_link_get_status()` once the age
  exceeds `SAFETY_LINK_DIAG_FRESH_MS`, and clear it only in
  `safety_apply_diag()`.

**Test.** Add a host test that reads at receive + 2^32 ms + 1000 ms and
expects stale.

### LOW-2: `uart_proto_message_make()` units are still untested at period 10 (N6 MISSED)

If `uart_proto_message_make()` stamped raw ticks (`m.rx_ms =
(uint32_t)now_ticks;`), all host executables would pass. On the device, every
received frame's `rx_ms` would then be in 10 ms ticks while `now` is in ms:

- DIAG ages would be wrong. They would be huge, so DIAG reads stale and heat
  is permanently refused (fail-closed).
- The DIAG-uptime reboot detector would see ESP time running 10 times slow
  (`safety_link_frames.c:1129-1133`).
- The FW_VERSION note window would compare mixed bases.

The tick10 executable uses the host stub of `uart_protocol.h`, which has no
`uart_proto_message_make()`. `test_message_make` runs only at period 1, where
ticks equal ms.

**Fix.** Build `test_uart_protocol_link_delegate.c` a second time with
`/DportTICK_PERIOD_MS=10u`, or add an `rx_ms == 3000` check on
`uart_proto_message_make(.., 300)` to a period-10 executable that includes the
real `uart_protocol.h`.

### INFO-1: the pin does not see a post-construction overwrite (N9 MISSED)

`msg.rx_ms = 0u;` added after the DATA-site make call passes every test.
`uart_protocol.c` is not host-compiled, so this can only be caught by text
scanning, for example by refusing any `.rx_ms` assignment in the stripped
source. This is a hypothetical regression; nothing in the current code does
it.

### INFO-2: other inline ms clocks remain, consistently

The ms formula `xTaskGetTickCount() * portTICK_PERIOD_MS` is still spelled
inline at:

- `safety_link_frames.c:554`, `:1098`, `:1191`, `:1195` and `:1302`;
- `safety_link.c:968` and `:973`;
- `danger_mode.c:50`.

None of these is compared against `rx_ms` or `diag_tick`. Each is its own
clock with the same formula, so the units are consistent on target.
`uart_proto_rx_stamp.h`'s claim that "the units formula exists exactly once"
holds only for the stamp and DIAG-age pair.

### INFO-3: mixed line endings in uart_protocol.c

The two new make-call lines are LF inside a CRLF file. This has no device
effect. It is noted only because it made a CRLF `-Find` miss during this
review's negtest setup.

## Negative tests

Command: `tools\negtest.ps1 -Preset kilnfw-host -Mutations <scratch>\muts.json
-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"
-Parallel 2`.

- Ran on worktree `C:\wt\rvrebootfx6_6jcxtl` (`50ea87f0b`).
- The unmutated baseline PASSED (130 s).
- `real_tree_unchanged: true` and `copies_removed: true`. "REAL TREE CHANGED"
  was not printed.
- Every CAUGHT was confirmed from its log as a compiled mutant with a named
  failing assertion. None was a build failure.
- N7 and N8 fail the text-scan assertions, because `uart_protocol.c` itself is
  never host-compiled.

| Mutant | File | Change | Verdict | Evidence |
|---|---|---|---|---|
| N1 helper drops period | uart_proto_rx_stamp.h:14 | `return (uint32_t)ticks;` | CAUGHT | test_safety_link_compile.c:3958 "rx stamp helper multiplies by the tick period", :3968 "DIAG age is 2000 ms" (tick10) |
| N2 age drops period on now | safety_link.c:705 | `(uint32_t)xTaskGetTickCount() - diag_tick` | CAUGHT | :3968 (tick10) |
| N3 age on tick basis | safety_link.c:705 | `safety_elapsed_ms((TickType_t)link->diag_tick)` | CAUGHT | :3968 (tick10) |
| N4 age reversed | safety_link.c:705 | `diag_tick - now` | CAUGHT | :3176, :3182, :3343, :3346, :3947 (period 1) and :3968 (tick10) |
| N5 make stamps zero | uart_protocol.h:142 | `m.rx_ms = 0u;` | CAUGHT | test_uart_protocol_link_delegate.c:303 "message_make stamps rx_ms with the helper" |
| N6 make stamps raw ticks | uart_protocol.h:142 | `m.rx_ms = (uint32_t)now_ticks;` | MISSED | all executables built and passed; period 1 (LOW-2) |
| N7 DATA site fed 0 ticks | uart_protocol.c:332 | `make(.., 0u)` | CAUGHT | test_uart_protocol_link_delegate.c:294 "both receive-path message constructions go through uart_proto_message_make(.., xTaskGetTickCount())" |
| N8 BROADCAST site hand-built | uart_protocol.c:281 | `bmsg = {.device=..., ...}` without rx_ms | CAUGHT | :294 and :295 "no hand-built uart_proto_message_t initializer" |
| N9 DATA rx_ms overwritten | uart_protocol.c:332 | `msg.rx_ms = 0u;` after make | MISSED | all passed (INFO-1) |

Summary: 7 CAUGHT, 2 MISSED. Every mutant of the REBOOTFX5 LOW-1 target (the
DIAG age path) is now caught.
