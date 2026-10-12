# Web batch review (2026-10-10)

Scope: origin/dev commits `6c341bb3` (auth reset strength-before-consume, TOTP fuzz recv cap,
zones save timeout notice, JS test coverage) and `2d26eae72` (audit doc SHA fill-in only).

Files read: `firmware/KilnFW/App/drivers/http/auth_totp_http.c` (whole reset handler and
backoff), `totp_http_core.c` (`totp_reset_token_consume`), `security_backend_web_auth.c`
(`web_auth_backend_set_web_password`), `persist/web_auth_store.c` (`web_auth_password_check`),
`app.js` (forgot/reset modal), `tools/PcTools/src/kilnctrl/mcp_server_totp.py`,
`zones_page.html` (save handler, `loadCurrent`, `showSaveTimeoutNotice`),
`setup_wizard_page.html` (`lockSave` and step handlers), `test_auth_totp_http_fuzz.c`,
`test_web_review_fixes.js`.

This was a read-only review. No code was changed and no board was touched. Line numbers are for
origin/dev `feda5da10`.

## Summary

| ID | Severity | Area | One line |
|----|----------|------|----------|
| W1 | LOW | reset fix, clients | The fix has no effect through either client today. `app.js` nulls the token before the POST and sends the operator back to step 1 on any failure, and the reply for a weak password is the same generic `400 {"ok":false}` as a bad token. `totp_reset_password` is single-shot. The operator still has to redo `forgot`, and is never told the password was the problem. |
| W2 | LOW | reset test gap | The failure backoff on the new weak-password path is not pinned. Turning it into a success record (which clears the ladder) passes the suite (negtest `weak_no_backoff` MISSED). That backoff is what throttles the pre-token branch (W3). |
| W3 | INFO | reset side channel | The weak check now runs before any token check, so an unauthenticated caller reaches `strcmp(password, ap_password / ap_ssid / username)`. Body, status and backoff are identical on both failure paths. Only timing differs, by one 4-slot constant-time consume. That is microseconds against Wi-Fi jitter, behind a 5 s+ per-IP ladder, and it is only an exact-guess oracle for the AP passphrase, which WPA2 already exposes offline. Not exploitable. Keep it in mind if W1 is fixed with a distinct "weak" reply. |
| W4 | LOW | zones JS test vacuity | The "AbortError branch does NOT auto-reload" assertion matches only the text `loadCurrent(`. A deferred reload such as `setTimeout(loadCurrent, 0)` passes (negtest `abort_deferred_reload` MISSED). |
| W5 | INFO | zones page | Other `loadCurrent()` callers still overwrite unsaved edits without asking: sweep completion (`zones_page.html:2567`) and autotune Accept (`:4297`). This predates the batch and is outside LOW-5's scope. |
| W6 | INFO | dev tree | `firmware/KilnFW/App/test/build_host_tests.ps1` on origin/dev still has conflict markers at lines 3300-3308 (`$totalExpected` 80 vs 76; the merged value is likely 81). A full host-test run on dev failed to parse. Since resolved on dev by `ecfd6bbb4` (81). For this review the conflict was resolved locally, not committed, and only `-Only auth_totp` was run. |

## auth_totp_http.c `reset_post_handler`

Verdict: correct. Apart from W1-W3, nothing to fix.

- **Token replay.** None. The weak branch never touches the token table. It zeroes its copies
  of the token and password and returns. `totp_reset_token_consume` is unchanged: it sets
  `used = true` only on a (token, username) match that has not expired and was not already used.
- **Consume still atomic with the write.** The order is unchanged: consume, then the still-bound
  re-check (TOTP enrolled, admin record, username), then `set_web_password`, all in the one httpd
  task. The pre-check uses exactly the backend's arguments: role ADMIN gives
  `username_for_check = username`, and both read the same `wifi_prov_get_ap_ssid/password()`. So
  the backend's own weak refusal after consume can only happen if the AP password changes in the
  microseconds between the two calls. A storage failure after consume still burns the token. That
  was true before this batch and fails closed.
- **Backoff bypass.** None. `totp_backoff_gate` runs before the body is read, and the weak branch
  records a failure (but see W2: no test pins this). The ladder is 5/10/30/60/300 s against a
  120 s token TTL, so about three weak retries fit inside one token's life.
