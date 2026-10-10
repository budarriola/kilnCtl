# Review: web page defect fixes D1-D7 (2026-10-10)

Commits reviewed (origin/dev): `8711ebf7d` (the fix), `c4e879db9` and `3f952a53f` (audit doc
updates in `docs/audits/WEB_JS_COVERAGE_GAPS_2026-10-10.md`). Reviewer: Opus, read-only. No code
changed by this review.

Verdict: **no HIGH or MED findings.** The fixes do what the audit asked. Five LOW findings, listed
below. Two are gaps in the tests and three are wording or scope edge cases.

## Verification run

- `node test_lint_mojibake.js`, `test_live_profile_action_errors.js`, `test_settings_reset_flows.js`,
  `test_backup_restore_flow.js`: all passed. `check_js_host_tests.ps1` finds all four by its
  `test_*.js` glob, so they run in the standing suite.
- `node lint_pages.js firmware/KilnFW/App/drivers/http`: 0 problems.
- Negative test of the lint rule: changing the pattern to `U+00E2 U+20AB` (scratch copy) makes
  `test_lint_mojibake.js` fail 2 of 3 cases. Caught, so the test is not vacuous.
- I scanned every `.html/.js/.css` under `drivers/` and `KilnFW_recovery/` for the wider mojibake
  shapes (`[U+00C2 U+00C3][U+0080-U+00BF]` and `U+00E2 U+20AC`). There were 0 hits.

## Answers to the review questions

**1. Does "board may already be rebooting" show only on a real connection drop, and never hide a
real refusal?** Yes, with one limit on the wording (L1). `kcResetCatch()`
(`settings_page.html:279-289`) runs only from `.catch()`. Every HTTP refusal (4xx/5xx, including
the 428 a declined safety-ack hands back from `kcFetchWithSafetyAck`) resolves the promise and
reaches the `.then` arm, which prints `Failed: <server text>`. In the catch arm, AuthCancelled is
checked first. The "rebooting" wording needs `err.name === 'TypeError'` (line 284). In practice
only a fetch network failure or a body-stream read failure produces that name, because the
`.then` arms only assign `textContent` and cannot throw it. Every other rejection prints
`Request failed: <message>`.

**2. Can a 500 from import be shown as "refused", or the reverse?** A 500 is never shown as
"refused" (`backup_page.html:243`). In `backup_import.c:4336`, every partial write is a 500, so a
real partial write always gets the "failed partway" text. The reverse error only goes in the safe
direction: a 500 can be shown as "failed partway, some settings may have changed" when nothing
was written. See L2.

**3. Is any error text from an untrusted body inserted as HTML?** No. `settings_page.html` and
`backup_page.html` only assign `textContent`. `live_profile_page.html` builds `innerHTML` in
three places (fork, save, decide), and each one passes `res.data.error` through `escapeHtml()`.
`escapeHtml()` delegates to `window.kcEscapeHtml`, which escapes `& < > " '`, and the page's
fallback escapes the same set. The new `readJsonResult()` puts the raw body into `data.error`,
truncated to 200 chars, and that value reaches the DOM only through the same `escapeHtml()`.
`showRefusal`, `setStatusText` and `workingName` use `textContent`.

**4. Does the beforeunload guard clear on every success path, and never block navigation after a
save?** Yes on both. `dirty` is cleared on save success (`live_profile_page.html:417`), on decide
success (`:443`, which covers save_as, overwrite and discard), and in `loadWorkingCopy()`
(`:291`, which covers reload and every working_id change). After a successful save, nothing sets
`dirty` again until the next real `input` event: `renderSegments()` rebuilds with `innerHTML`,
which fires no input event. Edge cases are in L3.

**5. Does the lint rule flag legitimate UTF-8?** No in practice. The rule (`lint_pages.js:188`)
matches only the pair U+00E2 U+20AC ("a-circumflex" followed by "euro sign"). No real English UI
text contains that pair, and the current tree has 0 hits. Real U+2026 and U+00B0 characters pass,
and the test checks the ellipsis case. The rule is narrow in the other direction too: see L4.

**6. Are the tests non-vacuous?** Mostly. The mojibake test was negative-tested (above). Each
settings and backup assertion pins a distinct branch. For example, `reject: 'other'` fails if the
TypeError test is removed, and `reject: 'net'` fails if the "rebooting" text is removed. One gap
is in L5.

## Findings

### L1 (LOW): the "Request sent" wording is used even when the request never left

