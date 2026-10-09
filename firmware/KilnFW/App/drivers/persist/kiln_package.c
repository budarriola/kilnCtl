#include "kiln_package.h"
#include "persist_scratch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crc.h" /* esp_crc32_le() -- same primitive zones_config_store.c/crash_report.c
                       * already use for their own CRC32s (host-testable via
                       * test/stubs/esp_crc.h, a real CRC32 not a fake -- see that stub's
                       * header comment). */
#include "backup_json.h" /* hand-rolled JSON reader already used by backup_import.c -- see
                           * this file's own header comment on why there is no general JSON
                           * library in this codebase. */

/* Reinterprets `value` (tagged by `type`) as a plain uint32_t bit pattern --
 * never a numeric conversion, always a byte-for-byte reinterpretation via a
 * union, so a float's IEEE-754 bits round-trip exactly and a bool/u8/u16
 * value_bits comparison is bit-for-bit too (plan section 3.2's "compare f32
 * params bit-for-bit on the stored representation, not with an epsilon" --
 * this is what makes that comparison possible one layer up). Unknown `type`
 * (should be unreachable -- safety_cfg_store.c's own table only ever reports
 * one of the four KILNLINK_PARAM_TYPE_* tags) reads as 0. */
static uint32_t value_to_bits(uint8_t type, const kilnlink_param_value_t *value)
{
    union {
        float f;
        uint32_t bits;
    } conv;
    switch (type) {
    case KILNLINK_PARAM_TYPE_BOOL:
        return (uint32_t)value->bool_val;
    case KILNLINK_PARAM_TYPE_U8:
        return (uint32_t)value->u8_val;
    case KILNLINK_PARAM_TYPE_U16:
        return (uint32_t)value->u16_val;
    case KILNLINK_PARAM_TYPE_F32:
        conv.f = value->f32_val;
        return conv.bits;
    default:
        return 0;
    }
}

kiln_pkg_pico_source_t kiln_pkg_pico_source_default(void)
{
    kiln_pkg_pico_source_t src;
    src.param_count = safety_cfg_store_param_count;
    src.get_by_index = safety_cfg_store_get_by_index;
    return src;
}

bool kiln_package_capture_pico_half(const kiln_pkg_pico_source_t *source, kiln_pkg_safety_t *out)
{
    memset(out, 0, sizeof(*out));
    size_t n = source->param_count();
    if (n > KILN_PKG_SAFETY_PARAM_CAP) {
        /* Never truncate -- section 6's "no partial write" rule generalises
         * to capture, not only to apply. A caller must bump
         * KILN_PKG_SAFETY_PARAM_CAP (and, in practice, KILN_CFG_STORE_VERSION
         * alongside it) before this can succeed again. */
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        safety_cfg_param_t row;
        if (!source->get_by_index(i, &row)) {
            /* Table says `n` rows exist but this one could not be read --
             * treat exactly like "too many to fit": refuse the whole
             * capture rather than package a hole. */
            memset(out, 0, sizeof(*out));
            return false;
        }
        kiln_pkg_pico_param_t *dst = &out->entries[i];
        dst->param_id = row.param_id;
        dst->type = row.type;
        dst->flags = row.set ? KILN_PKG_PARAM_FLAG_SET : 0;
        dst->value_bits = row.set ? value_to_bits(row.type, &row.value) : 0;
    }
    out->count = (uint16_t)n;

    /* Ascending-param_id sort (plan section 3.1.3 rule 2) -- CONFIG_PARAM_
     * TABLE's own iteration order is editorial, not numeric (config_params.c's
     * 0x0211 comment). Insertion sort: at most 96 elements, run once per
     * save, never a hot path -- clarity over cleverness here. */
    for (size_t i = 1; i < out->count; i++) {
        kiln_pkg_pico_param_t key = out->entries[i];
        size_t j = i;
        while (j > 0 && out->entries[j - 1].param_id > key.param_id) {
            out->entries[j] = out->entries[j - 1];
            j--;
        }
        out->entries[j] = key;
    }
    return true;
}

