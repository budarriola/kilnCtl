# REVIEW_FXAUP_2026_10_17

Adversarial review of the FX-01, FX-02, FX-05, AUP-02 and AUP-03 fixes on origin/dev
(d6a45154a, 95d3e8247, 8363dfc50, 9a2b95da4). Review only, no code changed, bench not touched.

Counts: HIGH 0, MED 1, LOW 4, INFO 2.

## FA-01 MED: stable channel can lose the newest stable release off the first list page
- Where: `firmware/KilnFW/App/drivers/update/update_fetch.c:62` (`FETCH_LIST_PER_PAGE 15u`), `:1027`, `:1039`.
- AUP-02 moved the stable channel from `/releases/latest` to `/releases?per_page=15`. GitHub orders the list
  by creation date, and only one page is read. Pre-releases count toward the 15.
- Scenario: the project tags one `v1.0.0-pre.N` per day. v1.0.0 stable ships, then 15 or more pre-release
  tags follow before the next stable. A board on a stable build older than v1.0.0, with pre-releases off,
  now gets `no_release`. Before this change, `/releases/latest` returned v1.0.0. A user on the stable
  channel is told no release exists.
- Fix direction: on the stable channel, also read `/releases/latest` when the page has no eligible stable
  element, or page further. Add a host test with 15 pre-releases ahead of one stable.

## FA-02 LOW: Wi-Fi path counts sessions that a posted destroy-all has already revoked
- Where: `firmware/KilnFW/App/drivers/http/http_session_iface.c:69-71` (`http_session_table_peek`), `:369-381`.
- The peek does not apply the posted destroy-all, which is correct, and it does not check for one either.
  After the AZ-03 reset gesture posts destroy-all, `http_auth_any_ap_session_active()` keeps counting the
  revoked AP sessions until httpd next touches the table.
- Scenario: the policy timeout is NEVER and a station stays associated. The owner does the reset gesture, and
  no HTTP request follows. The AP fallback stays pinned up by a session that no longer authorizes anything.
  This holds the AP open and does not open auth.
- Fix direction: in the peek path, read `s_destroy_all_posted_gen` and treat posted != applied as "no
  sessions". This needs no write. The FX-01 test in `test_http_session_iface.c` asserts the opposite
  ("still sees the session"), so the fix must change that check.

## FA-03 LOW: unsynchronised cross-task reads in the peek path
- Where: `http_session_iface.c:32`, `:69-70` (`s_web_auth_table_init_done`, a plain bool), `:84-91`, `:379`
  (`session_table_gap_wrapped` reads the 64-bit `s_last_access_ms` written by httpd).
- On the 32-bit Xtensa, httpd can write the 64-bit timestamp while the Wi-Fi task reads it, and the read can
  be torn. A torn value can make the gap look 2^31 ms or more, so one poll reports "no AP session". The
  init flag has no ordering against `web_auth_table_init()`. The table is zero-initialised static storage,
  so a reordered read sees an empty table.
- Both failures go toward "no session" (AP drops for one poll), never toward granting access.
- Fix direction: use `__atomic` loads and stores, or a 32-bit epoch, for the flag and the timestamp.

## FA-04 LOW: `no_stable_release` is now dead
- Where: `update_fetch.c:1033-1043`; `firmware/KilnFW/App/drivers/net/ota_page.html:876`; `test_update_page.js:55`.
- REVIEW_AUTOUPDATE asked to keep `no_stable_release` for a list with no usable stable element. The fix
  maps that case to the generic `no_release`. The page text still mentions pre-releases, so users are not
  misled. The `no_stable_release` string, its page text and its JS test now cover a reason the firmware no
  longer emits. The bench, which has only pre.3, sees a different error string than BENCH_TEST_LOG records.
- Fix direction: when `allow_prerelease` is false and the pick returns NO_RELEASE, return `no_stable_release`.
  Otherwise, delete the page entry and the JS test.

## FA-05 LOW: FX-05 counter logic and wiring have no test (a mutant passes)
- Where: `firmware/KilnFW/App/drivers/persist/persist_scratch.h:46-50`, `:64-70`;
  `persist_scratch.c:7-15`; `firmware/KilnFW/App/drivers/http/dashboard_http.c:563-565`.
- The shared counter and the `dashboard_get_status` assignment build only under `ESP_PLATFORM`. The host
  test sets `s_fake_status.persist_scratch_fallbacks = 7` and checks only the JSON formatting.
- Scenario: deleting the `persist_scratch_note_fallback()` call, or the assignment, passes every host test.
  `/api/status` then reports 0 while fallbacks eat into the 8192 B internal floor.
- Fix direction: compile `persist_scratch.c` on the host, and add a test that triggers a fallback with
  `persist_scratch_test_fail_nth` and reads the count back.

## FA-06 INFO: stale comment in flush_head
- Where: `firmware/KilnFW/App/drivers/update/update_stage.c:288-289`. The comment says "An invalid/blank
  descriptor version leaves the declared one". Since AUP-03, that holds only for ungated stages, and every
  HTTP upload and fetch is gated (`update_http.c:372`, `update_fetch.c:598`).

## FA-07 INFO: FX-02 and AUP-03 verified, no defect
- FX-02 (`web_auth_store.c:503-518`): the physical reset (`:929-985`) and `clear_all_credentials` (`:1042`) both
  write a blob with configured=false, so the key stays present and bootstrap reopens. Turning web auth on
  requires an admin password (`security_backend_web_auth.c:253-262`). So a key that is NOT_FOUND while the
  marker is set means the key was lost, never "never configured". An LCD-only enable also sets the marker,
  but bootstrap needs web auth effectively on. Any failure to read the marker fails closed. This matches
  login fail-closed.
- AUP-03: the gated branch now refuses an unparseable descriptor version. A declared "99.0.0" cannot override
  a valid older descriptor version (VERSION_MISMATCH), so this adds no downgrade path. The F2 test was
  inverted rather than deleted. Both fixes keep the existing allow_prerelease and allow_downgrade gates, so
  neither adds a channel-confusion path.
