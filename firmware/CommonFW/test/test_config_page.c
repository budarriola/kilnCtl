/* Host-native test for kilnlink_config_page.{c,h} -- the Pico->ESP
 * SAFETY_CMD_CONFIG_PAGE (0x1F) bulk-read reply codec, docs/LINK_PROTOCOL.md
 * sec 4/6, docs/COMMISSIONING.md sec 2. Covers the packing scheme itself
 * (kilnlink_config_page_pack() greedily fitting entries into the 253-byte
 * cap and reporting `more`), a byte-exact vector, and the hostile decode
 * input set: too-short, entry_count too large, bad type tag, truncated
 * mid-entry-header, truncated mid-value, and trailing garbage after the
 * last entry. A length sweep proves the decoder never reads out of bounds
 * and never reports OK except at the one length its own header implies.
 */

/* Also proves the KILNLINK_PROTOCOL_VERSION 7 split from the request
 * (kilnlink_get_config_page.h's SAFETY_CMD_GET_CONFIG_PAGE, now 0x24): this
 * reply's id (0x1F, unchanged) and the request's id must be DIFFERENT, and
 * this decoder must REJECT a frame carrying the request's id.
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_config_page.h"
#include "kilnlink/kilnlink_get_config_page.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);          \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static void print_hex(const char *label, const uint8_t *b, size_t n)
{
    printf("%s: ", label);
    for (size_t i = 0; i < n; ++i) printf("%02x", b[i]);
    printf("\n");
}

/* -- packing: everything fits in one page --------------------------------- */

static void test_pack_all_fit_one_page(void)
{
    kilnlink_config_page_entry_t entries[3];
    entries[0].param_id = 1;
    entries[0].type = KILNLINK_PARAM_TYPE_F32;
    entries[0].value.f32_val = 1300.0f;
    entries[0].set = true;
    entries[1].param_id = 2;
    entries[1].type = KILNLINK_PARAM_TYPE_U16;
    entries[1].value.u16_val = 3600;
    entries[1].set = true;
    entries[2].param_id = 3;
    entries[2].type = KILNLINK_PARAM_TYPE_BOOL;
    entries[2].value.bool_val = 1;
    entries[2].set = true;

    uint8_t buf[253];
    size_t packed = 0;
    kilnlink_config_page_status_t status;
    size_t n = kilnlink_config_page_pack(0, entries, 3, buf, sizeof(buf), &packed, &status);
    CHECK(status == KILNLINK_CONFIG_PAGE_OK, "pack(): one-page fit reports OK");
    CHECK(packed == 3, "pack(): all 3 entries packed when there is room");
    CHECK(n == KILNLINK_CONFIG_PAGE_HDR_LEN + (3u + 4u) + (3u + 2u) + (3u + 1u),
          "pack(): total length is header + sum of entry sizes");

    kilnlink_config_page_t decoded;
    CHECK(kilnlink_config_page_decode(buf, n, &decoded) == KILNLINK_CONFIG_PAGE_OK,
          "decode(): just-packed page decodes OK");
    CHECK(decoded.page_index == 0, "decode(): page_index round-trips");
    CHECK(decoded.entry_count == 3, "decode(): entry_count round-trips");
    CHECK(decoded.more == 0, "decode(): more == 0 when everything fit");
    CHECK(decoded.entries[0].param_id == 1 && decoded.entries[0].value.f32_val == 1300.0f,
          "decode(): entry 0 (F32) round-trips");
    CHECK(decoded.entries[1].param_id == 2 && decoded.entries[1].value.u16_val == 3600,
          "decode(): entry 1 (U16) round-trips");
    CHECK(decoded.entries[2].param_id == 3 && decoded.entries[2].value.bool_val == 1,
          "decode(): entry 2 (BOOL) round-trips");
    CHECK(decoded.entries[0].set && decoded.entries[1].set && decoded.entries[2].set,
          "decode(): a SET entry round-trips set=true");
}

