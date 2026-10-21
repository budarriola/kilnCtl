# Review: AG-01/AG-02/AG-04 fix (8370938dd), 2026-10-20

Static review of commit 8370938dd on origin/dev. No code changed, no board used.

Result: HIGH 0, MED 0, LOW 3, INFO 2.

## Answers to the review questions

- **Can the header keep a session alive forever?** No. `X-Kiln-Background` only *suppresses* the activity touch (`http_auth_http.c:446-447`). It can never extend a session, so a client gains nothing by sending or forging it. The idle timeout and the 12 h cap still apply to every request without the header.
- **Should the header be trusted? GET only?** It is safe to trust: a client can only shorten its own session. Today any method with the header skips the touch, including POST. That is harmless but loose (AF-03).
- **Does `kcSessionRole` gating break an admin dashboard feature?** Nothing breaks. `/api/autotune` is ADMIN, so a user or signed-out viewer already got 401. With auth off the role reports `admin`, so the poll still runs. Two small gaps remain: the first poll at load is skipped (AF-01), and the autotune state goes stale after a logout (AF-05).
- **Can AG-02 eviction flush a locked victim's entry?** Yes. An attacker rotating more than 16 LAN addresses can evict the oldest locked slot. The board-wide `TOTP_FORGOT_BOARD_CAP` still bounds total guesses. This matches the AZ-04 owner decision (evict oldest, modest hardening only) (AF-04).
- **Did the `test_status_reports_forgot_blocked` change hide a regression?** No. The board-cap blocked/unblocked checks (`test_auth_totp_http_fuzz.c:529-535`) are kept. The full-table check was inverted to match the new contract, and the new `test_totp_full_locked_evicts_oldest` asserts the victim and that the other locks are intact. The dropped "one free slot: not blocked" check is now subsumed.
- **Does the server-side skip need a C host test?** Yes, a small one (AF-02). Today only a JS source regex covers it.

## Findings

| ID | Sev | Where | Problem | Fix |
|---|---|---|---|---|
| AF-01 | LOW | `main_page.html:4686`, `app.js:2499` | `pollAutotune()` runs once at load before the first `pollSession()` sets `window.kcSessionRole`, so it returns early. A signed-in admin gets no autotune state (chart view, fast-poll rate at `:4616`) for up to 10 s after page load. | Call `pollAutotune()` from the session poll when the role first becomes `admin`, or retry once after the first role resolves. |
| AF-02 | LOW | `http_auth_http.c:446-447`, `test_autotune_poll_gating.js:30` | The header skip is guarded only by a source regex (`X-Kiln-Background"\) == 0`). An inverted or misplaced clause can still match it, and no C test proves that a background request leaves `last_activity` untouched while a normal one touches it. | Fold the header flag into a pure predicate (for example `http_auth_decision_counts_as_activity(tier, decision, is_background)` in `http_auth_enforce.c`), add a host test case, and negtest it. |
| AF-03 | INFO | `http_auth_http.c:447` | The skip applies to any method, so a POST or other action with the header does not count as activity. It cannot extend a session, only shorten it, so it is harmless. | Optional: limit the skip to `HTTP_GET` so a real user action always counts as activity. |
| AF-04 | INFO | `auth_totp_http.c:194-208` | A full table of locked slots now evicts the oldest locked entry, so a LAN attacker with more than 16 addresses can recycle a lockout. Total guesses stay bounded by `TOTP_FORGOT_BOARD_CAP`. | None. This is accepted under the AZ-04 owner decision (2026-10-17, evict oldest, modest hardening). |
| AF-05 | LOW | `main_page.html:4289-4296,4616` | When the role drops from admin (logout or expiry), `pollAutotune` stops but `lastAutotuneStatus` keeps its last value. An autotune that was active keeps the fast poll rate and draws a stale chart until reload. | Set `lastAutotuneStatus = null` (and reset the trace) when `kcSessionRole !== 'admin'`. |
