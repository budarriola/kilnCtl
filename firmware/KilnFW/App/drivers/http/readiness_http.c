#include "readiness_http.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "boot_guard.h"
#include "cfg_fs.h"
#include "crash_report.h"
#include "dashboard_http.h"
#include "estop_verification.h"
#include "nvs_report.h"
#include "profiles_builtin.h"
#include "profiles_http.h"
#include "safety_cfg_store.h"
#include "web_encoding.h"
#include "wifi_prov.h"
#include "wifi_provision_http.h"
#include "zones_config_accessors.h"

static const char *TAG = "readiness_http";

/* Embedded via EMBED_TXTFILES -- same convention as every other
 * *_page.html in this component. TODO.md 10.6a: embedded pre-gzipped
 * (gzip'd at configure time before idf_component_register runs), hence the
 * "_gz" in both the filename and the generated symbol. */
extern const uint8_t readiness_page_html_gz_start[] asm("_binary_readiness_page_html_gz_start");
extern const uint8_t readiness_page_html_gz_end[] asm("_binary_readiness_page_html_gz_end");

/* readiness_status_t (TODO.md 8.3's four required distinctions: "not_done",
 * "cannot_yet" per bullet 2, "deliberately_off" per bullet 3, and "ok") now
 * lives in readiness_http.h so readiness_commissioning_status() below can be
 * host-tested without esp_http_server. readiness_commissioning_status()
 * likewise lives there as a static inline, so the commissioning item's
 * decision can be tested (test_readiness_commissioning.c) without compiling
 * this file, which pulls in httpd and the whole NVS stack. */

static const char *status_name(readiness_status_t s)
{
    switch (s) {
    case READY_OK: return "ok";
    case READY_NOT_DONE: return "not_done";
    case READY_CANNOT_YET: return "cannot_yet";
    case READY_DELIBERATELY_OFF: return "deliberately_off";
    default: return "unknown";
    }
}

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

/* Bytes held back from the item loop so that, however full the buffer gets,
 * there is always room for the "list was cut short" item below plus the "]}"
 * that closes the document. Sized against those two literals with margin, and
 * asserted against the real thing at the point of use. */
#define READINESS_TRUNC_RESERVE 288u

/* One uniform size for every item's local `detail` buffer. Previously each
 * item picked its own (64..180) by eye, and the recovery-mode item's 112 was
 * one such guess that a clean (non-ccache) build rejected outright: its
 * longest branch is 127 chars + NUL, so -Werror=format-truncation= failed the
 * whole build. Sized from a worst-case audit of EVERY branch of EVERY item,
 * expanding each conversion to its type's maximum width (%u -> 10 digits,
 * %04X -> 8, %s -> its longest literal alternative): the largest is 152 bytes
 * including the NUL (the recovery-mode branch), and three other items
 * (commissioning, safety_trip, safety_context) were also over their own
 * buffers at the range extremes even though today's real values fit. 192
 * clears all of them with ~40 bytes of headroom, so adding a clause to any
 * message does not silently re-open this. Deliberately NOT sized to the
 * current longest string plus one.
 *
 * append_item()'s detail_esc[] is intentionally left at 192 rather than
 * doubled to match: escaping only grows strings containing '"' or '\', which
 * none of these messages contain, json_escape() truncates safely (never
 * overruns), and this runs on the shared 8 KB httpd stack alongside
 * json[4096] -- see the httpd-stack-blob notes; growing frames here is not
 * free. */
#define READINESS_DETAIL_MAX 192

/* Appends one checklist item object to *o within cap, returning the new
 * offset (unchanged, i.e. truncated, if it would overflow -- same
 * "stop rather than corrupt" convention as every APPEND macro elsewhere in
 * this codebase, just as a plain function since this file builds the array
 * across many small per-item calls rather than one big format string).
 *
 * A dropped item must never be silently dropped: this checklist exists to
 * tell an operator what is NOT ready, so an item that falls off the end reads
 * exactly like an item that passed. *dropped is raised on any overflow and
 * the handler turns it into a visible READY_CANNOT_YET entry.
 *
 * Callers must only clear `first` when the offset actually moved. Clearing it
 * unconditionally meant a dropped FIRST item still armed the separator, and
 * the array opened `[,{...}` -- invalid JSON from a single overflow. */
