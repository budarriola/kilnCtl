# Review: profilesfx (WEBGENFX LOW-B/LOW-C, WEBFX8 LOW-1/LOW-2, SUITEFX LOW-1/INFO-2) -- 2026-10-11

Reviewer: Opus, worktree at origin/dev `d9fa689b2`. No board access. No code changed.

Commits reviewed: `742a4f154` (code and tests) and `d9fa689b2` (Fix status sections in
`REVIEW_WEBGENFX_2026-10-10.md`, `REVIEW_WEBFX8_2026-10-11.md`, `REVIEW_SUITEFX_2026-10-11.md`).

## Verdict

No HIGH, no MED. The six items are fixed. Three LOW, all about what a client or the LCD shows; none
loses data.

## Runs on the tip (the fixer did not re-run after rebase)

| Run | Result |
|---|---|
| `build_host_tests.ps1` (full KilnFW host suite) | 98/98 executables built and passed; both new tests ran |
| `check_00_kilnfw_target_build.ps1` (through the build gate) | PASS, `KilnCtrl.bin` produced |
| `tools/check_lint_pages.ps1`, `tools/check_web_gzip_parity.ps1` | PASS (22/22 assets) |
| pytest `test_profile_live_http_client.py`, `test_profile_live_mcp_tools.py`, `test_profile_live_readback.py` | 57 passed |
| `negtest.ps1`, 5 mutations, `build_host_tests.ps1 -Only "profiles_http\|profiles_live_http"` | 5/5 CAUGHT, baseline passed |

Mutations: drop the `check_rev` compare in `profiles_http_save_impl()`; drop the rev-unknown precheck
in `profiles_delete_slot()`; rename `"saved"` in `send_err_json_saved()`; drop the 500
"saved to slot" branch in `send_decide_failure()`; revert the rollback log text. Each failed the
intended assertions.

## Checked, correct

- **expect_rev under the lock.** `profiles_http_save_impl()` compares `profiles_http_slot_rev()`
  right after `profiles_save_lock()`, before the name check and before `profiles_slot_gen_begin()`.
  Every `s_slot_gen` writer (save, web POST `profiles_edit_http.c` 770, delete, retarget
  commit/revert) runs under the save lock; the boot load runs while writers are blocked. So no save
  can land between the compare and the write. No new lock; nesting rule in `cfg_save_lock.h` is
  unchanged.
- **LCD capture.** `ui_page_profile_builder_start_edit()` reads the source slot's rev before the
  unlocked `profiles_http_get()`, so a save racing the load moves the generation and the later compare
  refuses. An odd (mid-write) capture is refused by the existing `(s_pick_rev & 1u)` check. A pick of
  another slot keeps the pick-time rev. `start_new()` and builtin sources reset the source slot to
  0xFF. A requested id >= `PROFILES_MAX_COUNT` ignores `check_rev`; the LCD never passes one.
- **Delete of a rev-unknown slot.** `s_profile_rev_unknown[]` is only written during the boot load
  (`nvs_load_files_only()`, `nvs_load_all_from()`, `profiles_boot_load_body()`), while
  `profiles_writers_blocked()` refuses every delete, so the unlocked precheck cannot race a flip. The
  precheck runs before the favorite clear and the `firing_stats_erase()`, so a refused delete keeps
  both (test asserts the favorite). The only `ESP_ERR_INVALID_STATE` left from
  `nvs_erase_slot_locked()` is `cfg_save_lock_reset_refused()` (factory reset in flight), still a 500.
  `profiles_http_delete()` (benchproto, LCD picker) compares `== PROFILES_DELETE_OK`, so the new enum
  value is a plain failure there.
- **No live file dropped.** The rollback delete still runs only for a fresh slot, only when the slot
  is not rev-unknown and the file carries exactly `s_profile_rev + 1`. Only the log text changed.
- **Buffers.** Longest decide error: "saved to slot NN; clearing the working copy failed: " (~52) plus
  `clear_err` (<= 63) is about 116 bytes, under 160. LCD `msg[192]` holds "Refused: " plus 159.
  `send_err_json_saved()`: `escaped[320]`, `json[420]` (about 50 bytes of fixed text). All `snprintf`;
  the target build (`-Werror=format-truncation`) passed. The new LCD refusal text (59 chars) fits
  `err_msg[64]` in `do_save_apply()`.
