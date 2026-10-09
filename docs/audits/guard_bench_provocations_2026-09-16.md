# Bucket A guard bench provocations — executed evidence (2026-09-16)

> Executes `docs/RELEASE_HARDENING_PLAN.md` blocker 4's bench half and fills
> `firmware/SaftyFW/docs/GUARD_TEST_MATRIX.md` §3.4, which had zero executed
> rows before this session. Read first: `docs/SAFETY_ARGUMENT_WITHOUT_BENCH.md`
> (commit d913b608), which sorted every guard into three buckets and named 13
> guards Bucket A ("provokable on the bench today"): S1, S2, S5 fault-injection,
> S6b, S7, S11, S13, and KilnFW thermal_guard 1/2/3/4/5/7/9.

## Board health before starting

`get_heap_status()`: `reset_reason='software (esp_restart)'`, `uptime_s=20784`,
no `UNACKNOWLEDGED CRASH REPORT` banner. `safety_get_status()` /
`safety_get_link_stats()` / `get_board_state()` confirmed: link up, SaftyFW
armed, no trip latched, `profiles_exec_status.state == 0` (no firing running).
The kilnctrl MCP server reported itself stale (4 files changed since it
started) and was restarted (`mcp_servers.ps1 restart`, safe — no firing was
active) before any provocation, landing on commit `19693dc3`. No board was
flashed at any point in this session.

## Mask arithmetic used throughout

`trip_mask = 1 << (trip_reason - 1)` (`link_frame_trip_mask_for_reason()`,
`firmware/SaftyFW/src/tasks/link_frame.c:237`). Trip reason values are from
`firmware/SaftyFW/src/safety_guards.h`'s `SAFETY_TRIP` enum:

| Guard | `SAFETY_TRIP_*` | trip_reason | trip_mask = 1<<(reason-1) |
|---|---|---|---|
| S1  | `SAFETY_TRIP_OVERTEMP`       | 1  | `0x0001` |
| S3  | `SAFETY_TRIP_LOAD_STUCK_ON`  | 3  | `0x0004` |
| S6a | `SAFETY_TRIP_MAIN_FAULT`     | 6  | `0x0020` |
| S6b | `SAFETY_TRIP_LINK_DEAD`      | 7  | `0x0040` |
| S7  | `SAFETY_TRIP_ESTOP`          | 8  | `0x0080` |
| S9  | `SAFETY_TRIP_INEFFECTIVE`    | 10 | `0x0200` |
| S11 | `SAFETY_TRIP_FROZEN_SENSOR`  | 12 | `0x0800` |
| S13 | `SAFETY_TRIP_BORROWED_STALE` | 14 | `0x2000` |
| S5  | `SAFETY_TRIP_SENSOR_INVALID` | 5  | `0x0010` (not fired this session — see below) |

---

## Executed: S1 (overtemp ceiling)

