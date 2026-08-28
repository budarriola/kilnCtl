#include "profiles_http.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crc.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "MAX31856.h"
#include "http_form.h"
#include "kiln_io.h"
#include "profile_feasibility.h"
#include "profiles_builtin.h"
#include "web_encoding.h"
#include "wifi_provision_http.h"
#include "zones_http.h"

static const char *TAG = "profiles_http";

#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_USED "prof_used"
/* "prof0".."prof7" -- see profile_nvs_key() below. */

/* profiles_nvs is the 2026-08-13 split target for fire profiles (see
 * partitions.csv and TODO.md 8.1) -- profiles are the one section of the old
 * default `nvs` partition's contents that grows with use, so they get their
 * own partition with the most headroom rather than sharing kiln_nvs with
 * zones/rules/relay_cycles/run_state. Each module manages its own migration
 * and partition init independently rather than assuming another module
 * already brought its partition up. NVS_DEFAULT_PART_NAME (from
 * nvs_flash.h, expands to "nvs") is the old, still-live home this module's
 * data used to persist to, kept readable for the one-time migration below
 * and for firmware rollback. */
#define PROFILES_NVS_PARTITION "profiles_nvs"

/* Bump whenever the on-flash per-slot layout (profile_persisted_t) changes,
 * OR whenever profile_t/profile_segment_t itself grows a field -- the latter
 * is the trap zones_http.c's ZONES_CFG_VERSION 6->7 fix (see that file's
 * header comment) exists to name explicitly: profile_t embeds
 * segments[PROFILE_MAX_SEGMENTS], an ARRAY of profile_segment_t, so growing
 * profile_segment_t itself (not just profile_t's tail) displaces every
 * element after the first -- the exact same "array element, not just the
 * struct" bug the zones fix caught. When that happens: add a new
 * profile_persisted_vN_t snapshot of the OLD layout below (never edit an
 * existing one), a case in expected_len_for_version(), and a
 * convert_profile_vN() that writes every field of a fresh current-format
 * profile_t by name (never a memcpy of one shape over another) -- same
 * pattern zones_http.c's convert_zone_v*()/convert_versioned_blob_to_current()
 * use.
 *
 * 1 -> 2: appended crc32 to profile_persisted_t. This is genuinely a new
 * on-flash SHAPE -- a real board's existing v1 blobs are
 * `{uint8_t version; profile_t profile;}`, sizeof(profile_persisted_t)
 * WITHOUT the crc32 tail, and expected_len_for_version() must keep answering
 * THAT exact size for version 1 forever, never sizeof(the current struct).
 * Getting this wrong was caught on real hardware: an earlier draft of this
 * fix computed expected_len_for_version(1) as sizeof(profile_persisted_t)
 * (i.e. INCLUDING the new crc32 field), which made every already-saved v1
 * blob "the wrong length for its claimed version" and silently wiped both of
 * a bench board's saved profiles on the very firmware meant to protect them
 * -- see profile_persisted_v1_t/convert_profile_v1() below, the exact same
 * mistake zones_http.c's ZONES_CFG_VERSION 6->7 comment already documents by
 * name for zone_cfg_t.
 *
 * 2 -> 3 (this pass, TODO relay/IO segments -- see profiles_http.h's
 * profile_seg_kind_t doc comment for the owner's request that drove this):
 * profile_segment_t itself grew four uint8_t fields (seg_kind/io_target/
 * io_state/io_blocking/io_leave_on_at_end). THIS is the exact trap this
 * comment has been warning about since v1->v2: profile_t embeds
 * segments[PROFILE_MAX_SEGMENTS] BY VALUE, so growing profile_segment_t
 * displaces every element after segment 0, not just the tail of the struct.
 * A real board's existing v1 AND v2 blobs both used the OLD, 12-byte
 * profile_segment_t (profile_t did not change shape between v1 and v2 --
 * only the persisted WRAPPER grew a crc32 tail then) -- so both frozen
 * snapshots below now point at profile_t_v2 (the old segment shape), NOT at
 * today's profile_t. Reusing today's profile_t for profile_persisted_v1_t,
 * the way the v1->v2 pass did (profile_t "hasn't changed shape between v1
 * and v2" was true THEN), would silently misinterpret every field of every
 * segment from element 1 onward on a real board's already-saved profiles --
 * exactly the "one struct assignment, wrong shape" data loss this file's own
 * header comment names as the hazard, just one version later than where it
 * was first caught. convert_profile_v1()/convert_profile_v2() below walk
 * every segment field-by-field for exactly this reason -- a struct
 * assignment or memcpy across the shape change is never safe again from this
 * version forward. */
#define PROFILE_VERSION 3

/* PROFILES_MAX_COUNT / PROFILE_NAME_MAX_LEN / PROFILE_MAX_SEGMENTS and the
 * profile_t/profile_segment_t layout now live in profiles_http.h --
 * profile_executor.c needs them too (via profiles_http_get()). */

/* Firmware sanity bounds, not real kiln-safety limits -- there is no
 * separate safety authority for firing profiles (TODO.md section 6, which
 * would run one, is unbuilt), so these exist only to reject obvious
 * garbage/typos before anything is stored. 1400C is comfortably above any
 * home-kiln cone this board's use case implies; a real ceramics kiln safety
 * limit would come from the kiln's own manufacturer data, not this file. */
#define PROFILE_TARGET_C_MIN 0.0f
#define PROFILE_TARGET_C_MAX 1400.0f
#define PROFILE_RAMP_C_PER_HR_MIN 0.0f
#define PROFILE_RAMP_C_PER_HR_MAX 1000.0f
#define PROFILE_DWELL_MIN_MAX 1440u /* 24h */

/* The 20%-margin warning rule, explicit in TODO.md section 5. */
#define PROFILE_RAMP_WARN_FRACTION 0.8f

/* TODO.md 10.6a: embedded pre-gzipped (CMakeLists.txt gzips it at configure
 * time before idf_component_register runs), hence the "_gz" in both the
 * filename and the symbol it generates. */
extern const uint8_t profiles_page_html_gz_start[] asm("_binary_profiles_page_html_gz_start");
extern const uint8_t profiles_page_html_gz_end[] asm("_binary_profiles_page_html_gz_end");

/* All 8 slots kept resident -- each is well under 200 bytes, so loading all
 * 8 at boot (rather than lazily per-request) is simpler and cheap enough
 * that the "only load what's used" optimization the header docstring
 * mentions as a design choice isn't worth the extra code path. The
 * prof_used bitmap still exists in NVS/RAM so a listing never has to probe
 * 8 keys to find out which exist. */
typedef struct {
    profile_t profiles[PROFILES_MAX_COUNT];
    uint8_t used_bitmap; /* bit N = slot N in use */
} profiles_state_t;

static profiles_state_t s_profiles;

/* On-flash per-slot layout, one per "profN" key. version-prefixed so a slot
 * can be told apart from a stale/rolled-back/corrupt one at load time --
 * see nvs_load_all_from(). profile_t itself (the payload) stays in
 * profiles_http.h unversioned; only the persisted wrapper carries the
 * version tag, since profile_executor.c consumes profile_t directly through
 * profiles_http_get() and has no business knowing about on-flash layout. */
typedef struct {
    uint8_t version;
    profile_t profile;
    /* esp_crc32_le() (same helper crash_report.c's compute_crc()/zones_http.c's
     * compute_zones_crc() use) over this whole struct with crc32 itself
     * zeroed, computed over a local copy so re-validating an already-loaded
     * slot never mutates it just by asking. Appended at the true tail --
     * covers the CURRENT-version blob only; there is no older layout yet to
     * lack one. */
    uint32_t crc32;
} profile_persisted_t;

/* EXACT snapshot of profile_segment_t / profile_t as they were BEFORE this
 * pass (2026-08-27) added the relay/IO segment fields -- what every
 * already-saved version-1 AND version-2 blob on a real board actually is on
 * flash today, since profile_t did not change shape between v1 and v2 (only
 * the persisted WRAPPER grew a crc32 tail then). Used ONLY to interpret a raw
 * blob whose length has already been checked (expected_len_for_version())
 * against the size of EXACTLY the matching persisted_vN struct below, before
 * a single byte is copied out of it or a field is read by name -- see
 * decode_profile_blob(). Never grown, edited, or reused for a later version;
 * the NEXT layout change gets its own new snapshot appended after this one,
 * never a change to this one. Same discipline as zones_http.c's
 * zone_cfg_v1_t/zones_cfg_v1_t etc. */
typedef struct {
    float target_c;
    float ramp_c_per_hr;
    uint32_t dwell_min;
} profile_segment_v2_t;

typedef struct {
    char name[PROFILE_NAME_MAX_LEN + 1];
    uint8_t zone_mask;
    uint8_t segment_count;
    profile_segment_v2_t segments[PROFILE_MAX_SEGMENTS];
} profile_t_v2;

typedef struct {
    uint8_t version;
    profile_t_v2 profile;
} profile_persisted_v1_t; /* v1: no crc32 tail, OLD (pre-relay/IO) segment shape */

typedef struct {
    uint8_t version;
    profile_t_v2 profile;
    uint32_t crc32;
} profile_persisted_v2_t; /* v2: crc32 tail, but STILL the OLD segment shape --
                            * this is the struct the v1->v2 pass called
                            * profile_persisted_t; frozen here under its own
                            * name now that a real current-format profile_t
                            * exists and is a different size. */

