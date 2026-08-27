#include "rules_http.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "MAX31856.h"
#include "ota_http.h" /* ota_http_check_interlocks() -- the shared "not while firing" gate */
#include "kiln_io.h"
#include "rules_task.h"
#include "rules_types.h"
#include "web_encoding.h"
#include "wifi_provision_http.h"
#include "zones_http.h"

/* rules_types.h mirrors these two constants without including MAX31856.h/
 * kiln_io.h (host-testability -- see that header's top comment). Caught here,
 * at compile time, in the one translation unit that has both the mirror and
 * the real headers in scope. */
_Static_assert(RULES_EVAL_RELAY_COUNT == KILN_IO_RELAY_COUNT,
               "rules_types.h's RULES_EVAL_RELAY_COUNT drifted from kiln_io.h's KILN_IO_RELAY_COUNT");
_Static_assert(RULES_EVAL_ZONE_COUNT == MAX31856_CHANNEL_COUNT,
               "rules_types.h's RULES_EVAL_ZONE_COUNT drifted from MAX31856.h's MAX31856_CHANNEL_COUNT");

static const char *TAG = "rules_http";

#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_RULES "rules_cfg"

/* kiln_nvs is the 2026-08-13 split target for zones/rules/relay_cycles/
 * run_state (see partitions.csv and TODO.md 8.1); each module manages its own
 * migration and partition init independently rather than assuming another
 * module already brought the partition up. NVS_DEFAULT_PART_NAME (from
 * nvs_flash.h, expands to "nvs") is the old, still-live home this module's
 * data used to persist to, kept readable for the one-time migration below and
 * for firmware rollback. */
#define KILN_NVS_PARTITION "kiln_nvs"

/* Bump whenever rules_cfg_t's on-flash layout changes; see nvs_load(). */
#define RULES_CFG_VERSION 1

/* RULES_MAX_RULES_PER_RELAY / RULES_MAX_CONDITIONS_PER_RULE now come from
 * rules_types.h. */

/* Embedded via EMBED_TXTFILES in CMakeLists.txt. TODO.md 10.6a: embedded
 * pre-gzipped (gzip'd at configure time before idf_component_register
 * runs), hence the "_gz" in both the filename and the generated symbol. */
extern const uint8_t rules_page_html_gz_start[] asm("_binary_rules_page_html_gz_start");
extern const uint8_t rules_page_html_gz_end[] asm("_binary_rules_page_html_gz_end");

/* Raw request body cap for POST /api/rules -- this is a hand-typed DSL in a
 * <textarea>, not a generated form, but the same defensive discipline
 * applies: bounded well above what 4 relays * 3 rules * 3 conditions could
 * ever need as text (a condition line is well under 40 bytes), checked
 * against Content-Length before a single byte is read. */
#define RULES_BODY_MAX 2048

/* cond_type_t/cmp_t/rule_condition_t/rule_t/relay_rules_cfg_t/rules_cfg_t now
 * live in rules_types.h, shared with rules_eval.c/.h and rules_task.c -- see
 * that header's top comment for why this moved out of here. */

static struct {
    rules_cfg_t cfg;
} s_rules;

/* ---- NVS ---------------------------------------------------------------- */

static esp_err_t nvs_save(void);
static void migrate_rules_cfg_v1_to_current(rules_cfg_t *cfg);

/* Brings up one NVS partition, erasing ONLY that partition if its contents
 * are unusable. Copied/adapted from wifi_prov.c's nvs_partition_init() (see
 * that file for the full rationale) -- NO_FREE_PAGES / NEW_VERSION_FOUND have
 * no other cure, so erasing is the only way forward, but the erase must stay
 * scoped to the partition that is actually broken rather than blast-radius
 * the rest of kiln_nvs (or, worse, the default partition) with it. */
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

/* Reads NVS_NAMESPACE/NVS_KEY_RULES out of `partition` into *out_cfg, applying
 * the same three-outcome version handling nvs_load() uses. *out_found reports
 * whether the namespace/key existed at all (vs. existing but unreadable),
 * which is what the one-time migration below keys off. */
