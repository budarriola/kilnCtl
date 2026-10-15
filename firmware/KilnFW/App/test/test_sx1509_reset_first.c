// AUDIT_BOOT_SEQUENCE MED-2: SX1509_start() must pulse the hardware ~RESET GPIO
// (low then high) BEFORE its first I2C access, including when the probe misses
// and when init later fails, so relays a warm restart left latched ON drop even
// on a boot where the expander never answers.
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "test_common.h"
#include "fake_time.h"
#define SX1509_OWNER_BUILD
#include "SX1509_internal.h"
#include "settings.h"

int g_test_failures = 0;
int g_test_count = 0;

#define MAXEV 64
static char g_ev[MAXEV];  /* 'L' ~RESET low, 'H' ~RESET high, 'P' probe, 'I' other I2C */
static int g_nev;
static esp_err_t g_probe_result;
static esp_err_t g_owner_init_result;

static void ev(char c) { if (g_nev < MAXEV) g_ev[g_nev++] = c; }
static void gpio_hook(int gpio, int level)
{
    if (gpio == SX1509_RESET_IO) ev(level ? 'H' : 'L');
}

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b, const i2c_device_config_t *c, i2c_master_dev_handle_t *o)
{ (void)b; (void)c; ev('I'); *o = (i2c_master_dev_handle_t)1; return ESP_OK; }
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d) { (void)d; return ESP_OK; }
esp_err_t i2c_master_probe(i2c_master_bus_handle_t b, uint16_t a, int t) { (void)b; (void)a; (void)t; ev('P'); return g_probe_result; }
esp_err_t i2c_master_receive(i2c_master_dev_handle_t d, uint8_t *rx, size_t n, int t)
{ (void)d; (void)rx; (void)n; (void)t; ev('I'); return ESP_FAIL; }
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *tx, size_t n, int t)
{ (void)d; (void)tx; (void)n; (void)t; ev('I'); return ESP_FAIL; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t *tx, size_t tl, uint8_t *rx, size_t rl, int t)
{ (void)d; (void)tx; (void)tl; (void)rx; (void)rl; (void)t; ev('I'); return ESP_FAIL; }
esp_err_t i2c_owner_transfer(i2c_owner_t *o, i2c_master_dev_handle_t d, const uint8_t *tx, size_t tl, uint8_t *rx, size_t rl, int t)
{ (void)o; (void)d; (void)tx; (void)tl; (void)rx; (void)rl; (void)t; ev('I'); return ESP_FAIL; }
esp_err_t i2c_owner_init(i2c_owner_t *o, i2c_master_bus_handle_t b, int a, int c, int d, int e)
{ (void)o; (void)b; (void)a; (void)c; (void)d; (void)e; ev('I'); return g_owner_init_result; }
esp_err_t i2c_owner_deinit(i2c_owner_t *o) { (void)o; return ESP_OK; }

static void setup(esp_err_t probe, esp_err_t owner_init)
{
    fake_time_reset_all();
    g_nev = 0;
    memset(g_ev, 0, sizeof(g_ev));
    g_probe_result = probe;
    g_owner_init_result = owner_init;
    g_stub_gpio_level_hook = gpio_hook;
}

/* index of the first event equal to c, or -1 */
static int first(char c) { for (int i = 0; i < g_nev; i++) if (g_ev[i] == c) return i; return -1; }

static void check_reset_before_i2c(const char *what)
{
    int lo = first('L'), hi = -1, i2c = -1;
    for (int i = lo + 1; lo >= 0 && i < g_nev; i++) if (g_ev[i] == 'H') { hi = i; break; }
    for (int i = 0; i < g_nev; i++) if (g_ev[i] == 'P' || g_ev[i] == 'I') { i2c = i; break; }
    printf("  %s: events=%.*s\n", what, g_nev, g_ev);
    TEST_CHECK(lo >= 0, "~RESET is driven low");
    TEST_CHECK(hi > lo, "~RESET is driven high again after the low");
    TEST_CHECK(i2c < 0 || hi < i2c, "the whole ~RESET pulse precedes the first I2C access");
}

static void test_probe_miss_still_pulses_reset(void)
{
    SX1509Class e; memset(&e, 0, sizeof(e));
    setup(ESP_ERR_TIMEOUT, ESP_OK);
    esp_err_t err = SX1509_start(&e, (i2c_master_bus_handle_t)1);
    TEST_CHECK(err == ESP_ERR_NOT_FOUND, "probe miss reports NOT_FOUND");
    TEST_CHECK(first('P') >= 0, "the probe was attempted");
    TEST_CHECK(first('I') < 0, "no other I2C transfer after a probe miss");
    check_reset_before_i2c("probe miss");
}

static void test_init_failure_still_pulses_reset_first(void)
{
    SX1509Class e; memset(&e, 0, sizeof(e));
    setup(ESP_OK, ESP_FAIL);
    esp_err_t err = SX1509_start(&e, (i2c_master_bus_handle_t)1);
    TEST_CHECK(err != ESP_OK, "init failure is reported");
    check_reset_before_i2c("init failure");
}

static void test_pulse_pin_standalone_and_unwired(void)
{
    setup(ESP_OK, ESP_OK);
    TEST_CHECK(SX1509_pulse_reset_pin(SX1509_RESET_IO) == ESP_OK, "standalone pulse succeeds");
    TEST_CHECK(g_nev >= 2 && g_ev[g_nev - 2] == 'L' && g_ev[g_nev - 1] == 'H', "ends low then high");
    TEST_CHECK(first('P') < 0 && first('I') < 0, "standalone pulse makes no I2C access");
    setup(ESP_OK, ESP_OK);
    TEST_CHECK(SX1509_pulse_reset_pin(-1) == ESP_OK && g_nev == 0, "unwired pin (-1) is a no-op");
}

int main(void)
{
    TEST_SECTION("SX1509 ~RESET pulsed before any I2C (boot MED-2)");
    test_probe_miss_still_pulses_reset();
    test_init_failure_still_pulses_reset_first();
    test_pulse_pin_standalone_and_unwired();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
