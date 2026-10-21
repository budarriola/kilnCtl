# Review: RF2-01..05 fix (6e706465c, d3a5c2807), 2026-10-20

Scope: `web_auth_store.c` set_password / load_policy / wipes, bootstrap gate
(`security_backend_web_auth.c`, `web_auth_store_admin_configured_or_unreadable`,
`web_auth_store_storage_reset_bootstrap_allowed`). Static review. Targeted host tests
(`web_auth_store|web_auth_login_http`): 16/16 built and passed.

Result: HIGH 0, MED 0, LOW 3, INFO 3. No state opens a provisioned board. No state locks out
permanently: every UNREADABLE state below recovers through gesture + bootstrap.

## State table

Key: P = policy key (OK / absent / corrupt), M = ever-enabled marker, W = web blob
(none / creds / wiped = admin unconfigured + must_change / corrupt), L = FS-02 latch.

| # | State | load_policy | Bootstrap | Outcome |
|---|-------|-------------|-----------|---------|
| 1 | Fresh: P absent, M absent, W none | ABSENT (open) | closed (auth off) | Wizard order: set_password writes both-off P first, then wizard set_policy works. Security page order: same. OK |
| 2 | Fresh, USER set first | ABSENT then both-off | n/a | Pre-write runs on first credential of either role. OK |
| 3 | Pre-write set_policy fails | unchanged | n/a | set_password returns the error, no credential written. OK (tested) |
| 4 | P OK enabled, M set, admin set | OK | closed | Normal. OK |
| 5 | P OK, gesture | OK | open (wiped) | Bootstrap; P OK so no extra write. OK |
| 6 | P lost, M kept, admin set (RF2-01) | UNREADABLE | closed | Dashboards only, banner "use LCD password reset". Gesture -> wiped -> bootstrap writes enabled P. Recovers. OK |
| 7 | P+M lost, admin set | UNREADABLE | closed | Same recovery as 6. OK |
| 8 | P+M lost, USER only (RV-03) | UNREADABLE | open to any caller | Bootstrap gives admin but no P write (wiped_state false, W exists, no pre-write): stays UNREADABLE, never opens. RF3-02 |
| 9 | 8 then gesture | UNREADABLE | open | Bootstrap writes enabled P. OK (tested, RF2-02) |
| 10 | Never provisioned, gesture (RF2-04) | UNREADABLE (W wiped) | open | Bootstrap writes enabled P (web+LCD on). RF3-04 |
| 11 | W corrupt, P+M lost | UNREADABLE | closed (admin unreadable) | Gesture rewrites W clean -> 9. OK |
| 12 | W key lost, M set | P state | closed (FX-02) | Gesture writes wiped W -> 5/6. OK |
| 13 | L latched (FS-02) | UNREADABLE | only after gesture (RT-01) | resolve writes enabled P + M; RF2-01 branch skipped while latched. OK |
| 14 | clear_all_credentials (admin route) | P unchanged | open if P enabled | Same as 5/6; banner refreshed. OK |
| 15 | P present corrupt | UNREADABLE | per admin | Gesture + bootstrap: load_policy UNREADABLE -> enabled P rewritten. OK |
| 16 | RF2-01 enabled-P write fails | UNREADABLE, admin set | closed | Bootstrap still reports ok. Gesture again recovers. RF3-01 |
| 17 | P+M lost, W none, LCD PIN blob present | ABSENT (open) | closed | RF3-03 |
| 18 | Legacy (no auth_mig), creds, P+M absent | migrated both-off | n/a | Known RF2-08. Unreadable auth_mig: skipped, stays UNREADABLE (tested) |

Banner (s_policy_lost) now refreshed after set_password and both wipes: OK.

## Findings

- **RF3-01 LOW** web_auth_store.c:714-721: the RF2-01 enabled-policy write is `(void)`. If it
  fails, bootstrap returns `{"ok":true}`, the admin is configured, policy stays UNREADABLE and
  the bootstrap gate is now closed. Not permanent (gesture again re-enters the wiped state), but
  silent. Fix: return the error (or retry once) and log it.
- **RF3-02 LOW** state 8: with a USER-only board that lost policy+marker, any LAN caller can
  still bootstrap an admin. It no longer opens auth (RF2-02 holds), and sessions stay dead, but it
  plants a credential and forces the owner into the gesture. Nuisance lockout, recoverable.
  Fix: when load_policy is UNREADABLE, allow bootstrap only from the wiped state (as RT-01 does
  for the latch).
- **RF3-03 LOW** web_auth_store.c:1092-1095: "provisioned" checks only the web blob. A board
  with LCD PINs but no web blob that loses policy+marker reads ABSENT (both off). Needs a double
  key loss; pre-AU-02 behaviour. Fix: count a present LCD blob as provisioned.
- **RF3-04 INFO** state 10 (the asked behaviour change): a gesture on a never-provisioned store
  now reads UNREADABLE (dashboards only) until the admin bootstrap, which writes web+LCD auth on
  with the LCD locked pending an admin PIN ("set LCD admin PIN or disable LCD lock"). Judged safe
  and correct: it fails closed, bootstrap is open, and an admin can turn auth off again. Cost: an
  owner who ran auth-off and used the gesture ends with auth on. Document in the user guide.
- **RF3-05 INFO** post-gesture bootstrap is first-LAN-client (unchanged design); it now also
  enables the policy, so the first client becomes admin of an enabled board. Same exposure as
  before; the gesture needs physical access.
- **RF3-06 INFO** web_auth_store.c:714-721: the RF2-01 load/set runs outside cred_store_lock
  (same theoretical TOCTOU as RF2-09).
