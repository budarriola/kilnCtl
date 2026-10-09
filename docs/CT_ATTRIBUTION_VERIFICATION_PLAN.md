# CT attribution verification -- pending items

Pending work only. Design, as-built record and test list: `docs/CT_ATTRIBUTION_VERIFICATION.md`. Verified against origin/dev 2026-10-09.

Built and host-tested: the step 9 verdict fix (`step9SweepVerdict()`), 10 s settle, `zone_ct_verify_threshold_a()` and `zone_sweep_verify_ct_attribution()`, the `ct_verify` store with `ct_verify_fingerprint()`/`ct_verify_current_fact()`, the `ct_attribution` readiness item and interlock, `SAFETY_CT_CAL_BLOB_VERSION` 2 trim and its commissioning page entry. No software item remains.

## Pending (all need a real kiln with measurable current; the bench reports INCONCLUSIVE by design)

- Any PASS verdict; any FAIL from a genuinely swapped clamp.
- Envelope settling at real current (10 s is ~10 time constants but unmeasured).
- Whether the 4x dominance ratio is right with real elements and lead coupling.
- Whether an operator-entered clamp ratio and trim bring a real installation onto a reference meter.
