// Golden for tools/PcTools/src/kilnctrl/config_convert.py's zones_blob store.
//
// config_convert.py hand-mirrors zones_cfg_t's byte layout as Python
// struct format strings. A total-size check (896) cannot catch a field
// reorder, a float<->int32 type change, or a padding shift that keeps the
// total size, and any of those would make the Python decoder read garbage.
// This test closes that gap from the FIRMWARE side:
//
//   1. Fill every field of a zones_cfg_t with a distinct per-field sentinel,
//      assigned through the field's REAL C type (so a float->int32 change
//      truncates the value and changes the bytes).
//   2. Save it through the REAL nvs_save() (zones_config_store.c, #included
//      by test_zones_http.c in this same executable) -- the actual write path,
//      including its zones_config_json_compute_crc() stamp -- and read the
//      exact bytes back out of the fake NVS.
//   3. Emit a text golden: the blob as hex, plus one line per field giving
//      its path, byte offset, size and intended value.
//   4. Compare against the committed golden,
//      tools/PcTools/tests/fixtures/config_convert/zones_cfg_golden.txt.
//
// tools/PcTools/tests/test_config_convert_zones_golden.py decodes that same
// committed file with config_convert.py and checks every field value, so
// a layout change on either side goes red: a C-side change fails this test
// (the regenerated text differs from the committed file); a Python-side
// change fails the pytest (decoded values differ from the field lines).
//
// Regenerate after an intentional zones_cfg_t change by running the zones
// host-test executable with KILNCTL_REGEN_CONFIG_CONVERT_GOLDENS=1 set, then
// update config_convert.py until the pytest passes again.
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

#ifdef _MSC_VER
/* ZBG_U assigns an unsigned long sentinel through each field's real type on
 * purpose (every sentinel fits its field); C4244 on that is expected. */
#pragma warning(disable : 4244)
#endif

#include "esp_err.h"
#include "esp_crc.h"
#include "fake_kv.h"
#include "hal_kv.h"

#ifdef _WIN32
#include <direct.h>
#define ZBG_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define ZBG_MKDIR(p) mkdir((p), 0755)
#endif
#include "cfg_fs.h"
#include "zones_config_cfg_fs.h"
#include "zones_http_internal.h" /* s_zones, nvs_save(), NVS_NAMESPACE/NVS_KEY_ZONES */

#define ZBG_GOLDEN_REL "../../../../tools/PcTools/tests/fixtures/config_convert/zones_cfg_golden.txt"
#define ZBG_REGEN_ENV "KILNCTL_REGEN_CONFIG_CONVERT_GOLDENS"

/* Padding bytes natural C alignment puts in zones_cfg_t today: 2 after the
 * 6-byte header, 11 per zone_cfg_t (3+3+3+2), 3 after timing_profile_count.
 * A field added to the struct but not to the fill below would show up as
 * extra "padding" here, so this is also the "every field was set" check. */
#define ZBG_EXPECTED_PAD_BYTES (2u + 11u * MAX31856_CHANNEL_COUNT + 3u)

static char s_text[64 * 1024];
static size_t s_text_len;
static uint8_t s_covered[sizeof(zones_cfg_t)];
static int s_f_next;
static unsigned s_u8_next;
static unsigned s_u16_next;

static void zbg_emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(s_text + s_text_len, sizeof(s_text) - s_text_len, fmt, ap);
    va_end(ap);
    if (w > 0 && (size_t)w < sizeof(s_text) - s_text_len) {
        s_text_len += (size_t)w;
    }
}

static size_t zbg_mark(const zones_cfg_t *c, const void *field, size_t size)
{
    size_t off = (size_t)((const uint8_t *)field - (const uint8_t *)c);
    for (size_t i = 0; i < size && off + i < sizeof(s_covered); i++) {
        if (s_covered[off + i]) {
            TEST_CHECK(false, "zones blob golden: a field was filled twice (overlapping byte range)");
        }
        s_covered[off + i] = 1;
    }
    return off;
}