/* Scratch buffer ceiling for kiln_package_compute_hash()'s canonical
 * serialization: 2 (pkg_schema) + 2 (esp_blob_len) + ZONES_CONFIG_BLOB_MAX_
 * SIZE-worst-case (bounded by the uint16_t param itself, so use its full
 * range ceiling actually enforced by the caller) + 2 (pico count) +
 * KILN_PKG_SAFETY_PARAM_CAP * 8 (each packed entry). Sized generously
 * against today's real ceilings (896 + 96*8 = 1664) with headroom, and
 * heap-allocated (never a stack buffer -- this runs from kiln_cfg_store.c's
 * save/apply paths, which the httpd worker can reach, and this codebase's
 * standing rule is "no stack buffer grows for this feature",
 * project_httpd_stack_blob_class). */
#define KILN_PKG_HASH_SCRATCH_CAP 4096u

bool kiln_package_compute_hash(uint16_t pkg_schema, const uint8_t *esp_blob, uint16_t esp_blob_len,
                                const kiln_pkg_safety_t *pico, uint32_t *out_hash)
{
    /* H2 fix: *out_hash is written ONLY on success, and never zeroed on
     * failure -- an untouched caller-owned variable is a clearer failure
     * signal than a value this function chose, and the PRIMARY return
     * value (bool) is what callers must check; see this header's own
     * comment for why a discardable uint32_t sentinel was the defect. */
    if (!pico || (esp_blob_len > 0 && !esp_blob) || !out_hash) {
        return false;
    }
    size_t needed = 2u /* pkg_schema */
                    + 2u /* esp_blob_len */
                    + (size_t)esp_blob_len
                    + 2u /* pico->count */
                    + (size_t)pico->count * 8u;
    if (needed > KILN_PKG_HASH_SCRATCH_CAP) {
        return false; /* defensive -- unreachable with today's ceilings; never hash a truncated buffer */
    }

    uint8_t *buf = (uint8_t *)persist_scratch_alloc(KILN_PKG_HASH_SCRATCH_CAP);
    if (!buf) {
        return false;
    }
    size_t off = 0;

    buf[off++] = (uint8_t)(pkg_schema & 0xFFu);
    buf[off++] = (uint8_t)((pkg_schema >> 8) & 0xFFu);

    buf[off++] = (uint8_t)(esp_blob_len & 0xFFu);
    buf[off++] = (uint8_t)((esp_blob_len >> 8) & 0xFFu);
    if (esp_blob_len > 0) {
        memcpy(buf + off, esp_blob, esp_blob_len);
        off += esp_blob_len;
    }

    buf[off++] = (uint8_t)(pico->count & 0xFFu);
    buf[off++] = (uint8_t)((pico->count >> 8) & 0xFFu);
    for (uint16_t i = 0; i < pico->count; i++) {
        const kiln_pkg_pico_param_t *e = &pico->entries[i];
        buf[off++] = (uint8_t)(e->param_id & 0xFFu);
        buf[off++] = (uint8_t)((e->param_id >> 8) & 0xFFu);
        buf[off++] = e->type;
        buf[off++] = e->flags;
        buf[off++] = (uint8_t)(e->value_bits & 0xFFu);
        buf[off++] = (uint8_t)((e->value_bits >> 8) & 0xFFu);
        buf[off++] = (uint8_t)((e->value_bits >> 16) & 0xFFu);
        buf[off++] = (uint8_t)((e->value_bits >> 24) & 0xFFu);
    }

    uint32_t crc = esp_crc32_le(0, buf, (uint32_t)off);
    free(buf);
    *out_hash = crc;
    return true;
}

/* ---- Section 5.1 envelope -------------------------------------------- */

static const char HEX_DIGITS[] = "0123456789abcdef";