/* -- packing: overflow into a second page --------------------------------- */

static void test_pack_overflow_two_pages(void)
{
    /* 5 BOOL entries, each 4 bytes on the wire (3-byte entry header + 1
     * value byte) = 20 bytes of entries, but a 10-byte out_cap only leaves
     * 6 bytes for entries after the 4-byte header -- room for exactly 1. */
    kilnlink_config_page_entry_t entries[5];
    for (unsigned i = 0; i < 5; i++) {
        entries[i].param_id = (uint16_t)(100 + i);
        entries[i].type = KILNLINK_PARAM_TYPE_BOOL;
        entries[i].value.bool_val = (uint8_t)(i & 1);
        entries[i].set = true;
    }

    uint8_t buf1[10];
    size_t packed1 = 0;
    kilnlink_config_page_status_t status;
    size_t n1 = kilnlink_config_page_pack(0, entries, 5, buf1, sizeof(buf1), &packed1, &status);
    CHECK(status == KILNLINK_CONFIG_PAGE_OK, "pack() page 0: reports OK");
    CHECK(packed1 == 1, "pack() page 0: only 1 of 5 entries fit in a 10-byte cap");
    CHECK(n1 == KILNLINK_CONFIG_PAGE_HDR_LEN + 4u, "pack() page 0: header + 1 entry");

    kilnlink_config_page_t decoded1;
    CHECK(kilnlink_config_page_decode(buf1, n1, &decoded1) == KILNLINK_CONFIG_PAGE_OK,
          "decode() page 0: OK");
    CHECK(decoded1.more == 1, "decode() page 0: more == 1, 4 entries remain");
    CHECK(decoded1.entries[0].param_id == 100, "decode() page 0: entry 0 is the first of the array");

    /* Caller re-invokes with the leftover slice and the next page_index. */
    uint8_t buf2[253];
    size_t packed2 = 0;
    size_t n2 = kilnlink_config_page_pack(1, entries + packed1, 5 - packed1, buf2, sizeof(buf2),
                                           &packed2, &status);
    CHECK(status == KILNLINK_CONFIG_PAGE_OK, "pack() page 1: reports OK");
    CHECK(packed2 == 4, "pack() page 1: the remaining 4 entries all fit with room to spare");

    kilnlink_config_page_t decoded2;
    CHECK(kilnlink_config_page_decode(buf2, n2, &decoded2) == KILNLINK_CONFIG_PAGE_OK,
          "decode() page 1: OK");
    CHECK(decoded2.page_index == 1, "decode() page 1: page_index is 1");
    CHECK(decoded2.more == 0, "decode() page 1: more == 0, nothing left");
    CHECK(decoded2.entries[0].param_id == 101, "decode() page 1: resumes where page 0 left off");
    CHECK(decoded2.entries[3].param_id == 104, "decode() page 1: last entry is the array's last");
}

/* -- byte-exact vector ---------------------------------------------------- */

static void test_vector_single_bool_entry(void)
{
    /* cmd, page_index=0, entry_count=1, more=0, then param_id=0x0001 LE,
     * type=BOOL(0x00), value=1 */
    static const uint8_t expected[] = {0x1f, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01};
    kilnlink_config_page_entry_t entry;
    entry.param_id = 1;
    entry.type = KILNLINK_PARAM_TYPE_BOOL;
    entry.value.bool_val = 1;
    entry.set = true; /* type byte 0x00, no KILNLINK_CONFIG_PAGE_UNSET_BIT -- exactly the old, pre-fix bytes */

    uint8_t buf[64];
    size_t packed = 0;
    kilnlink_config_page_status_t status;
    size_t n = kilnlink_config_page_pack(0, &entry, 1, buf, sizeof(buf), &packed, &status);
    CHECK(status == KILNLINK_CONFIG_PAGE_OK, "vector: pack OK");
    CHECK(packed == 1, "vector: 1 entry packed");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "vector: bytes match");
    } else {
        CHECK(1, "vector: bytes match");
    }
}

