# "S6a won't clear" investigation — 2026-09-08

## Headline correction

The latched trip is **not S6a**. `trip_mask 0x0004` is a per-reason bitfield
(`bit = 1 << (reason - 1)`), and `safety_get_diag()` independently reports
`trip_reason 3` = `SAFETY_TRIP_LOAD_STUCK_ON` (**S3**, `safety_guards.h`),
not `SAFETY_TRIP_MAIN_FAULT` (S6a = reason 6, which would be `0x0020`).
S6a was never involved.

## Evidence

- `pico_gpio_read(10)` → **high** (mainFault line NOT asserted; active-low
  per R8 pull-up). If this had been S6a, a high GPIO10 plus a refused clear
  would point at a real latch defect — but it isn't S6a, so this is moot.
- `safety_get_diag()`: `state tripped | trip_reason 3 | trip_mask 0x0004`,
  consistent across multiple reads and after issuing another
  `safety_clear_trip()` (delivered/ACKed, outcome unchanged — refused).
- `safety_get_status()`: `currents not fitted, not fitted, 0.00 A`,
  `ct_counts 16, 17, 59`.

## Root cause: uncommissioned CT, working as designed

`safety_guards.c`'s `guard_condition_still_immediate()` for S3 is:
`in->context_valid && in->any_current_present && !in->relay_commanded_recently`.

`any_current_present` comes from `current_presence_policy.h`
(`current_presence_is_flowing()`). Per that file's own header comment: when
a channel's `k_ct_v_per_a` is uncommissioned (`<= 0`), presence detection
falls back to a fixed raw-ADC-counts margin
(`CURRENT_PRESENCE_POLICY_FALLBACK_MARGIN_COUNTS = 25`, ~20 mV) against
`zero_counts` (default 0), deliberately biased toward **detecting** current
rather than missing it — S3 and S9 are the two guards this bias exists to
protect, because a false negative on either is the dangerous direction.

The third CT channel reads 59 raw counts, comfortably above the 25-count
fallback margin, while `k_ct_v_per_a` is not commissioned for it (hence
`0.00 A` reported — the amps path short-circuits to 0 when uncalibrated,
which is a *different* code path than the presence fallback and does not
contradict it). So `any_current_present` reads **true** on every tick,
`!relay_commanded_recently` is true (nothing has been commanded), and S3's
clear-condition recheck correctly refuses every time — not a bug, the
documented safe-direction behavior for an uncommissioned channel.

## (a) or (b)

**(a) — condition genuinely persists**, by design. S3 is doing its job:
refusing to clear while a CT channel reads above its safety-fallback
presence threshold and is not commissioned to a real amps scale. This is
not a S6a latch defect, and no `safety_clear_trip()` code path needs fixing.

## Version skew (recorded, not the cause here)

- Pico: `c13f8828`, built 2026-09-09 04:21:04Z, protocol v12 (min compat v7).
- ESP: `36394fbd`, built 2026-09-09 02:20:15Z, protocol v11, tree clean,
  **13 commits behind current HEAD (38748a25)**. Neither processor was
  reflashed in this pass. Protocol reports compatible (v11 vs. min-compat
  v7), so this skew is not implicated in the S3 finding.

## What clears it

Commission the CT channel(s) per `docs/CURRENT_SENSE.md` section 5 (zero
and gain, `safety_set_ct_cal`) so the amps-based path takes over from the
counts-domain fallback, or physically confirm the channel reading 59 counts
is genuinely at its true zero and re-zero it. Forcing the trip open (there
is no such path exposed, and none should be added) is not the fix — the
guard is reading real ADC counts above its safe margin.

## Does this block firing?

Yes, exactly as it should: S3 is the welded-SSR/load-stuck-on guard, one of
the two 🔴 dangerous-risk guards this fallback bias is written to protect,
and it will not clear until the CT reads convincingly at rest (below the
margin) or is properly commissioned. No firmware change is proposed here —
no defect was found in `safety_clear_trip()`'s S3 handling.

## No code changes

This was a read-only diagnosis. No files edited, no negative test needed —
there is no bug to demonstrate a fix for.