- **`out_id` on the saved paths.** `decide_finish_clear()` sets `*out_id` before either
  "saved to slot" outcome, so the `"id"` field is the real slot. The `!persisted` 500 ("storage save
  failed") is not prefixed "saved to slot" and correctly carries no `saved:true`.

## LOW-1: web page loses unsaved form edits' Save after any failed decide

`live_profile_page.html` `decide()` (~494) now sets `lastGen = null` and calls `refreshLive()` on
every failure. If the form has unsaved edits (`dirty`), `refreshLive()` sees
`live.generation !== null`, shows `showStaleBanner()` ("Changed elsewhere -- reload ...") and returns
without setting `lastGen`. From then on Save answers "Still loading the working copy" and decide
answers the same, until the user presses Reload, which discards the edits. This happens for failures
that change nothing on the board (400 "missing name", 403 builtin overwrite, 409 busy, the 500 after a
clear failure whose generation did not move), and the banner is then false.

Fix: drop `lastGen = null;` in the failure branch and keep `refreshLive()`. `refreshLive()` reloads
only when the generation really moved, and shows the banner only then (the SAVED_BUT_EDITED case).

## LOW-2: the new `saved`/`id` fields never reach a Python caller

`profile_live_http_client.py` `_error_detail()` keeps only the body's `error` string;
`ProfileLiveHttpError` has no `saved` or `id`. `profile_live_decide()` therefore still returns
`error: POST ... failed: saved to slot N; ... (HTTP 409)`. The two docstrings added in this commit
describe body fields the caller cannot see. The slot number survives only in the prose.

Fix: parse `saved` and `id` from the JSON body into the exception, and have the MCP tool print
"SAVED to slot N, but ..." instead of a leading `error:` when `saved` is true. Add a client test.

## LOW-3: LCD refusal tells the user to do something that cannot work

When the overwrite of the edited source slot is refused under the lock, the LCD shows "That slot
changed since you opened it. Pick the slot again." Picking the source slot again reuses the rev
captured at edit load (`ui_page_profile_builder_source_rev()`), so it is refused every time. The
refusal itself is right (the draft is based on stale content); only the advice is wrong. The user has
to reopen the profile from its detail page (or save to another slot).

Fix: for the source slot, say "This profile was changed since you opened it. Reopen it to edit, or
save to a different slot."

## INFO

- I-1: LCD save through `profiles_http_save_expect_rev()` passes `out_persisted = NULL`, so an
  overwrite whose storage write fails (including a rev-unknown slot) still shows "Saved to slot N"
  and stays applied in RAM. Same as the old `profiles_http_save()` call; not a regression.
- I-2: "saved to slot" is matched as a string prefix in two places (`send_decide_failure()`,
  `ui_page_live_decide.c`). The HTTP side is tested; the LCD branch and the source-rev capture are
  covered only by the source-text test in `test_delete_clears_favorite_before_erase_wiring()`.
- I-3: live decide OVERWRITE still saves to `rec.origin_id` with no rev compare. That is the
  overwrite's meaning (the working copy replaces the origin), outside this change.

## test_update_fetch.c 469/489 failures seen during the fixer's negtest

At the tip these lines are in `test_writer_wedge()`: 469 "the abandoned writer is still parked in
flash" (`g_fr_flash_blocked_n >= 1`) and 489 "wedged writer holds the stage in UPLOADING". Both fail
exactly when the scaled writer timeout (`g_fr_time_div = 1000`) fires before the writer thread has
reached the blocking flash op, which depends on scheduling. It is a real load flake, not caused by
this change. `023f99f03` ("writer-timeout clock starts when parked", committed 23:22, after
`742a4f154` was authored at 23:08) makes the fake `xSemaphoreTake()` start the scaled clock only once
a writer is parked, with an unscaled 15 s fallback. Stress on the tip: 60 runs of
`kilnctl_host_tests_update_fetch.exe`, 12 at once, during a KilnFW target build: 0 failures. The
residual risk is a writer that cannot park within 15 s.
