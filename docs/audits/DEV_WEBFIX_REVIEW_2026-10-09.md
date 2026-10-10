# Dev web-fix review, 2026-10-09

This is a read-only review of four origin/dev commits:

- `3ade435b`: POST /api/profile maps a builtin id to the first free user slot.
- `aba69118`: zones POST treats blank guard fields, xzone and `pc_link_abort_silence_ms` as omitted.
- `b744de75`: setup note is truncated UTF-8-safely to 31 bytes; `/api/auth/reset` body moves to a 512 B heap buffer; the wizard honours `ok:false`; adds `stepSaveFailed`.
- `762a1f2a`: follow-up test regex for the new note text.

Line numbers refer to origin/dev at `e6a0ff34`. The CRITICAL and HIGH findings were both still present at that tip.

## Findings

### CRITICAL-1: forgot handler uses `RESET_BODY_MAX` before it is defined and calls free() on a stack buffer **FIXED in f8860c92**

`firmware/KilnFW/App/drivers/http/auth_totp_http.c:351-352`, inside `forgot_post_handler()`. In this handler, `body` is `char body[FORGOT_BODY_MAX]` on the stack (line 346, 128 B). The read-failure path now does:

```c
totp_secure_zero(body, RESET_BODY_MAX);
free(body);
```

`RESET_BODY_MAX` is first `#define`d at line 444, and no header defines it. The file is therefore a compile error. `auth_totp_http.c` is built only by the ESP target build (`drivers/CMakeLists.txt`, unconditional); the host tests do not compile it, so they stay green.

Failure scenario:
- The KilnFW target build of the origin/dev tip fails. Nothing on dev can be flashed or promoted until this is fixed.
- If the macro were defined earlier, the code would still be wrong. Any short read on the OPEN-tier `POST /api/auth/forgot` would zero 512 B over a 128 B stack array, overwriting the httpd task stack. It would then call free() on a stack address, which is heap corruption and an abort.

The hunk was clearly meant for the read loop in `reset_post_handler()` (CRITICAL-1 and HIGH-1 are one misplaced edit).

Fix: delete these two lines from the forgot handler and add them to the reset handler's read-failure path.

### HIGH-1: reset handler read-failure path leaks the 512 B buffer and leaves it unzeroed **FIXED in f8860c92**

`auth_totp_http.c:471-475`. When `httpd_req_recv()` returns <=0 in `reset_post_handler()`, the handler records backoff, sends a 400 and returns. It never calls `totp_secure_zero(body, RESET_BODY_MAX)` and never calls `free(body)`.

Failure scenario: an unauthenticated client on this OPEN-tier route sends a `Content-Length` of up to 511 and then closes the socket early.
- Each such request leaks 512 B of internal RAM.
- The leaked buffer keeps any partial `username`/`reset_token`/`new_password` bytes, never zeroed.
- The backoff gate slows this down but does not stop it. Over time the leak walks the internal heap toward the 8 KB `heap_internal` floor.

The success path at lines 488-490 is correct. The OOM path at line 466 holds nothing to free. The read-failure path is the only path not covered.

### MEDIUM-1: `stepSaveFailed` is wired only to steps 0, 8, 9 and 11; steps 1-7 still report "Saved." after a refusal

`firmware/KilnFW/App/drivers/http/setup_wizard_page.html`. `postStepState()` (line 1737) resolves with `{ok: r.ok, body}` and never rejects on a non-2xx status. These call sites ignore the resolved value and go straight to `armStepMessage(... 'Saved.')` and `loadAll()`:

| Step | Line |
|------|------|
| 1 | 1866 |
| 2 | 2011 |
| 3 | 2119 |
| 4 | 2262 |
| 5 | 2459 |
| 6 | 2578 |
| 7 | 2936 |

Failure scenario: the zones write succeeds, then `POST /api/setup/progress` is refused, for example with a cfg-fs persist failure (`cfg_fs_http_persist_failed`, 5xx) or a 401 after the session expires. The step shows "Saved." even though its progress state was never stored. The stepper then shows the step not done, with no reason given.

So the answer to "can the wizard still report success after a refusal" is yes, for seven of the twelve steps.

Fix: add `.then(function (r) { if (stepSaveFailed(r)) return; ... })` to each of these sites, as steps 0/8/9/11 already do.

### MEDIUM-2: run-queue stabilized-profile rewrite of a builtin id now silently creates a copy (FIXED in f561a4dc)

`tools/PcTools/src/kilnctrl/run_queue.py:546` (`save_profile_segments`) and `:580-676` (`ensure_stabilized_profile`), called from `run_entry` at about line 1293 whenever `entry.stabilize` is True, which is the default. Both are documented as overwriting the profile IN PLACE, and they POST `id=<entry profile id>`. Neither checks that the reply's `id` equals the requested one.

