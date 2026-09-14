#include "config_divergence.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

float config_identity_normalize_f32(float value)
{
    /* Same convention as safety_cfg_http.c's f32 wire encode/decode:
     * %.9g is enough decimal digits to round-trip a float32 exactly for
     * any value that did not already carry more precision than a float32
     * can hold, so re-parsing it collapses any bit-pattern noise a JSON or
     * wire round trip may have introduced into the canonical value the
     * text itself represents. */
    char buf[32];
    snprintf(buf, sizeof(buf), "%.9g", (double)value);
    return strtof(buf, NULL);
}

/* FNV-1a, 32-bit. Not a cryptographic hash -- this is a "did anything in
 * this fixed, enumerated field set change" identity check, not a defence
 * against a hostile adversary crafting a collision; FNV-1a's job here is
 * just to fold a variable-length byte stream into a fixed 32-bit token
 * cheaply and deterministically. */
static uint32_t fnv1a(uint32_t hash, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t fnv1a_str(uint32_t hash, const char *s)
{
    if (!s) {
        return hash;
    }
    return fnv1a(hash, s, strlen(s));
}

config_identity_t config_identity_compute(uint16_t format_version, const config_identity_field_t *fields, size_t n)
{
    config_identity_t out;
    out.format_version = format_version;
    out.known = true;

    uint32_t hash = 2166136261u; /* FNV-1a offset basis */
    hash = fnv1a(hash, &format_version, sizeof(format_version));

    for (size_t i = 0; i < n; i++) {
        const config_identity_field_t *f = &fields[i];
        hash = fnv1a_str(hash, f->name);
        if (!f->known) {
            out.known = false;
            /* Fixed sentinel pattern, NOT the field's stale/default bit
             * pattern -- an unknown field must not coincidentally hash the
             * same as some real value. */
            static const unsigned char sentinel[4] = { 0xA5, 0x5A, 0xA5, 0x5A };
            hash = fnv1a(hash, sentinel, sizeof(sentinel));
            continue;
        }
        float normalized = config_identity_normalize_f32(f->value);
        hash = fnv1a(hash, &normalized, sizeof(normalized));
    }

    out.hash = hash;
    return out;
}

bool config_identity_matches(const config_identity_t *a, const config_identity_t *b)
{
    if (!a || !b) {
        return false;
    }
    if (a->format_version != b->format_version) {
        /* The gate: comparing hashes across format versions is meaningless. */
        return false;
    }
    if (!a->known || !b->known) {
        /* An identity with any unknown field never "matches", regardless
         * of what the other side's hash is -- see this header's own
         * comment on config_identity_t.known. */
        return false;
    }
    return a->hash == b->hash;
}

/* Finds and formats the first field where esp/pico actually disagree, for
 * the operator-facing message only -- never consulted by the pass/fail
 * decision, which is config_identity_matches()'s job alone. */
static bool format_first_field_difference(const config_identity_field_t *esp_fields,
                                           const config_identity_field_t *pico_fields, size_t n, char *reason_out,
                                           size_t reason_cap)
{
    for (size_t i = 0; i < n; i++) {
        const config_identity_field_t *e = &esp_fields[i];
        const config_identity_field_t *p = &pico_fields[i];
        bool differs;
        if (e->known != p->known) {
            differs = true;
        } else if (!e->known) {
            differs = false; /* both unknown on this one field -- not the culprit */
        } else {
            float ne = config_identity_normalize_f32(e->value);
            float np = config_identity_normalize_f32(p->value);
            differs = (ne != np);
        }
        if (!differs) {
            continue;
        }
        if (reason_out && reason_cap > 0) {
            char esp_side[32];
            char pico_side[32];
            if (e->known) {
                snprintf(esp_side, sizeof(esp_side), "%.2f", (double)e->value);
            } else {
                snprintf(esp_side, sizeof(esp_side), "%s", "unconfirmed");
            }
            if (p->known) {
                snprintf(pico_side, sizeof(pico_side), "%.2f", (double)p->value);
            } else {
                snprintf(pico_side, sizeof(pico_side), "%s", "unconfirmed");
            }
            snprintf(reason_out, reason_cap, "config divergence: %s ESP=%s Pico=%s",
                     e->name ? e->name : "(unnamed)", esp_side, pico_side);
        }
        return true;
    }
    return false;
}

bool config_divergence_check(const config_identity_field_t *esp_fields, const config_identity_field_t *pico_fields,
                              size_t n, char *reason_out, size_t reason_cap)
{
    if (reason_out && reason_cap > 0) {
        reason_out[0] = '\0';
    }

    config_identity_t esp_id = config_identity_compute(CONFIG_IDENTITY_FORMAT_VERSION, esp_fields, n);
    config_identity_t pico_id = config_identity_compute(CONFIG_IDENTITY_FORMAT_VERSION, pico_fields, n);

    if (config_identity_matches(&esp_id, &pico_id)) {
        return false;
    }

    /* Mismatch confirmed by the hash/version gate above. Everything past
     * this point is only about building an honest, specific message --
     * never about the decision itself. */
    if (format_first_field_difference(esp_fields, pico_fields, n, reason_out, reason_cap)) {
        return true;
    }
    /* Every individual field agreed and yet the identities still failed to
     * match -- can only happen if a caller passed mismatched format
     * versions in some other way, or an FNV collision (astronomically
     * unlikely for this field count). Name it honestly rather than staying
     * silent about a real "true" return. */
    if (reason_out && reason_cap > 0) {
        snprintf(reason_out, reason_cap,
                 "config divergence: identity mismatch (format_version %u vs %u, hash 0x%08X vs 0x%08X)",
                 (unsigned)esp_id.format_version, (unsigned)pico_id.format_version, (unsigned)esp_id.hash,
                 (unsigned)pico_id.hash);
    }
    return true;
}
