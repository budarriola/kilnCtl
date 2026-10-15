# AUDIT_TOTP: KilnFW TOTP password reset

Date: 2026-10-14. Base: origin/dev `153c12cf5`. Read-only source audit; no board
access, no code changes. Plan: `docs/TOTP_PASSWORD_RESET_PLAN.md`.

Paths are relative to `firmware/KilnFW/App/` unless noted.

Result: **HIGH 0, MED 1, LOW 6, INFO 8.**

## Scope

- `drivers/net/totp.c/.h`: SHA-1, HMAC, HOTP/TOTP, base32, otpauth URI.
- `drivers/persist/totp_config.c/.h`: secret and replay counter in NVS `kiln_auth`.
- `drivers/http/totp_http_core.c/.h`: pending enrollment, reset tokens, board cap, wipe helper.
- `drivers/http/auth_totp_http.c`: `POST /api/auth/forgot`, `POST /api/auth/reset`,
  `GET /api/auth/totp_status`, and the per-IP backoff.
- `drivers/http/security_http.c`: the `totp_enroll_begin`, `totp_enroll_confirm` and
  `totp_disable` commands.
- `drivers/http/security_backend_web_auth.c`: `clear_credentials`.
- `drivers/net/auth_reset_gesture_wiring.c`: the LCD four-corner reset.
- Route tiers: `drivers/http/route_tier_table.h`.
- Tests: `test/test_totp*.c`, `test/test_auth_totp_http_fuzz.c`,
  `test/test_totp_enroll_web_auth_off.js`, and the PcTools `totp` bench suite.

## Confirmed correct

- **RFC 6238 parameters.** SHA-1, 6 digits, 30 s step, window of ±1 step
  (`drivers/net/totp.h:49-52`). The counter is `t / 30` (`totp.h:95-97`).
- **Counter encoding and truncation.** The counter is encoded as an 8-byte
  big-endian value. Dynamic truncation uses `offset = hs[19] & 0x0F` and masks
  `0x7F` (`drivers/net/totp.c:211-212`). The RFC 6238 Appendix B vectors pass
  (`test/test_totp.c:136`).
- **Window order and replay check.** Candidates are tried in the order 0, -1, +1
  (`totp.c:264`). A candidate `<= last_accepted` is skipped before any compare
  (`totp.c:272`). The compare is constant-time over a fixed 6-character buffer
  (`totp.c:221`, `totp.c:287`).
- **Replay counter in NVS.** The counter is persisted before OK is returned
  (`drivers/persist/totp_config.c:271-300`). It survives a simulated reboot
  (`test/test_totp_config_persist.c:338-340`). A counter above `UINT32_MAX`
  fails closed (`totp_config.c:293`).
- **Loads fail closed.** Both secret and counter loads are tri-state: OK, ABSENT
  or UNREADABLE. A corrupt or unreadable entry refuses
  (`test_totp_config_persist.c:147-316`).
- **SNTP gate.** The gate runs first in every route (`auth_totp_http.c:323-337`,
  `security_http.c:257-349`). The `ever_synced` flag is RAM-only and is set only
  by the SNTP callback (`drivers/net/time_sync.c:125`, `:246`). An unsynced
  clock therefore refuses with 503 instead of computing codes for 1970.
- **Secret never leaves the device after enrollment.** `totp_status` returns
  only `enrolled`, `board_time_utc` and `sntp_synced` (`auth_totp_http.c:307-317`).
  Logs carry only the client IP. `/api/nvs/keys` refuses `kiln_auth`
  (`drivers/http/diagnostics_http.c:2027`). Backup export never reads it, and
  every factory-reset scope preserves it (`drivers/http/factory_reset.c:130`, `:327`).
- **Anti-oracle on `/forgot`.**
  - It always answers 202 with a token-shaped value.
  - It runs dummy HMAC work when no real HMAC ran.
  - It stores the token only when the code verified (`auth_totp_http.c:410-430`).