/* -- the `set` bit (2026-08-27 audit fix, commissioning-write defect d) --- */

static void test_vector_unset_entry_sets_top_bit(void)
{
    /* Same shape as test_vector_single_bool_entry() but set=false -- the
     * ONLY byte that differs is the type byte, which must carry
     * KILNLINK_CONFIG_PAGE_UNSET_BIT (0x80) ORed into KILNLINK_PARAM_TYPE_
     * BOOL (0x00) -> 0x80. */
    static const uint8_t expected[] = {0x1f, 0x00, 0x01, 0x00, 0x01, 0x00, 0x80, 0x01};
    kilnlink_config_page_entry_t entry;
    entry.param_id = 1;
    entry.type = KILNLINK_PARAM_TYPE_BOOL;
    entry.value.bool_val = 1; /* a placeholder value -- must still be encoded, per this header's contract */
    entry.set = false;

    uint8_t buf[64];
    size_t packed = 0;
    kilnlink_config_page_status_t status;
    size_t n = kilnlink_config_page_pack(0, &entry, 1, buf, sizeof(buf), &packed, &status);
    CHECK(status == KILNLINK_CONFIG_PAGE_OK, "unset vector: pack OK");
    CHECK(packed == 1, "unset vector: 1 entry packed");
    if (n != sizeof(expected) || memcmp(buf, expected, sizeof(expected)) != 0) {
        print_hex("  got     ", buf, n);
        print_hex("  expected", expected, sizeof(expected));
        CHECK(0, "unset vector: bytes match (type byte carries 0x80)");
    } else {
        CHECK(1, "unset vector: bytes match (type byte carries 0x80)");
    }

    kilnlink_config_page_t decoded;
    CHECK(kilnlink_config_page_decode(buf, n, &decoded) == KILNLINK_CONFIG_PAGE_OK,
          "unset vector: decodes OK (0x80 is not itself an error)");
    CHECK(decoded.entries[0].set == false, "unset vector: decode() reports set=false");
    CHECK(decoded.entries[0].type == KILNLINK_PARAM_TYPE_BOOL,
          "unset vector: the real type (BOOL) is recovered -- the 0x80 bit does NOT leak into `type`");
}

static void test_pack_mixed_set_and_unset_round_trips_independently(void)
{
    /* Directly reproduces the audit's motivating scenario: abs_max_temp_c
     * (here entry 0, still genuinely unset before commissioning) alongside
     * an ordinary already-committed threshold (entry 1) in the SAME page --
     * proves the bit is per-entry, not page-wide. */
    kilnlink_config_page_entry_t entries[2];
    entries[0].param_id = 0x0104; /* abs_max_temp_c */
    entries[0].type = KILNLINK_PARAM_TYPE_F32;
    entries[0].value.f32_val = 0.0f; /* the exact dangerous placeholder the audit named */
    entries[0].set = false;
    entries[1].param_id = 0x0201; /* firing_margin_c -- has a real compiled default, always set */
    entries[1].type = KILNLINK_PARAM_TYPE_F32;
    entries[1].value.f32_val = 100.0f;
    entries[1].set = true;

    uint8_t buf[64];
    size_t packed = 0;
    kilnlink_config_page_status_t status;
    size_t n = kilnlink_config_page_pack(0, entries, 2, buf, sizeof(buf), &packed, &status);
    CHECK(status == KILNLINK_CONFIG_PAGE_OK && packed == 2, "mixed set/unset: both entries pack");

    kilnlink_config_page_t decoded;
    CHECK(kilnlink_config_page_decode(buf, n, &decoded) == KILNLINK_CONFIG_PAGE_OK, "mixed set/unset: decodes OK");
    CHECK(decoded.entries[0].param_id == 0x0104 && decoded.entries[0].set == false,
          "mixed set/unset: abs_max_temp_c reads back UNSET -- this is the fix: it must NEVER read "
          "back set=true with a value of 0.0, which would mean the overtemperature guard never trips");
    CHECK(decoded.entries[1].param_id == 0x0201 && decoded.entries[1].set == true &&
              decoded.entries[1].value.f32_val == 100.0f,
          "mixed set/unset: firing_margin_c reads back SET with its real value, unaffected by entry 0's bit");
}

