# Web UI JS coverage gaps, 2026-10-10

Scope: `firmware/KilnFW/App/drivers/http/*.html|js` and `drivers/net/*_page.html`, against
`firmware/KilnFW/App/test/test_*.js` (run by `tools/check_page_js_tests.ps1`) and
`lint_pages.js`. `zones_page.html` and app.js's password-reset flow were out of scope
(another fixer owns them).

Existing style: most tests are regex/source-contract tests or extract one function. Few
EXECUTE a click handler, so "what text does the operator see for HTTP code X" was mostly
unpinned. This sweep adds `_page_vm.js` (vm + fake DOM; runs a page's last inline script)
and four executing tests (items 1-4 below).

## Ranked list (impact on a kiln operator, then likelihood)

| # | Behaviour | Status |
|---|-----------|--------|
| 1 | Settings destructive actions (factory-reset scopes, software reset, cfg-fs format): confirm gate, request body, `Failed: <server text>` for every refusal, no success line on failure, catch-arm wording | TESTED `test_settings_reset_flows.js`; defects D1, D2 |
| 2 | Backup restore: dry-run refusal text, cancel, `X-Kiln-Config-Ack-Delete` count, success vs refusal for 400/403/409/413/428/500, auth cancel, network failure | TESTED `test_backup_restore_flow.js`; defect D3 |
| 3 | C/F conversion: `kcUnit.toDisplay/fmt` offset, null/NaN, bad `temp_unit`, `set()` body; `main_page` `fmtDeltaC` must not add 32 | TESTED `test_kcunit_conversion.js` |
| 4 | Live (mid-firing) profile edit: save 400 bound / 409 window text verbatim, 200 with `ok:false`, overwrite checkbox gate, save-as name encoding, fork refusal | TESTED `test_live_profile_action_errors.js`; defects D4, D5, D6 |
| 5 | Unsaved-edit loss: no page registers `beforeunload` (0 hits in every html/js), so navigating away from the profile editor, wizard, safety config or kiln-configs forms silently drops edits; also pollers that re-render over an open form | UNTESTED; defect D7 (page-wide) |
| 6 | Session expiry on polling pages: a 401 from a background `setInterval` poll surfaces as a generic "Could not reach the board" (see D4's catch arms); `test_login_auth_wrapper.js` covers the fetch wrapper but no page's poll catch arm | UNTESTED |
| 7 | Mixed units inside one view: `profiles_page.html:1960` renders targets in the display unit while the editing inputs stay raw degrees C (app.js kcUnit comment: "inputs send raw C"), so an operator in F reads 1832 F and types into a box that expects 1000 | UNTESTED; design hazard, not a code bug |
| 8 | `main_page.html` run controls (start, stop, clear trip, pause) per refusal code, including the 409 system-mode gate text and the 428 interlock | UNTESTED (258 KB page, 84-handler class with no executing test) |
| 9 | `security_page`, `wifi_provision_page`, `ota_page` status-code handlers (31 `.ok/.status` sites): password change, Wi-Fi join, stage/install refusal text; only the OTA poll-auth and update-page cards have tests | UNTESTED |
| 10 | Mojibake / encoding: no check catches double-encoded UTF-8 in page sources; D6 shipped unnoticed | UNTESTED; suggest a `lint_pages.js` rule |

## Defects found and fixed (ab687431c)

- D1 FIXED ab687431c. settings_page.html: sign-in cancel on factory reset, software reset and cfg-fs format now shows "Sign-in cancelled" (the in-progress text is cleared).
- D2 FIXED ab687431c. "Request sent -- the board may already be rebooting" only for a dropped connection (fetch TypeError); any other failure shows "Request failed: <real error>".
- D3 FIXED ab687431c. backup_page.html: a 500 from the real import POST says "Restore failed partway -- some settings may have changed"; other refusals stay "Restore refused".
- D4 FIXED ab687431c. live_profile_page.html: save/fork/decide read the body as text and parse defensively; a non-JSON refusal shows "HTTP <status>: <body>".
- D5 FIXED ab687431c. Sign-in cancel clears Saving/Forking/Working; "Discard working copy" goes through kcConfirm.
- D6 FIXED ab687431c. Four double-encoded ellipses replaced with "..."; lint_pages.js now flags U+00E2 U+20AC in any page (test_lint_mojibake.js).
- D7 PARTLY FIXED ab687431c. live_profile_page.html has a beforeunload guard (dirty after a segment edit, cleared on load/save/decide). settings_page.html has no editable form fields, so there is nothing to guard. FOLLOW-UP: the same guard for profiles_page, setup_wizard_page, safety_config_page, kiln_configs_page, settings_display_page and zones_page (zones_page was left alone because another fixer owns it).

All tests now hard-assert these; the KNOWN-DEFECT markers are gone.

## Negative tests (tools/negtest.ps1, each CAUGHT by an assertion `FAIL:` line, not a crash)

backup failure shown as "complete" (FAIL real POST 400); Ack-Delete always sent (FAIL no
deletions); reset failure shown as success (FAIL reset: HTTP 401..500); confirm result ignored
(FAIL declined confirm); kcUnit drops +32 (FAIL Fahrenheit); delta gets +32 (FAIL delta of +10 C);
live save drops server text (FAIL save 400); overwrite ignores checkbox (FAIL overwrite
without the checkbox). 8/8 CAUGHT, baseline passed.