- **`/reset` re-checks binding when the token is used.** Before setting the
  password it requires:
  - TOTP still enrolled;
  - an administrator record still configured;
  - the submitted username exactly matching that record (`auth_totp_http.c:525-543`).

  The username compare is case-sensitive `strcmp` (`drivers/net/web_auth_login.c:17`),
  so a reset cannot silently rename the administrator.
- **Route tiers.** `/forgot` and `/reset` are OPEN (`route_tier_table.h:147-157`).
  `totp_status` and `/api/auth/security` are ADMIN (`route_tier_table.h:420`).
- **Owner decisions are implemented.**
  - `clear_credentials` calls `totp_wipe_disenroll()`, which clears the secret,
    the reset tokens and the pending enrollment
    (`drivers/http/security_backend_web_auth.c:364-404`).
  - Enrollment answers 409 `web_auth_disabled` while web auth is off
    (`security_http.c:412-434`).
- **Internal RAM.** Cost is small:
  - The reset-token table is about 290 B and the remote slot is in `.bss`.
  - The pending secret is about 32 B.
  - The 16-slot lockout table is in `EXT_RAM` (`auth_totp_http.c:196`).
  - The `/reset` body is a 512 B heap buffer, PSRAM first.
  - Handler stack use is under 1 KB, plus 320 B for the SHA-1 `w[80]`, against
    the 8192 B httpd stack (`drivers/http/wifi_provision_http.c:1270`).

## MED

### MED-1: the LCD reset gesture leaves admin web sessions alive, and a surviving session can enroll a new TOTP secret

- **Where:** `drivers/net/auth_reset_gesture_wiring.c:37-42`, with
  `drivers/persist/web_auth_store.c:738-830` and `security_http.c:257-349`.
- **What the gesture does:** `auth_reset_gesture_clear_all()` calls only
  `web_auth_store_clear_for_physical_reset()` and `totp_config_clear()`.
- **What it does not do:**
  - It does not call `web_auth_table_destroy_all()`.
  - It does not clear the reset-token table or `s_totp_pending`.

  The web-side wipe does all of these: `clear_credentials` goes through
  `totp_wipe_disenroll()` and invalidates sessions
  (`security_backend_web_auth.c:364-404`). `http_auth_http.c:194-210` resolves the
  role from the session table alone, so an admin session minted before the
  gesture is still admin afterwards. The web auth policy stays enabled, so
  enrollment is still allowed.
- **Scenario:** an attacker holds a stolen or leftover admin session cookie. The
  owner notices and uses the physical gesture to take the board back. The
  attacker's session survives.
  1. It calls `totp_enroll_begin`, or confirms an enrollment it already started
     within the 5 min TTL (`totp_http_core.h:82`). `enroll_begin` only requires
     the secret to be ABSENT, which the gesture just made true.
  2. Once the owner bootstraps a new password, the attacker's authenticator
     drives `/forgot` and `/reset` and takes the admin password again.

  The same surviving session can also call other ADMIN routes directly.
  Outstanding reset tokens are harmless, because `/reset` re-checks enrollment
  and the admin record (`auth_totp_http.c:536-543`).
- **Consequence:** the gesture is the one recovery path that should evict
  everyone. It does not end existing sessions, and it lets a holdover session
  install a persistent password-reset backdoor.
- **Fix:**
  - Make the gesture run the same wipe as `clear_credentials`:
    `totp_wipe_disenroll(totp_config_clear, auth_totp_http_clear_reset_tokens,
    security_totp_pending_clear)` plus `web_auth_table_destroy_all(http_session_table())`.
  - Marshal those calls onto the httpd task, or take their locks, because the
    gesture runs on the LVGL task (see LOW-1).
  - Add a host test that the wiring clears sessions, tokens and the pending
    secret.

## LOW

### LOW-1: no lock on `totp_config`'s RAM counter cache, and the LCD gesture clears it from another task

- **Where:**
  - `drivers/persist/totp_config.h:24-28`: the CONCURRENCY note says "single caller".
  - `totp_config.c:216-265`: `s_ram_loaded` and `s_ram_last_counter`.
  - Callers: `auth_reset_gesture_wiring.c:39` runs on the LVGL task
    (`drivers/ui/ui_page_home_actions.c:539-578`). `verify_and_consume` runs on
    httpd.
