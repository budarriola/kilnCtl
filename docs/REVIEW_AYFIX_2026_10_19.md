# REVIEW_AYFIX_2026_10_19 (AQ)

Review of the AY fix round on origin/dev: b803bc4af (AY-01/02/03/05), 4f79ed16e
(check_csrf_origin_wiring comment stripping), a435536eb (KNOWN_ISSUES). Context:
REVIEW_AXFIX_2026_10_19.md. Review only, no board, no builds or tests run.
KNOWN_ISSUES rows AQ-01..AQ-06.

Counts: HIGH 0, MED 1, LOW 2, INFO 3.

## AQ-01 MED: re-login with a full table now evicts a bystander session
AY-03 moved the presented-cookie revoke after `web_auth_table_create_session()`
(web_auth_login_http.c ~668 then ~718). When the table is full, create evicts the
least-recently-seen slot (web_auth_session.c ~82, SS-02/SL-01 order) before the
presenter's old slot is freed. The login route is OPEN tier and does not touch the old
session, so the evicted slot is often another client's idle session; then the old one is
revoked too. Net: one re-login logs out an unrelated client, and the table loses a slot.
Before AY-03 the revoke freed a slot first and no eviction happened. Same table-full
setup also lets an all-ADMIN table plus a USER presenter hit 503 where the old order
succeeded (the AY-03 503 test pins that new behavior as intended). Suggested fix: when
the presented token is valid for this IP, reuse/replace that slot in place (or revoke it
right before create, after every refusal path that precedes create), and keep the
AY-03 "failed create keeps old session" property only for paths before the table write.
No test covers a full mixed table with a re-login.

## AQ-02 LOW: AY-01 and AY-05 tests are source-text matches
`test_ay_source_scan()` checks for literal strings, including the comment text
"AY-01: the session token is not needed past the lookup" in the status handler. Deleting
the wipe loop but leaving the comment passes. The 64-bit check matches one exact line.
These are not behavior tests; the commit names no negtest run for them.

## AQ-03 LOW: out_token copy in the prehandler is caller-owned
resolve_role_for_request() now wipes its own buffer, but it strncpy's the token to the
caller (http_auth_http.c ~397); that caller wipes it (AX-03 line ~425) on the normal path.
Any early return between the copy and that wipe (not exhaustively traced here) would leave
it. Worth one host test that drives the prehandler deny paths and checks the buffer.

## AQ-04 INFO: AY-02 is correct
The generation check now runs before the ladder record; a refused success records
nothing (neither reset nor failure), a wrong password is still recorded. The AY-02 host
test distinguishes old/new behavior (5 s vs 10 s step). The later AX-02 check at ~657
is now redundant for ok paths but harmless (it also catches a reset between the two).

## AQ-05 INFO: AY-05 arithmetic
uint64 timeout_ms removes the wrap. `(long)` of the result is safe while timeout_s
< 2^31 s; elapsed_ms is still uint32 (session idle > 49.7 days wraps, already wiped by
the 2^31 ms idle rule).

## AQ-06 INFO: check_csrf_origin_wiring regex and docs
String-first single-pass regex is correct for '/*' in // comments and '//' in strings.
Removing a block comment with '' can join tokens (`a/**/b`), harmless for this check.
Character literal pattern handles '\''. AY KNOWN_ISSUES rows say FIXED without the fix
SHA (b803bc4af), unlike the AX rows.