static void test_decode_a_pre_fix_frame_with_no_unset_bit_defaults_every_entry_set(void)
{
    /* A hand-built frame using bytes a pre-2026-08-27 Pico would have sent
     * (bit 7 of the type byte never touched) -- proves the wire-compatible
     * direction this header's own comment claims: an OLD sender's frames
     * still decode, and every entry reads back set=true, matching that
     * era's actual (if less honest) behavior exactly. */
    uint8_t buf[KILNLINK_CONFIG_PAGE_HDR_LEN + KILNLINK_CONFIG_PAGE_ENTRY_HDR_LEN + 4u] = {
        KILNLINK_CONFIG_PAGE_CMD, 0x00, 0x01, 0x00,
        0x04, 0x01, KILNLINK_PARAM_TYPE_F32, /* param_id=0x0104 (abs_max_temp_c), type F32, bit 7 clear */
    };
    /* f32 0.0f = 4 zero bytes, already zero-initialized above. */
    kilnlink_config_page_t out;
    CHECK(kilnlink_config_page_decode(buf, sizeof(buf), &out) == KILNLINK_CONFIG_PAGE_OK,
          "pre-fix frame: decodes OK");
    CHECK(out.entries[0].set == true,
          "pre-fix frame (bit 7 never set): decodes as set=true -- the OLD, less-honest-but-not-"
          "wrong-direction default, never a spurious decode failure against an old peer");
}

/* -- pack() hostile inputs ------------------------------------------------- */

static void test_pack_buffer_too_small_for_header(void)
{
    kilnlink_config_page_entry_t entry = {0};
    entry.type = KILNLINK_PARAM_TYPE_BOOL;
    uint8_t buf[3]; /* header alone needs 4 */
    size_t packed = 123;
    kilnlink_config_page_status_t status;
    size_t n = kilnlink_config_page_pack(0, &entry, 1, buf, sizeof(buf), &packed, &status);
    CHECK(n == 0, "pack(): out_cap too small for the header writes nothing");
    CHECK(status == KILNLINK_CONFIG_PAGE_ERR_BUFFER_TOO_SMALL,
          "pack(): out_cap too small for the header -> ERR_BUFFER_TOO_SMALL");
    CHECK(packed == 0, "pack(): entries_packed reset to 0 on this error");
}

static void test_pack_bad_type_is_caller_bug(void)
{
    kilnlink_config_page_entry_t entry = {0};
    entry.type = 0x7Fu; /* not a real tag -- caller built a bad entry */
    uint8_t buf[64];
    size_t packed = 0;
    kilnlink_config_page_status_t status;
    size_t n = kilnlink_config_page_pack(0, &entry, 1, buf, sizeof(buf), &packed, &status);
    CHECK(n == 0, "pack(): a bad-type entry writes nothing");
    CHECK(status == KILNLINK_CONFIG_PAGE_ERR_BAD_TYPE, "pack(): a bad-type entry -> ERR_BAD_TYPE");
}

/* -- decode() hostile inputs ------------------------------------------------ */

static void test_decode_too_short(void)
{
    uint8_t buf[KILNLINK_CONFIG_PAGE_HDR_LEN - 1] = {0};
    buf[0] = KILNLINK_CONFIG_PAGE_CMD;
    kilnlink_config_page_t out;
    CHECK(kilnlink_config_page_decode(buf, sizeof(buf), &out) == KILNLINK_CONFIG_PAGE_ERR_LENGTH_MISMATCH,
          "decode() shorter than the 4-byte header -> ERR_LENGTH_MISMATCH");
}

