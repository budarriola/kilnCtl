#ifndef SAFETY_TRIP_WORDS_H
#define SAFETY_TRIP_WORDS_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Short, single-line decode of SaftyFW's safety_guards.h SAFETY_TRIP_* enum,
 * for LCD surfaces whose row must never wrap (480x320, no-scroll budget --
 * see ui_page_home.c and ui_page_safety.c's header comments). This is the
 * ONE shared copy for those two call sites; previously ui_page_home.c held
 * a private static copy and ui_page_safety.c printed the raw 0x%02X value
 * instead of decoding it at all (ROADMAP.md "LCD shows a raw bitmask where
 * the web shows a sentence" loose end) -- pulling it into a header both
 * ui_page_home.c and ui_page_safety.c #include removes that gap without
 * adding a third copy of the table.
 *
 * profile_executor.h's profile_executor_safety_trip_words() is a SEPARATE,
 * intentionally different copy: it returns the full sentence that fits a
 * 96-byte fault_reason field, not a single unwrappable LCD line, so it is
 * not reused here. main_page.html's SAFETY_TRIP_WORDS (JavaScript) is a
 * third, cross-language copy for the same reason profile_executor.h's
 * comment gives: KilnFW's web page cannot #include a C header, so the
 * wording has to be kept in sync by hand across the C/JS boundary. All
 * three tables must be updated together if safety_guards.h's enum changes. */
static inline const char *safety_trip_words_short(uint8_t reason)
{
    switch (reason) {
    case 1:  return "S1 over-temp limit";
    case 2:  return "S2 over setpoint";
    case 3:  return "S3 relay stuck on";
    case 5:  return "S5 TC invalid";
    case 6:  return "S6a main ctrl fault";
    case 7:  return "S6b link dead";
    case 8:  return "S7 E-stop";
    case 9:  return "S8 rise too fast";
    case 10: return "S9 contactor welded";
    case 12: return "S11 sensor frozen";
    case 13: return "S12 enclosure hot";
    case 14: return "S13 borrowed TC dead";
    case 15: return "config corrupt";
    case 16: return "self-test fail";
    default: return "unknown guard";
    }
}

/* CAUSE: what was actually detected -- the owner's "more important half"
 * (2026-08-27 scope change, verbatim: "all faults reported to the user
 * should come with... what was detected wrong"). Numbers (the actual
 * temperature, threshold, current, etc.) are NOT baked into this static
 * table -- they live on the wire per-trip (safety_link_status_t's
 * trip_safety_tc_c/trip_deciding_threshold/trip_current_a) and belong next
 * to this sentence at the call site, not duplicated into it here. Where this
 * firmware genuinely has no numbers for a guard (S3/S9's relay state, S8/
 * S11's specifics), the sentence says the mechanism honestly rather than
 * inventing a figure -- see this project's CLAUDE.md-adjacent report on
 * this pass for exactly which guards that applies to. */
static inline const char *safety_trip_words_cause(uint8_t reason)
{
    switch (reason) {
    case 1:  return "Chamber temperature exceeded the absolute over-temp limit";
    case 2:  return "Chamber stayed above setpoint longer than the over-setpoint window allows";
    case 3:  return "Relay/contactor read ON while the safety processor commanded it OFF";
    case 5:  return "Safety thermocouple reading was invalid past its grace period";
    case 6:  return "The ESP main controller asserted the isolated fault line -- see fault source below";
    case 7:  return "The safety link between ESP and safety processor went silent past its timeout";
    case 8:  return "E-stop input was asserted";
    case 9:  return "Chamber temperature rose faster than physically possible for this kiln";
    case 10: return "Relay/contactor still shows load current after being commanded OFF -- may be welded";
    case 12: return "A safety sensor reading stopped changing (frozen), not tracking a moving load";
    case 13: return "Enclosure/cold-junction temperature exceeded its limit";
    case 14: return "A borrowed zone's thermocouple reading stopped updating";
    case 15: return "Safety config failed its integrity check on load";
    case 16: return "Safety processor self-test failed at boot";
    default: return "Unrecognised guard code";
    }
}

/* CAUSE, WITH NUMBERS -- 2026-08-28 scope change ("all faults... should come
 * with... what was detected wrong", numbers not a generic sentence). Takes
 * the trip snapshot fields safety_link_status_t already carries off Frame D
 * (TRIP_EVENT) and, where THIS guard's deciding number is actually among
 * them, folds it into the sentence. Falls back to plain
 * safety_trip_words_cause(reason) -- verbatim, no placeholder appended --
 * for every guard/field combination this firmware genuinely cannot number:
 *   S5 (dual count+time threshold, no single magnitude -- SaftyFW's
 *       safety_guards_deciding_threshold_c() default case), S6a (the
 *       fault-source list IS its number, see safety_fault_source_words()),
 *       S7/S8 (boolean conditions), S8-rate (ships disabled, never trips),
 *       S12 (the number that trips it is the enclosure/cold-junction
 *       reading, but Frame D's trip_safety_tc_c is the SAFETY thermocouple,
 *       a different sensor -- SaftyFW's safety_core.c always captures
 *       input.tc_c regardless of which guard tripped, so this field is
 *       simply the wrong sensor for S12 and using it would be worse than
 *       the honest fallback; the true CJ reading at trip time is not on
 *       the wire at all today), S13 (the elapsed-stale-time number is not
 *       on the wire, only the threshold it was judged against is, via
 *       trip_deciding_threshold -- said as "over Ns" rather than invented).
 * trip_current_a[3] is amps on current-sense channels 1..3 (not
 * per-relay -- SAFETY_MODEL.md's channel map), trip_context_age_100ms is
 * hundredths of a second, both "at trip" snapshots, both left at their
 * caller-supplied sentinel (NAN / 255) when a real TRIP_EVENT has never
 * been received -- callers must gate on trip_event_ever_received same as
 * every other trip_* field, this function does not check it itself. Writes
 * into buf (buf_len bytes, always NUL-terminated); returns buf. */
static inline const char *safety_trip_words_cause_numbered(uint8_t reason, float trip_safety_tc_c,
                                                             float trip_deciding_threshold,
                                                             const float trip_current_a[3],
                                                             uint8_t trip_context_age_100ms,
                                                             char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return buf;
    }
    bool tc_ok = !isnan(trip_safety_tc_c);
    bool thr_ok = !isnan(trip_deciding_threshold);
    /* trip_current_a is a struct array member at every call site (dashboard_
     * http.c's ds.trip_current_a, ui_page_diagnostics.c's same field) -- it
     * decays to a non-NULL pointer unconditionally, so a `trip_current_a !=
     * NULL` test can never be false and is not a real gate (same "check that
     * structurally cannot fire" class as this repo's P1-A/config_store.c
     * finding). What actually varies at runtime is whether the THREE floats
     * behind that pointer are real readings -- an uncommissioned/never-
     * received current-sense channel is the caller-supplied NAN sentinel,
     * same convention as trip_safety_tc_c/trip_deciding_threshold above.
     * Without this check, S3/S9 on a board with uncommissioned current
     * sensing rendered "nanA/nanA/nanA" instead of the honest fallback. */
    bool cur_ok = (trip_current_a != NULL) && !isnan(trip_current_a[0]) &&
                  !isnan(trip_current_a[1]) && !isnan(trip_current_a[2]);
    switch (reason) {
    case 1: /* S1 overtemp: both numbers are the right sensor/threshold */
        if (tc_ok && thr_ok) {
            snprintf(buf, buf_len, "Chamber reached %.1fC, above the %.1fC over-temp limit.",
                      (double)trip_safety_tc_c, (double)trip_deciding_threshold);
            return buf;
        }
        break;
    case 2: /* S2 over-setpoint: threshold here is the overshoot MARGIN, not
             * an absolute setpoint -- the setpoint itself is not on this
             * frame, so it is deliberately not claimed. */
        if (tc_ok && thr_ok) {
            snprintf(buf, buf_len,
                      "Chamber reached %.1fC, more than %.1fC above setpoint for longer than "
                      "the over-setpoint window allows.",
                      (double)trip_safety_tc_c, (double)trip_deciding_threshold);
            return buf;
        }
        break;
    case 3: /* S3 load stuck on: current-sense channels vs. the present-
             * current threshold. Which physical channel maps to which zone
             * is not resolved here (SAFETY_MODEL.md's channel map). */
        if (cur_ok && thr_ok) {
            snprintf(buf, buf_len,
                      "Current sense reads %.2fA/%.2fA/%.2fA (ch1/2/3) while the relay was "
                      "commanded OFF -- above the %.2fA present-current threshold.",
                      (double)trip_current_a[0], (double)trip_current_a[1],
                      (double)trip_current_a[2], (double)trip_deciding_threshold);
            return buf;
        }
        break;
    case 7: /* S6b link dead: the deciding number is how long the link had
             * been silent, not a temperature -- trip_context_age_100ms. */
        if (trip_context_age_100ms != 255u) {
            snprintf(buf, buf_len,
                      "The safety link had not been heard from for %.1fs when this guard "
                      "tripped.",
                      (double)trip_context_age_100ms / 10.0);
            return buf;
        }
        break;
    case 10: /* S9 contactor welded: same current-sense reading as S3, no
              * threshold to quote (it is an escalation, not a fresh
              * crossing). */
        if (cur_ok) {
            snprintf(buf, buf_len,
                      "Current sense still reads %.2fA/%.2fA/%.2fA (ch1/2/3) after the "
                      "contactor was commanded OFF -- may be welded.",
                      (double)trip_current_a[0], (double)trip_current_a[1],
                      (double)trip_current_a[2]);
            return buf;
        }
        break;
    case 12: /* S11 frozen sensor: reading (trip_safety_tc_c) plus the
              * frozen-window threshold it was judged against. */
        if (tc_ok && thr_ok) {
            snprintf(buf, buf_len,
                      "A safety sensor held at %.1fC without changing for longer than the "
                      "%.0fs frozen-window threshold.",
                      (double)trip_safety_tc_c, (double)trip_deciding_threshold);
            return buf;
        }
        break;
    case 13: /* S12 enclosure temp: see this function's header comment --
              * trip_safety_tc_c is the wrong sensor for this guard, so only
              * the threshold (the number that IS right) is quoted, and the
              * missing reading is said plainly rather than guessed. */
        if (thr_ok) {
            snprintf(buf, buf_len,
                      "Enclosure/cold-junction temperature exceeded its %.1fC limit "
                      "(this firmware does not currently carry the exact reading at trip).",
                      (double)trip_deciding_threshold);
            return buf;
        }
        break;
    case 14: /* S13 borrowed TC stale: threshold (seconds) is on the wire,
              * the actual elapsed-stale time at trip is not. */
        if (thr_ok) {
            snprintf(buf, buf_len,
                      "A borrowed zone's thermocouple stopped updating for longer than the "
                      "%.0fs threshold (exact elapsed time at trip not carried on this link).",
                      (double)trip_deciding_threshold);
            return buf;
        }
        break;
    default:
        break;
    }
    snprintf(buf, buf_len, "%s", safety_trip_words_cause(reason));
    return buf;
}

/* REMEDY: what an operator standing at the kiln should actually do. Written
 * for that person, not a developer -- and honest about which guards are NOT
 * clearable from the UI at all (S9: possibly welded contactor, mains may
 * still be live -- SAFETY_MODEL.md sec 4/9 and link_task.c's own
 * LINK_CLEAR_TRIP_REFUSE_INEFFECTIVE comment). Every latch below still needs
 * an explicit Clear Trip after the cause is resolved -- see this header's
 * top comment and firmware/SaftyFW/docs/SAFETY_MODEL.md sec 6 for why a new
 * firing and a reboot both leave it latched. */
static inline const char *safety_trip_words_remedy(uint8_t reason)
{
    switch (reason) {
    case 1:  return "Let the kiln cool below the limit, then press Clear Trip.";
    case 2:  return "Let the chamber return under setpoint, then press Clear Trip.";
    case 3:  return "De-energize the relay/contactor and confirm it opens, then press Clear Trip.";
    case 5:  return "Check/replace the safety thermocouple and its wiring, then press Clear Trip.";
    case 6:  return "Resolve the fault source named below, then press Clear Trip.";
    case 7:  return "Restore the wired link between the ESP and safety processor, then press Clear Trip.";
    case 8:  return "Release the E-stop, then press Clear Trip.";
    case 9:  return "This guard ships disabled; if it ever trips, treat it as a possible runaway and check the load.";
    case 10: return "NOT clearable from here. Remove power at the breaker and inspect the contactor before touching anything else.";
    case 12: return "Power-cycle or replace the frozen sensor, then press Clear Trip.";
    case 13: return "Let the enclosure cool below its limit, then press Clear Trip.";
    case 14: return "Check the borrowed zone's thermocouple and its wiring, then press Clear Trip.";
    case 15: return "Recommission the safety config from the commissioning page, then press Clear Trip.";
    case 16: return "Power-cycle the safety processor; if it recurs, the board needs service.";
    default: return "See firmware/SaftyFW/docs/SAFETY_MODEL.md for this guard.";
    }
}

/* Decodes Frame B (SAFETY_CMD_DIAG)'s warn_mask, per LINK_PROTOCOL.md's
 * "warn_mask bit numbering" table -- one bit per WARN-capable guard, where a
 * guard with a real SAFETY_TRIP_* code uses (that code - 1) and S4/S10 (WARN-
 * only, no trip code) use their reserved gap bits 3/10. S14/S15
 * (CT_COMMISSIONING_PLAN.md) postdate that scheme and had no reserved gap, so
 * they take bits 14/15 directly (codes 15/16 already belong to the unrelated
 * CONFIG_CORRUPT/SELF_TEST trip guards) -- see that doc's own bit table for
 * the authoritative list this mirrors. Until this function existed, S14/S15
 * (and S4/S5/S9/S10/S12/S13's WARN bits) had no decoded name on this side at
 * all -- safety_page.html printed the mask as raw hex only. Writes a comma-
 * joined, short, operator-facing list into buf (buf_len bytes, always
 * NUL-terminated); mask == 0 (or no recognised bit set) writes "none". */
static inline const char *safety_warn_words_short(uint16_t warn_mask, char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return buf;
    }
    static const struct { uint16_t bit; const char *word; } bits[] = {
        { 1u << 3,  "S4 load-should-be-off current present" },
        { 1u << 4,  "S5 sensor invalid (pre-trip)" },
        { 1u << 9,  "S9 current present, uncommissioned" },
        { 1u << 10, "S10 safety TC disagrees with zone TCs" },
        { 1u << 12, "S12 enclosure/CJ over-temp (pre-trip)" },
        { 1u << 13, "S13 borrowed channel not updating (pre-trip)" },
        { 1u << 14, "S14 current above measured normal" },
        { 1u << 15, "S15 zone under-current (open heater)" },
    };
    buf[0] = '\0';
    if (warn_mask == 0u) {
        snprintf(buf, buf_len, "none");
        return buf;
    }
    bool first = true;
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if (warn_mask & bits[i].bit) {
            size_t used = strlen(buf);
            snprintf(buf + used, buf_len - used, "%s%s", first ? "" : ", ", bits[i].word);
            first = false;
        }
    }
    if (first) {
        /* No recognised bit matched (an unlisted/future bit only) -- do not
         * silently claim "none" for a mask that is genuinely non-zero. */
        snprintf(buf, buf_len, "unrecognised warn bits 0x%04X", (unsigned)warn_mask);
    }
    return buf;
}

