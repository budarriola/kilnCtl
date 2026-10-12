// Pure helper for the LCD network page (ui_page_network.c): wipes the cached AP join secret.
// REVIEW_LCDFX13 INFO-1: while the page masks (non-admin viewer), the plaintext WIFI:...;P:<password>;; payload and the
// "last encoded" copy must not stay in RAM for a later code path (or a hidden QR widget) to leak. No LVGL: host-testable.
#ifndef UI_PAGE_NETWORK_SECRET_H
#define UI_PAGE_NETWORK_SECRET_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Non-secret payload the hidden AP QR widget is re-encoded with after a wipe.
#define UI_PAGE_NETWORK_AP_QR_BLANK "WIFI:;;"

// Zeroes both buffers completely (not just the first byte). NULL / zero-size buffers are skipped.
void ui_page_network_secret_wipe(char *uri, size_t uri_cap, char *last, size_t last_cap);

#ifdef __cplusplus
}
#endif

#endif
