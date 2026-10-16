#include "flash_hop_host_stub.h"

bool g_flash_hop_stub_on_worker = false;

bool uart_bridge_ext_is_on_flash_worker(void)
{
    return g_flash_hop_stub_on_worker;
}
