# CT commissioning plan — real amps from any probe

Status: opened 2026-09-06. Owner request: user-entered probe rating and
zero offset, an automatic idle-offset measurement, and readings in real
amps regardless of which probe is fitted. Facts below come from
`CURRENT_SENSE.md`, `HARDWARE.md` §6/§9, `current_sense.c`,
`safety_guards.c` and `zones_current_sweep_*.c` as of `05087f0`.

## Does the request make sense?

Mostly yes; three things need adjusting.

1. **Probe rating is already the model.** `current_sense.c:170-196` computes
   `I = (counts - zero_counts) * 3.3/4096 / (gain * sqrt2 * k_ct_v_per_a)`.
   Every probe in this family outputs 1 V at full scale, so
   `k_ct_v_per_a = 1 / A_fs`. The user should type `A_fs` (the number on
   the probe: 1, 20, 50, 100 A); the firmware derives `k_ct`. No new physics,
   only a different unit at the input, and the field must become **editable**
   — today `k_ct_v_per_a[0..2]` and `zero_counts[0..2]` are readonly on
   `safety_commissioning_page.html` (ids 770-778) because the zone sweep
   writes them.
2. **Zero offset in the user's units.** The +59 mV is at the CT output. Through
   the 0.715 front-end gain that is ~42 mV at the ADC, ~52 counts. Let the
   user enter mV-at-probe; store `zero_counts` as now (the Pico's only
   representation). Auto-measurement writes the same field.
3. **The bench probe sits at 7 % of scale; real kilns will not.** At 70 mA
   total the whole kiln is ~50 mV at the ADC ≈ 62 counts above offset, one
   heater ≈ 21 counts. The reference is the Pico's own 3.3 V rail
   (`HARDWARE.md` §6) and no noise floor has ever been measured — the
   16x oversample buys "about 2 bits". So: step 0 is a measured noise floor,
   and everything at this scale is judged against it. On a real kiln (a
   20-50 A probe at 30-60 % of scale) the same firmware has 20-50x the
   signal. Design for the real kiln; verify what the bench can.

Two things the request did not cover but that decide whether the readings
mean anything:

- **One summed CT vs the per-zone design.** S3/S4/S9 read "any current
  present" and work unchanged on a summed CT. S14 (overcurrent) compares
  channel `ch` against `i_normal_a[ch]` — per zone. With a summed probe the
  expected value is the *sum of the normals of the zones currently
  commanded on*, which the Pico knows from relay feedback. The sweep's
  `ct_channel_map` derivation (`GUARD_TEST_MATRIX.md` §3.3: one relay must
  map to exactly one channel) will see all three zones on channel 3 —
  today that is either rejected or produces a nonsense map. A
  `ct_topology` setting (per_zone | summed) is needed, and CURRENT_SENSE.md
  (last reviewed 2026-08-16) still describes three per-zone probes.
- **The owner's 70 mA cannot be checked to better than about ±10 mA with a
  1 A probe** (1 % of full scale is 10 mA). The firmware will report what
  it sees; whether the meter or the probe is right is a bench question.
  Do not chase sub-10 mA disagreements.

## Model

```
A_fs            probe rating, amps at 1 V output          user field, per channel
zero_mv         probe output at zero current (mV)         user field OR auto-measured
gain            front-end divider, 0.715 default          existing, readonly
counts          16x-oversampled ADC mean                  existing
amps_rms = max(0, counts - zero_counts) * (3.3/4096) / (gain * sqrt2) * A_fs
zero_counts = zero_mv/1000 * gain * 4096/3.3
```

`sqrt2` assumes a sinusoidal load (rectified peak envelope, τ = 1 s); true
for resistive heaters on mechanical relays, wrong for phase-chopped SSRs —
document, do not solve.

## Steps

0. **Noise floor** (bench, no heating): with `ct_installed=yes`, all relays
   off, capture raw counts on channel 3 for 60 s at 20 Hz, report mean and
   standard deviation, repeat with the LCD backlight and Wi-Fi active. Store
   the result in `CURRENT_SENSE.md` §4. Decides the smallest detectable step
   (3σ) and therefore whether per-heater open detection is viable on this
   bench. Tool: a PcTools MCP call that reads the existing raw-counts
   diagnostic (add one if only amps are exposed).
1. **Editable calibration fields** (KilnFW page + SaftyFW params):
   `A_fs[3]` and `zero_mv[3]` as ASKED fields on the commissioning page,
   pushed via the existing `SET_PARAM`/`COMMIT_CONFIG` ids for
   `k_ct_v_per_a`/`zero_counts` (conversion on the ESP; Pico storage
   unchanged, so `config_store` needs no migration). A `source` marker per
   field: manual | sweep | auto-zero; manual wins over the sweep. `A_fs`
   must accept **any** probe rating — real kilns will use 10-100 A probes,
   the bench probe is 1 A. Range checks are sanity only: `A_fs` in
   [0.1, 2000], `zero_mv` in [-200, 200]. Nothing may assume 1 A.
