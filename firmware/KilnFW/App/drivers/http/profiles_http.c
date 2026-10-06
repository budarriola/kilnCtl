#include "profiles_http.h"
#include "http_auth_http.h" // kiln_http_register() -- WEB_AUTH_PLAN.md section 5

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h" /* EXT_RAM_BSS_ATTR -- see s_profiles_fallback in profiles_storage_ensure() */
#include "esp_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h" /* portMUX_TYPE -- profiles_storage_ensure()'s once-guard below */
#include "freertos/task.h" /* vTaskDelay() -- same once-guard, the losing task's wait */
#include "esp_heap_caps.h" /* heap_caps_malloc()/MALLOC_CAP_* -- profiles_storage_ensure()'s
                            * PSRAM allocation (docs/PROFILE_SLOTS_100_PLAN.md section 7 task 3) */
#include "esp_log.h"

#include "hal_kv.h"
#include "hal_esp_common.h" /* hal_status_to_esp_err() -- preserve the specific esp_err_t seen by
                              * callers of this module's nvs_*()-named wrappers below */
#include "nvs_key_check.h"

#include "MAX31856.h"
#include "http_form.h"
#include "kiln_io.h"
#include "live_profile.h" /* live_edit_name_collides() -- profiles_http_save() dup-name refusal */
#include "persist_scratch.h"
#include "profile_feasibility.h"
#include "profiles_builtin.h"
#include "profiles_favorites.h" /* profiles_favorites_set() -- see profiles_http_delete()'s doc
                                  * comment; profiles_edit_http.c's web delete handler already
                                  * clears a deleted slot's favorite mark, and this benchproto
                                  * path must not leave that half undone (Opus review item 1,
                                  * PROFILE_SLOTS_100_PLAN.md section 7). */
#include "wifi_provision_http.h"
#include "zones_config_accessors.h"
#include "aux_outputs_cfg.h"   /* aux_outputs_cfg_get()/_enabled_mask() -- spare-relay targets (WP-4) */
#include "profile_rule_target.h" /* zone_index 8..11 = aux relay 1..4 */
#include "on_off_trigger_decide.h" /* on_off_phase_bit_t/on_off_direction_bit_t/on_off_temp_cmp_t --
                                     * validate_on_off_rules() bounds-checks against these same bit
                                     * layouts so a stored rule can never encode a bit the decision
                                     * core does not know how to interpret. */

#include "profiles_http_internal.h"

#include "cfg_fs.h"
#include "cfg_fs_status.h"
#include "profiles_cfg_fs.h" /* docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 4:
                                * read-through/dual-write bridge to the `cfg`
                                * LittleFS partition, one file per slot. See
                                * that header for the full policy. */
#include "profile_executor.h" /* firing_stats_erase() -- docs/PROFILE_SLOTS_100_PLAN.md
                                 * section 7 task 10, called from nvs_erase_slot() below
                                 * so deleting a slot also prunes its firing history. */

const char *PROFILES_TAG = "profiles_http";

#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_USED "prof_used"
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_USED);
/* "prof0".."prof7" -- see profile_nvs_key() below. */

/* Per-slot rev counter, PROFILES_MAX_COUNT uint32_t, one blob -- see
 * profiles_http_internal.h's s_profile_rev doc comment for why this must be
 * bumped on delete too, not just save (docs/FILESYSTEM_USER_DATA_PLAN.md
 * section 5 step 4, user-profiles filesystem move). Separate key from
 * NVS_KEY_USED, same reasoning zones_http.c's NVS_KEY_ZONES_REV split from
 * its cfg blob: a rev counter is bookkeeping for the file/NVS bridge, not
 * part of any profile's own persisted shape. */
#define NVS_KEY_PROFILE_REV "prof_rev"
NVS_KEY_LEN_CHECK(NVS_KEY_PROFILE_REV);

uint32_t s_profile_rev[PROFILES_MAX_COUNT];

/* profiles_nvs is the 2026-08-13 split target for fire profiles (see
 * partitions.csv and TODO.md 8.1) -- profiles are the one section of the old
 * default `nvs` partition's contents that grows with use, so they get their
 * own partition with the most headroom rather than sharing kiln_nvs with
 * zones/rules/relay_cycles/run_state. Each module manages its own migration
 * and partition init independently rather than assuming another module
 * already brought its partition up. hal_kv's NULL-partition selector (the
 * old, still-live default partition this module's data used to persist to)
 * is kept readable for the one-time migration below
 * and for firmware rollback. */
#define PROFILES_NVS_PARTITION "profiles_nvs"
NVS_KEY_LEN_CHECK(PROFILES_NVS_PARTITION);

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
 * 3 -> 4 (docs/ON_OFF_ZONE_PLAN.md plan step 5): profile_t itself grew a
 * tail -- on_off_rule_count + on_off_rules[PROFILE_MAX_ON_OFF_RULES]
 * (profiles_types.h). This is the SAFE case the comments above warn is
 * rare: profile_segment_t's own shape is UNCHANGED, so the new fields land
 * strictly after segments[PROFILE_MAX_SEGMENTS] with no element-shift
 * hazard -- a plain trailing append, same shape as zones_http.c's v21->v22
 * (progress_band_c). profile_t_v3/profile_persisted_v3_t below freeze
 * exactly today's (pre-rules) shape -- segments unchanged from v2's -- so
 * v3 gets its own migration case (convert_profile_v3()) rather than being
 * folded into convert_profile_v2()'s path.
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
#define PROFILE_VERSION 4

/* PROFILES_MAX_COUNT / PROFILE_NAME_MAX_LEN / PROFILE_MAX_SEGMENTS and the
 * profile_t/profile_segment_t layout now live in profiles_http.h --
 * profile_executor.c needs them too (via profiles_http_get()). */

/* Firmware sanity bound, not a per-kiln safety limit -- profiles are
 * portable between kilns (a cone-10 or gas-kiln profile authored on one rig
 * is legitimate to SAVE on a low-temperature bench rig; only STARTING it is
 * refused, at profile_executor_run.c's own re-check against the zone's
 * CURRENT max_temp_c). This exists only to reject obvious garbage/typos
 * (a corrupt payload, a stray extra digit) before anything is stored, not to
 * cap what a real firing schedule may target.
 *
 * Owner request (2026-09-02): gas-kiln profiles up to cone 42 (2015C, the
 * top of the standard pyrometric cone table) must be storable. Set to
 * 2015.0f exactly -- the owner's own figure, not rounded up, since this is
 * an input-sanity ceiling rather than a value anything needs headroom
 * against. Required (`_Static_assert` below) to stay at or below
 * ZONE_MAX_TEMP_C_MAX (zones_http.h) -- a profile target must always be
 * representable against some legally configurable zone ceiling, or a
 * legitimate profile could be un-runnable on ANY kiln, not just this one.
 * profile_segment_t::target_c is a plain float (profiles_http.h) with no
 * fixed-point encoding anywhere in the on-flash blob (decode_profile_blob()
 * copies it field-for-field across every PROFILE_VERSION), so raising this
 * bound cannot truncate a stored value -- no PROFILE_VERSION bump needed. */

/* Enforced at compile time rather than left to be kept equal by hand (the
 * failure mode both constants' own comments warn about): a profile target
 * that could never be within reach of ANY legally configurable zone ceiling
 * would be a profile no kiln could ever be configured to run. Mutation-
 * tested by hand (2026-09-02 pass): bumping PROFILE_TARGET_C_MAX to
 * 2600.0f (above ZONE_MAX_TEMP_C_MAX's 2500.0f) fails the host build with
 * this assertion's message; reverted immediately after confirming it. */
_Static_assert(PROFILE_TARGET_C_MAX <= ZONE_MAX_TEMP_C_MAX,
               "PROFILE_TARGET_C_MAX must not exceed ZONE_MAX_TEMP_C_MAX -- a profile target must "
               "stay representable against some legally configurable zone ceiling");

/* The 20%-margin warning rule, explicit in TODO.md section 5. */

/* All 8 slots kept resident -- each is well under 200 bytes, so loading all
 * 8 at boot (rather than lazily per-request) is simpler and cheap enough
 * that the "only load what's used" optimization the header docstring
 * mentions as a design choice isn't worth the extra code path. The
 * prof_used bitmap still exists in NVS/RAM so a listing never has to probe
 * 8 keys to find out which exist. profiles_state_t itself now lives in
 * profiles_http_internal.h -- profiles_catalog_http.c/profiles_edit_http.c
 * need the type too.
 *
 * docs/PROFILE_SLOTS_100_PLAN.md section 7 task 3: profiles_state_t
 * (dominated by profiles[PROFILES_MAX_COUNT], and growing further once task
 * 6 raises PROFILES_MAX_COUNT) is now a lazily allocated PSRAM buffer
 * instead of a .bss global -- see profiles_storage_ensure() below, the only
 * function that touches this pointer directly. Every other reference in
 * this module (and profiles_catalog_http.c/profiles_edit_http.c/the host
 * tests) keeps writing `s_profiles.foo`, which profiles_http_internal.h's
 * `#define s_profiles (*profiles_storage_ensure())` transparently turns
 * into a call through this pointer -- so `memset(&s_profiles, 0,
 * sizeof(s_profiles))`, used throughout the host tests, still zeroes the
 * allocated struct in place rather than the pointer itself. */
