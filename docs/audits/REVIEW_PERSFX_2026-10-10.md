# Review: persist-batch fixes a433b9637 (M1, L1-L3, N1-N3)

Date: 2026-10-10. Reviewer: Opus. Scope: origin/dev commit `a433b9637`, which addresses
findings M1, L1-L3 and N1-N3 of `docs/audits/REVIEW_PERSIST_BATCHES_2026-10-10.md`.
Reviewed in a clean worktree detached at `a433b9637`. No code was changed.

## Verification run

- Full KilnFW host tests (`build_host_tests.ps1`, no `-Only`): **Built: 86/86 executables, all
  passed.** `$totalExpected = 86` (`build_host_tests.ps1:3416`) was not changed by the commit,
  and no test executable was dropped. The fix report's "83/83" was a stale or filtered count,
  not a missing test.
- Negative tests with `tools\negtest.ps1`. Each ran in a throwaway copy and rebuilt from
  scratch. The results are below in "Negative tests".

## Findings

### MED-1: refuse-until-reboot is only visible in logs

`pref_cfg_fs_save` refuses a rev-unknown path with `ESP_ERR_INVALID_STATE`
(`drivers/persist/pref_cfg_fs.c:366-370`). `nvs_save` likewise refuses while the zones load is
undecided (`drivers/persist/zones_config_store.c:896-899`). Each refusal is surfaced only as an
`ESP_LOGE` line:

- Nothing outside `pref_cfg_fs.c` calls `pref_cfg_fs_rev_unknown()`. No readiness item, no
  `/api/cfgfs` field and no status flag reports which stores are frozen this boot.
- An operator write that hits the refusal is answered by `cfg_fs_http_persist_failed()`
  (`drivers/common/cfg_fs_refusal_http.h:56-64`) with a generic `500 "could not be saved to
  flash"`. The fix status line in the source review (L1, "HTTP 409") is inaccurate. No route
  maps `ESP_ERR_INVALID_STATE` to 409, and the body never says "reboot to retry".
- Automatic writers fail for the rest of the boot. relay_cycles, adaptive_tune ki_base and
  iter_tune each log the failure and keep the value live in RAM, which is the documented
  convention. No caller was found that ignores the error and then reports success. However,
  one transient read error at boot leaves every later automatic save of that store refused
  until reboot, and nothing persistent tells the operator.

Recommendation: expose the set of rev-unknown paths, plus the zones-undecided flag, in
`GET /api/readiness` or `GET /api/cfgfs`. Answer the refusal with 409 and a "reboot to retry"
body, so it is distinct from a genuine flash failure.

### MED-2: one zones POST clears the load fault while saves stay refused

`zones_http_post.c:771-781` commits the submission to RAM, sets `s_zones_config_valid = true`
and calls `zones_config_load_fault_clear()`, all before `nvs_save()` (line 793). On an
undecided boot (an L1 file read error that latches UNREADABLE, `zones_config_store.c:699`),
the save is then refused, and the handler returns a 500 at line 829.

`zones_config_load_fault_clear()` does not clear `s_zones_cfg_undecided` (only
`zones_config_load_fault_reset_for_test()` at line 116 does). So after that single POST:

- the fault banner is gone;
- the firing refusal in `profile_executor_run.c:323-337`, which keys on `zones_config_is_valid()`
  and the latched load fault, no longer applies;
- the kiln can fire on a config that lives only in RAM;
- every later save is still refused, with log lines only.

That combines a cleared safety signal with silent non-persistence. Either keep the fault
latched (or re-latch it) when the following save is refused, or refuse the POST outright with
409 while undecided.

### MED-3: M1 adopts a frozen legacy NVS copy for control- and safety-relevant stores

The NVS dual-write was closed on 2026-10-06 (`af6e12ebd`). Since then the cfg file is the only
save target, and every surviving NVS copy is frozen at whatever it held at that date. M1 now
prefers that frozen copy over defaults whenever the file is unreadable or a scratch allocation
fails (`pref_cfg_fs.c:464-470` and `:572-578`), and it logs only "unreadable".

For several stores the frozen copy can be worse than the defaults:

- **aux_outputs** (`drivers/persist/aux_outputs_cfg.c:196`). The NVS aux blob was written only
  between `c26f3a435` (2026-10-04) and `af6e12ebd`. A stale *enabled* aux rule could be
  re-adopted in place of the safe defaults (all disabled), turning a relay on that the
  operator has since disabled.
- **ramp_assist** (`drivers/control/ramp_assist_cfg.c:125`), **ki_base**
  (`drivers/control/adaptive_tune.c:1471`) and **iter_tune**
  (`drivers/persist/iter_tune_store.c:211`). Stale tuning feeds the control loop.
- **ct_verify** passes `nvs_rev` 0, so its copy carries no rev information at all.

The original M1 concern ("never drop a valid value for defaults") is right for the stores
that are still written. For stores whose NVS writers are retired, a per-store policy is
needed. Aux should at least fail safe (disabled) rather than use the NVS copy, and the
adoption should be surfaced together with MED-1.

### LOW-1: relay_cycles retries at 1 Hz during a firing; counts can read 0

`persist_snapshot_now()` (`relay_cycles.c:1151-1192`) leaves `dirty` set and does not advance
`last_persist_us` on failure. `relay_cycles_maybe_persist()` (`:1200-1215`) is called on every
1 Hz executor tick (`profile_executor.c:2095`). Once the 600 s interval has elapsed, a refused
save is therefore retried every second for the rest of the firing. Each retry dispatches a
flash-worker job and writes about three log lines. The same storm already existed for an
unmounted cfg, but M1 adds a new persistent refusal source.

If the legacy NVS key was erased after migration, the counts also read 0 for that boot, which
the module itself calls "the one wrong answer that matters". Back off on
`ESP_ERR_INVALID_STATE` (for example, advance `last_persist_us` anyway).

### LOW-2: the rewritten zones rev-floor assertion is vacuous

In `test/test_zones_config_cfg_fs.c:1090-1108`, `oom_check_rev_floor_kept()` now does a
refused save, then a clean `nvs_load`, then a save, and finally asserts `rev > 1`. After the
clean reload the rev comes from the rev-5 file, so the save stamps 6 whether or not the OOM
path kept the floor. That assertion no longer tests the floor.

The refusal assertion at line 1095 does still guard against a clobber, so this is a lost
assertion rather than a lost safety property. See the negative-test results below.

### LOW-3: the pref scratch-alloc path's unknown mark is not asserted

`test/test_persist_campaign10.c:446-453` was changed from `!rok && valid && rev == 9` to
`rok && valid && rev == 9`. It no longer checks `pref_cfg_fs_rev_unknown("big.bin")` or a save
refusal for the scratch-OOM branch (`pref_cfg_fs.c:574`). Also, `bout` is overwritten by the
following `load_raw` before the check, so the "NVS kept in RAM" half of the message is not
checked either.

K10-10 itself (line 440) was rewritten to `rok && !used && rev == 3 && bout[1] == 6 &&
unknown`, which is *stronger* than before. The zones "later save lands" assertion (line 1106)
is a new positive check, not a weakening.

### LOW-4: two zones cannot-decide paths latch no fault

`zones_cfg_mark_undecided(..., false)` is used for the NVS decode OOM
(`zones_config_store.c:608`) and the scratch OOM (`:635`). Saves are refused, but no load
fault is latched, so there is no banner and no firing refusal. The board runs on a zeroed or
default config with persistence silently disabled. Only the file-read path (`:699`) latches.

### LOW-5: backup export can carry the stale adopted value

Backup export reads RAM. After an M1 adoption, it exports the frozen NVS value (see MED-3)
with no marker. A later import writes that value back as if it were current. Backup import
itself checks setter results (`backup_import.c:3407-3425`), so a refusal during import is
reported as a partial import, which is correct.

### Reset-one-side check (rev-unknown mark)

- Factory reset ends in `esp_restart`, so the RAM-only mark cannot outlive it.
- A cfg format only runs when a format is pending, which means cfg was not mounted at boot and
  no marks could exist.
- Backup import goes through the refusing setters and reports the refusal (see LOW-5 for the
  export side).
