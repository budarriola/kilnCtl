# Audit: thermocouple path (KilnFW + SaftyFW), 2026-10-14

Scope: the KilnFW MAX31856 driver (`drivers/hw/MAX31856.c`, `max31856_codec.c`),
`drivers/owners/thermo_owner.c`, the SPI bus shared with the display, fault bits
and open-TC handling, cold junction, freshness and staleness, and how readings
reach control (`thermo_channel_read.c`, `thermo_combine.c`, `profile_executor.c`,
`thermal_guard.c`), the dashboard (`dashboard_http.c`) and the safety link
context frame (`safety_link_frames.c`). It also covers the SaftyFW read path
(`src/tasks/thermo_task.c`, `max31856.c`, `max31856_live_check.c`).
Code-only. The board was not touched.

Key question: can a stale, faulted or wrong-zone reading be used as valid for
control? Faulted readings cannot: every fault bit that invalidates the TC
value NaNs it, and NaN is never `ok`. A **stale** reading can, and so can a
reading from a part that has lost its configuration (MED-1). A wrong-zone
reading never reaches the ESP's own control loop, which combines channels by
`thermo_mask`. It does reach the Pico's context frame (LOW-1).

HIGH 0, MED 1 (fixed, see Resolution), LOW 3, INFO 5

## Findings

### MED-1: control treats a stale or frozen reading as valid; no runtime check of the part's configuration

Evidence:
- `firmware/KilnFW/App/drivers/control/thermo_channel_read.c:28-29`:
  `out->ok[ci] = !readings[i].spi_failed && !isnan(out->raw_c[ci]) && !fault_bits_bad;`.
  `readings[i].stale` and `readings[i].age_ms` are never consulted.
- The driver does compute both: `drivers/hw/MAX31856.c:1239-1240`
  (`fresh = drdy_known ? drdy_asserted : max31856_result_due(ch); out->stale = !fresh;`)
  and `:1259-1263` (age advances only on a fresh, non-NaN conversion).
- `thermo_combine()` (`profile_executor.c:911`) and the guard input
  `.sensor_ok` (`profile_executor.c:1757`) both take `ok` as-is, and so do
  `autotune_engine.c:855` and `zones_current_sweep_engine.c:905` (the other
  `thermo_channels_read()` callers).
- The only user-facing staleness test is the dashboard's
  `age_ms >= KILN_TEMP_STALE_AGE_MS` (10 s, `dashboard_http.c` around line 200-250).
  The executor never applies it.
- The configuration is verified only at boot (`MAX31856_start_all()` CR1
  read-back, `MAX31856.c` around lines 751-853). Nothing in KilnFW re-reads CR0/CR1 at
  runtime or reconfigures a part that has reset. `grep MAX31856_configure`
  finds only the boot path and the zones POST tc_type change. SaftyFW has
  exactly this check in `max31856_live_check.c`.

Failure scenario: mid-firing, one thermocouple daughterboard browns out or
its ribbon glitches, and the MAX31856 power-on-resets. Its POR defaults are
CR0 = 00h (normally-off, open-circuit detection off) and LTCB = 0. From then on:
- the part never converts again, so ~DRDY stays high and the driver reports
  `stale = true`, with `age_ms` growing;
- every burst decodes as 0.0 C with SR = 0, which is no fault bit, so `ok = true`;
- the PID sees a zone at 0.0 C and drives full duty.

Guard 5 does not trip, because the default `min_temp_c` is -20 C. Guard 6
does not trip either, because `sensor_ok` stays true. Only guards 1/2
(heating-failed/wrong-direction, 120-900 s windows) and guard 7 (frozen, 600 s
at 0.05 C while duty > 0, `thermal_guard.c:32,41,196-203`) eventually stop
the zone. For up to about 10 minutes the zone heats open-loop, and the Pico's
independent TC (S1 abs max) is the only ceiling.

It is worse when a zone's `thermo_mask` combines two channels. The live
channel keeps the averaged value moving by more than the 0.05 C eps, so
guard 7 never trips. The combined reading sits about halfway between the true
temperature and 0 C, and the PID overheats that zone until guards 1/2 or the
Pico act.

The same 0.0 C / no-fault outcome follows from a floating or low MISO at
runtime. The driver's init comment admits that this all-zero burst "decodes as
a plausible 0.0 C with no fault bit set".

