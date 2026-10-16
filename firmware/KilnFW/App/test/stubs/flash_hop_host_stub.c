#include "flash_hop_host_stub.h"

#include "esp_err.h"
#include "fake_kv.h"
#include "hal_kv.h"

unsigned g_flash_hop_stub_dispatches = 0;
bool g_flash_hop_stub_fail = false;

void flash_hop_stub_reset(void)
{
    g_flash_hop_stub_dispatches = 0;
    g_flash_hop_stub_on_worker = false;
    g_flash_hop_stub_fail = false;
}

esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg)
{
    if (g_flash_hop_stub_fail) {
        return ESP_ERR_INVALID_STATE;
    }
    g_flash_hop_stub_dispatches++;
    bool prev_safe = hal_kv_write_safe_here();
    bool prev_on = g_flash_hop_stub_on_worker;
    fake_kv_set_write_safe_here(true);
    g_flash_hop_stub_on_worker = true;
    fn(arg);
    g_flash_hop_stub_on_worker = prev_on;
    fake_kv_set_write_safe_here(prev_safe);
    return ESP_OK;
}
