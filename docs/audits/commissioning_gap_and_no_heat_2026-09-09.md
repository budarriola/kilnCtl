# Commissioning gap and "no heat" — read-only trace and bench checklist

**Date:** 2026-09-09. **No flash, no firing, no autotune, no K4/relay writes, no
config writes were performed for this pass.** Every value below came from
`safety_get_status`, `safety_get_diag`, `safety_get_commissioning`,
`safety_get_fw_version`, `safety_get_ct_cal`, `get_board_state`,
`control_get_zones` and `get_heap_status` — all read-only — plus the firmware
source. Builds on `docs/audits/k4_relay_open_hypothesis_2026-09-09.md`
(`8487d85d`) and `docs/audits/cplval75_aborted_executor_panic_2026-09-09.md`,
which this document does not repeat except where the conclusion changes.

## 1. Read-only board snapshot (right now)

```
safety_get_status():
  link up, SaftyFW armed (relay_owner not tripped)
  safety thermocouple: valid, 36.75 C (CJ 36.66 C)
  currents: ch0 not fitted, ch1 not fitted, ch2 0.00 A ("ct zone: -")
  ct_counts: 16, 17, 59   (idle raw counts -- matches CLAUDE.md's
    "16/17/80" note within normal ADC noise; channel 2 is the summed CT)

safety_get_diag():
  boot reason: power-on, state ARMED, trip_reason 0
  trip_mask 0x0000, warn_mask 0x0000
  uptime 58,058,095 ms, context age 4500 ms, frames ok 22179 / bad 0
  flags: [calibration_missing]        <-- the one flag set

safety_get_commissioning():
  link_up=True, commissioned=False
  config CRC 64098 (matches live, not stale)
  S1 abs_max_temp_c=80C            ARMED
  S8 max_rate_c_per_min=33.3C/min  ARMED
  tc_source=0 (OWN_J7)
  S14 over-current (per channel, WARN only): ch0/ch1/ch2 all DORMANT
    (i_normal_a not measured)
  S15 under-current (per zone, WARN only, ct_topology=summed): z0/z1/z2
    all DORMANT (i_normal_a not measured)
  borrowed=False

safety_get_fw_version():
  Pico c13f8828, built 2026-09-09 04:21:04Z, boot_id=29
  config_version=132, config_crc=0xFA62 (== 64098 decimal, matches live)
  protocol v12 (min compatible v7)

safety_get_ct_cal():
  channel 0: uncalibrated; channel 1: uncalibrated; channel 2: uncalibrated

get_board_state() (relevant fields):
  ESP e8cfe344, built 2026-09-09 14:52:22Z, dirty=false
  thermo: ch0 36.80C, ch1 36.81C, ch2 36.56C -- all status=0, no faults
  io.relays = 0 (all off), safety_status.flags = 49 (0x31):
    bit0 LINK_UP=1, bit2 ESTOP=0, bit3 RELAY/K4=0, bit4 ENABLED=1, bit5 TEMP_VALID=1
  safety_link_stats: crc_errors=0, frame_crc_mismatch=0, frame_resync=0 --
    link integrity is clean, nothing here suggests a corrupted config push

control_get_zones(): 3 zones, all mode 3 (fuzzy PID), max_temp_c=80C on all
  three -- unrelated to the safety-side gate, listed for completeness

get_heap_status(): still shows
  "!!! UNACKNOWLEDGED CRASH REPORT !!! exc_task='profile_executo'
   exc_cause_str='IllegalInstruction' reset_reason='PANIC'"
  -- LEFT UNACKNOWLEDGED, as instructed. uptime_s=10437 now (board has
  rebooted again since the panic; this is a stale-but-unreviewed record,
  not a live crash).
```

K4 itself reads de-energized right now (bit3=0), which is simply correct
for an idle board with no run active -- it is not new evidence either way.

## 2. Commissioning checklist, derived from the code

The gate is `commissioning_gate_is_commissioned()`
(`firmware/SaftyFW/src/commissioning_gate.c`):

```c
if (rec->calibration_missing) return false;
return config_params_all_required_set(rec);
```