Suggested fix:
1. In `thermo_channels_read()`, also require
   `readings[i].age_ms != MAX31856_READING_AGE_UNKNOWN && readings[i].age_ms < THERMO_CONTROL_STALE_MS`.
   Use a few conversion periods, for example 2-3 s, not the dashboard's 10 s.
   A dead channel then goes `!ok`, and guard 6 trips after its 3-tick
   debounce, or `thermo_combine()` drops the channel from the average.
   Use `age_ms`, not the per-read `stale` flag, because other readers can
   consume the DRDY release (LOW-3).
2. Add a periodic CR0/CR1 read-back per channel (modeled on SaftyFW
   `max31856_live_check.c`), and on a mismatch reconfigure or mark the channel
   failed.

Host test: none. No host test drives `thermo_channels_read()` with a stale or
aged reading. `test_profile_executor_prestart.c` and
`test_autotune_engine_prestart.c` stub it.

### LOW-1: the Pico context frame pairs zone i with channel i, not with the zone's `thermo_mask`

Evidence: `firmware/KilnFW/App/drivers/safety/safety_link_frames.c:591-636`.
For each zone `i`, the frame looks up the reading whose `channel == i`
(lines 591-594) and sends `measured_c = reading->tc_temperature_c` (614),
`MEASURED_VALID` (636) and `sample_counter` (621-623) next to zone i's
ACTIVE flag and setpoint. `thermo_mask` is user-configurable from 0 to 0xFF
(`zones_http_post_parse.c:364-381`), and its default is `1 << i`.

Failure scenario: an operator maps zone 0 onto channels 1+2 and leaves
channel 0 unwired or pointed at another location. The Pico's S10
nearest-zone check (WARN) and its S2 eligibility (ACTIVE + MEASURED_VALID)
then compare the wrong probe. In `tc_source` BORROWED/BOTH mode, S13
borrows a temperature from a channel the zone's own control does not use.
The value is also raw and uncalibrated, while control uses `apply_cal()`.
With the default identity mask, nothing goes wrong.

Suggested fix: send the zone's own `thermo_combine()` result (the same
value the executor controls on), or refuse to mark the zone MEASURED_VALID
when `thermo_mask != (1 << i)`.

Host test: none for a non-identity mask.

### LOW-2: the dashboard calibrates by channel index

Evidence: `firmware/KilnFW/App/drivers/http/dashboard_http.c:233`,
`zones_config_apply_cal(r->channel, r->tc_temperature_c)`.
`zones_config_apply_cal()` takes a zone index
(`persist/zones_config_accessors.c:2837`). The comment at line 231 assumes
zone i reads channel i.

Failure scenario: with a non-identity `thermo_mask`, the dashboard shows a
channel corrected with another zone's calibration offset. This affects the
display only; control is not affected.

Suggested fix: label per-channel values raw, or calibrate per zone from the
combined value. Host test: none.

### LOW-3: DRDY freshness is consumed by whichever reader gets there first

Evidence: the burst read releases ~DRDY (`MAX31856.c`, the burst read in
`MAX31856_read()`). The executor, dashboard, LCD, safety-link context frame
and board_temps all call `MAX31856_read_all()` independently, through either
`thermo_owner` or `thermo_channels_read()` directly. The context frame bumps
`sample_counter` only when `!reading->stale` (`safety_link_frames.c:621`).

Failure scenario: when another reader reads a channel shortly before the
safety poll, the poll sees `stale = true` and the Pico's borrowed sample
counter does not advance for that tick. That can produce S13 borrowed-stale
nuisance WARNs. Both tasks use `vTaskDelay`, so their phase drifts, and a
sustained 60 s S13 trip is unlikely. It is also the reason MED-1's fix should
key on `age_ms` rather than `stale`.

Suggested fix: latch freshness in the driver per conversion (a sequence
number per channel), and let consumers compare sequence numbers instead of
sampling DRDY. Host test: none.

### INFO

- INFO-1: `thermo_channels_read()` bypasses `thermo_owner` and calls
  `MAX31856_read_all()` directly. The per-channel mutex (2000 ms) serializes
  the SPI transactions, so this is safe. It still undercuts the owner-task
  design.
- INFO-2: `fault_bits_bad` (`thermo_channel_read.c:28`) uses literals
  `0x01 | 0x02 | 0x40` (OPEN, OVUV, TCRANGE) instead of
  `max31856_fault_invalidates_tc()`. CJRANGE, the fourth bit, already NaNs
  the TC inside the codec, so the result is equivalent today. It could drift
  if the codec changes.
