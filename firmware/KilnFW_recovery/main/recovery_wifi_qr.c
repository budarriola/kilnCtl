// recovery_wifi_qr.c -- see recovery_wifi_qr.h.
#include "recovery_wifi_qr.h"

#include <string.h>

// Zeroes a buffer in a way the optimizer may not elide (a dead stack local's
// plain memset can be dropped at -Os).
void rqr_secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) {
        *v++ = 0;
    }
}

static bool needs_escape(char c)
{
    return c == '\\' || c == ';' || c == ',' || c == ':' || c == '"';
}

// Appends `s` (escaped when `esc`) at out[*n]; false when it does not fit.
static bool put_str(char *out, size_t cap, size_t *n, const char *s, bool esc)
{
    for (; *s; s++) {
        if (esc && needs_escape(*s)) {
            if (*n + 1 >= cap) {
                return false;
            }
            out[(*n)++] = '\\';
        }
        if (*n + 1 >= cap) {
            return false;
        }
        out[(*n)++] = *s;
    }
    return true;
}

size_t rqr_build_payload(const char *ssid, const char *pass, char *out, size_t cap)
{
    if (!ssid || !ssid[0] || !pass || !out || cap == 0) {
        return 0;
    }
    size_t n = 0;
    bool ok = put_str(out, cap, &n, "WIFI:T:WPA;S:", false) && put_str(out, cap, &n, ssid, true) &&
              put_str(out, cap, &n, ";P:", false) && put_str(out, cap, &n, pass, true) &&
              put_str(out, cap, &n, ";;", false);
    if (!ok) {
        rqr_secure_zero(out, cap);
        return 0;
    }
    out[n] = '\0';
    return n;
}

int rqr_encode(const char *ssid, const char *pass, uint8_t *qr, uint8_t *tmp)
{
    char payload[RQR_PAYLOAD_MAX + 1];
    size_t n = rqr_build_payload(ssid, pass, payload, sizeof(payload));
    int size = 0;
    if (n != 0 && qr && tmp &&
        qrcodegen_encodeText(payload, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, RQR_MAX_VERSION,
                             qrcodegen_Mask_AUTO, true)) {
        size = qrcodegen_getSize(qr);
    }
    rqr_secure_zero(payload, sizeof(payload));
    if (tmp) {
        rqr_secure_zero(tmp, RQR_BUF_LEN);
    }
    return size;
}

int rqr_extent_px(int size)
{
    return (size + 2 * RQR_QUIET) * RQR_MODULE_PX;
}

static void put_px(uint8_t *out, int x, uint16_t c)
{
    out[2 * x] = (uint8_t)(c >> 8);
    out[2 * x + 1] = (uint8_t)(c & 0xFF);
}

void rqr_scanline(const uint8_t *qr, int size, int y, uint16_t dark, uint16_t light, uint16_t bg,
                  uint8_t *out)
{
    int ext = (qr && size > 0) ? rqr_extent_px(size) : 0;
    int x0 = RQR_AREA_PX - ext;
    for (int x = 0; x < RQR_AREA_PX; x++) {
        uint16_t c = bg;
        if (ext > 0 && ext <= RQR_AREA_PX && y >= 0 && y < ext && x >= x0) {
            int mx = (x - x0) / RQR_MODULE_PX - RQR_QUIET;
            int my = y / RQR_MODULE_PX - RQR_QUIET;
            c = (mx >= 0 && my >= 0 && mx < size && my < size && qrcodegen_getModule(qr, mx, my)) ? dark
                                                                                              : light;
        }
        put_px(out, x, c);
    }
}