static void test_decode_entry_count_too_large(void)
{
    uint8_t buf[KILNLINK_CONFIG_PAGE_HDR_LEN] = {
        KILNLINK_CONFIG_PAGE_CMD, 0x00, (uint8_t)(KILNLINK_CONFIG_PAGE_MAX_ENTRIES + 1), 0x00,
    };
    kilnlink_config_page_t out;
    CHECK(kilnlink_config_page_decode(buf, sizeof(buf), &out) ==
              KILNLINK_CONFIG_PAGE_ERR_TOO_MANY_ENTRIES,
          "decode() entry_count above KILNLINK_CONFIG_PAGE_MAX_ENTRIES -> ERR_TOO_MANY_ENTRIES");
}

static void test_decode_truncated_entry_header(void)
{
    /* entry_count=1 but only 2 bytes follow the header -- not even enough
     * for one entry's 3-byte id+type prefix. */
    uint8_t buf[KILNLINK_CONFIG_PAGE_HDR_LEN + 2] = {
        KILNLINK_CONFIG_PAGE_CMD, 0x00, 0x01, 0x00, 0x01, 0x00,
    };
    kilnlink_config_page_t out;
    CHECK(kilnlink_config_page_decode(buf, sizeof(buf), &out) == KILNLINK_CONFIG_PAGE_ERR_LENGTH_MISMATCH,
          "decode() truncated before one entry's id+type -> ERR_LENGTH_MISMATCH");
}

static void test_decode_bad_type_in_entry(void)
{
    uint8_t buf[KILNLINK_CONFIG_PAGE_HDR_LEN + 4] = {
        KILNLINK_CONFIG_PAGE_CMD, 0x00, 0x01, 0x00, /* header, 1 entry, no more */
        0x01, 0x00, 0x7Fu, 0x00,                    /* param_id=1, bad type, 1 spare byte */
    };
    kilnlink_config_page_t out;
    CHECK(kilnlink_config_page_decode(buf, sizeof(buf), &out) == KILNLINK_CONFIG_PAGE_ERR_BAD_TYPE,
          "decode() an entry with an unrecognised type tag -> ERR_BAD_TYPE");
}

static void test_decode_truncated_mid_value(void)
{
    /* 1 entry, type=U16 (needs 2 value bytes), only 1 supplied. */
    uint8_t buf[KILNLINK_CONFIG_PAGE_HDR_LEN + 4] = {
        KILNLINK_CONFIG_PAGE_CMD, 0x00, 0x01, 0x00,
        0x01, 0x00, KILNLINK_PARAM_TYPE_U16, 0x10,
    };
    kilnlink_config_page_t out;
    CHECK(kilnlink_config_page_decode(buf, sizeof(buf), &out) == KILNLINK_CONFIG_PAGE_ERR_LENGTH_MISMATCH,
          "decode() truncated mid-U16-value -> ERR_LENGTH_MISMATCH");
}

static void test_decode_trailing_garbage(void)
{
    /* 1 BOOL entry correctly encoded, plus one extra trailing byte the
     * entry_count doesn't account for. */
    uint8_t buf[KILNLINK_CONFIG_PAGE_HDR_LEN + 5] = {
        KILNLINK_CONFIG_PAGE_CMD, 0x00, 0x01, 0x00,
        0x01, 0x00, KILNLINK_PARAM_TYPE_BOOL, 0x01,
        0xAAu, /* trailing garbage */
    };
    kilnlink_config_page_t out;
    CHECK(kilnlink_config_page_decode(buf, sizeof(buf), &out) == KILNLINK_CONFIG_PAGE_ERR_LENGTH_MISMATCH,
          "decode() with a trailing byte past the last entry -> ERR_LENGTH_MISMATCH");
}

