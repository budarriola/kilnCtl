// http_auth_policy_iface.h -- second small seam, parallel to
// http_session_iface.h: whether web auth is currently turned on at all
// (docs/WEB_AUTH_PLAN.md section 11's `auth_policy.web_enabled`, owned by
// sections 2/11, in flight when section 5 was built).
//
// Kept separate from http_session_iface.h on purpose -- "is a session
// valid" and "is auth even turned on for this interface" are two different
// facts with two different owners (the session table vs. the persisted
// policy record), and folding them into one seam would make a future
// change to either one a reason to touch the other's contract.
#ifndef KILNCTL_HTTP_AUTH_POLICY_IFACE_H
#define KILNCTL_HTTP_AUTH_POLICY_IFACE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// True iff web authentication is currently enabled.
//
// Section 2/3/11's credential storage foundation has since landed
// (firmware/KilnFW/App/drivers/persist/web_auth_store.h, commit c3008eb8) as
// web_auth_policy_effective_enabled(web_auth_load_status_t, bool
// stored_enabled) -- the one collapse function for this exact question,
// which this seam's real implementation (http_auth_policy_iface.c) calls
// directly rather than re-deriving the rule: ABSENT (never configured, the
// shipped/field-upgrade default) reads false; OK reads the stored value;
// UNREADABLE reads TRUE -- note this is fail-CLOSED (auth forced ON, not
// off) for a record this build cannot verify, e.g. after an OTA rollback
// across a schema change, so a board never silently drops back to
// unauthenticated because its credential record became unreadable. Calling
// http_auth_policy_web_enabled() rather than re-deriving this from
// web_auth_store_load_policy()'s raw status at each call site keeps that
// single collapse point singular -- a second copy of the ABSENT/OK/
// UNREADABLE decision is exactly the reset-one-side-of-a-pair shape
// CLAUDE.md warns about.
//
// http_auth_policy_iface_stub.c below (host-test / pre-landing builds only)
// always returns false, matching the shipped-OFF default -- so this seam
// changes nothing about existing route behaviour on any board this feature
// has not yet been wired for.
bool http_auth_policy_web_enabled(void);

// True iff the administrator-credential bootstrap state (plan items 10/11,
// web_auth_admin_bootstrap_needed() in net/web_auth_session.h) currently
// holds for the web interface. Calls that one predicate directly, fed by
// http_auth_policy_web_enabled() (for effective_enabled) and
// web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR) (for
// admin_credential_configured) -- never re-derives "enabled &&
// !configured" inline, same discipline as http_auth_policy_web_enabled()
// itself toward web_auth_policy_effective_enabled().
bool http_auth_policy_admin_bootstrap_needed(void);

// True iff the board is currently unprovisioned for Wi-Fi (owner decision
// 2026-09-28: ROUTE_TIER_WIFI_SETUP, route_tier_table.h). Calls
// wifi_prov_is_unprovisioned() directly -- never re-derives "no STA
// credentials saved" from wifi_prov_get_state()/wifi_prov_get_mode() inline
// here, same discipline as this seam's other two predicates.
bool http_auth_policy_wifi_unprovisioned(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_HTTP_AUTH_POLICY_IFACE_H