static profiles_state_t *s_profiles_ptr = NULL;
/* Review fold-in (PROFILE_SLOTS_100_PLAN.md section 7): the plain
 * check-then-act above raced two callers on the first call each -- both
 * could pass the NULL check, both allocate, and the losing store leaks its
 * allocation (or worse, two callers observe two different pointers across
 * the race window). Only the check-and-claim is under the critical section;
 * heap_caps_malloc()/memset() below run outside it since they can take real
 * time and must never run with interrupts disabled. */
static portMUX_TYPE s_profiles_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_profiles_init_claimed = false;

profiles_state_t *profiles_storage_ensure(void)
{
    if (s_profiles_ptr != NULL) {
        return s_profiles_ptr;
    }

    bool claimed_by_me = false;
    portENTER_CRITICAL(&s_profiles_init_mux);
    if (!s_profiles_init_claimed) {
        s_profiles_init_claimed = true;
        claimed_by_me = true;
    }
    portEXIT_CRITICAL(&s_profiles_init_mux);

    if (!claimed_by_me) {
        /* Another task got there first and is allocating right now --
         * spin until it publishes s_profiles_ptr. Bounded in practice by
         * one heap_caps_malloc()+memset(), not an unbounded wait. */
        while (s_profiles_ptr == NULL) {
            vTaskDelay(1);
        }
        return s_profiles_ptr;
    }

    /* hal_kv_set_blob()/nvs_set_blob() (nvs_save_slot() below) COPY the
     * bytes they are given into their own internal write buffer
     * synchronously -- they never DMA the caller's buffer -- so a PSRAM
     * source here is safe for the NVS write path. This is heap DATA, not a
     * task STACK: it is a different hazard from the PSRAM-stack class of
     * bug (a task whose STACK lives in PSRAM can panic
     * esp_task_stack_is_sane_cache_disabled() on an NVS write, guarded
     * separately in safety_cfg_store.c/nvs_save_slot()'s own
     * esp_ptr_external_ram() check on the CALLING stack, not on this
     * buffer). heap_caps_malloc() is a host-test stub (App/test/stubs/
     * esp_heap_caps.h) that allocates via the host's real malloc(), caps
     * ignored -- so this path runs for real, allocation included, under the
     * host test suite too. */
    profiles_state_t *p = heap_caps_malloc(sizeof(profiles_state_t), MALLOC_CAP_SPIRAM);
    if (p == NULL) {
        ESP_LOGW(PROFILES_TAG, "profiles storage: %u-byte PSRAM allocation failed, falling back "
                 "to internal RAM", (unsigned)sizeof(profiles_state_t));
        p = heap_caps_malloc(sizeof(profiles_state_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (p == NULL) {
        /* Both pools failed. Never dereference NULL: every caller of
         * `s_profiles` goes through this function, so falling back to one
         * static instance leaves the profile store empty-but-valid (no
         * profiles resident this boot) instead of crashing the first
         * request that touches it.
         *
         * EXT_RAM_BSS_ATTR is NOT optional here. Without it this one
         * never-normally-used instance costs sizeof(profiles_state_t) --
         * 42416 bytes at PROFILES_MAX_COUNT 100 -- of INTERNAL .dram0.bss
         * on every boot, whether or not the fallback is ever taken. The
         * first bench flash of the 100-slot build (5f58ba09, 2026-09-20)
         * did exactly that: internal heap fell to 8447 B free / 263 B
         * low-water, and the Wi-Fi ppTask aborted on
         * esp_timer_create() == ESP_ERR_NO_MEM inside phy_track_pll_init()
         * (docs/audits/dram_bss_profiles_fallback_2026-09-20.md). PSRAM
         * .bss is mapped before app_main() runs, so if PSRAM were absent
         * the boot would already have failed long before this line;
         * placing the fallback there keeps the never-NULL guarantee at
         * zero internal-RAM cost. check_kilnfw_dram_bss_budget.ps1 now
         * fails the suite if .dram0.bss grows past its budget again. */
        static EXT_RAM_BSS_ATTR profiles_state_t s_profiles_fallback;
        ESP_LOGE(PROFILES_TAG, "profiles storage: internal RAM allocation also failed -- "
                 "profile store starting EMPTY (no user profiles available this boot)");
        p = &s_profiles_fallback;
    }
    memset(p, 0, sizeof(*p));
    s_profiles_ptr = p;
    return s_profiles_ptr;
}

/* docs/PROFILE_SLOTS_100_PLAN.md section 7 task 1 -- the sanctioned way to
 * test/set/clear a bit of s_profiles.used_bitmap. Thin wrappers over the
 * generic profiles_slot_bitmap_t helpers (profiles_slot_bitmap.h); kept
 * here (not inline in the header) so this is the one place s_profiles is
 * touched by name for this purpose. */
bool profiles_slot_used(uint8_t id)
{
    return profiles_slot_bitmap_test(&s_profiles.used_bitmap, id);
}

void profiles_slot_set(uint8_t id)
{
    profiles_slot_bitmap_set(&s_profiles.used_bitmap, id);
}

void profiles_slot_clear(uint8_t id)
{
    profiles_slot_bitmap_clear(&s_profiles.used_bitmap, id);
}

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

/* EXACT snapshot of profile_t as it was BEFORE this pass (2026-09-08) added
 * on_off_rule_count/on_off_rules -- the CURRENT (v3) segment shape
 * (seg_kind/io_* fields present), just without the on/off-rule tail. Same
 * discipline as profile_t_v2 above: never grown or reused for a later
 * version, frozen the moment PROFILE_VERSION moves past it. A real board's
 * v3 blobs are exactly this shape. */
typedef struct {
    char name[PROFILE_NAME_MAX_LEN + 1];
    uint8_t zone_mask;
    uint8_t segment_count;
    profile_segment_t segments[PROFILE_MAX_SEGMENTS]; /* CURRENT segment shape, unchanged by this pass */
} profile_t_v3;

typedef struct {
    uint8_t version;
    profile_t_v3 profile;
    uint32_t crc32;
} profile_persisted_v3_t; /* v3: crc32 tail, current segment shape, NO on/off rules */

/* Frozen-snapshot size/offset asserts -- 2026-09-08 docs/audits/ review of
 * this feature found ZONES_CFG_VERSION's frozen-snapshot discipline had one
 * missing `_Static_assert` elsewhere in the tree; do not repeat that here.
 * Sizes/offsets computed off the actual struct layout (float/uint32-aligned
 * profile_segment_t forces 4-byte struct alignment throughout), not by
 * hand-counted bytes. */
_Static_assert(sizeof(profile_t_v3) == 260, "profile_t_v3 must stay exactly the pre-rules profile_t shape");
_Static_assert(sizeof(profile_persisted_v3_t) == 268,
               "profile_persisted_v3_t must stay exactly the v3 on-flash wrapper shape");
_Static_assert(offsetof(profile_persisted_v3_t, profile) == 4, "profile_t_v3 must start right after the padded version byte");
_Static_assert(offsetof(profile_persisted_v3_t, crc32) == 264, "crc32 must sit at the true tail of profile_persisted_v3_t");

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
    case 3: return sizeof(profile_persisted_v3_t);
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

/* v3 -> current: segment shape is IDENTICAL (profile_segment_t did not
 * change between v3 and v4), so segments copy element-for-element by value
 * -- no field-by-field walk needed here, unlike the v1/v2 converters above,
 * because there is no shape mismatch to guard against. The only new
 * behavior is the tail: on_off_rule_count = 0 and every rule slot zeroed,
 * which is the migration default profiles_types.h documents -- a v3
 * profile never had a rule, so this reproduces its old behavior exactly
 * (on_off_trigger_decide() sees rule.enable == false, precedence level 6). */
static void convert_profile_v3(const profile_persisted_v3_t *src, profile_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->name, src->profile.name, sizeof(out->name) - 1);
    out->zone_mask = src->profile.zone_mask;
    out->segment_count = src->profile.segment_count;
    for (uint8_t i = 0; i < src->profile.segment_count && i < PROFILE_MAX_SEGMENTS; i++) {
        out->segments[i] = src->profile.segments[i];
    }
    out->on_off_rule_count = 0; /* migration default -- see profiles_types.h */
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

/* profile_encode_current_blob() -- see profiles_http_internal.h. The one
 * place a profile_t is turned into the CURRENT-version on-flash wrapper,
 * shared by nvs_save_slot() below and profiles_cfg_fs.c's file writer, so a
 * file and an NVS blob are always byte-identical for the same profile_t. */
size_t profile_encode_current_blob(const profile_t *profile, void *out, size_t cap)
{
    if (!profile || !out || cap < sizeof(profile_persisted_t)) {
        return 0;
    }
    profile_persisted_t persisted = {
        .version = PROFILE_VERSION,
        .profile = *profile,
        .crc32 = 0,
    };
    persisted.crc32 = compute_profile_crc(&persisted);
    memcpy(out, &persisted, sizeof(persisted));
    return sizeof(persisted);
}

/* profile_decode_result_t is now declared in profiles_http_internal.h
 * (widened non-static, docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 4)
 * so profiles_cfg_fs.c can share this exact decode path for the `cfg`
 * filesystem file, not just the NVS blob -- see that header's comment. */

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
profile_decode_result_t profile_decode_blob(const void *blob, size_t len, profile_t *out,
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

    if (version == 3) {
        /* Current segment shape, crc32 tail, no on/off rules yet -- checked
         * the same way v2/v4 are, never a weaker gate for an older version. */
        profile_persisted_v3_t loaded_v3;
        memcpy(&loaded_v3, blob, sizeof(loaded_v3));
        uint32_t stored_crc = loaded_v3.crc32;
        profile_persisted_v3_t tmp = loaded_v3;
        tmp.crc32 = 0;
        uint32_t computed_crc = esp_crc32_le(0, (const uint8_t *)&tmp, sizeof(tmp));
        if (computed_crc != stored_crc) {
            *reason = "CRC mismatch -- treating as corrupt";
            return PROFILE_DECODE_CORRUPT;
        }
        convert_profile_v3(&loaded_v3, out);
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

/* "prof" + the id's decimal digits + NUL. Asserted rather than only trusted
 * in a comment, per docs/PROFILE_SLOTS_100_PLAN.md section 7 task 6 -- and
 * asserted against PROFILES_MAX_COUNT itself rather than against a literal
 * id, so a future raise of the slot count cannot quietly outgrow either
 * bound. The "+ 2" is the two reserved ids above the user range
 * (LIVE_EDIT_WORKING_SLOT_ID and PROFILE_BENCH_SLOT_ID), the same "+ 2"
 * profiles_types.h's own 128-id assert uses. Both call sites pass a
 * `char key[8]` buffer, which is exactly "prof" + 3 digits + NUL -- a
 * 4-digit id would silently TRUNCATE in snprintf() and alias two slots onto
 * one NVS key, which is what the first assert catches; the second is the
 * house NVS_KEY_LEN_CHECK() idiom applied to the longest key that buffer
 * can ever hold. */
_Static_assert(PROFILES_MAX_COUNT + 2 < 1000,
               "profile_nvs_key() formats into a char[8] (\"prof\" + 3 digits + NUL) -- "
               "a 4-digit id would truncate and alias two slots onto one NVS key");
NVS_KEY_LEN_CHECK("prof999");

static void profile_nvs_key(uint8_t id, char *out, size_t out_cap)
{
    snprintf(out, out_cap, "prof%u", id);
}

/* ---- NVS ---------------------------------------------------------------- */

esp_err_t nvs_save_slot(uint8_t id);

/* Brings up one NVS partition, erasing ONLY that partition if its contents
 * are unusable. Copied/adapted from wifi_prov.c's nvs_partition_init() (see
 * that file for the full rationale) -- NO_FREE_PAGES / NEW_VERSION_FOUND
 * have no other cure, so erasing is the only way forward, but the erase
 * must stay scoped to the partition that is actually broken rather than
 * blast-radius the default partition (or any other split-off partition)
 * with it. */
static esp_err_t nvs_partition_init(const char *partition)
{
    return hal_status_to_esp_err(hal_kv_init_partition(partition));
}

/* docs/PROFILE_SLOTS_100_PLAN.md section 7 task 6: NVS_KEY_USED used to be a
 * single uint8_t (8 bits, exactly PROFILES_MAX_COUNT's old value). Raising
 * PROFILES_MAX_COUNT to 100 needs all 4 words of profiles_slot_bitmap_t
 * persisted, not just word[0]. These two helpers are the read/write seam:
 * used_bitmap_load() accepts EITHER the new 16-byte blob OR a pre-existing
 * board's old single-byte value (migration, read-only -- the old key is
 * simply overwritten with the new 16-byte shape the next time anything
 * saves), and used_bitmap_save() always writes the full new shape.
 *
 * The two-branch dispatch (type-mismatch vs size-mismatch) exists because
 * the real ESP-IDF NVS backend enforces the on-flash type of a key (a
 * blob-typed read against a key written as U8 fails with
 * ESP_ERR_NVS_TYPE_MISMATCH, mapped to HAL_INVALID_ARG by
 * hal_kv_esp.c) while the host-test fake backend (hwAbstraction/host/
 * fake_kv.c) stores everything by raw size with no type tag, so the same
 * old byte instead comes back as a successful blob read of length 1. Both
 * are handled so the migration path is exercised the same way on host as
 * it will behave on target. */
static hal_status_t used_bitmap_load(hal_kv_handle_t *h, profiles_slot_bitmap_t *out)
{
    size_t len = 0;
    hal_status_t err = hal_kv_get_blob(h, NVS_KEY_USED, NULL, &len);
    if (err == HAL_NOT_FOUND) {
        return HAL_NOT_FOUND;
    }
    if (err == HAL_OK && len == sizeof(*out)) {
        size_t full_len = sizeof(*out);
        return hal_kv_get_blob(h, NVS_KEY_USED, out, &full_len);
    }
    /* Either a real-backend type mismatch (err == HAL_INVALID_ARG, the key
     * was written by a pre-task-6 hal_kv_set_u8()) or the host fake's
     * size-based equivalent (err == HAL_OK, len == 1) -- both mean "old
     * single-byte format", so fall back to reading it as one. */
    uint8_t legacy = 0;
    hal_status_t legacy_err = hal_kv_get_u8(h, NVS_KEY_USED, &legacy);
    if (legacy_err != HAL_OK) {
        return legacy_err;
    }
    profiles_slot_bitmap_from_u32(out, legacy);
    return HAL_OK;
}

/* Forward declaration: both of today's callers (nvs_save_slot(),
 * nvs_erase_slot()) already refuse up front when the calling task's stack is
 * in PSRAM, so the guard below is defence in depth -- but this is a real NVS
 * write call site in a PSRAM-stack-guarded module (DRAM_PSRAM_STATUS.md
 * section 7.2/9), and every such site in this file carries the same refusal
 * rather than relying on a caller a future edit could add without it. */
static bool caller_stack_is_external(void);

static hal_status_t used_bitmap_save(hal_kv_handle_t *h, const profiles_slot_bitmap_t *bm)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(PROFILES_TAG, "used_bitmap_save: REFUSING -- calling task's stack is in external RAM "
                      "(PSRAM); an NVS write from here would abort the whole board. See "
                      "DRAM_PSRAM_PLAN.md section 7.2.");
        return HAL_NOT_READY;
    }
    return hal_kv_set_blob(h, NVS_KEY_USED, bm, sizeof(*bm));
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

    hal_kv_handle_t h;
    hal_status_t kv_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, partition);
    if (kv_err == HAL_NOT_FOUND) {
        return ESP_OK; /* no kiln_cfg namespace on this partition yet -- nothing configured */
    }
    if (kv_err != HAL_OK) {
        return hal_status_to_esp_err(kv_err);
    }

    kv_err = used_bitmap_load(&h, &out->used_bitmap);
    esp_err_t err = hal_status_to_esp_err(kv_err);
    if (kv_err != HAL_OK && kv_err != HAL_NOT_FOUND) {
        hal_kv_close(&h);
        return err;
    }
    if (kv_err == HAL_OK) {
        if (out_any_found) {
            *out_any_found = true;
        }
    }

    /* Per-slot rev counters (see profiles_http_internal.h's s_profile_rev doc
     * comment) -- best-effort: a missing/short/corrupt blob leaves every
     * entry at 0, which is the correct default for a slot that has never
     * been through the file bridge before (an all-zero rev array on a board
     * upgrading to this firmware for the first time just means "no opinion
     * yet," not corruption). Read into a LOCAL array first; s_profile_rev is
     * only updated once, after the resolve pass below, so a load failure
     * partway through never leaves s_profile_rev half from-NVS/half-stale. */
    uint32_t nvs_rev[PROFILES_MAX_COUNT];
    memset(nvs_rev, 0, sizeof(nvs_rev));
    size_t rev_len = sizeof(nvs_rev);
    hal_kv_get_blob(&h, NVS_KEY_PROFILE_REV, nvs_rev, &rev_len); /* ignore result -- see above */

    bool nvs_slot_valid[PROFILES_MAX_COUNT] = {0};

    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        if (!profiles_slot_bitmap_test(&out->used_bitmap, id)) {
            continue;
        }
        char key[8];
        profile_nvs_key(id, key, sizeof(key));
        profile_persisted_t loaded;
        size_t len = sizeof(loaded);
        hal_status_t slot_kv_err = hal_kv_get_blob(&h, key, &loaded, &len);
        if (slot_kv_err != HAL_OK) {
            ESP_LOGW(PROFILES_TAG, "prof%u load from '%s' failed (%s) -- marking unused", id, partition,
                     hal_status_to_name(slot_kv_err));
            profiles_slot_bitmap_clear(&out->used_bitmap, id);
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
        profile_decode_result_t dres = profile_decode_blob(&loaded, len, &decoded, &reason);
        switch (dres) {
        case PROFILE_DECODE_OK:
            out->profiles[id] = decoded;
            nvs_slot_valid[id] = true;
            break;
        case PROFILE_DECODE_NEWER:
            ESP_LOGW(PROFILES_TAG, "prof%u load from '%s' refused: %s", id, partition, reason);
            profiles_slot_bitmap_clear(&out->used_bitmap, id);
            continue;
        case PROFILE_DECODE_CORRUPT:
        default:
            ESP_LOGW(PROFILES_TAG, "prof%u load from '%s' rejected: %s -- marking unused", id, partition, reason);
            profiles_slot_bitmap_clear(&out->used_bitmap, id);
            continue;
        }
    }

    hal_kv_close(&h);

    /* docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 4: read-through/
     * dual-write resolve against the `cfg` filesystem, one slot at a time.
     * Only meaningful for the real profiles partition -- this function is
     * never actually called with any other partition today (the pre-split
     * migration path reads the default partition directly, not through
     * here), but the guard keeps that assumption explicit rather than
     * silently relying on it. cfg_fs_is_available() is false on every board
     * today (no `cfg` partition mounted yet), so profiles_cfg_fs_resolve()
     * degrades to "trust whatever NVS decoded" for every slot, unchanged
     * from this function's pre-existing behavior. */
    if (strcmp(partition, PROFILES_NVS_PARTITION) == 0) {
        for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
            profile_t resolved;
            uint32_t resolved_rev = 0;
            bool used_file = false;
            bool trustworthy = profiles_cfg_fs_resolve(id, &out->profiles[id], nvs_slot_valid[id], nvs_rev[id],
                                                        &resolved, &resolved_rev, &used_file);
            if (trustworthy) {
                out->profiles[id] = resolved;
                profiles_slot_bitmap_set(&out->used_bitmap, id);
            } else {
                memset(&out->profiles[id], 0, sizeof(out->profiles[id]));
                profiles_slot_bitmap_clear(&out->used_bitmap, id);
            }
            s_profile_rev[id] = resolved_rev;
        }
    } else {
        memset(s_profile_rev, 0, sizeof(s_profile_rev));
    }

    return ESP_OK;
}

/* True iff the CURRENTLY EXECUTING task's own stack lives in external RAM
 * (PSRAM). Same predicate/reasoning as safety_cfg_store.c's
 * caller_stack_is_external() and kiln_cfg_store.c's copy of it: a flash/NVS
 * write disables the cache, and a PSRAM-resident stack becomes unreachable
 * while it is down, aborting the whole board via ESP-IDF's own
 * esp_task_stack_is_sane_cache_disabled() rather than failing just this one
 * call. See DRAM_PSRAM_PLAN.md section 7.2. All of today's callers
 * (control_task et al via uart_bridge_ext.c's bx_run_on_internal_stack(),
 * and the httpd worker directly) already run on internal-stack tasks; this
 * refuses loudly instead of crashing the board if a future caller does not.
 *
 * HW_ABSTRACTION.md Phase 3 item 3: now delegates to
 * hal_kv_write_safe_here() (this module's own negation of it) instead of
 * probing esp_ptr_external_ram() locally -- same predicate, one definition. */
static bool caller_stack_is_external(void)
{
    return !hal_kv_write_safe_here();
}

esp_err_t nvs_save_slot(uint8_t id)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(PROFILES_TAG, "nvs_save_slot: REFUSING -- calling task's stack is in external RAM "
                      "(PSRAM). A flash/NVS write from here would abort the whole board "
                      "(ESP-IDF's esp_task_stack_is_sane_cache_disabled()). Route this call "
                      "through a task with an internal-SRAM stack instead -- see "
                      "DRAM_PSRAM_PLAN.md section 7.2 and uart_bridge_ext.c's flash-safe "
                      "worker for the established pattern.");
        return ESP_ERR_INVALID_STATE;
    }
    /* FILE FIRST, then NVS (docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step
     * 4 requirement 1). A file write failure is logged and swallowed here --
     * profiles_cfg_fs_save() already does that logging -- NVS below remains
     * the persistence guarantee every existing caller of nvs_save_slot()
     * already depends on; a subsequent NVS write failure is a hard error
     * (ESP_LOGE below, exactly as before this pass) even though the profile
     * is still applied live in RAM, same convention as before. */
    uint32_t new_rev = s_profile_rev[id] + 1;
    (void)profiles_cfg_fs_save(id, &s_profiles.profiles[id], new_rev);

    hal_kv_handle_t h;
    hal_status_t kv_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, PROFILES_NVS_PARTITION);
    if (kv_err != HAL_OK) {
        return hal_status_to_esp_err(kv_err);
    }
    char key[8];
    profile_nvs_key(id, key, sizeof(key));
    profile_persisted_t persisted = {
        .version = PROFILE_VERSION,
        .profile = s_profiles.profiles[id],
        .crc32 = 0,
    };
    persisted.crc32 = compute_profile_crc(&persisted);
    kv_err = hal_kv_set_blob(&h, key, &persisted, sizeof(persisted));
    if (kv_err == HAL_OK) {
        kv_err = used_bitmap_save(&h, &s_profiles.used_bitmap);
    }
    /* Update the in-RAM rev in place and persist s_profile_rev itself rather
     * than a stack copy of it: at PROFILES_MAX_COUNT == 100 that copy was a
     * 400 B local (32 B at the old 8 slots) and it pushed nvs_erase_slot()'s
     * twin of this block over bx_flash_worker's stack ceiling
     * (check_all_task_stack_budgets.ps1). The persisted bytes are identical
     * -- the snapshot only ever differed from s_profile_rev by this one
     * element, which is assigned here instead. Assigning before the write
     * rather than after it is also behaviour-identical: the old code
     * assigned unconditionally once it got past hal_kv_open(), which is the
     * only early return above this point.
     *
     * The in-RAM rev is updated regardless of NVS outcome: it is ephemeral for
     * this boot only (a reboot re-derives it from whatever actually got
     * persisted, via nvs_load_all_from()'s resolve pass), and keeping it in
     * lockstep with the file (already written above) means a subsequent
     * save/delete this boot bumps from the true latest rev instead of
     * replaying an already-used one. */
    s_profile_rev[id] = new_rev;
    if (kv_err == HAL_OK) {
        kv_err = hal_kv_set_blob(&h, NVS_KEY_PROFILE_REV, s_profile_rev, sizeof(s_profile_rev));
    }
    if (kv_err == HAL_OK) {
        kv_err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(kv_err);
}