static void test_decode_wrong_cmd(void)
{
    uint8_t buf[KILNLINK_CONFIG_PAGE_HDR_LEN] = {0x1Eu, 0x00, 0x00, 0x00};
    kilnlink_config_page_t out;
    CHECK(kilnlink_config_page_decode(buf, sizeof(buf), &out) == KILNLINK_CONFIG_PAGE_ERR_WRONG_CMD,
          "decode() with wrong command byte -> ERR_WRONG_CMD");
}

/* -- length sweep for a fixed 2-entry page -------------------------------- */

static void test_length_sweep(void)
{
    kilnlink_config_page_entry_t entries[2];
    entries[0].param_id = 1;
    entries[0].type = KILNLINK_PARAM_TYPE_U16;
    entries[0].value.u16_val = 0x1234;
    entries[0].set = true;
    entries[1].param_id = 2;
    entries[1].type = KILNLINK_PARAM_TYPE_BOOL;
    entries[1].value.bool_val = 1;
    entries[1].set = true;

    uint8_t full[64];
    size_t packed = 0;
    kilnlink_config_page_status_t status;
    size_t full_len = kilnlink_config_page_pack(0, entries, 2, full, sizeof(full), &packed, &status);
    CHECK(status == KILNLINK_CONFIG_PAGE_OK && packed == 2, "length sweep: fixture packs both entries");

    for (size_t len = 0; len <= full_len + 4; len++) {
        uint8_t buf[64 + 4] = {0};
        size_t copy = len < full_len ? len : full_len;
        memcpy(buf, full, copy);
        for (size_t i = copy; i < len; i++) {
            buf[i] = 0xAA; /* trailing garbage past the real frame */
        }
        kilnlink_config_page_t out;
        kilnlink_config_page_status_t st = kilnlink_config_page_decode(buf, len, &out);
        if (len == full_len) {
            CHECK(st == KILNLINK_CONFIG_PAGE_OK, "length sweep: the one exact length decodes OK");
        } else {
            CHECK(st != KILNLINK_CONFIG_PAGE_OK, "length sweep: every other length is rejected");
        }
    }
}

/* -- request/reply id separation (KILNLINK_PROTOCOL_VERSION 7) ----------- */

static void test_request_and_reply_ids_differ(void)
{
    CHECK(KILNLINK_CONFIG_PAGE_CMD != KILNLINK_GET_CONFIG_PAGE_CMD,
          "CONFIG_PAGE reply id and GET_CONFIG_PAGE request id must be different");
}

static void test_decode_rejects_request_id(void)
{
    uint8_t buf[KILNLINK_CONFIG_PAGE_HDR_LEN] = {
        KILNLINK_GET_CONFIG_PAGE_CMD, 0x00, 0x00, 0x00,
    };
    kilnlink_config_page_t out;
    CHECK(kilnlink_config_page_decode(buf, sizeof(buf), &out) == KILNLINK_CONFIG_PAGE_ERR_WRONG_CMD,
          "CONFIG_PAGE decode() rejects a frame carrying GET_CONFIG_PAGE's (request) id");
}

int main(void)
{
    test_pack_all_fit_one_page();
    test_pack_overflow_two_pages();
    test_vector_single_bool_entry();
    test_vector_unset_entry_sets_top_bit();
    test_pack_mixed_set_and_unset_round_trips_independently();
    test_decode_a_pre_fix_frame_with_no_unset_bit_defaults_every_entry_set();
    test_pack_buffer_too_small_for_header();
    test_pack_bad_type_is_caller_bug();
    test_decode_too_short();
    test_decode_entry_count_too_large();
    test_decode_truncated_entry_header();
    test_decode_bad_type_in_entry();
    test_decode_truncated_mid_value();
    test_decode_trailing_garbage();
    test_decode_wrong_cmd();
    test_length_sweep();
    test_request_and_reply_ids_differ();
    test_decode_rejects_request_id();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
