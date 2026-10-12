# Review: firelowfx3 (ec201e46d), 2026-10-11

Scope: commit `ec201e46d` on origin/dev. It fixes
`docs/audits/REVIEW_FIRELOWFX2_2026-10-10.md` MED-1 and LOW-1..3:

- The favorites store left unresolved at boot is now registered as degraded.
- `POST /api/profiles/favorite` answers 409 `store_unreadable_at_boot`.
- `profiles_favorites_set()` re-runs the load and heals the store.
- Load and save share one used-bitmap shape predicate.
- A 1-byte used blob counts as wrong-size.
- A used-bitmap probe error logs the slot and sets a `profiles_used_bitmap` degraded mark.

This review is read-only. It ran in worktree `C:\wt\rvflfx3_x59td6`, detached at origin/dev `295e50a9d`. Nothing was fixed.

## Verdict

No HIGH, no MED. The fixes do what the FX2 review asked, with one exception: the
operator recovery path for a bad favorites key is still not reachable from the
text the 409 shows (LOW-1). The new tests can fail. Five of six mutations were
caught; the one missed covers a redundant line.

## Evidence

- **KilnFW host suite**, full run through the build gate (`build_host_tests.ps1`,
  out dir `C:\wt\rvflfx3_x59td6_ht1`): all 98 executables built and passed.
- **Negtest** (`tools\negtest.ps1 -Command build_host_tests -Only profiles_http`,
  log `negtest_logs\20261011_230757_ebas`):
  - Save path reverted to the old predicate: CAUGHT at `test_profiles_http.c:2051`.
  - 1-byte blob accepted as legacy again: CAUGHT at `:2068`.
  - `cfg_fs_degraded_set(PATH, true)` dropped from `favorites_load`: CAUGHT at `:2166`, `:2170`, `:2171`.
  - Retry reload dropped from `profiles_favorites_set`: CAUGHT at `:2179`-`:2181`.
  - `profiles_used_bitmap` degraded mark dropped: CAUGHT at `:2130`.
  - `cfg_fs_degraded_set(PATH, false)` dropped from the top of `favorites_load`: MISSED. See INFO-1.
- **Side failures in the fix agent's negtest run** (`negtest_logs\20261011_221813_6p8h`,
  mutation m01, which ran alongside four other mutation builds):
  - `test_web_auth_store.c:394`
  - `test_update_fetch.c:469`
  - `test_update_fetch.c:489`

  See LOW-3 and LOW-4.
- **Stress runs** of the stock test executables:
  - `test_web_auth_store`, under 48 concurrent processes: 2 of 96 runs failed, both at `:394`.
  - `test_update_fetch`, under 16 to 64 concurrent copies: 0 of 176 runs failed.

## Findings

### LOW-1: the 409 recovery text does not fix a bad favorites key; the fix-status claim overstates the remaining path

The 409 body uses `CFG_FS_STORE_UNREADABLE_REASON`. That text offers three remedies:

1. Reboot.
2. Run cfg format (`force_healthy`).
3. Run factory reset with scope kiln.

None of them removes a persistently unreadable `prof_favusr` key in `profiles_nvs`:

- cfg format erases the `cfg` partition only.
- Factory reset with scope kiln does not erase `profiles_nvs`. Only scope
  profiles (`kProfilesOnly`) does, and that also wipes every user profile.
- A reboot re-reads the same key.

A key that keeps failing therefore leaves favorites refused permanently. The
operator gets advice that cannot work, and the only real cure destroys all user
profiles.

The FX3 fix-status section defers the non-destructive favorites reset, which is
FX2 MED-1 part 3, and calls the existing "cfg format / reboot text" the
remaining recovery path. That claim is inaccurate. MED-1 part 3 is still open.

This is LOW, not MED, because:

- The doc says openly that the reset route was deferred.
- Transient read failures now heal on the next POST.

Suggested fix: give favorites its own reason text naming
`factory_reset(scope=profiles)`, or land the narrow erase of `prof_favusr`.

### LOW-2: each refused retry briefly zeroes the builtin favorites that lock-free readers see

`favorites_load()` starts by setting `s_fav_user = 0` and `s_fav_builtin = 0`.
After that it reads NVS and the cfg file again.

`profiles_favorites_is()` and `profiles_favorites_masks()` read those words
without `s_save_lock`. Their callers are the LCD picker and the GET favorites
list, and they can observe the zeroed masks during a retry.

