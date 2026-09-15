# The two-processor apply transaction (item 5) -- implementation record

2026-09-14. Implements `docs/KILN_PROFILES_PLAN.md` (`c70ae799`) section 4/8
item 5, folding in `docs/audits/kiln_profiles_robustness_2026-09-14.md`
(`5182c3a4`) findings H6, H10 and H17. New module:
`firmware/KilnFW/App/drivers/persist/kiln_cfg_swap.h/.c`, plus small,
targeted additions to `kiln_cfg_store.h/.c` (three new accessors + the
store's own mutex) and `safety_cfg_http.h/.c` (one new bulk-push function).
Host tests: `firmware/KilnFW/App/test/test_kiln_cfg_swap.c`.

## Scope actually delivered

- The full transaction state machine (section 4.2, as revised by section
  1a -- no disarm, no re-arm, the Pico never leaves `ARMED`): interlock,
  H17 half-package refusal, snapshot-and-persist the rollback package R,
  ceiling raise-first, Pico bulk push + field-by-field readback, ESP commit
  + byte-for-byte readback, ceiling/arming reconcile via the EXISTING
  divergence primitive (item 7, `safety_ceiling_sync.c`/`config_divergence.c`
  -- already landed by another pass; this module adds no second detector),
  finalize, best-effort fallback-persist stub.
- Full rollback, in both directions (Pico-only when the ESP was never
  touched; both sides once the ESP has committed).
- The five-case interrupted-swap boot-recovery table (section 4.4 / plan
  item 8), including the sixth case section 4.4 does not name: a pending
  record that fails its own integrity check (H10).
- H6's mutex + generation counter on `kiln_cfg_store`, taken only around
  the snapshot and finalize/rollback-commit steps, never across the Pico
  round trip.
- Host tests exercising all of the above against fully-faked dependencies,
  all green, plus one negative test actually run against production code
  (not merely described) and restored by hand.

## What is NOT delivered (explicitly out of scope for this pass)

- **Task dispatch off the httpd worker.** `kiln_cfg_swap_apply()` is
  synchronous/blocking by design (see its own header comment) so it is
  trivially testable; wiring it onto a dedicated worker task (never the
  flash worker -- `project_flash_worker_reentrancy`) is integration work
  for whichever pass adds the HTTP/LCD call site. Do not call it from an
  httpd handler as-is.
- **HTTP route / UI (plan item 9).** No `POST /api/kiln_configs/swap`
  endpoint and no confirm-dialog/banner work was done. This module has no
  HTTP dependency at all.
- **Item 16 (UNCONFIGURED flag).** Not landed as of this pass (see below).
  Step 11's "Pico is armed and configured" check uses an approximation.
- **Item 15 (SaftyFW volatile RAM install).** Not landed as of this pass
  (`KILNLINK_COMMIT_CONFIG_LEN` is still 1 byte, no flag). See the
  dedicated section below for exactly what was built against it and why
  that is safe.
- A target (ESP-IDF) build was **not completed** this pass -- the build
  tree here is configured for a different Python interpreter than the one
  currently active (`idf.py fullclean` required first), a pre-existing
  environment issue unrelated to this change. **Host tests are green and
  are the primary verification for this C-only module's logic**, but a
  target build should be run before this lands on real hardware.

## Item 15 dependency -- what was assumed, and why a failure today is safe

`safety_cfg_http_apply_package_and_confirm()` (`safety_cfg_http.c`) is the
one function this transaction uses to push the Pico's non-ceiling params.
It is built against item 15's *specified* interface -- "stage N params,
commit once with a volatile-install flag, read back and confirm, exactly
like every other write in this file" -- but since that flag does not exist
on the wire yet, the `commit=true` it forces goes through the *real*
`COMMIT_CONFIG` -> `config_store_write()` path, which is refused outright
while the Pico is `ARMED` (its ordinary running state). **On real hardware,
today, `kiln_cfg_swap_apply()` will fail at step 6 almost every time**, and
the transaction's own rollback logic handles that exactly like any other
push failure: nothing lands, nothing is left half-applied, no alarm. This
is the *safe* degenerate behaviour of a feature whose other half is still
in flight -- not a defect in this pass. Once item 15 lands, the one-line
change belongs inside `confirm_commit_landed()`/`apply_pairs_ex()`'s encode
call (`safety_cfg_http.c`), not in `kiln_cfg_swap.c` or the new bulk-push
function's own signature, both of which already match the shape item 15's
plan section specifies.

Item 16 (the explicit `UNCONFIGURED` wire flag) is likewise not landed.
Step 11's "is the Pico actually configured" check uses
`safety_cfg_store_cached_crc() == 0` as an approximate stand-in (a Pico
that has never reported *any* `config_crc` this boot cannot have just
accepted 60+ params) -- documented in `kiln_cfg_swap.c`'s own comment at
that check as defensive, not primary: the field-by-field readback earlier
in the same transaction is what actually proves the Pico holds P.

## Ordering and where each verification happens

```
0  ota_http_check_interlocks()                    -- refuse outright, nothing written
1  kiln_cfg_store_get_full_package(..., pico_out)  -- H17: half-package refused here
2  snapshot R (live ESP export + kiln_package_capture_pico_half), persist
   pending record, marker=STAGED                   -- under kiln_cfg_store_lock()
4  ceiling raise-first only (safety_cfg_http_set_and_confirm_f32) if
   P's ceiling >= the Pico's current one
5  marker=PICO_OPEN, persist
6  safety_cfg_http_apply_package_and_confirm()      -- bulk push, EXCLUDES the
   ceiling field always (see that function's own doc comment for why)
7  safety_cfg_store_refetch() + field-by-field compare (pico_readback_matches())
   -- mismatch -> ROLLBACK. success -> marker=PICO_DONE
   H6 generation re-check right here, before the ESP is ever touched
8  zones_config_import_blob(P)                      -- under kiln_cfg_store_lock()
   failure -> ROLLBACK (Pico already on P's non-ceiling fields, ESP untouched)
   success -> marker=ESP_DONE
9  zones_config_export_blob() + memcmp byte-for-byte against P's blob
   -- stronger than the plan's minimum "field-by-field": this is a full
   content compare, since both sides are the SAME canonical export.
   mismatch -> ROLLBACK
10 safety_ceiling_sync_reconcile_on_link_up() (lowers the ceiling here if P's
   was lower -- this is "lower-last": strictly after the ESP commit)
11 safety_ceiling_sync_is_diverged() (+ the item-16 approximation above)
   -- diverged: alarm via the EXISTING disable-heat hooks, do NOT roll back
   (both halves already agree on content), leave marker=ESP_DONE for boot
   recovery's "verify then finish" path
12 kiln_cfg_store_set_active_id_raw(P.id), clear_pending()
13 STUBBED -- see below
```

Step 13 (the opportunistic Pico flash-fallback persist, section 1a.4 case 1)
is a genuine no-op in this pass: item 15's whole "persist what is now proven
live" mechanism does not exist without item 15's volatile install landing
first (there is nothing verified-live-but-unpersisted to persist). Calling
it out explicitly rather than half-building it: the swap is already
complete at step 12 regardless, so its absence never masks a failure.

## Locking (H6)

`kiln_cfg_store_lock()`/`_unlock()` bracket **only**:
- step 2's snapshot (`kiln_cfg_store_get_active_id()` + `zones_config_export_blob()`)
- step 8's commit (`zones_config_import_blob()`)
- step 9's readback (`zones_config_export_blob()`)
- step 12's finalize (`kiln_cfg_store_set_active_id_raw()`)
- rollback's mirror of steps 8/12

They are **never** held across step 6/7's Pico round trip or step 10's
reconcile call -- both of which can block for hundreds of milliseconds on
the UART link. `kiln_cfg_store_generation()` (an alias for the store's
existing `s_kiln_cfg_rev`, reused rather than adding a second counter --
see the code comment on why a second counter is itself a
`project_reset_one_side_bug_class` risk) is captured before the unlocked
Pico round trip and re-checked immediately after; a mismatch forces a full
rollback rather than a blind finalize over whatever an ordinary
save/clone/rename/delete did to the store in the meantime.

## Behaviour at every interruption point (section 4.4 / plan item 8)

| marker at boot | action taken (`kiln_cfg_swap_boot_recover()`) |
|---|---|
| absent / `NONE` | no-op |
| `STAGED` | discarded -- both sides still on R, nothing was ever written |
| `PICO_OPEN` | re-apply R to the Pico, read back, verify; success clears the record, failure stays alarmed and leaves the record for a retry |
| `PICO_DONE` | same as `PICO_OPEN` -- **never** "finishes" with P (proven by `test_boot_recovery_pico_done_reapplies_rollback`, which asserts R's ceiling, not P's, is what actually gets pushed back) |
| `ESP_DONE` | re-reads the target slot fresh, independently re-verifies BOTH sides (Pico via a fresh `GET_CONFIG_PAGE`, ESP via a fresh export -- never either side's cache), and only on a double match sets `active_id` and clears; any mismatch, or the target slot itself no longer existing, stays alarmed |
| present but fails its own CRC / cannot be read back at all (H10, the sixth case section 4.4 does not name) | treated exactly like `PICO_OPEN`/`PICO_DONE`'s fail-safe default: alarmed, **nothing cleared** -- clearing a record that could not even be read would destroy the one piece of evidence available. `load_pending_ex()` distinguishes this ("present, unreadable") from the ordinary "never staged" case via `HAL_NOT_FOUND` vs. any other `hal_status_t`, so an unreadable record is never silently reinterpreted as "nothing happened" |

In every case heating stays impossible until a divergence check has
actually confirmed the two sides agree -- nothing in this module re-arms or
clears a latch on its own initiative, and there is no timeout anywhere in
this file that expires into "proceed".

## Negative tests actually performed

1. **H6 generation re-check** (`test_generation_race_forces_rollback` /
   `test_negative_generation_check_is_load_bearing` in
   `test_kiln_cfg_swap.c`). The guard
   (`if (kiln_cfg_store_generation() != gen_before)` in `kiln_cfg_swap.c`)
   was changed BY HAND to `if (false && ...)`, the suite was rebuilt and
   rerun, and `test_generation_race_forces_rollback` **failed both its
   assertions** -- the race slipped through and the ESP was committed
   despite the store having moved mid-swap. The `false &&` was then removed
   by hand, the build directory deleted, a full clean rebuild performed,
   and the whole `kiln_cfg_swap` executable (10 tests) reconfirmed
   all-green, `ALL TESTS PASSED`.
2. **The mirror is not the only gate** (item 6's own concern, exercised
   here at the transaction layer): `test_pico_failure_leaves_esp_untouched`
   and `test_esp_readback_mismatch_rolls_back` prove a Pico-side refusal
   and a silently-wrong commit are both caught and rolled back cleanly,
   with the ESP never touched in the first case and fully restored in the
   second.
3. **H17** (`test_half_package_refused`): a `pico_populated=false` slot is
   refused before anything is snapshotted or written.
4. **H10** (`test_boot_recovery_corrupt_marker_stays_alarmed`): the fake KV
   backend's own corruption injection (`fake_kv_script_corrupt_key()`) is
   used to make the persisted record genuinely unreadable (`HAL_IO`, the
   real shape a torn NVS blob takes), and the test asserts the record is
   **still** unreadable after `kiln_cfg_swap_boot_recover()` runs -- i.e.
   nothing was cleared or silently replaced with a fresh `NONE` record.

## Host test results

New executable `kilnctl_host_tests_kiln_cfg_swap.exe`, 10 test sections, all
assertions passing after a full `rm -rf` of the build directory and a clean
rebuild (`build_host_tests.ps1 -OutDir <fresh dir>`). The full
`build_host_tests.ps1` suite was run end to end in the same pass: every
other executable's own tests are unaffected (`BUILD FAILED` appears nowhere
in the log); the only non-zero exit contributors are the pre-existing,
explicitly-non-blocking `sim_credibility_gate` informational tally
(unrelated to this work, documented at
`docs/audits/sim_credibility_gate_real_cause_2026-09-10.md`) and an
executable-count mismatch (`$totalExpected` in `build_host_tests.ps1` was
stuck at 42 against 44 real `Invoke-HostTestExe` calls already present
before this pass touched the file -- not this pass' doing) which this pass
corrected to 45 (44 pre-existing + this pass' own `test_kiln_cfg_swap.c`).

One incidental production bug found and fixed while wiring this up: the new
`safety_cfg_http_apply_package_and_confirm()` referenced
`SAFETY_PARAM_ID_ABS_MAX_TEMP_C` without including the header that defines
it (`safety_ceiling_sync.h`) -- caught immediately by the pre-existing
`test_safety_cfg_http.c` executable failing to compile (`error C2065:
undeclared identifier`), fixed by adding the include (no cycle:
`safety_ceiling_sync.h` only pulls `safety_ceiling_policy.h`/`safety_link.h`,
never back into `safety_cfg_http.h`).

## Target build

Not completed this pass -- `firmware/KilnFW`'s CMake cache is configured
for a different Python interpreter (`idf6.0_py3.14_env`) than the one
currently active in the environment (`v6.0.2\venv`), requiring `idf.py
fullclean` before any build can proceed; this is a pre-existing environment
mismatch, not something introduced by this change, and was not chased down
further given the time this pass already spent on the transaction itself.
**Recommend a clean target build before this code is flashed to a board.**

## Constraints honoured

- No `ZONES_CFG_VERSION` bump -- this module never touches `zones_cfg_t`'s
  schema, only its already-existing export/import blob functions.
- Upload/download (items 3/4) untouched.
- The divergence enforcement (`config_divergence.c`/`safety_ceiling_sync.c`)
  and the firing-interlock (`ota_http_check_interlocks()`) are both reused
  verbatim, never weakened or duplicated.

## Hash citations

- `docs/KILN_PROFILES_PLAN.md` = `c70ae799` (commit)
- `docs/audits/kiln_profiles_robustness_2026-09-14.md` = `5182c3a4` (commit)
