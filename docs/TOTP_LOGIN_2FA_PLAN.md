# TOTP as optional login second factor

Follow-on to `docs/TOTP_PASSWORD_RESET_PLAN.md`, which is reset-only by
owner decision 2026-09-24 (see that plan's section 5). This plan designs
using the same enrolled TOTP secret as an **optional** second factor at
ordinary web login, reusing the reset plan's enrollment/verification
machinery (sections 1-3 and 6b there) with no new secret and no new NVS
key beyond one policy flag. Not implemented yet. Pending work only —
no narrative history beyond what is needed to justify a decision.

## 1. Scope decision: optional, per-installation, off by default

2FA-at-login is a policy toggle, not a hardwired behavior:

- **Off by default.** Enrolling TOTP (for password reset) must never
  silently start requiring it at login — that would turn today's reset
  plan into a login gate by surprise.
- **Opt-in, ADMIN-only toggle**, settings page, only offered once TOTP is
  enrolled (`totp_status` reads `enrolled: true`). Turning it on with no
  enrolled secret must be refused — otherwise the operator can lock
  themselves out of every login with no code to give.
- **Turning it off** never requires a code (same asymmetry as any
  "require less" policy change — the risk of a stuck-off admin is real,
  the risk of a stuck-on one who chose to turn it off is not). Turning it
  **on** does require a currently-valid code, same principle as
  `totp_disable` in the reset plan's section 6b: prove you hold the
  factor before making it load-bearing.
- Applies to the `administrator` role only, same as enrollment itself
  (`WEB_AUTH_ROLE_ADMINISTRATOR`). The `user` role has no TOTP secret and
  is out of scope (see section 8).

## 2. Policy flag: NVS key and persistence

New NVS key in the `kiln_auth` namespace: **`totp_login_req`** (13
chars, under the 15-char limit), a single byte/bool, alongside the
existing `totp_secret`/`totp_last_ctr` keys from the reset plan. Read
once at boot into the same RAM cache `totp_config.c` already keeps for
the secret's tri-state load status (absent/OK/unreadable) — a login-path
read must not touch NVS on every request. Write path funnels through the
same read-back-verified clear/write helper class the reset plan's
`totp_config_verify_and_consume()` uses, not a fire-and-forget NVS
write (this repo has three prior board-bricking postmortems on exactly
that shortcut — see CLAUDE.md's boot_guard history).

**Interaction with disenrollment (section 6 of the reset plan) and the
LCD gesture (section 6b):** clearing `totp_secret` must also clear
`totp_login_req` in the same operation, never leave it orphaned true
against an absent secret. This is another instance of this repo's
"reset one side of a pair" bug class: the flag and the secret are two
pieces of state joined by an implicit contract ("2FA is only meaningful
while a secret is enrolled"), and a disenroll or LCD-gesture reset that
clears the secret but not the flag would leave the next login gated on
a factor that can never be satisfied. Both `totp_disable` and the LCD
gesture's additive clear call (already planned to touch `totp_secret`/
`totp_last_ctr`) must clear `totp_login_req` too, in the same read-back-
verified pass, not a follow-up write.

## 3. Wire shape: two round trips, not one

**Recommendation: password then code, as a second round trip — do not
fold the code into the existing `/api/auth/login` POST body.**

Reasoning:

- **The login ladder keys off failure count per address/scope, not per
  factor.** `web_auth_login_http.c`'s `LOGIN_BACKOFF_LADDER_MS` and the
  LOCAL/REMOTE-pooled scheme in `login_ip_scope.c` treat "login POST
  failed" as one signal. If a single POST carried both password and
  code, a wrong code on an otherwise-correct password would consume a
  ladder step identically to a wrong password — correct in spirit (both
  are a failed login attempt) but it collapses two different failure
  causes into one Retry-After, making it impossible for the client UI to
  say "your password was fine, the code wasn't" without probing
  separately, which itself would be an oracle. Two routes let the second
  step fail (and be rate-limited) independently, the same shape the
  reset plan already chose in its own section 4 for exactly this reason.
- **A wrong password must not reveal whether 2FA is required.** If code
  and password travel together, a client can distinguish "wrong
  password" from "right password, wrong/missing code" only by the
  server's response shape — which is itself a partial username/password
  oracle (confirms the password was right before the code is checked).
  A second round trip avoids this: the first POST succeeds or fails
  exactly as it does today; only on success does the server ever
  indicate a second factor is needed.
- **Reuses the existing session-cookie plumbing instead of a new
  transient state machine.** See section 4 for the exact mechanism.

**Rejected alternative:** single POST with both fields. Would save one
round trip but forces either an oracle (as above) or folding the code
check into the same ladder slot as the password check, which conflates
two distinct failure causes the ladder's Retry-After is supposed to
describe accurately. Not worth it for a bench appliance with one
administrator.

## 4. Mechanism: a pending, unprivileged session state

`POST /api/auth/login` (existing route, `ROUTE_TIER_OPEN`, unchanged
wire shape) on a correct password:

- If `totp_login_req` is false (today's behavior, and every non-admin
  login): issue the normal session cookie exactly as today. No new
  round trip for anyone who has not opted into 2FA.
- If `totp_login_req` is true for that role: issue a session that is
  marked **pending-2FA** (a bit alongside the existing session record in
  `http_session_iface.h`'s table — no new table, no new cookie name).
  A pending-2FA session authenticates for exactly one route,
  `POST /api/auth/login/totp` (new, `ROUTE_TIER_OPEN` — it must be
  reachable with only the pending cookie, no password, since the whole
  point is presenting the second factor), and is rejected by every
  other `ROUTE_TIER_USER`/`ROUTE_TIER_ADMIN` handler exactly as an
  unauthenticated request would be. It expires quickly (recommend 2
  minutes, same window as the reset plan's reset-token lifetime, for
  consistency) — an abandoned pending session must not linger as
  attack surface.
- `POST /api/auth/login/totp` body: `{"code"}`. Verifies against
  `totp_config_verify_and_consume()` (the same call the reset plan's
  `/api/auth/forgot` uses — the ±1-step window and `totp_last_ctr` replay
  guard are **shared state** with the reset flow by design, see section
  5). On success, promotes the pending session to a full session
  in-place (same cookie, same session-table row — no re-issue, no second
  cookie) and returns 200 with the same body shape `/api/auth/login`
  returns today. On failure, the pending session is consumed
  (single-shot: a wrong code does not leave the pending session alive
  for unlimited guesses) and the client must start over at
  `/api/auth/login` — repeats section 1's ladder protection at the
  entry point rather than adding a second independent guess budget at
  this route.
- **Rate limiting:** `POST /api/auth/login/totp` goes through the same
  `login_ip_scope.c` LOCAL/REMOTE classification and existing ladder as
  `/api/auth/login` itself — not a separate counter. A wrong code and a
  wrong password both cost the same ladder step at the same address/
  scope, which is the intended effect of treating "presenting an
  invalid credential of either kind" as one signal for backoff purposes,
  while the actual factor-correctness distinction (section 3) stays
  purely a client-visible response-shape question, never a differently-
  throttled one.

**Net new routes: 1** (`/api/auth/login/totp`). Against the reset plan's
already-landed 162/170 (see that plan's section 4 and WT-A note), this
lands at 163/170, 7 spare — re-verify with `check_uri_handler_cap.ps1`
in the same commit, per this repo's standing rule that the count drifts.

## 5. Replay-guard sharing with the reset flow

`totp_last_ctr` (already NVS-persisted by the reset plan, WT-A) is one
counter per enrolled secret, not one per call site. `/api/auth/login/totp`
and `/api/auth/forgot` both call `totp_config_verify_and_consume()`
against the same secret and the same counter — this is deliberate, not
an oversight: a code accepted by one flow must not be replayable against
the other. No new persisted state is needed for this; it falls out of
reusing the existing function rather than duplicating it. Confirm this
sharing behavior with a host test (section 9) rather than relying on
code review alone, since a future refactor that gives login its own
counter "for isolation" would silently reopen a replay window across the
two flows.

## 6. Lazy-login modal changes

The modal built in worktree `lazylogin` (landed `090aaa9f`/`58e10e11`,
per ROADMAP.md) gains one conditional second step:

- Submit password as today. If the response indicates a pending-2FA
  session (recommend a `{"totp_required": true}` field on 200, distinct
  from a normal 200 login success body — the login POST itself always
  returns 200 on a correct password whether or not 2FA follows, so a
  wrong password still only ever gets the existing failure response;
  no oracle is added), swap the modal's password field for a 6-digit
  code field, same modal chrome (no new overlay, no new focus-trap
  logic — reuse `loginActiveCtl`'s existing suspend/resume shape the
  forgot-password link already uses per `app.js`).
- A wrong code shows an inline error and returns the modal to the
  password step (per section 4, the pending session is consumed on a
  wrong code, so resubmitting a code without a fresh password is not an
  option — the UI must not offer a "retry code" affordance that implies
  otherwise).
- The existing "Forgot password?" link stays reachable from the
  password step only; if the operator has forgotten *the code* rather
  than the password (phone lost, app not installed) the answer is
  section 7's recovery path, not the password-reset flow — the modal
  should not suggest the reset flow fixes a lost authenticator, since it
  doesn't need to (a reset with 2FA-required still asks for a code
  unless section 7's recovery has run first, see the interaction note
  below).
- **Interaction with password reset while 2FA-at-login is on:** completing
  `/api/auth/forgot` + `/api/auth/reset` changes the password but does
  **not** clear `totp_login_req` or `totp_secret` — a reset is not a
  disenroll. The very next login after a successful reset still asks for
  the code. This is intentional (resetting a forgotten password is not
  evidence the phone is also lost) but must be stated explicitly in the
  UI copy after a successful reset ("password changed; your authenticator
  app is still required to sign in") so the operator is not surprised by
  a second prompt — this is the "reset must not lock the operator out of
  2FA" requirement from the task brief, satisfied by *not* touching the
  flag/secret on reset, not by any new code.

## 7. Recovery when the phone is lost, with 2FA-at-login on

The LCD four-corner gesture is the only recovery path, same as the
reset plan's section 6, and needs no new mechanism — the reset plan
already commits to clearing `totp_secret`/`totp_last_ctr` on a
successful gesture confirm (`auth_reset_gesture_wiring.c`'s additive
call). This plan adds exactly one more field to that same clear:
`totp_login_req` (section 2). Once cleared, the very next login is a
plain password login with no second factor — physical presence at the
board is what restores access, exactly as it already is for a lost
password. No separate "recovery code" list, backup-code scheme, or
secondary contact method is proposed: this is a single-administrator
bench appliance with an existing physical-access recovery path, and
adding a parallel one (e.g. printable backup codes) would be new state
to persist, back up, and reason about for a threat this gesture already
covers.

## 8. Explicit non-goals

- **No 2FA for the `user` role.** `user` has no TOTP secret in the reset
  plan; extending enrollment to a second role is out of scope here and
  would need its own NVS keys and its own settings-page UI.
- **No "remember this device" / trusted-device cookie.** Every login
  with the policy on asks for a code; there is no reduced-friction path
  for a recognized browser. Revisit only if the owner reports the
  friction is unacceptable in practice.
- **No backup/recovery codes.** See section 7.
- **No change to the TOTP parameters, secret generation, QR rendering,
  or enrollment UI** — all of that is the reset plan's sections 1-2 and
  already landed (WT-B).
- **No change to `login_ip_scope.c`'s classification or ladder timings.**
  This plan reuses the existing ladder as-is (section 4).
- **No PC-side / MCP change beyond what already exists.** `http_auth.py`
  and `totp_http_client.py` (reset plan WT-C) already read a code from
  `KILNCTL_TOTP_CODE` for the reset flow; see section 10 for the one
  small addition needed for login.

## 9. Host tests

All of this is HTTP-handler-adjacent, and per this repo's
`project_http_handlers_are_target_build_only` finding, the route
handlers themselves are not host-testable — only the pure logic
underneath is. Host-testable pieces:

- `totp_login_req` NVS read/write/clear round trip, including the
  write-only/read-back-verified discipline (mirrors
  `test_totp_config_persist.c`'s existing pattern for `totp_secret`).
- The pending-2FA session promotion state machine, if it can be
  extracted as pure logic independent of `httpd_req_t` (recommend: a
  small pure function taking "session role, pending flag, code-check
  result" and returning "promote / reject-and-consume / expired", tested
  directly rather than through the HTTP layer).
- The shared-counter replay test from section 5: verify a code accepted
  through the login-2FA path is rejected if replayed through the reset
  path's `verify_and_consume`, and vice versa.
- Negative test (per this repo's standing rule): flip the pending-session
  single-shot consumption so a wrong code does *not* consume the pending
  session, confirm the host test that checks for exactly that fails,
  restore by hand, force a full rebuild, confirm green again.

## 10. PC-side / MCP tooling

`tools/PcTools/src/kilnctrl/http_auth.py`'s `_login()` is the one seam
every MCP tool's login goes through (see that module's own docstring).
It must learn the two-step shape:

- On a 200 from `/api/auth/login` that carries `totp_required: true`,
  read a code from environment variable **`KILNCTL_TOTP_CODE`** (already
  the reset plan's WT-C convention — reuse the exact same variable name,
  never a new one, and never a call parameter) and POST it to
  `/api/auth/login/totp` before treating the login as complete. If the
  variable is unset or empty, raise `HttpAuthError` naming the variable,
  the same actionable-error discipline `credentials()` already uses for
  `KILNCTL_WEB_USERNAME`/`KILNCTL_WEB_PASSWORD`.
- Every MCP tool that logs in today goes through this one function and
  needs no individual change — this is the entire point of the seam
  existing (see `http_auth.py`'s own docstring, "the redaction blinded
  the tooling" failure class this module was built to avoid repeating).
- Never print, log, or echo the code; never accept it as a tool
  parameter (matches `totp_reset_password`'s existing rule in the reset
  plan's WT-C acceptance criteria — same reasoning applies here
  unchanged).
- `run_pctools_tests` gets a unit test for the two-step `_login()` path
  against a fake server, mirroring the existing tests for the one-step
  path and for `totp_http_client.py`.

## 11. Bench cases

Cannot be exercised meaningfully without WT-A of the reset plan first
(no routes to log into with a code exist yet). Once available:

- Policy off (default): login unchanged, single round trip, no code
  prompt — regression case, must never regress once this lands.
- Policy on, correct password + correct code: full login succeeds.
- Policy on, correct password + wrong code: login refused, pending
  session consumed, retry requires fresh password (per section 4).
- Policy on, correct password + code replayed from a prior reset-flow
  use: refused (section 5's shared-counter guarantee, on real hardware
  this time rather than only host-tested).
- LCD gesture recovery: enroll, turn policy on, run the gesture, confirm
  the very next login needs no code (section 7).
- `check_uri_handler_cap.ps1` reconfirmed at whatever count is current
  when WT-A actually lands, plus this plan's one new route.

## 12. Work breakdown (not yet tranched)

This plan is not split into work tranches the way the reset plan is,
on top of that plan's WT-A (landed: the routes and
`totp_config_verify_and_consume()` this plan calls exist).
When picked up, expect roughly: one firmware tranche (NVS key, pending-
session state, new route, settings-page toggle wired through the
existing `/api/auth/security` `cmd=` dispatch rather than a second new
route — recommend `cmd=set_totp_login_required` alongside the existing
`set_policy`/`set_web_password` commands, since section 4's only
*necessary* new route is the login-code POST itself), one web-UI
tranche (modal second step), one PcTools tranche (`http_auth.py`), and
one host-test tranche — matching the reset plan's WT-A/B/C/D shape if
it is picked up the same way.