static esp_err_t nvs_load_from(const char *partition, rules_cfg_t *out_cfg, bool *out_found)
{
    if (out_found) {
        *out_found = false;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(partition, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* namespace never created -- nothing configured, not an error */
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(*out_cfg);
    err = nvs_get_blob(h, NVS_KEY_RULES, out_cfg, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(out_cfg, 0, sizeof(*out_cfg));
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rules_cfg blob read from '%s' failed (%s) -- treating as unreadable",
                 partition, esp_err_to_name(err));
        memset(out_cfg, 0, sizeof(*out_cfg));
        return ESP_OK;
    }
    if (out_found) {
        *out_found = true;
    }
    /* BUG FIXED (matching zones_http.c's nvs_load_from()): this used to
     * reject on `len != sizeof(*out_cfg)` BEFORE ever looking at `version`,
     * which would misclassify an older (smaller, pre-growth) blob as
     * corruption instead of running it through the migration chain below.
     * Only a blob too short to even contain the `version` byte is
     * genuinely unreadable; anything else is the version check's job. */
    if (len < sizeof(out_cfg->version)) {
        ESP_LOGW(TAG, "rules_cfg blob from '%s' is too short to contain a version -- treating as unreadable",
                 partition);
        memset(out_cfg, 0, sizeof(*out_cfg));
        return ESP_OK;
    }

    if (out_cfg->version == RULES_CFG_VERSION) {
        if (len != sizeof(*out_cfg)) {
            /* Current version but wrong size can only mean genuine
             * corruption -- a real current-version blob is always written
             * at exactly sizeof(*out_cfg). */
            ESP_LOGW(TAG, "rules_cfg blob from '%s' claims current version but is the wrong size -- "
                          "treating as unreadable", partition);
            memset(out_cfg, 0, sizeof(*out_cfg));
            return ESP_OK;
        }
        return ESP_OK; /* current version -- happy path */
    }
    if (out_cfg->version < RULES_CFG_VERSION) {
        /* Known older layout -- run it through the migration chain. v1 is the
         * first version that has ever existed, so this is currently just the
         * hook point: nothing to actually convert yet. */
        ESP_LOGI(TAG, "rules_cfg from '%s' is version %u, migrating to %u", partition,
                 (unsigned)out_cfg->version, (unsigned)RULES_CFG_VERSION);
        migrate_rules_cfg_v1_to_current(out_cfg);
        return ESP_OK;
    }
    /* out_cfg->version > RULES_CFG_VERSION: the data was written by NEWER
     * firmware than this build. This is the firmware-rollback case from
     * TODO.md 8.1 -- an operator rolled back after a bad update, and the
     * data on flash may use fields/layout this older build doesn't know
     * about. Wiping it here would destroy config the newer firmware (or a
     * roll-forward back to it) still needs, so refuse to load instead:
     * leave flash untouched and fall back to defaults for this boot only. */
    ESP_LOGW(TAG, "rules_cfg from '%s' is version %u, newer than this firmware's %u -- "
                  "refusing to load, flash data left untouched",
             partition, (unsigned)out_cfg->version, (unsigned)RULES_CFG_VERSION);
    memset(out_cfg, 0, sizeof(*out_cfg));
    if (out_found) {
        *out_found = false; /* don't let a newer-version blob look migratable */
    }
    return ESP_OK;
}

/* Hook point for migrating an older on-flash rules_cfg_t layout forward.
 * v1 is the first version that has ever shipped, so there is nothing to
 * convert yet -- this is a no-op passthrough that exists purely so the next
 * version bump has somewhere to add real field conversion. */
static void migrate_rules_cfg_v1_to_current(rules_cfg_t *cfg)
{
    cfg->version = RULES_CFG_VERSION;
}

/* One-time move of the persisted rules config out of the default partition's
 * NVS_NAMESPACE/NVS_KEY_RULES and into KILN_NVS_PARTITION's, for boards
 * provisioned by firmware predating the 2026-08-13 split. Simplified from
 * wifi_prov.c's migrate_from_default_partition() to a one-directional copy:
 * kiln_nvs is only ever consulted first, so if it already has something
 * there is nothing to migrate and no "which wins" question to answer -- only
 * the old default-partition location could hold pre-migration data. The old
 * copy is deliberately left in place (not deleted), same rationale as
 * wifi_prov.c: it needs to still be there if someone rolls back to
 * pre-split firmware. */
static void migrate_from_default_partition(void)
{
    rules_cfg_t from_default;
    bool found_in_default = false;
    esp_err_t err = nvs_load_from(NVS_DEFAULT_PART_NAME, &from_default, &found_in_default);
    if (err != ESP_OK || !found_in_default) {
        return; /* nothing to migrate */
    }

    ESP_LOGI(TAG, "migrating rules_cfg from the default NVS partition to '%s'", KILN_NVS_PARTITION);

    s_rules.cfg = from_default;
    esp_err_t save_err = nvs_save();
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "migration write to '%s' failed: %s -- running from the old copy this boot, will retry",
                 KILN_NVS_PARTITION, esp_err_to_name(save_err));
    }
}