/* Exact in binary32, never an integer, alternating sign: reinterpreting one
 * of these as any integer type (or vice versa) cannot round-trip. */
static float zbg_next_f(void)
{
    s_f_next++;
    float v = (float)(s_f_next * 8 + 1) + 0.25f;
    return (s_f_next & 1) ? -v : v;
}

static unsigned zbg_next_u8(void)
{
    return s_u8_next++;
}

static unsigned zbg_next_u16(void)
{
    unsigned v = s_u16_next;
    s_u16_next += 0x0101u;
    return v;
}

static void zbg_line(const char *kind, const char *prefix, int idx, const char *name, size_t off, size_t size)
{
    if (prefix) {
        zbg_emit("%s %s[%d].%s %u %u ", kind, prefix, idx, name, (unsigned)off, (unsigned)size);
    } else {
        zbg_emit("%s %s %u %u ", kind, name, (unsigned)off, (unsigned)size);
    }
}

/* Every assignment goes through the field's real declared type. */
#define ZBG_F(pfx, idx, lval, name)                                                                   \
    do {                                                                                               \
        float v_ = zbg_next_f();                                                                       \
        (lval) = v_;                                                                                   \
        zbg_line("f", pfx, idx, name, zbg_mark(c, &(lval), sizeof(lval)), sizeof(lval));               \
        zbg_emit("%.2f\n", (double)v_);                                                                \
    } while (0)
#define ZBG_U(pfx, idx, lval, name, value)                                                            \
    do {                                                                                               \
        unsigned long v_ = (unsigned long)(value);                                                     \
        (lval) = v_;                                                                                   \
        zbg_line("u", pfx, idx, name, zbg_mark(c, &(lval), sizeof(lval)), sizeof(lval));               \
        zbg_emit("%lu\n", v_);                                                                         \
    } while (0)
#define ZBG_S(pfx, idx, arr, name, text)                                                              \
    do {                                                                                               \
        memset((arr), 0, sizeof(arr));                                                                 \
        memcpy((arr), (text), strlen(text));                                                           \
        zbg_line("s", pfx, idx, name, zbg_mark(c, (arr), sizeof(arr)), sizeof(arr));                   \
        zbg_emit("\"%s\"\n", (text));                                                                  \
    } while (0)

#define ZF(m) ZBG_F("zones", zi, z->m, #m)
#define ZB(m) ZBG_U("zones", zi, z->m, #m, zbg_next_u8())
#define ZH(m) ZBG_U("zones", zi, z->m, #m, zbg_next_u16())

