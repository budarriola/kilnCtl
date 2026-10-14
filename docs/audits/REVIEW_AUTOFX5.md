# REVIEW_AUTOFX5 -- install result identity, update_install_start, lost-record UNKNOWN

Scope: `60c7ea992`, `4b8150857`, `95a04c070` on origin/dev (the fixes for `REVIEW_AUTOFX4.md`).
Reviewed in a clean worktree at origin/dev. No board access.

**Result: 0 HIGH, 0 MED, 4 LOW, 6 INFO.** Two gaps found in testing, closed in this commit:
the install id field had no mutation coverage in either status route, and nothing pinned the pending
TTL above the background wait.

## Questions asked

**Is the id written at the right moment in both images, so a match proves THIS install ran?** Yes.
The app writes only the request (`update_install_prepare`). It erases the old result with a verified
erase before it writes the request, so a stale result cannot survive a POST that reached the board.
Only recovery writes results. `recovery_install_boot()` copies `rq.sha256[0..3]` into `s_id` from the
same request blob that `update_install_boot_run()` decodes. Every refusal after a decoded request records
`req.sha256`. The pre-boot OK (`update_install_pre_boot`) and the poll-task FAIL/OUTCOME_UNKNOWN records
use `s_id`. A refusal before decode (NVS unavailable, record bad) records a zero id, and the tool still
counts it as this install's failure (`_failure_is_this`). That is the conservative direction. An `ok`
with id X that appears after a pre-POST read with no `ok X` can only come from a recovery boot that
applied a request with sha prefix X. If that request was a leftover retry rather than this POST, the
same image still got installed.

**Same-sha reinstall: false ok, or unconfirmable?** Neither. When `before_ok_id == rid`, the id shortcut
does not fire and `_ok_is_new` falls through to the old proof. An answered POST is accepted, which is
safe because prepare erased the old result first. A lost POST needs the uptime reboot proof
(`test_same_id_as_the_record_before_the_post_is_not_new`, P2 below). So a same-image reinstall can be
confirmed exactly as before the id existed, never by the id alone.

**Old firmware / all-zero id.** Safe. An app or recovery image older than this change has no `id` key,
or writes zeros. `_id_of` maps absent, malformed and `00000000` to `""`. `_stale_id` and the shortcut
both need a non-empty id, so the old logic runs unchanged (`test_zero_id_falls_back_to_the_old_proof`,
P7). An unreadable stage sha also falls back (`_staged_id` -> `""`).

**Blob reuse, versioning, CRC, rollback.** No schema bump is needed. Bytes 40..43 of the 48 B result
blob were `memset` to 0 by every earlier `update_install_res_encode`, and they were already inside the
CRC span (0..43). So:
- a record written by older firmware decodes with id = zeros;
- older firmware reading a new record checks the same CRC over the same bytes and ignores 40..43;
- size, magic and `REC_VERSION` are unchanged, and the request blob is untouched.

No rollback hazard: there is no new key and no length change. A mixed pair (new app with old recovery,
or the reverse) degrades to a zero id, so the old proof applies.

**900 s vs 600 s.** Consistent now. `_PENDING_TTL_S = 900`, measured from `t_pre`. The background job's
deadline is 600 s from `t0`, and `t0 < t_pre`, so the pending record always outlives the job. The
UNKNOWN "cannot confirm" text prints `int(_PENDING_TTL_S)` (900), and the docstring says 15 minutes.
Nothing pinned the ordering before this review. The new
`test_pending_record_outlives_the_background_wait` (a follow-up 30 s after a 600 s job gave up must
still find the pending record) catches P1 (TTL 500 s).

**404 as "recovery applying".** It does not change control flow. Every `UpdateHttpError` already kept
polling until the deadline. `n404` only changes the final UNKNOWN tail. An app build without the route
(pre-stage firmware) cannot reach the POST, because the initial `get_install_status` raises and the
tool returns `error:`. See LOW-1 for the remaining case.