static esp_err_t nvs_load(bool *out_found)
{
    return nvs_load_from(KILN_NVS_PARTITION, &s_rules.cfg, out_found);
}

static esp_err_t nvs_save(void)
{
    s_rules.cfg.version = RULES_CFG_VERSION;

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(KILN_NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_RULES, &s_rules.cfg, sizeof(s_rules.cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ---- HTML page ------------------------------------------------------------ */

/* TODO.md 10.6a: content negotiation lives in web_encoding.h's shared
 * web_client_accepts_gzip() -- absent Accept-Encoding is legal and served
 * gzip (RFC 9110 s12.5.3); a header that explicitly excludes gzip gets an
 * uncompressed 406 rather than a body it cannot decode. */
static esp_err_t page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "rules_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)rules_page_html_gz_start,
                           (size_t)(rules_page_html_gz_end - rules_page_html_gz_start));
}

/* ---- GET /api/rules: regenerate the DSL text from the in-RAM config ------
 *
 * Format, one line each (see rules_http.h and the page's own cheat-sheet):
 *   RELAY <n> DRIVEN <0|1>
 *   R <rule_index 0-2> TEMP <zone_index> <GE|LE> <threshold_c>
 *   R <rule_index 0-2> TIME <GE|LE> <seconds>
 *   R <rule_index 0-2> RELAY <other_relay 1-4> <0|1>
 * Lines apply to whichever RELAY...DRIVEN line most recently preceded them.
 *
 * Always emits a RELAY...DRIVEN line for every one of the KILN_IO_RELAY_COUNT
 * relays (even ones with no rules yet) so the full editable state is visible
 * up front, rather than making an unconfigured relay implicit. */
static esp_err_t rules_get_handler(httpd_req_t *req)
{
    char text[2048];
    size_t o = 0;
    int n;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(text + o, sizeof(text) - o, __VA_ARGS__);                                   \
        if (n < 0 || (size_t)n >= sizeof(text) - o) {                                             \
            goto send;                                                                            \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        const relay_rules_cfg_t *rc = &s_rules.cfg.relays[r];
        APPEND("RELAY %u DRIVEN %u\n", (unsigned)(r + 1), rc->rule_driven ? 1u : 0u);
        for (uint8_t k = 0; k < RULES_MAX_RULES_PER_RELAY; k++) {
            const rule_t *rule = &rc->rules[k];
            for (uint8_t c = 0; c < rule->condition_count; c++) {
                const rule_condition_t *cond = &rule->conditions[c];
                switch (cond->type) {
                case COND_TEMP:
                    APPEND("R %u TEMP %u %s %.2f\n", k, cond->zone_index,
                           cond->cmp == CMP_GE ? "GE" : "LE", (double)cond->threshold_c);
                    break;
                case COND_TIME:
                    APPEND("R %u TIME %s %lu\n", k, cond->cmp == CMP_GE ? "GE" : "LE",
                           (unsigned long)cond->seconds);
                    break;
                case COND_RELAY:
                    APPEND("R %u RELAY %u %u\n", k, cond->other_relay,
                           cond->other_relay_state ? 1u : 0u);
                    break;
                default:
                    break;
                }
            }
        }
    }

#undef APPEND

send:
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, text, o);
}

/* ---- POST /api/rules: parse and validate the whole text before committing
 *
 * Parses into a scratch rules_cfg_t; any bad line rejects the ENTIRE
 * submission with a 400 naming the 1-based line number and reason, before
 * s_rules.cfg or NVS are ever touched -- a partially-applied relay config is
 * a worse failure mode than a rejected one (per TODO.md's own framing for
 * this page). */

