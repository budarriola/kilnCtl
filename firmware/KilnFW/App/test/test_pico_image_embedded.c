// Host tests for App/drivers/net/pico_image_embedded.c -- the two embedded
// SaftyFW slot images and whether they carry one agreeing identity.
// docs/PICO_AUTO_UPDATE_PLAN.md, owner decision 2026-09-20.
//
// pico_image_embedded_describe_from() is the pure core: it takes two caller-
// supplied buffers instead of the linker-generated EMBED_FILES symbols, so it
// is host-testable exactly like every other ESP-IDF-adjacent module in this
// tree that splits a thin hardware-facing wrapper from a pure core (see
// pico_image_embedded.h's own doc comment on the split). The .c is #included
// directly so its statics and exact compiled behaviour are under test.
#include <string.h>

#include "test_common.h"

// asm("_binary_...") is a GCC/binutils extension (EMBED_FILES) with no MSVC
// equivalent -- #define it away to nothing so pico_image_embedded.c's
// `extern const uint8_t X[] asm("...");` lines parse as plain
// `extern const uint8_t X[];`. Real 1-byte placeholder definitions follow the
// include, below the matching #undef -- same convention test_zones_http.c's
// header comment documents, enforced by tools/check_host_embed_symbols_defined.ps1.
#define asm(x)

#include "../drivers/net/pico_image_embedded.c"

#undef asm

// ---- Embedded-image symbols pico_image_embedded_describe() references -----
// Never actually read by these tests (they exercise
// pico_image_embedded_describe_from() with synthetic buffers instead), but
// must exist for the linker.
const uint8_t SaftyFW_slotA_bin_start[1] = { 0 };
const uint8_t SaftyFW_slotA_bin_end[1] = { 0 };
const uint8_t SaftyFW_slotB_bin_start[1] = { 0 };
const uint8_t SaftyFW_slotB_bin_end[1] = { 0 };

/* Builds a well-formed record with the given commit text, mirroring
 * test_pico_image_identity.c's make_record() helper. */
static void make_record(saftyfw_image_identity_t *r, const char *commit, uint8_t dirty,
                        uint16_t config_format_version)
{
    memset(r, 0, sizeof(*r));
    r->magic0 = SAFTYFW_IMAGE_IDENTITY_MAGIC0;
    r->magic1 = SAFTYFW_IMAGE_IDENTITY_MAGIC1;
    r->record_version = SAFTYFW_IMAGE_IDENTITY_RECORD_VERSION;
    r->dirty = dirty;
    r->commit_len = (uint8_t)strlen(commit);
    memcpy(r->commit, commit, strlen(commit));
    r->config_format_version = config_format_version;
    r->link_protocol_version = 0u;
    r->magic_end = SAFTYFW_IMAGE_IDENTITY_MAGIC_END;
}

void run_test_pico_image_embedded(void);

