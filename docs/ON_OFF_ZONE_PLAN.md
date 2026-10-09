# On/off device zones -- pending items

Pending work only. Design and per-step history: `docs/ON_OFF_ZONE.md`. Verified against origin/dev 2026-10-09.

Steps 1-8 are shipped and host-tested (`d58492c9`, `3d740f78`, `dd1d6ada`, `172e3081`, step 8 actuation `profile_executor_on_off_zone_tick()`). The spare-relay path (`docs/SPARE_RELAY_ONOFF_PLAN.md`) was bench-exercised 2026-10-09 (aux relay 4 rule-driven on/off, `docs/BENCH_TEST_LOG.md`), which covers aux outputs but not an `ON_OFF` zone-typed slot.

## Pending

1. **Step 9 (bench, owner present, dry contacts only): fire with an `ON_OFF` zone.** No bench zone is `zone_type` ON_OFF (all `0` per the 2026-10-09 log), and `control_convert_onoff_zone_to_aux` is one-way. Verify quasi-dwell on a deliberately lagging segment and the actuation min_on_s/min_off_s hold. Hardware- and owner-gated; no software remainder found.
2. Owner questions 1-4 (supply behind K4, S8 proximity, interpretation, switching frequency) are in `docs/ON_OFF_ZONE.md` "Questions for the owner".