- **Scenario:** an httpd `/forgot` is inside `verify_and_consume` while the
  gesture's `totp_config_clear()` erases both keys and resets the cache. The
  consume can then write the counter key back after the erase.
- **Consequence:** benign. The result is a counter with no secret. Load returns
  ABSENT, and a later re-enroll resets the counter (`totp_config.c:158-188`).
  Every interleaving fails closed. The bug class is real, though: the header
  contract is broken by a second task.
- **Fix:** add a mutex in `totp_config`, or route the gesture's clear through
  the httpd task (same fix as MED-1).

### LOW-2: enroll-confirm persists in two NVS writes, so a power loss in between leaves counter 0

- **Where:** `security_http.c:335-336`. `totp_config_set_secret()` writes the
  secret and resets the counter to 0 (`totp_config.c:158-188`), and
  `totp_config_set_last_counter(matched)` writes it again.
- **Scenario:** reset or power loss between the two writes.
- **Consequence:** the secret is stored with counter 0, so the code used to
  confirm enrollment can be replayed on `/forgot` until its window closes
  (≤ about 60 s). Exploiting this needs the observed code and a well-timed
  reboot. Impact is negligible, and the outcome is never worse than "enrolled".
- **Fix:** write the counter to `matched` before the secret, or store the
  counter inside the secret blob as a single write.

### LOW-3: `/forgot` spends the TOTP step before the token exists, and `/reset` spends the token before the password write

- **Where:**
  - `auth_totp_http.c:384-426`: `verify_and_consume` persists the counter, then
    the token is stored in RAM.
  - `auth_totp_http.c:525-547`: `totp_reset_token_consume` runs before
    `set_web_password`.
- **Scenario:** a reboot between `/forgot` and `/reset` loses the RAM-only token.
  A failed NVS write in `set_web_password` leaves the token spent.
- **Consequence:** the owner must wait for the next 30 s step and repeat
  `/forgot`. There is no split state: the password and TOTP state never diverge,
  because a failed write changes nothing. The cost is availability only.
- **Fix:** none needed for safety. Optionally restore the token slot when
  `set_web_password` fails, and document the retry in the reset page text.

### LOW-4: lockout state is RAM-only, so a reboot resets the per-IP ladder and the board cap

- **Where:**
  - `auth_totp_http.c:149-197`: ladder of 5/10/30/60/300 s, 16 slots in
    `EXT_RAM`, one pooled remote slot.
  - `totp_http_core.h:159-174`: board cap of 20 failures, RAM only.
  - Plan section 4 records the board cap as RAM-only by design.
- **Scenario:** an attacker who can power-cycle the board, or crash it with an
  unrelated bug, gets a fresh 20-attempt budget each boot.
- **Consequence:** the brute-force ceiling is 20 attempts per boot. Each attempt
  covers up to 3 codes, against a code space of 10^6. Reaching a 50% success
  chance needs on the order of 10^4 reboots. Acceptable, but it differs from what
  the task asks ("lockout survives a reboot").
- **Fix:** optionally persist the board-cap count in NVS `kiln_auth`, writing
  only on failure. Otherwise state the per-boot budget explicitly in the plan's
  threat model.

### LOW-5: an unauthenticated caller can lock the owner out of `/forgot` until reboot

- **Where:**
  - `auth_totp_http.c:221-222`: `totp_lockout_slot_for()` returns NULL when all
    16 slots are locked, and the gate then refuses every new IP
    (`auth_totp_http.c:258-287`).
  - `auth_totp_http.c:337`: the board cap blocks everyone after 20 failures.
- **Scenario:** 20 wrong codes from any IP mix, about 2 minutes of work under
  the ladder, block `/forgot` board-wide. 16 locked IPs then deny any new client
  a slot.