/* Per-version expected blob length, checked in decode_profile_blob() BEFORE a
 * single byte is copied out of a stored blob or interpreted as any field --
 * same discipline as zones_http.c's expected_len_for_version(). A stored blob
 * whose length does not match the size EXACTLY implied by its own claimed
 * version is corrupt and is rejected outright, never partially repaired.
 * Returns 0 for a version this build has no known layout for -- 0 itself is
 * never valid (there is no "version 0" struct, historical or current), so
 * that value falls straight into the same rejection as any other unknown
 * version rather than needing a special case.
 *
 * version 1 answers sizeof(profile_persisted_v1_t) -- the HISTORICAL
 * pre-crc32 layout -- not sizeof(profile_persisted_t): a v1 blob on a real
 * board was written by firmware that never had a crc32 field, and is
 * genuinely that many bytes shorter. Answering the current struct's size
 * here would reject every already-saved v1 profile as "wrong length for its
 * claimed version" and wipe it -- exactly the data-loss regression this
 * comment exists to prevent a repeat of (found on bench hardware during this
 * pass; see PROFILE_VERSION's own comment). */
static size_t expected_len_for_version(uint8_t version)
{
    switch (version) {
    case 1: return sizeof(profile_persisted_v1_t);
    case 2: return sizeof(profile_persisted_v2_t);
    case PROFILE_VERSION: return sizeof(profile_persisted_t);
    default: return 0;
    }
}

/* Field-by-field converter, OLD (v1/v2, pre-relay/IO) segment shape ->
 * current profile_t. Deliberately walks every one of the up to
 * PROFILE_MAX_SEGMENTS elements by name rather than a struct assignment or
 * memcpy: profile_t embeds segments[] BY VALUE, so a bulk copy across the
 * v2_t -> current shape change would silently misinterpret every segment
 * from element 1 onward (see PROFILE_VERSION's own comment for the data-loss
 * this exact mistake caused on a real board, one version earlier). The four
 * new fields are given their SAFE, "this was always a temperature segment"
 * defaults -- an old profile never had a relay/IO segment in it, so
 * PROFILE_SEG_KIND_ZONE_RAMP with everything else zeroed reproduces its old
 * behavior exactly, and io_leave_on_at_end's default of 0 is the same
 * fail-off default a brand new segment gets. */
static void convert_profile_v2_segments(const profile_segment_v2_t *src, uint8_t count, profile_t *out)
{
    for (uint8_t i = 0; i < count && i < PROFILE_MAX_SEGMENTS; i++) {
        out->segments[i].target_c = src[i].target_c;
        out->segments[i].ramp_c_per_hr = src[i].ramp_c_per_hr;
        out->segments[i].dwell_min = src[i].dwell_min;
        out->segments[i].seg_kind = PROFILE_SEG_KIND_ZONE_RAMP;
        out->segments[i].io_target = PROFILE_IO_TARGET_NONE;
        out->segments[i].io_state = 0;
        out->segments[i].io_blocking = 0;
        out->segments[i].io_leave_on_at_end = 0; /* fail-off default, never inherited as "on" */
    }
}

static void convert_profile_v1(const profile_persisted_v1_t *src, profile_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->name, src->profile.name, sizeof(out->name) - 1);
    out->zone_mask = src->profile.zone_mask;
    out->segment_count = src->profile.segment_count;
    convert_profile_v2_segments(src->profile.segments, src->profile.segment_count, out);
}

/* Same shape conversion as convert_profile_v1() above -- v1 and v2 share the
 * identical profile_t_v2 payload (only the persisted WRAPPER differs, by the
 * crc32 tail), so this is convert_profile_v1() in every respect except which
 * persisted_vN_t it reads from. Kept as its own named function rather than
 * folded into the v1 one anyway, same reasoning zones_http.c's
 * convert_zone_v*() functions are never collapsed into each other: the day
 * v2's payload diverges from v1's (it hasn't yet) this is the one place that
 * has to change without touching the v1 path. */
static void convert_profile_v2(const profile_persisted_v2_t *src, profile_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->name, src->profile.name, sizeof(out->name) - 1);
    out->zone_mask = src->profile.zone_mask;
    out->segment_count = src->profile.segment_count;
    convert_profile_v2_segments(src->profile.segments, src->profile.segment_count, out);
}

/* esp_crc32_le() over `p` with crc32 zeroed -- the one place this file
 * computes a profile slot's CRC, used both to stamp it at save time and to
 * check it at load time. */
static uint32_t compute_profile_crc(const profile_persisted_t *p)
{
    profile_persisted_t tmp = *p;
    tmp.crc32 = 0;
    return esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
}

typedef enum {
    PROFILE_DECODE_OK,      /* *out is a valid, current-format profile_t, ready to adopt */
    PROFILE_DECODE_CORRUPT, /* reject outright: version 0, wrong length for the claimed
                             * version, unknown version, or CRC mismatch -- *out is
                             * zeroed, nothing is adopted */
    PROFILE_DECODE_NEWER,   /* version > PROFILE_VERSION -- refuse without guessing;
                             * *out is zeroed, but the caller must leave the SOURCE
                             * bytes untouched (see nvs_load_all_from()) */
} profile_decode_result_t;

/* The one place a stored profile blob is turned into a trustworthy,
 * current-format profile_t -- mirrors zones_http.c's decode_zones_blob()
 * field for field: a length check against the blob's OWN claimed version
 * before anything is copied or interpreted, and a CRC check for the
 * current-version case. This is also what makes `version == 0` -- the bug
 * TODO.md calls out, where the old code accepted it "at any length >= 1" and
 * installed it as a used slot -- impossible to reach ANY installed slot: 0
 * is neither PROFILE_VERSION nor greater than it, so it falls straight into
 * the "unknown version" branch below and is rejected before a single byte of
 * the payload is looked at. */
static profile_decode_result_t decode_profile_blob(const void *blob, size_t len, profile_t *out,
                                                    const char **err_reason)
{
    static const char *unused_reason;
    const char **reason = err_reason ? err_reason : &unused_reason;
    *reason = "";
    memset(out, 0, sizeof(*out));

    if (!blob || len < sizeof(((profile_persisted_t *)0)->version)) {
        *reason = "blob missing or too short to contain a version";
        return PROFILE_DECODE_CORRUPT;
    }
    uint8_t version = ((const uint8_t *)blob)[0];

    if (version == 0) {
        /* The exact bug TODO.md names: version 0 must never be treated as a
         * usable (if unusual) layout. There has never been, and will never
         * be, a "version 0" struct -- reject it exactly like any other
         * version this build doesn't recognize, before anything past this
         * byte is read. */
        *reason = "version 0 is never a valid profile layout";
        return PROFILE_DECODE_CORRUPT;
    }

    if (version > PROFILE_VERSION) {
        /* Firmware-rollback case, same as zones_http.c's decode_zones_blob():
         * this build does not know that layout and must not guess at it.
         * Deliberately does NOT check length against anything here -- an
         * unknown newer layout could be any size. */
        *reason = "this profile was saved by newer firmware -- refusing rather than guessing";
        return PROFILE_DECODE_NEWER;
    }

    size_t expected = expected_len_for_version(version);
    if (expected == 0) {
        *reason = "unknown/unsupported profile version";
        return PROFILE_DECODE_CORRUPT;
    }
    if (len != expected) {
        *reason = "blob length does not match its claimed version -- treating as corrupt";
        return PROFILE_DECODE_CORRUPT;
    }

    if (version == PROFILE_VERSION) {
        profile_persisted_t loaded;
        memcpy(&loaded, blob, sizeof(loaded));
        uint32_t stored_crc = loaded.crc32;
        uint32_t computed_crc = compute_profile_crc(&loaded);
        if (computed_crc != stored_crc) {
            *reason = "CRC mismatch -- treating as corrupt";
            return PROFILE_DECODE_CORRUPT;
        }
        *out = loaded.profile;
        return PROFILE_DECODE_OK;
    }

    if (version == 2) {
        /* Historical, pre-crc32-tail-but-has-one, pre-relay/IO-segments
         * layout. v2 DOES have a crc32 (unlike v1), so check it the same way
         * the current-version branch above does -- there is no reason a v2
         * blob deserves a weaker integrity gate than v3 just because it is
         * older. */
        profile_persisted_v2_t loaded_v2;
        memcpy(&loaded_v2, blob, sizeof(loaded_v2));
        uint32_t stored_crc = loaded_v2.crc32;
        profile_persisted_v2_t tmp = loaded_v2;
        tmp.crc32 = 0;
        uint32_t computed_crc = esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
        if (computed_crc != stored_crc) {
            *reason = "CRC mismatch -- treating as corrupt";
            return PROFILE_DECODE_CORRUPT;
        }
        convert_profile_v2(&loaded_v2, out);
        return PROFILE_DECODE_OK;
    }

    /* version == 1 here (the only other case expected_len_for_version()
     * currently answers non-zero for) -- the historical, pre-crc32 layout.
     * No CRC to check: v1 blobs never had one, so length-matches-claimed-
     * version plus this typed conversion IS its integrity gate, same as
     * zones_http.c's older-than-crc versions. */
    profile_persisted_v1_t loaded_v1;
    memcpy(&loaded_v1, blob, sizeof(loaded_v1));
    convert_profile_v1(&loaded_v1, out);
    return PROFILE_DECODE_OK;
}

/* application/x-www-form-urlencoded whole-profile submit: id, name, zone,
 * seg_count, plus 3 fields per segment across up to 12 segments. Generous
 * headroom over a legitimate 12-segment submission, checked against
 * Content-Length before a single byte is read. */
#define PROFILE_BODY_MAX 2048

static void profile_nvs_key(uint8_t id, char *out, size_t out_cap)
{
    snprintf(out, out_cap, "prof%u", id);
}

/* ---- NVS ---------------------------------------------------------------- */

static esp_err_t nvs_save_slot(uint8_t id);

/* Brings up one NVS partition, erasing ONLY that partition if its contents
 * are unusable. Copied/adapted from wifi_prov.c's nvs_partition_init() (see
 * that file for the full rationale) -- NO_FREE_PAGES / NEW_VERSION_FOUND
 * have no other cure, so erasing is the only way forward, but the erase
 * must stay scoped to the partition that is actually broken rather than
 * blast-radius the default partition (or any other split-off partition)
 * with it. */