- INFO-3: when the DRDY provider fails, the time-based fallback
  (`max31856_result_due()`) cannot detect a stopped part. This is
  acknowledged in the driver. `fault_pin_asserted` is not used by control.
- INFO-4: the thermo_owner slot pool, its 200 ms result timeout (fail-closed
  as NaN, spi_failed and AGE_UNKNOWN) and its NOT_FOUND paths look sound.
- INFO-5 (SaftyFW): the read path does not share MED-1. In
  `src/tasks/thermo_task.c` around lines 586-611, DRDY silence for more than
  `THERMO_TASK_DRDY_SILENCE_MULTIPLIER` conversion periods sets
  `snap.valid = false` with NaN TC/CJ, which feeds S5 directly. The
  missed-edge recovery reads only when the pin is actually low.
  `max31856_live_check.c` re-verifies the configuration at runtime, and the
  per-type plausibility band downgrades a reading that has drifted out of
  range. The time-based read is only in the bench-only
  `SAFTYFW_THERMO_ASSUME_DRDY` build, which defaults to OFF and prints a loud
  pragma when on. Prior audits (`safety_tc_drdy_stall_2026-09-08.md`,
  `SAFTYFW_GUARD_REVIEW_2026-10-09.md`) already cover this path, and I found
  nothing new.

## Test gaps

- No host test of `thermo_channels_read()`'s `ok` derivation, covering stale,
  aged, fault-bit and spi_failed readings (MED-1).
- No executor or guard test where one channel of a multi-channel
  `thermo_mask` freezes at 0.0 C while the other stays live (the MED-1
  guard 7 bypass).
- No test of the context frame or the dashboard with a non-identity
  `thermo_mask` (LOW-1, LOW-2).

## Resolution

MED-1 fixed (LOW-1 and LOW-2 untouched).

- `thermo_channel_read.c`: `ok` now comes from `thermo_reading_ok()`, which also
  requires `age_ms <= MAX31856_CONTROL_MAX_AGE_MS` (3000 ms) on a channel in
  automatic conversion (`MAX31856Reading.continuous`). 3000 ms is more than 3.8x
  the slowest figure the driver can produce (410 ms steady-state, 785 ms first
  conversion after a reconfigure), so a healthy part never trips it, while a
  frozen part is rejected within 3 s instead of the dashboard's 10 s.
  Keyed on `age_ms`, not `stale`, as the S13 note above advises. Normally-off
  and one-shot channels are exempt: they legitimately go without new results.
- `MAX31856_live_check()` (modeled on SaftyFW's `max31856_live_check.c`),
  called from `MAX31856_read_all()` for each channel: every 5 s
  (`MAX31856_LIVE_CHECK_INTERVAL_MS`) it reads CR0/CR1/MASK in one burst and
  compares with the shadows (self-clearing CR0 bits and reserved MASK bits
  ignored). On mismatch it counts, logs, drops the good-result history (so
  `age_ms` is UNKNOWN and the channel not ok until a fresh post-reconfigure
  conversion) and rewrites CR0 (stopped), CR1, MASK, CR0. It takes only the
  channel's leaf lock; no module lock is held across SPI. No new task, no new
  buffers (3-byte stack array).
- Fail-safe path unchanged: a not-ok channel is dropped by `thermo_combine()`;
  an all-invalid zone gives `sensor_ok=false`, no PID duty, and the debounced
  thermal_guard guard 6 trip. Thresholds untouched. A reset part is invalid for
  up to roughly 3 s (age bound) plus the 5 s check interval, so guard 6 may trip
  on such an event; that is the existing fail-safe behaviour.
- Tests: `test_max31856_hal_spi.c` (`test_reading_ok_staleness`,
  `test_live_check_reconfig`), 106/106. Negtest: 10 mutants, all CAUGHT by a
  compiled build with failing assertions.

### lowfx11 (2026-10-15)

- LOW-1 fixed: the safety context frame takes each zone's measured_c and validity from the zone thermo mask (`thermo_zone_raw_from_readings`, mean of the masked channels), not channel index. Host test: `test_max31856_hal_spi.c` `test_zone_helpers`.
- LOW-2 fixed: the dashboard applies the per-channel calibration only when `thermo_channel_cal_applies` (identity mask); a combined or remapped zone shows the raw value.
- LOW-3: not in scope of this pass.
