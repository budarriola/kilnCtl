# REVIEW_AQFIX_2026_10_19 (AR)

Review of the AQ fix round on origin/dev: 388b5fd6d (AQ-01 revoke before create, AQ-02/AQ-03
wipe-placement scans, host tests) and ef793919d (KNOWN_ISSUES). Context: REVIEW_AYFIX_2026_10_19.md.
Review only, no board, no builds or tests run. KNOWN_ISSUES rows AR-01..AR-06.

Counts: HIGH 0, MED 0, LOW 2, INFO 4.

## AR-01 LOW: no test pins "revoke after every refusal path"
The revoke now sits after the hash (AX-04), destroy-gen (AX-02) and Set-Cookie refusals and right
before create. Dropping `test_ay03_failed_create_keeps_presented_session` is correct for the 503
(after a revoke a slot is free, so create cannot refuse; a 503 now implies nothing was revoked).
But nothing replaces it for the earlier refusals: moving the revoke block above the destroy-gen check
or the Set-Cookie check (a user loses the old session and gets no new one) passes every host test.
Add a span scan (revoke block lies between "Set-Cookie formatting failed" and
`web_auth_table_create_session(login_table`) or a behavior test driving the AX-02 gen-moved path with
a presented valid cookie and asserting it survives; negtest it.

## AR-02 LOW: GD-04 scan dropped without a replacement ordering check
The Set-Cookie snprintf now runs before create, so the GD-04 orphan cannot arise and its destroy call
is rightly gone. The new invariant (cookie formatted before create; the 500 path never reaches create)
is unpinned: moving the snprintf back below create without restoring the destroy passes. Practically
unreachable (fixed 64-hex token, 109 of 128 bytes), hence LOW. Fold into the AR-01 span scan.

## AR-03 INFO: refusal after revoke is not reachable
After `http_auth_session_status()` succeeds, `session_logout_impl()` calls
`web_auth_table_destroy_session()`, so a slot is free and `web_auth_table_create_session()` takes it
via the first-free branch: no SL-01 refusal, no eviction. Remaining steps after create (set_via_ap,
set_hdr, sendstr) do not refuse. Revoke and create run on the single httpd task with no other web-slot
writer between them, so no race frees-then-steals the slot. Set-Cookie failure is handled before the
revoke (wipes cookie and token_hash, 500, presented session kept).

## AR-04 INFO: same-IP cookie misuse grants nothing new
An attacker behind the victim's NAT IP who presents the victim's stolen cookie with the attacker's own
credentials revokes the victim's session. Holding that cookie already allows POST /api/auth/logout
(or using it), so this is no new capability; a USER login revoking a presented ADMIN cookie likewise
only destroys, never inherits, the role (new session role comes from store_role). Different-IP cookies
fail the status IP binding and are not revoked. Accepted.

## AR-05 INFO: lockout ordering and SL-01 unchanged
AY-02 ordering (generation check, then record success/failure under s_login_lock) is untouched; the
revoke runs after the ladder update, so a refused or failed login never revokes. SL-01: a USER login
into an all-ADMIN table without a presented own session still gets the 503; with one, it reuses its own
freed slot (new test). ADMIN logins keep LRU eviction when no own slot is presented.

## AR-06 INFO: redundant wipe on the 503 path
`token_hex` is wiped right after the snprintf and again on the 503 path; harmless (volatile wipe of an
already-zero buffer). The 503 path now also wipes `cookie`, which closes that copy. No action.
