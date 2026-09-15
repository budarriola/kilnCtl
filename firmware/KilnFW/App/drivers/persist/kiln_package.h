// kiln_package -- the Pico-half packaging and package-identity primitives for
// a saved "kiln config" slot (docs/KILN_PROFILES_PLAN.md, items 1/2/12).
//
// SCOPE OF THIS MODULE, STATED EXPLICITLY: this implements only the early,
// non-safety-critical slice of KILN_PROFILES_PLAN.md's section 8 --
// capturing the Pico's 68 commissioning parameters into a fixed-size,
// host-testable struct by WALKING the ESP-side mirror of CONFIG_PARAM_TABLE
// (safety_cfg_store_param_count()/_get_by_index()), never a curated field
// list, plus the package-identity hash (pkg_schema + pkg_hash, section
// 3.1.1/3.1.3). It deliberately does NOT implement: the two-processor apply
// transaction (plan item 5), the volatile Pico RAM install (item 15), the
// UNCONFIGURED boot-ordering fix (item 16), and the standing divergence
// alarm (item 7). Those are safety-critical or depend on work in flight
// elsewhere -- see the plan's own section 8 ordering note. This module only
// captures what the ESP already has cached from the Pico (safety_cfg_store.c)
// and hashes it; it never talks to the Pico itself and never pushes anything.
//
// 2026-09-14 follow-up (plan items 3/4/9/14, "finish upload/download"): this
// module now ALSO owns the section 5.1 JSON envelope's encode/decode
// (kiln_package_export_json()/kiln_package_import_json() below) -- the
// generic "wrap the already-hashed halves as portable text" step. It
// deliberately still does NOT know what a valid zones_cfg_t looks like or
// what this controller's hardware can run: kiln_cfg_store.c (which already
// includes zones_config_json.h/zones_config_accessors.h) is what calls
// zones_config_json_validate() and the section 5.2a hardware-compatibility
// checks against the blob this module hands back. Keeping that split means
// this module never has to be touched when a zone field or a compatibility
// rule changes -- same reasoning as kiln_cfg_store.h's own "never touched
// when a zone field is added" note.
//
// WHY A TABLE WALK, NOT A FIELD LIST: KILN_PROFILES_PLAN.md section 1.3 is
// explicit that a curated list of "the Pico params that matter" is exactly
// how a swap leaves a stale value behind (the three-day plant-model loss,
// `137dea1a`, is the same defect shape one level up). safety_cfg_store_
// param_count()/_get_by_index() already enumerate every row of
// CONFIG_PARAM_TABLE the ESP knows about; this module walks 0..count and
// captures whichever ones the cache currently reports set=true, unchanged,
// via an INJECTABLE accessor pair (kiln_pkg_pico_source_t) so a host test can
// drive it against a fake table without linking the real safety_cfg_store.c
// (which needs NVS/UART stubs of its own -- see test_kiln_package.c's header
// comment). Production code always passes kiln_pkg_pico_source_default().
#ifndef KILN_PACKAGE_H
#define KILN_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "safety_cfg_store.h" /* safety_cfg_param_t, KILNLINK_PARAM_TYPE_* */

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed capacity of the packaged Pico-param array, deliberately larger than
 * today's SAFETY_CFG_PARAM_COUNT (68, "ids only ever go up" per
 * COMMISSIONING.md sec 2.1) so a modest future growth of CONFIG_PARAM_TABLE
 * does not by itself force a KILN_CFG_STORE_VERSION bump -- kiln_cfg_store.c
 * has a compile-time assert that SAFETY_CFG_PARAM_COUNT fits inside this
 * cap; when it stops fitting, THAT assert is the forcing function to bump
 * both this cap and the store version together, not a silent truncation. */
#define KILN_PKG_SAFETY_PARAM_CAP 96u

/* bit0 of kiln_pkg_pico_param_t.flags: this param was `set` (had a real,
 * fetched-or-commissioned value) in the safety_cfg_store cache at capture
 * time. Mirrors safety_cfg_param_t.set (COMMISSIONING.md sec 3.1's "an
 * unset parameter carries value:false and OMITS value entirely") -- a param
 * the ESP has never fetched a value for is packaged as unset, never as a
 * fabricated 0. */
#define KILN_PKG_PARAM_FLAG_SET 0x01u

