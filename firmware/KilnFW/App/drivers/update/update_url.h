// update_url.h -- the pure URL/host/redirect rules for the GitHub release
// fetch (docs/GITHUB_RELEASE_UPDATE_PLAN.md sections 9 and 11, WP8).
//
// Everything here is string logic with no ESP-IDF dependency, so the rules
// that stop the board from being steered at an arbitrary host are host-tested
// (App/test/test_update_url.c) rather than only reviewed.
//
// Rules:
//  - https only, no userinfo, no port other than 443, host is a plain DNS name.
//  - The host must EXACTLY match (case-insensitive, whole host) one of four
//    names: api.github.com, github.com, objects.githubusercontent.com,
//    release-assets.githubusercontent.com. Never a substring, prefix or
//    suffix match on the URL text. (The plan's section 9 says a
//    githubusercontent.com suffix; WP8 is deliberately narrower, listing only
//    the two content hosts GitHub has been seen redirecting to. Add a host
//    here, with a test, if a real release ever redirects elsewhere.)
//  - At most UPDATE_MAX_REDIRECTS redirects are followed, each target
//    re-checked with the same rules. No credential is ever sent.
//  - The repo setting is only ever `owner/name`, validated here; the board
//    builds the API URL itself and never fetches a user-typed URL.
#ifndef KILNCTL_UPDATE_URL_H
#define KILNCTL_UPDATE_URL_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_URL_MAX 2048 // incl. NUL; a signed release-assets redirect URL carries a JWT query
#define UPDATE_HOST_MAX 64  // incl. NUL
#define UPDATE_MAX_REDIRECTS 3
#define UPDATE_REPO_MAX 141 // buffer size: 39 + 1 + 100 + NUL (a safe over-bound)
#define UPDATE_REPO_VALID_MAX_LEN 62 // the real total-length cap (the persisted setting blob holds 62 + NUL)

typedef enum {
    UPDATE_URL_OK = 0,
    UPDATE_URL_E_EMPTY,         // NULL or ""
    UPDATE_URL_E_TOO_LONG,      // >= UPDATE_URL_MAX
    UPDATE_URL_E_SCHEME,        // not https://
    UPDATE_URL_E_MALFORMED,     // userinfo, bad port, control chars, bad host characters
    UPDATE_URL_E_HOST,          // well formed but not on the allowlist
    UPDATE_URL_E_TOO_MANY_HOPS  // redirect budget exhausted
} update_url_err_t;

// Parses `url` (must be https), writes the lowercase host into `host` and a
// pointer to the path (starting with '/', "/" when absent; includes any query)
// into *path_out (points into `url`). Does NOT check the allowlist.
update_url_err_t update_url_parse(const char *url, char *host, size_t host_cap, const char **path_out);

// True only for the four allowed hosts, exact whole-host match, case-insensitive.
bool update_host_allowed(const char *host);

// parse + allowlist. Writes the lowercase host (host may be NULL, host_cap 0).
update_url_err_t update_url_check(const char *url, char *host, size_t host_cap);

// A redirect `Location` after `hops_followed` redirects already followed: the
// (hops_followed + 1)th redirect is refused when that would exceed the cap.
// Relative Locations are refused (scheme error): GitHub always sends absolute.
update_url_err_t update_redirect_check(unsigned hops_followed, const char *location);

const char *update_url_err_name(update_url_err_t e);

// `owner/name`: ^[A-Za-z0-9._-]{1,39}/[A-Za-z0-9._-]{1,100}$, exactly one '/', total
// length 3..UPDATE_REPO_VALID_MAX_LEN, no "..", owner neither starting nor ending with '-',
// neither segment starting with '.'. THE single repo validator: update_settings_repo_is_valid()
// (WP9) delegates here so the setting and the fetcher can never disagree.
bool update_repo_valid(const char *repo);

// "https://api.github.com/repos/<repo>/releases/latest". False on a bad repo
// or a too-small buffer (out left empty).
bool update_url_build_latest(const char *repo, char *out, size_t out_cap);

// A release tag as GitHub hands it back and as the release tooling creates it:
// ^v\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$ and at most 32 characters (plus the semver rules).
// gen_build_info.cmake's FW_RELEASE_VERSION regex must accept the same character set;
// tools/check_release_version_regex.ps1 enforces that.
bool update_tag_valid(const char *tag);

// True when `url` is exactly
//   https://github.com/<repo>/releases/download/<tag>/<name>
// (repo compared case-insensitively, tag and name exactly, no query/fragment).
// This pins an asset URL taken from the release JSON to the configured repo,
// the tag it claims, and the asset it claims to be.
bool update_asset_url_matches(const char *url, const char *repo, const char *tag, const char *name);

// Response `Location` capture. esp_http_client_get_header() reads the REQUEST headers (IDF v6.0.2),
// so the redirect target has to be taken from the HTTP_EVENT_ON_HEADER events instead. This is the
// pure, host-tested part: key compare is case-insensitive (HTTP field names are), the value is
// copied into a caller-owned bounded buffer, and anything ambiguous (value too long for the buffer,
// a second Location header) is REFUSED rather than truncated. Feed every response header, then read
// the result with update_loc_capture_get().
typedef struct {
    char *buf;
    size_t cap;
    bool seen;
    bool refused;
} update_loc_capture_t;

void update_loc_capture_init(update_loc_capture_t *c, char *buf, size_t cap);
void update_loc_capture_feed(update_loc_capture_t *c, const char *key, const char *value);
// Returns the captured Location, or NULL when none was seen or it was refused (*refused says which).
const char *update_loc_capture_get(const update_loc_capture_t *c, bool *refused);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_URL_H