/* Owner's design rule: "the relays that are controlled by pid/thermocouples
 * should not be controlable through rules ... they may be used as rule data
 * though". A relay belonging to ANY zone (zones_config_get_relay_mask(),
 * checked across every configured zone) is such a relay -- refuse a
 * RELAY <n> DRIVEN 1 submission for it outright, naming both the relay and
 * the owning zone, rather than silently accepting and dropping the bit (the
 * enforcement rules_task.c applies every tick would make a silently-accepted
 * bit a no-op anyway, but a save that claims success while quietly doing
 * nothing is a worse UX than a rejected save with a clear reason).
 * DRIVEN 0 is always allowed regardless of zone ownership -- turning rule
 * control OFF for a relay is never something this needs to block.
 *
 * Returns NULL if relay_n (1-based) is not zone-owned. Otherwise returns a
 * pointer to a static, file-scope buffer holding the formatted reason --
 * not thread-safe, but this codebase's httpd handlers already assume
 * effectively-serialized request handling (see rules_get_handler's/
 * rules_post_handler's own single in-RAM s_rules.cfg with no lock), so one
 * more non-reentrant static buffer used strictly within a single request's
 * handling is consistent with the rest of this file. */
static const char *check_relay_not_zone_owned(int relay_n)
{
    static char s_reason[96];
    uint8_t bit = (uint8_t)(1u << (relay_n - 1));
    uint8_t zone_count = zones_config_get_thermo_count();
    for (uint8_t zi = 0; zi < zone_count; zi++) {
        uint8_t zone_mask = 0;
        if (!zones_config_get_relay_mask(zi, &zone_mask)) {
            continue;
        }
        if ((zone_mask & bit) != 0) {
            /* Two things this message used to get wrong, both found on the
             * bench trying to rule-drive relay 2:
             *
             * It numbered the zone from 1 while every other surface on this
             * board -- the JSON, the web pages, and the LCD -- numbers zones
             * from 0, so it reported "zone 2" for zone index 1 and sent the
             * reader looking at the wrong zone's settings.
             *
             * And it asserted "is controlled by PID" unconditionally, without
             * ever reading the zone's control mode. Relay 2 belongs to zone 1,
             * which was set to OFF; the refusal named a mode that zone did not
             * have. The refusal itself is still correct -- a relay assigned to
             * a zone stays that zone's relay whatever its mode is today, since
             * the operator can switch the mode back on at any moment and two
             * owners would then fight over the same contact -- but it has to
             * say what is actually true. */
            zone_control_mode_t zmode = ZONE_CONTROL_MODE_OFF;
            zones_config_get_control_mode(zi, &zmode);
            const char *mode_words = (zmode == ZONE_CONTROL_MODE_PID)      ? "PID"
                                     : (zmode == ZONE_CONTROL_MODE_BANGBANG) ? "bang-bang"
                                                                             : "OFF";
            snprintf(s_reason, sizeof(s_reason),
                     "relay %d is assigned to zone %u (control mode %s) -- it cannot be rule_driven",
                     relay_n, (unsigned)zi, mode_words);
            return s_reason;
        }
    }
    return NULL;
}

static bool parse_cmp(const char *s, cmp_t *out)
{
    if (strcmp(s, "GE") == 0) {
        *out = CMP_GE;
        return true;
    }
    if (strcmp(s, "LE") == 0) {
        *out = CMP_LE;
        return true;
    }
    return false;
}

/* Returns NULL on success, or a static reason string on failure. current_relay
 * is an in/out index (0-based) into cfg->relays, -1 meaning "no RELAY line
 * seen yet"; RELAY lines update it, R lines require it already set. */