- **Response shape.** The weak reply is the same status and body as every other reset failure,
  so it adds no enumeration channel. That is also why W1 happens.
- **Test fake.** The fake `set_web_password` refuses below 12 characters, while the real rule is
  10 characters plus the other rules. This no longer matters for the weak path, because the
  handler now calls the real `web_auth_password_check`, which the test links.

## test_auth_totp_http_fuzz.c

- **New weak-password case.** It pins the fix. Negtest `drop_precheck` and
  `precheck_after_consume` (which spends the token before the check) are both CAUGHT at `:450`.
  The first assertion's `st >= 400` is loose (a 429 or 500 would pass), but
  `fuzz_state_intact()` and the strong retry that follows cover that.
- **Recv cap (1000 calls).** It cannot hide real behavior. Every handler call goes through
  `fuzz_post`, which resets the counter and then asserts `!s_recv_spun`, so tripping the cap is a
  loud FAIL and can never produce a pass. Legitimate use stays far below the cap: bodies are at
  most `RESET_BODY_MAX` 512 bytes, the chunk is 999 bytes, and `s_chunk` is never changed. Negtest
  `reset_recv_spin` (`ret <= 0` to `ret < 0`) is CAUGHT at `:246` instead of hanging. The returned
  -1 is `HTTPD_SOCK_ERR_FAIL`, not the timeout code a retry loop would treat as transient.

## zones_page.html `showSaveTimeoutNotice`

Verdict: correct.

- The AbortError branch no longer calls `loadCurrent()`. The form is left exactly as the operator
  typed it, including any inherited values the save handler re-resolved before the POST.
- The text says the result is unknown. The button is labelled "Reload from board (discards your
  edits)" and is the only path to `loadCurrent()` from here.
- The button sits inside `#msg`. It disappears the next time the save handler sets
  `msg.textContent`, for example on a new Save, so no stale button outlives the notice.
- Saving again after a timeout whose save actually landed re-posts the same values. The
  `POST /api/zones` generation re-check is internal to the server, so the client sends no stale
  generation and there is no spurious 409.

## test_web_review_fixes.js vacuity (negtest)

| Mutation | Verdict |
|----------|---------|
| `loadCurrent()` added inside `showSaveTimeoutNotice` | CAUGHT |
| AbortError branch calls `loadCurrent()` instead of the notice | CAUGHT |
| AbortError branch adds `setTimeout(loadCurrent, 0)` after the notice | MISSED (W4) |
| Reload button handler emptied | CAUGHT |
| `ZONE_OPTIONAL_KEY_RE` drops the blank-value anchor (drops filled optionals too) | CAUGHT |
| `lockSave` no longer refuses an already-disabled button | CAUGHT |
| `lockSave` no longer disables the button | CAUGHT |
| Step 5 handler ignores a null unlock | CAUGHT |

Minor: the "Save handler runs params through omitBlankOptionalParams" check searches the whole
rest of the file after the first `'Saving` text. It would still pass if the call moved into a
later handler. That is low risk, because the behavioral `ob(...)` assertion covers the function
itself.

## Tests run

- `build_host_tests.ps1 -Only auth_totp`, with the W6 conflict resolved locally:
  1/1 executable, 1819/1819 checks passed.
- `node test_web_review_fixes.js`: 35 passed, 0 failed.
- Negtest (`tools\negtest.ps1`, throwaway copies, the real tree was verified unchanged):
  - C, run with `-IncludeDirty` to carry the local conflict resolution: 3 CAUGHT, 1 MISSED (W2).
  - JS: 7 CAUGHT, 1 MISSED (W4).

## Resolution

W1, W2, W4, W5 fixed 2026-10-10: `app.js` `kcResetPasswordProblem` and `totp_http_client.reset_password_problem` precheck strength before the token or TOTP code is spent (the AP SSID/passphrase equality rule stays board-side); `test_auth_totp_http_fuzz.c` pins the weak-path backoff (429 on immediate retry); `handleSaveAbort` is run behaviourally; sweep completion and autotune Accept go through `reloadUnlessDirty` (warns, offers Reload, never silently discards). Negtests CAUGHT.

W6: done by `ecfd6bbb4` after this review was written (`$totalExpected = 81`, keeping both
   comment lines).