/* DRAM_PSRAM_PLAN.md section 9 write-path re-audit (2026-09-02): this
 * function writes NVS (nvs_set_u8()/nvs_commit() below) exactly like
 * nvs_save_slot() just above, but never got that function's
 * caller_stack_is_external() guard -- the earlier pass treated "this file
 * has the guard" as true of the file's whole write surface, not just the
 * one call site it added it to. Both of today's callers (delete-profile HTTP
 * handlers) run on httpd_worker, an internal-SRAM stack, so this cannot fire
 * the crash today; added so a future audit does not read this file as fully
 * covered when it was not. */
esp_err_t nvs_erase_slot(uint8_t id)
{
    if (caller_stack_is_external()) {
        ESP_LOGE(PROFILES_TAG, "nvs_erase_slot: REFUSING -- calling task's stack is in external RAM "
                      "(PSRAM). See nvs_save_slot()'s guard comment in this file and "
                      "DRAM_PSRAM_PLAN.md section 7.2/9.");
        return ESP_ERR_INVALID_STATE;
    }
    /* A delete is a mutation of this slot's rev too (docs/
     * FILESYSTEM_USER_DATA_PLAN.md section 5 step 4 requirement 1 /
     * profiles_cfg_fs.h's header comment) -- bumping it here, and deleting
     * the file FIRST, is what lets profiles_cfg_fs_resolve() tell "this
     * slot was legitimately deleted" apart from "a save's NVS write failed
     * after its file write succeeded" on the next boot. */
    uint32_t new_rev = s_profile_rev[id] + 1;
    (void)profiles_cfg_fs_delete(id); /* logged internally on failure, best-effort */
    /* docs/PROFILE_SLOTS_100_PLAN.md section 7 task 10: prune this id's
     * firing history too, best-effort, same "delete must not itself fail"
     * contract as the cfg-fs delete just above -- see firing_stats_erase()'s
     * own doc comment (profile_executor.h) for why this matters once ids
     * start being reused at higher slot counts. */
    firing_stats_erase(id);

    hal_kv_handle_t h;
    hal_status_t kv_err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, PROFILES_NVS_PARTITION);
    if (kv_err != HAL_OK) {
        return hal_status_to_esp_err(kv_err);
    }
    char key[8];
    profile_nvs_key(id, key, sizeof(key));
    hal_status_t erase_err = hal_kv_erase_key(&h, key);
    if (erase_err != HAL_OK && erase_err != HAL_NOT_FOUND) {
        hal_kv_close(&h);
        return hal_status_to_esp_err(erase_err);
    }
    kv_err = used_bitmap_save(&h, &s_profiles.used_bitmap);
    /* ephemeral this boot, see nvs_save_slot()'s identical comment -- and see
     * that function for why s_profile_rev is updated in place and persisted
     * directly instead of through a PROFILES_MAX_COUNT-sized stack copy. */
    s_profile_rev[id] = new_rev;
    if (kv_err == HAL_OK) {
        kv_err = hal_kv_set_blob(&h, NVS_KEY_PROFILE_REV, s_profile_rev, sizeof(s_profile_rev));
    }
    if (kv_err == HAL_OK) {
        kv_err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(kv_err);
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
    /* NULL partition == hal_kv's default-partition selector, matching
     * nvs_open()'s shape -- this used to be the explicit
     * nvs_open_from_partition(NVS_DEFAULT_PART_NAME, ...) form (that macro
     * expands to "nvs", the same partition), see hal_kv.h's own header
     * comment on partition==NULL. */
    hal_kv_handle_t old_h;
    hal_status_t kv_err = hal_kv_open(&old_h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, NULL);
    if (kv_err != HAL_OK) {
        return; /* no kiln_cfg namespace on the default partition -- nothing to migrate */
    }

    uint8_t old_bitmap = 0;
    kv_err = hal_kv_get_u8(&old_h, NVS_KEY_USED, &old_bitmap);
    if (kv_err != HAL_OK || old_bitmap == 0) {
        hal_kv_close(&old_h);
        return; /* nothing recorded as used in the old location */
    }

    ESP_LOGI(PROFILES_TAG, "migrating fire profiles from the default NVS partition to '%s'", PROFILES_NVS_PARTITION);

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
        hal_status_t slot_err = hal_kv_get_blob(&old_h, key, &old_profile, &len);
        if (slot_err != HAL_OK || len != sizeof(old_profile)) {
            ESP_LOGW(PROFILES_TAG,
                     "prof%u migration read failed or wrong size (%s) -- skipping this slot, others still "
                     "attempted",
                     id, hal_status_to_name(slot_err));
            continue;
        }

        s_profiles.profiles[id] = old_profile;
        profiles_slot_set(id);
        esp_err_t save_err = nvs_save_slot(id);
        if (save_err != ESP_OK) {
            ESP_LOGE(PROFILES_TAG,
                     "prof%u migration write to '%s' failed: %s -- running from the old copy this boot, will "
                     "retry",
                     id, PROFILES_NVS_PARTITION, esp_err_to_name(save_err));
            /* Don't let a failed write claim the slot as migrated in RAM --
             * a write failure on this slot must not affect any other. */
            profiles_slot_clear(id);
            memset(&s_profiles.profiles[id], 0, sizeof(s_profiles.profiles[id]));
        }
    }

    hal_kv_close(&old_h);
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

    if (id >= PROFILES_MAX_COUNT || !profiles_slot_used(id)) {
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
bool validate_io_segment(const profile_segment_t *seg, uint8_t seg_num, char *err_msg, size_t err_cap)
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
        /* Owner decision 2026-10-04 (plan sec 14 item 10): a relay bound to an
         * ENABLED aux output belongs to the aux evaluator; a RELAY_IO segment
         * on it would fight the aux rule. Refused at save. */
        if ((aux_outputs_cfg_enabled_mask() & (uint8_t)(1u << (t - 1u))) != 0) {
            snprintf(err_msg, err_cap,
                    "segment %u: relay %u is bound to an aux output -- only relays not owned by a zone "
                    "or an aux output can be a segment target",
                    seg_num, t);
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

/* docs/ON_OFF_ZONE_PLAN.md plan step 5 validation -- shared by
 * profiles_http_save() (both the HTTP POST and UART-bridge entry points)
 * so a rule referencing a nonexistent segment or a non-ON_OFF zone can
 * never be persisted, regardless of entry point. THIS IS THE DANGEROUS
 * DIRECTION this check exists to close: a rule pointing at a HEATER zone
 * would let on/off (bang-bang, no PID, no guards 1-4/9) logic drive a real
 * heating element -- see this function's own negative test. Bounds every
 * numeric field so a corrupt/hand-crafted candidate cannot smuggle an
 * out-of-range value past decode. */
bool validate_on_off_rules(const profile_t *candidate, char *err_msg, size_t err_cap)
{
    for (uint8_t i = 0; i < candidate->on_off_rule_count; i++) {
        const profile_on_off_rule_t *r = &candidate->on_off_rules[i];
        if (i >= PROFILE_MAX_ON_OFF_RULES) {
            snprintf(err_msg, err_cap, "rule %u: on_off_rule_count exceeds PROFILE_MAX_ON_OFF_RULES (%u)", i,
                     (unsigned)PROFILE_MAX_ON_OFF_RULES);
            return false;
        }
        if (r->segment_index >= candidate->segment_count) {
            snprintf(err_msg, err_cap, "rule %u: segment_index %u does not exist in this profile (%u segments)",
                     i, r->segment_index, candidate->segment_count);
            return false;
        }
        for (uint8_t j = 0; j < i; j++) {
            /* At most one rule per (segment, target): the executor's resolver takes the
             * first match and never looks for a second (profile_executor.c). */
            if (candidate->on_off_rules[j].segment_index == r->segment_index &&
                candidate->on_off_rules[j].zone_index == r->zone_index) {
                snprintf(err_msg, err_cap, "rule %u: duplicates rule %u (segment %u, target %u) -- one rule per "
                         "segment and target", i, j, r->segment_index, r->zone_index);
                return false;
            }
        }
        if (profile_rule_target_is_aux(r->zone_index)) {
            /* Aux target (plan sec 6): the aux entry for that relay must be enabled
             * and not conflicted; a temperature axis needs a valid tc_zone and the
             * "this zone's TC" source (1 = the entry's tc_zone). Sources 2/3 stay
             * reserved. */
            uint8_t relay = profile_rule_target_aux_relay(r->zone_index);
            aux_output_t ax;
            if (!aux_outputs_cfg_get(relay, &ax)) {
                snprintf(err_msg, err_cap, "rule %u: aux relay %u cannot be read", i, relay);
                return false;
            }
            if (ax.conflicted) {
                snprintf(err_msg, err_cap,
                         "rule %u: aux relay %u is conflicted (a zone also claims that relay) -- resolve it on the "
                         "zones page first",
                         i, relay);
                return false;
            }
            if (!ax.enabled) {
                snprintf(err_msg, err_cap,
                         "rule %u: aux relay %u is not an enabled aux output -- enable it on the zones page first",
                         i, relay);
                return false;
            }
            if (r->temp_source > 1) {
                snprintf(err_msg, err_cap, "rule %u: temp_source %u is reserved for aux targets (use 0 or 1)", i,
                         r->temp_source);
                return false;
            }
            if (r->temp_cmp != ON_OFF_TEMP_CMP_NONE &&
                (r->temp_source != 1 || ax.tc_zone == AUX_TC_ZONE_NONE)) {
                snprintf(err_msg, err_cap,
                         "rule %u: aux relay %u temperature rule needs temp_source 1 and a thermocouple zone "
                         "set on the aux output",
                         i, relay);
                return false;
            }
        } else {
            if (r->zone_index >= MAX31856_CHANNEL_COUNT) {
                snprintf(err_msg, err_cap,
                         "rule %u: zone_index %u out of range (zones 0-%u, aux relays %u-%u)", i, r->zone_index,
                         (unsigned)(MAX31856_CHANNEL_COUNT - 1u), (unsigned)PROFILE_RULE_TARGET_AUX_BASE,
                         (unsigned)(PROFILE_RULE_TARGET_AUX_BASE + PROFILE_RULE_TARGET_AUX_COUNT - 1u));
                return false;
            }
            zone_type_t zt = ZONE_TYPE_HEATER;
            if (!zones_config_get_zone_type(r->zone_index, &zt) || zt != ZONE_TYPE_ON_OFF) {
                /* THE dangerous direction: refuse a rule aimed at anything that
                 * is not (already, currently) a typed on/off device -- most
                 * importantly a HEATER, which on/off logic must never drive. */
                snprintf(err_msg, err_cap,
                         "rule %u: zone %u is not configured as an on/off device -- refusing to let on/off logic "
                         "drive it",
                         i, r->zone_index);
                return false;
            }
        }
        if ((r->phase_mask & (uint8_t)~(ON_OFF_PHASE_RAMP | ON_OFF_PHASE_DWELL)) != 0) {
            snprintf(err_msg, err_cap, "rule %u: phase_mask has unknown bits set", i);
            return false;
        }
        if ((r->direction_mask & (uint8_t)~(ON_OFF_DIR_HEATING | ON_OFF_DIR_COOLING | ON_OFF_DIR_FLAT)) != 0) {
            snprintf(err_msg, err_cap, "rule %u: direction_mask has unknown bits set", i);
            return false;
        }
        if (r->temp_cmp > ON_OFF_TEMP_CMP_BELOW) {
            snprintf(err_msg, err_cap, "rule %u: temp_cmp %u is not a known comparison", i, r->temp_cmp);
            return false;
        }
        if (r->temp_source > 3) {
            snprintf(err_msg, err_cap, "rule %u: temp_source %u is not a known source", i, r->temp_source);
            return false;
        }
        if (r->temp_source == 2 && r->temp_ref_zone >= MAX31856_CHANNEL_COUNT) {
            snprintf(err_msg, err_cap, "rule %u: temp_ref_zone %u out of range", i, r->temp_ref_zone);
            return false;
        }
        /* TODO (Opus review N8): the catalog/export/edit HTTP handlers do not
         * yet round-trip temp_ref_zone at all -- this bounds check is the
         * only place today that knows the field exists. When temp_source==2
         * (an explicit reference-zone thermocouple, distinct from
         * temp_source 0/1) is actually wired up end to end, add
         * temp_ref_zone to profiles_catalog_http.c's JSON output,
         * profiles_export_http.c's export/import, and profiles_edit_http.c's
         * form parser in the SAME change -- adding it to only one leaves the
         * others silently dropping or defaulting the field. */
        if (r->temp_cmp != ON_OFF_TEMP_CMP_NONE &&
            (isnan(r->temp_threshold_c) || r->temp_threshold_c < PROFILE_TARGET_C_MIN ||
             r->temp_threshold_c > PROFILE_TARGET_C_MAX)) {
            snprintf(err_msg, err_cap, "rule %u: temp_threshold_c out of range (%.0f-%.0f)", i,
                     (double)PROFILE_TARGET_C_MIN, (double)PROFILE_TARGET_C_MAX);
            return false;
        }
        /* time_stop_s == 0 means "to end of segment" (profiles_types.h) --
         * only a NONZERO stop must be after start. Both fields are
         * uint16_t, so an upper bound is enforced structurally already
         * (max 65535 s ~ 18.2h, comfortably above PROFILE_DWELL_MIN_MAX's
         * 1440 minutes/segment); no separate range check needed. */
        if (r->time_stop_s != 0 && r->time_stop_s <= r->time_start_s) {
            snprintf(err_msg, err_cap, "rule %u: time_stop_s must be after time_start_s (or 0 for end-of-segment)",
                     i);
            return false;
        }
        if (r->enable > 1 || r->invert > 1) {
            snprintf(err_msg, err_cap, "rule %u: enable/invert must be 0 or 1", i);
            return false;
        }
    }
    return true;
}

/* True iff some ZONE_RAMP segment's target_c exceeds the CURRENTLY configured
 * max_temp_c of one of its zone_mask zones, and fills `note` (if non-NULL)
 * describing the first offending segment/zone found -- same shape as the
 * refusal message this replaces (2026-09-02 owner correction, see the SAFETY
 * TASK / GAS-KILN comment on PROFILE_TARGET_C_MAX above): a profile whose
 * targets exceed the zone ceiling is now a legitimate, SAVEABLE thing (a
 * gas-kiln or cone-10 profile authored on a low-temperature bench rig --
 * profiles are portable between kilns, the ceiling is a property of the
 * installation, not the profile), so this is advisory, not a gate. The
 * actual enforcement point stays profile_executor_run.c's run-start
 * re-check against the zone's CURRENT ceiling -- this function exists only
 * to make the condition VISIBLE at save/list/edit time instead of silent
 * until a failed start. A zone with max_temp_c == 0 (never commissioned) is
 * skipped -- it has no real ceiling to compare against yet. */
bool profile_exceeds_zone_ceiling(const profile_t *p, char *note, size_t note_cap)
{
    for (uint8_t i = 0; i < p->segment_count; i++) {
        if (p->segments[i].seg_kind != PROFILE_SEG_KIND_ZONE_RAMP) {
            continue;
        }
        float target = p->segments[i].target_c;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (!(p->zone_mask & (1u << zi))) {
                continue;
            }
            float zone_max_c = 0.0f, zone_min_c = 0.0f;
            zones_config_get_temp_limits(zi, &zone_max_c, &zone_min_c);
            if (!(zone_max_c > 0.0f)) {
                continue; /* uncommissioned zone -- nothing real to compare against */
            }
            if (target > zone_max_c) {
                if (note && note_cap > 0) {
                    snprintf(note, note_cap,
                            "segment %u: target %.1fC exceeds zone %u's %.1fC limit -- cannot run here",
                            i + 1, (double)target, zi, (double)zone_max_c);
                }
                return true;
            }
        }
    }
    if (note && note_cap > 0) {
        note[0] = '\0';
    }
    return false;
}

/* live_edit_name_collides()'s name_at seam, backed directly by s_profiles --
 * same pattern profiles_live_http.c's live_http_name_at() already uses. */
static const char *profiles_http_name_at(void *ctx, uint8_t id)
{
    (void)ctx;
    if (id >= PROFILES_MAX_COUNT || !profiles_slot_used(id)) {
        return NULL;
    }
    return s_profiles.profiles[id].name;
}

/* profiles_http_first_free_slot()'s predicate callback (Opus review nit N5)
 * for the live-board case: ctx is unused, this just forwards to
 * profiles_slot_used(). */
static bool profiles_http_slot_used_cb(void *ctx, uint8_t id)
{
    (void)ctx;
    return profiles_slot_used(id);
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
            snprintf(err_msg, err_cap, "segment %u: target_c out of range (%.0f-%.0f)", i + 1,
                     (double)PROFILE_TARGET_C_MIN, (double)PROFILE_TARGET_C_MAX);
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
    if (candidate->on_off_rule_count > PROFILE_MAX_ON_OFF_RULES) {
        snprintf(err_msg, err_cap, "on_off_rule_count out of range (0-%u)", (unsigned)PROFILE_MAX_ON_OFF_RULES);
        return false;
    }
    if (!validate_on_off_rules(candidate, err_msg, err_cap)) {
        return false;
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
        /* profiles_http_first_free_slot() (Opus review nit N5) -- shared with
         * backup_import.c's pass-1 commit simulation so the two allocation
         * scans cannot silently drift apart. A callback straight onto
         * profiles_slot_used(), not a materialized bool[PROFILES_MAX_COUNT]
         * array -- see the helper's own comment: that array once pushed
         * bx_flash_worker over its stack ceiling. */
        int free_slot = profiles_http_first_free_slot(profiles_http_slot_used_cb, NULL);
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

    /* OWNER CORRECTION (2026-09-02): this pass previously REFUSED to save a
     * profile whose target_c exceeded the participating zone's configured
     * max_temp_c ("you should be able to put in firing profiles that
     * exceed kiln max but not run them if they go beyond kiln max" -- a
     * cone-10 or gas-kiln profile authored on a low-temp bench rig is
     * legitimate; profiles are portable between kilns, and the kiln's
     * ceiling is a property of the installation, not of the profile). Save
     * now only WARNS (via profile_exceeds_zone_ceiling(), same helper the
     * profile list/detail endpoints use to surface this at edit time too);
     * it never refuses and never clamps. The single enforcement point stays
     * profile_executor_run.c's run-start re-check against each zone's
     * CURRENT ceiling -- unchanged by this pass, and it is what actually
     * stops the run. */
    if (profile_exceeds_zone_ceiling(candidate, NULL, 0)) {
        warn_count++;
    }

    /* Owner request 2026-09-19: saving must never silently create/overwrite a
     * duplicate name. Reuses live_edit_name_collides_ex() (live_profile.c),
     * same case/whitespace normalization as the live-edit SAVE_AS path.
     * exclude_id is the slot this save is writing into: overwriting an
     * existing slot's OWN unchanged name must stay legal, so that slot is
     * excluded from the scan. Passing target_id directly (no
     * profiles_slot_used() ternary) is enough: for a brand-new/unused slot,
     * profiles_http_name_at() already returns NULL, which
     * live_edit_name_collides_ex()'s own `if (!existing) continue;` skips
     * regardless of exclude_id's value.
     *
     * include_builtins=false (Opus review of 5dd23944, finding 1/BLOCKER):
     * this writes a USER slot, and "SAVE-VS-COPY" above already redirects a
     * save aimed at a builtin id into a fresh user slot -- refusing that new
     * slot for merely sharing the builtin's name/code would defeat the
     * copy-a-builtin feature entirely. See live_edit_name_collides_ex()'s
     * doc comment. */
    if (live_edit_name_collides_ex(candidate->name, profiles_http_name_at, NULL, target_id, false, err_msg,
                                    err_cap)) {
        return false; /* live_edit_name_collides_ex already filled err_msg */
    }

    s_profiles.profiles[target_id] = *candidate;
    profiles_slot_set(target_id);
    esp_err_t err = nvs_save_slot(target_id);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "nvs_save_slot(%u) failed: %s -- profile applied live but will not survive a reboot",
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
    if (id >= PROFILES_MAX_COUNT || !profiles_slot_used(id)) {
        return false;
    }
    /* Opus review item 2 (PROFILE_SLOTS_100_PLAN.md section 7): refuse to
     * delete a slot the executor is currently running or has paused --
     * deleting it out from under an in-progress firing would leave
     * profile_executor_run()'s copied-at-start name/segments as the only
     * surviving record of what is actually executing, and a later re-save
     * of this id would silently relabel that run's history. Same check as
     * profiles_edit_http.c's web delete handler. */
    /* Only "is this id currently running/paused" is needed here -- use the
     * narrow accessor profile_executor.h recommends over a 1384-byte
     * profile_exec_status_t stack local. */
    uint8_t active_id = 0;
    if (profile_executor_get_active_id(&active_id) && active_id == id) {
        return false;
    }
    /* Clear the favorite mark BEFORE erasing the slot (review fold-in,
     * PROFILE_SLOTS_100_PLAN.md section 7): erase-then-clear left a window
     * where a power cut between the two steps could survive with the slot
     * erased but its favorite bit still set -- an import that later lands on
     * this same id inherits that orphaned favorite (profiles_favorites.h's
     * lifecycle keeps favorites across import). Clearing first means the
     * worst a power cut can leave behind is an erased-but-still-favorited
     * slot that gets cleaned up the next time this id is reused, never an
     * orphan bit surviving into a fresh profile. Best-effort: a failed save
     * is logged inside the module and must not block the delete. */
    (void)profiles_favorites_set((uint8_t)id, false);
    profiles_slot_clear(id);
    memset(&s_profiles.profiles[id], 0, sizeof(s_profiles.profiles[id]));
    esp_err_t err = nvs_erase_slot((uint8_t)id);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "nvs_erase_slot(%u) failed: %s -- deleted live but may reappear after reboot", id,
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

esp_err_t profiles_http_start(void)
{
    /* profiles_nvs is used only by this module, but nvs_flash_init_partition()
     * on an already-initialized partition is a harmless no-op (ESP_OK), so
     * bringing it up here independently (rather than assuming some other
     * module did it) is safe either way. */
    esp_err_t part_err = nvs_partition_init(PROFILES_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "NVS init for '%s' failed: %s -- profiles will not persist", PROFILES_NVS_PARTITION,
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
        ESP_LOGW(PROFILES_TAG, "profile NVS load failed: %s -- starting with no saved profiles", esp_err_to_name(err));
        memset(&s_profiles, 0, sizeof(s_profiles));
    }

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(PROFILES_TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/profiles", .method = HTTP_GET, .handler = profiles_page_get_handler,
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
    static const httpd_uri_t favorites_list_uri = {
        .uri = "/api/profiles/favorites", .method = HTTP_GET, .handler = favorites_list_get_handler,
    };
    static const httpd_uri_t favorite_post_uri = {
        .uri = "/api/profile/favorite", .method = HTTP_POST, .handler = profile_favorite_post_handler,
    };
    err = kiln_http_register(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "httpd_register_uri_handler(/profiles) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = kiln_http_register(server, &list_uri);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "httpd_register_uri_handler(GET /api/profiles) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = kiln_http_register(server, &detail_uri);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "httpd_register_uri_handler(GET /api/profile) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = kiln_http_register(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "httpd_register_uri_handler(POST /api/profile) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = kiln_http_register(server, &delete_uri);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "httpd_register_uri_handler(POST /api/profile/delete) failed: %s", esp_err_to_name(err));
        return err;
    }

    err = kiln_http_register(server, &builtin_list_uri);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "httpd_register_uri_handler(GET /api/profiles/builtin) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = kiln_http_register(server, &builtin_hide_uri);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "httpd_register_uri_handler(POST /api/profile/builtin/hide) failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = kiln_http_register(server, &builtin_restore_uri);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "httpd_register_uri_handler(POST /api/profile/builtin/restore) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    err = kiln_http_register(server, &favorites_list_uri);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "httpd_register_uri_handler(GET /api/profiles/favorites) failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = kiln_http_register(server, &favorite_post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(PROFILES_TAG, "httpd_register_uri_handler(POST /api/profile/favorite) failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(PROFILES_TAG, "profiles API up (storage/validation; execution runs in profile_executor.c)");
    return ESP_OK;
}

void profiles_http_get_dualwrite_status(uint8_t id, bool *file_valid, uint32_t *file_rev, bool *nvs_valid,
                                         uint32_t *nvs_rev, bool *diverged)
{
    if (file_valid) {
        *file_valid = false;
    }
    if (file_rev) {
        *file_rev = 0;
    }
    if (nvs_valid) {
        *nvs_valid = false;
    }
    if (nvs_rev) {
        *nvs_rev = 0;
    }
    if (diverged) {
        *diverged = false;
    }
    if (id >= PROFILES_MAX_COUNT) {
        return;
    }

    /* 2026-09-23 httpd-stack fix: this function used to carry two profile_t
     * locals, a profile_persisted_t, a decoded profile_t and a
     * PROFILES_MAX_COUNT-uint32_t revs[] array all as plain stack locals
     * (~1840 B), making it the single largest frame on
     * cfgfs_status_get_handler's path -- the deepest reachable httpd_worker
     * path measured by check_httpd_task_stack_budget.py. Bundled into one
     * heap allocation instead, freed on every return path, same convention
     * as this file's other malloc'd scratch structs (e.g. kiln_cfg_import_
     * scratch_t). Never enlarges any buffer -- same fields, same sizes, just
     * off the 8 KB httpd stack. */
    struct dualwrite_scratch {
        profile_t f_profile;
        profile_t n_profile;
        profile_persisted_t loaded;
        profile_t decoded;
        uint32_t revs[PROFILES_MAX_COUNT];
    };
    /* 2026-10-04: PSRAM first (read-only status scratch; plain malloc <= 8 KB
     * is internal RAM and this runs per slot under GET /api/cfgfs). */
    struct dualwrite_scratch *s = persist_scratch_alloc(sizeof(*s));
    if (!s) {
        ESP_LOGE(PROFILES_TAG, "profiles_http_get_dualwrite_status: malloc failed -- reporting unknown");
        return;
    }
    memset(s, 0, sizeof(*s));

    uint32_t f_rev = 0;
    bool f_valid = false;
    profiles_cfg_fs_load_raw(id, &s->f_profile, &f_rev, &f_valid);

    bool n_valid = false;
    uint32_t n_rev = 0;
    if (nvs_partition_init(PROFILES_NVS_PARTITION) == HAL_OK) {
        hal_kv_handle_t h;
        if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, PROFILES_NVS_PARTITION) == HAL_OK) {
            char key[8];
            profile_nvs_key(id, key, sizeof(key));
            size_t len = sizeof(s->loaded);
            if (hal_kv_get_blob(&h, key, &s->loaded, &len) == HAL_OK) {
                const char *reason = "";
                if (profile_decode_blob(&s->loaded, len, &s->decoded, &reason) == PROFILE_DECODE_OK) {
                    n_valid = true;
                    s->n_profile = s->decoded;
                }
            }
            size_t rev_len = sizeof(s->revs);
            if (hal_kv_get_blob(&h, NVS_KEY_PROFILE_REV, s->revs, &rev_len) == HAL_OK) {
                n_rev = s->revs[id];
            }
            hal_kv_close(&h);
        }
    }

    bool content_equal = f_valid && n_valid && (memcmp(&s->f_profile, &s->n_profile, sizeof(s->f_profile)) == 0);
    if (file_valid) {
        *file_valid = f_valid;
    }
    if (file_rev) {
        *file_rev = f_rev;
    }
    if (nvs_valid) {
        *nvs_valid = n_valid;
    }
    if (nvs_rev) {
        *nvs_rev = n_rev;
    }
    if (diverged) {
        *diverged = cfg_fs_status_item_diverged(f_valid, n_valid, content_equal);
    }
    free(s);
}

/* ---- Zone -> aux rule retarget (docs/SPARE_RELAY_ONOFF_PLAN.md section 10) ----
 *
 * One transform, two directions: a rule whose zone_index is `from` gets `to`,
 * nothing else changes. Because it is an exact byte swap and the plan refuses
 * any slot that already carries a rule at the destination, the reverse swap
 * restores the original profile bit-exactly -- the commit's rollback needs no
 * saved copy of the old profiles. */
static uint8_t retarget_rule_limit(const profile_t *p)
{
    return p->on_off_rule_count > PROFILE_MAX_ON_OFF_RULES ? (uint8_t)PROFILE_MAX_ON_OFF_RULES
                                                           : p->on_off_rule_count;
}

static uint16_t retarget_swap_rules(profile_t *p, uint8_t from, uint8_t to)
{
    uint16_t n = 0;
    uint8_t cnt = retarget_rule_limit(p);
    for (uint8_t i = 0; i < cnt; i++) {
        if (p->on_off_rules[i].zone_index == from) {
            p->on_off_rules[i].zone_index = to;
            n++;
        }
    }
    return n;
}

static uint16_t retarget_count_rules(const profile_t *p, uint8_t target)
{
    uint16_t n = 0;
    uint8_t cnt = retarget_rule_limit(p);
    for (uint8_t i = 0; i < cnt; i++) {
        if (p->on_off_rules[i].zone_index == target) {
            n++;
        }
    }
    return n;
}

/* allow_dest: a resume, where an earlier run already moved some slots, so rules at the destination
 * are expected and not a refusal. */
static bool retarget_plan(uint8_t zone, uint8_t relay, bool zone_has_tc, bool allow_dest,
                          profiles_retarget_counts_t *counts, char *err, size_t err_cap)
{
    profiles_retarget_counts_t c = {0};
    uint8_t dest = profile_rule_target_from_aux_relay(relay);
    if (dest == 0xFFu || zone >= MAX31856_CHANNEL_COUNT) {
        snprintf(err, err_cap, "zone or relay out of range");
        if (counts) *counts = c;
        return false;
    }
    uint8_t active_id = 0;
    bool have_active = profile_executor_get_active_id(&active_id);
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        if (!profiles_slot_used(id)) {
            continue;
        }
        c.profiles_scanned++;
        const profile_t *p = &s_profiles.profiles[id];
        if (!allow_dest && retarget_count_rules(p, dest) != 0) {
            snprintf(err, err_cap, "profile slot %u already has a rule targeting aux relay %u", id, relay);
            if (counts) *counts = c;
            return false;
        }
        uint8_t cnt = retarget_rule_limit(p);
        uint16_t hits = 0;
        for (uint8_t i = 0; i < cnt; i++) {
            const profile_on_off_rule_t *r = &p->on_off_rules[i];
            if (r->zone_index != zone) {
                continue;
            }
            hits++;
            if (r->temp_source > 1) {
                snprintf(err, err_cap,
                         "profile slot %u rule %u uses temp_source %u, which an aux output cannot represent", id, i,
                         r->temp_source);
                if (counts) *counts = c;
                return false;
            }
            if (r->temp_cmp != ON_OFF_TEMP_CMP_NONE && (r->temp_source != 1 || !zone_has_tc)) {
                snprintf(err, err_cap,
                         "profile slot %u rule %u has a temperature compare but the aux output would have no "
                         "thermocouple zone",
                         id, i);
                if (counts) *counts = c;
                return false;
            }
        }
        if (hits != 0) {
            if (have_active && active_id == id) {
                snprintf(err, err_cap, "profile slot %u is running or paused and has a rule for zone %u", id, zone);
                if (counts) *counts = c;
                return false;
            }
            c.profiles_affected++;
            c.rules_retargeted = (uint16_t)(c.rules_retargeted + hits);
        }
    }
    if (counts) *counts = c;
    return true;
}

bool profiles_retarget_zone_to_aux_plan(uint8_t zone, uint8_t relay, bool zone_has_tc,
                                        profiles_retarget_counts_t *counts, char *err, size_t err_cap)
{
    return retarget_plan(zone, relay, zone_has_tc, false, counts, err, err_cap);
}

/* Read slot `id` back out of NVS and require the WHOLE stored profile to equal RAM: right length and
 * version, a CRC that matches the stored bytes, and the profile payload byte-for-byte equal. A
 * three-field compare of the rules could not see a save that lost or altered anything else. The
 * wrapper is NOT compared against a freshly encoded one: its padding bytes are indeterminate, so two
 * encodes of the same profile can differ while both are valid. */
static bool retarget_verify_slot(uint8_t id)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, PROFILES_NVS_PARTITION) != HAL_OK) {
        return false;
    }
    char key[8];
    profile_nvs_key(id, key, sizeof(key));
    profile_persisted_t *loaded = persist_scratch_alloc(sizeof(*loaded));
    bool ok = false;
    if (loaded) {
        size_t len = sizeof(*loaded);
        ok = hal_kv_get_blob(&h, key, loaded, &len) == HAL_OK && len == sizeof(*loaded) &&
             loaded->version == PROFILE_VERSION && loaded->crc32 == compute_profile_crc(loaded) &&
             memcmp(&loaded->profile, &s_profiles.profiles[id], sizeof(loaded->profile)) == 0;
    }
    free(loaded);
    hal_kv_close(&h);
    return ok;
}