**update_install_start gates.** `confirm is not True` answers synchronously and starts no job. Tests
cover `False`, `1`, `"yes"` and `None`, and P5 (truthy gate) is CAUGHT. The job calls `_install_impl`
with `True`, so the stage precheck and every refusal run inside it. It is bounded: deadline
`t0 + 600`, and each poll is clamped to what is left. It takes no board lock. Neither does
`update_install_staged`, and the update tools never have; see LOW-2.

**Recovery fragment id test.** Added as section 3 of `check_recovery_status_fragments.ps1`. It checks
both images:
- the recovery fragment carries `"id":"%s"`, gets `ir_id` as its argument, and builds `ir_id` from
  `have_ir ? ir.id : no_id`;
- the app's `status_get_handler` format carries `"id":"%s"`, builds `lid` from `res.id`, and passes
  `lid` after `lv`.

Mutants C1 to C4 are all CAUGHT.

**Internal RAM.** No new heap allocation in either image.
- App: no new static. 9 B (`lid`) plus 4 B (`res.id`) on the httpd handler stack. The 448 B JSON buffer
  still fits the worst case (about 350 B).
- Recovery: +4 B `.bss` (`s_id`), and `no_id` is a `static const` in flash. 9 B (`ir_id`) on the httpd
  stack. The static `frag[200]` is unchanged, worst case 196 B.

## Findings

### LOW-1: downgrade to an image without `/api/update/status` reads as "recovery still applying"
If the installed image has no install-status route (a downgrade to a pre-WP release), every poll after
the reboot 404s. The job then ends UNKNOWN with `HTTP 404 ... x N: the recovery image is still applying
the stage`, which is wrong once the app is back. It fails safe: no false ok, and the result is bounded
by the deadline. Suggestion: on a 404, probe `GET /api/recovery/status`. Recovery answers it and the
app does not, so a 404 there plus a reachable `/api/status` means "an application without the install
route", not "applying".

### LOW-2: the background install takes no board lock and has no single-flight guard
`update_install_start` can run for 600 s while the board reboots into recovery and back. A
`bench_test_start`/`ota_matrix_start` run, or a second `update_install_start`, is not refused
meanwhile. The second job would overwrite `_PENDING_INSTALL[host]` and POST into a rebooting board.
This was pre-existing for the synchronous tool, but the window is now longer. Suggestion: refuse a
second confirmed install while a job for the same host is running (an in-process set is enough). Take
the bench board lock as a mutating owner, or document why it is exempt.

### LOW-3: a stale failure record can end a lost-POST install as FAILED
`_failure_is_this` has no "before" comparison. Suppose the POST never reached the board and the
pre-POST record is a FAIL for the same image (an earlier failed attempt), or a zero-id FAIL. Then the
first poll returns FAILED for an install that never ran. If the POST was answered, the app erased the
record first, so only the lost-POST case is affected. It fails safe (never a false ok). Suggestion:
when `post_unknown`, ignore a failure whose code, reason and id equal the pre-POST record's.

### LOW-4: a plain status read with nothing staged now says "UNKNOWN: cannot confirm"
`update_install_staged()` / `update_install_start()` with `confirm=False`, no pending record and
`staged=false` answer "cannot confirm ... whether an install happened is not provable". That happens
even on a board where no install was ever attempted (`test_nothing_staged_and_no_record_says_cannot_confirm`
pins it). It is not unsafe, but it is noise for an ordinary status read. Suggestion: only answer
"cannot confirm" when there is an expiry tombstone, or when `last_install` is recorded. Otherwise report
the status line as a dry run.

### INFO-1: the recovery-side id plumbing is source-checked only
`s_id` capture in `recovery_install_boot()` and its use in `pre_boot`/`poll_task` have no host test (the
recovery image has no host-test harness). The codec and boot logic are host-tested (K1 to K7), and the
fragment and route emission are now checked (C1 to C4).