static size_t append_item(char *json, size_t cap, size_t o, bool first, const char *key, const char *label,
                          readiness_status_t status, const char *detail, const char *fix_url,
                          bool *dropped)
{
    char detail_esc[192];
    json_escape(detail, detail_esc, sizeof(detail_esc));
    if (o >= cap) {
        *dropped = true;
        return o;
    }
    int n = snprintf(json + o, cap - o,
                     "%s{\"key\":\"%s\",\"label\":\"%s\",\"status\":\"%s\",\"detail\":\"%s\","
                     "\"fix_url\":\"%s\"}",
                     first ? "" : ",", key, label, status_name(status), detail_esc, fix_url);
    if (n < 0 || (size_t)n >= cap - o) {
        json[o] = '\0'; /* drop the partial object snprintf just wrote */
        *dropped = true;
        return o; /* leave o unchanged -- caller's overflow guard */
    }
    return o + (size_t)n;
}

static esp_err_t api_readiness_get_handler(httpd_req_t *req)
{
    /* thermo_count gates several later items (bullet 2: "order and gate the
     * items rather than presenting a flat list of red crosses") -- read it
     * once up front rather than re-deriving the same gate per item. */
    uint8_t thermo_count = zones_config_get_thermo_count();

    /* 3072 -> 4096, 2026-09-08: four new items (crash_report, recovery_mode,
     * cfg_fs, safety_context) pushed the previous size close enough to
     * READINESS_TRUNC_RESERVE that the truncation notice could plausibly
     * fire on a board with long detail strings on every item. */
    char json[4096];
    size_t o = 0;
    int n = snprintf(json, sizeof(json), "{\"items\":[");
    o = (n < 0 || (size_t)n >= sizeof(json)) ? sizeof(json) - 1 : (size_t)n;
    bool first = true;

    /* The item loop may only use the buffer up to item_cap; the rest is held
     * for the truncation notice and the closing "]}", so neither can itself be
     * the thing that gets truncated. */
    bool dropped = false;
    const size_t item_cap = sizeof(json) - READINESS_TRUNC_RESERVE;

    /* 1. Network configured. AP-only is an explicit, recordable choice
     * (wifi_prov.c's mode model -- "AP only, forever, by user choice"), not
     * an unfinished home-network setup, so it is reported as deliberately
     * off rather than nagged. */
    {
        wifi_prov_mode_t mode = wifi_prov_get_mode();
        readiness_status_t st;
        char detail[READINESS_DETAIL_MAX];
        if (mode == WIFI_PROV_MODE_AP) {
            st = READY_DELIBERATELY_OFF;
            snprintf(detail, sizeof(detail), "AP-only mode chosen -- no home network join attempted");
        } else {
            wifi_prov_saved_network_t saved[1];
            size_t saved_count = 0;
            wifi_prov_get_saved_networks(saved, 1, &saved_count);
            if (saved_count > 0) {
                st = READY_OK;
                snprintf(detail, sizeof(detail), "home network saved");
            } else {
                st = READY_NOT_DONE;
                snprintf(detail, sizeof(detail), "no network saved yet -- AP-only until one is added");
            }
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "network", "Network configured", st, detail, "/wifi", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 2. Thermocouple count / zone mapping. Gates almost everything below --
     * relay assignment, control mode, guard limits, calibration, and
     * autotune are all meaningless before the board knows how many zones
     * exist (bullet 2's example is literally this one). */
    {
        readiness_status_t st = thermo_count > 0 ? READY_OK : READY_NOT_DONE;
        char detail[READINESS_DETAIL_MAX];
        snprintf(detail, sizeof(detail), "%u of %u channels configured", thermo_count,
                 (unsigned)MAX31856_CHANNEL_COUNT);
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "thermo_count", "Thermocouple count / zone mapping", st,
                        detail, "/settings/zones", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 3. Relays assigned to zones. */
    {
        readiness_status_t st;
        char detail[READINESS_DETAIL_MAX];
        if (thermo_count == 0) {
            st = READY_CANNOT_YET;
            snprintf(detail, sizeof(detail), "set thermocouple count first");
        } else {
            uint8_t assigned = 0;
            for (uint8_t i = 0; i < thermo_count; i++) {
                uint8_t mask = 0;
                if (zones_config_get_relay_mask(i, &mask) && mask != 0) {
                    assigned++;
                }
            }
            st = (assigned == thermo_count) ? READY_OK : READY_NOT_DONE;
            snprintf(detail, sizeof(detail), "%u of %u zones have a relay assigned", assigned, thermo_count);
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "relays_assigned", "Relays assigned to zones", st, detail,
                        "/settings/zones", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 4. Control mode chosen per zone. NOTE: zone_control_mode_t's OFF
     * (0) is BOTH the zero-initialized default of an untouched zone AND a
     * legitimate operator choice ("a zone the board supports but the kiln
     * doesn't use" -- zones_http.h's own doc comment on the enum). There is
     * no separate "chosen" bit for this field the way there is for
     * max_temp_c/cross_zone_max_delta_c, so this item cannot honestly tell
     * "never touched" from "deliberately left off" -- it is reported ok as
     * soon as the zone exists, since every value the field can hold
     * (including 0) is a real, actionable mode. This is a known gap (see
     * TODO.md 8.3's honesty note); closing it would need a persisted
     * "mode explicitly set" flag this pass does not add. */
    {
        readiness_status_t st = (thermo_count > 0) ? READY_OK : READY_CANNOT_YET;
        char detail[READINESS_DETAIL_MAX];
        if (thermo_count == 0) {
            snprintf(detail, sizeof(detail), "set thermocouple count first");
        } else {
            /* Per zone, OFF is a legitimate choice and this item says so. But
             * if EVERY zone is OFF the board cannot heat at all, and reporting
             * that as "ok" is what let a bench firing run its full length with
             * a climbing target and no relay ever closing while this page said
             * the board was ready. One zone off is a choice; all of them off
             * is a kiln that does nothing. */
            uint8_t heating = 0;
            for (uint8_t i = 0; i < thermo_count; i++) {
                zone_control_mode_t m = ZONE_CONTROL_MODE_OFF;
                if (zones_config_get_control_mode(i, &m) && m != ZONE_CONTROL_MODE_OFF) {
                    heating++;
                }
            }
            if (heating == 0) {
                st = READY_NOT_DONE;
                snprintf(detail, sizeof(detail),
                         "every zone is OFF -- no firing can heat anything");
            } else {
                snprintf(detail, sizeof(detail), "%u of %u zones can heat (OFF is a valid choice)",
                         (unsigned)heating, (unsigned)thermo_count);
            }
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "control_mode", "Control mode chosen per zone", st, detail,
                        "/settings/zones", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 5. Guard limits (max_temp_c). TODO.md 96's reconciliation
     * (2026-09-06): max_temp_c == 0 on a zone that CANNOT heat
     * (control_mode == OFF) is a legitimate, permanent choice -- that zone
     * will never command a relay, so it needs no ceiling and is reported
     * deliberately_off. On a zone that CAN heat, max_temp_c == 0 means
     * "uncommissioned", and profile_executor_run()'s guard-5 refusal
     * (zones_config_accessors.h's doc comment on zones_config_get_temp_limits)
     * already refuses to start ANY firing while such a zone is active -- so
     * reporting that state as ok/deliberately_off here, as a prior version
     * of this item did, told the operator the board was ready when starting
     * a firing would immediately be refused. Such a zone is now reported
     * not_done instead. */
    {
        readiness_status_t st;
        char detail[READINESS_DETAIL_MAX];
        if (thermo_count == 0) {
            st = READY_CANNOT_YET;
            snprintf(detail, sizeof(detail), "set thermocouple count first");
        } else {
            uint8_t set_count = 0, off_unset_count = 0, heating_unset_count = 0;
            for (uint8_t i = 0; i < thermo_count; i++) {
                float max_c = 0.0f, min_c = 0.0f;
                zones_config_get_temp_limits(i, &max_c, &min_c);
                if (max_c > 0.0f) {
                    set_count++;
                    continue;
                }
                zone_control_mode_t m = ZONE_CONTROL_MODE_OFF;
                if (zones_config_get_control_mode(i, &m) && m != ZONE_CONTROL_MODE_OFF) {
                    heating_unset_count++;
                } else {
                    off_unset_count++;
                }
            }
            st = readiness_guard_max_temp_status(thermo_count, set_count, heating_unset_count);
            if (st == READY_NOT_DONE) {
                snprintf(detail, sizeof(detail),
                         "%u zone(s) can heat but have no max_temp_c ceiling -- starting a firing will be refused",
                         (unsigned)heating_unset_count);
            } else if (st == READY_OK) {
                snprintf(detail, sizeof(detail), "all %u zones have an explicit max_temp_c ceiling",
                         thermo_count);
            } else {
                snprintf(detail, sizeof(detail),
                         "%u zone(s) have max_temp_c 0, but all are OFF and cannot heat",
                         (unsigned)off_unset_count);
            }
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "guard_max_temp", "Guard limits (max_temp_c)", st, detail,
                        "/settings/zones", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 6. Cross-zone plausibility guard (guard 8). Same 0-is-deliberate
     * convention as max_temp_c above, per zones_http.h's doc comment on
     * cross_zone_max_delta_c -- and it needs >=2 zones reporting to mean
     * anything at all (zones_http.h: "stays inert on a single-zone kiln"). */
    {
        readiness_status_t st;
        char detail[READINESS_DETAIL_MAX];
        if (thermo_count < 2) {
            st = READY_CANNOT_YET;
            snprintf(detail, sizeof(detail), "needs at least 2 zones to mean anything (guard is inert otherwise)");
        } else {
            uint8_t set_count = 0;
            for (uint8_t i = 0; i < thermo_count; i++) {
                float delta = 0.0f;
                if (zones_config_get_cross_zone_delta(i, &delta) && delta > 0.0f) {
                    set_count++;
                }
            }
            if (set_count == 0) {
                st = READY_DELIBERATELY_OFF;
                snprintf(detail, sizeof(detail), "cross_zone_max_delta_c is 0 (guard disabled) on all zones");
            } else {
                st = READY_OK;
                snprintf(detail, sizeof(detail), "%u of %u zones have the guard configured", set_count,
                         thermo_count);
            }
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "guard_cross_zone", "Cross-zone plausibility guard", st,
                        detail, "/settings/zones", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 7. Thermocouple calibration offsets. The storage cannot tell "never
     * looked at" from "looked at and correctly left at zero" -- cal_offset_c
     * has no separate reviewed bit -- and a thermocouple that reads true
     * genuinely needs no offset. So an all-zero result is NOT an unfinished
     * task: there is no action the operator must take, and reporting
     * not_done for it (as this item used to) told them to go do something
     * that does not exist.
     *
     * Chosen fix: report it accurately as an informational state and rename
     * the item after what is actually being reported. The alternative --
     * persisting a real "offsets reviewed" acknowledgement in kiln_nvs when
     * the zones page is saved -- would be strictly more informative, but it
     * buys a new NVS key, a migration for every board already in the field
     * (all of which would read back "never reviewed" and start nagging), and
     * a write coupling from zones_http.c into this module, all to add a
     * checkbox to a low-stakes, non-safety item that gates nothing. Not
     * worth it; if a "first-run walkthrough" ever needs that bit, that is
     * the feature to add it with.
     *
     * deliberately_off is the closest of the four existing states: nothing
     * is missing and no red cross is warranted. */
    {
        readiness_status_t st;
        char detail[READINESS_DETAIL_MAX];
        if (thermo_count == 0) {
            st = READY_CANNOT_YET;
            snprintf(detail, sizeof(detail), "set thermocouple count first");
        } else {
            /* There is no standalone zones_config_get_cal() getter -- reuse
             * zones_config_apply_cal(), which returns raw_c + cal_offset_c
             * (zones_http.c), against a raw_c of 0 to recover the offset
             * itself without needing a new accessor. */
            uint8_t cal_count = 0;
            for (uint8_t i = 0; i < thermo_count; i++) {
                float cal = zones_config_apply_cal(i, 0.0f);
                if (cal != 0.0f) {
                    cal_count++;
                }
            }
            if (cal_count > 0) {
                st = READY_OK;
                snprintf(detail, sizeof(detail), "%u of %u zones have a calibration offset applied",
                         cal_count, thermo_count);
            } else {
                st = READY_DELIBERATELY_OFF;
                snprintf(detail, sizeof(detail),
                         "no offsets applied -- correct if your thermocouples read true");
            }
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "calibration", "Thermocouple calibration offsets", st,
                        detail, "/settings/zones", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 8. At least one profile available to fire. The 28 shipped schedules
     * (profiles_builtin.c) are as runnable as a user's own saved slots, so a
     * board straight out of the box already satisfies this -- counting only
     * user slots, as this item used to, told operators to go create a
     * profile while a full catalogue sat on the /profiles page. Hidden
     * built-ins do not count: hiding is the user's way of removing one from
     * their listings, and something they cannot see is not something they
     * can pick and fire.
     *
     * Renamed from "At least one fire profile saved" for the same reason:
     * the thing satisfying it is shipped, not saved. */
    {
        uint8_t saved = 0;
        for (uint8_t id = 0; id < PROFILES_MAX_COUNT; id++) {
            profile_t p;
            if (profiles_http_get(id, &p)) {
                saved++;
            }
        }
        uint16_t builtin_visible = 0;
        for (size_t i = 0; i < g_builtin_profile_count; i++) {
            uint8_t id = (uint8_t)(PROFILE_BUILTIN_ID_BASE + i);
            if (!profiles_builtin_is_hidden(id)) {
                builtin_visible++;
            }
        }
        bool any = (saved > 0) || (builtin_visible > 0);
        char detail[READINESS_DETAIL_MAX];
        if (!any) {
            snprintf(detail, sizeof(detail),
                     "no profiles saved and every shipped schedule has been removed");
        } else {
            snprintf(detail, sizeof(detail), "%u saved, %u shipped schedule%s available", saved,
                     (unsigned)builtin_visible, builtin_visible == 1 ? "" : "s");
        }
        readiness_status_t st = any ? READY_OK : READY_NOT_DONE;
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "profile_saved",
                        "At least one fire profile available", st, detail, "/profiles", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 9. Autotune run per zone, or gains entered by hand -- either counts,
     * per TODO.md 8.3's own wording ("or gains entered by hand"). */
    {
        readiness_status_t st;
        char detail[READINESS_DETAIL_MAX];
        if (thermo_count == 0) {
            st = READY_CANNOT_YET;
            snprintf(detail, sizeof(detail), "set thermocouple count first");
        } else {
            uint8_t tuned = 0;
            for (uint8_t i = 0; i < thermo_count; i++) {
                float kp = 0, ki = 0, kd = 0;
                float k_dc = 0, tau_s = 0, dead_time_s = 0;
                /* kp > 0 specifically, NOT "any of kp/ki/kd nonzero". A PID
                 * loop with kp == 0 has no proportional term at all: ki alone
                 * integrates its way to setpoint eventually and kd alone does
                 * nothing but fight noise, so neither is a usable set of
                 * gains for a kiln. The looser any-of test previously here
                 * reported a zone as tuned when only kd had been typed in,
                 * which is exactly the "green light on an untuned zone" this
                 * whole page exists to prevent. ki/kd are still allowed to be
                 * zero -- a pure-P zone is crude but genuinely functional. */
                bool has_gains = zones_config_get_pid(i, &kp, &ki, &kd) && kp > 0.0f;
                bool has_model =
                    zones_config_get_model(i, &k_dc, &tau_s, &dead_time_s) && k_dc > 0.0f && tau_s > 0.0f;
                if (has_gains || has_model) {
                    tuned++;
                }
            }
            st = (tuned == thermo_count) ? READY_OK : READY_NOT_DONE;
            snprintf(detail, sizeof(detail), "%u of %u zones have gains or an identified model", tuned,
                     thermo_count);
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "autotune", "Autotune run per zone (or gains by hand)", st,
                        detail, "/settings/zones", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 10. Hardware present and answering. Mirrors /api/status's io_ready/
     * thermo_ready/safety_ready exactly (dashboard_http_get_hw_ready()). */
    {
        bool io_ready = false, thermo_ready = false, safety_ready = false;
        dashboard_http_get_hw_ready(&io_ready, &thermo_ready, &safety_ready);
        readiness_status_t st = (io_ready && thermo_ready && safety_ready) ? READY_OK : READY_NOT_DONE;
        char detail[READINESS_DETAIL_MAX];
        snprintf(detail, sizeof(detail), "io=%s thermo=%s safety=%s", io_ready ? "up" : "down",
                 thermo_ready ? "up" : "down", safety_ready ? "up" : "down");
        /* fix_url used to be "/" -- owner report: "the ready to fire hardware
         * connected item just takes me to the main page." "/" is a real,
         * registered route (wifi_provision_http.c's index_get_handler(), the
         * dashboard), so this never 404'd or looked broken; it just landed
         * somewhere that cannot help diagnose which of io/thermo/safety is
         * down. "/diagnostics" is the actual system-health hub for exactly
         * this (diagnostics_http.c: ESP + board-health info, plus links to
         * /diagnostics/thermo and /safety for the per-subsystem detail this
         * item's own `detail` string already breaks io/thermo/safety out
         * into). */
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "hardware", "Hardware present and answering", st, detail,
                        "/diagnostics", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 10a. Safety processor commissioned. Added 2026-08-22: the whole
     * commissioning parameter set (trip thresholds, TC type, guard limits --
     * COMMISSIONING.md sec 2.1) could be entirely unset and this page still
     * showed every light green, because nothing here ever asked. That is the
     * worst possible omission on a readiness page: the safety processor is
     * the thing that stops a runaway, and an uncommissioned one has no
     * thresholds to trip on.
     *
     * cached_crc == 0 means "never fetched a config from the Pico" -- 0 is
     * never a real CRC, so it doubles as "never commissioned"
     * (safety_cfg_store.h's own note on the sentinel). A param reading
     * set == false is one the Pico has no value for; the sec-1 fields with
     * no compiled-in default read exactly this way until an operator sets
     * them, so any unset param means commissioning is genuinely incomplete
     * rather than merely un-refetched.
     *
     * Reported cannot_yet (not not_done) when the safety link is down: with
     * no link the ESP cannot tell an uncommissioned Pico from one it simply
     * has not talked to yet, and nagging about a task the operator cannot
     * perform right now is what READY_CANNOT_YET exists for. */
    {
        bool io_ready = false, thermo_ready = false, safety_ready = false;
        dashboard_http_get_hw_ready(&io_ready, &thermo_ready, &safety_ready);

        char detail[READINESS_DETAIL_MAX];
        uint16_t cached_crc = safety_cfg_store_cached_crc();
        size_t count = safety_cfg_store_param_count();

        /* ct_installed (param id 0x0109) gates whether ct_channel_map[0..2]
         * and i_normal_a[0..2] are applicable -- see
         * readiness_param_required_for_commissioning()'s comment. Default to
         * 1 (installed) per config_store.c's own safe-direction rule: an
         * unset/unfetched ct_installed must never relax the six CT params'
         * requirement, only an explicit 0 does. */
        uint8_t ct_installed_value = 1;
        for (size_t i = 0; i < count; i++) {
            safety_cfg_param_t p;
            if (safety_cfg_store_get_by_index(i, &p) && p.param_id == 0x0109u && p.set) {
                ct_installed_value = p.value.u8_val;
                break;
            }
        }

        size_t applicable = 0;
        size_t unset = 0;
        size_t excluded = 0;
        for (size_t i = 0; i < count; i++) {
            safety_cfg_param_t p;
            if (!safety_cfg_store_get_by_index(i, &p)) {
                continue;
            }
            if (!readiness_param_required_for_commissioning(p.param_id, ct_installed_value)) {
                excluded++;
                continue;
            }
            applicable++;
            if (!p.set) {
                unset++;
            }
        }

        readiness_status_t st = readiness_commissioning_status(safety_ready, cached_crc, unset, applicable);

        if (cached_crc == 0) {
            snprintf(detail, sizeof(detail), "%s",
                     safety_ready ? "no commissioning values have ever been read from the safety processor"
                                  : "safety link is down -- cannot tell uncommissioned from unread");
        } else if (unset == 0 && excluded == 0) {
            snprintf(detail, sizeof(detail), "all %u safety parameters have values (config_crc 0x%04X)",
                     (unsigned)count, (unsigned)cached_crc);
        } else if (unset == 0) {
            snprintf(detail, sizeof(detail),
                     "all %u applicable safety parameters have values (%u not applicable to this hardware "
                     "config, config_crc 0x%04X)",
                     (unsigned)applicable, (unsigned)excluded, (unsigned)cached_crc);
        } else {
            snprintf(detail, sizeof(detail), "%u of %u applicable safety parameters still have no value",
                     (unsigned)unset, (unsigned)applicable);
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "safety_commissioned", "Safety processor commissioned", st,
                        detail, "/safety/commissioning", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 10b. Safety processor trip status. 2026-09-08 live dry run
     * (docs/audits/setup_wizard_live_dryrun_2026-09-08.md) found the wizard
     * reporting complete=true, reasons=[] on a board with S5 actively
     * latched (diag_trip_mask nonzero): item 10 above only asks whether the
     * safety link is UP, never whether the processor on the other end is
     * currently refusing to let the board heat. See
     * readiness_safety_trip_status()'s doc comment in readiness_http.h for
     * the full reasoning; this is deliberately its own item rather than
     * folded into item 10, because "hardware answering" and "hardware
     * currently vetoing a firing" are different facts an operator needs to
     * tell apart from the detail string alone. */
    {
        bool link_up = false;
        uint16_t trip_mask = 0;
        dashboard_http_get_safety_trip(&link_up, &trip_mask);
        readiness_status_t st = readiness_safety_trip_status(link_up, trip_mask);
        char detail[READINESS_DETAIL_MAX];
        if (!link_up) {
            snprintf(detail, sizeof(detail), "safety link is down -- cannot tell tripped from unknown");
        } else if (trip_mask != 0u) {
            snprintf(detail, sizeof(detail),
                     "safety processor has an ACTIVE trip latched (diag_trip_mask 0x%04X) -- firing will be refused",
                     (unsigned)trip_mask);
        } else {
            snprintf(detail, sizeof(detail), "no guard is currently tripped");
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "safety_trip", "Safety processor trip status", st, detail,
                        "/safety", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 11. Storage sections compatible -- the zones_config_valid flag (TODO.md
     * 8.2) plus every NVS partition's present/mounted state (nvs_report.c). */
    {
        bool zones_valid = zones_config_is_valid();
        size_t nvs_count = 0;
        const nvs_report_section_t *sections = nvs_report_get(&nvs_count);
        bool all_mounted = true;
        char bad_name[32] = "";
        for (size_t i = 0; i < nvs_count; i++) {
            if (!sections[i].present || !sections[i].mounted) {
                all_mounted = false;
                strncpy(bad_name, sections[i].name, sizeof(bad_name) - 1);
                break;
            }
        }
        readiness_status_t st = (zones_valid && all_mounted) ? READY_OK : READY_NOT_DONE;
        char detail[READINESS_DETAIL_MAX];
        if (!zones_valid) {
            snprintf(detail, sizeof(detail), "zone config failed to load cleanly -- see /settings/zones");
        } else if (!all_mounted) {
            snprintf(detail, sizeof(detail), "NVS partition '%s' is not present/mounted", bad_name);
        } else {
            snprintf(detail, sizeof(detail), "all storage sections mounted, zone config valid");
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "storage", "Storage sections compatible", st, detail,
                        "/settings/zones", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 12. Unacknowledged crash report. 2026-09-08 follow-on to the
     * safety_trip item above: capability_preflight (tools/PcTools) already
     * refuses to start a run on a board with an unacknowledged crash, but
     * this page never asked, so readiness could show every light green
     * while a different layer was already refusing. See
     * readiness_crash_report_status()'s doc comment in readiness_http.h. */
    {
        crash_report_record_t rec;
        bool have_record = crash_report_get(&rec);
        bool acknowledged = have_record && rec.acknowledged != 0;
        readiness_status_t st = readiness_crash_report_status(have_record, acknowledged);
        char detail[READINESS_DETAIL_MAX];
        if (!have_record) {
            snprintf(detail, sizeof(detail), "no crash on record");
        } else if (!acknowledged) {
            snprintf(detail, sizeof(detail),
                     "unacknowledged crash on record (%s, task %s) -- review /diagnostics before firing",
                     rec.exc_cause_str[0] ? rec.exc_cause_str : "unknown cause", rec.exc_task);
        } else {
            snprintf(detail, sizeof(detail), "last crash on record has been acknowledged");
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "crash_report", "Unacknowledged crash report", st, detail,
                        "/diagnostics", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 13. Recovery-mode boot. 2026-09-08 follow-on: a board in recovery mode
     * has skipped starting profile_executor/autotune_engine/rules_task
     * (boot_guard.h's RECOVERY_MODE_ENABLED gate) and cannot fire a profile
     * this boot no matter what every other item says. See
     * readiness_recovery_mode_status()'s doc comment in readiness_http.h. */
    {
        bool recovery = boot_guard_is_recovery_mode();
        readiness_status_t st = readiness_recovery_mode_status(recovery);
        char detail[READINESS_DETAIL_MAX];
        snprintf(detail, sizeof(detail), "%s",
                 recovery ? "this boot is in RECOVERY MODE -- profile executor, autotune, and rules are "
                            "not running; reflash or clear the boot-guard counter"
                          : "not in recovery mode");
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "recovery_mode", "Recovery-mode boot", st, detail,
                        "/diagnostics", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 14. Config filesystem (cfg_fs). 2026-09-08 follow-on: a failed/absent
     * `cfg` LittleFS mount leaves every *_cfg_fs.c bridge running on its NVS
     * fallback alone -- degraded, not broken, so this is deliberately
     * non-blocking. See readiness_cfg_fs_status()'s doc comment in
     * readiness_http.h for why DELIBERATELY_OFF rather than NOT_DONE. */
    {
        bool mounted = cfg_fs_is_available();
        readiness_status_t st = readiness_cfg_fs_status(mounted);
        char detail[READINESS_DETAIL_MAX];
        if (mounted) {
            snprintf(detail, sizeof(detail), "cfg filesystem mounted -- config is file-backed with NVS mirror");
        } else {
            snprintf(detail, sizeof(detail),
                     "cfg filesystem not mounted -- running on NVS-only fallback storage (degraded, not "
                     "blocking; see /diagnostics)");
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "cfg_fs", "Config filesystem (cfg_fs)", st, detail,
                        "/diagnostics", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 15. Safety link command delivery. 2026-09-08 follow-on, the fourth
     * blind spot: a Pico that keeps answering GET_STATUS while its
     * PUSH_CONTEXT handling has wedged reads link_up=true and every item
     * above that only checks link_up reads clean. See
     * readiness_safety_context_status()'s doc comment in readiness_http.h
     * for why diag_context_age_100ms is the one honest signal for this and
     * why no round-trip command-ack equivalent exists to use instead. */
    {
        bool link_up = false, diag_ever_received = false;
        uint8_t context_age = 0;
        dashboard_http_get_safety_context_health(&link_up, &diag_ever_received, &context_age);
        readiness_status_t st = readiness_safety_context_status(link_up, diag_ever_received, context_age);
        char detail[READINESS_DETAIL_MAX];
        if (!link_up) {
            snprintf(detail, sizeof(detail), "safety link is down -- cannot tell delivery healthy from unknown");
        } else if (!diag_ever_received) {
            snprintf(detail, sizeof(detail),
                     "no DIAG frame received yet -- cannot tell command delivery healthy from unknown");
        } else if (st == READY_NOT_DONE) {
            snprintf(detail, sizeof(detail),
                     "safety processor has not applied a context update in %u.%us -- link answers status polls "
                     "but commands may not be landing",
                     (unsigned)(context_age / 10u), (unsigned)(context_age % 10u));
        } else {
            snprintf(detail, sizeof(detail), "safety processor is applying context updates normally");
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "safety_context", "Safety link command delivery", st, detail,
                        "/safety", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* 16. E-stop interlock verified. 2026-09-08 follow-on to 3b5ced00
     * (firmware-side E-stop is test-locked, polarity now configurable via
     * param 0x0212). The board only provides POLE 2 (GPIO9/R10/C3/J1) --
     * POLE 1, in series with the external line contactor's coil, is wiring
     * the OWNER adds and no firmware check can ever exercise it. This item
     * reports whether the OPERATOR has confirmed running the bench
     * verification procedure (firmware/SaftyFW/README.md) -- see
     * readiness_estop_verification_status()'s doc comment in
     * readiness_http.h for why this is unconditionally blocking, and
     * estop_verification.h for exactly what invalidates a standing
     * verification (a commit to param 0x0212, or a "kiln"/"all"-scope
     * factory reset). The detail string also surfaces the two gaps no layer
     * currently covers, so an operator learns them here rather than in an
     * audit: a WELDED line contactor (stuck closed) is undetectable by
     * either pole, and at ACTIVE_LOW polarity a CUT signal line reads
     * identical to healthy because R10 pulls the input up -- ACTIVE_HIGH
     * (the default) is the polarity where a cut line is instead caught. */
    {
        bool verified = estop_verification_is_verified();
        readiness_status_t st = readiness_estop_verification_status(verified);
        char detail[READINESS_DETAIL_MAX];
        if (verified) {
            snprintf(detail, sizeof(detail),
                     "confirmed by operator. Known gaps: welded contactor undetectable; ACTIVE_LOW "
                     "polarity can't see a cut signal line -- use ACTIVE_HIGH default");
        } else {
            snprintf(detail, sizeof(detail),
                     "never confirmed -- pole 1 (contactor coil) is wiring firmware can't see. Run the "
                     "README.md bench procedure, no heat, then confirm via /safety");
        }
        size_t before_o = o;
        o = append_item(json, item_cap, o, first, "estop_verified", "E-stop interlock verified", st, detail,
                        "/safety", &dropped);
        if (o != before_o) {
            first = false;
        }
    }

    /* An operator reads this list to decide whether it is safe to fire. A
     * checklist that quietly came back short would show no red crosses and
     * look like a pass, so a dropped item is reported AS an item -- and as
     * READY_CANNOT_YET, because what those checks would have said is exactly
     * what is unknown. */
    if (dropped) {
        bool notice_dropped = false;
        o = append_item(json, sizeof(json), o, first,
                        "checklist_truncated", "Checklist incomplete", READY_CANNOT_YET,
                        "the board ran out of room to report every check -- items are missing "
                        "from this list, so treat it as inconclusive, not as a pass",
                        "/diagnostics", &notice_dropped);
        first = false;
        if (notice_dropped) {
            /* Would mean READINESS_TRUNC_RESERVE is too small for its own
             * notice -- a build-time sizing error, not a runtime condition. */
            ESP_LOGE(TAG, "readiness truncation notice did not fit -- raise READINESS_TRUNC_RESERVE");
        }
    }

    n = snprintf(json + o, sizeof(json) - o, "]}");
    if (n < 0 || (size_t)n >= sizeof(json) - o) {
        /* Unreachable while the reserve holds, but an unterminated body is
         * invalid JSON, and the page's fetch would throw and render nothing at
         * all -- so close the document by force rather than ship a fragment. */
        o = sizeof(json) - 3;
        memcpy(json + o, "]}", 2);
        o += 2;
        ESP_LOGE(TAG, "readiness JSON had no room to close -- forced terminator");
    } else {
        o += (size_t)n;
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, o);
}

/* TODO.md 10.6a: content negotiation lives in web_encoding.h's shared
 * web_client_accepts_gzip() -- absent Accept-Encoding is legal and served
 * gzip (RFC 9110 s12.5.3); a header that explicitly excludes gzip gets an
 * uncompressed 406 rather than a body it cannot decode. */
static esp_err_t page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "readiness_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)readiness_page_html_gz_start,
                           (size_t)(readiness_page_html_gz_end - readiness_page_html_gz_start));
}

esp_err_t readiness_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t page_uri = {
        .uri = "/readiness", .method = HTTP_GET, .handler = page_get_handler,
    };
    static const httpd_uri_t api_uri = {
        .uri = "/api/readiness", .method = HTTP_GET, .handler = api_readiness_get_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/readiness) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &api_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/readiness) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "readiness API up");
    return ESP_OK;
}