/* Decodes safety_link.h's safety_fault_source_t bitmask (the ESP's OWN
 * reason for driving the isolated fault line, one bit per source -- see
 * that header's comment: SaftyFW sees only the resulting single bit and
 * cannot know which of these caused it) into a short, comma-joined,
 * operator-facing sentence. Writes into buf (buf_len bytes, always
 * NUL-terminated on success); returns buf. mask == 0 writes "none". Used by
 * BOTH surfaces that show heat_block_sources/trip_fault_sources (live vs.
 * at-trip-time -- see safety_link.h's trip_fault_sources field comment for
 * why those two can legitimately disagree) so the wording cannot drift
 * between them, same reasoning as safety_trip_words_short() above. */
static inline const char *safety_fault_source_words(uint32_t mask, char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return buf;
    }
    if (mask == 0u) {
        snprintf(buf, buf_len, "none");
        return buf;
    }
    static const struct { uint32_t bit; const char *word; } bits[] = {
        { 0x01u, "manual (PC SET_FAULT_OUT)" },
        { 0x02u, "PC control link lost" },
        { 0x04u, "main-board thermocouple fault" },
        { 0x08u, "safety UART link stale" },
        { 0x10u, "app-level fault" },
        { 0x20u, "thermal sanity check" },
    };
    buf[0] = '\0';
    bool first = true;
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        if (mask & bits[i].bit) {
            size_t used = strlen(buf);
            snprintf(buf + used, buf_len - used, "%s%s", first ? "" : ", ", bits[i].word);
            first = false;
        }
    }
    return buf;
}

