// rules_types.h -- the on-flash/in-RAM rule schema, shared between
// rules_http.c (storage/HTTP) and rules_eval.h/.c (the pure evaluator logic).
//
// Pulled out of rules_http.c (which used to define these as private,
// file-static types) so rules_eval.c can operate on the exact same schema
// the Relay & Rules page already produces, without inventing a parallel one
// and without rules_eval.c/its host tests dragging in rules_http.c's
// ESP-IDF/NVS dependencies (nvs.h, esp_log.h, esp_http_server.h transitively
// via wifi_provision_http.h).
//
// RULES_EVAL_ZONE_COUNT/RULES_EVAL_RELAY_COUNT deliberately MIRROR
// MAX31856_CHANNEL_COUNT (MAX31856.h, itself THERMO_CHANNEL_COUNT from
// uart_task_ids.h) and KILN_IO_RELAY_COUNT (kiln_io.h) rather than including
// either header -- both drag in FreeRTOS/SPI/ESP-IDF headers that are not
// host-buildable, and this file (plus rules_eval.h/.c) needs to stay host-
// testable, same reasoning relay_authority.c's own RELAY_AUTHORITY_MAX_ZONES/
// RELAY_AUTHORITY_MAX_RELAYS constants give for the identical trick. Every
// ESP-IDF translation unit that #includes this header alongside the real
// headers (rules_http.c, rules_task.c) _Static_assert()s the two stay equal
// -- see rules_http.c -- so a future channel/relay count change cannot
// silently desync the mirrored value here from the real one.
#ifndef RULES_TYPES_H
#define RULES_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RULES_MAX_RULES_PER_RELAY 3
#define RULES_MAX_CONDITIONS_PER_RULE 3

/* Mirrors KILN_IO_RELAY_COUNT (kiln_io.h) -- see this header's top comment. */
#define RULES_EVAL_RELAY_COUNT 4u
/* Mirrors MAX31856_CHANNEL_COUNT (MAX31856.h / THERMO_CHANNEL_COUNT,
 * uart_task_ids.h) -- see this header's top comment. */
#define RULES_EVAL_ZONE_COUNT 3u

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
 * 1KB), so a flat struct keeps the parser/serializer/evaluator simpler. */
typedef struct {
    cond_type_t type;
    uint8_t zone_index;      /* TEMP, 0-based, < RULES_EVAL_ZONE_COUNT */
    cmp_t cmp;                /* TEMP, TIME */
    float threshold_c;        /* TEMP */
    uint32_t seconds;         /* TIME */
    uint8_t other_relay;      /* RELAY, 1-based (RULES_EVAL_RELAY_COUNT numbering) */
    bool other_relay_state;   /* RELAY */
} rule_condition_t;

typedef struct {
    uint8_t condition_count; /* 0 = this rule slot is unused (never fires -- see
                               * rules_eval_relay_wants_on()'s doc comment) */
    rule_condition_t conditions[RULES_MAX_CONDITIONS_PER_RULE];
} rule_t;

typedef struct {
    bool rule_driven; /* mirrors the RULE owner tag (relay_authority.h's
                       * RELAY_OWNER_RULE) -- rules_task.c claims/releases that
                       * tag to match this flag, and a relay with this false
                       * is never touched by the evaluator regardless of what
                       * stale rule slots below still hold. */
    rule_t rules[RULES_MAX_RULES_PER_RELAY];
} relay_rules_cfg_t;

typedef struct {
    uint8_t version; /* RULES_CFG_VERSION at save time -- see rules_http.c's nvs_load() */
    relay_rules_cfg_t relays[RULES_EVAL_RELAY_COUNT];
} rules_cfg_t;

#ifdef __cplusplus
}
#endif

#endif // RULES_TYPES_H