static void zbg_fill_zone(zones_cfg_t *c, int zi)
{
    static const char *const names[MAX31856_CHANNEL_COUNT] = {"ABCDEFGHIJKLMNO", "z1", "Zone_2-x"};
    zone_cfg_t *z = &c->zones[zi];
    ZBG_S("zones", zi, z->name, "name", names[zi]);
    ZF(cal_offset_c);
    ZF(pid_kp);
    ZF(pid_ki);
    ZF(pid_kd);
    ZF(max_ramp_c_per_hr);
    ZF(sanity_rate_c_per_min);
    ZF(max_temp_c);
    ZF(min_temp_c);
    ZF(heater_window_ms);
    ZF(heater_min_on_ms);
    ZF(heater_min_off_ms);
    ZF(guard_wrong_dir_window_s);
    ZF(guard_wrong_dir_rate_c_per_min);
    ZF(guard_off_settle_s);
    ZF(guard_runaway_rate_c_per_min);
    ZF(guard_runaway_margin_c);
    ZF(guard_drift_period_s);
    ZF(guard_sensor_fault_debounce_ticks);
    ZF(guard_frozen_window_s);
    ZF(cross_zone_max_delta_c);
    ZF(model_k_dc);
    ZF(model_tau_s);
    ZF(model_dead_time_s);
    ZF(fuzzy_strength_pct);
    ZF(coupling_coeff[0]);
    ZF(coupling_coeff[1]);
    ZF(coupling_coeff[2]);
    ZF(coupling_tau_s[0]);
    ZF(coupling_tau_s[1]);
    ZF(coupling_tau_s[2]);
    ZF(coupling_dead_time_s[0]);
    ZF(coupling_dead_time_s[1]);
    ZF(coupling_dead_time_s[2]);
    ZB(relay_mask);
    ZB(control_mode);
    ZB(tc_type);
    ZB(thermo_mask);
    ZB(ct_mask);
    ZB(timing_profile);
    ZB(settings_source[0]);
    ZB(settings_source[1]);
    ZB(settings_source[2]);
    ZB(settings_source[3]);
    ZB(settings_source[4]);
    ZB(tuning_valid);
    ZB(tuning_method);
    ZB(tuning_rule);
    ZB(tuning_settled);
    ZB(tuning_extrapolation_converged);
    ZB(tuning_tau_consistent);
    ZF(tuning_baseline_c);
    ZF(tuning_step_ambient_c);
    ZF(tuning_raw_rise_c);
    ZF(tuning_rise_inf_c);
    ZBG_U("zones", zi, z->tuning_seq, "tuning_seq", 0xA1B2C3D0ul + (unsigned long)zi);
    ZB(adaptive_tune_enabled);
    ZF(coupling_diag_k_dc);
    ZF(ease_off_window_mult);
    ZF(approach_rate_cap_c_per_hr);
    ZF(error_band_c);
    ZF(rate_band_c_per_s);
    ZB(relay_type);
    ZF(progress_band_c);
    ZB(zone_type);
    ZB(failsafe_state);
    ZF(hyst_c);
    ZH(min_on_s);
    ZH(min_off_s);
    ZF(model_fit_temp_c);
    ZF(model_fit_ambient_c);
    ZF(coil_power_w);
    ZF(autotune_baseline_k_dc);
}

#define TF(m) ZBG_F("timing_profiles", ti, t->m, #m)

static void zbg_fill_timing_profile(zones_cfg_t *c, int ti)
{
    static const char *const names[MAX31856_CHANNEL_COUNT] = {"TPabcde", "t1", ""};
    zone_timing_profile_t *t = &c->timing_profiles[ti];
    ZBG_S("timing_profiles", ti, t->name, "name", names[ti]);
    TF(guard_progress_duty_min);
    TF(guard_progress_window_s);
    TF(guard_drift_hysteresis_c);
    TF(guard_frozen_eps_c);
    TF(guard_cross_zone_period_s);
    TF(bangbang_hysteresis_c);
    TF(cooling_limited_margin_c);
    TF(cooling_limited_hold_s);
    TF(ramp_lock_band_c);
}

#define HB(m, value) ZBG_U(NULL, 0, c->m, #m, (value))

static void zbg_fill(zones_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    memset(s_covered, 0, sizeof(s_covered));
    s_f_next = 0;
    s_u8_next = 0x11u;
    s_u16_next = 0x1234u;

    HB(version, ZONES_CFG_VERSION); /* nvs_save() stamps this anyway */
    HB(thermo_count, zbg_next_u8());
    HB(relay_count, zbg_next_u8());
    HB(max_simultaneous_relays, zbg_next_u8());
    HB(continue_on_zone_trip, zbg_next_u8());
    HB(safety_tc_type, zbg_next_u8());
    for (int zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
        zbg_fill_zone(c, zi);
    }
    HB(timing_profile_count, zbg_next_u8());
    for (int ti = 0; ti < MAX31856_CHANNEL_COUNT; ti++) {
        zbg_fill_timing_profile(c, ti);
    }
    ZBG_F(NULL, 0, c->pc_link_abort_silence_ms, "pc_link_abort_silence_ms");
    (void)zbg_mark(c, &c->crc32, sizeof(c->crc32)); /* stamped by nvs_save(), reported separately */
}

