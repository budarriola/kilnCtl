# Review: misc8fx (7878879eb) -- 2026-10-10

Adversarial review of origin/dev `7878879eb`, which fixes REVIEW_MISC8_2026-10-10.md
LOW-1, LOW-3, LOW-4 and REVIEW_MCPFX1_R3SFW_2026-10-10.md LOW-6. Code was not changed.

Summary: one MED (the new rollback delete can remove a file the boot path left alone
on purpose), two LOW, three observations. The parsing change, the restored SaftyFW
asserts and the ct_cal edge asserts are correct. The MISSED `peer_len > 0` mutation
is equivalent.

## MED-1: the rollback delete runs even when nothing was written, and can delete an unexamined file

**Where:** `firmware/KilnFW/App/drivers/http/profiles_http.c`, `profiles_http_save_ex()`,
the new `(void)profiles_cfg_fs_delete(target_id);` in the fresh-slot rollback.

The rollback runs on any `err != ESP_OK` from `nvs_save_slot_locked()`. Several of
those errors are returned before any file write:

- `s_profile_rev_unknown[id]` (the rev floor is unknown this boot),
- `cfg_save_lock_reset_refused()` (a factory reset is in flight),
- `caller_stack_is_external()`, a `cfg_fs` that is not available, an encode failure,
- a write that fails before its rename.

In these cases the file at `profiles/<id>` was not written by this save, but the
rollback deletes it anyway.

The fresh slot comes from `profiles_http_first_free_slot()`, which tests only the
RAM used bit. A slot can be free in RAM while a file exists on flash:

- `nvs_load_files_only()` / `nvs_load_all_from()` set `slot_res_err[id]` when the cfg
  read of a slot errors (OOM, I/O). The slot stays unused in RAM, the file is not
  examined, and `s_profile_rev_unknown[id]` is set. The comment there says "file
  unexamined -> slot unknown, saves/deletes refused". `nvs_erase_slot_locked()` does
  refuse the delete. The new rollback does not: a live SAVE_AS
  (`profiles_live_http.c:671`, the only caller that passes `out_persisted` with a
  fresh slot) picks that slot as the first free one, `nvs_save_slot_locked()` refuses
  with `ESP_ERR_INVALID_STATE`, and the rollback then deletes a profile file that
  might have loaded on the next boot.
- The degraded files-only path ("This degraded path must never delete") leaves a
  file that fails to decode (for example, a blob from newer firmware) on flash with
  the slot free in RAM and every floor unknown. A SAVE_AS refused there deletes that
  file.

Before this commit the file survived both cases. **Demonstrated** by a negtest
mutation that adds the test `test_rvfx_rollback_keeps_unexamined_file`: slot 0 is
free in RAM, has a valid file at rev 5, and `s_profile_rev_unknown[0]` is set. A
SAVE_AS lands on slot 0, is refused, and the file is gone afterwards (see the
Negative tests section below).

**Fix:** delete only a file this save actually wrote. Two ways to do that:

- `nvs_save_slot_locked()` reports whether `s_write_fn` ran (or the rollback re-checks
  `!s_profile_rev_unknown[target_id]` and the pre-write refusals).
- Better: before unlinking, read the file back with `profiles_cfg_fs_load_raw()` and
  delete only if its rev equals the `s_profile_rev[target_id] + 1` this save tried to
  write. That also covers a write that failed before the rename over an undecodable
  older file (LOW-1 below).

Make the test above permanent.

## LOW-1: a write that fails before the rename over an undecodable file now deletes that file

This happens on a normal boot, with the floor known. If a free slot holds a file that
was rejected at load (newer-firmware blob, corrupt), a SAVE_AS whose write fails before
the rename now deletes that file. Previously the file survived. A successful save would
overwrite it anyway, so the loss is small. The read-back-rev check proposed for MED-1
closes this too.

## LOW-2: the `+` test cases do not test a literal `+`

`http_form_url_decode()` turns `+` into a space. So `level=+2` and `step=+1` test the
leading-space case a second time (the same as `%20`). A literal plus (`%2B`) is the
input the new `[0] < '0'` check newly refuses, because `strtol` accepts `+2`. It is not
tested in `test_dashboard_settings_http.c` or `test_setup_progress_http.c`. The code is
correct. Add `level=%2B2` and `step=%2B1` to the bad lists.

## Checked, no finding

