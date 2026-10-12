# Review: PROFILESFX11 (92ca60e9d, d6855f7d2) -- 2026-10-12

Opus review of the PROFILESFX11 fix on origin/dev: `92ca60e9d` (fix) and `d6855f7d2` (test tweak). The fix answers
`docs/audits/REVIEW_PROFILESFX10_2026-10-12.md` (MED-1, LOW-1, LOW-2, INFO-2, INFO-5). This is a static review plus
negtest runs over the KilnFW host tests. No target build was run, and the bench board was not touched. The owner
rule applies: only device bugs are rated. Tooling and PC-side nits are skipped.

Counts: HIGH 0, MED 0, LOW 2, INFO 2.

## Earlier findings: all closed on the device

- **FX10 MED-1: closed.** `profile_post_handler` (`profiles_edit_http.c:721-728`) now refuses a rev-unknown target
  under the save lock. The refusal comes after slot selection and before the name check, `gen_begin` and the RAM
  copy. It unlocks, frees `warn_json` and `tmp`, and only then sends the same 409 profile text
  (`cfg_fs_http_persist_failed_state_profiles(req, true, ...)`). No lock is held while the response is sent, and no
  new lock is taken, so this cannot deadlock. RAM, the used bit, the generation and the file are all left as they
  were, so nothing can diverge. An overwrite that carries `expected_rev` is still checked for a stale rev first
  (409 `profile_changed`), as before. Proven by N1, N7 and N8.
- **FX10 INFO-2: closed.** First-free allocation skips rev-unknown slots in three places: the POST loop
  (`profiles_edit_http.c:706`), the shared callback `profiles_http_slot_used_cb` (`profiles_http.c:1851`, used by
  `save_impl` for UART, LCD, live save_as, the import route and backup commit), and the backup pass-1 simulation
  callback (`backup_import.c:170`). The pass-1 simulation and the commit still choose the same slot. Proven by N2,
  N3 and N5.
- **FX10 LOW-1: closed.** Backup pass 1 (`backup_import.c:1296-1302`) refuses a has-id candidate on a rev-unknown
  slot with `PROFILES_SAVE_ERR_STORE_UNREADABLE` before any zones, aux, relay_cycles or prefs write. The flag is set
  only by the boot load, so nothing can change between pass 1 and commit. Proven by N4: the test asserts zero writes
  of any kind.
- **FX10 LOW-2: closed.** The seqlock test again drives a failed persist (`failing_write_fn` on an overwrite) and
  asserts that the old capture is refused. N6 (the earlier M10, a generation rewind) is CAUGHT at
  `test_profiles_http.c:2939`.
- **FX10 INFO-5: closed.** Only one declaration of `zones_config_is_undecided()` remains
  (`zones_config_accessors.h`).

Write paths swept for "can still write a rev-unknown slot, or overwrite a newer persisted profile":

- POST: fixed above.
- `save_impl`, used by UART, the LCD builder (`save_expect_rev`), live save_as and overwrite, the import route and
  backup commit: refuses up front (FX10).
- Delete: refuses before the favorite clear and the stats prune.
- Legacy migration: `nvs_save_slot` refuses, and the slot is cleared in RAM.
- Retarget: see LOW-1 below.

No path persists over a rev-unknown slot. The file write always stamps `s_profile_rev + 1`, and an overwrite of a
newer file is still gated by the optional `expected_rev` under the lock. That is unchanged.

## New findings

### LOW-1: zone-to-aux retarget writes zones and aux first, then fails on a rev-unknown slot with a false "REVERT INCOMPLETE"

`retarget_plan` (`profiles_http.c:2617-2675`) does not check `s_profile_rev_unknown`. A used slot can be rev-unknown:
when the rev floors are unknown, every slot is (`nvs_load_files_only`, `profiles_http.c:1176-1178`).

On such a board, `POST /api/zones move_zone_to_aux=Z` runs as follows:

1. The plan passes.
2. The convert writes the journal, frees the zone and enables the aux (`zone_aux_convert_core.c:327-362`).
3. `retarget_commit` swaps the first affected slot in RAM. `nvs_save_slot_locked` refuses it with
   `ESP_ERR_INVALID_STATE`.