On a retry that fails again, `favorites_read_builtin_only()` restores the
builtin mask. If that read also fails, the builtin favorites stay 0 until the
next successful load.

Before this commit the unresolved state never wrote these words after boot.
The effect is cosmetic, a momentary empty favorites list, and it is
self-correcting.

Suggested fix: load into locals and publish under the lock only at the end, the
same pattern `aux_outputs_cfg` adopted for its LOW-3.

### LOW-3: the `test_web_auth_store.c:394` side failure is a real load-sensitive flake (unrelated to this commit)

`time_verify_calls()` times 20 PBKDF2 verifies, 2000 iterations each, with
`clock()`. On MSVC `clock()` measures wall-clock time.

Line 394 asserts that the unconfigured-role path and the wrong-password path
take times within 5x of each other. Under concurrent load, one batch can be
preempted far more than the other.

The stress run reproduced it: 2 failures in 96 runs at 48-way concurrency, both
at `:394`. negtest runs several mutation builds at once, which is exactly that
condition.

Suggested fix: time with `QueryPerformanceCounter` plus thread CPU time
(`GetThreadTimes`), or take the minimum of N runs per path, or relax the bound
when the process is not alone.

**Fix status:** fixed. `time_verify_calls()` now uses QueryPerformanceCounter and the minimum of 9 batches per path (load can only inflate a batch); the 5x assertion is unchanged. Stress: 128 runs at 32 parallel, 0 failures (review saw 2 of 96 before). Negtest: `rec.iterations : 1` for the unconfigured path in `web_auth_store.c` is CAUGHT (test line 412).

### LOW-4: the `test_update_fetch.c:469/489` side failure is a plausible real load race (not reproduced; unrelated to this commit)

`test_writer_wedge()` sets `g_fr_time_div = 1000`. That shrinks the writer wait,
`FETCH_WR_TIMEOUT_MS` = 30000, to 30 ms of wall time.

If the writer thread is not scheduled onto its blocking flash write within
30 ms of the job queuing it, the job declares the writer wedged first. The
assertions then fail:

- Line 469, `g_fr_flash_blocked_n >= 1`, fails because the writer never parked
  in flash.
- Line 489, `stage_phase() == UPDATE_STAGE_UPLOADING`, can fail for the same
  reason.

This matches the two lines that failed together in m01. The stress run did not
reproduce it (0 of 176), so it needs heavier load than the stress run produced.
It is a test-harness race, not a firmware defect: on target the bound is 30 s.

Suggested fix: have the test wait until the fake flash write is actually
blocked (`g_fr_flash_blocked_n >= 1`) before letting the scaled timeout run.
Alternatively, gate the scaled clock on that event.

**Fix status:** fixed (test stub only). The fake `xSemaphoreTake` now starts the scaled writer-timeout clock only once a writer thread is parked in flash (`g_fr_flash_blocked_n >= 1`, 15 s fallback), in `stubs_update_fetch/fake_support.c`. Stress: 128 runs at 32 parallel, 0 failures. Negtest: forcing `wedged = false` in `update_fetch.c` AND dropping the `update_wr_arb_caller_timeout()` call is CAUGHT (test:504); with the arbiter call kept it is MISSED (almost-equivalent mutant: the 1000-tick L1 grace take latches the wedge the same way; REVIEW_FLAKEFX LOW-2/INFO-1).

### LOW-5: `profiles_used_bitmap` in the degraded registry is reported as an unreadable boot store

`cfg_fs_degraded_set("profiles_used_bitmap", true)` puts a name that is not a
cfg file into the registry that both of these surfaces read:

- `GET /api/cfgfs`: at most 6 names, under a comment saying "saves refused until reboot".
- Backup export `stale_or_unknown_stores`: at most 8 names. The PC tool's
  `_stale_stores_note` warns that these stores "could not be read at boot".

Neither description fits:

- The mark can be set at any time, from a save or erase path, not only at boot.
- It does not refuse saves.
- It clears only on a later successful wrong-size rebuild, or at reboot.
- It also competes for the 6- and 8-entry listing caps.

It is a useful signal, but the label misleads.

Suggested fix: surface it as a separate diagnostics field, or document the
entry in both surfaces.

### INFO-1: the explicit degraded clear at the top of `favorites_load` is redundant and untested

`pref_cfg_fs_resolve()` calls `unknown_set(path, false/true)` on every path, so
it re-evaluates the same registry entry. That is why the
`fav_degraded_clear_dropped` mutation was MISSED.

