// Host test for App/drivers/hw/board_temps.c's board_temps_get(), added
// 2026-08-27 for the array-position-vs-channel-number indexing defect found
// by the sensor-correctness audit (TODO.md): MAX31856_read_all() fills
// readings[0..out_count) for *initialized* channels only, packed by array
// POSITION, not by channel number (MAX31856.h's own doc comment on that
// function) -- so with channel 0 dead, readings[0] holds channel 1's
// reading, readings[1] holds channel 2's, and so on.
//
// board_temps_get()'s ORIGINAL code indexed thermo_cj_valid[]/thermo_cj_c[]
// by the loop position `i`, which -- with a dead channel anywhere but the
// last slot -- mislabeled every channel above the first dead one AND never
// reported the dead channel itself as absent (it just silently vanished
// instead of showing up missing). The fix indexes by
// MAX31856Reading::channel instead. A test that only ever exercises an
// identity mapping (channel i always at readings[i]) cannot tell these two
// implementations apart, which is why the case below deliberately puts a
// NON-ZERO channel first: channel 0 dead, channels 1 and 2 alive, so the
// buggy code and the fixed code disagree on every checked field.
//
// This is its own SEPARATE host-test executable (own main(), not merged
// into test_main.c/kilnctl_host_tests.exe), same convention as
// test_zones_http.c/test_backup_import.c/test_kiln_cfg_store.c: it
// #includes board_temps.c directly to reach board_temps_get(), the only
// pure-logic half of that file (the other half -- the HTTP handlers, the
// gzip-embedded page -- has no seam worth testing on the host and would
// drag in a full esp_http_server/wifi_provision_http stub surface for no
// benefit).
//
// 2026-09-06 migration: board_temps.c now reaches the ESP32-S3 on-die
// sensor through hal_sysinfo_temp_*() (firmware/hwAbstraction/interface/
// hal_sysinfo.h) instead of driver/temperature_sensor.h directly, so this
// executable links the real host fake backend (host/fake_sysinfo.c) --
// build_host_tests.ps1's exe10 -- instead of defining temperature_sensor_*()
// stub bodies here. Kept as its own executable regardless: linking
// fake_sysinfo.c's hal_sysinfo_* symbols alongside another translation unit
// that defines its own fakes of the same names (none do today, but
// test_partition_info_http.c already claims fake_sysinfo.c for itself the
// same way, per that executable's own comment) would collide.
#include <math.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "fake_sysinfo.h"

// asm("_binary_...") is a GCC/binutils extension (EMBED_TXTFILES,
// CMakeLists.txt) with no MSVC equivalent -- #define it away to nothing so
// `extern const uint8_t X[] asm("...");` parses as plain
// `extern const uint8_t X[];`. Real (empty) definitions follow the include,
// same convention the scratch harness that proved this fix used.
#define asm(x)

#include "../drivers/hw/board_temps.c"

#undef asm

// ---- link-time stub bodies for board_temps.c's non-pure half --------------
// 2026-09-05 (HW_ABSTRACTION_PLAN.md "drivers/ layering" item 3):
// board_temps.c's httpd handler/registration moved out to
// board_temps_http.c, so board_temps.c no longer includes
// esp_http_server.h/wifi_provision_http.h at all -- the httpd_*/
// wifi_provision_http_get_server()/web_*() stub bodies that used to live
// here for the linker are gone along with that dependency, not just unused.

// board_temps_get_live() calls this, but no test here calls
// board_temps_get_live() -- only board_temps_get() directly, with a
// caller-supplied readings[] array, same as the scratch harness that
// originally proved this fix. Stubbed only so the file links.
esp_err_t MAX31856_read_all(MAX31856BusClass *bus, MAX31856Reading *out, size_t max_readings,
                            size_t *out_count)
{
    (void)bus; (void)out; (void)max_readings;
    if (out_count) {
        *out_count = 0;
    }
    return ESP_FAIL;
}