- **Consequence:** password recovery is denied until reboot. A reboot restores
  it, which the owner can do physically. This is a deliberate trade-off (plan
  section 4), but nothing reports it in `totp_status` or `/api/readiness`.
- **Fix:** report `forgot_blocked` in `GET /api/auth/totp_status` and on the
  reset page, so the owner knows a reboot is needed.

### LOW-6: a weak new password on `/reset` counts as a backoff failure

- **Where:** `auth_totp_http.c:512-523`, at `totp_backoff_record(ip, ip_known, false)`
  on line 516.
- **Scenario:** the owner types two weak passwords in a row on the reset page.
- **Consequence:** a 5 s and then a 10 s 429 lockout. The 2-minute token TTL
  (`totp_http_core.h:115`) can lapse while the owner waits, which forces a new
  `/forgot` and costs another step. This is a usability cost only, and the fuzz
  test pins the behavior on purpose (`test/test_auth_totp_http_fuzz.c:451`).
- **Fix:** do not record a failure for a weak-password refusal. The check runs
  before the token lookup, so it reveals nothing.

## INFO

- **INFO-1: secrets travel in plaintext.** The enrollment secret and otpauth URI
  are returned over plain HTTP (`security_http.c:257-305`), and the `/forgot`
  token does the same. Anyone sniffing the LAN during enrollment gets the secret.
  This is inherent to a board without TLS; document it next to enrollment.
- **INFO-2: NVS is not encrypted.** `sdkconfig.defaults` sets no
  `NVS_ENCRYPTION` or `FLASH_ENCRYPTION`, so the TOTP secret sits in the clear in
  `kiln_auth`. Anyone with flash or JTAG access can read it, but that access
  already owns the board.
- **INFO-3: SNTP is unauthenticated.** It talks to `pool.ntp.org`
  (`time_sync.c`).
  - A rolled-back clock cannot replay old codes, because of the persisted counter.
  - A forward-spoofed clock lets a code be accepted at a high counter. Every
    honest code then refuses until real time passes that counter, which is
    self-inflicted denial of service.
  - Fix option: refuse a candidate counter more than a day ahead of the
    persisted one, or surface the drift in `totp_status`.
- **INFO-4: the otpauth URI does not percent-encode the username.**
  `totp_build_otpauth_uri()` inserts it verbatim (`totp.c:363-371`). A username
  with `?`, `&`, `#` or a space would produce a malformed URI. Whether that is
  reachable depends on the username charset rule in `web_auth_store`.
- **INFO-5: accepting step +1 moves the counter past the current step.** After a
  code is accepted at counter+1, the current step's code is refused (`totp.c:272`).
  This is correct replay semantics, noted for the bench runbook.
- **INFO-6: token consume has a small timing difference.** It returns at the
  first matching slot and compares the username with `strcmp`
  (`totp_http_core.c`, consume loop). The leak is negligible: 4 slots, and the
  token compare is constant-time.
- **INFO-7: the lockout ladder is not shared with login.** `cases_totp.py:57-63`
  says `/forgot` and `/reset` share `login_ip_scope.c`'s ladder. In the code,
  `auth_totp_http.c` shares only the local/remote classification
  (`login_ip_scope_classify()`, `:247`) and keeps its own ladder and table
  (`:149-197`). Bad TOTP attempts therefore never lock out `/api/auth/login`,
  and bad logins never lock out `/forgot`. The docstring is imprecise. The
  device behavior is fine.
- **INFO-8: constant-time compare has a functional test only.** It is tested for
  correctness (`test/test_totp.c:263`), not for timing. The OR-accumulate form
  (`totp.c:221`) is constant-time by construction.

## Test coverage

**Covered:**

