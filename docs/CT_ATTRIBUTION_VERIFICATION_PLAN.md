# CT attribution verification — commissioning plan

Status: PARTIALLY BUILT. Landed so far (this commit):

- Step 9's false verdict is fixed. `step9SweepVerdict()`
  (`firmware/KilnFW/App/drivers/http/setup_wizard_page.html`) is now the single
  place that decides, and it calls a finished sweep a pass only when
  `ct_map_derived_mask` shows a CT channel resolved for every zone swept. An
  inconclusive run renders the inconclusive text with the firmware's own reason
  and posts nothing, leaving step 9 PENDING â€” deliberately not `skipped`, which
  `computeCompleteness()` would treat as an outstanding reason forever. Covered
  by seven new assertions in `firmware/KilnFW/App/test/test_setup_wizard.js`,
  including the all-inconclusive case that is this bench's normal outcome.
- `ZONE_SWEEP_SETTLE_MS` is 10000 ms
  (`firmware/KilnFW/App/drivers/control/zones_current_sweep_engine.c`), with the
  comment block above it rewritten: it previously justified 1000 ms as "two full
  poll periods" and claimed a 5000 ms/zone total, neither of which survives the
  peak-hold envelope's ~1 s time constant. No sweep-wide deadline exists to
  overrun; the PcTools client's timeouts are per-HTTP-request and say so.
- Both PcTools documentation surfaces now state 10 s settle and ~14 s/zone.

- The derived threshold and the three-state verdict are built, pure and
  host-tested. `zone_ct_verify_threshold_a()` returns
  `max(noise_floor_a(channel), RESPOND_FRACTION * i_normal_a(zone))`, where the
  noise floor is the existing `ZONE_SWEEP_NORMAL_NOISE_FLOOR_A` rescaled by the
  live `k_ct_v_per_a` exactly as `zone_sweep_summed_normal_a()` already does, so
  the threshold genuinely moves with the entered clamp ratio instead of being a
  constant under a new name. `zone_sweep_verify_ct_attribution()` sits beside
  `zone_sweep_derive_ct_channel()` in
  `firmware/KilnFW/App/drivers/control/zones_current_sweep_engine.c`; it takes
  plain scalars, so the fitted question keeps its single owner on SaftyFW and
  the whole verdict is testable off-target. INCONCLUSIVE is the initial value
  and the only one reachable without a measurement, and PASS is written in
  exactly one place, behind one floor comparison.
  - Not fitted, no entered clamp ratio, a shared channel, no recorded normal
    current, no dominant channel (any NaN channel included), and a response
    below the derived threshold are all INCONCLUSIVE -- never FAIL, since none
    of them is evidence of miswiring.
  - A channel other than the configured one responding, and two distinct zones
    resolving to one channel, are FAIL.
  - Dominance resolution and the threshold comparison are deliberately separate
    steps. The plan's test 3 as originally written ("all three channels at
    0.023 A") exits on the dominance refusal and never reaches the floor
    comparison, which would have made the mandated negative test vacuous. That
    test is written as the real bench case instead -- one channel at ~23 mA, the
    others near zero -- with the all-equal form kept as its own assertion.
  - Twelve host test cases in `firmware/KilnFW/App/test/test_zones_http.c`:
    the plan's cases 1-6 and 8-11, plus a no-recorded-normal case and a
    bad-input case. Case 7 (a stale verdict) lands with the store.

- The `ct_verify`/`verdict_v1` store, its configuration fingerprint, the
  `ct_attribution` readiness item and the firing interlock are built, and they
  are deliberately ONE piece: a verdict nothing can act on, and an interlock
  with no verdict behind it, are each half a feature.
  - `firmware/KilnFW/App/drivers/persist/ct_verify_store.c` holds the blob
    (namespace `ct_verify`, key `verdict_v1`, 52 bytes, NVS only -- never the
    `cfg` filesystem, because a measurement about one board's clamps must not
    travel with a config backup). It depends on nothing but `hal_kv` and libc,
    which is what lets it link into any host-test executable.
  - `ct_verify_fingerprint()` is an FNV-1a hash over every configuration input
    that can change what a CT reading MEANS, field by field in a fixed order
    with floats canonicalized (all NaNs alike, -0.0f as +0.0f). It is hashed
    field-wise rather than as a struct memcpy on purpose: padding bytes are
    uninitialized, and hashing them would re-take every verdict at random.
  - The producer lives in
    `firmware/KilnFW/App/drivers/control/zones_current_sweep_task.c`, because
    gathering today's configuration needs the committed safety-config cache
    and the zones config. It records the verdict at end-of-run AFTER
    `zone_sweep_push_kct_and_inormal()` -- a verdict taken against the
    pre-push `k_ct` would be stale the moment it was written.
  - `ct_verify_current_fact()` is the ONLY reader of the stored verdict, and
    it compares the stored fingerprint against today's in the same expression
    that resolves the verdict. That is what makes a stale verdict impossible
    to act on: `readiness_http.c` (the displayed item) and `readiness_gate.c`
    (the interlock) both call it, so the page and the gate cannot drift.
  - The owner's rule is mapped onto the existing machinery without being
    re-derived: FAIL -> `READY_NOT_DONE`, which is the only status
    `readiness_gate_evaluate()` blocks on, so FAIL blocks both step 9 and
    firing; INCONCLUSIVE / STALE / never-run -> `READY_CANNOT_YET`, which
    blocks step 9 through the page's `anyCannotYet` arm and never reaches the
    gate; PASS -> `READY_OK`; `ct_installed == 0` -> `READY_DELIBERATELY_OFF`.
  - Step 9's `readinessKeys` is now `['ct_attribution']`
    (`setup_wizard_page.html`), so the step regresses when the fact regresses.
  - Tests: six new cases in `test_zones_http.c` (fingerprint stability and
    canonicalization, a per-field sensitivity sweep over all fifteen fields of
    `ct_verify_fingerprint_in_t`, blob validation, the NVS round trip, the
    resolved fact, and the plan's case 7 -- a stored PASS reading STALE after
    the CT map or `k_ct` changes, and returning when the configuration does),
    plus two in `test_readiness_gate.c` (a FAIL alone refuses a start; the
    other five facts never do) and the gate/display cross product widened from
    256 to 1536 combinations so the enum's six values are actually walked.