/* Undo `from`->`to` on the first `n` journal entries, newest first. Returns
 * false if any slot could not be re-persisted. */
static bool retarget_revert(const uint8_t *journal, uint8_t n, uint8_t from, uint8_t to)
{
    bool clean = true;
    while (n > 0) {
        uint8_t id = journal[--n];
        (void)retarget_swap_rules(&s_profiles.profiles[id], from, to);
        if (nvs_save_slot(id) != ESP_OK || !retarget_verify_slot(id)) {
            clean = false;
        }
    }
    return clean;
}

static bool retarget_commit(uint8_t zone, uint8_t relay, bool zone_has_tc, bool resume,
                            profiles_retarget_counts_t *counts, char *err, size_t err_cap)
{
    profiles_retarget_counts_t plan;
    if (!retarget_plan(zone, relay, zone_has_tc, resume, &plan, err, err_cap)) {
        if (counts) *counts = plan;
        return false;
    }
    uint8_t dest = profile_rule_target_from_aux_relay(relay);
    uint8_t journal[PROFILES_MAX_COUNT];
    uint8_t jn = 0;
    profiles_retarget_counts_t done = plan;
    done.profiles_affected = 0;
    done.rules_retargeted = 0;
    profile_t *trial = persist_scratch_alloc(sizeof(*trial));
    if (!trial) {
        snprintf(err, err_cap, "out of memory");
        if (counts) *counts = done;
        return false;
    }
    const char *fail = NULL;
    char why[96] = "";
    uint8_t fail_id = 0;
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT && !fail; id++) {
        if (!profiles_slot_used(id) || retarget_count_rules(&s_profiles.profiles[id], zone) == 0) {
            continue;
        }
        *trial = s_profiles.profiles[id];
        uint16_t n = retarget_swap_rules(trial, zone, dest);
        fail_id = id;
        if (!validate_on_off_rules(trial, why, sizeof(why))) {
            fail = "rewritten profile failed validation";
            break;
        }
        s_profiles.profiles[id] = *trial;
        journal[jn++] = id;
        if (nvs_save_slot(id) != ESP_OK) {
            fail = "persisting the rewritten profile failed";
            break;
        }
        done.profiles_affected++;
        done.rules_retargeted = (uint16_t)(done.rules_retargeted + n);
    }
    free(trial);
    for (uint8_t k = 0; !fail && k < jn; k++) {
        if (!retarget_verify_slot(journal[k])) {
            fail = "read-back of a rewritten profile did not match";
            fail_id = journal[k];
        }
    }
    if (fail) {
        bool clean = retarget_revert(journal, jn, dest, zone);
        snprintf(err, err_cap, "profile slot %u: %s%s%s -- %s", fail_id, fail, why[0] ? ": " : "", why,
                 clean ? "all profiles restored" : "REVERT INCOMPLETE, profiles may be mixed");
        if (counts) *counts = done;
        return false;
    }
    if (counts) *counts = done;
    return true;
}

bool profiles_retarget_zone_to_aux_commit(uint8_t zone, uint8_t relay, bool zone_has_tc,
                                          profiles_retarget_counts_t *counts, char *err, size_t err_cap)
{
    return retarget_commit(zone, relay, zone_has_tc, false, counts, err, err_cap);
}

bool profiles_retarget_zone_to_aux_resume(uint8_t zone, uint8_t relay, bool zone_has_tc,
                                          profiles_retarget_counts_t *counts, char *err, size_t err_cap)
{
    return retarget_commit(zone, relay, zone_has_tc, true, counts, err, err_cap);
}

bool profiles_retarget_zone_to_aux_revert(uint8_t zone, uint8_t relay)
{
    uint8_t dest = profile_rule_target_from_aux_relay(relay);
    if (dest == 0xFFu || zone >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    uint8_t journal[PROFILES_MAX_COUNT];
    uint8_t jn = 0;
    for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
        if (profiles_slot_used(id) && retarget_count_rules(&s_profiles.profiles[id], dest) != 0) {
            journal[jn++] = id;
        }
    }
    return retarget_revert(journal, jn, dest, zone);
}