- **Overwrite versus SAVE_AS:** the delete only runs when
  `requested_id >= PROFILES_MAX_COUNT`. An overwrite of an existing slot never reaches
  it. The slot is chosen under `profiles_save_lock()`, so no other save can take the
  same id before the rollback. Apart from MED-1 and LOW-1, a reused target_id belongs
  to no live slot.
- **Lock order:** the delete runs under the profiles save lock, inside the
  slot-generation window, before `profiles_slot_gen_end()`/`profiles_save_unlock()`. It
  is the same nesting as `nvs_erase_slot_locked()`, which calls `profiles_cfg_fs_delete()`
  under the same lock, and `nvs_save_slot_locked()`'s own `cfg_fs` write. It takes no
  new lock. Boot-time writers are excluded by `profiles_writers_blocked()`, and
  `profiles_edit_http.c`'s save is under the lock.
- **Parsing boundaries:** `[0] < '0' || [0] > '9'` refuses an empty value, a space, `+`,
  `-` and any letter. `"0"` and the top values (`4`, `STEP_COUNT-1`) still pass. A
  leading-zero `007` still parses as 7, which is harmless. `%00` is refused by the
  decoder, so no embedded NUL gets through. The 7-character buffer cannot overflow
  `long`. `http_form_parse_long()` already implements this contract (but uses
  `isspace`, so it would accept `+`). Using a shared helper would avoid a third
  hand-rolled copy.
- **SaftyFW case 4:** both restored checks pass. They are placed after the 4a
  heat-unsafe asserts, before 4b resets the context, which is where they belonged.
- **ct_cal edges:** they match `config_params.c`: gain in [0, 10] inclusive, |offset| <= 50
  inclusive, and gain 0 allowed only when uncalibrated. Each refused send varies one
  field. The lower gain bound and the "calibrated with gain 0" refusal are not part of
  this block (observation only).
- **MISSED `peer_len > 0` mutation (is_relay):** equivalent. Inside
  `if (peer_len != -1)`, `peer_len` is either -2 or >= 0. On every failure path
  `http_form_url_decode()` leaves `peer_val[0] == '\0'` (the F1 invariant in
  `http_form.h`), and `peer=` with an empty value decodes to `""`. So whenever
  `peer_len <= 0`, `strcasecmp(peer_val, "relay") != 0` and `is_relay` is false with or
  without the guard. The equivalence rests on the decoder's empty-on-failure contract;
  the guard is defence in depth.

## Tests run (worktree at origin/dev 7878879eb)

- KilnFW `build_host_tests.ps1 -Only '^(profiles_http|setup_progress_http|dashboard_settings_http|cfg_fs_format_http)( |$)'`:
  all 4 executables passed (1583, 85, 53, 64 checks).
- SaftyFW `build_host_tests.ps1`: all passed. `test_link_task_fuzz`: 32339 checks,
  0 failures.

## Negative tests (tools/negtest.ps1, `-Only ^profiles_http\s`, -ExpectPattern per rules)

Base 7878879eb. The baseline passed, the real tree was unchanged, and every copy was removed. Verdict: ALL_CAUGHT.

- `demo_rollback_deletes_rev_unknown_file`: adds `test_rvfx_rollback_keeps_unexamined_file` (MED-1 scenario).
  CAUGHT at test_profiles_http.c:2354 "slot 0's unexamined file survived the rollback". The check
  before it passed (the save landed on slot 0 and was refused), so the file was deleted by the
  rollback of a save that never wrote. This confirms MED-1 on current code.
- `drop_rollback_delete`: removes the new `profiles_cfg_fs_delete()` line. CAUGHT at
  test_profiles_http.c:2329 (the LOW-1 test `test_save_ex_rollback_unlinks_written_file`), so the
  fix's own test is not vacuous.

## Fix status

- MED-1 FIXED in ebdff64ee (test hardened in 5f687382b): the SAVE_AS rollback deletes the file only if the slot is not rev-unknown and the file read back has rev == attempted rev. Demo test is now permanent (`test_rvfx_rollback_keeps_unexamined_file`, `test_rvfx_rollback_keeps_file_with_other_rev`). Negtested: rollback-unconditional-delete, rollback-ignores-unknown CAUGHT.
- LOW-1 FIXED in ebdff64ee: same read-back check covers a free slot holding an undecodable file (invalid file is never deleted).
- LOW-2 FIXED in ebdff64ee: literal `%2B` cases for `level=` and `step=`. Negtested: dropping the digit check in dashboard_settings_http.c and setup_progress_http.c CAUGHT.