Still unbuilt: the `SAFETY_CT_CAL_BLOB_VERSION` 1->2 migration that adds the
operator-entered offset/gain trim. The fingerprint already hashes
`trim_offset_a`/`trim_gain` at their identity values, so landing the trim is a
one-line producer change that cannot forget to invalidate standing verdicts.

Four owner decisions of
2026-09-18 — settle time, verdict scope, an operator-entered clamp ratio with
offset/gain trim, and land order — are folded into the body below and summarised
in "Owner decisions", which replaces this plan's former open-questions list. The
third of them changes the design, not just a number.

## The problem

Nothing today checks that a current-transformer clamp is on the conductor the
configuration says it is on. The configuration is simply believed. Every
downstream current guard inherits that unverified assumption: presence detection
(`current_sense.c`'s `conducting = (amps > i_present_a)`), S3 stuck-load, S9
ineffective-heat, S11 frozen-sensor, and the two guards that depend on
attribution directly, S14 overcurrent and S15 open-heater deficit. A clamp on
the wrong conductor, or a channel configured as fitted with no clamp on it at
all, does not produce a fault. It produces a confident wrong answer.

There is a second, worse defect already live. Setup wizard step 9 is titled "CT
mapping verification" and it does energize zones, but its client logic in
`firmware/KilnFW/App/drivers/http/setup_wizard_page.html` finishes like this:

```js
if (st.state === 'done') {
  step9StopPoll();
  document.getElementById('step9Ok').innerHTML = '<p class="wok">Verification complete.</p>';
  postStepState(9, 'done').then(...)
```

`st.state === 'done'` means the sweep ran to completion. It does not mean the
sweep resolved anything. The firmware already knows the difference — the
accumulator in `zones_current_sweep_task.c` carries `derived_mask`,
`conflict_mask` and `unresolved_zone_mask`, and `zone_sweep_plan_k_ct()` refuses
with a note when zones were left unresolved — but none of that is inspected
before "Verification complete" is printed and DONE is persisted. On the bench
fixture nothing is ever resolved, and step 9 nonetheless reads green. That is
precisely the failure mode the owner named: a verification that silently passes
when it could not measure anything, converting an unknown into a false
assurance that the guards downstream then trust.

Step 9 also declares `readinessKeys: []`, so there is no readiness fact that can
ever regress it. Once DONE, it stays DONE across any later configuration change.

## Recommendation: fold into step 9, do not add a standalone step

Step 9 already is the standalone step. It applies heat, it requires the owner
physically present behind an explicit "I am present and the kiln is safe to heat
right now" acknowledgement, it refuses while a firing or an autotune is actually
running, it has an abort path, and it drives
`/api/zones/current_sweep/{start,status,abort}`, which is the mechanism that
energizes one zone at a time with every other zone off. The energizing sequence
this feature needs exists and is in the right place in the step order, after CT
installation and calibration in step 8 and before PID tuning in step 10.

What is missing from step 9 is not a procedure. It is a verdict.

PID tuning (step 10) is a poor host and should be rejected. Autotune runs one
zone at a time for hours, needs a rested kiln with no residual heat, feeds a
deliberately fake setpoint, and has an entirely different refusal set. Its
energizing pattern is long and single-zone, not the short cross-zone pattern
attribution needs — during an autotune of zone 0 you learn nothing about whether
zone 1's clamp is on zone 1. Folding attribution into tuning would also mean a
miswiring is not discovered until hours into the commissioning sequence, after
it has already been trusted by everything in step 9.

Folding into the sweep engine itself, rather than the wizard step, is where the
actual work goes: the decision logic belongs in
`zones_current_sweep_engine.c` beside `zone_sweep_derive_ct_channel()`, which is
already pure and host-testable, and the wizard step becomes the surface that
reads and refuses to launder the verdict.

## One energizing pass, two verdicts

Normal-current measurement and attribution verification share the same energizing
sequence and must not be run twice. They are one step. They are two verdicts,
stored and reported separately, because they fail independently: a zone can
produce a clean, believable `i_normal_a` while its clamp is on the wrong
conductor (the magnitude is right, the attribution is wrong), and a correctly
attributed channel can still be too noisy to yield a normal current worth
recording. Collapsing them into one pass/fail would let either one's success
speak for the other.

## Procedure

The sweep's existing shape is correct and should not be changed. Per zone, in
index order, with all other zones commanded off through `kiln_io_owner`:

- energize, settle `ZONE_SWEEP_SETTLE_MS` (**10000 ms**, raised from 1000 ms by
  owner decision 1),
- sample `ZONE_SWEEP_SAMPLE_MS` (4000 ms), accumulating a per-channel mean over
  all three channels, not just the expected one,
- de-energize, and require the channels to fall back before the next zone.

### Settle time: 10 s, and what it costs (owner decision 1)

The CT front end is a rectified peak envelope with a time constant of about 1 s
(`firmware/SaftyFW/docs/CURRENT_SENSE.md` §1/§3: R77 ∥ C57 = 1 MΩ ∥ 1 µF,
τ = 1.0 s), so the original 1000 ms of settling was **one** time constant,
roughly 63 % of the step — a zone whose true current sat at the decision
threshold could be measured a third low. This plan first proposed 3000 ms
(three constants, ~95 %). **The owner chose 10000 ms**, about ten time
constants — essentially fully settled, deliberately preferred over the ~95 %
that 3 s would give. Use 10000 ms throughout; nothing in this document is
predicated on 3000 ms any more.

What it costs, stated plainly:

- The delay is paid **per zone, per sweep**. `ZONE_SWEEP_ENERGIZE_MS` is
  `ZONE_SWEEP_SETTLE_MS + ZONE_SWEEP_SAMPLE_MS`
  (`firmware/KilnFW/App/drivers/control/zones_current_sweep_engine.c`), so a
  zone goes from 5 s to **14 s**. `zone_sweep_run_one_zone()` de-energizes and
  returns with no separate fall-back wait, and `zone_sweep_run_all_zones()`
  starts the next zone immediately, so a full **three-zone sweep goes from
  about 15 s to about 42 s end to end** — heat applied for 14 s per zone, one
  zone at a time, every other relay off for the whole window.
- `ZONE_SWEEP_SETTLE_MS` is **shared with the existing k_ct calibration path**,
  which is the same energizing pass — so that path lengthens identically, from
  ~15 s to ~42 s for three zones. There is no second sweep to leave at 1 s, and
  deliberately so: two sweeps with different settle times would be two
  different measurements of the same quantity.
- Nothing else is quantized against the old value. The loop advances in
  `ZONE_SWEEP_POLL_MS` (500 ms) steps, so 10000 ms lands exactly on a poll
  boundary, and the existing host tests
  (`firmware/KilnFW/App/test/test_zones_http.c`, the
  `zone_sweep_should_sample()` / `zone_sweep_zone_done()` cases) assert against
  the constants symbolically rather than against literals, so they follow the
  change rather than breaking on it.
- Two documentation surfaces quote "1 s settle … roughly 5 seconds per zone"
  and must be corrected in the same commit as the constant, or they become the
  stale-claim class this repo keeps finding:
  `tools/PcTools/src/kilnctrl/mcp_server_zones_current_sweep.py` and
  `tools/PcTools/src/kilnctrl/zones_current_sweep_http_client.py`.

Sample count against noise: publish is 20 Hz, so a 4000 ms window is about 80
samples. Measured idle standard deviation on the fitted channel is 4.678 counts
in the 2026-09-06 capture and 9.329 counts in a later larger one, against 0.180
and 0.223 on the two unfitted channels — the fitted channel is roughly 40 to 50
times noisier, which is lead pickup, not a defect. Taking the pessimistic 9.329
counts, the standard error of an 80-sample mean is about 1.04 counts, or roughly
0.8 mA at k_ct 1.0 V/A. That is comfortably below the existing
`ZONE_SWEEP_NORMAL_NOISE_FLOOR_A` of 0.045 A, so the sample window is not the
binding constraint. The binding constraint is the fixture, below.

Attribution decision, per zone: reuse `zone_sweep_derive_ct_channel()`
unchanged. It already requires a dominant channel at
`ZONE_SWEEP_CT_DOMINANCE` (4x), already treats a NaN entry as making the whole
call ambiguous rather than silently zero, and already refuses rather than
guesses when two channels read within 4x of each other — which is exactly the
shared-CT case or a foreign load, neither of which is derivable. Its refusal is
the correct behaviour and must be preserved, not softened.

The verdict then compares the derived channel against the configured one. Because
this plan lands after step 4 of `docs/CT_CHANNEL_MASK_PLAN.md` (owner decision 4),
the configured channel is per-zone `zone_ct_channel[z]`, with
`member(ch) = {z : zone_ct_channel[z] == ch}`, not the `ct_channel_map` plus
`ct_topology` pair it replaces. Read it through a single accessor rather than
indexing the field directly, so a later encoding change touches one function.

"Is this channel wired at all" is a separate question and it already has an
owner: **`config_store_ct_channel_fitted()`** (`firmware/SaftyFW/src/config_store.h`,
added by commit bdded44b), the single predicate every call site now goes through
— the `s_current_sensing_commissioned` gate, the S14 per-channel loop, and
`config_store_mask_current_present_to_fitted()`, which masks `any_current_present`
to fitted channels so an unfitted channel's idle ADC floor can no longer read as
current. That commit's own header comment names this function as the seam the
per-zone-selection change will cut along. **This verification must call it and
must not introduce a second notion of fitted.** A channel that is not fitted is
not a FAIL and not a PASS: it is simply outside the comparison, and a zone whose
configured channel is not fitted is INCONCLUSIVE with that reason. Where two or
more zones
share a channel, per-zone attribution is not derivable by construction and the
correct verdict for those zones is INCONCLUSIVE with the shared-channel reason,
never PASS; what can still be verified there is that the shared channel responds
to each member and that no non-member zone drives it.

## The response threshold: clamp ratio, offset and gain trim (owner decision 3)

The owner rejected both options this plan originally posed — neither the fixed
`ZONE_SWEEP_CT_RESPOND_A` of 2.0 A nor a threshold silently derived from
`i_normal_a`. Their words: *"The circuit has a fixed gain but clamp ratios change.
Allow the user to enter clamp ratio and offset/gain tuning to get more accurate."*

That is the correct reading of the hardware. The board's analog gain really is
fixed: `CURRENT_SENSE.md` §1 traces it to R46/R43 = 7.15k/10k = 0.715, a
resistor ratio, and the burden R72 is **DNP on all three channels**, so the
board expects a self-burdened, voltage-output clamp. Which clamp is fitted is an
installation choice with its own turns ratio, and that ratio — not the board — is
what converts a measured secondary voltage into a real primary current. A fixed
2.0 A threshold bakes one clamp's assumption into firmware.

### Where the ratio belongs in the existing chain — and what already exists

Traced rather than assumed, because a prior agent in this project mistook
`config_store.h`'s legacy `ct_cal[3]` table (params `0x0310`-`0x0312`, the
end-to-end amps-correction fit) for the real calibration path. **It is not.** The
live path is:

| Quantity | Where it lives | Who enters it |
|---|---|---|
| `A_fs` — probe rating, amps at 1 V output | ESP-local NVS blob `safetyctcal`, namespace `kiln_cfg` (`safety_cfg_store.c`), per channel, with a `source` marker | operator, web UI |
| `zero_mv` — probe output at zero current | same blob | operator, or the auto-zero action |
| `gain` — front-end divider, 0.715 default | Pico params `0x030B`-`0x030D`, `F32`, already settable over `SET_PARAM` | nobody today — **readonly in the UI** |
| `k_ct_v_per_a` | Pico params `0x0308`-`0x030A`, **derived**, `= 1 / A_fs` | `safety_ct_cal_convert()` |
| `zero_counts` | Pico params `0x0302`-`0x0304`, **derived**, `= zero_mv/1000 · gain · 4096/3.3` | `safety_ct_cal_convert()` |

and the Pico then computes
`I = (counts − zero_counts) · (3.3/4096) / (gain · √2 · k_ct_v_per_a)`.

Three conclusions follow, and two of them mean less new work than the decision
first suggests:

1. **The clamp ratio is already stored — as `A_fs`.** For a self-burdened
   voltage-output clamp, "amps at 1 V out" *is* the turns ratio expressed in the
   units this front end sees, which is exactly why `k_ct_v_per_a = 1/A_fs`. This
   is a **re-expression of an existing field, not a new one**: no new stored
   field, no new param id, no schema bump on either processor. What is missing
   is honesty at the entry surface — the field is presented as a probe rating
   rather than as the clamp ratio the verdict depends on, and nothing tells the
   operator that every current-derived verdict is scaled by it.