| Topic | Test |
|---|---|
| SHA-1 and HMAC vectors | `test/test_totp.c:19`, `:77` |
| RFC 6238 Appendix B | `test/test_totp.c:136` |
| Window and replay | `test/test_totp.c:162`, `:197` |
| Malformed codes | `test/test_totp.c:243` |
| Base32 and URI | `test/test_totp.c:278`, `:333` |
| Persistence: round trip, re-enroll resets counter, clear, tri-state fail-closed, consume with failed write | `test/test_totp_config_persist.c:24-389` |
| Replay across a simulated reboot | `test/test_totp_config_persist.c:338-340` |
| Pending TTL, reset-token single use, expiry, capacity, wrong username | `test/test_totp_http_core.c:37-295` |
| Pure board cap | `test/test_totp_http_core.c:296` |
| Wipe disenroll | `test/test_totp_http_core.c:325` |
| `enroll_allowed` | `test/test_totp_http_core.c:28` |
| Handler body fuzz, weak password, valid reset drops sessions | `test/test_auth_totp_http_fuzz.c:265`, `:325` |
| Page message for 409 `web_auth_disabled` | `test/test_totp_enroll_web_auth_off.js` |

**Gaps:**

1. The `security_http.c` commands `totp_enroll_begin`, `totp_enroll_confirm` and
   `totp_disable` (`security_http.c:257-384`) have no C host test. Untested:
   - the ABSENT precondition;
   - persist-failure rollback;
   - clearing reset tokens on confirm;
   - the 409 path through the real dispatcher (only the pure predicate is tested).
2. The handler-level SNTP-unsynced 503 is untested. The fuzz stub is always
   synced (`test/test_auth_totp_http_fuzz.c:153-158`), so only the pure
   `totp_clock_ready` is covered.
3. The board cap through the `/forgot` handler (`auth_totp_http.c:337`) is
   untested: 20 failures then 429, and a success not resetting the cap.
4. Per-IP slot exhaustion (`auth_totp_http.c:221-222`) and the pooled remote
   slot (`:232-249`) are untested.
5. The LCD gesture wiring (`auth_reset_gesture_wiring.c:37-42`) is untested.
   `test/test_auth_reset_gesture.c` uses a fake seam, so nothing checks what the
   real wiring clears. This is the MED-1 regression test.
6. Power loss between the enroll-confirm writes (LOW-2) is untested. `fake_kv`
   can script a write failure.
7. No test asserts that the `GET /api/auth/totp_status` body never contains the
   secret.
8. Concurrent gesture clear against httpd consume (LOW-1) is untested.
9. No test pins the lockout as RAM-only across reboot, so the behavior in LOW-4
   is undocumented in tests.

## PcTools `totp` bench suite

- **Cases** (`tools/PcTools/src/kilnctrl/bench_test/cases_totp.py:161-307`):
  - TP-R01: GET status.
  - TP-R02: wrong code returns 202.
  - TP-R03: OPEN tier, never completes a reset.
  - TP-M01: full round trip. It SKIPs without `KILNCTL_TOTP_CODE` and
    `KILNCTL_WEB_PASSWORD_NEW`.
- **Why the 2026-10-13 run had NOT_RUN:**
  - Run `logs/bench_test/20261013T181206Z_totp/summary.json` marked all four
    cases NOT_RUN. The run-level preflight failed on a latched S6b trip:
    `trip_reason=7`, `diag_trip_mask 0x0040`, and the readiness gate blocked on
    `safety_trip`.
  - The preflight refuses every suite on a latched trip, even though all four
    TOTP cases are HTTP auth checks that never heat. That is a PcTools
    preflight scope issue, not a device bug.
  - The re-run 59 s later (`20261013T181305Z_totp`) found the trip cleared and
    got PASS on TP-R01/R02/R03 and SKIP on TP-M01 (no env vars).
- **Further limits seen in that run:**
  - TP-R01 observed `enrolled=false`, so TP-M01 could not pass even with the env
    vars set. No bench case enrolls, which follows the owner decision that
    enrollment is settings-page only. The round trip has never run on hardware,
    which matches plan section 7's pending live-board steps.
  - TP-R03 passed on `forgot_status=429, reset_status=429`. TP-R02's failure
    one request earlier put the bench IP on the 5 s ladder step. A 429 still
    proves the route was reached without a session, but the 400 paths were
    never exercised. A 6 s gap before TP-R03 would make the case meaningful.
