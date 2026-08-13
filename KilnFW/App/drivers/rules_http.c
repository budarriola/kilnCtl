#include "rules_http.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "MAX31856.h"
#include "kiln_io.h"
#include "wifi_provision_http.h"

static const char *TAG = "rules_http";

#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_RULES "rules_cfg"

#define RULES_MAX_RULES_PER_RELAY 3
#define RULES_MAX_CONDITIONS_PER_RULE 3

/* Embedded via EMBED_TXTFILES in CMakeLists.txt. */
extern const uint8_t rules_page_html_start[] asm("_binary_rules_page_html_start");
extern const uint8_t rules_page_html_end[] asm("_binary_rules_page_html_end");

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
    relay_rules_cfg_t relays[KILN_IO_RELAY_COUNT];
} rules_cfg_t;

static struct {
    rules_cfg_t cfg;
} s_rules;

/* ---- NVS ---------------------------------------------------------------- */

static esp_err_t nvs_load(void)
{
    memset(&s_rules.cfg, 0, sizeof(s_rules.cfg));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* first boot, nothing configured -- not an error */
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(s_rules.cfg);
    err = nvs_get_blob(h, NVS_KEY_RULES, &s_rules.cfg, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        memset(&s_rules.cfg, 0, sizeof(s_rules.cfg));
        return ESP_OK;
    }
    if (err != ESP_OK || len != sizeof(s_rules.cfg)) {
        ESP_LOGW(TAG, "rules_cfg blob load failed or wrong size (%s) -- starting unconfigured",
                 esp_err_to_name(err));
        memset(&s_rules.cfg, 0, sizeof(s_rules.cfg));
    }
    return ESP_OK;
}

static esp_err_t nvs_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
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

static esp_err_t page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)rules_page_html_start,
                           (size_t)(rules_page_html_end - rules_page_html_start));
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
    esp_err_t err = nvs_load();
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