static const char *parse_line(char *line, rules_cfg_t *cfg, int *current_relay)
{
    /* Trim trailing CR/whitespace (textarea submissions commonly carry
     * \r\n) and leading whitespace; an all-blank line is valid and skipped
     * by the caller before this is reached. */
    size_t len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1])) {
        line[--len] = '\0';
    }
    char *p = line;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p == '\0') {
        return NULL; /* blank line -- nothing to do */
    }

    int relay_n, driven;
    if (sscanf(p, "RELAY %d DRIVEN %d", &relay_n, &driven) == 2) {
        if (relay_n < 1 || relay_n > KILN_IO_RELAY_COUNT) {
            return "relay number out of range (1-4)";
        }
        if (driven != 0 && driven != 1) {
            return "DRIVEN must be 0 or 1";
        }
        if (driven == 1) {
            const char *zone_reason = check_relay_not_zone_owned(relay_n);
            if (zone_reason != NULL) {
                return zone_reason;
            }
        }
        int idx = relay_n - 1;
        if (cfg->relays[idx].rule_driven || cfg->relays[idx].rules[0].condition_count ||
            cfg->relays[idx].rules[1].condition_count || cfg->relays[idx].rules[2].condition_count) {
            /* Already touched by an earlier RELAY line in this same
             * submission -- reject rather than silently discard whatever
             * that earlier block parsed, since which one "wins" would be
             * surprising either way. */
            return "relay declared twice in this submission";
        }
        cfg->relays[idx].rule_driven = (driven == 1);
        *current_relay = idx;
        return NULL;
    }

    int rule_idx;
    int zone_idx;
    char cmp_str[8];
    float threshold;
    if (sscanf(p, "R %d TEMP %d %7s %f", &rule_idx, &zone_idx, cmp_str, &threshold) == 4) {
        if (*current_relay < 0) {
            return "condition line with no preceding RELAY line";
        }
        if (rule_idx < 0 || rule_idx >= RULES_MAX_RULES_PER_RELAY) {
            return "rule index out of range (0-2)";
        }
        /* Bounded by the zones this board actually HAS, not by the number of
         * MAX31856 channels it could carry. Every zones_config_* getter
         * refuses an index >= thermo_count, so a condition on a zone past
         * that count could be saved, reported back by GET /api/rules, and
         * then silently never evaluate true -- the same "accepted, then
         * quietly never fires" failure this file's temperature conditions
         * were fixed for once already. Refuse it at save time instead, and
         * name the count so the message says what to do about it. */
        const uint8_t zone_count = zones_config_get_thermo_count();
        if (zone_idx < 0 || zone_idx >= (int)zone_count) {
            return zone_count == 0
                       ? "no zones are configured yet -- set the thermocouple count first"
                       : "zone index out of range for this kiln's configured zone count";
        }
        cmp_t cmp;
        if (!parse_cmp(cmp_str, &cmp)) {
            return "TEMP comparator must be GE or LE";
        }
        if (isnan(threshold) || threshold < -50.0f || threshold > 1400.0f) {
            return "TEMP threshold out of range";
        }
        rule_t *rule = &cfg->relays[*current_relay].rules[rule_idx];
        if (rule->condition_count >= RULES_MAX_CONDITIONS_PER_RULE) {
            return "too many conditions in this rule (max 3)";
        }
        rule_condition_t *cond = &rule->conditions[rule->condition_count++];
        cond->type = COND_TEMP;
        cond->zone_index = (uint8_t)zone_idx;
        cond->cmp = cmp;
        cond->threshold_c = threshold;
        return NULL;
    }

    unsigned long seconds;
    if (sscanf(p, "R %d TIME %7s %lu", &rule_idx, cmp_str, &seconds) == 3) {
        if (*current_relay < 0) {
            return "condition line with no preceding RELAY line";
        }
        if (rule_idx < 0 || rule_idx >= RULES_MAX_RULES_PER_RELAY) {
            return "rule index out of range (0-2)";
        }
        cmp_t cmp;
        if (!parse_cmp(cmp_str, &cmp)) {
            return "TIME comparator must be GE or LE";
        }
        /* No hard physical bound on elapsed-since-start seconds, but an
         * unbounded value is more likely a typo than a real firing longer
         * than the RAM history buffer's own 24h design point (TODO.md
         * section 0) -- reject past that as a sanity check, not a claimed
         * safety limit. */
        if (seconds > 24u * 3600u) {
            return "TIME seconds exceeds 24h sanity bound";
        }
        rule_t *rule = &cfg->relays[*current_relay].rules[rule_idx];
        if (rule->condition_count >= RULES_MAX_CONDITIONS_PER_RULE) {
            return "too many conditions in this rule (max 3)";
        }
        rule_condition_t *cond = &rule->conditions[rule->condition_count++];
        cond->type = COND_TIME;
        cond->cmp = cmp;
        cond->seconds = (uint32_t)seconds;
        return NULL;
    }

    int other_relay, other_state;
    if (sscanf(p, "R %d RELAY %d %d", &rule_idx, &other_relay, &other_state) == 3) {
        if (*current_relay < 0) {
            return "condition line with no preceding RELAY line";
        }
        if (rule_idx < 0 || rule_idx >= RULES_MAX_RULES_PER_RELAY) {
            return "rule index out of range (0-2)";
        }
        if (other_relay < 1 || other_relay > KILN_IO_RELAY_COUNT) {
            return "referenced relay out of range (1-4)";
        }
        if (other_relay - 1 == *current_relay) {
            return "a relay's rule cannot reference itself";
        }
        if (other_state != 0 && other_state != 1) {
            return "referenced relay state must be 0 or 1";
        }
        rule_t *rule = &cfg->relays[*current_relay].rules[rule_idx];
        if (rule->condition_count >= RULES_MAX_CONDITIONS_PER_RULE) {
            return "too many conditions in this rule (max 3)";
        }
        rule_condition_t *cond = &rule->conditions[rule->condition_count++];
        cond->type = COND_RELAY;
        cond->other_relay = (uint8_t)other_relay;
        cond->other_relay_state = (other_state == 1);
        return NULL;
    }

    return "unrecognized line format";
}

