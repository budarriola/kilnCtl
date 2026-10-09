# K4 safety relay hypothesis — cplval75's "full duty, zero heat" — investigated, not resolved

**Date:** 2026-09-09. **Trigger:** owner hypothesis that K4 (the safety processor's
pilot relay, in series with the whole heat path) was open during the aborted
`cplval75` capture (`docs/audits/cplval75_aborted_executor_panic_2026-09-09.md`),
which would explain zone SSRs actuating (`relay_on` true, masks `0x02`/`0x06`)
while all three zones sat flat at ~31.9 C for 397 s. **No flash, no firing, no
write to the board was performed.** The unacknowledged crash report from that
run was left untouched, as instructed.

## Correction to the investigation's own premise

The task briefing assumed this board has independent relay **feedback** — a
sense line that confirms K4 actually closed, separate from the command. It
does not. `docs/CONTACTOR_FEEDBACK_OPTIONS.md` (2026-09-04, schematic-verified)
states plainly: **"No feedback path exists today from the contactor or any SSR
back to either processor."** K4's second Form-C contact set (pins 3/4/5) is
wired to nothing — same for K1/K2/K3/K5. `relay_owner_is_energized()` (the
value behind `SAFETY_FLAG_RELAY` in every status frame) is a **software
mirror of the GPIO6 command output**, not an independent read of the
armature or the external contactor. So "commanded vs. sensed disagreement"
is not a distinction this board can currently make in hardware — there is
only "commanded," and everything below should be read that way.

## What was measured (live, right now)

```
safety_get_status(): link up; SaftyFW armed (relay_owner not tripped);
  trip_mask 0x0000, warn_mask 0x0000
safety_get_diag(): trip_reason 0, uptime 52770095 ms, flags [calibration_missing]
get_board_state().safety_status.flags = 49 (0x31):
  bit0 LINK_UP(1) bit4 ENABLED(1) bit5 TEMP_VALID(1)
  bit2 ESTOP(0) -- not asserted        bit3 RELAY/K4(0) -- not energized (idle, no run active)
safety_get_commissioning(): commissioned=False, config CRC 64098 (matches live, not stale)
safety_fw_version: config_version=132, config_crc=64098
```

K4 is currently de-energized, which is expected — nothing is firing right
now. The live-right-now finding that matters is `commissioned=False` with
`calibration_missing` set.

## The mechanism this points to

`firmware/SaftyFW/src/tasks/safety_core.c` (~line 1617),
`safety_core_request_enable(enable=true)`:

```c
if (!commissioning_gate_energize_allowed(enable, &cfg_rec)) {
    log_task_log(LOG_LEVEL_WARN, "request_enable",
                 "refused: safety processor not commissioned");
    return false;
}
```

`commissioning_gate_is_commissioned()` (`commissioning_gate.c`) requires BOTH
`!calibration_missing` AND `config_params_all_required_set()`. Right now
`calibration_missing` is true, so **any fresh heat-enable request issued to
this board right now would be refused before it ever reaches
`relay_owner_command_energize()` — K4 would never be asked to close.** This
is a purely software/config gate, unrelated to E-stop wiring or a failed
contactor, and it was not checked by the original `cplval75` audit (its
preflight table checked link-up/trip/warn/thermocouples/ceilings, not the
commissioning flag).

**What I could not establish:** whether `calibration_missing` was already
true at 10:31:23 when `cplval75` started, or became true only from the
concurrent session's commissioning writes that the original audit already
flagged (`safety_config_version` moved 131→132, `config_crc` 13756→64098,
during the same 10-minute window). `get_device_log_json` no longer reaches
back to that window (buffer has wrapped past the panic-reboot since), and no
tool exposes a historical log of the Pico's own `request_enable` accept/
refuse decisions. Circumstantially: zone temperatures never moved from the
very first samples (t=3s) through abort (t=397s) — a "blocked from the
start" explanation fits that shape better than a mid-run flip would (which
would predict some initial heat before the block appeared). But this is
inference from shape, not a direct measurement, and `coupid6` reaching 70 C
earlier proves the board *was* commissioned at some prior point — this
research did not establish when that stopped being true.