static void hex_encode(const uint8_t *src, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = HEX_DIGITS[(src[i] >> 4) & 0xF];
        out[i * 2 + 1] = HEX_DIGITS[src[i] & 0xF];
    }
    out[len * 2] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes exactly `out_cap` bytes' worth of hex text (2*out_cap hex chars)
 * from a NUL-terminated string of unknown length. Fails on any non-hex
 * character, an odd count, or a length that does not match `out_len`
 * exactly -- never a partial/truncated decode (same "no partial write"
 * discipline as kiln_package_capture_pico_half()). */
static bool hex_decode_exact(const char *src, size_t src_len, uint8_t *out, size_t out_len)
{
    if (src_len != out_len * 2) {
        return false;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(src[i * 2]);
        int lo = hex_nibble(src[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool kiln_package_export_json(const char *name, uint16_t pkg_schema, const uint8_t *esp_blob,
                               uint16_t esp_blob_len, const kiln_pkg_safety_t *pico, uint32_t pkg_hash,
                               uint32_t source_board_id, char *out, size_t out_cap, size_t *out_len)
{
    if (!name || !esp_blob || !pico || !out || !out_len) {
        return false;
    }
    if (pico->count > KILN_PKG_SAFETY_PARAM_CAP) {
        return false;
    }

    /* Hex scratch for the ESP blob -- heap, sized exactly, never a fixed
     * stack buffer (this function is reachable from an httpd export
     * handler). */
    char *esp_hex = (char *)persist_scratch_alloc((size_t)esp_blob_len * 2 + 1);
    if (!esp_hex) {
        return false;
    }
    hex_encode(esp_blob, esp_blob_len, esp_hex);

    /* esp_hex is freed exactly ONCE, at this single `done`/`fail` exit,
     * never inside the APPEND macro itself -- an earlier version freed it
     * from within the macro's own failure branch, which is textually
     * expanded at every APPEND call site and made GCC's -Werror=use-after-
     * free (correctly) unable to prove the later, unconditional free()
     * could never run on an already-freed pointer, even though the actual
     * control flow (an early `return false` on every failure branch) makes
     * that impossible. A single cleanup point removes the ambiguity for
     * both the compiler and the next reader. */
    size_t o = 0;
    int w;
    bool ok = true;
#define APPEND(...)                                                                                             \
    do {                                                                                                         \
        w = snprintf(out + o, o < out_cap ? out_cap - o : 0, __VA_ARGS__);                                       \
        if (w < 0) {                                                                                             \
            ok = false;                                                                                          \
            goto done;                                                                                          \
        }                                                                                                        \
        o += (size_t)w;                                                                                          \
    } while (0)

    APPEND("{\"kind\":\"%s\",\"pkg_schema\":%u,\"name\":\"", KILN_PKG_KIND, (unsigned)pkg_schema);
    /* Name is an operator-entered label already length-bounded and
     * character-restricted by kiln_cfg_store.c's normalize_name() before it
     * ever reaches a saved slot -- no separate JSON-escaping pass needed
     * here (same assumption kiln_cfg_http.c's own json_escape() call sites
     * make explicit for OTHER surfaces; this one relies on the store's own
     * character restriction instead of re-escaping). */
    APPEND("%s", name);
    APPEND("\",\"esp_blob_len\":%u,\"esp_blob_hex\":\"%s\",\"source_board_id\":\"0x%08x\",\"pico\":[",
           (unsigned)esp_blob_len, esp_hex, (unsigned)source_board_id);

    for (uint16_t i = 0; i < pico->count; i++) {
        const kiln_pkg_pico_param_t *e = &pico->entries[i];
        APPEND("%s{\"id\":%u,\"type\":%u,\"flags\":%u,\"value_bits\":%u}", i == 0 ? "" : ",",
               (unsigned)e->param_id, (unsigned)e->type, (unsigned)e->flags, (unsigned)e->value_bits);
    }
    APPEND("],\"pkg_hash\":\"0x%08x\"}", (unsigned)pkg_hash);
#undef APPEND

done:
    free(esp_hex);
    if (!ok || o >= out_cap) {
        /* Truncated -- refuse outright rather than hand back a JSON blob
         * that looks complete but silently isn't (the exact class of defect
         * kiln_cfg_store.c's own format-truncation note warns about). */
        return false;
    }
    *out_len = o;
    return true;
}

bool kiln_package_import_json(const char *json, char *name_out, size_t name_cap, uint16_t *out_pkg_schema,
                               uint8_t *esp_blob_out, size_t esp_blob_cap, uint16_t *out_esp_blob_len,
                               kiln_pkg_safety_t *pico_out, uint32_t *out_declared_hash,
                               bool *out_has_source_board, uint32_t *out_source_board_id, char *reason_out,
                               size_t reason_cap)
{
#define REFUSE(msg)                                                                                              \
    do {                                                                                                         \
        if (reason_out && reason_cap) {                                                                          \
            snprintf(reason_out, reason_cap, "%s", (msg));                                                       \
        }                                                                                                        \
        return false;                                                                                            \
    } while (0)

    if (!json || !name_out || !out_pkg_schema || !esp_blob_out || !out_esp_blob_len || !pico_out ||
        !out_declared_hash) {
        REFUSE("internal error: NULL argument to package parser");
    }

    char kind[32];
    if (!backup_json_field_str(json, "kind", kind, sizeof(kind)) || strcmp(kind, KILN_PKG_KIND) != 0) {
        REFUSE("not a kiln package file (missing or wrong \"kind\")");
    }

    double schema_d;
    if (!backup_json_field_num(json, "pkg_schema", &schema_d) || schema_d < 0 || schema_d > 65535) {
        REFUSE("not a kiln package file (missing or malformed \"pkg_schema\")");
    }
    uint16_t pkg_schema = (uint16_t)schema_d;
    if (pkg_schema > KILN_PKG_SCHEMA_VERSION) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "this package's format version (%u) is newer than this firmware knows (%u) -- refused, "
                 "not best-effort parsed; update the firmware first",
                 (unsigned)pkg_schema, (unsigned)KILN_PKG_SCHEMA_VERSION);
        REFUSE(msg);
    }
    /* pkg_schema == 0 is not a real version this module has ever emitted
     * (KILN_PKG_SCHEMA_VERSION starts at 1) -- refuse it as malformed rather
     * than silently accepting it as "very old". */
    if (pkg_schema == 0) {
        REFUSE("not a kiln package file (pkg_schema is 0)");
    }

    if (!backup_json_field_str(json, "name", name_out, name_cap)) {
        REFUSE("not a kiln package file (missing \"name\")");
    }

    double esp_len_d;
    if (!backup_json_field_num(json, "esp_blob_len", &esp_len_d) || esp_len_d < 0 || esp_len_d > 65535) {
        REFUSE("not a kiln package file (missing or malformed \"esp_blob_len\")");
    }
    uint16_t esp_blob_len = (uint16_t)esp_len_d;
    if (esp_blob_len == 0 || (size_t)esp_blob_len > esp_blob_cap) {
        REFUSE("package's ESP configuration section is missing, empty, or too large for this firmware");
    }

    const char *esp_hex_val = backup_json_obj_find(json, "esp_blob_hex");
    if (!esp_hex_val || *esp_hex_val != '"') {
        REFUSE("not a kiln package file (missing \"esp_blob_hex\")");
    }
    /* Decode straight out of the source text -- esp_blob_hex can be up to
     * ZONES_CONFIG_BLOB_MAX_SIZE*2 (1792) hex chars, well past any stack
     * buffer this codebase's own rule allows, so this walks the source
     * string directly rather than copying it to a local buffer first. */
    const char *hstart = esp_hex_val + 1;
    const char *hend = hstart;
    while (*hend && *hend != '"') {
        hend++;
    }
    if (*hend != '"' || !hex_decode_exact(hstart, (size_t)(hend - hstart), esp_blob_out, esp_blob_len)) {
        REFUSE("package's ESP configuration section is not valid hex, or its length does not match "
               "esp_blob_len -- file is truncated or corrupted");
    }
    *out_esp_blob_len = esp_blob_len;
    *out_pkg_schema = pkg_schema;

    const char *pico_arr = backup_json_obj_find(json, "pico");
    if (!pico_arr) {
        REFUSE("package is missing its safety-processor (\"pico\") section -- a half-package cannot be "
               "used");
    }
    memset(pico_out, 0, sizeof(*pico_out));
    uint16_t count = 0;
    const char *elem = backup_json_arr_first(pico_arr);
    while (elem) {
        if (count >= KILN_PKG_SAFETY_PARAM_CAP) {
            REFUSE("package's safety-processor section has more parameters than this firmware supports");
        }
        double id_d, type_d, flags_d, vb_d;
        if (!backup_json_field_num(elem, "id", &id_d) || id_d < 0 || id_d > 65535 ||
            !backup_json_field_num(elem, "type", &type_d) || type_d < 0 || type_d > 255 ||
            !backup_json_field_num(elem, "flags", &flags_d) || flags_d < 0 || flags_d > 255 ||
            !backup_json_field_num(elem, "value_bits", &vb_d) || vb_d < 0 || vb_d > 4294967295.0) {
            REFUSE("package's safety-processor section has a malformed parameter entry");
        }
        uint8_t type = (uint8_t)type_d;
        if (type != KILNLINK_PARAM_TYPE_BOOL && type != KILNLINK_PARAM_TYPE_U8 &&
            type != KILNLINK_PARAM_TYPE_U16 && type != KILNLINK_PARAM_TYPE_F32) {
            REFUSE("package's safety-processor section names a parameter type this firmware does not "
                   "recognise -- refused, not skipped");
        }
        kiln_pkg_pico_param_t *dst = &pico_out->entries[count];
        dst->param_id = (uint16_t)id_d;
        dst->type = type;
        dst->flags = (uint8_t)flags_d;
        dst->value_bits = (uint32_t)vb_d;
        count++;
        elem = backup_json_arr_next(elem);
    }
    pico_out->count = count;

    /* source_board_id: OPTIONAL field (added after pkg_schema 1 shipped
     * without it -- KILN_PKG_SCHEMA_VERSION is NOT bumped for this, see
     * kiln_package.h's ruling comment). Absent entirely -> *out_has_source_
     * board=false, caller treats that as "foreign" (fail-safe, see
     * kiln_cfg_store.c). Present but malformed hex -> REFUSE outright, same
     * as a malformed pkg_hash -- a field that exists but cannot be trusted
     * is worse than one that was never written. */
    if (out_has_source_board && out_source_board_id) {
        const char *sb_val = backup_json_obj_find(json, "source_board_id");
        if (!sb_val) {
            *out_has_source_board = false;
            *out_source_board_id = 0;
        } else {
            char sb_hex[16];
            if (!backup_json_field_str(json, "source_board_id", sb_hex, sizeof(sb_hex))) {
                REFUSE("package's \"source_board_id\" is present but malformed");
            }
            char *sb_end = NULL;
            unsigned long sbv = strtoul(sb_hex, &sb_end, 16);
            if (sb_end == sb_hex || *sb_end != '\0') {
                REFUSE("package's \"source_board_id\" is present but malformed");
            }
            *out_has_source_board = true;
            *out_source_board_id = (uint32_t)sbv;
        }
    }

    char hash_hex[16];
    if (!backup_json_field_str(json, "pkg_hash", hash_hex, sizeof(hash_hex))) {
        REFUSE("not a kiln package file (missing \"pkg_hash\")");
    }
    char *end = NULL;
    unsigned long hv = strtoul(hash_hex, &end, 16);
    if (end == hash_hex || *end != '\0') {
        REFUSE("not a kiln package file (malformed \"pkg_hash\")");
    }
    *out_declared_hash = (uint32_t)hv;

    return true;
#undef REFUSE
}
