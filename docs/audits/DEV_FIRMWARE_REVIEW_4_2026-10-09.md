# Dev firmware review 4 (2026-10-09)

Scope: every commit in `754436e1..origin/dev` at `8340e540` (oldest first):
9e664dfb, c231c97c, d4599866, 009f91fb, 139471ff, 15748a19, 7beee17c,
6d6d04e5, 70ec462c, e2978098, d56fa877, 45c2b4de, 8340e540, plus 676c503f
(persist scratch mallocs). Code review only: no builds, no board access.

No Wi-Fi legacy-erase, run_state migration, update-chain or CSRF commits were in
the range when it was reviewed.

## HIGH

### H1. WEB-KCFG-02 leaves a bench config active and loses the original active id

- Where: `tools/PcTools/src/kilnctrl/bench_test/cases_web_misc.py:403` (save), `:441-443` (cleanup).
- Firmware: `firmware/KilnFW/App/drivers/persist/kiln_cfg_store.c:1275-1277`.

What happens:

- `POST /api/kiln_configs/save {"name":"BENCH_tmp"}` calls `kiln_cfg_store_save_current(id=-1)`.
- That sets `s_store.active_id = id` to the new entry (`:1277`).
- It also drops the previous active slot's pico-half recapture flag (`:1275`).

Why the restore fails:

- The cleanup in `finally` skips only `snap["active_id"]`, the original id. It deletes every other created id.
- The firmware refuses to delete the active config (~`:1676`). BENCH_tmp is now active, so its delete is refused.
- Nothing re-activates the original. The only route back is `/api/kiln_configs/apply`, a real apply.

Scenario: after one run the board reports BENCH_tmp as its active kiln config. The original's pico-half dirty flag is gone, and every later run leaks another entry. The judge FAILs with "restore mismatch", but the mutation is already permanent.

Fix:

- Reduce the case to the rename/clone path on a non-active entry, or to read-only like KCFG-03/04.
- At minimum, refuse to run unless `save` is proven not to change `active_id`.

### H2. WEB-ZONE-02 POSTs the whole `/api/zones` page

- Where: `tools/PcTools/src/kilnctrl/bench_test/cases_web_prof.py:689` (identity write) and `:713` (restore).
- The whole-page `/api/zones` POST is on the forbidden list for web judges.

Why the identity write is not an identity:

- The body is rebuilt from GET output (`zones_http_client.build_post_body`), so PID gains and limits go back at GET's printed precision. The narrow writers document the same `%.4f` residual.
- The `safety_ceiling` precheck guards only the Pico `abs_max_temp_c` resync.

Why the restore cannot work:

- It re-posts the same rounded `fields`, so it can never restore true values once the first POST rounded them.
- The read-back compares GET to GET, which is equally rounded. That comparison can report "restored" while tuned gains have drifted in their 5th+ significant digit.

Fix: drop the case, or keep only a 409/400 refusal probe that never sends a valid body.

## MED

### M1. Aux fault-drop does not cover an aux relay that is ON but no longer enabled or claimed

- Where: `firmware/KilnFW/App/drivers/control/profile_executor_relay_io.c:599`, `cand = aux_outputs_cfg_enabled_mask() | s_exec.aux_claim_mask`.
- Related: `firmware/KilnFW/App/drivers/http/aux_outputs_http_core.c:71` `core_set_locked()` persists `enabled=0` but never drives the relay OFF.

Scenario:

1. Manual aux ON.
2. `POST /api/aux_outputs` with `enabled=0`. The relay stays energized; the relay shadow still shows it ON.
3. An IDLE safety fault or Pico trip occurs. `cand` excludes the bit, so fault-drop never writes OFF.
4. The watchdog only logs an IDLE trip (`LOG_IDLE_TRIP`) and does not call `kiln_io_all_relays_off`.

Result: the relay stays ON after a fault, which breaks the owner rule that aux outputs are driven off on any safety fault. A spare relay switched ON through the raw dashboard relay path has the same gap.

Fix:

- Build `cand` from the relay shadow over every non-zone relay, not from config or claim.
- Also make `enabled=0` write OFF.

### M2. Profile rev-floor junk repair can clear the fail-closed latch with zeros

- Where: `firmware/KilnFW/App/drivers/http/profiles_http.c:844` `rev_repair_junk()`, called at `:936` and `:1096`.

The repair only raises slots not in `used` to `maxrev`. That has two consequences.

(a) A used slot with NVS-only data and no cfg file keeps rev 0. The commit message says file-less slots get raised.

(b) The degraded-boot scenario:

