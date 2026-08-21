/* GENERATED FILE -- do not hand-edit.
 *
 * Regenerate with:
 *   python tools/scripts/gen_builtin_profiles.py SCRAPED.json \
 *       firmware/KilnFW/App/drivers/profiles_builtin_table.inc
 *
 * This file holds ONLY the catalogue table. It is #included by
 * profiles_builtin.c, which is hand-written and holds the API implementation
 * -- regenerating this file therefore cannot wipe hand-written code, which is
 * exactly why the split exists.
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
