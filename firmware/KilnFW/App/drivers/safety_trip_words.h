#ifndef SAFETY_TRIP_WORDS_H
#define SAFETY_TRIP_WORDS_H

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
    case 1:  return "S1 overtemp";
    case 2:  return "S2 over setpoint";
    case 3:  return "S3 relay stuck on";
    case 5:  return "S5 sensor invalid";
    case 6:  return "S6a main fault";
    case 7:  return "S6b link dead";
    case 8:  return "S7 E-stop";
    case 9:  return "S8 rate of rise";
    case 10: return "S9 INEFFECTIVE";
    case 12: return "S11 frozen sensor";
    case 13: return "S12 enclosure temp";
    case 14: return "S13 stale data";
    case 15: return "config corrupt";
    case 16: return "self-test fail";
    default: return "unknown guard";
    }
}

#endif /* SAFETY_TRIP_WORDS_H */