static void test_dead_first_channel_indexes_by_channel_number(void)
{
    // THE regression case: channel 0 is dead (never initialized), so
    // MAX31856_read_all() would have compacted its result down to two
    // entries -- readings[0] holding channel 1's data, readings[1] holding
    // channel 2's -- with channel 0 simply absent from the array, not
    // represented by a failed entry at position 0. This is deliberately NOT
    // an identity mapping (readings[i].channel != i): the old
    // array-position-indexed code and the fixed channel-indexed code only
    // disagree when the dead channel isn't the last one.
    MAX31856Reading readings[2];
    memset(readings, 0, sizeof(readings));
    readings[0].channel = 1;
    readings[0].tc_temperature_c = 111.0f;
    readings[0].cj_temperature_c = 21.0f;
    readings[0].spi_failed = false;
    readings[1].channel = 2;
    readings[1].tc_temperature_c = 222.0f;
    readings[1].cj_temperature_c = 22.0f;
    readings[1].spi_failed = false;

    board_temps_t out;
    memset(&out, 0xAA, sizeof(out));
    esp_err_t err = board_temps_get(&out, readings, 2);
    TEST_CHECK(err == ESP_OK, "board_temps_get() returns ESP_OK");

    TEST_CHECK(out.thermo_count == MAX31856_CHANNEL_COUNT,
               "thermo_count is the full channel count, not the readings[] length");

    // Dead channel 0 must show up as ABSENT, not silently vanish.
    TEST_CHECK(out.thermo_cj_valid[0] == false,
               "channel 0 (dead, never in readings[]) reports invalid at ITS OWN index");

    // Channel 1's reading must land at index 1, not index 0 (the old
    // array-position bug would have put it at thermo_cj_valid[0]/
    // thermo_cj_c[0] instead, since it was readings[0]).
    TEST_CHECK(out.thermo_cj_valid[1] == true, "channel 1's reading reports valid at index 1");
    TEST_CHECK_NEAR(out.thermo_cj_c[1], 21.0, 0.001,
                     "channel 1's cold-junction value (21.0) lands at thermo_cj_c[1], not thermo_cj_c[0]");

    // Channel 2's reading must land at index 2, not index 1.
    TEST_CHECK(out.thermo_cj_valid[2] == true, "channel 2's reading reports valid at index 2");
    TEST_CHECK_NEAR(out.thermo_cj_c[2], 22.0, 0.001,
                     "channel 2's cold-junction value (22.0) lands at thermo_cj_c[2], not thermo_cj_c[1]");
}

static void test_all_channels_alive_identity_case_still_correct(void)
{
    // Sanity companion to the regression case above: when nothing is dead,
    // readings[i].channel == i for every i, so a buggy array-position
    // implementation would ALSO pass this one -- it proves nothing about the
    // bug by itself, but does confirm the fix didn't break the common case.
    MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
    memset(readings, 0, sizeof(readings));
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        readings[i].channel = i;
        readings[i].tc_temperature_c = 100.0f + (float)i;
        readings[i].cj_temperature_c = 20.0f + (float)i;
        readings[i].spi_failed = false;
    }

    board_temps_t out;
    memset(&out, 0xAA, sizeof(out));
    esp_err_t err = board_temps_get(&out, readings, MAX31856_CHANNEL_COUNT);
    TEST_CHECK(err == ESP_OK, "board_temps_get() returns ESP_OK (all channels alive)");
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        char msg[96];
        snprintf(msg, sizeof(msg), "channel %u valid at its own index (all-alive case)", i);
        TEST_CHECK(out.thermo_cj_valid[i] == true, msg);
        snprintf(msg, sizeof(msg), "channel %u cold-junction value at its own index (all-alive case)", i);
        TEST_CHECK_NEAR(out.thermo_cj_c[i], 20.0 + (double)i, 0.001, msg);
    }
}

static void test_spi_failed_channel_reports_invalid_at_its_own_index(void)
{
    // A channel that IS in readings[] but whose transfer failed must report
    // invalid at its own channel index too, not just a dead (absent)
    // channel -- board_temps_get()'s validity test checks spi_failed and
    // isnan(cj_temperature_c) together, same convention dashboard_http.c
    // applies to tc_temperature_c.
    MAX31856Reading readings[2];
    memset(readings, 0, sizeof(readings));
    readings[0].channel = 0;
    readings[0].tc_temperature_c = 50.0f;
    readings[0].cj_temperature_c = 19.0f;
    readings[0].spi_failed = false;
    readings[1].channel = 2;
    readings[1].tc_temperature_c = NAN;
    readings[1].cj_temperature_c = NAN;
    readings[1].spi_failed = true;

    board_temps_t out;
    memset(&out, 0xAA, sizeof(out));
    esp_err_t err = board_temps_get(&out, readings, 2);
    TEST_CHECK(err == ESP_OK, "board_temps_get() returns ESP_OK (one channel spi_failed)");

    TEST_CHECK(out.thermo_cj_valid[0] == true, "channel 0's good reading reports valid at index 0");
    TEST_CHECK_NEAR(out.thermo_cj_c[0], 19.0, 0.001, "channel 0's cold-junction value lands at index 0");

    // Channel 1 was never in readings[] at all (bus never answered it this
    // poll) -- absent, same as the dead-channel case above.
    TEST_CHECK(out.thermo_cj_valid[1] == false, "channel 1 (absent from readings[]) reports invalid");

    // Channel 2 WAS in readings[], but spi_failed -- must still be invalid,
    // at its own index (2), not index 1 (its array position).
    TEST_CHECK(out.thermo_cj_valid[2] == false, "channel 2 (spi_failed) reports invalid at its own index");
    TEST_CHECK(out.thermo_cj_c[2] == 0.0f, "channel 2's cold-junction value reports 0.0, not the NaN reading");
}

