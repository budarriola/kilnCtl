#include "profiles_http.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "MAX31856.h"
#include "http_form.h"
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

/* Bump whenever the on-flash per-slot layout (profile_persisted_t) changes;
 * see nvs_load_all_from(). */
#define PROFILE_VERSION 1

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
} profile_persisted_t;

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

/* Hook point for migrating an older on-flash profile_persisted_t layout
 * forward. v1 is the first version that has ever shipped, so there is
 * nothing to convert yet -- this is a no-op passthrough that exists purely
 * so the next version bump has somewhere to add real field conversion,
 * rather than inventing the load-time branching from scratch. */
static void migrate_profile_v1_to_current(profile_persisted_t *slot)
{
    static bool logged = false;
    if (!logged) {
        ESP_LOGI(TAG, "migrating a profile slot from struct version 1 -- no-op passthrough (v1 is current)");
        logged = true;
    }
    slot->version = PROFILE_VERSION;
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
        if (slot_err != ESP_OK || len != sizeof(loaded)) {
            /* The bitmap says used but the blob is missing/wrong-size --
             * trust the blob, not the bitmap: mark THIS slot unused rather
             * than hand a client a garbage-decoded profile. Other slots are
             * unaffected. */
            ESP_LOGW(TAG, "prof%u load from '%s' failed or wrong size (%s) -- marking unused", id, partition,
                     esp_err_to_name(slot_err));
            out->used_bitmap &= ~(1u << id);
            continue;
        }

        if (loaded.version == PROFILE_VERSION) {
            out->profiles[id] = loaded.profile; /* current version -- happy path */
        } else if (loaded.version < PROFILE_VERSION) {
            /* Known older layout -- run it through the migration chain. */
            ESP_LOGI(TAG, "prof%u is struct version %u, migrating to %u", id, (unsigned)loaded.version,
                     (unsigned)PROFILE_VERSION);
            migrate_profile_v1_to_current(&loaded);
            out->profiles[id] = loaded.profile;
        } else {
            /* loaded.version > PROFILE_VERSION: this slot was written by
             * NEWER firmware than this build -- the firmware-rollback case
             * from TODO.md 8.1. Its layout may use fields this older build
             * doesn't understand, so treating it as corrupt and wiping it
             * would destroy data a roll-forward (or the newer firmware
             * itself) still needs. Refuse to load instead: leave the slot's
             * flash bytes completely untouched and just don't surface it
             * for this boot -- this is the ONE outcome above that is not a
             * "bad slot," so it deliberately does not erase or overwrite
             * anything. */
            ESP_LOGW(TAG, "prof%u is struct version %u, newer than this firmware's %u -- refusing to load, "
                          "flash left untouched",
                     id, (unsigned)loaded.version, (unsigned)PROFILE_VERSION);
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
    };
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

static esp_err_t profiles_list_get_handler(httpd_req_t *req)
{
    char json[PROFILES_MAX_COUNT * 96 + 16];
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

send:
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
    /* Sized for the per-segment "feasibility":"unreachable" field added
     * alongside the three numeric ones -- ~112 bytes per segment worst case. */
    char json[224 + PROFILE_MAX_SEGMENTS * 112];
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
        APPEND("%s{\"target_c\":%.2f,\"ramp_c_per_hr\":%.2f,\"dwell_min\":%lu,\"feasibility\":\"%s\"}",
               i == 0 ? "" : ",", (double)s->target_c, (double)s->ramp_c_per_hr,
               (unsigned long)s->dwell_min, profile_feasibility_verdict_str(per_seg[i]));
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
        char key[16];
        profile_segment_t *seg = &p->segments[i];

        snprintf(key, sizeof(key), "seg%u_target", i);
        char val[24];
        int len = http_form_find_field(body, key, val, sizeof(val));
        char *fend = NULL;
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
