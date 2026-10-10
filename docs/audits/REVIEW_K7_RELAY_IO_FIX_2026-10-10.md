# Review: K7 relay IO (SX1509) fix batch, 2026-10-10

Reviewer: Opus, review only (no fixes in this commit).
Range: `501bb0d35`, `4a8c7dc35` on origin/dev (fixes K7-01..04 of
`docs/audits/HOST_TEST_CAMPAIGN_FINDINGS_2026-10-09.md`, campaign 7).
Reviewed at dev tip `b752f77e1`.
Files: `firmware/KilnFW/App/drivers/owners/kiln_io.c`, `kiln_io.h`,
`kiln_io_owner.c`, `firmware/KilnFW/App/test/test_kiln_io_sx_fake.c`.

Rule judged against: no path may energise a relay on uncertainty, and the
reported relay state must never claim OFF while the coil may be ON.

## Verdict

The batch is a clear improvement. The ON refusal (K7-03), the post-failure
resync and the verified re-init (K7-04) all do what they claim in the cases the
fake exercises, and all three negative tests below are caught. The rule is
still broken in two narrower paths: a re-init that fails its read-back after
the shadow was zeroed (HIGH), and an ON write that lands just before the bus
dies (MED). The re-init is also now reachable from tasks other than the owner
with no lock (MED).

## Fix status (2026-10-10)

All findings fixed in `bcf448e71` (stack-budget re-pin in `fbf6e1070`), on origin/dev:

- HIGH-1: a failed init/re-init after the relay pins became outputs returns them to inputs (coil de-energised); the shadow is only claimed 0 once that verifies, otherwise it is re-derived from the chip or `relay_state_unknown` is raised.
- MED-1: `relay_state_unknown` (`KILN_IO_FLAG_RELAY_UNKNOWN` 0x04 in `kiln_io_read` flags) refuses ON writes and is asserted as a fault by the profile executor watchdog.
- MED-2: `kiln_io` lock around every public entry; the fail-safe all-off waits a bounded 2000 ms, then does an unlocked OFF write and leaves `relay_state_unknown` raised.
- MED-3: a dead-bus all-off no longer runs the reset/re-init cycle when the chip is unreadable.
- LOW-1: re-init restores user IO, pull-ups, D/C and ~RESET.
- LOW-2: documented. A failed re-init stays not-initialised (fail-safe); recovery is `kiln_io_reset_and_reinit` via `CMD_SX_RESET`, no automatic retry by design.
- LOW-3: documented in the code. The verify path is the real backstop for an unnoticed POR. Known remaining gap: an ON claimed against undriven pins when external pull-ups match.
- LOW-4: relay pins that read as inputs after a failed write are repaired through a re-init.
- LOW-5, NIT-1: `CMD_SX_RESET` goes through `kiln_io_reset_and_reinit`, notes the off-tracker and uses a hard reset when the reset GPIO is wired.
- NIT-2: assertions are unconditional.

Not fixed here: `check_safety_call_results_checked` flags two `profile_executor_relay_io.c` calls and `check_all_task_stack_budgets` flags `http_async_job`; both are on dev independent of this change.

## Findings

### HIGH-1: failed re-init read-back leaves the shadow at 0 while a relay pin is an energised output

`kiln_io.c:252-255` (`kiln_io_reinit`) sets `relay_shadow = 0` and notes it
changed right after the soft reset, then calls `kiln_io_configure_pins()`.
`configure_pins` (`kiln_io.c:188-240`) writes SAFE_DATA, then sets the
direction register (relay pins become outputs), and only then reads back. When
the read-back finds a relay pin high (`kiln_io.c:235-238`) it returns
`ESP_ERR_INVALID_RESPONSE`, and `reinit` returns that error without driving the
pin low again or returning relay pins to inputs.

Failure scenario: an output latch is stuck high (the case
`test_k7_03_reinit_readback_catches_stuck_high_latch` builds), or a concurrent
ON write lands between SAFE_DATA and `set_dir` (see MED-2). The re-init makes
the pin an output, so the coil is energised. The read-back correctly sees it
and the call fails, but `relay_shadow` stays 0 and
`relays_all_off_since_us` was just stamped, so `kiln_io_relays_off_ms()` grows
and every consumer reads "all off". The same path runs inside
`kiln_io_all_relays_off()` (`kiln_io.c:389`, the fail-safe path) and
`CMD_SX_RESET` (`kiln_io_owner.c:570-575`).