2. **Gain trim needs no new storage either.** `gain[0..2]` already exists as a
   per-channel Pico parameter, already travels the generic
   `SET_PARAM`/`COMMIT_CONFIG` path, and already sits in the denominator of the
   Pico's amps formula. It is merely marked `readonly: true` on
   `safety_commissioning_page.html`. Making it operator-editable is a UI change
   plus a range check, not a storage change.

   **The arithmetic is already correct — do not "fix" it.** An earlier revision
   of this plan asserted a defect here: that `safety_ct_cal_convert()` applies
   `gain` to `zero_counts` only and not to `k_ct_v_per_a`, so a gain edit would
   move the zero point but not the scale. That assertion was wrong, and it is
   retracted. The host and the Pico apply the same divider once each, in
   opposite directions, on the two sides of the counts domain:

   - host, `safety_cfg_store.c:845` and `:856` — `k = 1.0f / a_fs`, then
     `counts_f = (zero_mv / 1000.0f) * gain * (4096.0f / 3.3f)`: multiplies by
     `gain` to carry probe millivolts *into* ADC counts.
   - Pico, `current_sense.c:287` and `:288` —
     `v_adc = delta_counts * CS_ADC_VREF_V / CS_ADC_FULL_SCALE`, then
     `amps = v_adc / (gain * CS_SQRT2 * k_ct)`: divides by `gain` to carry ADC
     counts back *out* to probe volts.

   `k_ct_v_per_a` is deliberately gain-free precisely because the Pico applies
   `gain` itself, exactly once. Composing the two halves gives the end-to-end,
   operator-visible conversion:

   ```
   I(counts) = [ counts * 3.3/(4096 * gain) - zero_mv/1000 ] * A_fs / sqrt(2)
   slope     = 3.3 * A_fs / (4096 * sqrt(2) * gain)     depends on 1/gain
   offset    = - zero_mv * A_fs / (1000 * sqrt(2))      independent of gain
   ```

   So `gain` is already a **pure slope control** and `zero_mv` is the offset —
   exactly the split an operator trimming against a reference meter wants. The
   `gain` inside `zero_counts` cancels against the Pico's own `/gain`, and that
   cancellation is what makes the offset term gain-independent.

   **Applying `gain` to `k_ct_v_per_a` as well would cancel against the Pico's
   divider, removing `gain` from the slope entirely and leaving it scaling the
   offset — which is precisely the offset-only control the retracted claim
   described.** The "fix" would have created the defect it was reported as.
   Nobody should re-propose it. `ct_auto_zero_counts_to_mv()`
   (`safety_cfg_http.c:1023`) is the exact algebraic inverse of `:856` and
   confirms the single-application convention from a third site.
   `firmware/SaftyFW/docs/CURRENT_SENSE.md` section 5 corroborates the Pico
   half directly, writing the composed formula with `0.715` as a literal in the
   denominator; it covers the host half only obliquely, via its `k_ct_v_per_a`
   row, so it is not independent confirmation of both halves.

   **Risk labelling corrected alongside the unlock.** The page carried
   `risk: 'cosmetic'` and `guards: ['(power estimate only)']` on `gain[0..2]`,
   and section 5's table says the same. That is wrong: `gain` is not
   power-estimate-only. `current_presence_is_flowing()`
   (`current_presence_policy.h:120-121`) takes `gain` as a parameter and derives
   its counts-domain presence threshold from it; that predicate feeds
   `current_snapshot_t.present[]`, which `current_any_present()` returns, which
   S3/S4/S9 and S6b's current-gated trip read. A bad `gain` therefore moves a
   guard input, not only a wattage display. The three `gain[0..2]` rows are
   relabelled to match; no other field's labelling was touched, and
   `CURRENT_SENSE.md` section 5's table row remains to be corrected separately.