1. Boot with `cfg_fs` unavailable. Every slot looks file-less, so `maxrev = 0`.
2. The repair writes zeros, verifies, and clears `s_profile_rev_unknown`. Saves are re-enabled at rev 1.
3. Edits are made during that boot.
4. On a later boot with cfg mounted, the older cfg files carry rev N > 1 and win `profiles_cfg_fs_resolve`.

Result: the edits from the degraded boot are silently reverted. The pre-70ec462c fail-closed refusal prevented exactly this.

Fix:

- Repair only when `cfg_fs_is_available()`.
- Raise every slot that lacks a file rev, used or not.

### M3. Web judge write gates are fail-open on suite

- `cases_web_prof.py:61` `_mutating_gate()` has no suite check at all. It relies on the board lock, and a direct call (MCP, test harness, ad hoc) writes freely.
- `cases_web_diag.py:50-53` `mutating_gate()` and `cases_web_safety.py:101-103` `_mutating_gate()` allow writes when `ctx` has no `"suite"`.
- The runner sets `"suite"` (`runner.py:438/453`), so a normal run is safe. Any other caller of a writing judge is not.
- `cases_web_diag.py:62` also accepts an autotune state of `None`, so a response missing `"state"` passes the gate.

Fix: refuse unless `suite` is present and mutating, and treat a missing state as not idle. Another session is already working on this ("fail-closed suite gate").

## LOW

### L1. Fault-drop retry logs are only partly rate-limited

- Where: `profile_executor_relay_io.c:513`, inside `aux_apply_relay()`.
- Every fault-drop retry tick emits `aux_apply_relay`'s own `ESP_LOGW` "aux relay write failed". 45c2b4de rate-limited only the fault-drop logs.
- A failed OFF still calls `relay_cycles_add()` for a transition that never happened, which inflates wear counters.
- `commanded_on` is set to the wanted state even when the write fails.
- With `s_exec.io == NULL`, the manual branch reports `off_ok` without writing. This is host-only.

### L2. Backup import checks the builtin catalogue size only at commit time

- Where: `firmware/KilnFW/App/drivers/http/backup_import.c:3285`.
- `g_builtin_profile_count > BACKUP_HIDDEN_MASK_BITS` is checked during apply, after earlier prefs are already written, so it produces a partial import.
- Make it a pass-1 check, or a `_Static_assert` if the count is a compile-time constant.

### L3. Auth window judges rewrite the admin password from the environment

- Where: `cases_web_misc.py` `_open_window()` (LOG-02, X-02).
- `set_web_password(user, pw)` re-sets the admin password from `KILNCTL_WEB_PASSWORD`. If the environment value differs from the board's, the board password changes permanently, and `_close_window()` restores policy only.
- LOG-02 also burns one failed-login attempt per run against the lockout counter.
- Fix: verify by `login()` first and skip `set_web_password` when it already succeeds.

### L4. WEB-SET-04 (setup progress) cannot restore timestamps

- Where: `cases_web_safety.py` (setup/progress writer, ~`:800-835`).
- The restore re-posts state and note, but each step's `ts` is rewritten. The judge documents "ts excluded", so this is an accepted residual. It is noted here because it is a permanent change on every mutating run.

## Checked, no defect found

- 676c503f: `persist_scratch_alloc` falls back to malloc and frees with `free`.
- d56fa877: the hidden-builtins parse bound is fine (apart from L2).
- `cases_web_rw.py`: the POST deny-list matches the firmware Wi-Fi routes (`/provision`, `/forget`, `/ip_config`, `cmd=clear_credentials`).
- A grep of every `cases_web_*.py` found no call to `load_config_preset`, `factory_reset` (SAF only inspects the page JS), `estop_verify`, `autotune/accept` (listed only as forbidden), `backup/import`, `update/stage/clear`, `debug_write_memory` or a flash. H2 is the only forbidden action found.
- COMM-07 relay_type uses the narrow `/api/safety/commissioning/relay_type` same-value write, with read-back restore.
- TZ/unit and DISP-02 restore in `finally` and FAIL on a read-back mismatch.
- RDY-04: `POST /api/profile_exec/start` with `probe=1` cannot start a firing, for two reasons:
  - The firmware has no `probe` field (`dashboard_exec_http.c:677`).
  - The case fires only inside the OT-B01 tripped window, so the readiness interlock answers 409 first. If the gate ever passed, the body has no `id` and gets a 400.

  It is safe, but only by those two facts, not because a real probe mode exists. Do not reuse the `probe=1` form elsewhere.
- 8340e540 `run_start_probe`: read-only (`GET /api/auth/config`, `/status`), runs only after a passing preflight.
- WIFI-05 never sends a Wi-Fi write: a denied path raises before transport.