2. **Auto idle offset**: a commissioning action, not a background task.
   Preconditions checked by the caller (not `current_sense.c`, per
   `current_sense.h:188-195`): every relay reported off for ≥ 5 s (five
   peak-hold time constants), K4 closed, no trip latched. Then
   `current_sense_recalibrate_zero()` over ≥ 10 s. Refuse and say why if the
   measured zero differs from the stored/nominal by more than 100 mV-at-probe
   — current flowing with every relay off is S3's fault condition and must
   not be calibrated away. Result shown before it is committed.
3. **`ct_topology`**: new commissioning question (per_zone | summed). In
   summed mode the sweep records per-zone normals from channel 3 alone
   (`normal_a[zone] = sum(with zone on) - sum(idle)`), skips the
   channel-map check, and S14 compares channel 3 against the sum of normals
   of commanded-on zones. `i_present_a` auto-derives as half the smallest
   NONZERO zone normal unless set by hand (a zone commissioned at 0 A is
   skipped, never allowed to drive the shared threshold to 0). Add an
   **under-current** warn (open heater: commanded sum minus measured > 0.7x
   that zone's normal for 30 s) — the deficit is one shared-CT scalar
   checked against each commanded zone's own threshold, so it identifies
   "one of the commanded zones," not necessarily every zone whose bit sets.
   WARN-only like S4/S14 until it has been seen on hardware.

   **Pico side: done** (`safety_guards.c`/`.h`, `safety_core.c`,
   `config_store.c`/`.h`, `config_params.c`/`.h` — CT_COMMISSIONING_PLAN.md
   commit). `ct_topology` is param `0x031F` (U8, 0=per_zone/1=summed,
   `CHECK_U8_MAX(1u)`, no `fields_set` gate — per_zone is already the safe
   silent default). New guard **S15** (WARN-only, per zone, `0.7×` that
   zone's `i_normal_a` sustained 30 s, hardcoded not config-exposed) covers
   the under-current/open-heater case; S14 gains a summed-topology branch
   (channel 2 vs. the sum of `i_normal_a[]` for zones commanded on right
   now; channels 0/1 report not-fitted via `amps_valid`). `i_present_a`
   auto-derivation lives in `config_params_finalize_i_present_a()`, called
   at `COMMIT_CONFIG` next to the existing `ct_channel_map` finalizer, gated
   by a new non-`fields_set` marker byte `i_present_a_manual` (fields_set is
   full — see config_store.h). No `KILNLINK_PROTOCOL_VERSION` bump: SET_
   PARAM/GET_PARAM/GET_CONFIG_PAGE are already generic by param id, so a new
   id needs no frame/version change. Host tests: `test/test_safety_guards.c`
   (summed-topology S14 + new S15 cases, quantized-counts negative test) and
   `test/test_config_store.c`/`test/test_config_params.c` (topology pack/
   unpack round-trip, legacy-record decode, `i_present_a` auto-derive).

   **ESP side: pending.** KilnFW needs: a `ct_topology` commissioning
   question on `safety_commissioning_page.html`, forwarding `SET_PARAM`
   0x031F, updating the zones-current-sweep flow to skip the channel-map
   check and derive normals from channel 3 alone in summed mode
   (`GUARD_TEST_MATRIX.md` §3.3), dashboard/LCD display of the summed amps
   plus single-zone attribution, and every KilnFW trip/warn name table
   (`safety_trip_words.h`, `profile_executor.h` and any other place S1-S14
   are named) gaining an S15 entry — none of that lives in SaftyFW and none
   of it was touched by this pass.
4. **Real-amps display**: dashboard and LCD show the summed amps and, in
   summed mode, the per-zone attribution only when exactly one zone is on.
   `safety_get_status` keeps three fields; channels 1-2 report "not fitted"
   rather than 0.00 A.
5. **Docs**: rewrite CURRENT_SENSE.md §0.1/§5 for both topologies; update
   ROADMAP row M (line 75) and `GUARD_TEST_MATRIX.md` §3.3.
6. **Bench** (owner present): steps 0 and 2 on the test kiln, then one
   heating run to record the three zone normals and check the 70 mA figure.

## Order and ownership

Step 0 first (it may make step 3's under-current warn moot on this bench).
Steps 1-2 are independent of 3. All SaftyFW edits wait until the S8 change
(`safety_guards.c/.h`, `test_safety_guards.c`) is committed; `current_sense.c`
and `current_task.c` are free. Host tests must feed quantized counts, not
ideal amps (idealized-input class).