static esp_err_t nvs_partition_init(const char *partition)
{
    esp_err_t err = nvs_flash_init_partition(partition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition '%s' needs erase (%s) -- erasing THAT PARTITION ONLY and retrying",
                 partition, esp_err_to_name(err));
        err = nvs_flash_erase_partition(partition);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(partition);
        }
    }
    return err;
}

/* Loads NVS_NAMESPACE/NVS_KEY_USED + "profN" out of `partition` into *out,
 * applying a three-outcome version check to EACH slot independently: a
 * failed read, wrong blob size, or unrecognized version marks only that one
 * slot unused -- one bad slot must never take any other slot down with it
 * (TODO.md 8.1's explicit "a failed profile migration must not block"
 * requirement). *out_any_found reports whether the used-bitmap key existed
 * at all (vs. existing but empty/unreadable), which the one-time migration
 * below keys off. */
static esp_err_t nvs_load_all_from(const char *partition, profiles_state_t *out, bool *out_any_found)
{
    memset(out, 0, sizeof(*out));
    if (out_any_found) {
        *out_any_found = false;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(partition, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* no kiln_cfg namespace on this partition yet -- nothing configured */
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t bitmap = 0;
    err = nvs_get_u8(h, NVS_KEY_USED, &bitmap);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return err;
    }
    if (err == ESP_OK) {
        out->used_bitmap = bitmap;
        if (out_any_found) {
            *out_any_found = true;
        }
    }

    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        if (!(out->used_bitmap & (1u << id))) {
            continue;
        }
        char key[8];
        profile_nvs_key(id, key, sizeof(key));
        profile_persisted_t loaded;
        size_t len = sizeof(loaded);
        esp_err_t slot_err = nvs_get_blob(h, key, &loaded, &len);
        if (slot_err != ESP_OK) {
            ESP_LOGW(TAG, "prof%u load from '%s' failed (%s) -- marking unused", id, partition,
                     esp_err_to_name(slot_err));
            out->used_bitmap &= ~(1u << id);
            continue;
        }
        /* decode_profile_blob() is the ONE place a stored blob is checked
         * before any byte of it is interpreted -- a length check against the
         * blob's OWN claimed version, then (for the current version) a CRC
         * check. This is the fix for the TODO.md bug: `version == 0` used to
         * be accepted "at any length >= 1" and installed as a used slot; it
         * now falls into decode_profile_blob()'s explicit "version 0 is
         * never valid" rejection before anything past that byte is read. A
         * failed decode marks only THIS slot unused -- one bad slot must
         * never take any other slot down with it (TODO.md 8.1). The NEWER
         * case additionally leaves the flash bytes untouched, matching
         * zones_http.c's decode_zones_blob()/nvs_load_from() convention: a
         * slot written by newer firmware may hold a layout this build
         * cannot interpret, and wiping it would destroy data a roll-forward
         * (or the newer firmware itself) still needs. */
        profile_t decoded;
        const char *reason = "";
        profile_decode_result_t dres = decode_profile_blob(&loaded, len, &decoded, &reason);
        switch (dres) {
        case PROFILE_DECODE_OK:
            out->profiles[id] = decoded;
            break;
        case PROFILE_DECODE_NEWER:
            ESP_LOGW(TAG, "prof%u load from '%s' refused: %s", id, partition, reason);
            out->used_bitmap &= ~(1u << id);
            continue;
        case PROFILE_DECODE_CORRUPT:
        default:
            ESP_LOGW(TAG, "prof%u load from '%s' rejected: %s -- marking unused", id, partition, reason);
            out->used_bitmap &= ~(1u << id);
            continue;
        }
    }

    nvs_close(h);
    return ESP_OK;
}

static esp_err_t nvs_save_slot(uint8_t id)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[8];
    profile_nvs_key(id, key, sizeof(key));
    profile_persisted_t persisted = {
        .version = PROFILE_VERSION,
        .profile = s_profiles.profiles[id],
        .crc32 = 0,
    };
    persisted.crc32 = compute_profile_crc(&persisted);
    err = nvs_set_blob(h, key, &persisted, sizeof(persisted));
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_USED, s_profiles.used_bitmap);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_erase_slot(uint8_t id)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PROFILES_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char key[8];
    profile_nvs_key(id, key, sizeof(key));
    esp_err_t erase_err = nvs_erase_key(h, key);
    if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return erase_err;
    }
    err = nvs_set_u8(h, NVS_KEY_USED, s_profiles.used_bitmap);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* One-time move of persisted profiles out of the default partition's
 * NVS_NAMESPACE/"profN" keys and into PROFILES_NVS_PARTITION's, for boards
 * provisioned by firmware predating the 2026-08-13 split. Simplified from
 * wifi_prov.c's migrate_from_default_partition() the same way
 * rules_http.c's is: profiles_nvs is only ever consulted first, so if it
 * already has anything there is nothing to migrate and no "which wins"
 * question to answer -- only the old default-partition location could hold
 * pre-migration data.
 *
 * Migrated slot-by-slot rather than as one operation: a failure migrating
 * one slot must not stop the others (TODO.md 8.1's explicit requirement).
 * Old default-nvs blobs predate the version field entirely (they are a bare
 * profile_t, not a profile_persisted_t) -- this migration IS the event that
 * introduces struct versioning for profiles, so each slot found is stamped
 * with the current PROFILE_VERSION as it's carried across. The old copies
 * are deliberately left in place (not deleted), same rationale as
 * wifi_prov.c/rules_http.c: they need to still be there if someone rolls
 * back to pre-split firmware. */
static void migrate_from_default_partition(void)
{
    nvs_handle_t old_h;
    esp_err_t err = nvs_open_from_partition(NVS_DEFAULT_PART_NAME, NVS_NAMESPACE, NVS_READONLY, &old_h);
    if (err != ESP_OK) {
        return; /* no kiln_cfg namespace on the default partition -- nothing to migrate */
    }

    uint8_t old_bitmap = 0;
    err = nvs_get_u8(old_h, NVS_KEY_USED, &old_bitmap);
    if (err != ESP_OK || old_bitmap == 0) {
        nvs_close(old_h);
        return; /* nothing recorded as used in the old location */
    }

    ESP_LOGI(TAG, "migrating fire profiles from the default NVS partition to '%s'", PROFILES_NVS_PARTITION);

    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        if (!(old_bitmap & (1u << id))) {
            continue;
        }
        char key[8];
        profile_nvs_key(id, key, sizeof(key));

        /* Old (pre-split, pre-version) blobs are a bare profile_t -- no
         * version prefix ever existed for them. */
        profile_t old_profile;
        size_t len = sizeof(old_profile);
        esp_err_t slot_err = nvs_get_blob(old_h, key, &old_profile, &len);
        if (slot_err != ESP_OK || len != sizeof(old_profile)) {
            ESP_LOGW(TAG,
                     "prof%u migration read failed or wrong size (%s) -- skipping this slot, others still "
                     "attempted",
                     id, esp_err_to_name(slot_err));
            continue;
        }

        s_profiles.profiles[id] = old_profile;
        s_profiles.used_bitmap |= (1u << id);
        esp_err_t save_err = nvs_save_slot(id);
        if (save_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "prof%u migration write to '%s' failed: %s -- running from the old copy this boot, will "
                     "retry",
                     id, PROFILES_NVS_PARTITION, esp_err_to_name(save_err));
            /* Don't let a failed write claim the slot as migrated in RAM --
             * a write failure on this slot must not affect any other. */
            s_profiles.used_bitmap &= ~(1u << id);
            memset(&s_profiles.profiles[id], 0, sizeof(s_profiles.profiles[id]));
        }
    }

    nvs_close(old_h);
}

/* ---- Public getter (profile_executor.c) ----------------------------------- */

bool profiles_http_get(uint8_t id, profile_t *out)
{
    if (!out) {
        return false;
    }

    /* Builtin catalogue ids (>= PROFILE_BUILTIN_ID_BASE) resolve here too, so
     * that every existing consumer of this getter -- profile_executor_run()
     * above all -- can run a shipped schedule with no new code path and no
     * new id validation to get wrong. The id ranges do not overlap by
     * construction (128 vs 0..7), so there is no ambiguity to resolve.
     *
     * A hidden entry is still returned: hiding is a listing preference, not a
     * deletion, and an in-flight or stored reference to one must not dangle.
     *
     * zone_mask: the catalogue is zone-agnostic and profiles_builtin_get()
     * therefore leaves the mask 0. Filling in every configured zone here is
     * the only sensible reading of "run this schedule" on a board whose
     * zones are already declared on the Zones page, and it keeps the
     * executor untouched. A user who wants a subset saves a copy into a
     * slot (see profiles_http_save) and edits the mask there.
     *
     * This returns TRUE with a zero mask when no zones are configured yet.
     * It used to return false, which made every caller report "no such
     * profile" for a schedule that plainly exists and is plainly listed --
     * the user-visible bug. A getter's answer to "does this profile exist"
     * must not depend on whether a DIFFERENT subsystem has been configured;
     * zone configuration is the executor's business, and
     * profile_executor_run() already has an accurate message for a zero mask
     * (see its "targets no zones" branch). The other three callers all cope:
     * the UART LIST/GET paths simply report the schedule with mask 0 (which
     * is what a zone-less board honestly has), and readiness_http.c only
     * ever passes user-slot ids, which never reach this branch. */
    if (profiles_builtin_id_valid(id)) {
        if (!profiles_builtin_get(id, out)) {
            return false;
        }
        uint8_t n = zones_config_get_thermo_count();
        out->zone_mask = (n >= 8) ? 0xFFu : (uint8_t)((1u << n) - 1u);
        return true;
    }

    if (id >= PROFILES_MAX_COUNT || !(s_profiles.used_bitmap & (1u << id))) {
        return false;
    }
    *out = s_profiles.profiles[id];
    return true;
}