/* One packaged Pico param -- 8 bytes, fixed layout, no padding ambiguity
 * (never memcpy'd from/into a compiler struct for hashing purposes; see
 * kiln_package_compute_hash()'s own comment on why padding is never hashed).
 * value_bits holds the RAW wire value reinterpreted per `type`: bool/u8 in
 * the low byte, u16 in the low 2 bytes (both as the host's native byte
 * order -- this struct never crosses a network boundary itself; only its
 * CANONICAL SERIALIZATION, built explicitly field-by-field in
 * kiln_package_compute_hash(), is byte-order-fixed), f32 as the IEEE-754 bit
 * pattern of the IN-MEMORY float (kilnlink_param_value_t's own union member,
 * reinterpreted through a union rather than a cast so no conversion ever
 * happens). */
typedef struct {
    uint16_t param_id;
    uint8_t type;   /* KILNLINK_PARAM_TYPE_BOOL/U8/U16/F32 */
    uint8_t flags;  /* KILN_PKG_PARAM_FLAG_SET or 0 */
    uint32_t value_bits;
} kiln_pkg_pico_param_t;

/* The whole packaged Pico half. `count` <= KILN_PKG_SAFETY_PARAM_CAP;
 * entries[0..count) are populated in ASCENDING param_id order (plan section
 * 3.1.3 rule 2 -- CONFIG_PARAM_TABLE's own order is editorial, not numeric,
 * per config_params.c's 0x0211 comment, so the table's iteration order must
 * never be assumed to already be sorted); entries[count..cap) are zeroed and
 * not meaningful. */
typedef struct {
    uint16_t count;
    kiln_pkg_pico_param_t entries[KILN_PKG_SAFETY_PARAM_CAP];
} kiln_pkg_safety_t;

/* Injectable accessor pair so this module's capture/round-trip logic is
 * host-testable against a fake table with no dependency on the real
 * safety_cfg_store.c (which needs its own NVS/UART/FreeRTOS stub surface --
 * see that module's own test file). Production callers use
 * kiln_pkg_pico_source_default(); a host test builds its own struct
 * pointing at file-local fake functions matching these two signatures
 * exactly (safety_cfg_store_param_count()/_get_by_index()'s own
 * signatures). */
typedef struct {
    size_t (*param_count)(void);
    bool (*get_by_index)(size_t index, safety_cfg_param_t *out);
} kiln_pkg_pico_source_t;

/* The production accessor pair -- safety_cfg_store_param_count()/
 * _get_by_index(), unmodified. */
kiln_pkg_pico_source_t kiln_pkg_pico_source_default(void);

/* Walks source->param_count() rows via source->get_by_index(), sorts them
 * into *out by ascending param_id (an insertion sort over at most
 * KILN_PKG_SAFETY_PARAM_CAP=96 elements -- O(n^2) is fine at this size and
 * this call site, once per save, never in a hot loop), and returns true.
 * Every row is packaged regardless of `set` (an unset param is packaged
 * with flags=0 and value_bits=0, NEVER omitted) -- omitting an unset row
 * would make "the Pico has never been asked about this param" indistinguishable
 * from "this package's schema predates the param entirely" (section 6's
 * missing-field rule, generalised: a row that IS in the table must always be
 * represented, set or not).
 *
 * Returns false (out left zeroed, out->count == 0) only if source->
 * param_count() exceeds KILN_PKG_SAFETY_PARAM_CAP -- see this header's
 * capacity comment. Never truncates. `out`/`source`/its two function
 * pointers must not be NULL. */
bool kiln_package_capture_pico_half(const kiln_pkg_pico_source_t *source, kiln_pkg_safety_t *out);

/* Current package format identity (section 3.1.1/5.1's `pkg_schema`).
 * Versioned independently of KILN_CFG_STORE_VERICATION and ZONES_CFG_VERSION
 * -- this is the identity of the PACKAGE CONTENTS this module knows how to
 * build and hash, not of the on-flash slot layout (that is
 * KILN_CFG_STORE_VERSION, kiln_cfg_store_internal.h) or of zones_cfg_t
 * (ZONES_CFG_VERSION, zones_config_json.h). Bump only when this module's own
 * canonical serialization changes in a way that would change the hash of an
 * unchanged config -- see kiln_package_compute_hash()'s rules. */
#define KILN_PKG_SCHEMA_VERSION 1

