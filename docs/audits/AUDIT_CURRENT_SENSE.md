# Audit: CT current-sense path (SaftyFW + KilnFW), 2026-10-14

Scope: SaftyFW CT ADC sampling (GPIO28 and siblings), counts-to-amps math,
calibration (`k_ct_v_per_a`, `gain`, `zero_counts`, `ct_cal`), the
`ct_installed` / `ct_topology` / `ct_channel_map` / `zone_ct_channel` /
`i_normal_a` presence branches, the guards that read current (S3, S4, S6b
soft tier, S9, S11 arming, S14, S15), the ESP-side H9 CT leak alarm, and the
ESP sweep/config path that writes these parameters. Code-only, base
`origin/dev` at 4a0080fa1. The bench board was not touched.

Prior audits checked so that fixed items are not reported again:
`ct0_not_fitted_2026-09-09`, `ct_topology_stale_cache_2026-09-19`,
`ct_unfitted_channel_s3_floor_margin_2026-09-18` and
`s13_s14_s15_current_guard_arming_2026-09-10`. None of them covers the
split-map finding below.

HIGH 1, MED 0, LOW 1, INFO 5

## Findings

### HIGH-1: a committed split `zone_ct_channel` map blinds S3, S9, S6b-soft, S11 arming, S14/S15 and H9 on channels 0/1

**Evidence**

- `firmware/SaftyFW/src/config_params.c:593-595` and `:690-692`: each zone's
  `zone_ct_channel` accepts any value 0-2. Every split map is accepted:
  `{0,0,1}`, `{0,1,1}`, `{0,0,0}`, `{1,1,1}`.
- `firmware/KilnFW/App/drivers/http/safety_commissioning_page.html:756-770`:
  the commissioning page offers these selectors. Its help text says a
  genuine split "is now expressible too".
- `firmware/SaftyFW/src/config_store_flash.c:1492,1699`: on every commit,
  `config_store_backfill_legacy_ct_topology()` rewrites `ct_topology`. Any
  non-identity map becomes `SUMMED`. This is pinned by
  `test_config_store_zone_ct_channel.c:434-470`.
- `firmware/SaftyFW/src/config_store.h:561-571`:
  `config_store_ct_channel_fitted()` is still keyed on `ct_topology` alone.
  Under `SUMMED` only channel 2 counts as fitted.
- The map-aware predicate `config_store_ct_channel_fitted_map()` exists at
  `config_store.h:514-521`, but nothing in `src/` calls it (tests only).
  `docs/CT_CHANNEL_MASK.md:33-36` says the fitted predicate was supposed to
  become "exists z: zone_ct_channel[z] == ch". That swap never happened.
- These consumers all go through the topology-keyed predicate:
  - `config_store_mask_current_present_to_fitted()` (`config_store.h:1089-1100`),
    called from:
    - `safety_core.c:1254-1257`: `any_current_present`, which feeds S3, S9,
      the S6b soft tier and `.heat_commanded` for S11 at `safety_core.c:1388`;
    - `current_task.c:188-189`: `current_task_any_current_present()`.
  - `safety_core.c:1289`: `amps_valid_for_ct[ch]` is never set for
    channels 0/1.
  - `config_store.h:1057`: `config_store_current_sensing_commissioned()`
    (the S9 commissioned flag) looks only at `k_ct_v_per_a[2]`.
- The ESP H9 alarm reads the back-filled `ct_topology` (param 0x031F,
  refetched from the Pico):
  `ct_leak_alarm_service.c:62-63` and `ct_leak_alarm.c:20-24`. With
  `summed=true` it evaluates channel 2 only.

**Failure scenario.** A kiln has two shared CTs. Zones 0 and 1 are on the
channel 0 clamp and zone 2 is on the channel 1 clamp. The operator answers
the selectors 0, 0, 1, exactly as the page invites. The sweep calibrates
`k_ct` for channels 0 and 1 (`zone_sweep_plan_k_ct_mapped()`,
`zones_current_sweep_task.c:1011`). The commit back-fills `ct_topology` to
SUMMED, which has these effects:

- Every real current reading is masked out of `any_current_present`, and the
  only channel left in it (channel 2) has no CT.
  - S3 (current with nothing commanded, e.g. a welded SSR) can never fire.
  - S9 (welded K4 or ineffective heat) can never fire. Its commissioned flag
    reads `k_ct[2]`, which is 0, so it is warn-only as well.
  - S11 is never armed off current, and the S6b soft tier never sees heat.