The line is harmless. Either remove it or add a test where resolve
legitimately does not reach `unknown_set`, if such a path exists.

### INFO-2: the re-load under `s_save_lock` is sound

- Concurrent `set()` calls are serialized by the leaf `s_save_lock`. Only one
  re-load runs at a time, and each caller re-checks `s_fav_user_unresolved`
  after its own attempt.
- The NVS-to-file migration inside `pref_cfg_fs_resolve()` runs under the
  favorites lock. That is the same nesting as the existing commit path, so it
  adds no lock-order inversion against `cfg_save_lock.h`.
- `profiles_favorites_start()` runs at boot (`main_network_http.c:417`) before
  `profiles_http_start()`, so there is no start-versus-handler race.
- The delete path's `(void)profiles_favorites_set(id, false)` now may trigger a
  re-load. That is acceptable.

### INFO-3: the degraded set and clear ordering is correct

- **Favorites:** the mark is cleared at load start and set only after the NVS
  read and resolve both fail. A successful retry leaves it cleared.
- **Used bitmap:** the mark is set on the probe error and cleared only after a
  successful rebuild. The clear happens before the caller's save, so a failed
  save after a good rebuild leaves the mark clear. The save failure itself is
  reported through the caller's error path, so this is acceptable.

### INFO-4: 409 versus 500 is consistent

`cfg_fs_http_persist_failed_for()` answers 409 `store_unreadable_at_boot` when
the store is degraded and 500 otherwise. That is the same split `zones.json`
and the unit preference use.

The code name `store_unreadable_at_boot` is a slight misnomer now that the
condition heals on retry. It is not worth changing the wire contract for.

### INFO-5: handler TOCTOU between `set()` returning and `cfg_fs_degraded_is()`

The handler reads `cfg_fs_degraded_is()` without the favorites lock after
`set()` returns. A concurrent successful heal in between would turn a 409 into
a 500 for that one request. This is negligible.

### INFO-6: boot-time fault notes stay latched after a heal

`main_network_http.c`'s "no favorites shown this boot" log line and
`startup_fault_note(STARTUP_FAULT_SETTINGS_STORE)` are not withdrawn when a
later `set()` heals the store. This is cosmetic.

### INFO-7: cosmetic redundancy in `used_bitmap_save`

The `cur_len > sizeof(*bm) &&` test duplicates the first clause of
`used_bitmap_len_is_longer_shape()`.

## Fix status (firelowfx4)

| Finding | Status | Where |
|---|---|---|
| LOW-1 | FIXED (honest text, no new route) | `cfg_fs_refusal_http.h` `CFG_FS_FAVORITES_UNREADABLE_REASON` + `cfg_fs_http_persist_failed_favorites()`; the favorite POST 409 now says a retry re-reads, and that cfg format / scope kiln do not clear it, only `factory_reset scope=profiles` does (and it also erases all user profiles). No favorites-only reset added (would need a route; URI cap has 9 spare, left alone). Test in `test_profiles_http.c`. |
| LOW-2 | FIXED | `favorites_load()` resolves into locals and publishes only on success; a refused retry leaves `s_fav_user`/`s_fav_builtin` untouched (builtin re-read starts from the current value). Test `test_favorites_refused_retry_keeps_published_masks`. |
| LOW-3, LOW-4 | already fixed (see above) | |
| LOW-5 | FIXED | new `cfg_fs_condition_*` registry (`cfg_fs.c`); `profiles_used_bitmap` is listed under `GET /api/cfgfs` `"conditions"` only, no longer in `degraded` or the backup `stale_or_unknown_stores`, and no longer uses the 6/8 listing caps. Test asserts it is a condition and NOT degraded. |
| INFO-1 | FIXED | redundant `cfg_fs_degraded_set(path,false)` at the top of `favorites_load` removed; `pref_cfg_fs_resolve()` owns the entry (existing heal test still asserts the mark clears). |
| INFO-4 | left | wire contract name kept. |
| INFO-6 | PARTLY | boot log line reworded ("favorites unresolved (the next favorite POST retries the read)"); no API exists to withdraw the startup fault note, not added. |
| INFO-2,3,5,7 | no action | |

Negtests (`tools\negtest.ps1 -Preset kilnfw-host`, all CAUGHT): favorites POST back to the generic 409 text (LOW-1, test lines 2209-2213); builtin mask re-read starting from 0 instead of the published value (LOW-2, :2188); `profiles_used_bitmap` back in the degraded registry (LOW-5, :2130-2131).
