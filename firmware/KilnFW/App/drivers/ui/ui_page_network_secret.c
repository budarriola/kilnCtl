#include "ui_page_network_secret.h"

static void wipe_buf(char *buf, size_t cap)
{
    if (!buf) {
        return;
    }
    volatile char *p = buf; /* volatile: the buffers are never read again, do not let the compiler drop the stores */
    for (size_t i = 0; i < cap; i++) {
        p[i] = '\0';
    }
}

void ui_page_network_secret_wipe(char *uri, size_t uri_cap, char *last, size_t last_cap)
{
    wipe_buf(uri, uri_cap);
    wipe_buf(last, last_cap);
}
