/* NOT ACTUALLY GENERATED -- hand-maintained as of 2026-09-04.
 *
 * This file used to be produced by tools/scripts/gen_builtin_profiles.py from
 * a FAMILIES dict that was its single source of truth. That script and the
 * scraped source JSON it read are gone from the repo (confirmed: a repo-wide
 * find for gen_builtin_profiles.py turns up nothing), so the "Regenerate
 * with:" instruction that used to live here was stale and actively
 * misleading -- there is no working regeneration path today. Do not restore
 * a comment claiming otherwise until a real generator exists again.
 *
 * Consequence: every field below, including the .firing_type and .cone
 * fields added 2026-09-04, is now maintained BY HAND directly in this file.
 * Treat the segment values (.target_c / .ramp_c_per_hr / .dwell_min) with the
 * same care a generator would have enforced -- a misplaced digit in a firing
 * schedule ruins a kiln load -- but there is no tooling left to catch one.
 *
 * .firing_type (Bisque / Glaze / Other) and .cone were added to support
 * browsing by firing-type-then-cone instead of by publisher family. .cone is
 * derived per entry from the schedule's peak segment target_c and the
 * ramp_c_per_hr of the segment that reaches it, read off the standard
 * two-speed Orton cone chart (a 60 C/hr "slow" column and a 108 C/hr "fast"
 * column -- the ramp rate that actually reaches the peak picks the column).
 * Where the title already states a cone, that stated cone was used instead
 * of the derived one (titles are curated publisher data; the chart-lookup
 * here was reconstructed from memory and is the fallback for entries with no
 * stated cone, not a replacement for a stated one). .firing_type has no such
 * derivation -- it was hand-classified per entry from title wording and
 * profile shape (e.g. "Bartlett Fast Bisque Cone 04" -> Bisque; a decal
 * firing or a quartz-inversion crack-avoidance schedule -> Other, since
 * forcing either into Bisque or Glaze would misclassify it). See the task
 * report for the full 28-entry classification table so it can be corrected.
 *
 * This file holds ONLY the catalogue table. It is #included by
 * profiles_builtin.c, which is hand-written and holds the API implementation
 * -- keeping the table in its own file means editing it directly (as this
 * comment now instructs) still cannot touch the hand-written code below the
 * #include.
 *
 * Hand-transcribing 136 segments is 136 chances to misplace a digit, and a
 * misplaced digit in a firing schedule ruins a kiln load.
 *
 * Source: the published firing schedules at https://digitalfire.com/schedule
 * (see the Credits section on the web Profiles page). Values are taken from
 * each schedule's Celsius column verbatim. The site's tables also carry a
 * cumulative-elapsed-time column which is NOT a hold time -- it is not
 * imported, and conflating the two was the first mistake made reading these.
 *
 * Semantics note: a source step with rate 0 C/hr means "no rate limit, go as
 * fast as the kiln can" on a Bartlett-style controller. profile_executor.c
 * already reads ramp_c_per_hr <= 0 exactly that way (it jumps the setpoint
 * straight to the segment target), so 0 is imported unchanged.
 */