- The generalised S14/S15 arm (`safety_guards.c:1178-1261`) was built for
  exactly this map, but it is inert: `amps_valid[0/1]` is always false.
- H9 on the ESP also checks only channel 2.

The same happens for a single shared CT on jack 0 or 1 (`{0,0,0}` or
`{1,1,1}`). There, every current guard is blind with no warning. Readiness
does not catch it either, because it only counts unset params and every
param is set.

The opposite case also fails. If the backfill stopped forcing SUMMED but the
predicate stayed the same, a PER_ZONE value with map `{0,0,1}` would treat
unwired channel 2 as fitted. With `k_ct[2]==0` that channel uses the 25-count
fallback margin (`current_presence_policy.c:30`) against a measured ~16-17
count idle floor, which risks a false S3 trip.

**Suggested fix.** Change the body of `config_store_ct_channel_fitted()`
(the documented seam, `config_store.h:551-559`) to delegate to the
map-aware form `config_store_ct_channel_fitted_map()` whenever the
`CONFIG_STORE_SET_ZONE_CT_CHANNEL` group bit is set, and use the topology
rule only when it is clear. That means passing the record, or the effective
map plus the group bit, instead of the raw `ct_topology` byte. The callers
to update are the safety_core S14 loop, both masking call sites and the
commissioned flag. `current_task.c:283` would cache the effective map
alongside `s_ct_topology`.

On the ESP, H9's `channel_relevant()` should be driven by the committed map
(params 0x0320-0x0322) when all three entries are set. This widens what the
guards see; it disables nothing.

**Host test.** None for the end-to-end path.

- `test_safety_guards.c:2044` (`test_s14_s15_two_ct_split`) sets
  `in->amps_valid[]` directly, so it passes even though
  `safety_core_build_input()` never produces those inputs for a split map.
- `test_config_store_ct_channel_fitted.c:140-170` tests masking only for the
  legacy topologies.

## LOW

### LOW-1: no lower bound on `k_ct_v_per_a` or `gain`; a typo inflates amps and zeroes the presence threshold

**Evidence**

- `config_params.c:545-550` checks `k_ct_v_per_a[0..2]` and `gain[0..2]` for
  finiteness only (`CHECK_F32_FINITE`). The comment at `config_params.c:359-363`
  says this is deliberate.
- `current_sense.c:271-290`: amps = `v_adc / (gain * sqrt2 * k_ct)`.
- `current_presence_policy.c:19`: the presence threshold scales with
  `k_ct * gain`.

**Failure scenario.** A manual `k_ct` entered off by a unit, e.g. 0.0001
instead of 0.1:

- The presence threshold shrinks 1000x. Any count above `zero_counts` reads
  as current, so idle noise gives false S3 trips. These trip safe and are
  clearable, but are confusing.
- Amps read 1000x high, so S14 warns.

A denormal positive `k_ct` or `gain` makes `amps` overflow to `+inf`:

- `snap.amps` = inf, so S14 warns, which is the safe direction.
- The reporting filter at `current_sense.c:409-414` then computes
  `inf - inf = NaN` on its second pass. `s_filtered_amps[n]` stays NaN until
  reboot, and the `i_conducting_a` and power telemetry go dead silently.

No guard is silenced, so this is LOW.

**Suggested fix.** Reject `k_ct_v_per_a` (when non-zero) and `gain` below a
physical floor, e.g. 1e-4 V/A, mirroring the `ct_cal` gain bounds at
`config_params.c:397-432`. Also reseed the filter when it is non-finite.

**Host test.** None for small finite values.

## INFO

- **INFO-1: unlocked cross-core copy of `s_cal`.**
  - `current_sense.c:143-150`: `s_cal` is struct-copied without a lock from
    `link_task` (core `SAFTYFW_CORE_LINK_PATH`, `link_task.c:2246,2901,3056`).
    `current_task` reads it on the trip-path core (`current_task.c:167`).
  - A publish that races a commit can mix old and new per-channel fields for
    one 50 ms snapshot. Every field is an aligned 32-bit word, so no single
    value tears.
  - S3 is debounced by `stuck_on_time_s` (`safety_guards.c:1089-1091`) and S9
    needs a 3-tick streak, so a one-tick mix cannot trip either guard.
  - A critical section around the copy would close it.
- **INFO-2: the `ct_cal` correction is applied after presence.**
  `ct_amps_cal_apply()` (`current_sense.c:399`) runs after
  `current_presence_is_flowing()` (`:387`). Presence therefore uses the
  uncorrected physics scale while S14/S15 use the corrected amps. This is
  consistent with the design comment and the two are never compared.