/* ---- UART bridge entry points (uart_bridge_ext.c) -------------------------
 *
 * Same range/feasibility validation and NVS commit as profile_post_handler()/
 * profile_delete_post_handler() below, factored out so the UART CONTROL
 * bridge doesn't have to re-implement (and risk drifting from) this file's
 * one copy of "what makes a profile valid." The HTTP handlers keep their own
 * string-parsing step (http_form_find_field + strtof/strtol) since the UART
 * side already hands over a decoded profile_t -- everything downstream of
 * that parse is shared. */

/* Owner's design rule, the exact one rules_http.c's check_relay_not_zone_owned()
 * already enforces for RULE-driven relays (that file is untouched by this pass
 * -- see profiles_http.h's profile_seg_kind_t comment): a relay already
 * assigned to a zone's heater output must never ALSO be reachable as a
 * profile segment target -- a segment turning it on/off would fight (or
 * silently lose to) that zone's own PID/bang-bang control of the same
 * contact. This is an independent copy of the same check, not a shared call
 * into rules_http.c: this file owns profile validation and rules_http.c owns
 * rule validation, and neither may depend on the other (rules_*.* is deleted
 * in a later task; this file must keep working the day that happens, same as
 * rules_http.c's own comment already notes about zones_config_get_relay_mask()
 * being read fresh every check, never cached, since a relay can be
 * (re)assigned to a zone at any time from the Thermocouples & Zones page).
 * relay_1_4 is 1-based, matching kiln_io_set_relay()'s convention. Returns
 * true (refuse) if ANY configured zone currently claims this relay. */
static bool profile_relay_is_zone_owned(uint8_t relay_1_4, uint8_t *out_zone_index)
{
    uint8_t bit = (uint8_t)(1u << (relay_1_4 - 1u));
    uint8_t zone_count = zones_config_get_thermo_count();
    for (uint8_t zi = 0; zi < zone_count; zi++) {
        uint8_t zone_mask = 0;
        if (!zones_config_get_relay_mask(zi, &zone_mask)) {
            continue;
        }
        if ((zone_mask & bit) != 0) {
            if (out_zone_index) *out_zone_index = zi;
            return true;
        }
    }
    return false;
}

/* Validates one RELAY_IO segment's target/flags -- the SAVE-TIME half of the
 * "two independent checks, deliberately" the owner's IO-side gate needs. The
 * second is profile_executor.c's own re-check at run start (relay_io_target_
 * is_zone_owned() in that file), for the same reason profile_post_handler's
 * feasibility check is re-run at run start too: a relay can be reassigned to
 * a zone AFTER a profile was saved, same reload-time hazard zones_http.c's
 * relay_mask comment and rules_task.c's compute_heater_relay_mask() both
 * already document. Only meaningful for seg->seg_kind ==
 * PROFILE_SEG_KIND_RELAY_IO -- callers check the kind first. */
static bool validate_io_segment(const profile_segment_t *seg, uint8_t seg_num, char *err_msg, size_t err_cap)
{
    uint8_t t = seg->io_target;
    bool is_relay = (t >= PROFILE_IO_TARGET_RELAY_BASE) && (t < PROFILE_IO_TARGET_RELAY_BASE + KILN_IO_RELAY_COUNT);
    bool is_io = (t >= PROFILE_IO_TARGET_IO_BASE) && (t < PROFILE_IO_TARGET_IO_BASE + KILN_IO_DIGITAL_COUNT);
    if (!is_relay && !is_io) {
        /* Covers PROFILE_IO_TARGET_NONE, the deliberate dead gap between the
         * two ranges (would-be DRDY/LCD encodings -- see profiles_http.h),
         * and anything past either range -- all rejected the same way,
         * before this value is ever turned into a kiln_io call. This is the
         * refusal the investigation found missing: kiln_io_set_io()'s own
         * index parameter structurally can't reach IO8-10 (~DRDY) or IO14-15
         * (LCD_IORQ/LCD_Reset) either (it only accepts 1-7), but that
         * structural limit lives in a driver two layers away from a saved
         * profile and must not be the ONLY thing standing between a bad
         * io_target value and those lines -- this gate is the explicit,
         * named one, checked before a value is ever handed to that driver. */
        snprintf(err_msg, err_cap,
                "segment %u: io_target %u is not a valid relay (1-%u) or IO (%u-%u) target",
                seg_num, t, (unsigned)KILN_IO_RELAY_COUNT, (unsigned)PROFILE_IO_TARGET_IO_BASE,
                (unsigned)(PROFILE_IO_TARGET_IO_BASE + KILN_IO_DIGITAL_COUNT - 1u));
        return false;
    }
    if (is_relay) {
        uint8_t owning_zone = 0;
        if (profile_relay_is_zone_owned(t, &owning_zone)) {
            snprintf(err_msg, err_cap,
                    "segment %u: relay %u is assigned to zone %u -- only relays not owned by any "
                    "zone can be a segment target",
                    seg_num, t, owning_zone);
            return false;
        }
    }
    if (seg->io_leave_on_at_end && (seg->io_blocking || !seg->io_state)) {
        /* "Leave it on at run end" is nonsensical for a segment that isn't
         * commanding the relay/IO ON in the first place, and for a BLOCKING
         * segment the schedule has already waited for it and moved past it
         * by the time the run could possibly end mid-segment -- there is no
         * "still running when the profile ends" case for a blocking segment
         * to leave anything in. Rejected rather than silently ignored, same
         * as every other malformed-combination gate in this file. */
        snprintf(err_msg, err_cap,
                "segment %u: leave-on-at-end only applies to a non-blocking segment commanding the "
                "relay/IO ON",
                seg_num);
        return false;
    }
    return true;
}

bool profiles_http_save(uint8_t requested_id, const profile_t *candidate, uint8_t *out_id,
                        uint8_t *out_warning_count, char *err_msg, size_t err_cap)
{
    if (!candidate || !out_id) {
        if (err_msg) snprintf(err_msg, err_cap, "internal error");
        return false;
    }
    if (candidate->segment_count < 1 || candidate->segment_count > PROFILE_MAX_SEGMENTS) {
        snprintf(err_msg, err_cap, "seg_count out of range (1-12)");
        return false;
    }
    uint8_t thermo_count = zones_config_get_thermo_count();
    uint8_t valid_zone_bits = thermo_count >= 8 ? 0xFF : (uint8_t)((1u << thermo_count) - 1u);
    if (candidate->zone_mask == 0 || (candidate->zone_mask & (uint8_t)~valid_zone_bits) != 0) {
        snprintf(err_msg, err_cap,
                "zone_mask must select at least one configured zone (check Thermocouples & Zones settings)");
        return false;
    }
    for (uint8_t i = 0; i < candidate->segment_count; i++) {
        const profile_segment_t *seg = &candidate->segments[i];
        if (seg->seg_kind == PROFILE_SEG_KIND_RELAY_IO) {
            if (!validate_io_segment(seg, i + 1, err_msg, err_cap)) {
                return false;
            }
            continue; /* target_c/ramp_c_per_hr are not meaningful for this kind */
        }
        if (seg->seg_kind != PROFILE_SEG_KIND_ZONE_RAMP) {
            snprintf(err_msg, err_cap, "segment %u: unknown segment kind %u", i + 1, seg->seg_kind);
            return false;
        }
        if (isnan(seg->target_c) || seg->target_c < PROFILE_TARGET_C_MIN || seg->target_c > PROFILE_TARGET_C_MAX) {
            snprintf(err_msg, err_cap, "segment %u: target_c out of range (0-1400)", i + 1);
            return false;
        }
        if (isnan(seg->ramp_c_per_hr) || seg->ramp_c_per_hr < PROFILE_RAMP_C_PER_HR_MIN ||
            seg->ramp_c_per_hr > PROFILE_RAMP_C_PER_HR_MAX) {
            snprintf(err_msg, err_cap, "segment %u: ramp_c_per_hr out of range (0-1000)", i + 1);
            return false;
        }
        if (seg->dwell_min > PROFILE_DWELL_MIN_MAX) {
            snprintf(err_msg, err_cap, "segment %u: dwell_min out of range (0-1440)", i + 1);
            return false;
        }
    }

    /* SAVE-VS-COPY, for a save aimed at a builtin catalogue id: the catalogue
     * lives in .rodata and cannot be written, so "overwrite it" is not a
     * thing that can happen. Rather than fail, this redirects to "save a copy
     * into a user slot" -- which is exactly what the existing API shape
     * already does with any id >= PROFILES_MAX_COUNT ("first free slot"), so
     * builtin ids need no special case to land on the right behaviour, only
     * this note saying it is deliberate. The caller learns the real slot from
     * *out_id, so nothing is silent about it. */
    uint8_t target_id;
    if (requested_id < PROFILES_MAX_COUNT) {
        target_id = requested_id;
    } else {
        int free_slot = -1;
        for (uint8_t i = 0; i < PROFILES_MAX_COUNT; i++) {
            if (!(s_profiles.used_bitmap & (1u << i))) {
                free_slot = i;
                break;
            }
        }
        if (free_slot < 0) {
            snprintf(err_msg, err_cap, "profile storage full");
            return false;
        }
        target_id = (uint8_t)free_slot;
    }

    /* Feasibility check (TODO.md section 5), same rule profile_post_handler
     * runs: every participating zone's max-ramp ceiling must accommodate
     * every ramped segment, or the whole submission is rejected. */
    uint8_t warn_count = 0;
    for (uint8_t i = 0; i < candidate->segment_count; i++) {
        if (candidate->segments[i].seg_kind != PROFILE_SEG_KIND_ZONE_RAMP) {
            continue; /* a relay/IO segment has no ramp rate to check against a zone's ceiling */
        }
        float rate = candidate->segments[i].ramp_c_per_hr;
        if (rate <= 0.0f) {
            continue;
        }
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!(candidate->zone_mask & (1u << zi))) {
                continue;
            }
            float ceiling = 0.0f;
            zones_config_get_max_ramp(zi, &ceiling);
            if (rate > ceiling) {
                snprintf(err_msg, err_cap, "segment %u: ramp rate %.1f C/hr exceeds zone %u's %.1f C/hr ceiling",
                        i + 1, (double)rate, zi, (double)ceiling);
                return false;
            }
            if (rate > PROFILE_RAMP_WARN_FRACTION * ceiling) {
                warn_count++;
            }
        }
    }

    s_profiles.profiles[target_id] = *candidate;
    s_profiles.used_bitmap |= (1u << target_id);
    esp_err_t err = nvs_save_slot(target_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_slot(%u) failed: %s -- profile applied live but will not survive a reboot",
                 target_id, esp_err_to_name(err));
        /* Still applied -- same convention as profile_post_handler(). */
    }

    *out_id = target_id;
    if (out_warning_count) {
        *out_warning_count = warn_count;
    }
    return true;
}