static void test_null_readings_reports_all_absent(void)
{
    // readings/count NULL/0 (no thermo_bus this boot, board_temps.h's own
    // doc comment) -- thermo_count comes back 0 and every thermo_cj_valid[]
    // entry stays at its memset-zero default (false), not left as whatever
    // 0xAA garbage the caller's struct started with.
    board_temps_t out;
    memset(&out, 0xAA, sizeof(out));
    esp_err_t err = board_temps_get(&out, NULL, 0);
    TEST_CHECK(err == ESP_OK, "board_temps_get() returns ESP_OK with readings=NULL");
    TEST_CHECK(out.thermo_count == 0, "thermo_count is 0 when readings/count are NULL/0");
    for (uint8_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "channel %u invalid when readings is NULL", i);
        TEST_CHECK(out.thermo_cj_valid[i] == false, msg);
    }
}

// ---- hal_sysinfo migration coverage (2026-09-06) --------------------------
// board_temps_start()/board_temps_get() now go through hal_sysinfo_temp_*()
// instead of owning a driver/temperature_sensor.h handle directly -- these
// cases exercise that seam via fake_sysinfo.c, the same host fake
// test_fake_sysinfo.c (firmware/hwAbstraction/test) already proves against
// hal_sysinfo.h's own contract.

static void test_start_not_called_leaves_esp32_invalid(void)
{
    // Never calling board_temps_start() must leave hal_sysinfo's temperature
    // lifecycle uninitialized, so board_temps_get() takes its esp32_valid =
    // false path -- same "safe to call any time" contract board_temps.h
    // promises.
    fake_sysinfo_reset_all();

    board_temps_t out;
    memset(&out, 0xAA, sizeof(out));
    esp_err_t err = board_temps_get(&out, NULL, 0);
    TEST_CHECK(err == ESP_OK, "board_temps_get() returns ESP_OK with hal_sysinfo temp uninitialized");
    TEST_CHECK(out.esp32_valid == false, "esp32_valid is false when board_temps_start() was never called");
}

static void test_start_success_flows_into_get(void)
{
    // board_temps_start() -> hal_sysinfo_temp_init() -> board_temps_get()
    // reads the value fake_sysinfo_set_temp_celsius() armed, proving the
    // migrated call actually reaches hal_sysinfo rather than reading some
    // leftover local state.
    fake_sysinfo_reset_all();
    fake_sysinfo_set_temp_celsius(57.25f);

    esp_err_t start_err = board_temps_start();
    TEST_CHECK(start_err == ESP_OK, "board_temps_start() succeeds when hal_sysinfo_temp_init() succeeds");

    board_temps_t out;
    memset(&out, 0xAA, sizeof(out));
    esp_err_t err = board_temps_get(&out, NULL, 0);
    TEST_CHECK(err == ESP_OK, "board_temps_get() returns ESP_OK after a successful start");
    TEST_CHECK(out.esp32_valid == true, "esp32_valid is true after board_temps_start() succeeded");
    TEST_CHECK_NEAR(out.esp32_c, 57.25, 0.001, "esp32_c reads the value fake_sysinfo_set_temp_celsius() armed");
}

static void test_start_failure_propagates_and_get_stays_invalid(void)
{
    // A hal_sysinfo_temp_init() failure (e.g. the real install/enable
    // sequence failing on real hardware) must make board_temps_start() fail
    // too, and must leave board_temps_get()'s esp32_valid false rather than
    // reporting a stale/garbage reading.
    fake_sysinfo_reset_all();
    fake_sysinfo_script_temp_init_status(HAL_IO);

    esp_err_t start_err = board_temps_start();
    TEST_CHECK(start_err != ESP_OK, "board_temps_start() fails when hal_sysinfo_temp_init() fails");

    board_temps_t out;
    memset(&out, 0xAA, sizeof(out));
    esp_err_t err = board_temps_get(&out, NULL, 0);
    TEST_CHECK(err == ESP_OK, "board_temps_get() still returns ESP_OK (esp32 failure is non-fatal to the rest of the call)");
    TEST_CHECK(out.esp32_valid == false, "esp32_valid stays false after a failed board_temps_start()");
}

int main(void)
{
    TEST_SECTION("board_temps");

    test_dead_first_channel_indexes_by_channel_number();
    test_all_channels_alive_identity_case_still_correct();
    test_spi_failed_channel_reports_invalid_at_its_own_index();
    test_null_readings_reports_all_absent();
    test_start_not_called_leaves_esp32_invalid();
    test_start_success_flows_into_get();
    test_start_failure_propagates_and_get_stays_invalid();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
