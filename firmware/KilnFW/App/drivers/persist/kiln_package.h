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
// UNCONFIGURED boot-ordering fix (item 16), the standing divergence alarm
// (item 7), or upload/download (items 3/4/9/14). Those are safety-critical
// or depend on work in flight elsewhere -- see the plan's own section 8
// ordering note. This module only captures what the ESP already has cached
// from the Pico (safety_cfg_store.c) and hashes it; it never talks to the
// Pico itself and never pushes anything.
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
 * KNOWN LIMITATION, not yet fixed (H1, same audit doc): `esp_blob` is
 * expected to be zones_config_export_blob()'s raw output, which is a whole-
 * struct memcpy of zones_cfg_t -- so this hash currently covers whatever
 * bytes the compiler put in that struct's padding. On THIS pass's actual
 * delivered surface (kiln_cfg_store.c saves/clones/applies -- no JSON
 * transport exists yet) that is harmless: the only "reconstruction" of
 * zones_cfg_t is the live, zero-initialized static s_zones.cfg
 * (zones_config_accessors.c) being memcpy'd, so padding is always zero and
 * the hash is fully reproducible for every comparison this pass actually
 * makes. It becomes UNSAFE the moment a package is rebuilt from JSON
 * (docs/KILN_PROFILES_PLAN.md items 3/4, upload) into a NOT-necessarily-
 * zeroed destination -- whoever implements that must either (a) add a
 * canonical, padding-free, field-by-field zones_cfg_t serializer (the
 * plan's own section 3.1.3 rule 3) built by walking the struct's OWN
 * declaration mechanically (never a hand-written, forgettable field list --
 * the same "un-forgettable memcpy" property this module's Pico-half
 * packing already has via CONFIG_PARAM_TABLE's enumerable count/get-by-
 * index), or (b) memset(0) every JSON-reconstructed zones_cfg_t before
 * populating it AND prove via a test that fills the destination with a
 * poison byte (0xA5) first that the resulting hash still matches a live
 * board's own compute -- see the audit doc's own negative-test note. Do
 * NOT ship upload/download against this function without one of those. */
bool kiln_package_compute_hash(uint16_t pkg_schema, const uint8_t *esp_blob, uint16_t esp_blob_len,
                                const kiln_pkg_safety_t *pico, uint32_t *out_hash);

#ifdef __cplusplus
}
#endif

#endif // KILN_PACKAGE_H