bool profiles_http_delete(uint8_t id)
{
    /* A builtin is read-only and cannot be deleted -- it is a const table in
     * flash. The user-facing equivalent is hiding it
     * (POST /api/profile/builtin/hide), which is reversible; see
     * profiles_builtin.h. Refuse rather than pretend. */
    if (profiles_builtin_id_valid(id)) {
        return false;
    }
    if (id >= PROFILES_MAX_COUNT || !(s_profiles.used_bitmap & (1u << id))) {
        return false;
    }
    s_profiles.used_bitmap &= ~(1u << id);
    memset(&s_profiles.profiles[id], 0, sizeof(s_profiles.profiles[id]));
    esp_err_t err = nvs_erase_slot((uint8_t)id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_slot(%u) failed: %s -- deleted live but may reappear after reboot", id,
                 esp_err_to_name(err));
    }
    return true;
}

void profiles_http_get_bounds(float *out_target_c_min, float *out_target_c_max,
                              float *out_ramp_c_per_hr_min, float *out_ramp_c_per_hr_max,
                              uint32_t *out_dwell_min_max)
{
    if (out_target_c_min) *out_target_c_min = PROFILE_TARGET_C_MIN;
    if (out_target_c_max) *out_target_c_max = PROFILE_TARGET_C_MAX;
    if (out_ramp_c_per_hr_min) *out_ramp_c_per_hr_min = PROFILE_RAMP_C_PER_HR_MIN;
    if (out_ramp_c_per_hr_max) *out_ramp_c_per_hr_max = PROFILE_RAMP_C_PER_HR_MAX;
    if (out_dwell_min_max) *out_dwell_min_max = PROFILE_DWELL_MIN_MAX;
}

/* ---- HTML page ------------------------------------------------------------ */

/* TODO.md 10.6a: content negotiation lives in web_encoding.h's shared
 * web_client_accepts_gzip() -- absent Accept-Encoding is legal and served
 * gzip (RFC 9110 s12.5.3); a header that explicitly excludes gzip gets an
 * uncompressed 406 rather than a body it cannot decode. */
static esp_err_t page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "profiles_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)profiles_page_html_gz_start,
                           (size_t)(profiles_page_html_gz_end - profiles_page_html_gz_start));
}

/* ---- JSON ------------------------------------------------------------------ */

static void json_escape(const char *src, char *out, size_t out_cap)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < out_cap; p++) {
        if (*p == '"' || *p == '\\') {
            if (o + 3 >= out_cap) {
                break;
            }
            out[o++] = '\\';
        }
        out[o++] = *p;
    }
    out[o] = '\0';
}

/* ---- Builtin catalogue JSON ------------------------------------------------
 *
 * RESPONSE-SIZE FINDING, which is why the catalogue gets its own endpoint
 * rather than being appended to GET /api/profile's detail object:
 *   - The UART CONTROL bridge caps a reply at BRIDGE_REPLY_MAX, which is
 *     UART_PROTO_MAX_PAYLOAD (uart_bridge.c) -- a few hundred bytes. Nothing
 *     resembling a catalogue fits through it, so the catalogue is HTTP-only
 *     and the UART profile commands are left alone entirely.
 *   - Every existing JSON handler in this file builds into ONE stack buffer
 *     and snprintf-truncates on overflow (see the APPEND macros). The
 *     catalogue is 28 entries x up to 12 segments plus a title and slug --
 *     roughly 25 KB. That is far past any sane stack buffer on this target,
 *     so this endpoint is the one place in the file that streams with
 *     httpd_resp_send_chunk() and reuses a single ~1 KB per-entry buffer.
 *     Building the whole thing in one buffer would have silently truncated
 *     the tail of the catalogue, which is the failure mode most likely to go
 *     unnoticed until a schedule is missing on the page.
 *
 * GET /api/profiles keeps its existing single-buffer shape but is likewise
 * chunked now, because it lists the catalogue's summaries alongside the user
 * slots.
 */

/* Escapes into a caller buffer and returns it, for use inline in a printf
 * argument list. */
static const char *esc(const char *src, char *buf, size_t cap)
{
    json_escape(src, buf, cap);
    return buf;
}

/* Sends one snprintf'd chunk, honouring the one thing snprintf's return value
 * is easy to get wrong: on truncation it reports the length it WOULD have
 * written, which is larger than the buffer. Passing that straight to
 * httpd_resp_send_chunk() reads past the end of the buffer. The worst-case
 * field widths in the builtin JSON below (fixed text + escaped code + a
 * 127-char title + two copies of the slug) add up to more than the 384-byte
 * chunk buffer, so this is reachable the day someone adds a longer title --
 * and the table those titles live in is generated, so that is a plausible
 * edit rather than a theoretical one.
 *
 * Truncation also means the JSON is malformed, which a clamp alone would hide,
 * so it is logged rather than silently shortened. */
static esp_err_t send_chunk_checked(httpd_req_t *req, const char *buf, int n, size_t cap, const char *what)
{
    if (n < 0) {
        return ESP_OK; /* encoding error -- skip this fragment, keep the response alive */
    }
    if ((size_t)n >= cap) {
        ESP_LOGW(TAG, "%s JSON truncated at %u bytes -- response will be malformed", what,
                 (unsigned)cap);
        n = (int)(cap - 1);
    }
    return httpd_resp_send_chunk(req, buf, (size_t)n);
}

/* Appends one builtin entry's summary (no segments) to a chunked response. */
static esp_err_t send_builtin_summary(httpd_req_t *req, uint8_t id, const builtin_profile_t *b, bool first)
{
    profile_t p;
    profile_seg_verdict_t rollup = PROFILE_SEG_UNKNOWN;
    if (profiles_builtin_get(id, &p)) {
        rollup = profile_feasibility_profile_mask(0, &p, NULL, 0);
    }

    char code_e[PROFILE_NAME_MAX_LEN * 2 + 1];
    char title_e[128];
    char slug_e[64];
    char chunk[384];
    int n = snprintf(chunk, sizeof(chunk),
                     "%s{\"id\":%u,\"builtin\":true,\"name\":\"%s\",\"code\":\"%s\",\"title\":\"%s\","
                     "\"slug\":\"%s\",\"url\":\"https://digitalfire.com/schedule/%s\",\"hidden\":%s,"
                     "\"zone_mask\":0,\"segment_count\":%u,\"feasibility\":\"%s\"}",
                     first ? "" : ",", id, esc(b->code, code_e, sizeof(code_e)),
                     esc(b->code, code_e, sizeof(code_e)), esc(b->title, title_e, sizeof(title_e)),
                     esc(b->slug, slug_e, sizeof(slug_e)), b->slug,
                     profiles_builtin_is_hidden(id) ? "true" : "false", b->segment_count,
                     profile_feasibility_verdict_str(rollup));
    return send_chunk_checked(req, chunk, n, sizeof(chunk), "builtin summary");
}

/* Full builtin entry: summary fields + every segment with its own verdict. */
static esp_err_t send_builtin_full(httpd_req_t *req, uint8_t id, const builtin_profile_t *b, bool first)
{
    profile_t p;
    profile_seg_verdict_t per_seg[PROFILE_MAX_SEGMENTS];
    profile_seg_verdict_t rollup = PROFILE_SEG_UNKNOWN;
    for (size_t i = 0; i < PROFILE_MAX_SEGMENTS; i++) {
        per_seg[i] = PROFILE_SEG_UNKNOWN;
    }
    if (profiles_builtin_get(id, &p)) {
        rollup = profile_feasibility_profile_mask(0, &p, per_seg, PROFILE_MAX_SEGMENTS);
    }

    char code_e[PROFILE_NAME_MAX_LEN * 2 + 1];
    char title_e[128];
    char slug_e[64];
    char chunk[384];
    int n = snprintf(chunk, sizeof(chunk),
                     "%s{\"id\":%u,\"builtin\":true,\"read_only\":true,\"name\":\"%s\",\"code\":\"%s\","
                     "\"title\":\"%s\",\"slug\":\"%s\",\"url\":\"https://digitalfire.com/schedule/%s\","
                     "\"hidden\":%s,\"zone_mask\":0,\"segment_count\":%u,\"feasibility\":\"%s\","
                     "\"segments\":[",
                     first ? "" : ",", id, esc(b->code, code_e, sizeof(code_e)),
                     esc(b->code, code_e, sizeof(code_e)), esc(b->title, title_e, sizeof(title_e)),
                     esc(b->slug, slug_e, sizeof(slug_e)), b->slug,
                     profiles_builtin_is_hidden(id) ? "true" : "false", b->segment_count,
                     profile_feasibility_verdict_str(rollup));
    esp_err_t err = send_chunk_checked(req, chunk, n, sizeof(chunk), "builtin header");
    if (err != ESP_OK) {
        return err;
    }

    for (uint8_t i = 0; i < b->segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
        n = snprintf(chunk, sizeof(chunk),
                     "%s{\"target_c\":%.2f,\"ramp_c_per_hr\":%.2f,\"dwell_min\":%lu,\"feasibility\":\"%s\"}",
                     i == 0 ? "" : ",", (double)b->segments[i].target_c,
                     (double)b->segments[i].ramp_c_per_hr, (unsigned long)b->segments[i].dwell_min,
                     profile_feasibility_verdict_str(per_seg[i]));
        err = send_chunk_checked(req, chunk, n, sizeof(chunk), "builtin segment");
        if (err != ESP_OK) {
            return err;
        }
    }
    return httpd_resp_send_chunk(req, "]}", 2);
}

