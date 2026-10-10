# Dev web review 2026-10-09: browser side of 0aec8e7c

Scope: the web-page half of `0aec8e7c` on origin/dev ("Web: host-refusal guidance on
login/forgot/reset/stage; zones blank guard refusal + save timeout; wizard step save
failures reported, step 11 needs matching read-back"). Files: `http/app.js`,
`http/login_page.html`, `net/ota_page.html`, `http/zones_page.html`,
`http/setup_wizard_page.html`, `test/test_host_refusal_pages.js`. Review 11 covers the
commit broadly. This review covers browser behavior only: XSS sinks, double submit,
aborted requests, raw host text, and the step 11 read-back. Reviewed at origin/dev
`0dd056c6`. None of the five pages changed between `0aec8e7c` and that tip. No code was
changed.

## Findings

### MED-1: the login page change does nothing; `/login` still shows raw `{"error":"bad_host"}` [FIXED 2026-10-09 in 6d8b7314 (MED-1) / 731f0466 (LOW-1..5)]

`login_page.html` loads no `app.js` (its only script is the inline `<script>` at line 45,
plus `theme.css`). `window.kcHostRefusalFromText` is therefore always undefined there, and
both new guards (`(window.kcHostRefusalFromText && ...) || text || ...`, lines 110 and 151)
fall through to the raw response body.

This is the page that matters most for a host refusal. `http_auth_http.c`'s prehandler
host-checks every non-GET request, but a GET of a page is not host-checked. So an operator
who opens the board by a foreign name (DNS rebinding, a proxy, an unlisted alias) is
on `/login` (directly or through a redirect) gets the page loaded, and then the login POST or bootstrap POST is
refused with 403 `{"error":"bad_host"}`. The operator sees that JSON verbatim. The
`textContent` sink means there is no XSS. The fix (out of scope here) is to inline the
small mapper in `login_page.html` or load `/app.js` there.

`test_host_refusal_pages.js` line 33 checks only that the guarded expression is present in
the page source, so it passes while the code is dead. A behavioral test, or an assertion
that the page can reach the helper, would catch this.

### LOW-1: the zones Save 30 s timer also runs while the login modal is open; the retry can abort at once with a false "board did not answer" [FIXED 2026-10-09 in 6d8b7314 (MED-1) / 731f0466 (LOW-1..5)]

`zones_page.html` passes `signal: saveAbort.signal` into `window.fetch`, which is
`app.js`'s wrapper. On a 401, or a 403 `insufficient_role`, the wrapper opens the login
modal and later calls `retryOnce()`. `retryOnce()` copies every `init` key, including the
same `signal`, and nothing pauses the timer while the modal is open. If the operator takes
more than 30 s to sign in (finding the password, a TOTP-era flow), the signal is already
aborted when `retryOnce()` runs. `nativeFetch` then rejects at once with `AbortError`, and
the page says "Save timed out after 30 s -- the board did not answer". The board was never
asked. The operator must click Save again.

The Save button stays disabled for the whole modal, so this causes no double submit.

There is a related limit when the timer fires on a POST that has already reached the board:
the save may have committed, but the message says only "did not answer ... try again" and
does not reload the form. Retrying re-posts the same form. The page sends no generation
token, so a retry is not refused as a lost update, and it is idempotent. The text could say
the save may or may not have landed and suggest reloading.

### LOW-2: blank-guard refusal names wire keys, and can name a field the operator cannot see [FIXED 2026-10-09 in 6d8b7314 (MED-1) / 731f0466 (LOW-1..5)]

The refusal lists keys such as `z0_wrongdirwindow`: the internal wire name with a 0-based
zone index, while the page labels zones 1-based with descriptive labels. Six of the eight
guard inputs sit in `.row2.heaterOnly` rows, which are hidden for non-heater zone types but
still read into `params`. If an operator clears one of them and then switches the zone's
type, Save is refused and names a field that is no longer visible.

GET-populated values cannot be blank (`|| 0` defaults), so this happens only after an edit.
The logic itself is correct: the refusal comes before `disabled = true` and before the
POST, and it covers `type=number` inputs holding non-numeric text, whose `.value` is `''`.

### LOW-3: step 11's read-back chain is not returned, so a progress-save rejection is unhandled [FIXED 2026-10-09 in 6d8b7314 (MED-1) / 731f0466 (LOW-1..5)]

In `renderStep11()`, the `.then(function (r) { ... fetch('/api/auth/config')... })` body
does not `return` the inner read-back chain. A rejection from `postStepState(11, 'done')`
therefore never reaches the outer `.catch`: no error shows in `step11Err` and an unhandled
rejection is logged. Two cases cause it: a network failure, and an `AuthCancelled` when
enabling login dropped the session and the operator cancels the modal. A refused (non-2xx)
progress save does get shown, via `stepSaveFailed`. The non-returned chain existed before
this commit, but the commit's claim that "step save failures are reported" does not hold
for this path.

Smaller items:

- On a mismatched read-back, `step11Msg` still starts with "Saved. Read back: ..." next to
  the red "NOT marked done" error. The two messages read as contradictory.
- If the operator cancels the login modal during the read-back GET (ADMIN tier, so it 401s
  right after login is enabled), the page shows "Could not read the setting back", not
  "Sign-in cancelled".

Read-back race: none on the server side. `set_policy` goes through
`web_auth_store_set_policy()` synchronously before the POST answers, so the following GET
reflects it.

Step 11 Save has no double-submit guard. A double click sends two
`set_web_password`/`set_policy` chains and two progress POSTs. These are idempotent and
converge. The guard was already missing before this commit; the same is true of steps
1, 2, 4, 5 and 6.

### LOW-4: `stepSaveFailed` leaves `stepStatusLine` permanently styled as an error [FIXED 2026-10-09 in 6d8b7314 (MED-1) / 731f0466 (LOW-1..5)]

`stepSaveFailed()` sets `className = 'werr'` on `#stepStatusLine`. `gRenderStepDetail()`
later rewrites only that element's `textContent` (around line 3006), so every later
"Current state ..." line on that page load renders bold red. The behavior predates this
commit, but the commit adds two new callers (steps 3 and 7).

On the same paths, step 3's `okEl` keeps "Write accepted and every channel verified
converting." on screen next to the failure line. That is true (the zones write did land),
but nothing says the step was not marked done.

### LOW-5: some pages still show the raw host-refusal code [FIXED 2026-10-09 in 6d8b7314 (MED-1) / 731f0466 (LOW-1..5)]

The pages that were not changed still show the host-refusal code verbatim:

- **zones Save:** the catch handler unwraps only `parsed.reason`, so a 403 shows
  `Save failed: {"error":"bad_host"}`.
- **wizard progress saves:** `Could not save this step: bad_host -- try again.`

The fetch wrapper's one-shot `kcAlert` explains the cause on the first refusal of a page
load. It is suppressed afterwards (`hostRefusalShown`), so a second refusal shows only the
code.

## Checked, no finding

- **XSS:** every sink for the new strings is safe:
  - `textContent`: the login modal, forgot/reset (`forgotErrorEl`, `forgotBackToStep1`),
    `login_page.html`, and the zones `msg`.
  - `Error.message` then `textContent`: the stage XHR, via `stageRefusalText` and `stageMsg`.
  - `kcEscapeHtml`: the wizard, through `renderErrList` (`r.body.error` from the host
    included).
  - `kcAlert`/`kcConfirm` build paragraphs with `textContent`.

  `kcHostRefusalText`'s constant contains `<name>.local`, which would become markup only
  under `innerHTML`. No such sink exists today.
- **`kcHostRefusalFromText`:** it maps only the two exact error codes and returns null for
  non-JSON, other JSON, or `null`. The `try` also covers `kcHostRefusalText` throwing.
- **forgot/reset 403 branches:** they keep the `gen !== forgotGeneration` staleness guard
  and fall back to the old `kcForgotStatusMessage(403, null)`.
- **stage XHR:** it applies the mapping only to a 403 and keeps the raw body for other
  statuses, so the 409 and 428 parsing in `stageRefusalText` and `stageXhrUpload` is
  unchanged. The XHR bypasses the fetch wrapper, so it opens no modal.
- **zones Save:** the timer is cleared on both settle paths, the button is re-enabled on
  every path, and the `AbortError` branch returns before the JSON/auth-cancel handling.
  Without `AbortController` the request falls back to no timeout.
- **wizard steps 1, 2, 4, 5 and 6:** `postStepStateOrThrow` rejects before
  `armStepMessage`, so "Saved." no longer appears after a refused progress save.

## Gzip page copies

`firmware/KilnFW/App/drivers/CMakeLists.txt` gzips `KILNCTL_GZIP_ASSETS` at configure time
into the build directory. The list covers all five changed pages (`app.js`,
`login_page.html`, `zones_page.html`, `setup_wizard_page.html`, `ota_page.html`). Each
source file is added to `CMAKE_CONFIGURE_DEPENDS`, so the next KilnFW build reconfigures and
regenerates the copies automatically. No `.gz` file is tracked in git, so nothing needs
regenerating or committing. `tools/check_web_gzip_parity.ps1` SKIPs (exit 3) in a tree with
no `build/` and SKIPs on a `.gz` older than its source. It fails only on a hand-edited or
corrupt one.

## Tests run (worktree at origin/dev)

- 28 node page tests under `firmware/KilnFW/App/test` that reference these pages passed:
  `test_host_refusal_pages.js`, `test_setup_wizard.js`, `test_forgot_password_modal.js`,
  `test_login_*`, `test_ota_page_poll_auth.js`, `test_update_page.js`,
  `test_web_xss_fixes.js`, `test_zones_*`, `test_strict_form_pages.js`, `test_fetch_auth_ack.js`
  and others. 0 failed.
- `lint_pages.js` (run with its drivers-directory argument), and
  `tools/check_lint_pages.ps1`: 0 problems.
- `tools/check_web_gzip_parity.ps1`: SKIP, exit 3, because there is no build output in the
  worktree.

`test_host_refusal_pages.js` is almost entirely source-regex assertions. It never runs
`postStepStateOrThrow`, the step 11 chain, the abort path or the login page's helper lookup,
which is why it missed MED-1.