### INFO-2: OUTCOME_UNKNOWN in-RAM fallback has id zeros
When both the NVS write and the reload fail, `recovery_install_boot()` fills `s_last` by `memset` with
no id. Recovery status then shows `00000000` for that record. The tool treats it as "this install's"
failure, which is correct.

### INFO-3: fragment headroom is 4 B
The `last_install` fragment worst case is 196 of 200 B. The next field must go in its own fragment. The
check enforces this.

### INFO-4: 32-bit id
A 4-byte sha prefix collides with probability 2^-32 per pair of images. That is negligible here, and a
collision falls back to the version/proof logic in any case.

### INFO-5: `_staged_id` read is not charged to the deadline clamp
`get_stage_status` uses the client's default timeout. It runs before the POST, inside `deadline - t0`,
so the call stays bounded. It is just not clamped like the in-loop reads.

### INFO-6: tool count
235 (`update_install_start`), consistent across CLAUDE.md, MCP_SERVERS.md and the fix status.

## Negtest

All runs: baseline passed, real tree unchanged, copies removed.

| # | Preset | Mutation | Verdict | Evidence |
|---|---|---|---|---|
| C1 | check | recovery `ir_id` hard-zeroed | CAUGHT | `recovery ir_id is not built from the loaded record's id` |
| C2 | check | recovery fragment drops `id` | CAUGHT | `fragment does not carry "id"` + `does not pass ir_id` |
| C3 | check | app `lid` hard-zeroed | CAUGHT | `app lid is not built from the read record` |
| C4 | check | app status drops `id` | CAUGHT | `lacks "id"` + `does not pass lid after lv` |
| P1 | pytest | `_PENDING_TTL_S` 900 -> 500 (below the 600 s job) | CAUGHT | `test_pending_record_outlives_the_background_wait` (new) |
| P2 | pytest | id shortcut ignores `before_ok_id` | CAUGHT | `test_same_id_as_the_record_before_the_post_is_not_new` |
| P3 | pytest | `_stale_id` always False | CAUGHT | 3 IdentityTests |
| P4 | pytest | 404 "recovery applying" tail dropped | CAUGHT | `test_only_404s_is_unknown_naming_the_recovery_image_and_the_background_tool` |
| P5 | pytest | start gate `confirm is not True` -> `not confirm` | CAUGHT | `test_unconfirmed_starts_no_job_and_posts_nothing` |
| P6 | pytest | background job uses the 270 s sync cap | CAUGHT | `test_job_runs_the_real_install_and_waits_much_longer_than_one_call` |
| P7 | pytest | `00000000` treated as a real id | CAUGHT | `test_zero_id_falls_back_to_the_old_proof`, `test_failure_without_an_id_is_still_failed` |
| P8 | pytest | `_failure_is_this` ignores a stale id | CAUGHT | `test_stale_failure_with_another_id_is_ignored` |
| K1 | kilnfw-host | encode skips id bytes | CAUGHT | `test_update_install_record.c:343/368/376` assertions |
| K2 | kilnfw-host | decode skips id bytes | CAUGHT | `:343/368/376` assertions |
| K3 | kilnfw-host | pre-boot OK written without id | CAUGHT | `:376` assertion |
| K4 | kilnfw-host | refusal recorded without id | CAUGHT | `:368` assertion |
| K5 | kilnfw-host | started outcome without id | CAUGHT | `:362` assertion |
| K6 | kilnfw-host | hex nibble order swapped | CAUGHT | `:353` assertion |
| K7 | kilnfw-host | `record_result` ignores id | CAUGHT | `:368/376` assertions |

Every kilnfw-host mutant compiled (no BUILD FAILURES line) and failed a named assertion.

## Changes in this commit
- `firmware/KilnFW_recovery/main/check_recovery_status_fragments.ps1`: section 3, install id emission
  in both images.
- `tools/PcTools/tests/test_mcp_server_update_install.py`: `test_pending_record_outlives_the_background_wait`.