static esp_err_t rules_post_handler(httpd_req_t *req)
{
    /* Refuse to rewrite rule configuration while a firing is running, the same
     * gate kiln_cfg_http.c's apply and backup_http.c's restore already put in
     * front of the very same zones_cfg_t. Without it, changing relay_mask
     * mid-run moved the firing onto a different physical relay and left the
     * old one wherever it was last commanded, with nobody driving it off --
     * a contact that stays closed because the code that owned it stopped
     * looking at it.
     *
     * ota_http_check_interlocks() is that shared gate rather than a private
     * profile-is-RUNNING check, deliberately: it also covers a hot zone and
     * a commanded heater, and reads the kiln's ACTUAL current state rather
     * than profile_executor's own view (see its doc comment for why that
     * distinction matters). ota_http_req_ack_no_safety() carries the same
     * per-request operator acknowledgement every other caller passes, so a
     * board with no safety processor can still be configured -- saving this
     * page streams nothing over the link, exactly as backup_http's restore
     * argues for itself. */
    char interlock_reason[OTA_INTERLOCK_REASON_MAX];
    interlock_reason[0] = '\0';
    ota_interlock_result_t gate = ota_http_check_interlocks(ota_http_req_ack_no_safety(req),
                                                            interlock_reason, sizeof(interlock_reason));
    if (gate != OTA_INTERLOCK_OK) {
        ESP_LOGW(TAG, "POST /api/rules refused by interlock: %s", interlock_reason);
        return ota_http_send_interlock_refusal(req, gate, interlock_reason);
    }

    if (req->content_len <= 0 || req->content_len > RULES_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    char body[RULES_BODY_MAX + 1];
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "rules body read failed/short: %d", ret);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body read failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    rules_cfg_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    int current_relay = -1;

    /* 160, not 96: the zone-owned-relay refusal names both the relay and the
     * zone that owns it ("relay N is assigned to zone M and is controlled by
     * PID -- it cannot be rule_driven"), which is 95 bytes before the
     * "line %d: " prefix is added. At 96 the compiler rejected the build
     * outright (-Werror=format-truncation), which is the right outcome: a
     * silently truncated error would have cut off the very part that tells
     * the operator which relay to fix. */
    char err_msg[160];
    int line_no = 0;
    char *save = NULL;
    /* strtok_r over the caller's own buffer -- body is already a private
     * stack copy nothing else reads, so mutating it in place is fine and
     * avoids a second allocation. */
    char *line = strtok_r(body, "\n", &save);
    while (line != NULL) {
        line_no++;
        const char *reason = parse_line(line, &tmp, &current_relay);
        if (reason != NULL) {
            snprintf(err_msg, sizeof(err_msg), "line %d: %s", line_no, reason);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err_msg);
            return ESP_OK;
        }
        line = strtok_r(NULL, "\n", &save);
    }

    s_rules.cfg = tmp;
    esp_err_t err = nvs_save();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_save failed: %s -- config applied live but will not survive a reboot",
                 esp_err_to_name(err));
    }
    return httpd_resp_sendstr(req, "ok");
}