/* Section 3.1.3's canonical binary serialization and CRC-32, computed over:
 *   pkg_schema (2B LE)
 *   esp_blob_len (2B LE) + esp_blob_len bytes of esp_blob verbatim
 *     (zones_config_export_blob()'s own output -- ALREADY a packed binary
 *     blob with its own version tag inside it, so its floats are already
 *     raw IEEE-754 bytes; this function does not re-derive or re-round them)
 *   pico->count (2B LE)
 *   for each of pico->entries[0..count) IN THE ARRAY'S OWN ORDER (the
 *     caller -- kiln_package_capture_pico_half() -- is what guarantees
 *     ascending param_id; this function does not re-sort, so a caller that
 *     hands it an unsorted kiln_pkg_safety_t gets a hash that is internally
 *     consistent but not the canonical one section 3.1.3 defines):
 *       param_id (2B LE), type (1B), flags (1B), value_bits (4B LE)
 * Built into one scratch buffer field-by-field -- never a struct memcpy --
 * so compiler padding can never leak into the hash for the PICO half (see
 * this header's KNOWN LIMITATION note below re: the ESP half).
 *
 * docs/audits/kiln_profiles_robustness_2026-09-14.md finding H2 (fixed
 * here): this used to return a plain uint32_t and signal failure ONLY via
 * an optional out_ok, with 0 returned on every failure path -- and 0 is
 * ALSO kiln_cfg_entry_t's documented "hash never computed" sentinel
 * (kiln_cfg_store_internal.h). A caller that forgot to check out_ok (an
 * easy mistake -- it is not the primary return value, the exact shape
 * `project_safety_calls_logging_unchecked_success` has already been bitten
 * by three times in this codebase) could store pkg_hash=0 on a slot marked
 * pico_populated=1, indistinguishable from "genuinely never computed" and
 * therefore never re-attempted. Returning bool makes the failure the
 * PRIMARY return value, so it cannot be silently discarded the way a
 * uint32_t can; the hash is written to *out_hash ONLY on success (true),
 * and *out_hash is left untouched -- never zeroed, never garbage -- on
 * failure, so a caller cannot mistake an untouched output for a real
 * value either. Fails (returns false, *out_hash untouched) only if `pico`
 * is NULL, `esp_blob` is NULL while esp_blob_len > 0, or esp_blob_len/
 * pico->count together would not fit this function's internal scratch
 * buffer (defensive; unreachable with today's ZONES_CONFIG_BLOB_MAX_SIZE/
 * KILN_PKG_SAFETY_PARAM_CAP ceilings) -- NEVER silently hashes a truncated
 * buffer.
 *
 * H1 FIXED (docs/audits/kiln_profiles_robustness_2026-09-14.md,
 * docs/audits/kiln_package_canonical_serializer_2026-09-14.md): `esp_blob`
 * is no longer expected to be zones_config_export_blob()'s raw (padding-
 * including) struct memcpy. Every caller now passes
 * zones_config_export_canonical()'s output instead (kiln_cfg_store.c's
 * populate_pico_half_and_hash()) -- a fixed, declaration-order, padding-free
 * encoding of zones_cfg_t built from an X-macro field table with a
 * compile-time completeness proof (see zones_config_accessors.c's own
 * comment on ZONE_CFG_FIELDS/ZONES_CFG_FIELDS for the "cannot silently
 * forget a field" argument, the same property this module's own Pico-half
 * table walk already has via CONFIG_PARAM_TABLE's enumerable count/get-by-
 * index). This function itself is unchanged -- it still just hashes
 * whatever bytes `esp_blob` hands it -- so this is a caller-contract note,
 * not a code change here. */
bool kiln_package_compute_hash(uint16_t pkg_schema, const uint8_t *esp_blob, uint16_t esp_blob_len,
                                const kiln_pkg_safety_t *pico, uint32_t *out_hash);

