# Safety TC invalid vs warn_mask disagreement — 2026-09-08

## Live state at time of investigation (post-24090c9a flash, uptime 230s)

```
safety_get_status:
  link up | safety TC invalid | currents 0.00 A, 0.00 A, 0.00 A | ct zone: -
  | 340 ms old | tx_dropped 0 | ct_counts 17, 17, 59

safety_get_diag:
  boot reason: power-on | state tripped | trip_reason 5 | warn_mask 0x0010
  | trip_mask 0x0010 | uptime 230006 ms | context age 400 ms
  | context frames ok 455, bad 0 | tx frames dropped 0

safety_get_fw_version:
  Pico build 24090c9a, built 2026-09-08 23:12:11Z, boot_id=85,
  config_version=121, config_crc=0x61D2 (commissioned), protocol v12

safety_get_link_stats:
  sent 3403, received 3402, crc/framing errors 1, timeouts 4,
  diag applied 850, power applied 850, cmd histogram status=3402 diag=850
  power=850 trip_event=6 ct_cal=2

safety_get_commissioning:
  commissioned=True, config CRC 25042 matches live (not stale)
  S1 abs_max_temp_c=80C ARMED, S8 max_rate=33.3C/min ARMED
  tc_source=0 (CONFIG_STORE_TC_SOURCE_OWN_J7)
  S14/S15 dormant (no CTs fitted)

/api/status (ESP): safety_temp_c and safety_ready were 28.56/true earlier
today (per owner report); not reproduced at time of this check.
```

At the time of this check **warn_mask and trip_mask both read 0x0010 (S5),
and the status text agrees ("safety TC invalid")** — the two sources are NOT
currently disagreeing. The earlier disagreement (warn_mask 0x0000 alongside
"safety TC invalid") did not reproduce on this boot.

## Why they can disagree — two different sources, no shared contract

- `safety_get_status()`'s "safety TC invalid" text comes from the **STATUS
  frame's `temp_valid` bit** (`devices_safety.py:357-360`), which is a direct,
  un-debounced mirror of `thermo_task.c`'s per-tick snapshot (`link_task.c`
  `link_task_send_status()`: `temp_valid = th_present && th.valid`).
- `warn_mask`/`trip_mask` in `safety_get_diag()` come from the **DIAG frame**,
  sourced from `safety_core_get_diag_status()` → `safety_guards_warn_mask()`
  (`safety_guards.c`). S5 ("safety TC invalid") is **graduated**: it requires
  `BAD_READ_COUNT_DEFAULT` (10) bad reads within `BAD_READ_TIME_S_DEFAULT`
  (5.0s) before it warns/trips, and once tripped it **latches permanently**
  (`safety_guards.c` ~line 562: "a PERMANENTLY [latched]" trip by design).

So `temp_valid` (STATUS) can flip to false on the very first bad read, while
`warn_mask` bit 4 (DIAG) only catches up 5-10s later once the graduated
threshold is crossed. A snapshot taken inside that ~5-10s window will show
exactly what was reported: TC-invalid text with warn_mask still 0x0000. This
is a real gap between two independently-derived mirrors of the same
underlying fact (STATUS frame vs DIAG frame, both built in `link_task.c` from
different state), matching the project's "reset one side of a pair" class —
here it's "debounce one side of a pair" rather than a reset, but the root
cause is structurally the same: nothing ties the two frames' TC-validity
representations together, so they can transiently disagree with no code path
enforcing consistency. It is not data corruption; it resolves itself once S5
either latches or clears.

## TC vs cold-junction verdict

Cannot be determined conclusively over the wire as currently reported: when
`th.valid` is false, `link_task_send_status()` transmits `tc_c`/`cj_c` as NaN
for **both** channels regardless of which one is actually finite
(`link_task.c:774-776`), so a "chip alive, probe dead" case (real CJ, NaN TC)
and a "chip dead" case (NaN/NaN) are indistinguishable on the wire.