/* Per-source remedy for the S6a "identify the source, then act" flow --
 * safety_trip_words_remedy(6) above defers to this for detail. Some sources
 * are not operator-actionable from the kiln at all (SAFETY_LINK: this
 * board's own link to the safety processor, not a firing-side cause) -- said
 * plainly rather than implying a Clear Trip alone will fix it, same rule the
 * scope-change request calls out for S9 above. Takes exactly one bit; a
 * caller with a multi-bit mask calls this once per set bit (see
 * dashboard_http.c/main_page.html's fault-source list rendering). */
static inline const char *safety_fault_source_remedy_one(uint32_t bit)
{
    switch (bit) {
    case 0x01u: return "Cleared by the PC operator who set it (SET_FAULT_OUT clear), then Clear Trip.";
    case 0x02u: return "Reconnect the PC control link, then Clear Trip.";
    case 0x04u: return "Clear the main-board thermocouple fault (Thermocouple Faults page), then Clear Trip.";
    case 0x08u: return "Not fixable from the firing side: this board's own link to the safety processor went stale -- check UART wiring/power between them.";
    case 0x10u: return "Check the app-level condition that raised this (KilnFW logs), then Clear Trip.";
    case 0x20u: return "Check the flagged zone: temperature isn't tracking the commanded relay state as expected.";
    default:    return "See firmware/SaftyFW/docs/SAFETY_MODEL.md sec 6.";
    }
}

#endif /* SAFETY_TRIP_WORDS_H */