/* ---- Section 5.1 envelope: download/upload's portable JSON wrapper ------
 *
 * { "kind": "kilnctl_kiln_package", "pkg_schema": 1, "name": "...",
 *   "esp_blob_hex": "<hex of zones_config_export_blob()'s raw bytes>",
 *   "pico": [ {"id":N,"type":T,"flags":F,"value_bits":V}, ... ],
 *   "pkg_hash": "0xXXXXXXXX" }
 *
 * `esp_blob_hex` carries the SAME raw bytes kiln_cfg_store.c already keeps
 * in kiln_cfg_entry_t::blob (zones_config_export_blob()'s own output) --
 * hex, not base64, because this codebase has no base64 encoder anywhere
 * (see backup_json.h's own header comment on why there is no general JSON
 * library either) and a hand-rolled hex codec is four lines and cannot get
 * padding/alphabet edge cases wrong the way a hand-rolled base64 one could.
 * `pkg_hash` is carried as a hex string (not a JSON number) so a 32-bit
 * value near/above 2^31 round-trips exactly through every JSON reader,
 * including this file's own hand-rolled one, without a signed/unsigned
 * boundary surprise. */
#define KILN_PKG_KIND "kilnctl_kiln_package"

/* Upper bound on kiln_package_export_json()'s output for the largest ESP
 * blob (ZONES_CONFIG_BLOB_MAX_SIZE, hex-doubled) and a fully-populated Pico
 * half (KILN_PKG_SAFETY_PARAM_CAP entries, hex-doubled), plus name/envelope
 * overhead -- generous, not tight; callers heap-allocate this, never stack
 * it (project_httpd_stack_blob_class). */
#define KILN_PKG_JSON_MAX_LEN 6144u

/* Builds the envelope above into `out` (out_cap >= KILN_PKG_JSON_MAX_LEN).
 * `esp_blob`/`esp_blob_len` are the RAW bytes (zones_config_export_blob()'s
 * output, i.e. kiln_cfg_entry_t::blob/blob_len) -- NOT the canonical form
 * kiln_package_compute_hash() hashes; the hash itself is supplied by the
 * caller (`pkg_hash`, already computed the H1 canonical way by
 * kiln_cfg_store.c) rather than recomputed here, so this function never has
 * to know about zones_config_export_canonical() at all. Fails (nothing
 * written) if any pointer is NULL, `pico->count` exceeds
 * KILN_PKG_SAFETY_PARAM_CAP, or the encoded text would not fit `out_cap`. */
bool kiln_package_export_json(const char *name, uint16_t pkg_schema, const uint8_t *esp_blob,
                               uint16_t esp_blob_len, const kiln_pkg_safety_t *pico, uint32_t pkg_hash,
                               char *out, size_t out_cap, size_t *out_len);

/* Parses `json` (NUL-terminated) back into its parts. Checks performed HERE
 * (envelope-level only -- section 5.2 rules 1/2/4/5; rule 3, the ESP-half
 * field validation, is the caller's job, see this header's own banner):
 *   - `kind` must equal KILN_PKG_KIND exactly.
 *   - `pkg_schema` must be present and <= KILN_PKG_SCHEMA_VERSION -- a
 *     package from a NEWER firmware is refused here, before any field is
 *     decoded (section 5.1: "rejected, never best-effort parsed").
 *   - `esp` and `pico` must both be present and well-formed (missing either
 *     half is refused -- "package both processors together" means a
 *     half-package is not a kiln package).
 *   - every pico entry's `type` must be one of the four KILNLINK_PARAM_TYPE_*
 *     values this build knows; an unrecognised type is the same class of
 *     failure as an unknown param id (section 5.2 rule 5) and is refused,
 *     never skipped.
 *   - `pkg_hash` is decoded but NOT verified here -- verifying it requires
 *     re-deriving the ESP half's CANONICAL form (zones_config_export_
 *     canonical()), which this module deliberately does not link against;
 *     the caller (kiln_cfg_store_import_package_json()) does that
 *     comparison immediately after this call succeeds, before anything is
 *     written. *out_declared_hash is always populated on success so the
 *     caller has something to compare against.
 * `esp_blob_out`/`esp_blob_cap`/`out_esp_blob_len` receive the raw (not
 * canonical) ESP-half bytes, sized identically to what
 * kiln_package_export_json() was given. On any failure, reason_out (if
 * non-NULL/non-zero) is filled with a specific reason and nothing else is
 * touched. */
bool kiln_package_import_json(const char *json, char *name_out, size_t name_cap, uint16_t *out_pkg_schema,
                               uint8_t *esp_blob_out, size_t esp_blob_cap, uint16_t *out_esp_blob_len,
                               kiln_pkg_safety_t *pico_out, uint32_t *out_declared_hash, char *reason_out,
                               size_t reason_cap);

#ifdef __cplusplus
}
#endif

#endif // KILN_PACKAGE_H
