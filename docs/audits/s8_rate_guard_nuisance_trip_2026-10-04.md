# S8 rate guard: nuisance-trip headroom, input to the owner decision, 2026-10-04

Source-only note at origin/main de964b1e. Nothing was run on hardware and no
value was changed anywhere. It is input to the open owner decision on the
ROADMAP `S8 sanity rate` row (`ROADMAP.md:67`). Revised the same day after a
live read-back: the question is no longer "20 or 33.3", see below.

## How the guard works

- Threshold `max_rate_c_per_min`, window `rate_window_s` (default 60 s,
  `firmware/SaftyFW/src/safety_guards.c:35`). `0` means never trip
  (`safety_guards.c:703-704`, `:734`).
- The rate is an AVERAGE over one whole window from two endpoint samples:
  `delta_c / elapsed_min` (`safety_guards.c:744-745`). The code comment
  explains why a per-tick derivative is unusable (`:713-722`).
- Only a positive delta can exceed the threshold, so cooling cannot trip it
  (`CONFIG_REFERENCE.md:56`).
- Debounce: two consecutive over-threshold windows are required before the trip
  (`S8_OVER_RATE_STREAK_TO_TRIP 2u`, `safety_guards.c:64`; streak logic
  `:747-755`). So a trip needs the average to exceed the threshold for about
  120 s, and any single window at or below it resets the streak (`:752`).
- No other hysteresis. Windows are back to back, each starting from the reading
  that ended the previous one (`safety_guards.c:764-772`).

## The numbers

| | C/min | C/hr | Source |
|---|---|---|---|
| Armed on the bench now | 33.3 | 2000 | live `safety_get_rate_guard()` read 2026-10-04 (below); also `docs/BENCH_TEST_LOG.md:47` (2026-09-21) |
| Firmware default (compiled record) | 33.3 | 2000 | `firmware/SaftyFW/src/config_store.h:216` `CONFIG_STORE_DEFAULT_MAX_RATE_C_PER_MIN 33.3f`, applied at `config_store.c:1006`; derivation (2 x 999 C/hr / 60) in `config_store.h:205-211` and `firmware/SaftyFW/docs/CONFIG_REFERENCE.md:56` |
| Earlier hand-set bench value, no longer in effect | 20 | 1200 | `ROADMAP.md:67` (live read 2026-09-14), `docs/CONFIG_FILESYSTEM.md:393` (pre-flash reading of 2026-09-14) |

Live read-back, 2026-10-04, firmware `9310367b` / Pico `405d3c54`, tool
`safety_get_rate_guard()`: "S8 max_rate_c_per_min=33.3C/min (ARMED) |
rate_window_s=60". So the bench Pico is armed at 33.3 C/min, which is also the
firmware default; the 20 C/min figure was a hand-set value that has since been
replaced (by whom or when is not recorded in the files read here). Correction to
this note's first draft and to the coordinator's brief: 20 C/min is NOT the
firmware default; the default is 33.3 (`config_store.h:216`).

## Steepest builtin ramp

All 28 builtin schedules are in
`firmware/KilnFW/App/drivers/persist/profiles_builtin_table.inc`. Scanning every
segment whose target is above the previous segment's target (a rising segment):

- Steepest rising segments: two tied 999 C/hr steps in `FSCGB1`
  (`profiles_builtin_table.inc:340`, 1075 C to 1100 C, and `:343`, 1050 C to
  1075 C). 999 C/hr is 16.65 C/min, which is 83% of the armed 1200 C/hr
  (headroom 201 C/hr, about 3.35 C/min) and 50% of the documented 2000 C/hr.
- Next: `03DSFF` 666 C/hr (`:65`, 121 C to 1065 C), then 500 C/hr for several
  (for example `FSNM5` `:449`, `FSHP3` `:429`, `FSHP1` `:409`).
- The 9999 C/hr segments (`:359`, `:376`, `:393`) are crash-cools (target below
  the previous segment's), and S8 ignores falling readings, so they are not a
  basis (`CONFIG_REFERENCE.md:56`).
- So no builtin ramp can exceed either threshold as a SETPOINT rate. Because the
  rate is measured on the thermocouple, not the setpoint, what matters is the
  actual temperature rise, which a schedule only caps from above when the
  controller follows it.

## Measured rates

- Bench fixture: no measured C/min figure exists in `docs/BENCH_TEST_LOG.md`.
  The one rise recorded there is about 25.4 C to 32.6 C over roughly 190 s of
  autotune stepping (`BENCH_TEST_LOG.md:622`), about 2.3 C/min, an order of
  magnitude under either threshold. The fixture is also capped by its 80 C
  ceiling (`BENCH_TEST_LOG.md:599`).
- Real kiln: unknown. No document in the repo records a measured maximum
  legitimate rise rate for a real kiln (the guard's own comment says nobody has
  measured it, `safety_guards.c:705-707`). In particular there is no
  measurement of how fast a cold real kiln heats when first fired at full
  power; a real element at low temperature can in principle exceed the
  scheduled ramp because the controller is not rate limited below the setpoint
  gap, but that is a consideration, not a number.

## Decision now open to the owner

The 20-versus-33.3 nuisance-trip question is moot for the bench: it runs the
default 33.3, which is 2000 C/hr, 2x the steepest builtin ramp, permissive by
construction (`CONFIG_REFERENCE.md:56`, `config_store.h:210-211`). What remains
is whether 33.3 stays acceptable on a real kiln.

1. Keep 33.3 as both the default and the bench value: no nuisance-trip risk
   from any builtin schedule; S8 stays a loose runaway detector (a slow runaway
   below 33.3 C/min would not trip it).
2. Commission a tighter measured value on the real kiln once a measurement
   exists (as `config_store.h:213-215` already advises; 20 C/min is an example
   with 3.35 C/min of margin over the steepest builtin ramp): a tighter
   detector, at the price of the unmeasured nuisance-trip risk on a cold first
   firing noted above.

Either way, the stale "20 C/min armed" wording in `ROADMAP.md` (rows at lines 67
and 78) was corrected in the same commit as this note.