## E-stop hypothesis — measured against, not supported

- GPIO9 wiring (`docs/HARDWARE.md` §5, `discrete_pin_policy.h`): active HIGH
  = STOP (button pressed, wire cut, or connector unfitted all read
  identically); active LOW = healthy/closed loop. Compiled default and
  fail-safe polarity (`estop_active_level` = 0, `ACTIVE_HIGH`) was only
  minted as a commissionable parameter (0x0212) the day before, in `3b5ced00`
  (2026-09-08 21:50).
- S7 (E-stop trip) has **no accumulation window** — a genuine assert latches
  immediately. The aborted run's 60 s sidecar (`cplval75_20260909_ambient.tsv`)
  shows `diag_trip_mask 0x0000` at **every** one of its 10 samples across the
  full 10:31:23–10:40:31 span, and `diag_state` stayed `2` (`RELAY_OWNER_STATE_
  ARMED`) throughout — S7 never fired.
- This rules out the straightforward version of the owner's hypothesis (E-stop
  physically asserted, correctly read, tripping S7 and blocking K4) — that
  would have shown up as a latched trip, and none did, then or now
  (`trip_mask 0x0000` still today).
- It does **not** rule out a polarity misconfiguration that makes a **real**
  open loop read as healthy (the dangerous direction: `estop_active_level`
  set to the wrong value would silence S7 while the physical break remains).
  No read-only tool in the current MCP surface exposes the live
  `estop_active_level` value — `safety_get_commissioning()` renders S1/S8/
  S14/S15/tc_source only, and `safety_set_commissioning_fields()` is
  write-only. This is a genuine gap in what could be checked remotely; a
  visual/continuity check of the physical E-stop loop is the only way to
  close it from here.

## Conclusion

**Not proven that K4 is "open" in a hardware-fault sense** — there is no
feedback circuit to prove or disprove an armature or contactor fault, contra
the task's framing. What was found instead is a **live, currently-verified
software gate** (`commissioned=False` / `calibration_missing`) that would
refuse to close K4 for any new heating request issued to this board right
now, and which is a strong candidate — timing-consistent but not
confirmed — for blocking heat throughout `cplval75` too, via ordinary
commissioning-gate refusal rather than a relay or E-stop failure. The
E-stop-polarity variant of the owner's hypothesis is not supported by the
trip-mask history but also could not be fully excluded (live polarity value
unreadable with current tools). The original audit's own leading
hypothesis — mains/SSR/element downstream of K4 — remains untested and
still requires the CT-current check it already recommended.

## Recommended next step (a write, deliberately not performed here)

The one test that would distinguish "commissioning gate refuses K4" from
"K4/contactor/mains fault downstream" cleanly, with no other side effect if
refused, is calling `safety_request_enable(enable=true)` once on the idle
board and reading back `safety_get_diag()`'s WARN log / `SAFETY_FLAG_RELAY`
bit:
- If refused (expected, given `calibration_missing` is true right now): the
  gate confirms itself, no relay moves, nothing energizes — the safe outcome.
- If accepted: K4 **and the physical contactor** would actually close and
  apply mains to the zone SSR outputs for as long as it takes to observe and
  then call `safety_request_enable(enable=false)` — a real actuation with a
  real electrical consequence, which is why this was not run unilaterally.

Recommend the owner either authorize that single request/observe/disable
cycle, or first re-commission the required fields (`safety_set_tc_type` /
`safety_set_commissioning_fields`) to clear `calibration_missing`, then
re-check `safety_get_commissioning()` before the next firing attempt, and
separately walk the physical E-stop loop with a meter to settle the polarity
question this session could not answer remotely.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