/* GET /api/profiles/builtin -- the whole catalogue, segments and verdicts
 * included. ?all=1 includes hidden entries (the "restore" UI needs to show
 * what it would restore); the default omits them. */
static esp_err_t builtin_list_get_handler(httpd_req_t *req)
{
    bool include_hidden = false;
    char query[48];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "all", val, sizeof(val)) == ESP_OK && val[0] == '1') {
            include_hidden = true;
        }
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send_chunk(req, "[", 1);
    bool first = true;
    for (size_t i = 0; i < g_builtin_profile_count && err == ESP_OK; i++) {
        uint8_t id = (uint8_t)(PROFILE_BUILTIN_ID_BASE + i);
        if (!include_hidden && profiles_builtin_is_hidden(id)) {
            continue;
        }
        err = send_builtin_full(req, id, &g_builtin_profiles[i], first);
        first = false;
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]", 1);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0); /* terminate the chunked response */
    }
    return err;
}

/* Worst-case size of one user-slot entry's JSON, sized against a name that
 * FULLY escapes -- the TODO.md bug this replaces: the old 96-byte-per-slot
 * budget was sized off PROFILE_NAME_MAX_LEN's raw 15 chars, but
 * json_escape() can double every one of them (a `"` or `\` costs two output
 * bytes), and the fixed text around the name is not free either. Counted
 * literally: `,{"id":255,"builtin":false,"name":"` (36) + up to
 * PROFILE_NAME_MAX_LEN*2 (30) escaped name bytes + `","zone_mask":255,`
 * `"segment_count":12}` (37) = 103; rounded up with slack for the format
 * rather than re-deriving the exact count if a field ever widens. */
#define PROFILE_LIST_ENTRY_MAX 160

/* Bytes reserved at the tail of `json` that no per-slot APPEND is ever
 * allowed to write into -- so the fallback "listing truncated" notice below
 * always has guaranteed room to land, and the array's own close (sent as a
 * separate chunk, never through this buffer) is never the thing at risk.
 * Same discipline as readiness_http.c's append_item() reserve. */
#define PROFILE_LIST_CLOSE_RESERVE 96

