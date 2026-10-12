# Review: PROFILESFX10 (520a782d1) -- 2026-10-12

Adversarial review of `520a782d18006285229f005da6b8f4fb65e9f4f2` on origin/dev, which fixes the findings in
`docs/audits/REVIEW_PROFILESFX9_2026-10-12.md` (MED-1, LOW-1..3, INFO-2, INFO-3). Static review plus targeted
negtest runs over the KilnFW host tests. No target build was run, and the bench board was not touched.

Counts: HIGH 0, MED 1, LOW 2, INFO 5.

## Findings

### MED-1: POST /api/profile still assigns RAM on a rev-unknown slot, then answers 409 "refused"

`profiles_edit_http.c:770-782` (`profile_post_handler`) does gen_begin, `s_profiles.profiles[target_id] = *tmp`,
`profiles_slot_set`, and then `nvs_save_slot_locked()`. On a rev-unknown slot that save returns
`ESP_ERR_INVALID_STATE` (`profiles_http.c:1484`). The handler then answers
`cfg_fs_http_persist_failed_state_profiles(req, s_profile_rev_unknown[target_id], err, -1)`, which is a 409 saying
the store was refused. RAM keeps the new content anyway:

- The "refused" version is live. It can be started, and the executor fires content that the client was told was
  not saved.
- If the target was a new slot (from the handler's own free-slot loop at `profiles_edit_http.c:705`, which does not
  skip rev-unknown slots), the slot becomes a used phantom. Delete then refuses it with `PROFILES_DELETE_REV_UNKNOWN`
  (`profiles_http.c:2238`), so it cannot be removed until a reboot.
- A reboot reverts it.

This is the same class as FX9 MED-1. FX10 closed it for `profiles_http_save_impl` (`profiles_http.c:2025`), and so
for every other caller, but not for the main web editor path. The two paths now behave differently under the same
condition. It is not a safety hazard, because the content passed full validation. It is a state/report divergence
on the most-used write path. The fix should mirror `save_impl`: refuse before gen_begin when
`s_profile_rev_unknown[target_id]`, with the same 409. Ideally the handler would also route through `save_impl`.

### LOW-1: Backup import has no pass-1 rev-unknown precheck, so a degraded board gets a partial write

`backup_import.c` pass 1 simulates slot allocation and refuses "profile storage full" up front
(`backup_import.c:1311-1314`) so that nothing is committed. It has no rev-unknown check. At commit, zones, aux,
relay_cycles and prefs land first (around `backup_import.c:3003-3027`). Then the profiles loop calls
`profiles_http_save()`, which now refuses a rev-unknown target with `PROFILES_SAVE_ERR_STORE_UNREADABLE`
(`backup_import.c:3033-3040`). The result is a 500 `partial_write` with "profile entry N rejected at commit: ...".