- A clean re-resolve clears the mark (`unknown_set(rel_path, false)`).

No paired-state break was found beyond LOW-5.

### NITs

- The unknown-path table (`pref_cfg_fs.c:295-321`, 32 slots) drops an entry silently on
  overflow. That is not reachable with today's roughly 16 paths, but it should log.
- `cfg_fs_test_inject_read_error()` is compiled into firmware, with unsynchronized statics and
  no on-device caller. It should be test-only.
- `pref_cfg_fs.c:460-461`: the comment "adopt nothing, write nothing, report failure" now
  contradicts the M1 branch just below it, which keeps the NVS copy.
- `zones_page.html` `atCeilingMessage()`: the `REJECTED_OUT_OF_RANGE` line is mis-indented.
- The fix report's "83/83" host executables: the real count is 86/86 (see above).

## Negative tests

Each mutation ran through `tools
egtest.ps1` with
`-Command "build_host_tests.ps1 -OutDir {OUT} -Only <regex>"` and
`-ExpectPattern 'RUN FAILURES \('`. Each unmutated baseline passed.

| Mutation | Selection | Result |
|---|---|---|
| A: `pref_cfg_fs.c:574`, skip `unknown_set(rel_path, true)` in the scratch-alloc failure branch | `-Only pref_cfg_fs` | **MISSED** (exit 0). Confirms LOW-3: nothing asserts the unknown mark or the save refusal on that branch. |
| B: `zones_config_store.c:634`, drop `s_zones_cfg_rev = nvs_rev` (the rev floor) in the scratch-OOM path | `-Only zones` | **MISSED** (exit 0). Consistent with LOW-2: the rewritten floor assertion does not detect a lost floor. The save refusal still prevents a clobber on that boot. |
| C: `zones_config_store.c:896`, `nvs_save` ignores `s_zones_cfg_undecided` | `-Only zones` | **CAUGHT** (`RUN FAILURES (1)`). The L1 refusal is covered. |

Both runs ended with negtest's "REAL TREE CHANGED" error. The only differences were this review
file and the reviewer's own `hostbuild/` output, both created in the worktree during the runs.
No source file changed, so the CAUGHT/MISSED verdicts stand.

## Fix status (persfx2 batch)

Fixed in `a5303da6b (tests follow in 94fe879fc, 0459bf715)`. Host tests 86/86 (the earlier "83/83" was a stale count).

- MED-2: `POST /api/zones` answers 409 `zones_config_undecided` before the commit point while the boot could not
  decide the stored config, and the load fault is cleared only after `nvs_save()` returns ESP_OK. Test:
  `test_zones_post_refused_while_load_undecided_keeps_fault`.
- MED-1: a degraded-store registry (`cfg_fs_degraded_*`, `cfg_fs.c`) records every rev-unknown store and the
  undecided zones config. `GET /api/cfgfs` reports `degraded_count`/`degraded`; the shared HTTP persist-failure
  helper answers 409 `store_unreadable_at_boot` ("reboot to retry") instead of the generic 500. The bounded
  boot-read retry was deliberately not added: it would change the existing OOM/injection timing tests for a
  transient that a reboot already clears.
- MED-3: `pref_cfg_fs_resolve_nvs_retired()` keeps the store at its safe default (and marked unknown) on an
  unreadable file. Used by aux_outputs, ramp_assist, ki_base (adaptive_tune), iter_tune and ct_verify. NVS
  adoption stays (stale is harmless) for the display/unit/relay-name/preference stores and relay_cycles,
  which are cosmetic or wear counters that only ever raise.
- LOW-1: a failed periodic relay_cycles persist stamps the attempt time, so the retry waits a full interval.
- LOW-2/3: rev-floor and unknown-mark asserts now discriminate (mutations A and B).
- LOW-4: both zones OOM cannot-decide paths latch the load fault.
- LOW-5: backup export lists `stale_or_unknown_stores` when any store is degraded.
- NITs: loud log on unknown-table overflow, test inject seam excluded under `ESP_PLATFORM`, stale comment
  rewritten, `zones_page.html` indent.