const builtin_profile_t g_builtin_profiles[] = {
    {
        .code = "03DSFF",
        .title = "Cone 03 Fast Fire",
        .slug = "03dsff",
        .family = "General",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = -3,
        .segment_count = 4,
        .segments = {
            { .target_c = 121.0f, .ramp_c_per_hr = 194.0f, .dwell_min = 30 },
            { .target_c = 1065.0f, .ramp_c_per_hr = 666.0f, .dwell_min = 15 },
            { .target_c = 1010.0f, .ramp_c_per_hr = 555.0f, .dwell_min = 30 },
            { .target_c = 760.0f, .ramp_c_per_hr = 83.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "04DSDH",
        .title = "Low Temperature Drop-and-Hold",
        .slug = "04dsdh",
        .family = "General",
        .firing_type = PROFILE_FIRING_GLAZE,
        /* Verified 2026-09-04 against https://digitalfire.com/schedule/04dsdh:
         * "This is configured for cone 04" -- corrected from -5 (memory-derived,
         * wrong) to -4. */
        .cone = -4,
        .segment_count = 5,
        .segments = {
            { .target_c = 115.0f, .ramp_c_per_hr = 60.0f, .dwell_min = 60 },
            { .target_c = 982.0f, .ramp_c_per_hr = 250.0f, .dwell_min = 15 },
            { .target_c = 1037.0f, .ramp_c_per_hr = 60.0f, .dwell_min = 15 },
            { .target_c = 982.0f, .ramp_c_per_hr = 555.0f, .dwell_min = 30 },
            { .target_c = 760.0f, .ramp_c_per_hr = 83.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "BQ1000",
        .title = "Plainsman Electric Bisque",
        .slug = "bq1000",
        .family = "Plainsman",
        .firing_type = PROFILE_FIRING_BISQUE,
        /* Verified 2026-09-04 against https://digitalfire.com/schedule/bq1000:
         * page states "about cone 05" (approximate, not a firm stated cone).
         * Corrected from -6 (memory-derived, wrong) to -5. */
        .cone = -5,
        .segment_count = 4,
        .segments = {
            { .target_c = 120.0f, .ramp_c_per_hr = 170.0f, .dwell_min = 60 },
            { .target_c = 945.0f, .ramp_c_per_hr = 170.0f, .dwell_min = 0 },
            { .target_c = 1000.0f, .ramp_c_per_hr = 60.0f, .dwell_min = 15 },
            { .target_c = 30.0f, .ramp_c_per_hr = 0.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "BRTF05",
        .title = "Bartlett Fast Glaze Cone 05",
        .slug = "brtf05",
        .family = "Bartlett",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = -5,
        .segment_count = 2,
        .segments = {
            { .target_c = 913.0f, .ramp_c_per_hr = 316.0f, .dwell_min = 0 },
            { .target_c = 1052.0f, .ramp_c_per_hr = 111.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "BRTF6",
        .title = "Bartlett Fast Glaze Cone 6",
        .slug = "brtf6",
        .family = "Bartlett",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = 6,
        .segment_count = 2,
        .segments = {
            { .target_c = 1083.0f, .ramp_c_per_hr = 316.0f, .dwell_min = 0 },
            { .target_c = 1222.0f, .ramp_c_per_hr = 111.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "BRTS6",
        .title = "Bartlett Slow Glaze Cone 6",
        .slug = "brts6",
        .family = "Bartlett",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = 6,
        .segment_count = 3,
        .segments = {
            { .target_c = 121.0f, .ramp_c_per_hr = 83.0f, .dwell_min = 0 },
            { .target_c = 1083.0f, .ramp_c_per_hr = 222.0f, .dwell_min = 0 },
            { .target_c = 1222.0f, .ramp_c_per_hr = 66.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "BTFB04",
        .title = "Bartlett Fast Bisque Cone 04",
        .slug = "btfb04",
        .family = "Bartlett",
        .firing_type = PROFILE_FIRING_BISQUE,
        .cone = -4,
        .segment_count = 5,
        .segments = {
            { .target_c = 121.0f, .ramp_c_per_hr = 66.0f, .dwell_min = 0 },
            { .target_c = 537.0f, .ramp_c_per_hr = 166.0f, .dwell_min = 0 },
            { .target_c = 593.0f, .ramp_c_per_hr = 83.0f, .dwell_min = 0 },
            { .target_c = 923.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 0 },
            { .target_c = 1062.0f, .ramp_c_per_hr = 60.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "BTSB04",
        .title = "Bartlett Slow Bisque Cone 04",
        .slug = "btsb04",
        .family = "Bartlett",
        .firing_type = PROFILE_FIRING_BISQUE,
        .cone = -4,
        .segment_count = 5,
        .segments = {
            { .target_c = 121.0f, .ramp_c_per_hr = 44.0f, .dwell_min = 0 },
            { .target_c = 537.0f, .ramp_c_per_hr = 111.0f, .dwell_min = 0 },
            { .target_c = 593.0f, .ramp_c_per_hr = 55.0f, .dwell_min = 0 },
            { .target_c = 913.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 0 },
            { .target_c = 1052.0f, .ramp_c_per_hr = 44.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "BTSG05",
        .title = "Bartlett Slow Glaze Cone 05",
        .slug = "btsg05",
        .family = "Bartlett",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = -5,
        .segment_count = 3,
        .segments = {
            { .target_c = 121.0f, .ramp_c_per_hr = 66.0f, .dwell_min = 0 },
            { .target_c = 913.0f, .ramp_c_per_hr = 222.0f, .dwell_min = 0 },
            { .target_c = 1052.0f, .ramp_c_per_hr = 66.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "C04PLTP",
        .title = "Plainsman Low Temperature Drop-and-hold",
        .slug = "c04pltp",
        .family = "Plainsman",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = -4,
        .segment_count = 5,
        .segments = {
            { .target_c = 121.0f, .ramp_c_per_hr = 55.0f, .dwell_min = 60 },
            { .target_c = 1004.0f, .ramp_c_per_hr = 194.0f, .dwell_min = 0 },
            { .target_c = 1060.0f, .ramp_c_per_hr = 55.0f, .dwell_min = 10 },
            { .target_c = 1004.0f, .ramp_c_per_hr = 500.0f, .dwell_min = 30 },
            { .target_c = 23.0f, .ramp_c_per_hr = 0.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "C10RPL",
        .title = "Plainsman Cone 10R Firing",
        .slug = "c10rpl",
        .family = "Plainsman",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = 10,
        .segment_count = 6,
        .segments = {
            { .target_c = 120.0f, .ramp_c_per_hr = 10.0f, .dwell_min = 0 },
            { .target_c = 550.0f, .ramp_c_per_hr = 50.0f, .dwell_min = 0 },
            { .target_c = 980.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 0 },
            { .target_c = 1300.0f, .ramp_c_per_hr = 50.0f, .dwell_min = 0 },
            { .target_c = 1280.0f, .ramp_c_per_hr = 0.0f, .dwell_min = 30 },
            { .target_c = 30.0f, .ramp_c_per_hr = 0.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "C5DHSC",
        .title = "Plainsman Cone 5 Drop-and-Hold Slow-Cool",
        .slug = "c5dhsc",
        .family = "Plainsman",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = 5,
        .segment_count = 5,
        .segments = {
            { .target_c = 104.0f, .ramp_c_per_hr = 60.0f, .dwell_min = 60 },
            { .target_c = 1128.0f, .ramp_c_per_hr = 194.0f, .dwell_min = 10 },
            { .target_c = 1183.0f, .ramp_c_per_hr = 60.0f, .dwell_min = 10 },
            { .target_c = 1128.0f, .ramp_c_per_hr = 500.0f, .dwell_min = 30 },
            { .target_c = 760.0f, .ramp_c_per_hr = 83.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "C6DHSC",
        .title = "Plainsman Cone 6 Drop-and-hold, Slow Cool",
        .slug = "c6dhsc",
        .family = "Plainsman",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = 6,
        .segment_count = 5,
        .segments = {
            { .target_c = 121.0f, .ramp_c_per_hr = 60.0f, .dwell_min = 60 },
            { .target_c = 1148.0f, .ramp_c_per_hr = 194.0f, .dwell_min = 15 },
            { .target_c = 1204.0f, .ramp_c_per_hr = 60.0f, .dwell_min = 10 },
            { .target_c = 1148.0f, .ramp_c_per_hr = 500.0f, .dwell_min = 30 },
            { .target_c = 760.0f, .ramp_c_per_hr = 83.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "C6IRED",
        .title = "Cone 6 Iron Reds",
        .slug = "c6ired",
        .family = "General",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = 6,
        .segment_count = 5,
        .segments = {
            { .target_c = 121.0f, .ramp_c_per_hr = 138.0f, .dwell_min = 60 },
            { .target_c = 1148.0f, .ramp_c_per_hr = 194.0f, .dwell_min = 0 },
            { .target_c = 1204.0f, .ramp_c_per_hr = 60.0f, .dwell_min = 10 },
            { .target_c = 1148.0f, .ramp_c_per_hr = 555.0f, .dwell_min = 30 },
            { .target_c = 960.0f, .ramp_c_per_hr = 555.0f, .dwell_min = 50 },
        },
    },
    {
        .code = "C6MSGL1",
        .title = "Mastering Glazes Cone 6",
        .slug = "c6msgl1",
        .family = "General",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = 6,
        .segment_count = 5,
        .segments = {
            { .target_c = 105.0f, .ramp_c_per_hr = 55.0f, .dwell_min = 0 },
            { .target_c = 1080.0f, .ramp_c_per_hr = 200.0f, .dwell_min = 0 },
            { .target_c = 1200.0f, .ramp_c_per_hr = 85.0f, .dwell_min = 15 },
            { .target_c = 1000.0f, .ramp_c_per_hr = 275.0f, .dwell_min = 0 },
            { .target_c = 760.0f, .ramp_c_per_hr = 70.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "C6PLST",
        .title = "Plainsman Cone 6 Electric Standard",
        .slug = "c6plst",
        .family = "Plainsman",
        /* Verified 2026-09-04 against https://digitalfire.com/schedule/c6plst:
         * "We use this schedule to fire clay test bars. Pieces in the kiln
         * have not been bisque fired and are generally not glazed." Corrected
         * from Glaze to Other -- this is a raw test-bar firing, neither
         * bisque nor glaze. */
        .firing_type = PROFILE_FIRING_OTHER,
        .cone = 6,
        .segment_count = 3,
        .segments = {
            { .target_c = 121.0f, .ramp_c_per_hr = 55.0f, .dwell_min = 60 },
            { .target_c = 1148.0f, .ramp_c_per_hr = 194.0f, .dwell_min = 0 },
            { .target_c = 1204.0f, .ramp_c_per_hr = 60.0f, .dwell_min = 10 },
        },
    },
    {
        .code = "FSCG1",
        .title = "Shimbo Crystal Schedule 1",
        .slug = "fscg1",
        .family = "Crystalline",
        .firing_type = PROFILE_FIRING_GLAZE,
        /* UNRESOLVED 2026-09-04: https://digitalfire.com/schedule/fscg1 does
         * not state a cone number anywhere on the page (crystalline glaze
         * schedules are driven by peak temperature/hold, not a cone rating).
         * Value below is the unverified memory-derived estimate carried over
         * from the original pass -- treat as not authoritative. */
        .cone = 9,
        .segment_count = 5,
        .segments = {
            { .target_c = 250.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 0 },
            { .target_c = 500.0f, .ramp_c_per_hr = 200.0f, .dwell_min = 0 },
            { .target_c = 1270.0f, .ramp_c_per_hr = 500.0f, .dwell_min = 0 },
            { .target_c = 1100.0f, .ramp_c_per_hr = 999.0f, .dwell_min = 0 },
            { .target_c = 1075.0f, .ramp_c_per_hr = 10.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "FSCGB1",
        .title = "Shimbo Crystal Holding Pattern 2",
        .slug = "fscgb1",
        .family = "Crystalline",
        .firing_type = PROFILE_FIRING_GLAZE,
        /* UNRESOLVED 2026-09-04: https://digitalfire.com/schedule/fscgb1 does
         * not state a cone number (crystalline glaze, no stated cone rating).
         * Value below is the unverified memory-derived estimate carried over
         * from the original pass -- treat as not authoritative. */
        .cone = 8,
        .segment_count = 8,
        .segments = {
            { .target_c = 200.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 0 },
            { .target_c = 500.0f, .ramp_c_per_hr = 250.0f, .dwell_min = 0 },
            { .target_c = 1260.0f, .ramp_c_per_hr = 500.0f, .dwell_min = 0 },
            { .target_c = 1075.0f, .ramp_c_per_hr = 999.0f, .dwell_min = 90 },
            { .target_c = 1100.0f, .ramp_c_per_hr = 999.0f, .dwell_min = 15 },
            { .target_c = 1075.0f, .ramp_c_per_hr = 999.0f, .dwell_min = 60 },
            { .target_c = 1050.0f, .ramp_c_per_hr = 999.0f, .dwell_min = 15 },
            { .target_c = 1075.0f, .ramp_c_per_hr = 999.0f, .dwell_min = 60 },
        },
    },
    {
        .code = "FSCGCL",
        .title = "Shimbo Crystal Celestite Schedule",
        .slug = "fscgcl",
        .family = "Crystalline",
        .firing_type = PROFILE_FIRING_GLAZE,
        /* UNRESOLVED 2026-09-04: https://digitalfire.com/schedule/fscgcl does
         * not state a cone number (crystalline glaze, no stated cone rating).
         * Value below is the unverified memory-derived estimate carried over
         * from the original pass -- treat as not authoritative. */
        .cone = 8,
        .segment_count = 5,
        .segments = {
            { .target_c = 250.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 0 },
            { .target_c = 350.0f, .ramp_c_per_hr = 250.0f, .dwell_min = 0 },
            { .target_c = 1265.0f, .ramp_c_per_hr = 500.0f, .dwell_min = 0 },
            { .target_c = 1130.0f, .ramp_c_per_hr = 9999.0f, .dwell_min = 10 },
            { .target_c = 1050.0f, .ramp_c_per_hr = 35.0f, .dwell_min = 10 },
        },
    },
    {
        .code = "FSCGWM",
        .title = "Wollast-O-Matte Fara Shimbo Crystalline Glaze",
        .slug = "fscgwm",
        .family = "Crystalline",
        .firing_type = PROFILE_FIRING_GLAZE,
        /* UNRESOLVED 2026-09-04: https://digitalfire.com/schedule/fscgwm does
         * not state a cone number (crystalline glaze, no stated cone rating).
         * Value below is the unverified memory-derived estimate carried over
         * from the original pass -- treat as not authoritative. */
        .cone = 9,
        .segment_count = 5,
        .segments = {
            { .target_c = 200.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 0 },
            { .target_c = 500.0f, .ramp_c_per_hr = 250.0f, .dwell_min = 0 },
            { .target_c = 1275.0f, .ramp_c_per_hr = 500.0f, .dwell_min = 0 },
            { .target_c = 1150.0f, .ramp_c_per_hr = 9999.0f, .dwell_min = 0 },
            { .target_c = 1080.0f, .ramp_c_per_hr = 30.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "FSCRGL",
        .title = "GC106 Base for Crystalline Glazes",
        .slug = "fscrgl",
        .family = "Crystalline",
        .firing_type = PROFILE_FIRING_GLAZE,
        /* UNRESOLVED 2026-09-04: https://digitalfire.com/schedule/fscrgl
         * does not itself state a cone number; the page's *related-recipe*
         * link (GC106, a different page) says "Cone 8-10", which is not the
         * same as this schedule stating a cone. Value below is the
         * unverified memory-derived estimate carried over from the original
         * pass -- treat as not authoritative. */
        .cone = 9,
        .segment_count = 5,
        .segments = {
            { .target_c = 200.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 0 },
            { .target_c = 500.0f, .ramp_c_per_hr = 250.0f, .dwell_min = 0 },
            { .target_c = 1275.0f, .ramp_c_per_hr = 500.0f, .dwell_min = 0 },
            { .target_c = 1130.0f, .ramp_c_per_hr = 9999.0f, .dwell_min = 15 },
            { .target_c = 1075.0f, .ramp_c_per_hr = 30.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "FSHP1",
        .title = "Shimbo Crystal Holding Pattern 1",
        .slug = "fshp1",
        .family = "Crystalline",
        .firing_type = PROFILE_FIRING_GLAZE,
        /* UNRESOLVED 2026-09-04: https://digitalfire.com/schedule/fshp1 does
         * not state a cone number (crystalline glaze, no stated cone rating).
         * Value below is the unverified memory-derived estimate carried over
         * from the original pass -- treat as not authoritative. */
        .cone = 6,
        .segment_count = 8,
        .segments = {
            { .target_c = 250.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 0 },
            { .target_c = 500.0f, .ramp_c_per_hr = 250.0f, .dwell_min = 0 },
            { .target_c = 1221.0f, .ramp_c_per_hr = 500.0f, .dwell_min = 0 },
            { .target_c = 1100.0f, .ramp_c_per_hr = 999.0f, .dwell_min = 0 },
            { .target_c = 1000.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 0 },
            { .target_c = 1050.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 30 },
            { .target_c = 1020.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 30 },
            { .target_c = 100.0f, .ramp_c_per_hr = 0.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "FSHP3",
        .title = "Shimbo Crystal Holding Pattern 3",
        .slug = "fshp3",
        .family = "Crystalline",
        .firing_type = PROFILE_FIRING_GLAZE,
        /* UNRESOLVED 2026-09-04: https://digitalfire.com/schedule/fshp3 does
         * not state a cone number (crystalline glaze, no stated cone rating).
         * Value below is the unverified memory-derived estimate carried over
         * from the original pass -- treat as not authoritative. */
        .cone = 6,
        .segment_count = 8,
        .segments = {
            { .target_c = 250.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 0 },
            { .target_c = 500.0f, .ramp_c_per_hr = 250.0f, .dwell_min = 0 },
            { .target_c = 1221.0f, .ramp_c_per_hr = 500.0f, .dwell_min = 0 },
            { .target_c = 1075.0f, .ramp_c_per_hr = 999.0f, .dwell_min = 15 },
            { .target_c = 1100.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 15 },
            { .target_c = 1075.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 15 },
            { .target_c = 1050.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 15 },
            { .target_c = 1075.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 15 },
        },
    },
    {
        .code = "FSNM5",
        .title = "Fa's Number Five",
        .slug = "fsnm5",
        .family = "Crystalline",
        .firing_type = PROFILE_FIRING_GLAZE,
        /* UNRESOLVED 2026-09-04: https://digitalfire.com/schedule/fsnm5 does
         * not itself state a cone number or firing purpose; a *related*
         * recipe link (FAC5, a different page) says "Cone 8-9", which is not
         * the same as this schedule stating a cone. Value below is the
         * unverified memory-derived estimate carried over from the original
         * pass -- treat as not authoritative. */
        .cone = 8,
        .segment_count = 4,
        .segments = {
            { .target_c = 200.0f, .ramp_c_per_hr = 100.0f, .dwell_min = 0 },
            { .target_c = 500.0f, .ramp_c_per_hr = 250.0f, .dwell_min = 0 },
            { .target_c = 1270.0f, .ramp_c_per_hr = 500.0f, .dwell_min = 0 },
            { .target_c = 1100.0f, .ramp_c_per_hr = 9999.0f, .dwell_min = 120 },
        },
    },
    {
        .code = "MDDCL",
        .title = "Medalta Decal Firing",
        .slug = "mddcl",
        .family = "General",
        .firing_type = PROFILE_FIRING_OTHER,
        /* UNRESOLVED 2026-09-04: https://digitalfire.com/schedule/mddcl does
         * not state a cone number anywhere (it's a decal-firing schedule --
         * decals fire well below any bisque/glaze cone and are not rated by
         * cone at all). -15 is the unverified memory-derived estimate
         * carried over from the original pass -- treat as not authoritative.
         * firing_type = Other IS confirmed by the source ("Medalta Decal
         * Firing", explicitly neither bisque nor glaze). */
        .cone = -15,
        .segment_count = 4,
        .segments = {
            { .target_c = 43.0f, .ramp_c_per_hr = 111.0f, .dwell_min = 30 },
            { .target_c = 97.0f, .ramp_c_per_hr = 111.0f, .dwell_min = 45 },
            { .target_c = 482.0f, .ramp_c_per_hr = 194.0f, .dwell_min = 0 },
            { .target_c = 821.0f, .ramp_c_per_hr = 388.0f, .dwell_min = 20 },
        },
    },
    {
        .code = "PLC6CR",
        .title = "Cone 6 Crystal Glaze Plainsman",
        .slug = "plc6cr",
        .family = "Plainsman",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = 6,
        .segment_count = 5,
        .segments = {
            { .target_c = 372.0f, .ramp_c_per_hr = 176.0f, .dwell_min = 0 },
            { .target_c = 1093.0f, .ramp_c_per_hr = 398.0f, .dwell_min = 0 },
            { .target_c = 1210.0f, .ramp_c_per_hr = 65.0f, .dwell_min = 0 },
            { .target_c = 1093.0f, .ramp_c_per_hr = 398.0f, .dwell_min = 0 },
            { .target_c = 1037.0f, .ramp_c_per_hr = 398.0f, .dwell_min = 0 },
        },
    },
    {
        .code = "PLC6DS",
        .title = "Cone 6 Drop-and-Soak",
        .slug = "plc6ds",
        .family = "Plainsman",
        .firing_type = PROFILE_FIRING_GLAZE,
        .cone = 6,
        .segment_count = 4,
        .segments = {
            { .target_c = 121.0f, .ramp_c_per_hr = 60.0f, .dwell_min = 60 },
            { .target_c = 1148.0f, .ramp_c_per_hr = 194.0f, .dwell_min = 0 },
            { .target_c = 1204.0f, .ramp_c_per_hr = 60.0f, .dwell_min = 10 },
            { .target_c = 1148.0f, .ramp_c_per_hr = 500.0f, .dwell_min = 30 },
        },
    },
    {
        .code = "QICA",
        .title = "Quartz Inversion Cracking Avoider",
        .slug = "qica",
        .family = "General",
        .firing_type = PROFILE_FIRING_OTHER,
        /* UNRESOLVED 2026-09-04: https://digitalfire.com/schedule/qica does
         * not state a cone number -- it's explicitly "not a standard bisque
         * or glaze firing schedule tied to a specific cone" (quartz-
         * inversion crack avoidance is temperature-window-driven, not cone-
         * rated). 6 is the unverified memory-derived estimate carried over
         * from the original pass -- treat as not authoritative. firing_type
         * = Other IS confirmed by the source. */
        .cone = 6,
        .segment_count = 8,
        .segments = {
            { .target_c = 121.0f, .ramp_c_per_hr = 138.0f, .dwell_min = 60 },
            { .target_c = 518.0f, .ramp_c_per_hr = 111.0f, .dwell_min = 30 },
            { .target_c = 629.0f, .ramp_c_per_hr = 41.0f, .dwell_min = 30 },
            { .target_c = 1148.0f, .ramp_c_per_hr = 111.0f, .dwell_min = 0 },
            { .target_c = 1204.0f, .ramp_c_per_hr = 55.0f, .dwell_min = 15 },
            { .target_c = 629.0f, .ramp_c_per_hr = 83.0f, .dwell_min = 30 },
            { .target_c = 518.0f, .ramp_c_per_hr = 41.0f, .dwell_min = 30 },
            { .target_c = 121.0f, .ramp_c_per_hr = 83.0f, .dwell_min = 0 },
        },
    },
};

const size_t g_builtin_profile_count =
    sizeof(g_builtin_profiles) / sizeof(g_builtin_profiles[0]);