The comment at `backup_import.c:3035` ("Should not happen -- pass 1 ... already checked everything
profiles_http_save() itself checks") is now false. The flag is sticky per boot, so pass 1 can know it exactly as it
knows "storage full". Before FX10 the same board took a silent RAM-only commit, so this is an improvement, but it is
still a partial write that pass 1 could prevent.

Message sizes are fine: `save_err[96]` holds the 73-character constant, and the outer `err_msg[160]`
(`backup_import.c:4320`) holds the prefix plus the constant.

### LOW-2: The seqlock test no longer covers the generation bump on a failed-persist overwrite

`test_profiles_slot_gen_seqlock` (`test_profiles_http.c:2909`) used a rev-unknown save to drive the failed-persist
path. It asserted "failed save after capture: old copy refused", meaning a failed persist that still changed RAM
must bump the generation. FX10 changed that block to assert the opposite for the up-front refusal, which is correct
for the new behaviour. Nothing replaced the old property. Persist failures that still assign RAM remain reachable
(for example a write failure on an overwrite: `failing_write_fn`, or `profile_post_handler` per MED-1).

Negtest proof, mutant M10 below (rewind the generation by 2 after a failed persist in `save_impl`):

- CAUGHT at the parent `859bf2a9a` (`test_profiles_http.c:2935: failed save after capture: old copy refused`)
- MISSED at `520a782d1`

Fix: in the seqlock test, drive the failed persist with `profiles_cfg_fs_set_write_fn(failing_write_fn)` on an
overwrite, and keep the "old copy refused" assertion. The stale comment at `test_profiles_http.c:2934` ("Failed
save: persist refused, RAM still takes the new content.") also describes the old behaviour.

`test_rvfx_rollback_keeps_unexamined_file` (`test_profiles_http.c:2845`) still protects its invariant: the
unexamined file survives and slot 0 stays unused. It now does so by the up-front refusal, not by the rollback guard.

### INFO

- INFO-1: The rollback guard `if (!s_profile_rev_unknown[target_id])` (`profiles_http.c:2077`) is now dead: the
  up-front refusal at :2025 means this branch never sees a rev-unknown slot. Mutant M11 (guard forced true) is
  MISSED, as expected. Keep it as defence in depth, or drop it with a comment.
- INFO-2: First-free allocation (`profiles_http.c:2014`, `profiles_http_first_free_slot`) does not skip a free
  rev-unknown slot. One unreadable empty slot therefore blocks every new-profile create (import, UART, live
  save_as, backup), even when other slots are free. The remedy text then points at a destructive
  `scope=profiles` reset. This was already the behaviour on the web path (`X-Profile-Floors-Unknown`,
  `profiles_catalog_http.c:318-324`, fwlow16 L3); FX10 extends the same outcome to the other callers. Worth a
  decision: skip rev-unknown slots in first-free.
- INFO-3: The LCD builder (`ui_page_profile_builder_review.c:124`) and the UART bridge (`uart_bridge_ext_control.c:506`)
  show the short constant "...reboot, or factory reset scope=profiles". It does not say that this reset erases every
  user profile. The HTTP 409 body does.
- INFO-4: In `profiles_http_save_expect_rev`, the rev compare runs before the rev-unknown check, so a stale LCD
  draft on a degraded slot reports "changed since you opened it" first. This is harmless; the retry then gets the
  unreadable text.
- INFO-5: `zones_config_is_undecided()` is now declared in both `zones_config_accessors.h:803` and
  `zones_http_internal.h:231`. The signatures are identical, so this is harmless.

## Requested checks

- (a) Callers of the save chain:
  - import route: 409, profile remedy (`profiles_export_http.c` import handler).
  - backup import: refuses at commit, giving a partial write (LOW-1).
  - LCD `save_expect_rev`: shows the text (INFO-3/4).
  - UART bridge: replies with the text; 96 B buffer fits.
  - live decide: save_as and overwrite answer 409, and the pending edit is kept.
  - The one write path outside the chain, `profile_post_handler`, is MED-1.
- (b) Changed older tests: the rollback test still protects its invariant. The seqlock test lost the failed-persist
  generation property (LOW-2, negtest-proven).
- (c) Open item `profile_post_handler`: rated MED (MED-1).
- (d) 409 body move: verified byte-identical. A Python comparison of the old `zones_post_send_undecided_409` (at the
  parent) and the new `cfg_fs_http_send_zones_undecided_409` gave 440 B, the same status `409 Conflict` and the same
  type `application/json`. Only one copy of the literal remains (`cfg_fs_refusal_http.h`). `zones_http_post.c` now
  forwards to it.
- (e) Stack: no new locals on httpd paths. The new helpers are `static inline` over string literals, and
  `decide_save_refusal` has no locals. `zones_post_send_undecided_409` stays NOINLINE. This is static review only;
  no target build was allowed.

## Negtest evidence

The fixer's doc section added no negtest table, so all evidence is from this review's own runs.

Command:

```
tools\negtest.ps1 -Rev <rev> -Mutations <json>
  -Command "build_host_tests.ps1 -OutDir {OUT} -Only profiles_http|profile_export_import|profiles_live_http|zones_http|iter_tune_http"
  -ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"
```

Every CAUGHT below is from a compiled mutant with a failing assertion (`FAIL file:line`), not a build failure. The
baseline passed and the real tree was unchanged in both runs.

| # | Mutation | Rev | Verdict | Failing assertion |
|---|----------|-----|---------|-------------------|
| M1 | `save_impl` rev-unknown check off (`0 &&`) | 520a782d1 | CAUGHT | test_profiles_http.c:6263-6277, :2939, :2856 |
| M2 | refusal returns true with `*out_id` set | 520a782d1 | CAUGHT | test_profiles_http.c:6263, :6270, :6276, :2856 |
| M3 | import branch for the constant off | 520a782d1 | CAUGHT | test_profile_export_import.c:442-444 |
| M4 | import uses generic `cfg_fs_http_persist_failed_state` | 520a782d1 | CAUGHT | test_profile_export_import.c:444 |
| M5 | live `decide_save_refusal` maps to BAD_REQUEST | 520a782d1 | CAUGHT | test_profiles_live_http.c:780-782 (save_as and overwrite) |
| M6 | live `send_decide_failure` uses generic helper | 520a782d1 | CAUGHT | test_profiles_live_http.c:782 (x2) |
| M7 | `zones_http_pid.c` undecided check off | 520a782d1 | CAUGHT | test_zones_http.c:2410-2412 |
| M8 | `iter_tune_http.c` undecided check off | 520a782d1 | CAUGHT | test_iter_tune_http.c:743-746 |
| M10 | rewind `s_slot_gen` by 2 after a failed persist in `save_impl` | 520a782d1 | MISSED | none (LOW-2) |
| M10 | same mutant | 859bf2a9a (parent) | CAUGHT | test_profiles_http.c:2935 "failed save after capture: old copy refused" |
| M11 | rollback guard forced true | 520a782d1 | MISSED | none, expected (dead code, INFO-1) |
| M12 | `profile_post_handler` save 409 uses generic helper | 520a782d1 | CAUGHT | test_profiles_http.c:6221 |

Logs:

- `%TEMP%\negtest_logs\20261012_121412_j8pb` (M1-M12 at 520a782d1)
- `%TEMP%\negtest_logs\20261012_121412_cod4` (M10 at the parent)

The hide and restore 409 call sites (`profiles_edit_http.c:936`, `:957`) share an identical find string, so they
were not mutated separately. They are covered by the same `test_profile_routes_use_profile_409_text` that caught M12.

## Fixed (PROFILESFX11)

Fixed in 4f9f65224 (pre-rebase SHA; see the landed SHA in the landing commit):

- MED-1: `profile_post_handler` refuses a rev-unknown target before the RAM copy (same 409 profile text); its free-slot search skips rev-unknown slots.
- INFO-2: every first-free allocation (`profiles_http_slot_used_cb`, backup pass-1 simulation, POST handler) skips rev-unknown slots.
- LOW-1: backup import pass 1 refuses a rev-unknown target slot up front (nothing written); stale "Should not happen" comment corrected.
- LOW-2: the seqlock test again drives a failed persist (`failing_write_fn` on an overwrite, which still changes RAM) and asserts the old capture is refused. M10 is now CAUGHT (test_profiles_http.c:2939). The rollback guard at `profiles_http.c:2077` (INFO-1/M11) is kept as defence in depth: a rev-unknown slot can no longer reach it.
- INFO-5: duplicate `zones_config_is_undecided` declaration removed from `zones_http_internal.h`.

Negtests (kilnfw-host, all compiled mutants with failing assertions): M1 post refusal off CAUGHT (test_profiles_http.c:6308-6311); M2 post free-slot skip off CAUGHT (:6330); M3 first-free callback skip off CAUGHT (:6331-6332); M4 backup precheck off CAUGHT (test_backup_import.c:3373-3376); M5 backup simulation skip off CAUGHT (:3393-3394); M10 generation rewind CAUGHT (test_profiles_http.c:2939). Full KilnFW target build passed; `check_httpd_task_stack_budget` on the resulting ELF: OK, honest headroom 1864 B (22.8%, classified LOW).