static esp_err_t profiles_list_get_handler(httpd_req_t *req)
{
    char json[PROFILES_MAX_COUNT * PROFILE_LIST_ENTRY_MAX + PROFILE_LIST_CLOSE_RESERVE + 16];
    size_t o = 0;
    int n;
    bool dropped = false; /* an item didn't fit even the enlarged budget -- report it, don't hide it */

    /* Never writes past sizeof(json) - PROFILE_LIST_CLOSE_RESERVE -- `avail`
     * is clamped to 0 once `o` reaches that line, so a would-be write past it
     * is treated exactly like any other overflow (dropped, not truncated
     * into the reserve). */
#define APPEND(...)                                                                              \
    do {                                                                                          \
        size_t avail = (o + PROFILE_LIST_CLOSE_RESERVE < sizeof(json))                             \
                           ? sizeof(json) - PROFILE_LIST_CLOSE_RESERVE - o                          \
                           : 0;                                                                     \
        n = snprintf(json + o, avail, __VA_ARGS__);                                               \
        if (n < 0 || (size_t)n >= avail) {                                                         \
            dropped = true;                                                                        \
            goto list_done;                                                                        \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    json[o++] = '[';
    bool first = true;
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        if (!(s_profiles.used_bitmap & (1u << id))) {
            continue;
        }
        const profile_t *p = &s_profiles.profiles[id];
        char name_escaped[PROFILE_NAME_MAX_LEN * 2 + 1];
        json_escape(p->name, name_escaped, sizeof(name_escaped));
        APPEND("%s{\"id\":%u,\"builtin\":false,\"name\":\"%s\",\"zone_mask\":%u,\"segment_count\":%u}",
               first ? "" : ",", id, name_escaped, p->zone_mask, p->segment_count);
        first = false;
    }

#undef APPEND

list_done:
    if (dropped) {
        /* Guaranteed to fit: PROFILE_LIST_CLOSE_RESERVE bytes at json+o were
         * never touched by any APPEND above. Reported AS an item -- a
         * silently shortened list looks exactly like a pass, which is the
         * failure mode this exists to prevent (same rule readiness_http.c's
         * append_item() dropped-item notice follows). */
        int n2 = snprintf(json + o, sizeof(json) - o,
                          "%s{\"id\":null,\"builtin\":false,\"error\":\"one or more profiles omitted -- "
                          "listing too large\"}",
                          first ? "" : ",");
        if (n2 > 0 && (size_t)n2 < sizeof(json) - o) {
            o += (size_t)n2;
            first = false;
        } else {
            ESP_LOGE(TAG, "profiles listing: dropped-item notice itself didn't fit -- "
                         "PROFILE_LIST_CLOSE_RESERVE is too small");
        }
    }

    /* Chunked, because the visible builtin summaries appended after the user
     * slots would not fit alongside them in one stack buffer -- see the
     * response-size note above builtin_list_get_handler(). Segments are
     * deliberately NOT included here; a listing does not need 136 of them,
     * and GET /api/profile?id=<builtin> / GET /api/profiles/builtin serve
     * them when something actually does. */
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send_chunk(req, json, o);
    for (size_t i = 0; i < g_builtin_profile_count && err == ESP_OK; i++) {
        uint8_t bid = (uint8_t)(PROFILE_BUILTIN_ID_BASE + i);
        if (profiles_builtin_is_hidden(bid)) {
            continue; /* "removed by the user" -- see /api/profiles/builtin?all=1 */
        }
        err = send_builtin_summary(req, bid, &g_builtin_profiles[i], first);
        first = false;
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]", 1);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t profile_detail_get_handler(httpd_req_t *req)
{
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing");
        return ESP_OK;
    }
    char id_str[8];
    if (httpd_query_key_value(query, "id", id_str, sizeof(id_str)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing");
        return ESP_OK;
    }
    char *end = NULL;
    long id = strtol(id_str, &end, 10);
    if (end == id_str || id < 0 || id > 255) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }

    /* Builtin catalogue entry: served read-only, hidden or not (hiding is a
     * listing preference, so a direct reference must still resolve). */
    if (profiles_builtin_id_valid((uint8_t)id)) {
        const builtin_profile_t *b = profiles_builtin_entry((uint8_t)id);
        httpd_resp_set_type(req, "application/json");
        esp_err_t berr = send_builtin_full(req, (uint8_t)id, b, true);
        if (berr == ESP_OK) {
            berr = httpd_resp_send_chunk(req, NULL, 0);
        }
        return berr;
    }

    if (id >= PROFILES_MAX_COUNT || !(s_profiles.used_bitmap & (1u << id))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }

    const profile_t *p = &s_profiles.profiles[id];
    /* Sized for the per-segment "feasibility":"unreachable" field plus the
     * seg_kind/io_target/io_state/io_blocking/io_leave_on_at_end fields added
     * below (relay/IO segment support) -- worst case measured at 170 bytes
     * per segment, rounded up. */
    char json[224 + PROFILE_MAX_SEGMENTS * 192];
    size_t o = 0;
    int n;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(json + o, sizeof(json) - o, __VA_ARGS__);                                   \
        if (n < 0 || (size_t)n >= sizeof(json) - o) {                                             \
            goto send;                                                                            \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    /* Same model-based feasibility the catalogue entries carry -- a user's own
     * profile deserves the identical answer, and the UI can then colour both
     * kinds with one rule. */
    profile_seg_verdict_t per_seg[PROFILE_MAX_SEGMENTS];
    for (size_t si = 0; si < PROFILE_MAX_SEGMENTS; si++) {
        per_seg[si] = PROFILE_SEG_UNKNOWN;
    }
    profile_seg_verdict_t rollup =
        profile_feasibility_profile_mask(p->zone_mask, p, per_seg, PROFILE_MAX_SEGMENTS);

    char name_escaped[PROFILE_NAME_MAX_LEN * 2 + 1];
    json_escape(p->name, name_escaped, sizeof(name_escaped));
    APPEND("{\"id\":%ld,\"builtin\":false,\"read_only\":false,\"name\":\"%s\",\"zone_mask\":%u,"
           "\"segment_count\":%u,\"feasibility\":\"%s\",\"segments\":[",
           id, name_escaped, p->zone_mask, p->segment_count,
           profile_feasibility_verdict_str(rollup));
    for (uint8_t i = 0; i < p->segment_count; i++) {
        const profile_segment_t *s = &p->segments[i];
        /* Genuine firmware defect found while wiring the editor UI to this
         * endpoint (owner's relay/IO segment request, profiles_http.h's
         * profile_seg_kind_t comment): this response used to emit only the
         * three ZONE_RAMP fields, so GETting a profile that has a RELAY_IO
         * segment silently dropped seg_kind/io_target/io_state/io_blocking/
         * io_leave_on_at_end -- editProfile() in profiles_page.html loads a
         * profile through exactly this call and repopulates the editor from
         * it, so without these fields every "Edit" of a saved relay segment
         * would reload it as target_c 0 / ramp 0 / dwell <whatever dwell_min
         * held>, i.e. a bogus ZONE_RAMP row, discarding the relay config on
         * the very next save. Added rather than routed around client-side. */
        APPEND("%s{\"seg_kind\":%u,\"target_c\":%.2f,\"ramp_c_per_hr\":%.2f,\"dwell_min\":%lu,"
               "\"io_target\":%u,\"io_state\":%u,\"io_blocking\":%u,\"io_leave_on_at_end\":%u,"
               "\"feasibility\":\"%s\"}",
               i == 0 ? "" : ",", s->seg_kind, (double)s->target_c, (double)s->ramp_c_per_hr,
               (unsigned long)s->dwell_min, s->io_target, s->io_state, s->io_blocking,
               s->io_leave_on_at_end, profile_feasibility_verdict_str(per_seg[i]));
    }
    APPEND("]}");

#undef APPEND

send:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
}

/* ---- POST /api/profile ----------------------------------------------------
 * Validates into a scratch profile_t before touching s_profiles/NVS. Runs
 * the TODO.md section 5 feasibility check against zones_http.c's
 * user-entered per-zone ramp ceiling (zones_config_get_max_ramp()): a
 * segment whose ramp rate exceeds the ceiling rejects the whole submission;
 * one within 20% of it (PROFILE_RAMP_WARN_FRACTION) is accepted with a
 * warning, per TODO.md's explicit "warn, don't block" rule at that margin.
 * This is the creation-time half of that check -- TODO.md also calls for
 * re-checking at profile-*start* time, which belongs to the profile
 * executor, not this page, and profile_executor.c does exactly that against
 * every participating zone's ceiling as it stands at start. */

static bool parse_profile_fields(const char *body, profile_t *p, char *err_msg, size_t err_cap)
{
    char name[PROFILE_NAME_MAX_LEN + 1];
    int name_len = http_form_find_field(body, "name", name, sizeof(name));
    if (name_len == -2) {
        snprintf(err_msg, err_cap, "name too long");
        return false;
    }
    if (name_len <= 0) {
        snprintf(err_msg, err_cap, "name missing");
        return false;
    }
    strncpy(p->name, name, PROFILE_NAME_MAX_LEN);
    p->name[PROFILE_NAME_MAX_LEN] = '\0';

    char zone_val[8];
    int zone_len = http_form_find_field(body, "zone_mask", zone_val, sizeof(zone_val));
    if (zone_len <= 0) {
        snprintf(err_msg, err_cap, "zone_mask missing");
        return false;
    }
    char *end = NULL;
    long zone_mask = strtol(zone_val, &end, 10);
    uint8_t thermo_count = zones_config_get_thermo_count();
    uint8_t valid_bits = thermo_count >= 8 ? 0xFF : (uint8_t)((1u << thermo_count) - 1u);
    if (end == zone_val || zone_mask <= 0 || zone_mask > 0xFF || ((uint8_t)zone_mask & ~valid_bits) != 0) {
        snprintf(err_msg, err_cap,
                "zone_mask must select at least one configured zone (check Thermocouples & Zones settings)");
        return false;
    }
    p->zone_mask = (uint8_t)zone_mask;

    char seg_count_val[8];
    int seg_count_len = http_form_find_field(body, "seg_count", seg_count_val, sizeof(seg_count_val));
    if (seg_count_len <= 0) {
        snprintf(err_msg, err_cap, "seg_count missing");
        return false;
    }
    end = NULL;
    long seg_count = strtol(seg_count_val, &end, 10);
    if (end == seg_count_val || seg_count < 1 || seg_count > PROFILE_MAX_SEGMENTS) {
        snprintf(err_msg, err_cap, "seg_count out of range (1-12)");
        return false;
    }
    p->segment_count = (uint8_t)seg_count;

    for (uint8_t i = 0; i < p->segment_count; i++) {
        /* 24, not 16. The longest key built here is "seg%u_io_blocking", and
         * at the last segment index that is "seg11_io_blocking" -- 17
         * characters plus the terminator, which does not fit 16. The MSVC
         * host build does not run -Wformat-truncation, so this compiled and
         * passed every host test; only the target build (-Werror=format-
         * truncation) caught it. A truncated key would not have failed
         * loudly either: http_form_find_field() would simply not find
         * "seg11_io_blockin", and the field would silently read as absent,
         * taking its default. 2026-08-28. */
        char key[24];
        profile_segment_t *seg = &p->segments[i];
        memset(seg, 0, sizeof(*seg));

        /* seg%u_kind is OPTIONAL and defaults to PROFILE_SEG_KIND_ZONE_RAMP
         * (0) when absent -- every existing caller of this endpoint (the
         * profiles_page.html editor as it stands today, and any UART/scripted
         * submission written before this pass) never sends it and must keep
         * producing exactly the temperature-ramp segment it always has. */
        snprintf(key, sizeof(key), "seg%u_kind", i);
        char val[24];
        int len = http_form_find_field(body, key, val, sizeof(val));
        char *fend = NULL;
        long kind = len > 0 ? strtol(val, &fend, 10) : PROFILE_SEG_KIND_ZONE_RAMP;
        if (len > 0 && fend == val) {
            kind = PROFILE_SEG_KIND_ZONE_RAMP;
        }
        if (kind != PROFILE_SEG_KIND_ZONE_RAMP && kind != PROFILE_SEG_KIND_RELAY_IO) {
            snprintf(err_msg, err_cap, "segment %u: unknown segment kind %ld", i + 1, kind);
            return false;
        }
        seg->seg_kind = (uint8_t)kind;

        if (seg->seg_kind == PROFILE_SEG_KIND_RELAY_IO) {
            snprintf(key, sizeof(key), "seg%u_io_target", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            end = NULL;
            long io_target = len > 0 ? strtol(val, &end, 10) : -1;
            if (len <= 0 || end == val || io_target < 0 || io_target > 255) {
                snprintf(err_msg, err_cap, "segment %u: io_target missing or out of range", i + 1);
                return false;
            }
            seg->io_target = (uint8_t)io_target;

            snprintf(key, sizeof(key), "seg%u_io_state", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            seg->io_state = (len > 0 && val[0] != '0') ? 1 : 0;

            snprintf(key, sizeof(key), "seg%u_io_blocking", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            /* Missing defaults to BLOCKING (1) -- the safer of the two: a
             * segment nobody said was non-blocking should still hold up the
             * schedule and get an explicit force-off at its own end, rather
             * than silently running loose in the background. */
            seg->io_blocking = (len <= 0 || val[0] != '0') ? 1 : 0;

            snprintf(key, sizeof(key), "seg%u_io_leave_on", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            /* Owner's explicit instruction: "Default must be OFF (force it
             * off)". Missing, empty, or "0" all mean off -- only an explicit
             * nonzero value turns this on. */
            seg->io_leave_on_at_end = (len > 0 && val[0] != '0') ? 1 : 0;

            snprintf(key, sizeof(key), "seg%u_dwell", i);
            len = http_form_find_field(body, key, val, sizeof(val));
            end = NULL;
            long dwell = len > 0 ? strtol(val, &end, 10) : 0; /* missing = 0, same as "no hold" */
            if (len > 0 && (end == val || dwell < 0 || dwell > (long)PROFILE_DWELL_MIN_MAX)) {
                snprintf(err_msg, err_cap, "segment %u: dwell_min out of range (0-1440)", i + 1);
                return false;
            }
            seg->dwell_min = (uint32_t)(dwell < 0 ? 0 : dwell);

            if (!validate_io_segment(seg, (uint8_t)(i + 1), err_msg, err_cap)) {
                return false;
            }
            continue;
        }

        snprintf(key, sizeof(key), "seg%u_target", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        float target = len > 0 ? strtof(val, &fend) : NAN;
        if (len <= 0 || fend == val || isnan(target) || target < PROFILE_TARGET_C_MIN ||
            target > PROFILE_TARGET_C_MAX) {
            snprintf(err_msg, err_cap, "segment %u: target_c missing or out of range (0-1400)", i + 1);
            return false;
        }
        seg->target_c = target;

        snprintf(key, sizeof(key), "seg%u_ramp", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        fend = NULL;
        float ramp = len > 0 ? strtof(val, &fend) : NAN;
        if (len <= 0 || fend == val || isnan(ramp) || ramp < PROFILE_RAMP_C_PER_HR_MIN ||
            ramp > PROFILE_RAMP_C_PER_HR_MAX) {
            snprintf(err_msg, err_cap, "segment %u: ramp_c_per_hr missing or out of range (0-1000)", i + 1);
            return false;
        }
        seg->ramp_c_per_hr = ramp;

        snprintf(key, sizeof(key), "seg%u_dwell", i);
        len = http_form_find_field(body, key, val, sizeof(val));
        end = NULL;
        long dwell = len > 0 ? strtol(val, &end, 10) : -1;
        if (len <= 0 || end == val || dwell < 0 || dwell > (long)PROFILE_DWELL_MIN_MAX) {
            snprintf(err_msg, err_cap, "segment %u: dwell_min missing or out of range (0-1440)", i + 1);
            return false;
        }
        seg->dwell_min = (uint32_t)dwell;
    }
    return true;
}

/* Appends a JSON string element for warnings[]; returns false (and leaves
 * *o unchanged) if it wouldn't fit, matching every other APPEND-macro
 * handler's "stop rather than overrun" convention. */
static bool append_warning(char *json, size_t cap, size_t *o, bool *first, const char *text)
{
    int n = snprintf(json + *o, cap - *o, "%s\"%s\"", *first ? "" : ",", text);
    if (n < 0 || (size_t)n >= cap - *o) {
        return false;
    }
    *o += (size_t)n;
    *first = false;
    return true;
}

static esp_err_t profile_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > PROFILE_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[PROFILE_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "profile body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    /* id: empty or "-1" creates in the first free slot; a valid existing id
     * overwrites that slot. Any other value in 0..7 also targets that exact
     * slot (create-or-overwrite), so a client that already knows its id can
     * address it directly rather than relying on "first free". */
    char id_val[8];
    int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
    long requested_id = (id_len > 0) ? strtol(id_val, NULL, 10) : -1;

    uint8_t target_id;
    if (requested_id >= 0 && requested_id < PROFILES_MAX_COUNT) {
        target_id = (uint8_t)requested_id;
    } else {
        int free_slot = -1;
        for (uint8_t i = 0; i < PROFILES_MAX_COUNT; i++) {
            if (!(s_profiles.used_bitmap & (1u << i))) {
                free_slot = i;
                break;
            }
        }
        if (free_slot < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "profile storage full");
            return ESP_OK;
        }
        target_id = (uint8_t)free_slot;
    }

    profile_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    char err_msg[128];
    if (!parse_profile_fields(body, &tmp, err_msg, sizeof(err_msg))) {
        char json[192];
        int n = snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", err_msg);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
    }

    /* Feasibility check (TODO.md section 5): a segment with ramp_c_per_hr ==
     * 0 has no ramp-rate constraint at all (dwell/hold segment) and is
     * exempt. For a segment that does specify a rate, no ceiling on record
     * for the zone (zones_config_get_max_ramp returning a ceiling of 0.0,
     * which is also its "never configured" default) makes every nonzero
     * rate infeasible -- correct, since there is nothing to feasibility
     * check against until the zone's max ramp rate is set on the
     * Thermocouples & Zones page. */
    char warn_json[PROFILE_MAX_SEGMENTS * 96 + 16];
    size_t warn_o = 0;
    bool warn_first = true;
    warn_json[warn_o++] = '[';
    /* Multi-zone (TODO.md 6A.5): check every participating zone's ceiling
     * against every ramped segment -- a profile is only feasible if ALL of
     * its zones can sustain the requested rate, since ramp-lock will hold
     * the shared setpoint back to whichever zone is slowest anyway; a
     * profile that's infeasible for even one zone would just always be
     * ramp-locked against that zone forever. */
    for (uint8_t i = 0; i < tmp.segment_count; i++) {
        if (tmp.segments[i].seg_kind != PROFILE_SEG_KIND_ZONE_RAMP) {
            continue; /* a relay/IO segment has no ramp rate to check against a zone's ceiling */
        }
        float rate = tmp.segments[i].ramp_c_per_hr;
        if (rate <= 0.0f) {
            continue;
        }
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!(tmp.zone_mask & (1u << zi))) {
                continue;
            }
            float ceiling = 0.0f;
            zones_config_get_max_ramp(zi, &ceiling); /* zone already validated < thermo_count */
            if (rate > ceiling) {
                char json[224];
                int n = snprintf(json, sizeof(json),
                                 "{\"ok\":false,\"error\":\"segment %u: ramp rate %.1f C/hr exceeds zone %u's "
                                 "%.1f C/hr ceiling\"}",
                                 i + 1, (double)rate, zi, (double)ceiling);
                httpd_resp_set_status(req, "400 Bad Request");
                httpd_resp_set_type(req, "application/json");
                return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
            }
            if (rate > PROFILE_RAMP_WARN_FRACTION * ceiling) {
                char text[96];
                snprintf(text, sizeof(text),
                        "segment %u: ramp rate %.1f C/hr is within 20%% of zone %u's %.1f C/hr ceiling",
                        i + 1, (double)rate, zi, (double)ceiling);
                append_warning(warn_json, sizeof(warn_json), &warn_o, &warn_first, text);
            }
        }
    }
    if (warn_o + 1 < sizeof(warn_json)) {
        warn_json[warn_o++] = ']';
    }
    warn_json[warn_o < sizeof(warn_json) ? warn_o : sizeof(warn_json) - 1] = '\0';

    s_profiles.profiles[target_id] = tmp;
    s_profiles.used_bitmap |= (1u << target_id);
    esp_err_t err = nvs_save_slot(target_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save_slot(%u) failed: %s -- profile applied live but will not survive a reboot",
                 target_id, esp_err_to_name(err));
    }

    char json[256 + sizeof(warn_json)];
    int n = snprintf(json, sizeof(json), "{\"ok\":true,\"id\":%u,\"warnings\":%s}", target_id, warn_json);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

static esp_err_t profile_delete_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }
    char body[65];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "profile delete body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char id_val[8];
    int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
    char *end = NULL;
    long id = (id_len > 0) ? strtol(id_val, &end, 10) : -1;
    if (id_len > 0 && end != id_val && id >= 0 && id <= 255 && profiles_builtin_id_valid((uint8_t)id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "built-in schedules are read-only and cannot be deleted -- "
                            "hide it instead (POST /api/profile/builtin/hide)");
        return ESP_OK;
    }
    if (id_len <= 0 || end == id_val || id < 0 || id >= PROFILES_MAX_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id missing or out of range");
        return ESP_OK;
    }
    if (!(s_profiles.used_bitmap & (1u << id))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such profile");
        return ESP_OK;
    }

    s_profiles.used_bitmap &= ~(1u << id);
    memset(&s_profiles.profiles[id], 0, sizeof(s_profiles.profiles[id]));
    esp_err_t err = nvs_erase_slot((uint8_t)id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_slot(%ld) failed: %s -- deleted live but may reappear after reboot", id,
                 esp_err_to_name(err));
    }
    return httpd_resp_sendstr(req, "ok");
}

/* ---- Builtin hide / unhide / restore ---------------------------------------
 *
 * "Remove this shipped schedule" cannot be a delete -- the catalogue is a
 * const table in flash -- so it is a persisted hide, and unhiding is
 * therefore always possible. See profiles_builtin.h.
 *
 * POST /api/profile/builtin/hide     body: id=<128..>&hidden=0|1
 * POST /api/profile/builtin/restore  body: (none) -- unhides everything
 */

static bool read_small_body(httpd_req_t *req, char *buf, size_t cap)
{
    if ((size_t)req->content_len >= cap) {
        return false;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            return false;
        }
        received += (size_t)ret;
    }
    buf[received] = '\0';
    return true;
}

static esp_err_t builtin_hide_post_handler(httpd_req_t *req)
{
    char body[65];
    if (!read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing, too large, or read failed");
        return ESP_OK;
    }

    char id_val[8];
    int id_len = http_form_find_field(body, "id", id_val, sizeof(id_val));
    char *end = NULL;
    long id = (id_len > 0) ? strtol(id_val, &end, 10) : -1;
    if (id_len <= 0 || end == id_val || id < 0 || id > 255 || !profiles_builtin_id_valid((uint8_t)id)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such built-in schedule");
        return ESP_OK;
    }

    /* Missing "hidden" defaults to 1: the endpoint is named "hide", so the
     * request with no qualifier means hide. Unhiding takes an explicit
     * hidden=0. */
    char hid_val[8];
    int hid_len = http_form_find_field(body, "hidden", hid_val, sizeof(hid_val));
    bool hidden = (hid_len <= 0) || (hid_val[0] != '0');

    esp_err_t err = profiles_builtin_set_hidden((uint8_t)id, hidden);
    char json[128];
    int n = snprintf(json, sizeof(json), "{\"ok\":%s,\"id\":%ld,\"hidden\":%s,\"persisted\":%s}",
                     "true", id, hidden ? "true" : "false", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

static esp_err_t builtin_restore_post_handler(httpd_req_t *req)
{
    char body[65];
    if (req->content_len > 0 && !read_small_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or read failed");
        return ESP_OK;
    }
    esp_err_t err = profiles_builtin_restore_all();
    char json[96];
    int n = snprintf(json, sizeof(json), "{\"ok\":true,\"persisted\":%s}", err == ESP_OK ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, n < 0 ? 0 : (size_t)n);
}

esp_err_t profiles_http_start(void)
{
    /* profiles_nvs is used only by this module, but nvs_flash_init_partition()
     * on an already-initialized partition is a harmless no-op (ESP_OK), so
     * bringing it up here independently (rather than assuming some other
     * module did it) is safe either way. */
    esp_err_t part_err = nvs_partition_init(PROFILES_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init for '%s' failed: %s -- profiles will not persist", PROFILES_NVS_PARTITION,
                 esp_err_to_name(part_err));
    }

    esp_err_t err = ESP_OK;
    if (part_err == ESP_OK) {
        bool found_in_profiles_nvs = false;
        err = nvs_load_all_from(PROFILES_NVS_PARTITION, &s_profiles, &found_in_profiles_nvs);
        if (err == ESP_OK && !found_in_profiles_nvs) {
            /* Nothing recorded in profiles_nvs yet -- see if the old
             * default partition has pre-split profiles worth carrying
             * forward. */
            migrate_from_default_partition();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "profile NVS load failed: %s -- starting with no saved profiles", esp_err_to_name(err));
        memset(&s_profiles, 0, sizeof(s_profiles));
    }

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/profiles", .method = HTTP_GET, .handler = page_get_handler,
    };
    static const httpd_uri_t list_uri = {
        .uri = "/api/profiles", .method = HTTP_GET, .handler = profiles_list_get_handler,
    };
    static const httpd_uri_t detail_uri = {
        .uri = "/api/profile", .method = HTTP_GET, .handler = profile_detail_get_handler,
    };
    static const httpd_uri_t post_uri = {
        .uri = "/api/profile", .method = HTTP_POST, .handler = profile_post_handler,
    };
    static const httpd_uri_t delete_uri = {
        .uri = "/api/profile/delete", .method = HTTP_POST, .handler = profile_delete_post_handler,
    };
    static const httpd_uri_t builtin_list_uri = {
        .uri = "/api/profiles/builtin", .method = HTTP_GET, .handler = builtin_list_get_handler,
    };
    static const httpd_uri_t builtin_hide_uri = {
        .uri = "/api/profile/builtin/hide", .method = HTTP_POST, .handler = builtin_hide_post_handler,
    };
    static const httpd_uri_t builtin_restore_uri = {
        .uri = "/api/profile/builtin/restore", .method = HTTP_POST, .handler = builtin_restore_post_handler,
    };
    err = httpd_register_uri_handler(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/profiles) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &list_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/profiles) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &detail_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/profile) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/profile) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &delete_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/profile/delete) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = httpd_register_uri_handler(server, &builtin_list_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/profiles/builtin) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &builtin_hide_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/profile/builtin/hide) failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &builtin_restore_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/profile/builtin/restore) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "profiles API up (storage/validation; execution runs in profile_executor.c)");
    return ESP_OK;
}
