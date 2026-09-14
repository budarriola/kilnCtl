#ifndef CONFIG_DIVERGENCE_H
#define CONFIG_DIVERGENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* config_divergence -- a REUSABLE "do the ESP's and the Pico's config
 * actually match?" identity check, written for the 2026-09-14 owner
 * decision ("if a config doesn't land and match on both sides then alarm
 * and dissable heaters") but deliberately not tied to the safety-ceiling
 * field: a planned multi-kiln config-swap feature (both processors' config
 * packaged into one saved profile, an opus session is designing it as of
 * this writing) is expected to build on this SAME mechanism with its own,
 * larger field list, rather than a second comparator.
 *
 * WHAT "MATCHING" MEANS -- owner's own words, verbatim: "Matching config
 * meens format version number and a hash that identifys the set." This is
 * NOT a field-by-field comparator: two configs match iff their FORMAT
 * VERSION is equal AND their HASH over the field set is equal. Per-field
 * comparison still exists in this file (config_identity_first_difference())
 * but ONLY to build the operator-facing message once the hash gate has
 * already said "no match" -- it never gates the decision itself.
 *
 * FORMAT VERSION GATES THE COMPARISON. Two identities computed under
 * different format_version numbers are UNCONDITIONALLY a mismatch,
 * regardless of what the hash says -- comparing hashes computed over
 * different field sets (or a different normalisation) is meaningless, and
 * a coincidental hash collision across versions would be worse than
 * useless. CONFIG_IDENTITY_FORMAT_VERSION below is this mechanism's OWN
 * version tag for "which fields, in which order, are in the set" -- it is
 * independent of ZONES_CFG_VERSION (the ESP's persisted zones-JSON schema)
 * and of KILNLINK_PROTOCOL_VERSION (the wire framing): bumping it changes
 * nothing on flash or on the wire, only what this in-memory check compares.
 * Bump it whenever the field SET changes (e.g. the multi-kiln feature
 * adding fields) so an old and a new build can never silently agree on a
 * hash that means different things.
 *
 * WHO COMPUTES WHAT, INDEPENDENTLY (owner's stated hazard: "If the ESP
 * computes a hash and merely sends it to the Pico to hold and report back,
 * a Pico whose actual values have drifted would still return the expected
 * hash"). This file supplies ONE pure hash function; nothing stops a
 * caller from misusing it as a cached/echoed token, so the discipline is
 * enforced at the CALL SITE, not here -- see safety_ceiling_sync.c's own
 * comment on this for the concrete case: the ESP computes its own identity
 * from its own live authoritative zone config, from scratch, every check;
 * the "Pico side" identity is computed from values FRESHLY fetched off the
 * Pico's own GET_CONFIG_PAGE report (safety_cfg_store.c's live cache,
 * refreshed by safety_poll_task), never from a value the ESP itself wrote
 * moments ago and is merely re-reading back its own echo of. A caller that
 * instead fed this function a value it just pushed would defeat the whole
 * point -- that misuse is a call-site bug, not something this file can
 * prevent structurally, so it is called out here in the one place every
 * caller is expected to read first.
 *
 * FLOAT NORMALISATION -- the actual trap. Two floats can be numerically
 * equal yet carry different bit patterns after a round trip through JSON,
 * a units conversion, or a wire encode/decode -- hashing raw bit patterns
 * would then spuriously "diverge" on values nobody actually disagrees
 * about, and a nuisance safety check earns exactly one response: getting
 * switched off. So every float is normalised through the SAME %.9g
 * stringify -> strtof reparse round trip safety_cfg_http.c already performs
 * on every wire write (its own established convention for this exact
 * class of value, see that file's f32 encode/decode) BEFORE hashing --
 * config_identity_normalize_f32() below, exercised in
 * test_config_divergence.c against a value that has actually been through
 * that wire round trip, not just an idealised one. */

#define CONFIG_IDENTITY_FORMAT_VERSION 1u

typedef struct {
    /* Short, stable, human-meaningful name -- goes into the operator-facing
     * reason string when this field is the one that differs. Not copied;
     * must outlive the call (callers pass string literals). */
    const char *name;
    bool  known;
    float value;
} config_identity_field_t;

typedef struct {
    uint16_t format_version;
    uint32_t hash;
    /* false iff at least one field feeding the hash was unknown on this
     * side (e.g. the Pico has never confirmed a value this boot) --
     * carried alongside the hash because two "unknown" identities must
     * never be reported as matching just because their hashes happen to
     * collide over the same all-zero input. */
    bool known;
} config_identity_t;

/* Normalises `value` exactly the way safety_cfg_http.c's f32 wire encode
 * does (snprintf("%.9g", value) then strtof() back) -- so a value that
 * travelled the wire and a value that never left the process hash
 * IDENTICALLY, which is the entire reason this function exists separately
 * from just hashing the raw bytes of `value`. */
float config_identity_normalize_f32(float value);

/* Computes one side's identity over `fields` (n entries), in the order
 * given -- ORDER IS PART OF THE FORMAT and must be identical on both sides
 * a caller compares (document the fixed order at the call site, the way
 * safety_ceiling_sync.c's single-field array does). `known` fields are
 * normalised then folded into an FNV-1a hash together with the field's
 * name and `format_version`; an unknown field folds in a fixed sentinel
 * pattern instead of a real value, and also clears the returned `.known`
 * flag for the whole identity (see that field's own comment -- an identity
 * with any unknown field can never be reported as "matches" no matter what
 * the other side's hash is, see config_identity_matches() below). */
config_identity_t config_identity_compute(uint16_t format_version, const config_identity_field_t *fields, size_t n);

/* True iff `a` and `b` describe the SAME config: same format_version (the
 * gate -- see this header's own top comment), both `.known`, and equal
 * hashes. Two identities computed under different format_version numbers,
 * or either with `.known == false`, NEVER match, regardless of their hash
 * values. */
bool config_identity_matches(const config_identity_t *a, const config_identity_t *b);

/* Reason buffer size every caller in this codebase is expected to use --
 * sized generously against the longest message this file can produce, NOT
 * the 96-byte `ki_refusal_reason` mistake this convention exists to avoid
 * repeating (docs/audits/pico_ceiling_mirror_and_rate_guard_2026-09-14.md). */
#define CONFIG_DIVERGENCE_REASON_MAX 160

/* The operator-facing half. `esp_fields`/`pico_fields` (n entries each,
 * SAME order, same names) are each side's raw values for the same field
 * set. Computes both identities via config_identity_compute() and reports
 * true iff they do NOT match. On a mismatch, `reason_out`/`reason_cap` (if
 * non-NULL/nonzero) receive an operator-facing message naming the FIRST
 * field where `esp_fields[i]`/`pico_fields[i]` actually differ (or is
 * unknown on one side) -- purely for that message; the pass/fail decision
 * itself was already made by the hash/version comparison above, never by
 * this per-field walk. If every individual field happens to agree yet the
 * identities still fail to match (format_version differs between the two
 * `config_identity_t`s a caller already computed elsewhere), the message
 * names the version mismatch instead. */
bool config_divergence_check(const config_identity_field_t *esp_fields, const config_identity_field_t *pico_fields,
                              size_t n, char *reason_out, size_t reason_cap);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_DIVERGENCE_H */