4. `retarget_revert` swaps it back. That save is refused too, so `clean=false`.
5. The reply then says "REVERT INCOMPLETE, profiles may be mixed", and the convert rolls back the aux and the zone.

The end state is consistent: RAM equals the file, and nothing on the profile side was written. But:

- The operator is told that profiles may be mixed when they are not.
- Four flash writes, plus their undo writes, happen for a request that could never succeed. A failed undo at stage 1
  or 2 is exactly the divergence this sequence exists to avoid.

Fix:

- Make `retarget_plan` refuse any affected slot (one with `hits != 0`) that is rev-unknown, using the
  store-unreadable text. The convert's plan check already runs before the journal write, so the refusal would then
  come before any write.
- Optionally, have `retarget_revert` count a slot whose forward save was refused as clean.

Not a safety hazard. Retarget is idle-only.

### LOW-2: when the floors are unknown, every create now answers "profile storage full"

When the floors are unknown, every slot is rev-unknown, so the new first-free skip finds no slot. Every create path
(`profile_post_handler` at `profiles_edit_http.c:710-716`, a 400; `save_impl` at `profiles_http.c:2016-2019`, which
covers live save_as, the LCD builder new profile, UART, the import route and backup) now refuses with "profile
storage full". Before FX11 the same board got the 409 store-unreadable text with its remedy (reboot, or factory reset
scope=profiles). "Storage full" is false on a board with empty slots, and it points the operator at deleting
profiles, which delete also refuses.

The backup test codifies this text (`test_backup_import.c:3393`). Nothing is written, so this is about the message
only.

Fix: in the no-free-slot branches, when any unused slot is rev-unknown, answer with the store-unreadable text (the
409 on HTTP) instead of "storage full".

### INFO

- INFO-1: Two comments are now stale. `profiles_catalog_http.c:318-320` says that with floors unknown "a save into the
  first free slot is then refused". The page comment at `profiles_page.html:1406` says "free slots refuse saves too".
  With FX11, first-free skips those slots. The header itself is still correct, because it drives only the degraded
  notice.
- INFO-2: `profile_post_handler` and `save_impl` still carry their own copies of the slot-pick and refusal logic.
  FX10 MED-1 suggested routing POST through `save_impl`. The two now agree, and N1, N2, N3 and N8 pin both, but they
  can drift again.

## Negtest evidence

Command (from a wrapper script, so the `|` in `-Only` survives):

```
tools\negtest.ps1 -Mutations <json>
  -Command 'powershell ... build_host_tests.ps1 -OutDir "{OUT}" -Only "profiles_http|backup_import"'
  -ExpectPattern '(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES'
```

Base `7be419e7f` (origin/dev). The baseline passed, the real tree was unchanged, and the copies were removed. Every
CAUGHT below is a compiled mutant with a failing `FAIL file:line` assertion, not a build failure. Log dir:
`%TEMP%\negtest_logs\20261012_161015_g04k`.

| # | Mutation | Verdict | Failing assertion |
|---|----------|---------|-------------------|
| N1 | POST up-front rev-unknown refusal off (`0 &&`) | CAUGHT | test_profiles_http.c:6308, :6310, :6311 |
| N2 | POST free-slot loop no longer skips rev-unknown | CAUGHT | test_profiles_http.c:6330 |
| N3 | `profiles_http_slot_used_cb` no longer skips rev-unknown | CAUGHT | test_profiles_http.c:6338, :6339 |
| N4 | backup pass-1 rev-unknown precheck off | CAUGHT | test_backup_import.c:3373-3376 |
| N5 | backup simulation callback no longer skips rev-unknown | CAUGHT | test_backup_import.c:3393, :3394 |
| N6 | rewind `s_slot_gen` by 2 after a failed persist in `save_impl` (old M10) | CAUGHT | test_profiles_http.c:2939 |
| N7 | POST refusal sends the generic 500 (`store_refused=false`) | CAUGHT | test_profiles_http.c:6223, :6225, :6228 |
| N8 | POST refusal placed after the RAM copy | CAUGHT | test_profiles_http.c:6308 |