- **INFO-3: `snap.clipped[]` is published but unused.** It is set at
  `current_sense.c:402`, but no guard reads it. A saturated CT reads at the
  rail, so presence stays true, which is the safe direction, but S14 sees a
  capped magnitude.
- **INFO-4: one timing-out channel stalls all three.** A single degraded
  tick blocks the whole snapshot (`current_sense.c:345-361`). A persistently
  timing-out channel therefore makes all three channels stale after
  `CURRENT_MAX_AGE_MS`. S3/S9 then read "no current", and only S4's
  commanded-but-no-current warning surfaces it. The code comment says this
  is deliberate; noted because a single bad channel's ADC blinds the other
  two.
- **INFO-5: the H5 heat-rise check does not use current.**
  - The owner's 2026-10-04 heat-rise check is KilnFW `thermal_guard` guard 1,
    which is keyed on commanded duty, not current (`docs/SAFETY_CASE.md:63`).
    It is therefore unaffected by any CT fault.
  - Its non-coverage (on/off zones, Pico side) is already documented there.
  - Separately, the sweep's `zone_sweep_plan_k_ct_mapped()`
    (`zones_current_sweep_task.c:1056-1064`) scales every member channel by
    the same `measured_total_a` ratio. With a split map, two CTs of
    different turns ratios cannot be calibrated independently by the sweep;
    a manual `ct_cal` input is needed.

## Test gaps

1. No end-to-end `safety_core_build_input()` test with a committed split map
   that checks `any_current_present`, `amps_valid[]` and the commissioned
   flag (HIGH-1). The existing split tests inject guard inputs directly.
2. No test of `config_store_mask_current_present_to_fitted()` or
   `config_store_current_sensing_commissioned()` against a map with the
   group bit set.
3. No H9 test (`test_ct_leak_alarm*`) with a split map, where current on
   channel 0 must raise the alarm.
4. No `config_params` test that rejects implausibly small positive
   `k_ct_v_per_a` or `gain` (LOW-1). There is also no test that the
   reporting filter recovers from a non-finite sample.

## Resolution (ctfx, 2026-10-14)

HIGH-1 fixed. `config_store.h` gained `config_store_ct_channel_fitted_rec()`,
`config_store_fitted_channel_mask()` and
`config_store_mask_current_present_to_fitted_mask()`: when the
`CONFIG_STORE_SET_ZONE_CT_CHANNEL` group bit is set the committed map decides
which channels are fitted (`config_store_ct_channel_fitted_map()`), otherwise the
old `ct_topology` rule applies unchanged. Routed through them:
`safety_core.c` (`any_current_present` mask, S14/S15 `amps_valid_for_ct`),
`current_task.c` (`current_task_any_current_present()`, now caches the fitted
mask instead of topology plus installed), and
`config_store_current_sensing_commissioned()` (S9), which now requires `k_ct > 0`
on every channel fitted under the committed map. ESP H9
(`ct_leak_alarm.c`/`ct_leak_alarm_service.c`) reads params 0x0320-0x0322 and,
when all three are set, watches exactly the channels some zone selects.
Legacy maps ({0,1,2}, {2,2,2}) and an uncommitted map behave as before; the
change only widens what guards see for split maps.

S9 commissioned semantics: a board with a committed SPLIT/non-legacy map and
`k_ct` calibrated only on channel 2 (the old gate's only input) is no longer
"commissioned" (S9 warn-only) until its fitted channels are calibrated. That
board was already blind on those channels, so nothing is lost; boards on
identity or {2,2,2} maps (including the bench config, if it is on either) are
unaffected. Not verified against the live bench record (board not touched).

LOW-1 fixed. `k_ct_v_per_a[0..2]` and `gain[0..2]` accept 0 or a value in
[1e-4, 1e6] at SET_PARAM and at `config_params_validate()`
(`CONFIG_PARAMS_CT_SCALE_MIN/MAX`). The non-finite reporting-filter reseed
(`current_sense.c:409-414`) was not done; the bounds make the overflow
unreachable from the commit path.

Tests: `test_ct_split_map_guards.c` (SaftyFW; real SET_PARAM -> finalize ->
back-fill, S3 trip off channel 0, S14/S15 arm, commissioned, legacy maps,
bounds) and additions to `test_ct_leak_alarm.c` / `test_ct_leak_alarm_service.c`
(KilnFW). Negtested: all mutants CAUGHT with compiled failing assertions
except one equivalent mutant (permuting zone indices of the map, which cannot
change the fitted set).
