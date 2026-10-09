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
     * match. Two distinct causes land here, and they must not share one
     * message: format_version really can differ even though every field's
     * VALUE happens to agree (a caller comparing across versions), which is
     * a real, nameable mismatch -- but format_first_field_difference()
     * above also returns false, with format_version equal on both sides,
     * whenever EVERY field is unknown on both sides at once (each field's
     * `e->known != p->known` test is false, and the `!e->known` branch
     * marks it "not the culprit" instead of a difference) -- and in that
     * case config_identity_t.known is false on both identities, which is
     * exactly why config_identity_matches() refused to call them equal
     * despite the hashes matching (see that function's own comment). Opus
     * review 2026-09-14 (defect 1): the OLD text here printed
     * "identity mismatch (format_version 1 vs 1, hash 0xBE20868A vs
     * 0xBE20868A)" for that all-unknown case -- an alarm that disables
     * heaters while reporting IDENTICAL versions and IDENTICAL hashes, an
     * operator has no way to act on and would reasonably read as the
     * system being broken rather than the config. Latent only because
     * today's one-field identity set can never actually be all-unknown in
     * a way that reaches this branch in practice from this file's own
     * caller (safety_ceiling_sync.c's target_known gate short-circuits
     * first) -- but the multi-kiln profile feature is adding fields to
     * this SAME identity right now, and a caller with several fields, none
     * yet confirmed on one side, reaches exactly this branch. Report what
     * is actually true instead: state plainly that one or more fields are
     * unknown, and name every unknown one, rather than a version/hash
     * comparison that reads as a contradiction. */
    if (reason_out && reason_cap > 0) {
        if (esp_id.format_version != pico_id.format_version) {
            snprintf(reason_out, reason_cap,
                     "config divergence: identity format version mismatch (ESP format_version %u vs Pico "
                     "format_version %u) -- values cannot be compared across versions",
                     (unsigned)esp_id.format_version, (unsigned)pico_id.format_version);
        } else if (!esp_id.known || !pico_id.known) {
            /* Build "field_a, field_b" naming every field unknown on
             * either side, bounded by reason_cap so a long field list
             * cannot overflow the caller's buffer -- snprintf's own
             * truncation-safe return-length contract does that for us; the
             * assertion below just proves the whole message still fit. */
            char names[CONFIG_DIVERGENCE_REASON_MAX];
            size_t names_len = 0;
            names[0] = '\0';
            bool any_known_gap = false;
            for (size_t i = 0; i < n; i++) {
                bool unk = !esp_fields[i].known || !pico_fields[i].known;
                if (!unk) {
                    continue;
                }
                const char *nm = esp_fields[i].name ? esp_fields[i].name : "(unnamed)";
                int written =
                    snprintf(names + names_len, sizeof(names) - names_len, "%s%s", any_known_gap ? ", " : "", nm);
                if (written > 0) {
                    size_t advance = (size_t)written;
                    names_len += (advance < sizeof(names) - names_len) ? advance : sizeof(names) - names_len - 1;
                }
                any_known_gap = true;
            }
            int len = snprintf(reason_out, reason_cap,
                                "config divergence: one or more fields not yet confirmed on both sides (%s) -- "
                                "cannot confirm the configs match",
                                any_known_gap ? names : "(unnamed)");
            /* 2026-09-14 opus review: this file's own header cites the
             * ki_refusal_reason 162-into-96-byte silent truncation as the
             * reason every message length here must be checked, not
             * assumed to fit -- so assert rather than trust snprintf's
             * return value went unexamined. */
            if (len < 0 || (size_t)len >= reason_cap) {
                abort();
            }
        } else {
            /* Neither version nor known-ness differ and yet the hash still
             * disagreed -- can only be an FNV collision (astronomically
             * unlikely for this field count) or an internal inconsistency
             * in this file. Name it honestly rather than staying silent
             * about a real "true" return. */
            snprintf(reason_out, reason_cap,
                     "config divergence: identity hash mismatch with matching version/known-ness (hash 0x%08X "
                     "vs 0x%08X) -- possible hash collision",
                     (unsigned)esp_id.hash, (unsigned)pico_id.hash);
        }
    }
    return true;
}