void run_test_pico_image_embedded(void)
{
    printf("\n-- pico image embedded --\n");

    saftyfw_image_identity_t rec;
    make_record(&rec, "0123456789abcdef0123456789abcdef01234567", 0u, 2u);

    uint8_t buf_a[256];
    uint8_t buf_b[256];

    /* ---- both slots agree: usable ---- */
    memset(buf_a, 0xA5, sizeof(buf_a));
    memset(buf_b, 0xA5, sizeof(buf_b));
    memcpy(buf_a + 64, &rec, sizeof(rec));
    memcpy(buf_b + 96, &rec, sizeof(rec)); /* different offset -- position must not matter */

    pico_image_embedded_info_t out;
    TEST_CHECK(pico_image_embedded_describe_from(buf_a, sizeof(buf_a), buf_b, sizeof(buf_b), &out),
               "describe_from always returns true (there is always an answer)");
    TEST_CHECK(out.usable, "two slots with an identical identity are usable");
    TEST_CHECK(out.commit_len == 40u, "the shared commit length is carried through");
    TEST_CHECK(memcmp(out.commit, rec.commit, 40u) == 0, "the shared commit text is carried through");
    TEST_CHECK(!out.dirty, "the shared dirty flag is carried through");
    TEST_CHECK(out.config_format_version == 2u, "the shared config format version is carried through");
    TEST_CHECK(out.slot_found[0] && out.slot_found[1], "both slots report a found identity record");
    TEST_CHECK(out.slot_data[0] == buf_a && out.slot_data[1] == buf_b,
               "raw slot pointers are always populated for the staging writer");
    TEST_CHECK(out.reason[0] == '\0', "no reason is set when usable");

    /* ---- mismatched commit: unusable, both commits named in reason ---- */
    saftyfw_image_identity_t rec_b;
    make_record(&rec_b, "fedcba9876543210fedcba9876543210fedcba9", 0u, 2u);
    memset(buf_a, 0xA5, sizeof(buf_a));
    memset(buf_b, 0xA5, sizeof(buf_b));
    memcpy(buf_a + 64, &rec, sizeof(rec));
    memcpy(buf_b + 64, &rec_b, sizeof(rec_b));

    memset(&out, 0, sizeof(out));
    TEST_CHECK(pico_image_embedded_describe_from(buf_a, sizeof(buf_a), buf_b, sizeof(buf_b), &out),
               "describe_from returns true even when unusable");
    TEST_CHECK(!out.usable, "a commit mismatch between the two slots is unusable -- ABANDONED_NO_IMAGE "
                            "semantics, never 'trust slot A'");
    TEST_CHECK(strstr(out.reason, "0123456789") != NULL && strstr(out.reason, "fedcba9876") != NULL,
               "the reason names BOTH slots' commits, not just one");

    /* ---- mismatched dirty flag: unusable ---- */
    saftyfw_image_identity_t rec_dirty = rec;
    rec_dirty.dirty = 1u;
    memset(buf_a, 0xA5, sizeof(buf_a));
    memset(buf_b, 0xA5, sizeof(buf_b));
    memcpy(buf_a + 64, &rec, sizeof(rec));
    memcpy(buf_b + 64, &rec_dirty, sizeof(rec_dirty));
    memset(&out, 0, sizeof(out));
    pico_image_embedded_describe_from(buf_a, sizeof(buf_a), buf_b, sizeof(buf_b), &out);
    TEST_CHECK(!out.usable, "a dirty-flag mismatch between the two slots is unusable");

    /* ---- mismatched config_format_version: unusable ---- */
    saftyfw_image_identity_t rec_cfg = rec;
    rec_cfg.config_format_version = 3u;
    memset(buf_a, 0xA5, sizeof(buf_a));
    memset(buf_b, 0xA5, sizeof(buf_b));
    memcpy(buf_a + 64, &rec, sizeof(rec));
    memcpy(buf_b + 64, &rec_cfg, sizeof(rec_cfg));
    memset(&out, 0, sizeof(out));
    pico_image_embedded_describe_from(buf_a, sizeof(buf_a), buf_b, sizeof(buf_b), &out);
    TEST_CHECK(!out.usable, "a config_format_version mismatch between the two slots is unusable");

    /* ---- one slot has no identity record at all: unusable, names which one ---- */
    memset(buf_a, 0xA5, sizeof(buf_a));
    memset(buf_b, 0xA5, sizeof(buf_b));
    memcpy(buf_a + 64, &rec, sizeof(rec));
    /* buf_b left as noise -- no record. */
    memset(&out, 0, sizeof(out));
    pico_image_embedded_describe_from(buf_a, sizeof(buf_a), buf_b, sizeof(buf_b), &out);
    TEST_CHECK(!out.usable, "a slot with no readable identity record is unusable");
    TEST_CHECK(strstr(out.reason, "B") != NULL, "the reason names slot B as the one missing a record");

    memset(buf_a, 0xA5, sizeof(buf_a));
    memset(buf_b, 0xA5, sizeof(buf_b));
    memcpy(buf_b + 64, &rec, sizeof(rec));
    /* buf_a left as noise this time. */
    memset(&out, 0, sizeof(out));
    pico_image_embedded_describe_from(buf_a, sizeof(buf_a), buf_b, sizeof(buf_b), &out);
    TEST_CHECK(!out.usable, "the other slot missing a record is also unusable");
    TEST_CHECK(strstr(out.reason, "A") != NULL, "the reason names slot A as the one missing a record");

    /* ---- NULL-safety ---- */
    TEST_CHECK(!pico_image_embedded_describe_from(buf_a, sizeof(buf_a), buf_b, sizeof(buf_b), NULL),
               "a NULL out returns false and does nothing");

    /* ---- review finding D4: pico_image_embedded_should_use() ---- */
    /* usable + clean -> use it. */
    memset(buf_a, 0xA5, sizeof(buf_a));
    memset(buf_b, 0xA5, sizeof(buf_b));
    memcpy(buf_a + 64, &rec, sizeof(rec));
    memcpy(buf_b + 64, &rec, sizeof(rec));
    memset(&out, 0, sizeof(out));
    pico_image_embedded_describe_from(buf_a, sizeof(buf_a), buf_b, sizeof(buf_b), &out);
    TEST_CHECK(out.usable && !out.dirty, "sanity: this pair is usable and clean");
    TEST_CHECK(pico_image_embedded_should_use(&out),
               "usable, clean, agreeing pair -- should_use is true");

    /* usable but dirty (both slots agree they are dirty) -> must NOT be used,
     * even though `usable` alone is true. This is the exact defect: a dirty
     * embedded image can never be confirmed as matching, so trying it burns
     * the attempt budget and latches the readiness block every boot. */
    saftyfw_image_identity_t rec_both_dirty = rec;
    rec_both_dirty.dirty = 1u;
    memset(buf_a, 0xA5, sizeof(buf_a));
    memset(buf_b, 0xA5, sizeof(buf_b));
    memcpy(buf_a + 64, &rec_both_dirty, sizeof(rec_both_dirty));
    memcpy(buf_b + 64, &rec_both_dirty, sizeof(rec_both_dirty));
    memset(&out, 0, sizeof(out));
    pico_image_embedded_describe_from(buf_a, sizeof(buf_a), buf_b, sizeof(buf_b), &out);
    TEST_CHECK(out.usable && out.dirty, "sanity: this pair is usable but dirty");
    TEST_CHECK(!pico_image_embedded_should_use(&out),
               "usable but dirty -- should_use is false (inert, not NEEDED)");

    /* not usable at all -> also never used. */
    memset(&out, 0, sizeof(out));
    out.usable = false;
    out.dirty = false;
    TEST_CHECK(!pico_image_embedded_should_use(&out), "not usable -- should_use is false");

    /* NULL-safety. */
    TEST_CHECK(!pico_image_embedded_should_use(NULL), "NULL -- should_use is false");
}
