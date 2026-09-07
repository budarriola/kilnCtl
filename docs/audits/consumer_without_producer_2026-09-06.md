# Consumer-without-producer sweep, 2026-09-06

Mechanical sweep of `firmware/KilnFW/App/drivers/` (recursive, `test/`/`stubs/`
excluded): extracted struct fields from headers, checked every field read in a
`.c` file for at least one non-test assignment (`.field =`, `->field =`,
`field =`, plus `++`/`+=`/pointer-of/array-index/designated-initializer
forms). ~964 fields scanned mechanically, 149 raw candidates, false positives
(comment text, `++`/`+=` increments, `&s.field` task-handle patterns, array
index writes, `.inc`-table designated initializers not in the `.c` glob)
filtered out by hand. Two confirmed findings.

| Field | Reader | Evidence no writer exists | Severity |
|---|---|---|---|
| `thermal_guard_cfg_t.progress_band_c` | `control/thermal_guard.c:238` (`effective_f(cfg->progress_band_c, PROGRESS_BAND_C)`) | Every sibling guard-cfg field (`max_temp_c`, `wrong_dir_window_s`, `progress_duty_min`, `cross_zone_max_delta_c`, etc.) is set in the `(thermal_guard_cfg_t){...}` designated initializers at `control/profile_executor_run.c:548` and `control/autotune_engine.c:1148`; `progress_band_c` is absent from both lists, so it is always the compound literal's implicit 0. No `zones_config_get_guard_*` accessor exposes it either (checked `persist/zones_config_accessors.c`/`.h` — no such field). Unlike `cross_zone_max_delta_c`, which has an explicit comment saying it is deliberately operator-only with no substituted default, this field carries no such note — it looks wired but silently always falls back to the hardcoded `PROGRESS_BAND_C` default. | Medium — Guard 1's arrival-band threshold (thermal runaway/progress detection) can never be tuned per zone even though the struct and the `effective_f()` override mechanism exist for exactly that; not a live safety miss (default is applied), but any future zones-config UI wiring for it would be a no-op today. |
| `iter_tune_firing_t.start_temp_c` (and the whole `iter_tune` module) | `control/iter_tune.c:33` (`iter_tune_comparable()`, `fabsf(a->start_temp_c - b->start_temp_c)`) | Grepped the whole `firmware/KilnFW` tree for `iter_tune_process_firing`, `iter_tune_comparable`, `iter_tune_propose_perturbation`: the only non-header hits are `control/iter_tune.c` itself and `test/test_iter_tune.c`. No production caller ever constructs an `iter_tune_firing_t` (no `.start_temp_c =` anywhere outside the module/test), so this field's value is supplied only by hand in the host test. | Low today (the entire `iter_tune` module is unintegrated dead code — nothing calls it, so nothing downstream reads a wrong value in production) but High if it is wired up as-is: `iter_tune_comparable()`'s "rested baseline" check (directly related to the existing `project_autotune_needs_rested_baseline` note) would pass on an uninitialized/zero `start_temp_c` for every firing, silently disabling the very rested-baseline guard the module exists to provide. |

No other confirmed instances found in this pass (JSON-key producer check on
`http/*.c` and NVS-key load/save pairing in `persist/*.c` turned up no
additional candidates beyond the above — the safety-link stats/counters
initially flagged were all false positives, incremented via `++`/`+=` which
the first-pass regex missed).