/* ---- GET /api/rules/status: live per-relay firing/ownership state --------
 * Backed by rules_task.c's rules_task_get_status() -- see rules_task.h for
 * what each field means. Plain hand-rolled JSON (no cJSON dependency in
 * this codebase), same APPEND-macro-over-a-stack-buffer style as
 * rules_get_handler() above. */
static esp_err_t rules_status_get_handler(httpd_req_t *req)
{
    rules_task_status_t st;
    rules_task_get_status(&st);

    char text[768];
    size_t o = 0;
    int n;

#define APPEND(...)                                                                              \
    do {                                                                                          \
        n = snprintf(text + o, sizeof(text) - o, __VA_ARGS__);                                   \
        if (n < 0 || (size_t)n >= sizeof(text) - o) {                                             \
            goto send;                                                                            \
        }                                                                                          \
        o += (size_t)n;                                                                            \
    } while (0)

    APPEND("{\"safety_link_ok\":%s,\"heat_interlock_ok\":%s,\"watchdog_forced_off\":%s,\"relays\":[",
           st.safety_link_ok ? "true" : "false", st.heat_interlock_ok ? "true" : "false",
           st.watchdog_forced_off ? "true" : "false");
    for (uint8_t r = 0; r < RULES_EVAL_RELAY_COUNT; r++) {
        const rules_task_relay_status_t *rs = &st.relays[r];
        APPEND("%s{\"relay\":%u,\"rule_driven\":%s,\"rule_wants_on\":%s,\"owned_by_rules\":%s,"
               "\"commanded_on\":%s,\"heater_owned\":%s}",
               r == 0 ? "" : ",", (unsigned)(r + 1), rs->rule_driven ? "true" : "false",
               rs->rule_wants_on ? "true" : "false", rs->owned_by_rules ? "true" : "false",
               rs->commanded_on ? "true" : "false", rs->heater_owned ? "true" : "false");
    }
    APPEND("]}");

#undef APPEND

send:
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, text, o);
}

void rules_http_get_cfg(rules_cfg_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_rules.cfg;
}

esp_err_t rules_http_start(void)
{
    /* kiln_nvs is shared by zones/rules/relay_cycles/run_state, and each
     * module brings it up independently rather than assuming another module
     * already has -- nvs_flash_init_partition() on an already-initialized
     * partition is a harmless no-op (ESP_OK), so this is safe to repeat. */
    esp_err_t part_err = nvs_partition_init(KILN_NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init for '%s' failed: %s -- rules will not persist",
                 KILN_NVS_PARTITION, esp_err_to_name(part_err));
    }

    esp_err_t err = ESP_OK;
    if (part_err == ESP_OK) {
        bool found_in_kiln_nvs = false;
        err = nvs_load(&found_in_kiln_nvs);
        if (err == ESP_OK && !found_in_kiln_nvs) {
            /* Nothing usable in kiln_nvs yet -- see if the old default
             * partition has a pre-split copy worth carrying forward. */
            migrate_from_default_partition();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rules_cfg NVS load failed: %s -- starting unconfigured", esp_err_to_name(err));
        memset(&s_rules.cfg, 0, sizeof(s_rules.cfg));
    }

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/settings/relays", .method = HTTP_GET, .handler = page_get_handler,
    };
    static const httpd_uri_t get_uri = {
        .uri = "/api/rules", .method = HTTP_GET, .handler = rules_get_handler,
    };
    static const httpd_uri_t post_uri = {
        .uri = "/api/rules", .method = HTTP_POST, .handler = rules_post_handler,
    };
    static const httpd_uri_t status_uri = {
        .uri = "/api/rules/status", .method = HTTP_GET, .handler = rules_status_get_handler,
    };
    err = httpd_register_uri_handler(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/settings/relays) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &get_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/rules) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(POST /api/rules) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(GET /api/rules/status) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "rules API up -- rules_task_start() (main.c) drives the actual evaluator off this config");
    return ESP_OK;
}
