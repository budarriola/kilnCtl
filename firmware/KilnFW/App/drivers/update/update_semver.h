// update_semver.h -- semantic-version parse/compare for the GitHub release
// update path (docs/GITHUB_RELEASE_UPDATE_PLAN.md D3/section 6): tags are
// vMAJOR.MINOR.PATCH, optionally with a -prerelease and +build suffix.
//
// Rules (semver 2.0.0): an optional leading 'v' or 'V' is accepted; numeric
// fields have no leading zeros and fit 9 digits; a prerelease sorts BELOW the
// same release; prerelease identifiers compare numerically when both numeric,
// numeric < alphanumeric, otherwise by ASCII, and a shorter identifier list
// sorts below a longer one it prefixes. Build metadata (+...) is validated
// and then IGNORED by comparison.
//
// Pure C, no ESP-IDF dependency; host-tested by App/test/test_update_semver.c.
#ifndef KILNCTL_UPDATE_SEMVER_H
#define KILNCTL_UPDATE_SEMVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_SEMVER_PRE_MAX 31 // prerelease characters kept (excl. NUL)

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    bool has_pre;
    char pre[UPDATE_SEMVER_PRE_MAX + 1];
} update_semver_t;

// Parse a NUL-terminated string. Returns false (and zeroes *out) on any
// malformed input, including NULL, empty, trailing junk, a prerelease longer
// than UPDATE_SEMVER_PRE_MAX, or whitespace.
bool update_semver_parse(const char *s, update_semver_t *out);

// Parse a bounded buffer (not necessarily NUL-terminated); len <= 64 or false.
bool update_semver_parse_n(const char *s, size_t len, update_semver_t *out);

// -1 if a < b, 0 if equal (build metadata ignored), +1 if a > b.
int update_semver_compare(const update_semver_t *a, const update_semver_t *b);

// Format as "MAJOR.MINOR.PATCH[-pre]" (no leading v). Returns the length
// written (excluding NUL), or 0 if buf is too small / args NULL.
size_t update_semver_format(const update_semver_t *v, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_UPDATE_SEMVER_H