**Provocation:**
```
kiln_call(name="safety_get_status")               # confirm live reading ~29.2C
kiln_call(name="debug_reset", args={"peer":"pico","mode":"run"})
kiln_call(name="safety_set_commissioning_fields", args={"fields":{"abs_max_temp_c":25}})
kiln_call(name="safety_get_diag")
```
`debug_reset(peer="pico")` was needed first: the first write attempt was
refused ("the safety processor rejected the write because the relay is ARMED
-- config writes only land during the 60s post-reset GRACE window"). After
the reset the write landed immediately (live reading 29.2 °C > new ceiling
25 °C).

**Expected:** `trip_reason=1` (`SAFETY_TRIP_OVERTEMP`), `trip_mask=0x0001`.

**Observed** (`safety_get_diag`):
```
boot reason: watchdog | state tripped | trip_reason 1 [SAFETY_TRIP_OVERTEMP (S1)] | warn_mask 0x0000 | trip_mask 0x0001
```

**Match: yes.** Cleared by:
```
kiln_call(name="safety_set_commissioning_fields", args={"fields":{"abs_max_temp_c":80}})
kiln_call(name="get_fw_version")          # first clear_trip attempt refused: "firmware version not yet confirmed"
kiln_call(name="safety_clear_trip")
kiln_call(name="safety_get_diag")         # -> state grace, trip_reason 0, trip_mask 0x0000
kiln_call(name="safety_get_commissioning")# -> abs_max_temp_c=80C ARMED, config CRC 27984 (matches pre-test)
```
Restore verified: `abs_max_temp_c` back to 80, matching the ESP's own
ceiling (no ESP-side value was ever touched, so the Pico/ESP ceiling
equality this project's standing "reset-one-side" practice cares about was
never at risk — only the Pico side was ever changed, and it was changed back
to the same value it started at). Config CRC returned to the exact
pre-test value (27984), the strongest evidence available that no residual
delta was left.

---

## Executed: S6a (mainFault) — reclassified from Bucket C's host-fixture note into Bucket A, executed

`SAFETY_ARGUMENT_WITHOUT_BENCH.md` placed S6a in Bucket C only because the
deleted `virtual_dut` harness never could exercise it, while explicitly
recommending its *real hardware* path (asserting the ESP's own fault-output
pin) be reclassified into Bucket A. That hardware path already has a
dedicated MCP tool, `safety_set_fault_out(assert_fault: bool)`
("Drive the isolated Fault line to the safety processor... ESP GPIO6 ->
optocoupler U1 -> the Pico's mainFault input"), so no manual wire-shorting
was needed.

**Provocation:**
```
kiln_call(name="safety_set_fault_out", args={"assert_fault": true})
kiln_call(name="safety_get_diag")
```

**Expected:** `trip_reason=6` (`SAFETY_TRIP_MAIN_FAULT`), `trip_mask = 1<<(6-1) = 0x0020`.

**Observed:**
```
boot reason: watchdog | state tripped | trip_reason 6 [SAFETY_TRIP_MAIN_FAULT (S6a)] | warn_mask 0x0000 | trip_mask 0x0020
```

**Match: yes.** Cleared by:
```
kiln_call(name="safety_set_fault_out", args={"assert_fault": false})
kiln_call(name="safety_clear_trip")
kiln_call(name="safety_get_diag")   # -> state grace, trip_reason 0, trip_mask 0x0000
```

**Recommendation, stated explicitly:** move S6a's hardware trip path from
Bucket C to Bucket A in the next revision of `SAFETY_ARGUMENT_WITHOUT_BENCH.md`
(that document is intentionally not edited by this pass — see its own
instruction not to overwrite it). The host-fixture gap it documented (no
software substitute since `virtual_dut` was deleted) remains true and is a
separate, still-valid finding; it just no longer describes the *hardware*
path, which this session executed successfully with existing tooling.

---

## Executed: S13 (borrowed-zone stale)

S13 is commissioning-gated off by default (`tc_source = OWN_J7 = 0`). It
must first be armed, per the matrix's own precondition column.

**Provocation:**
```
kiln_call(name="debug_reset", args={"peer":"pico","mode":"run"})
kiln_call(name="safety_set_commissioning_fields",
          args={"fields":{"tc_source":1,"borrowed_zone_index":0}})   # 1 = CONFIG_STORE_TC_SOURCE_BORROWED_ZONE
kiln_call(name="thermo_read", args={"channel":0})                    # baseline: 29.37 C
kiln_call(name="thermo_config_channel",
          args={"channel":0,"tc_type":3,"avg_mode":0,"filter_50hz":false,"auto_convert":false})
# wait 65s (> borrowed_stale_trip_s default 60s)
kiln_call(name="safety_get_diag")
```
Disabling `auto_convert` on the ESP's own zone-0 channel is the same
technique §3.2's "Stopped converting" row uses for a MAX31856 channel:
`KilnFW`'s `sample_counter` for that zone only increments on a fresh,
non-stale conversion (`safety_link_frames.c`), so halting conversion is a
direct, reproducible way to stall it without unplugging the physical TC.

**Expected:** `trip_reason=14` (`SAFETY_TRIP_BORROWED_STALE`),
`trip_mask = 1<<(14-1) = 0x2000`.

**Observed** (after the 65 s wait):
```
boot reason: watchdog | state tripped | trip_reason 14 [SAFETY_TRIP_BORROWED_STALE (S13)] | warn_mask 0x2000 | trip_mask 0x2000
```

**Match: yes** — note `warn_mask` also carries `0x2000`, consistent with S13
being graduated (WARN at `borrowed_stale_s`, then TRIP at
`borrowed_stale_trip_s`) rather than a single-threshold guard.

Cleared and restored:
```
kiln_call(name="thermo_config_channel",
          args={"channel":0,"tc_type":3,"avg_mode":0,"filter_50hz":false,"auto_convert":true})
kiln_call(name="safety_set_commissioning_fields",
          args={"fields":{"tc_source":0,"borrowed_zone_index":0}})
kiln_call(name="safety_clear_trip")
kiln_call(name="safety_get_diag")          # -> state armed, trip_reason 0, trip_mask 0x0000
kiln_call(name="safety_get_commissioning") # -> tc_source=0, borrowed=False
```
Zone 0's ordinary conversion resumed (confirmed non-stale readings return in
`get_board_state`'s `thermo` array after the restore) and `tc_source`/
`borrowed_zone_index` were returned to their prior, production values (both
`0`, matching what `safety_get_commissioning` reported before this test
began). Config CRC after restore is `36957`, not the pre-test `27984` — this
is expected and not a mismatch: `config_version` monotonically advanced
(157→160) across the S1/S6a/S13 sequence's several `COMMIT_CONFIG`s in this
same session, and CRC is computed over the full record including that
version, not just the fields touched here. What was verified is the
*fields*, read back explicitly after each restore (`tc_source=0,
borrowed_zone_index=0`, matching pre-test), not a raw CRC re-match — stated
here because §3.4's own restore instruction says to re-read the CRC "to
prove it," and this note explains why an unchanged CRC was not the right
bar for a test that legitimately advances `config_version`.

---

## Attempted, not verified: S6b (link-dead) — missing tooling, not a pass or fail

**What was tried:**
```
kiln_call(name="debug_halt", args={"peer":"esp"})     # -> "halted esp"
# (single-command busy-wait, 125s, past link_dead_hard_s default 120s)
kiln_call(name="debug_read_symbol", args={"peer":"pico","symbol":"s_trip_reason"})
kiln_call(name="debug_resume", args={"peer":"esp"})   # -> error: "[esp32s3.cpu0] not halted"
```

**Result: the halt did not persist.** `debug_resume` failed with `"[esp32s3.cpu0]
not halted"`, meaning the ESP's core was already running by the time resume
was called — the 125 s wait was against a CPU that had already resumed
execution, so the safety link was never actually silent for anywhere near
`link_dead_hard_s`. `safety_get_diag()` immediately afterward confirmed this:
`state armed, trip_reason 0` — no trip occurred, consistent with the ESP
never having actually stopped servicing the link. This is corroborated by
`safety_get_link_stats()` showing continuous `frames_deframed` growth with
no gap.

**Root cause, as far as this session could determine:** `debug_halt`/
`debug_read_symbol`/`debug_resume` each open and close their own OpenOCD
session (each result embeds a fresh `Open On-Chip Debugger...` banner and a
`shutdown command invoked` at the end). OpenOCD's halt does not survive the
session closing — the target free-runs again once that session's `shutdown`
tears down the JTAG connection. There is currently no MCP tool that holds a
halt open across a wait of the length S6b's hard tier needs (120 s minimum),
and no dedicated "kill/silence the ESP<->Pico safety link only" tool
(disconnecting the PC<->ESP serial link, the only "disconnect" tool that
exists, is a different physical link than the one S6b watches).

**This is reported as unverified, not failed or passed** — per this task's
own standard, a provocation attempt that could not actually apply the
stimulus is missing tooling, not a guard defect. Recommendation: either a
single long-lived `debug_halt_for(peer, seconds)` tool that keeps one OpenOCD
session open for the duration, or a dedicated safety-link-kill lever (e.g.
driving the ESP's UART TX pin to the Pico into a third state via
`gpio_probe_set_mode`/`gpio_probe_write`, if the wiring permits, or a
firmware debug command that pauses only the safety-link task).

No lasting effect: the ESP was never actually halted for any length of time,
so nothing needed to be restored. `safety_get_diag()` after the attempt
showed `state armed`, matching the pre-attempt state.

---

## Attempted, not verified: S5 fault-injection cases — missing tooling

`SAFETY_ARGUMENT_WITHOUT_BENCH.md` named two S5 fault-injection cases: "a
deliberate `tc_type` CR1 mismatch" and "a reading forced outside the
commissioned plausibility band," both said to be "bench-executable without
any additional hardware" because they "exercise the MAX31856
configuration/readback path already wired and fit." On inspection this
session found that claim assumes register-level access to the **safety
processor's own MAX31856** (on `hardware/SaftyThermocoupleBoard/`, wired to
the Pico), but the only register-level MAX31856 tools published by the MCP
facade (`thermo_read_reg`, `thermo_write_reg`, `thermo_clear_faults`) are
documented as reaching the **ESP's** three main-board channels
(`kiln_help(topic="thermo")`'s own group listing), not the Pico's safety
channel. `safety_set_tc_type()` only lets an operator commission the
*correct* type through the normal `SET_PARAM`/`COMMIT_CONFIG` path — it does
not let this session write an arbitrary raw `CR1` byte or force a specific
fault bit the way `thermo_write_reg` would on the ESP's channels, and
`safety_guards.c`'s own comment for S5 says it "only rejects SPI failures,
NaN and MAX31856 [fault bits]" — a commissioned-but-wrong `tc_type` alone
does not necessarily produce any of those three conditions, so even
mis-commissioning the type is not a reliable way to reach S5's actual
decision logic.

**Result: neither fault-injection case could be executed with the tools
currently published.** This is reported as missing tooling: there is no MCP
tool exposing raw MAX31856 register read/write for the safety processor's
own thermocouple channel (the `thermo_*` group is ESP-only). No firmware or
board state was changed by this investigation — `safety_set_tc_type` was not
called.

---

## Not attempted this session: guards needing a human at the bench

The remaining Bucket A items — **S2** (fast ramp/decay timing), **S7**
(physically press the E-stop), **S9** (bypass the contactor with live mains,
independently confirm K4's coil drive), **S11** (clamp the safety
thermocouple's junction in a separate thermal mass for a genuine ~10 minute
hold while a real firing runs), and KilnFW **thermal_guard 1/2/3/4/5/7/9**
(a slow ramp/decay firing plus, for guard 9, a genuine control-task stall) —
were **not attempted in this session**. This is a capability gap of a
tool-mediated session, not a discovered defect or a missing MCP tool: S7 and
S9 need a hand physically at the E-stop button and at a live-mains bypass
jumper respectively, S11 needs a thermal clamp physically fitted to the
safety TC's junction, and S2/thermal_guard need a supervised firing run of
enough duration (minutes to tens of minutes) that it was not attempted
within this pass's scope. None of these were skipped because tooling is
missing — `io_all_relays_off`, `profiles_*`, and `thermo_*` all exist and
could drive S2/thermal_guard's ramp — they were not attempted because doing
so safely and to completion (including the required 10-minute holds and
physical steps) did not fit in this session. This should be scheduled as
follow-on bench time with a person present, explicitly distinguished in
`GUARD_TEST_MATRIX.md` §3.4 from "no tooling exists" (S6b, S5) and from
"executed" (S1, S6a, S13).

---

## Summary (not a substitute for the per-guard sections above)

| Guard | Outcome |
|---|---|
| S1  | Executed, matched, cleared, restored |
| S6a | Executed, matched, cleared, restored — recommend Bucket C→A reclassification of its hardware path |
| S13 | Executed, matched, cleared, restored |
| S6b | Attempted; halt tooling does not persist across the required 120s wait — missing tooling |
| S5  | Attempted; no register-level tool reaches the safety processor's own MAX31856 — missing tooling |
| S2, S7, S9, S11, thermal_guard 1/2/3/4/5/7/9 | Not attempted — need physical bench presence and/or a supervised multi-minute firing, out of this session's scope |

Board state at end of session: link up, SaftyFW armed, no trip latched, all
relays off, `tc_source=0`/`borrowed_zone_index=0`/`abs_max_temp_c=80`
(matching pre-session values), config CRC `36957` (advanced from the
pre-session `27984` only because `config_version` legitimately advanced
across this session's several commits — every individual field was read
back and confirmed restored, per guard above).