Mitigations present: `initialized` stays false, so further ON is refused and
the call reports an error. Missing: on read-back failure, set relay pins back
to inputs (`set_dir` with relay bits 1), or at least write the relay bits low
and resync the shadow from a chip read instead of leaving the zeroed value.
The existing stuck-high test asserts only the error and `!initialized`; it
should also assert that the shadow matches the chip (or that the pins were
returned to inputs).

### MED-1: ON write that lands before the bus dies is reported as OFF (K7-01, dead-bus direction)

`kiln_io.c:163-183` (`kiln_io_resync_relay_shadow`): when the chip cannot be
read and the safe-off write also fails, the shadow keeps its last verified
value (`kiln_io.c:171`).

Failure scenario: the shadow is 0, an ON is commanded, the port write is ACKed
(the driver's `data_shadow` updates at once) and lands on the chip, then the
bus dies before the read-back (`SX1509.c:227-255`). `set_relay_mask` gets an
error and resyncs, but the read fails and the safe-off write fails, so the
shadow stays 0 while the coil is ON. A lost ACK after the data byte gives the
same result. The fake has a `die_after` knob, but no test covers
"ON landed, then the bus is dead".

Note on the remedy: flipping the shadow to a conservative "ON" would be wrong.
A reported OFF keeps the Pico S3 guard ("current present with no relay
commanded recently", fed by `relay_now_mask`, `safety_link_frames.c:491`) and
the ESP H9 CT leak alarm (`ct_leak_alarm_service.c:97` stops evaluating when a
relay is ON) armed, and those guards are what catch a coil stuck on. Recommend
a separate `relay_state_unknown` flag. It should be raised here, cleared only
by a verified chip read, surfaced in `kiln_io_read()`/status, treated as a
fault by the executor, and retried with all-off, while the shadow semantics
for S3/H9 stay unchanged. The unowned-relay stray sweep
(`profile_executor_relay_io.c:1432`) reads the shadow only and misses this
state. The aux fault-drop (`profile_executor_relay_io.c:619`) ORs in
commanded/actuated state and is robust.

### MED-2: `kiln_io_reinit()` runs outside the owner task with no lock

`kiln_io_all_relays_off()` (`kiln_io.c:369-396`) now calls `kiln_io_reinit()`
on any write failure. Its direct callers outside the owner task are documented
exceptions (`kiln_io_owner.h:75-89`): `profile_executor.c:2190`, `2241` and
`2262` (guard 9, FAULT and RETRY in the watchdog task) and `main.c:169`
(`main_kiln_enter_safe_state`). That exception was justified as "one
unconditional write, OFF only". The call now also does a soft reset, rewrites
the whole chip configuration and mutates `initialized`/`relay_shadow` with no
kiln_io-level lock, concurrently with the owner task's read-modify-writes
(`set_relay_mask`, `set_io_dir`, `kiln_io_read`) and `lvgl_port_task`'s
direct `kiln_io_lcd_dc`/`kiln_io_lcd_reset`.

Failure scenario: the owner passes the `initialized` and dir-shadow checks
(`kiln_io.c:340`) before the watchdog's reset. Its ON write then lands after
`configure_pins` writes SAFE_DATA but before `set_dir`. The verify mask
excludes relay bits that are still inputs, so the owner reports OK. The
re-init's `set_dir` then drives the pin, energising the relay, and leads into
HIGH-1: the read-back fails, `all_relays_off` returns an error, and the shadow
says 0. The SX1509 driver's own lock serialises single transfers, not the
multi-step sequence.

Fix direction: take a kiln_io-level mutex around `set_relay_mask`,
`all_relays_off`, `reinit` and the LCD helpers. Or keep the direct path to the
single OFF write and post the re-init to the owner queue.

### MED-3: fail-safe all-off latency on a stuck bus roughly doubles

`kiln_io.c:380-393`. On a hung bus, `SX1509_write_masked` already costs up to
5 x 5 x 1000 ms, about 25 s (`SX1509_TIMEOUT_MS`,
`I2C_WRITE_RETRY_ATTEMPTS`). The failure path then adds a resync chip read, a
safe-off `write_masked` (about 25 s), and a reset plus re-init (several more
seconds), for about 55 s total. The guard 9, FAULT and RETRY callers
(`profile_executor.c:2190-2262`) and `main.c:169` run their next safety step
(`guard9_assert_stale_tick_fault`, `safety_link_set_fault_source`) only after
this returns, so a stuck bus now delays fault reporting to the Pico by about
30 s more than before. Recommend bounding the repair: skip the resync safe-off
and re-init when the first failure was a transport timeout rather than a
verify mismatch, or post the repair to the owner.

### LOW-1: re-init after any transient all-off failure also resets LCD and user IO

`kiln_io.c:242-259`. The soft reset is a POR: LCD DC/RESET and user-configured
IO directions go back to defaults. `configure_pins` restores SAFE_DATA and
`KILN_IO_DIR_MASK`, but not `io_shadow`, the cached `lcd_dc_data`
(`kiln_io.c:501-533`) or user IO direction changes. One transient I2C glitch
during a fault all-off can blank or reset the panel and silently revert IO
configuration. Not a relay hazard, but now triggered more often than the old
code ever reset the expander.

### LOW-2: a failed re-init leaves the board permanently not-initialised

`kiln_io.c:245` and `254`. After a failed re-init, `kiln_io_read`, DRDY and
the LCD helpers all return `ESP_ERR_INVALID_STATE` until `CMD_SX_RESET` or a
reboot, with no automatic retry. This is fail-safe (ON is refused), but a
glitch that has already cleared leaves telemetry and the panel dark. Consider a
bounded retry from the owner task.

### LOW-3: K7-03 refusal checks the driver's dir shadow, not the chip

`kiln_io.c:340`. A POR behind the driver's back leaves the dir shadow at
"outputs", so this check passes and the refusal relies on the verify path
(the write cannot verify, so it errors). The result is still fail-safe (no
false OK), but the refusal's comment overstates what it catches. Document that
the verify path is the real backstop for an unnoticed POR.

### LOW-4: the K7-04 repair may never trigger on real hardware

`test_kiln_io_sx_fake.c` (K7-04 tests, about lines 400-430). In the fake, the
all-off write fails to verify after a POR only because the LCD DC/RESET output
bits read 0 as inputs (`ext_pins` default 0). On the board, a panel RESET
pull-up can make those pins read 1, so verification passes, `all_relays_off`
returns OK, and relay pins stay inputs (safe) while `initialized` stays true.
The next ON is then refused only by the verify path, not repaired. Add a fake
case with `ext_pins` matching the real pull-ups.

### LOW-5: `CMD_SX_RESET` zeroes the shadow without noting the change

`kiln_io_owner.c:570`. The shadow is set to 0 without
`kiln_io_note_relay_shadow_changed()`. If the following `kiln_io_reinit`
(`kiln_io_owner.c:575`) fails at its own reset step (`kiln_io.c:247-250`, which
returns before its own note), `relays_all_off_since_us` is not stamped. So
`kiln_io_relays_off_ms()` keeps returning `UINT32_MAX` (H9 disarmed) while the
shadow says 0. Unlike `CMD_ALL_RELAYS_OFF` (`kiln_io_owner.c:482`), the reset
path also never updates `relay_off_tracker`.

### NIT-1: re-init uses only the soft reset

`kiln_io.c:246` calls `SX1509_reset(io->exp, false)` even when the hard reset
GPIO is wired. A soft reset is two I2C writes and cannot recover a chip whose
I2C state machine is wedged, which is exactly when the repair runs.

### NIT-2: `test_por_then_off_commands_are_honest` can pass vacuously

`test_kiln_io_sx_fake.c`: the assertions after the ON command are conditional
on ON succeeding. If a future change makes ON fail there, the test checks
nothing. Assert the precondition unconditionally.

## Test evidence

- `build_host_tests.ps1` full run at `b752f77e1` in a fresh worktree: Built
  75/80, exit 1. The five failures are all link errors,
  `cfg_fs_status.obj : error LNK2019: unresolved external symbol cfg_fs_get_status`
  (also `cfg_fs_is_available`, `cfg_fs_list`), in `ota_http`, `kiln_io_owner`,
  `kiln_cfg_http`, `adaptive_tune_http_gate` and `iter_tune_http`. These are
  not caused by K7, but `kiln_io_owner` coverage did not run. Newer origin/dev
  (`feda5da10`) also carries unresolved `<<<<<<<` conflict markers in
  `build_host_tests.ps1` around the campaign 10 `$totalExpected` bump.
- `-Only "kiln_io_sx_fake|kiln_io_owner"`: kiln_io_sx_fake 224/224 checks
  passed. kiln_io_owner BUILD FAILED, same link error.
- `tools\negtest.ps1 -Command "build_host_tests.ps1 -OutDir {OUT} -Only kiln_io_sx_fake"`,
  baseline PASS, ALL_CAUGHT:
  - drop the K7-03 dir-shadow ON refusal (`kiln_io.c:340` to `if (0)`):
    CAUGHT (`test_kiln_io_sx_fake.c:324`).
  - configure_pins read-back ignores the relay port level: CAUGHT
    (`test_kiln_io_sx_fake.c:340-341`).
  - all_relays_off skips the K7-04 re-init: CAUGHT
    (`test_kiln_io_sx_fake.c:426-429`).