`thermo_task.c` (SaftyFW) has two distinct invalidation paths that both
collapse to the same "safety TC invalid" text:
1. **DRDY silence** (part not converting) — `snap.valid=false`,
   `cj_c=NaN` explicitly (no real reading was ever taken).
2. **CR1-verify failure** — a *successful* SPI transfer with real, finite
   `tc_c`/`cj_c` is downgraded to `valid=false` post-hoc if
   `max31856_tc_type_verified()` is false, i.e. the last `configure()` call's
   CR1 readback didn't confirm the commissioned type stuck
   (`thermo_task.c:506-518`). This matches the project's known "safety TC
   invalid is one CR1 byte" failure mode — NaN on the wire is the symptom,
   not proof the chip itself is dead.

`fault_status` (the MAX31856 SR/MASK fault bits) **is** transmitted
independently of `temp_valid` (`link_task.c:777`: `fault_bits` is read
unconditionally), but `mcp_server_safety.safety_get_status()` never surfaced
it — so an operator reading only the tool's text had no way to tell these
cases apart. **Fixed below.** No SWD access was available in this session to
read `s_reconfig_retries`/`s_reconfig_gave_up` directly, so it cannot be said
with certainty which of the two paths is currently active; the fix at least
lets a future check distinguish "real fault bits set" (probe/wiring fault)
from "no fault bits, still invalid" (CR1-verify or DRDY-silence — chip-level
question, needs the SWD-only counters or a bench continuity check on J7).

## A/B migration (24090c9a) — TC fields preserved?

`24090c9a` ("SaftyFW config_store: A/B sectors close the zero-valid-copies
erase window") is a **pure storage-layout change** (adds sector B, switches
write target instead of erasing in place). Its own commit message states
"Sector A keeps its original offset/format, so an existing board's on-flash
image loads unchanged with no migration step," and its test suite explicitly
covers a legacy-single-sector migration fixture (169/169 passing, up from
60). `tc_source=0` (OWN_J7) read back live is the documented default and is
consistent with a board never commissioned to borrowed/both — **no evidence
of a lost or defaulted TC field from this migration.** This commit is not the
root cause.

## Fix applied

`tools/PcTools/src/kilnctrl/devices_safety.py`: `SafetyStatus.describe()` now
appends the MAX31856 fault labels (or "no MAX31856 fault bits set") to the
"safety TC invalid" text, e.g. `safety TC invalid (open circuit (no
thermocouple?))`, so a plain `safety_get_status()` call carries enough
information to start telling a real probe/wiring fault apart from a
CR1-verify/DRDY-silence software-side invalidation, without needing a raw
register read. 33 existing safety-status/GUI tests still pass unchanged
(`tools/PcTools/tests/test_safety_status.py`,
`test_ct_fitted_display.py`, `test_safety_tc_borrowed.py`,
`test_gui_safety_summary.py`).

## Root cause / verdict

- The warn_mask-vs-text disagreement observed earlier today was almost
  certainly the STATUS-frame/DIAG-frame debounce window described above, not
  corruption and not a migration defect — it self-resolves once S5 latches
  or the read streak clears.
- The underlying "safety TC invalid" condition itself, currently latched
  (trip_reason 5, non-clearable per S5's design), still needs physical
  attention: check the J7 connector/probe wiring on the safety processor's
  MAX31856, and if fault bits now show a real MAX31856 fault (open circuit,
  short, etc. — visible via the fix above), that names the physical fault
  directly. If fault bits still read clear, the CR1-verify/DRDY-silence path
  is more likely and needs an SWD session to read
  `s_reconfig_retries`/`s_reconfig_gave_up` to confirm.
- `safety_clear_trip()` was intentionally NOT called — S5 is a permanent
  latch by design and clearing it without fixing the physical sensor would
  just mask the condition.

## Reflash needed?

No. The fix is PC-side tooling only (`devices_safety.py`), does not touch
firmware. No SaftyFW or KilnFW rebuild/reflash required.
