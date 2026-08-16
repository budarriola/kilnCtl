// nvs_report -- boot-time summary of every NVS partition this firmware owns,
// for TODO.md 8.2's "one boot-time report" and 8.3's config wizard to render.
//
// This is a read-only STATUS check, not an init: by the time
// nvs_report_capture() runs, wifi_prov_start()/zones_http_start()/
// rules_http_start()/profiles_http_start()/relay_cycles_init()/run_state_init()
// have already each brought up their own partition (see those files'
// nvs_partition_init() helpers, all copied from the same wifi_prov.c
// pattern). This module just asks the partition table and NVS driver what
// happened and remembers it for /api/status to report.
#ifndef NVS_REPORT_H
#define NVS_REPORT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_REPORT_MAX_SECTIONS 4

typedef struct {
    const char *name;   /* partition label, e.g. "wifi_nvs" */
    bool present;        /* partition exists in the flashed partition table */
    bool mounted;         /* NVS successfully mounted it (false = present but
                            * unreadable/needs erase, or absent entirely) */
} nvs_report_section_t;

/* Walks every partition this firmware persists to (wifi_nvs, kiln_nvs,
 * profiles_nvs) and records present/mounted for each. Call once from
 * app_main(), after every module that owns one of these partitions has
 * already run its own start/init -- this does not itself initialize
 * anything, it only observes state those calls already established. Safe to
 * call more than once (idempotent, just re-observes). */
void nvs_report_capture(void);

/* Returns the captured sections (see nvs_report_capture()); *out_count is
 * always <= NVS_REPORT_MAX_SECTIONS. Returns NULL / *out_count == 0 if
 * nvs_report_capture() has not run yet. Points at static storage. */
const nvs_report_section_t *nvs_report_get(size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif // NVS_REPORT_H
