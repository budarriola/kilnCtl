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
#include "kiln_io.h"
#include "web_encoding.h"
#include "wifi_provision_http.h"

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

#define RULES_MAX_RULES_PER_RELAY 3
#define RULES_MAX_CONDITIONS_PER_RULE 3

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

typedef enum {
    COND_NONE = 0,
    COND_TEMP,
    COND_TIME,
    COND_RELAY,
} cond_type_t;

typedef enum {
    CMP_GE = 0,
    CMP_LE,
} cmp_t;

/* One condition. Which fields are meaningful depends on type -- a tagged
 * union would save a few bytes but this whole config is tiny (well under
 * 1KB), so a flat struct keeps the parser/serializer simpler. */
typedef struct {
    cond_type_t type;
    uint8_t zone_index;      /* TEMP */
    cmp_t cmp;                /* TEMP, TIME */
    float threshold_c;        /* TEMP */
    uint32_t seconds;         /* TIME */
    uint8_t other_relay;      /* RELAY, 1-based (KILN_IO_RELAY_COUNT numbering) */
    bool other_relay_state;   /* RELAY */
} rule_condition_t;

typedef struct {
    uint8_t condition_count; /* 0 = this rule slot is unused */
    rule_condition_t conditions[RULES_MAX_CONDITIONS_PER_RULE];
} rule_t;

typedef struct {
    bool rule_driven; /* mirrors the RULE owner tag -- recorded intent only, see header */
    rule_t rules[RULES_MAX_RULES_PER_RELAY];
} relay_rules_cfg_t;

typedef struct {
    uint8_t version; /* RULES_CFG_VERSION at save time -- see nvs_load() */
    relay_rules_cfg_t relays[KILN_IO_RELAY_COUNT];
} rules_cfg_t;

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
    if (len != sizeof(*out_cfg)) {
        ESP_LOGW(TAG, "rules_cfg blob from '%s' is the wrong size -- treating as unreadable",
                 partition);
        memset(out_cfg, 0, sizeof(*out_cfg));
        return ESP_OK;
    }

    if (out_cfg->version == RULES_CFG_VERSION) {
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
        if (zone_idx < 0 || zone_idx >= MAX31856_CHANNEL_COUNT) {
            return "zone index out of range";
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

    char err_msg[96];
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

    ESP_LOGI(TAG, "rules API up (config storage only -- no rule evaluator exists yet)");
    return ESP_OK;
}
