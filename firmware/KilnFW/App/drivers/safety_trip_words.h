#ifndef SAFETY_TRIP_WORDS_H
#define SAFETY_TRIP_WORDS_H

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