Failure scenario: a run-queue entry names builtin 128.
- Before `3ade435b`, the POST got a 400 and the run failed loudly with `RunQueueError`.
- Now the firmware saves the hold-prefixed copy into the first free user slot and returns 200. `ensure_stabilized_profile` returns True, and the run starts builtin 128, which has no hold. Scoring then treats segment 1 as segment 0, so results are mis-scored, and a stray user profile is left behind.
- A second run of the same entry gets a 400 name collision instead.

Fix: the client should refuse builtin ids (>=128), or compare the returned id with the requested one and fail on a mismatch.

### LOW-1: zones page comment is wrong; blanking xzone now disables guard 8 with a 200 (FIXED firmware/PcTools side in f561a4dc; zones_page.html comment left to its owner)

`firmware/KilnFW/App/drivers/http/zones_page.html:2654-2660`. The comment says omitting a blank optional field "keeps the stored value". That holds for `pc_link_abort_silence_ms` (`zones_http_post.c:488` keeps the stored value). It does not hold for the guard fields and xzone: `zones_http_post.c:317` zeroes `tmp`, so an omitted field becomes 0.
- For the guard fields, 0 means the firmware default.
- For xzone, 0 means guard 8 is disabled.

Failure scenario: an operator clears the xzone input (about line 1517, posted at about line 2774) meaning "leave as is". Before `aba69118` this was a 400. Now it is a 200 that silently disables guard 8 (cross-zone). An explicit 0 already did the same, but this new path takes no deliberate value, which conflicts with the spirit of the owner decision never to build a thermal-guard disable. Clearing a tuned guard field also silently reverts it to the default.

Fix: either refuse a blank xzone (keep it required-when-present), or make blank keep the stored value as `pc_link` does. Correct the comment either way.

### LOW-2: PcTools `zones_http_client._format_scalar` passes `""` through, and it is now accepted as 0 (FIXED in f561a4dc)

`tools/PcTools/src/kilnctrl/zones_http_client.py:~705`. A preset or call that carries `""` for a guard or xzone field used to get a 400. Now it is silently accepted as 0, meaning default or disabled. Same root cause as LOW-1, on the PC side.

### LOW-3: reset body uses plain malloc, which is served from internal RAM **FIXED in f8860c92**

`auth_totp_http.c:465`. With `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192`, `malloc(512)` comes from internal RAM.
- The draw is transient, the route is rare, and `RESERVE_INTERNAL` is 32 KB, so the risk to the 8 KB floor is small on its own.
- Combined with HIGH-1, the risk becomes cumulative.

Suggest `heap_caps_malloc(RESET_BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` with an internal fallback, matching the profile handler.

### INFO (stale 0..7 comments, builtin-id test strengthening, note=%00 message FIXED in f561a4dc)

- `profiles_edit_http.c:~526` and `tools/PcTools/src/kilnctrl/profile_edit_http_client.py:122` still say the valid ids are "0..7". They are stale; user slots are 0..99, and builtins now map to -1.
- `test_profiles_http.c`: the new builtin-id test asserts only that slot 0 is used and that the reply is not a 400. It does not check the returned `id` or the saved contents, and it does not check that the builtin is unchanged.
- `setup_wizard_progress.c`: `note=%00` makes `http_form_find_field` return -2, which is reported as the misleading 400 "note too long".
- `setup_wizard_progress.c` `utf8_safe_len`: on invalid input made entirely of continuation bytes it truncates to an empty note (the step is still saved). A dangling lead byte can survive. Both are harmless, and the result is never worse than the input.
- `test_zones_http.c`: the blank-field test cannot tell "blank" from "omitted", because both yield 0. It does verify that blank is accepted and that garbage is a 400.
- Notes are not checked for control bytes (pre-existing). GET JSON-escapes them (`setup_progress_http.c:~102`), so there is no injection.

## Questions answered with no defect

**3ade435b**
- The builtin mapping cannot overwrite a user slot. Allocation picks a free slot under `profiles_save_lock`.
- The builtin cannot be overwritten either: it is const and never written.
- There is no id collision. 100 (live-edit) and 101 (bench) are still 400.
- The profiles page copy works: `copyBuiltin` at about line 1971, the save at about line 2210, and the re-point to `result.data.id` at about line 2270.
- The bench WEB-PROF judges never POST a builtin id, so they are unaffected.
- `mcp_server_aux.py:550` and `cases_heat.py:313` post to user slots only.

**aba69118**
- GET prints every affected field as a number, never blank, so blank cannot round-trip from GET.
- No required field is accepted blank. Only optional guard/xzone/pc_link fields use `zones_http_field_nonblank`.
- `safety_config_page.html:317-318` omits a blank `pc_link`, which is consistent with the firmware.

**b744de75 / 762a1f2a**
- UTF-8 truncation is correct for valid input: strnlen to 31, then back off any continuation bytes, then memset and memcpy.
- The step 11 security chain now requires `res.ok && body.ok === true`.
- The test regex matches the new 24-char note.