/* Compare ignoring '\r' so a CRLF checkout (core.autocrlf=true) still matches. */
static int zbg_text_equal_ignoring_cr(const char *a, const char *b, int *first_diff_line)
{
    int line = 1;
    while (*a || *b) {
        if (*a == '\r') {
            a++;
            continue;
        }
        if (*b == '\r') {
            b++;
            continue;
        }
        if (*a != *b) {
            *first_diff_line = line;
            return 0;
        }
        if (*a == '\n') {
            line++;
        }
        a++;
        b++;
    }
    return 1;
}

static void test_zones_blob_golden_matches_firmware_layout(void)
{
    TEST_SECTION("zones_cfg_t golden for config_convert.py -- real nvs_save() bytes, every field distinct");

    cfg_fs_deinit();
    zones_config_load_fault_reset_for_test(); /* clears the cannot-decide save gate an earlier test in this exe may leave */
    zones_config_cfg_fs_reset_write_fn_for_test();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    /* Saves go to the cfg file only (NVS dual-write closed): mount a scratch
     * directory and start from no zones file. */
    (void)ZBG_MKDIR("cfg_fs_test_zones_golden");
    TEST_CHECK(cfg_fs_init("cfg_fs_test_zones_golden", NULL) == ESP_OK, "zones blob golden: cfg_fs mounts");
    (void)cfg_fs_delete(ZONES_CFG_FILE_PATH);

    s_text_len = 0;
    s_text[0] = '\0';
    zones_cfg_t *c = &s_zones.cfg;
    zbg_emit("# zones_cfg_t golden -- GENERATED by firmware/KilnFW/App/test/test_zones_blob_golden.c.\n");
    zbg_emit("# Do not edit by hand; regenerate with %s=1 (see that file's header).\n", ZBG_REGEN_ENV);
    zbg_emit("# Field lines: <f|u|s> <path> <offset> <size> <value>\n");
    zbg_fill(c);

    size_t uncovered = 0;
    for (size_t i = 0; i < sizeof(s_covered); i++) {
        uncovered += s_covered[i] ? 0u : 1u;
    }
    TEST_CHECK(uncovered == ZBG_EXPECTED_PAD_BYTES,
               "zones blob golden: every zones_cfg_t field is filled (only alignment padding left over) -- "
               "a new field needs a line in zbg_fill_zone()/zbg_fill()");

    TEST_CHECK(nvs_save() == ESP_OK, "zones blob golden: real nvs_save() succeeds");

    uint8_t blob[sizeof(zones_cfg_t)];
    static uint8_t filebuf[4 + sizeof(zones_cfg_t)];
    size_t len = 0;
    bool read_ok = cfg_fs_read(ZONES_CFG_FILE_PATH, filebuf, sizeof(filebuf), &len) == ESP_OK;
    if (read_ok && len >= 4) {
        len -= 4; /* 4-byte LE rev prefix */
        memcpy(blob, filebuf + 4, len < sizeof(blob) ? len : sizeof(blob));
    } else {
        read_ok = false;
    }
    TEST_CHECK(read_ok && len == sizeof(zones_cfg_t), "zones blob golden: blob reads back from the cfg file at full size");
    if (!read_ok || len != sizeof(zones_cfg_t)) {
        return;
    }

    for (size_t i = 0; i < sizeof(s_covered); i++) {
        if (!s_covered[i] && blob[i] != 0) {
            TEST_CHECK(false, "zones blob golden: a padding byte is non-zero in the saved blob");
            break;
        }
    }

    /* Not zones_config_json_decode_blob(): its trailing
     * zones_config_json_validate() rightly refuses these out-of-range
     * sentinels, and raise_heater_timing_to_floors() rewrites fields. What
     * the golden needs is narrower: nvs_save() writes the struct's own bytes
     * verbatim (CRC stamped in place), which this pins directly. The CRC
     * itself is checked independently on the Python side (zlib.crc32 over
     * the blob with the crc32 field zeroed). */
    TEST_CHECK(memcmp(blob, &s_zones.cfg, sizeof(zones_cfg_t)) == 0,
               "zones blob golden: nvs_save() wrote the in-RAM struct byte for byte");
    TEST_CHECK(s_zones.cfg.crc32 == zones_config_json_compute_crc(&s_zones.cfg),
               "zones blob golden: nvs_save() stamped the firmware CRC");

    /* Pin the host stub of esp_crc32_le() (test/stubs/esp_crc.h, a C bit-loop, NOT the on-target ROM
     * routine) to the standard CRC-32/ISO-HDLC check value, so the Python-side
     * test_config_convert_zones_golden.py CRC assertion rests on a CRC the firmware code path
     * demonstrably computes as plain CRC-32. */
    {
        static const uint8_t check[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        TEST_CHECK(esp_crc32_le(0, check, 9) == 0xCBF43926u,
                   "zones blob golden: esp_crc32_le(\"123456789\") is the standard CRC-32 check value");
    }

    zbg_emit("version %u\n", (unsigned)ZONES_CFG_VERSION);
    zbg_emit("sizeof zones_cfg_t %u\n", (unsigned)sizeof(zones_cfg_t));
    zbg_emit("sizeof zone_cfg_t %u\n", (unsigned)sizeof(zone_cfg_t));
    zbg_emit("sizeof zone_timing_profile_t %u\n", (unsigned)sizeof(zone_timing_profile_t));
    zbg_emit("pad_bytes %u\n", (unsigned)uncovered);
    zbg_emit("crc32 %u 0x%08lx\n", (unsigned)offsetof(zones_cfg_t, crc32), (unsigned long)s_zones.cfg.crc32);
    zbg_emit("blob ");
    for (size_t i = 0; i < len; i++) {
        zbg_emit("%02x", blob[i]);
    }
    zbg_emit("\n");

    char *golden_path = test_resolve_from_here(__FILE__, ZBG_GOLDEN_REL);
    TEST_CHECK(golden_path != NULL, "zones blob golden: resolve golden path");
    if (!golden_path) {
        return;
    }
    const char *regen = getenv(ZBG_REGEN_ENV);
    if (regen && regen[0] == '1') {
        FILE *f = fopen(golden_path, "wb");
        TEST_CHECK(f != NULL, "zones blob golden: open golden for regeneration");
        if (f) {
            fwrite(s_text, 1, s_text_len, f);
            fclose(f);
            printf("  regenerated %s\n", golden_path);
        }
    }
    char *committed = test_read_whole_file(golden_path);
    if (!committed) {
        printf("  golden not found at %s\n", golden_path);
        TEST_CHECK(false, "zones blob golden: committed golden file exists");
    } else {
        int diff_line = 0;
        int same = zbg_text_equal_ignoring_cr(s_text, committed, &diff_line);
        if (!same) {
            printf("  zones_cfg_t golden differs from the committed file at line %d: %s\n"
                   "  zones_cfg_t's layout (or this test's fill) changed. If intentional, regenerate with\n"
                   "  %s=1 and update tools/PcTools/src/kilnctrl/config_convert.py to match.\n",
                   diff_line, golden_path, ZBG_REGEN_ENV);
        }
        TEST_CHECK(same, "zones blob golden: firmware-generated golden matches the committed file");
        free(committed);
    }
    free(golden_path);

    fake_kv_reset_all();
    memset(&s_zones.cfg, 0, sizeof(s_zones.cfg));
}

void run_test_zones_blob_golden(void)
{
    test_zones_blob_golden_matches_firmware_layout();
}