`settings_page.html:284-286`. Fetch rejects with the same `TypeError` whether the connection
dropped after the board got the request or the request never left (board unreachable, Wi-Fi down
on the PC, DNS failure). Both cases print "Request sent -- the board may already be rebooting".
This does not hide an HTTP refusal, which was the question asked. But an operator whose request
never left is told it was sent. The browser cannot tell these two cases apart, so the honest fix
is wording: "No reply -- the board may be rebooting, or the request may not have reached it."
D2 narrowed the message correctly. This is what remains.

### L2 (LOW): a 500 sent before any write is labelled "failed partway"

`backup_page.html:243`. Several 500 responses come before any write: `backup_import.c:4275` and
`:4327` (out of memory in the job, before `backup_import_apply`), `:4572` and `:4603` (ctx alloc
or async handoff failure on httpd_worker), and httpd's own 500s. All of them print "Restore failed
partway -- some settings may have changed: out of memory". This errs in the safe direction (it
says "may") and the server text is shown. It is still a false alarm that can send an operator to
check every setting after an OOM where nothing changed. A distinct body or status for "partial
write" (or an `X-Kiln-Partial-Write: 1` header set only at `:4336`) would let the page tell the
two apart.

Related, pre-existing and outside this diff (`backup_page.html:252`): if the connection drops
after the import job has started committing (`http_async_job` keeps running), the page prints
"Upload failed -- check the connection and try again". That reads as "nothing happened", but the
restore may have been fully or partly applied. Re-running the same file is harmless, so this
stays LOW. The wording should still say the result is unknown.

### L3 (LOW): edge cases in the beforeunload guard

`live_profile_page.html:267-291, 355-368, 417`.
- When the firing ends without a pending decision, or the working copy disappears (another client
  discards it, which hits the `live.active && !hasWorking` branch), the editor card is hidden but
  `dirty` stays true. Leaving the page then prompts about edits that can no longer be saved. The
  prompt is harmless and arguably accurate (the edits are lost), but it is stale.
- If `working_id` changes underneath the page (for example, another client forks or saves), the
  poll's `loadWorkingCopy()` re-renders over the operator's in-progress edits and silently clears
  `dirty`. The silent clobber already existed before this change. The guard now joins the
  clobber instead of flagging it.
- If the operator types between clicking Save and the response arriving, those keystrokes are
  marked clean (`:417`), and the follow-up reload then discards them.

None of these blocks navigation after a save.

### L4 (LOW): the lint rule covers only one mojibake shape

`lint_pages.js:182-195`. The rule catches only the cp1252 decode of U+2000-block punctuation:
ellipsis, dashes and smart quotes. It does not catch the most likely shape for this project, a
double-encoded degree sign `U+00C2 U+00B0` ("A-circumflex, degree") in a `°C` string, or other
`U+00C2/U+00C3` + `U+0080-U+00BF` pairs. It also misses the Latin-1 decode variant, which
produces `U+00E2 U+0080` instead of `U+00E2 U+20AC`. Adding `[ÂÃ][\u0080-¿]`
would catch these, and the tree has 0 current hits for it, so it would not false-positive today.
French text that legitimately contains "Ã" followed by a symbol would collide, but these pages are
English-only.

### L5 (LOW): no test that the guard clears on success

`test_live_profile_action_errors.js:103-113` checks that the guard is registered, is quiet before
an edit, and prompts after one. No case checks that `dirty` is cleared after a successful save or
decide. Removing `dirty = false;` at `live_profile_page.html:417` or `:443` would not fail any
test. That is the "never blocks navigation after save" property this review was asked about. The
fix is one more scenario: fire `input`, click Save with `{status:200, json:{ok:true}}`, then call
the beforeunload handler and assert it does not prevent.

### Note (no finding): a literal JSON `null` body

`readJsonResult()` (`live_profile_page.html:258`): if a body parses to `null`, the
`res.data.ok` lookup throws a TypeError, which shows up as "could not reach the board". A JSON
primitive body loses its text to the generic fallback. `profiles_live_http.c` always sends an
object (`send_err_json`, `{"ok":...}`), so this cannot happen today.

## Audit doc commits

`c4e879db9` and `3f952a53f` are correct after the second one. No stale `ab687431c` reference
remains in `WEB_JS_COVERAGE_GAPS_2026-10-10.md`. "D7 PARTLY FIXED" correctly lists the other
pages that still have no guard.