`calibration_missing` itself is only ever written in one place
(`link_task.c`'s COMMIT_CONFIG handler): `to_write.calibration_missing =
!config_params_all_required_set(&to_write)`. So in practice there is one
real gate: `config_params_all_required_set()`
(`firmware/SaftyFW/src/config_params.c:912-963`). Its required-bit mask is:

```c
required = SET_TC_SOURCE | SET_BORROWED_ZONE_INDEX | SET_TC_PLACEMENT_MODE |
           SET_ABS_MAX_TEMP_C | SET_MAX_RATE_C_PER_MIN | SET_MAINS_VOLTAGE_V |
           SET_TC_TYPE | SET_CT_INSTALLED;
if (rec->ct_installed != 0) required |= SET_CT_CHANNEL_MAP;   // group bit,
                                                                // derived from
                                                                // all 3 per-
                                                                // channel bits
```

No read-only tool currently exposes the raw `fields_set` bitmap or the raw
values of every one of these params (`safety_get_commissioning()` deliberately
renders only S1/S8/S14/S15/tc_source — see its own header comment in
`mcp_server_safety.py`). The table below marks each required field CONFIRMED
where the render (or another live field) proves it, and UNKNOWN where it does
not — that gap is itself a finding, see section 4.

| param_id | field | required? | current state (this board) | needs current flowing to answer, or knowledge-only? |
|---|---|---|---|---|
| 0x0101 | tc_source | always | **SET, value 0 (OWN_J7)** — confirmed, `safety_get_commissioning()` prints it only when set+reliable | knowledge-only (which sensor feeds the safety processor) |
| 0x0102 | borrowed_zone_index | always, **even when tc_source=OWN_J7 and the value is otherwise meaningless** | UNKNOWN — no read-only tool renders it | knowledge-only (any value 0-2 satisfies the bit when tc_source is not BORROWED_ZONE) |
| 0x0103 | tc_placement_mode | always | UNKNOWN | knowledge-only (where the safety TC physically sits) |
| 0x0104 | abs_max_temp_c | always | **SET, 80C, S1 ARMED** — confirmed | knowledge-only (kiln ceiling, already answered) |
| 0x0204 | max_rate_c_per_min | always | **SET, 33.3C/min, S8 ARMED** — confirmed | knowledge-only (already answered) |
| 0x030E | mains_voltage_v | always | UNKNOWN | knowledge-only (nameplate mains voltage, e.g. 240V) |
| 0x0105 | tc_type | always (joined the mask 2026-08-24) | UNKNOWN — a valid temperature reading (`safety thermocouple: valid`) proves the MAX31856's own CR1 register is configured, but that is a *different* write path from this config_store bit; they can disagree | knowledge-only (read the part number / physically-known TC type, e.g. Type K) |
| 0x0109 | ct_installed | always | **SET, value 1** — inferred: `safety_get_commissioning()`'s S14 block prints per-channel DORMANT lines rather than the "ct_installed=0 -- no CTs fitted" special case, which only happens when the bit is set+reliable and the value is nonzero | knowledge-only (already answered) |
| 0x0106/07/08 | ct_channel_map[0..2] | **only because ct_installed=1** | UNKNOWN, and see the specific hazard in section 2a below | see 2a |

**Ordered action list** (fields that are UNKNOWN and therefore the candidate
blockers, in the order to check/set them):

1. `borrowed_zone_index` (0x0102) — write any valid value (0 is fine, since
   `tc_source=OWN_J7` already) via
   `safety_set_commissioning_fields({"borrowed_zone_index": 0})`.
   Knowledge-only, zero risk, no current needed.
2. `tc_placement_mode` (0x0103) — write the value matching where the safety
   TC actually sits (see `docs/HARDWARE.md`/`CONFIG_REFERENCE.md` section 1
   for the enum). Knowledge-only.
3. `mains_voltage_v` (0x030E) — write the bench's actual mains voltage.
   Knowledge-only (a voltmeter reading of the incoming supply, not the kiln
   under load — this does not require the kiln itself to be powered).
4. `tc_type` (0x0105) — confirm/re-write via `safety_set_tc_type(...)` even
   if it already reads correctly on the wire; the fields_set bit is a
   separate fact from "the MAX31856 currently reports a plausible number."
   Knowledge-only.
5. `ct_channel_map[0..2]` (0x0106/07/08) — see 2a immediately below; this is
   the one entry that may need current flowing, or may not, depending on
   the summed-CT gap this board already has.

None of items 1-4 need mains power, an energized element, or K4 closed —
every one of them is either a static fact the owner already knows or a
voltage-only reading a multimeter across live mains gives (item 3), which is
not the same as commanding the kiln to heat.

### 2a. The `ct_channel_map` gap — a genuine mismatch between the summed-CT
design and the raw commissioning gate

This board's CT is configured `ct_topology=summed` (one physical CT on
GPIO28/channel 2, `+59mV` offset, per CLAUDE.md and `CURRENT_SENSE.md`
section 5.2). `CURRENT_SENSE.md`'s own text says explicitly, of summed mode:

> "the mapping check ... is skipped entirely — there is no per-relay mapping
> to resolve"

But `config_params_all_required_set()` (`config_params.c:959-961`) does not
know about `ct_topology` at all — it only branches on `ct_installed`:

```c
if (rec->ct_installed != 0u) {
    required = (uint16_t)(required | CONFIG_STORE_SET_CT_CHANNEL_MAP);
}
```

So on a `ct_installed=1` board, the three per-channel `CONFIG_STORE_SET_
CT_CHANNEL_MAP_0/1/2` bits are **still required by the raw gate even in
summed mode**, despite the summed workflow's own documentation saying the
mapping question doesn't apply. This is very likely the actual missing bit
today, and it can be satisfied entirely from knowledge, with the kiln
completely unpowered:

```
safety_set_commissioning_fields({
  "ct_channel_map[0]": 2, "ct_channel_map[1]": 2, "ct_channel_map[2]": 2
})
```

(channel index 2 — the summed channel — for all three zones; there is only
one physical CT to name). This does **not** require current flowing: it is
answering "which channel do you read," not "what does that channel read at
full load." The genuine current-flowing step (measuring `i_normal_a` per
zone for S14/S15, see section 5) is a separate, non-blocking step.

**Caveat:** no read-only tool confirms whether these three bits are
currently set or clear on this board, so this is the leading hypothesis, not
a proven fact — see section 4 for how to settle it without a write.

## 3. Confidence on `calibration_missing` explaining the no-K4 refusal

**High confidence that `calibration_missing=true` is real and would refuse a
fresh heat-enable request right now**, traced end to end:

```
safety_core_request_enable(enable=true)          [safety_core.c:1592]
  -> cfg_rec.safety_tc_installed == 0? no (default 1, and TC reads valid) -- passes
  -> commissioning_gate_energize_allowed(true, &cfg_rec)   [commissioning_gate.c]
       -> commissioning_gate_is_commissioned(&cfg_rec)
            -> rec->calibration_missing == true  -> return false immediately
  -> refused, logged "refused: safety processor not commissioned", K4 never reached
```

This is confirmed by direct read right now: `safety_get_diag()` reports the
`calibration_missing` flag live, and `safety_get_commissioning()` reports
`commissioned=False` from the same underlying record. The refusal path is a
single `if` with no other interlock in between — nothing else needs to be
true for this refusal to fire.

**What is not settled**, exactly as the prior audit already noted: whether
`calibration_missing` was already `true` at the moment `cplval75` started
(10:31:23), or flipped true only from the concurrent session's commissioning
writes during that same window (`config_version` 131→132). No tool exposes
a historical log of `request_enable` accept/refuse decisions, and the
device log has already wrapped past that window. This document does not
improve on that uncertainty — it only firms up *what* is currently missing,
not *when* it went missing during `cplval75`.

**What would settle it definitively** (not run here, and only the read half
of it is free of any real-world consequence): read
`config_params_is_set()`/`fields_set` for every required bit directly (see
section 4's proposed tool). If every required bit reads SET and
`calibration_missing` is nonetheless `true`, that is the "two sources of
truth disagree" case `commissioning_gate.h`'s own header comment calls out
by name (a v1→v2 migration or a writer that forgot to recompute the stored
flag) — a firmware bug, not a missing commissioning step. If any required
bit reads CLEAR, that confirms an ordinary incomplete commissioning pass,
which is the far more likely story given `ct_channel_map`'s summed-mode gap
above.

## 4. The `estop_active_level` / config version 131→132 question — still open, same as before

`safety_config_version` moved 131→132 with `config_crc` 13756→64098 during
the `cplval75` run window, written by a concurrent session this audit cannot
identify. **No read-only tool in the current MCP surface exposes the live
value of `estop_active_level` (0x0212) or any of the other UNKNOWN fields in
section 2's table** — `safety_get_commissioning()` renders a curated subset
(S1/S8/S14/S15/tc_source only, per its own docstring), and
`safety_set_commissioning_fields()` is write-only.

This is a real, fixable tooling gap, not a hardware limitation: the
underlying `GET /api/safety/commissioning` HTTP response already carries
every param's `value`/`set` pair (`mcp_server_safety.py`'s `numeric()`
helper reads exactly this dict for the fields it does choose to print) — the
data exists, it is simply filtered out of the curated text. Two ways to
close this gap without writing anything to the board:

1. **Cheapest:** extend `_describe_commissioning()`
   (`tools/PcTools/src/kilnctrl/mcp_server_safety.py`, the function whose
   S1/S8/S14/S15/tc_source lines are quoted throughout this document) to
   also print every other id's raw `value`/`set` pair — this is a pure
   read-side formatting change, no new wire traffic, no risk. This would
   answer section 2's every UNKNOWN in one call.
2. If (1) is not done before the next bench session: a **physical
   continuity check of the E-stop loop with a meter** is the only way to
   settle the polarity question from outside the firmware entirely, as the
   prior audit already recommended.

Because `estop_active_level` is deliberately **excluded** from
`config_params_all_required_set()`'s required mask (`config_params.c`'s own
comment: "making this an ASKED question would leave every existing board
uncommissionable" — its safe default, 0/ACTIVE_HIGH, is indistinguishable
from "never answered"), a change to this field during the concurrent
session's writes would **not** explain `calibration_missing` — the two are
unrelated bits. It could still be a real, separate hazard (wrong polarity
silencing a genuine E-stop assert), just not the cause of today's specific
refusal. `trip_mask` reading `0x0000` at every sample this session and last
is consistent with no E-stop assertion either way, polarity notwithstanding.

## 5. Guards dormant for want of calibration

`safety_get_commissioning()` reports, live, right now:

```
S14 over-current (WARN only, per channel): ch0 DORMANT; ch1 DORMANT; ch2 DORMANT
  (i_normal_a not measured, all three)
S15 under-current (WARN only, per zone, ct_topology=summed): z0 DORMANT;
  z1 DORMANT; z2 DORMANT (i_normal_a not measured, all three)
```

Both read the same underlying gate in `safety_guards.c`: a channel/zone is
**skipped entirely** (no accumulation, no warn, never a false pass) unless
`cfg->i_normal_valid[ch]` is true and `cfg->i_normal_a[ch] > 0.0f`. Per
`config_params_finalize_i_present_a()`'s comment and `i_normal_a[0..2]`'s own
`fields_set` gating (`CONFIG_STORE_SET_I_NORMAL_A_0/1/2`), that value is only
ever written by:

- `safety_set_commissioning_fields({"i_normal_a[N]": <measured amps>})`
  (manual entry), or
- the zones-page "measure normal current" sweep referenced in
  `COMMISSIONING_UX.md` section 3.3.

**This step genuinely requires current actually flowing** — `i_normal_a` is
by definition "what does this zone draw at its normal commanded state,"
which cannot be known without energizing that zone's relay with mains
present. This is exactly the class of step the task asked to flag rather
than perform: **arming S14/S15 requires a real heating step, with K4 closed
and at least one zone relay commanded on, measured through the CT.** It is
independent of the `calibration_missing`/commissioning-gate blocker above —
S14/S15 stay WARN-only and dormant even on a board that clears
`calibration_missing` and gets K4 to close; they simply provide no
protection until this measurement is taken. Unlike `ct_channel_map`
(section 2a), there is no way to satisfy this one from knowledge alone.

## Recommended order for the owner

1. Read-only, no owner action needed: this document's section 2 table
   already narrows the candidate blockers to five fields, four of which are
   answerable from knowledge alone right now with zero electrical risk
   (`borrowed_zone_index`, `tc_placement_mode`, `mains_voltage_v`, `tc_type`
   re-confirm).
2. Also from knowledge alone, with the kiln unpowered: write
   `ct_channel_map[0..2] = 2` (all three, the summed channel) per section 2a
   — the leading candidate for the actual missing bit today.
3. After 1-2, read back `safety_get_commissioning()`
   (`commissioned=` should flip to `True`) and `safety_get_diag()`'s flags
   (should drop `calibration_missing`) before touching anything else.
4. **Do not yet call `safety_request_enable(enable=true)`** on this session's
   own authority — that is a real K4/contactor actuation with mains
   consequence, exactly as the prior audit flagged, and it was intentionally
   not run here either. Once `commissioned=True` is confirmed by read-back,
   that single-request/observe/disable test (previous audit's Recommended
   Next Step) becomes the correct next action, ONLY with owner
   authorization, and only then does measuring `i_normal_a` per zone
   (section 5, genuinely requires live current) make sense to schedule.
5. Separately, and not blocking on the above: walk the physical E-stop loop
   with a meter, or land the one-line `_describe_commissioning()` extension
   in section 4, to close the `estop_active_level` visibility gap before the
   next K4 actuation of any kind.
6. The unacknowledged `profile_executor` crash report remains
   **untouched** — `capability_preflight` will keep refusing runs until a
   human reviews it. Nothing in this pass required or performed that
   review.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