3. **Offset trim already exists** as `zero_mv`, operator-entered in the same
   form, with an auto-measure action beside it.

So the only genuinely new quantity is a **scale trim distinct from the nameplate
ratio** — "my clamp says 30 A at 1 V, but against a reference meter it reads 4 %
high". Folding that into `A_fs` silently would lose the distinction between what
the clamp claims and what it measures, which is the thing the operator will want
to re-edit later. Keeping them separate costs an ESP-local schema step: two more
floats per channel in `safety_ct_cal_blob_t`, taking
`SAFETY_CT_CAL_BLOB_VERSION` from 1 to 2. That bump is cheap (ESP-local NVS, not
a wire format, not the Pico's record) **but must not be taken as the code stands**:
`load_ct_cal()` has no migration — a version mismatch logs a warning and
*resets to defaults*, discarding every entered `A_fs`/`zero_mv` on the board.
The bump must therefore land together with a real v1→v2 branch that carries the
existing pair forward and seeds the trim at unity. A bump without that migration
would silently de-commission a commissioned board, which is the same
"configuration quietly reverts to defaults" hazard recorded for the Pico
rollback in `docs/CT_CHANNEL_MASK_PLAN.md`.

### What the verification threshold then is

Per zone, the response threshold is derived, never a bare constant:

```
threshold_a(z) = max( noise_floor_a(ch)                    /* rescaled by the live k_ct, as
                                                              zone_sweep_summed_normal_a() already does */
                    , RESPOND_FRACTION * i_normal_a(z) )   /* the zone's own configured normal */
```

`ZONE_SWEEP_CT_RESPOND_A` stops being the verification's threshold. It stays
where it is for `zone_sweep_derive_ct_channel()`'s existing "did anything
conduct" test, whose job is coarse and unchanged; the verdict adds its own,
calibrated test on top.

### How a wrong or un-entered ratio must fail

This is the part that decides whether the feature is worth having, because a
mis-entered ratio scales a reading into a **confident wrong answer** — the exact
defect this plan exists to close, reintroduced one layer up. Rules, all of them
INCONCLUSIVE-by-construction rather than best-effort:

- **Ratio not entered** — the channel's blob entry has `has_value == 0`, or the
  committed `k_ct_v_per_a <= 0`. Verdict INCONCLUSIVE, reason "clamp ratio not
  entered for channel N". **Never substitute a default ratio**, and in particular
  never `SAFETY_CT_CAL_DEFAULT_GAIN`'s sibling assumption that some clamp is
  1 V/A: an uncalibrated channel already falls back to a counts-domain presence
  margin and reports 0 A, so a PASS computed there would be arithmetic on a
  number the firmware itself declines to believe.
- **`i_normal_a` not measured for the zone** — INCONCLUSIVE, reason "no normal
  current recorded". No silent fallback to the old 2.0 A constant; that is the
  hidden-constant behaviour the owner rejected.
- **Out-of-range entry** — rejected at the door, nothing written.
  `safety_ct_cal_convert()` already returns false and leaves its outputs
  untouched for a non-finite `a_fs`, an `a_fs` outside [0.1, 2000] A, a
  `zero_mv` outside ±200 mV, or a non-positive `gain`, and the POST handler
  stages to the Pico *before* persisting locally. Extend the same discipline to
  the trim fields; do not clamp a bad trim into a plausible-looking one.
- **Ratio changed since the verdict** — covered structurally by the
  configuration fingerprint below, which hashes the ratio, the trim, the offset
  and the provenance marker. A verdict taken under one ratio can never be read
  as a pass under another.
- **A wrong but in-range ratio** is not detectable by this firmware and the plan
  must not pretend otherwise: it is a hardware-truth question like
  `ct_installed`, and the honest mitigation is that a badly wrong ratio pushes
  the measured current away from `i_normal_a` and lands on INCONCLUSIVE or
  FAIL rather than on PASS, because the threshold and the reading are scaled by
  the same number only when the number is right.

## Pass, fail, inconclusive

Three states, never two. Per zone for attribution, and separately per zone for
normal current:

- **PASS** — a dominant channel was resolved, it matches the configured channel,
  that channel is fitted per `config_store_ct_channel_fitted()`, its clamp ratio
  has actually been entered, and the response cleared `threshold_a(z)` above with
  all non-configured channels quiet. Only a measurement can produce this.
- **FAIL** — a dominant channel was resolved and it is not the configured
  channel, or the configured channel stayed quiet while another responded, or two
  zones both claimed the same channel where the configuration says they are
  distinct (`conflict_mask`).
- **INCONCLUSIVE** — anything else. Below the threshold, no dominant channel, a
  channel in a shared group, a channel that is not fitted, a clamp ratio or a
  zone normal that was never entered, a NaN, an aborted or refused pass, a link
  drop mid-pass.

The rule that makes this safe is structural, not a matter of care at each call
site: **INCONCLUSIVE is the initial value and the only value reachable without a
measurement.** PASS is written in exactly one place, and only from a branch that
has already established a resolved dominant channel above the floor. Absence of
evidence therefore cannot fall through to PASS; it lands on the value it started
at.

## What the physics permits here, honestly

This bench cannot produce a PASS, and the plan should stop pretending a future
run on it will.

The fixture draws about 70 mA total, roughly 23 mA per zone. The sweep refuses to
record a normal current below `ZONE_SWEEP_NORMAL_NOISE_FLOOR_A` of 0.045 A, so
every zone is below the floor by a factor of two — and that floor, not the old
2.0 A constant, is what the derived threshold above collapses to when no normal
current has ever been measured, which on this bench is every zone. Deriving the
threshold from the clamp ratio therefore does **not** rescue the bench: it
removes the arbitrary 2 A, but the noise floor remains, and the honest verdict
is still INCONCLUSIVE.

This is not an arithmetic prediction. It was attempted empirically on
2026-09-18 and recorded in
`docs/audits/ct_clamp_channel_identification_2026-09-18.md` (commit 91c41de4):
each of the four relays was energized in turn for 25 s, one at a time through
`kiln_io_owner`, against a 65 s all-off baseline, and **no relay produced a CT
step distinguishable from that channel's own baseline noise, on any channel**.
Channel 2 — the only channel with a non-trivial reading — drifted 85.6 → 88.2
counts across the session regardless of which relay was on, inside its own 3.6-
count baseline standard deviation. The audit's own verdict is that neither
agreement nor disagreement with the configuration could be established, both
because the measurement came back negative and because no configured per-zone
CT attribution exists to compare against yet. That is precisely the gap this
plan closes on the configuration side and cannot close on the measurement side.

No adjustment of settling or sample count reaches that — including the 10 s
settle of decision 1, which fixes an under-settled envelope, not a signal
smaller than the noise. The front
end is a rectified peak envelope, not a waveform sampler, so per-cycle techniques
— phase comparison, correlation against relay switching edges, synchronous
detection — are unavailable in principle, not merely unimplemented. The `sqrt2`
in the conversion also assumes a sinusoidal resistive load, which is fine for
elements and wrong for anything else.

So: **on this bench, every zone must report INCONCLUSIVE, and a run that reports
anything else is a bug.** That is a useful property, not a disappointment — it
makes the inconclusive path the one the bench exercises by default, so the false-
pass defect cannot hide in a rarely-taken branch. On a real kiln, with elements
drawing tens of amps, every threshold above is met with a wide margin and the
verdict is a real measurement.

Splitting it explicitly:

- **Verifiable on this bench**: the decision logic in host tests; that the
  bench run yields INCONCLUSIVE for all three zones and does not mark the step
  done; that the verdict is persisted, surfaced, and invalidated on a
  configuration change; that the step refuses to start during a firing or an
  autotune; that relays are driven only through `kiln_io_owner`.
- **Requires a real kiln**: any PASS at all; any FAIL from a genuinely swapped
  clamp; the true settling behaviour of the envelope at real current (10 s is
  about ten time constants, so this is expected to be settled rather than
  marginal, but it has never been measured at real current); whether the 4x
  dominance ratio is the right margin with real elements and real lead coupling;
  whether an operator-entered clamp ratio and trim actually bring a real
  installation's reading onto a reference meter.

## Reporting and blocking

A wrong clamp is safety-relevant miswiring, not a preference. **Confirmed by the
owner (decision 2), as recommended: a FAIL blocks step 9 from completing AND
blocks the firing interlock.** It should
be a readiness item, not merely a wizard note, so it reaches the same gate that
already refuses to fire on a latched trip or an unverified E-stop. The operator
text should name the zone, the configured channel, and the channel that actually
responded, because that triple is enough to walk to the panel and move the clamp.

INCONCLUSIVE must **warn and block step 9 from reading done, and must never
block the firing interlock** — also confirmed by the owner. Blocking firing on
inconclusive would brick this bench and any kiln too small to reach the response
threshold, which is worse than the status quo: a check that blocks everything
gets switched off, and a check that is switched off protects nothing. It must be
visibly distinct from PASS in every surface —
a distinct status word and colour in the readiness list, an explicit "could not
determine" in the step 9 text — and must never be summarized alongside PASS as
"no problems found". The wizard step's stored state for an all-inconclusive run
is PENDING or SKIPPED, never DONE.

Note that the wizard's own tri-state `SETUP_WIZ_STEP_{PENDING,DONE,SKIPPED}` has
no room for "inconclusive", which is structurally why the current code posts
`done`. The verdict must therefore live in its own store and be projected into
the wizard through a readiness key, not squeezed into the progress blob.

## What is stored, and staleness

A verdict that outlives the configuration it was taken against is a trap. Store,
per zone and per verdict kind: the verdict enum, the measured current, the
channel that responded, the Unix timestamp, and a **configuration fingerprint** —
a hash over the fields the verdict actually depends on: each zone's
`zone_ct_channel`, `ct_installed` and hence each channel's
`config_store_ct_channel_fitted()` answer, the per-channel calibration inputs the
threshold is scaled by (the clamp ratio `A_fs`, `zero_mv`, `gain`, and any trim),
the derived `k_ct_v_per_a`/`zero_counts` they produce, the `source` provenance
marker, each zone's `i_normal_a`, and the zone-to-relay mapping. Everything the
threshold arithmetic reads must be in the hash: a field that scales a reading but
not the fingerprint is a stale-verdict hole.

Invalidation is then not a matter of remembering to clear it. The readiness item
recomputes the fingerprint from live config on every read and reports
INCONCLUSIVE, with the reason "configuration changed since this was verified",
whenever it differs from the stored one. A stale verdict cannot be read as a pass
because it is never compared for equality with PASS without the fingerprint check
in the same expression. This deliberately mirrors the wizard's existing REGRESSED
precedence in `setup_wizard_progress_effective_state()`, where `/api/readiness`
is authoritative over stored progress — giving step 9 a readiness key is what
lets that existing, already-tested machinery do the regression for free.

The reset-one-side-of-a-pair class applies directly here: the verdict and the
configuration are two pieces of state joined by a semantic contract. The
fingerprint is what expresses that contract in one place instead of relying on
every config writer to remember a companion clear.

## Schema and storage cost

New NVS namespace and key, both under the 15-character limit enforced by
`NVS_KEY_LEN_CHECK()` in `firmware/KilnFW/App/drivers/persist/nvs_key_check.h`
and by `tools/check_nvs_key_length.ps1`:

- namespace `ct_verify` — 9 characters.
- key `verdict_v1` — 10 characters.

Both fit with margin. (`relay_names_cfg` already sits at exactly 15, so this was
checked rather than assumed.) The blob is small: three zones times two verdicts
times {enum, float, channel byte} plus one timestamp and one 32-bit fingerprint —
under 64 bytes with a version byte and room to grow. NVS only, not the `cfg`
LittleFS partition, matching `setup_wizard_progress.c`'s reasoning.

**This feature bumps no config schema version on either processor**, and an
earlier draft of this section was wrong to reach for `ZONES_CFG_VERSION` even to
deny it. `ZONES_CFG_VERSION` is KilnFW's zones blob on the ESP32-S3
(`firmware/KilnFW/App/drivers/persist/zones_config_json.h`); the schema that
bumps for `zone_ct_channel` is **SaftyFW's `CONFIG_STORE_FORMAT_VERSION`, 2 to
3** (`firmware/SaftyFW/src/config_store.h`), a different schema on a different
processor, and that bump belongs to `docs/CT_CHANNEL_MASK_PLAN.md` step 2 — see
that plan's storage-decision section, revised at 7e517564, and the Pico-rollback
cost it records. This plan inherits that bump by landing after it; it adds none
of its own, because it adds a new ESP-side NVS store rather than widening any
existing blob.

It must be written against the accessor `docs/CT_CHANNEL_MASK_PLAN.md`
introduces, and lands after that plan's step 4, so the fingerprint hashes
`zone_ct_channel` rather than the `ct_topology` byte that work supersedes.

New readiness item key `ct_attribution`, alongside the existing `autotune` item
in `readiness_http.c`. Step 9's `readinessKeys` gains it. `SETUP_WIZARD_STEP_COUNT`
is unchanged — no step is added — so the mirror-drift check is unaffected.

`SETUP_PROGRESS_JSON_CAP` is unaffected for the same reason, which matters
because that constant carries an explicit instruction not to grow it.

## Testing

Host tests, in `zones_current_sweep_engine.c`'s existing pure-function test
target. Note that `test_zones_http.c` builds its own separate executable because
it `#include`s the implementation files directly; these tests belong beside the
engine tests, not in that one.

Required cases:

1. **Correct attribution** — zone 0 drives channel 0 at 12 A with channels 1 and
   2 near zero, configured channel 0. Expect PASS.
2. **Swapped clamp** — zone 0 drives channel 1 at 12 A, configured channel 0.
   Expect FAIL, with channel 1 named in the result.
3. **Below floor (the bench case)** — all three channels at 0.023 A. Expect
   INCONCLUSIVE, and explicitly assert it is not PASS.
4. **No dominant channel** — two channels within 4x. Expect INCONCLUSIVE with the
   ambiguous reason, not a guess at the larger one.
5. **NaN channel** — expect INCONCLUSIVE, not a zero substitution.
6. **Shared channel** — two zones configured to the same channel, both drive it.
   Expect INCONCLUSIVE for per-zone attribution, not FAIL and not PASS.
7. **Stale fingerprint** — a stored PASS whose fingerprint does not match live
   config. Expect the readiness item to report INCONCLUSIVE.
8. **Conflict** — two zones configured distinct, both resolving to the same
   channel. Expect FAIL.
9. **Clamp ratio never entered** — a channel whose calibration input is unset
   (`has_value == 0`, committed `k_ct_v_per_a <= 0`) while the channel reads a
   healthy dominant response in raw terms. Expect INCONCLUSIVE with the
   ratio-not-entered reason, and assert explicitly that no default ratio was
   substituted.
10. **Channel not fitted** — the zone's configured channel answers false to
    `config_store_ct_channel_fitted()`. Expect INCONCLUSIVE, not FAIL: an
    unfitted channel is outside the comparison, not evidence of miswiring.
11. **Threshold scales with the entered ratio** — the same raw measurement,
    evaluated against two different entered clamp ratios, must produce different
    verdicts in the direction the arithmetic predicts. This is what proves the
    threshold is derived rather than a constant wearing a new name.

**The negative test.** The single line that must be broken is the floor
comparison that gates the PASS branch — the condition requiring the resolved
channel's measured current to exceed the noise floor before a verdict may be
written as PASS. Inverting or deleting it must make test 3 (below floor) fail by
reporting PASS where INCONCLUSIVE was expected. If test 3 still passes with that
line removed, the test is vacuous and the false-pass defect this whole plan
exists to close is not actually covered. Per repo practice the negative test ends
with a forced full rebuild, not a hand-restore and an empty diff — a poisoned
executable surviving in the build directory has already produced a committed
wrong verdict once in this repo.

A second negative test is worth having on the fingerprint: breaking the
fingerprint comparison must make test 7 fail. Otherwise staleness invalidation is
untested and the trap is open.

## Coexistence with the k_ct sweep

`zone_sweep_plan_k_ct()` skips a channel only when its `safety_cfg_store` cal
source is `SAFETY_CT_CAL_SOURCE_MANUAL`. On this board all three channels report
`uncalibrated`, so that skip never engages and the sweep remains entitled to
overwrite channel 2's correct calibration — already recorded in ROADMAP as a
latent exposure. The verification pass must not widen it. Concretely: the
verification reads per-channel averages and writes only its own verdict store; it
never writes `k_ct_v_per_a`, never writes `ct_channel_map` or `zone_ct_channel`,
and never writes `i_normal_a` for a zone whose measurement was below the floor.
Running the verification must be safe to repeat and must not be capable of
changing the configuration it is checking — a verifier that rewrites its own
subject cannot fail.

Conversely, a k_ct sweep that does change calibration changes the fingerprint,
which correctly invalidates any prior verdict.

## Owner decisions, 2026-09-18

All four of this plan's former open questions are decided. There are no open
questions for the owner in this document.

1. **Settle time: 10000 ms**, not the 3000 ms proposed — about ten envelope time
   constants, deliberately chosen over the ~95 % that 3 s would give. Paid per
   zone per sweep and shared with the k_ct calibration path, taking a three-zone
   pass from ~15 s to ~42 s end to end. See the Procedure section.
2. **Verdict scope: confirmed as recommended.** FAIL blocks both the wizard step
   and the firing interlock. INCONCLUSIVE blocks only the wizard step and never
   the firing interlock.
3. **Operator-entered clamp ratio, with offset and gain trim**, rejecting both
   the fixed 2.0 A constant and a threshold silently derived from `i_normal_a`.
   The board's gain is fixed in hardware; the fitted clamp's ratio is an
   installation fact and must be entered. The threshold is derived from the
   entered ratio, the trim and the zone's configured normal together. Entry is
   web UI only, on `safety_commissioning_page.html` beside the fields it belongs
   with, consistent with the owner's standing choice for configuration of this
   kind — no LCD entry surface and no PcTools-only path, since a value only
   enterable from a tool is a value a kiln owner cannot correct. Most of it is
   already expressible in existing fields (`A_fs` is the clamp ratio; `gain` and
   `zero_mv` already exist and only need to become editable); only a scale trim
   distinct from the nameplate ratio is new, and it costs an ESP-local
   `SAFETY_CT_CAL_BLOB_VERSION` 1→2 step that must ship with a real migration.
   Full derivation in "The response threshold" above.
4. **Land order: confirmed.** This plan lands after step 4 of
   `docs/CT_CHANNEL_MASK_PLAN.md`, so the fingerprint hashes `zone_ct_channel`
   rather than the `ct_topology` byte that work replaces, and the fitted
   predicate it builds on is `config_store_ct_channel_fitted()` from bdded44b.
