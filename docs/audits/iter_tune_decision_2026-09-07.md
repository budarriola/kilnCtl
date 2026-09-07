# iter_tune.c: wire or delete — decision brief (2026-09-07)

1. **What it does.** Pure decision logic (no ESP-IDF/NVS/lock) for a per-zone
   hill-climb: after each firing, perturb kp/ki by ±5%, keep the change only
   if the next comparable firing's `iae_normalized` improves by more than a
   noise floor, else revert. Needs, per `iter_tune_firing_t`: `profile_id`,
   `zone_mask` (from `profile_firing_run_record_t`), `iae_normalized` (from
   `profile_exec_firing_stats_t` via `firing_stats_snapshot()`), the gains
   that produced the score, and `start_temp_c` — the one field **not
   currently captured anywhere**; the header says to add it to
   `zone_runtime_t` alongside the existing `fs_target_min_c`/`fs_target_max_c`
   first-tick pattern (`profile_executor_run.c`). The hook exists:
   `profile_executor_firing_stats.c`'s `firing_stats_maybe_finalize()`
   (called from `profile_executor.c:326`) is the one run-boundary call site;
   the header's own "INTEGRATION POINT" comment documents it in full but
   nothing calls into `iter_tune_process_firing()` from there today.

2. **Evidence it works.** `test_iter_tune.c` (501 lines, 14 tests) is solid
   pure-logic coverage: seeding, perturb/revert bounds, clamp, both floors,
   all three incomparability reasons, and a regression test built from real
   confounded capture data. But it is logic-only — no test exercises the
   integration this brief is about, because that code doesn't exist. The
   accept threshold's basis is documented in unusual detail in
   `iter_tune.h` (lines 70-218) and `PID_EXPANSION_PLAN.md` §3.3 (~line
   1070+): the noise floor is measured (six-repeat `noise_floor.json`,
   z0/z1/z2 std_c 0.053/0.032/0.054), converted to a two-sample 97.5%
   prediction interval (0.19/0.12/0.20 °C), and the header itself concludes
   the pure-relative 20% threshold falls *below* z2's floor at today's
   measured baseline magnitude — 0.175 °C required vs 0.197 °C noise, a
   0.89× margin, unsafe — which is why `ITER_TUNE_MIN_ABSOLUTE_IMPROVEMENT_C
   = 0.20` was added as a max()-with-relative floor. That fix is applied in
   the shipped code (`iter_tune_process_firing()` line 125-127) and both
   memory items ("relative threshold vs absolute floor", "IAE noise floor
   unknown") are accurately reflected in the header/plan, not stale. So:
   yes, defensible today, with an explicit caveat the header itself states —
   the six-repeat sample wasn't perfectly like-for-like (start temps span
   1.29 °C), so the floor is called a conservative overestimate, not exact.

3. **Cost.**
   - **DONE (2026-09-06, prep only, decision-neutral):** `start_temp_c`
     capture landed as `zone_runtime_t.fs_start_temp_c`
     (`profile_executor_internal.h`), written by `firing_stats_zone_tick()`
     (`profile_executor_firing_stats.c`) at this zone's first accumulated
     tick -- not in `profile_executor_run.c` as originally sketched, since
     the tick loop already has the exact "first accumulated tick" moment
     `iter_tune_firing_t.start_temp_c`'s doc comment asks for, matching the
     existing `fs_*` running-accumulator pattern instead of a separate
     warm-start-style read. Surfaced live-only as `profile_exec_zone_
     status_t.start_temp_c` / `/api/profile_exec`'s per-zone
     `start_temp_c` -- NOT added to `profile_exec_firing_stats_t` itself,
     because that struct is embedded byte-for-byte in `profile_firing_
     history_blob_t`, persisted to NVS as a fixed-size versionless blob;
     widening it would be a schema bump, refused as out of scope for this
     reversible pass. So the persisted run-history JSON does not yet carry
     `start_temp_c` -- adding it there still needs the schema-bump call
     this brief flags as a separate, larger decision. `iter_tune_process_
     firing()` is still not called from anywhere.
   - **Remaining wire cost:** add a
     persisted `iter_tune_zone_state_t` per zone — new NVS namespace per the
     header's explicit instruction not to reuse `adap_tune`'s (~40-60 LOC in
     `kiln_cfg_store.c`, a schema/version bump); call
     `iter_tune_active_gains()` at run start and
     `iter_tune_process_firing()`/`iter_tune_propose_perturbation()` at
     finalize (~30-40 LOC in `profile_executor.c`/`profile_executor_firing_
     stats.c`); an enable/disable + status surface needs *some* UI/HTTP
     exposure (`zones_http.c`) since it's opt-in per zone — another ~50-100
     LOC there plus a settings-page control. Total order: ~150-250 LOC
     across 4-5 files, one schema bump, one new UI control. No hardware
     validation of the mechanism itself exists yet (only its unit logic).
   - **Delete:** `control/iter_tune.c`, `control/iter_tune.h`,
     `test/test_iter_tune.c`, its registration in `test_main.c`; the "built"
     line and its evidence blocks in `PID_EXPANSION_PLAN.md` (§3.3, ~lines
     857, 1070-1095); the `tools/drivers_reorg/mapping.csv` census rows
     (2 lines, harmless either way). Grep across the whole tree (including
     `tools/PcTools`) finds no other references — the module is fully
     self-contained dead code today.

4. **Recommendation: wire it, don't delete.** Deciding fact: the accept
   threshold is not a guess needing more design — it is a already-derived,
   already-fixed number backed by a real six-firing measurement campaign,
   and the only missing piece to make the module live is the one
   `start_temp_c` field plus routine plumbing at a call site the header
   already pinpoints exactly. Deleting throws away validated logic and a
   non-trivial noise-floor derivation to save at most ~250 LOC of
   well-specified plumbing.
