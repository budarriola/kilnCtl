// recovery_wifi_qr.h -- pure (no ESP-IDF) Wi-Fi join QR for the recovery LCD.
// Host-tested by check_recovery_wifi_qr.ps1 (test_recovery_wifi_qr.c, MSVC).
//
// The recovery SoftAP passphrase is shown only on the LCD (owner decision
// 2026-10-02). The text stays; this adds the standard "WIFI:T:WPA;S:<ssid>;
// P:<pass>;;" QR beside it so the bench join script (lcd_ap_reader.py) can
// read the credentials without glyph segmentation (whose 5/S ambiguity is why
// this exists). Encoder: vendored Project Nayuki qrcodegen (MIT,
// third_party/qrcodegen/LICENSE.txt), caller-supplied static buffers, no heap.
//
// Nothing here logs. The payload buffer and the encoder scratch are wiped
// before rqr_encode() returns; the finished module matrix (equivalent to the
// passphrase already held in RAM by the LCD module) is the only residue.
#ifndef RECOVERY_WIFI_QR_H
#define RECOVERY_WIFI_QR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "third_party/qrcodegen/qrcodegen.h"

#ifdef __cplusplus
extern "C" {
#endif

// Version 4 (33 modules) at ECC L holds 78 bytes; boostEcl raises the level
// within whatever version the payload needs. A default 16-char SSID plus the
// 12-char passphrase is 46 bytes (version 3, 29 modules).
#define RQR_MAX_VERSION 4
#define RQR_BUF_LEN     qrcodegen_BUFFER_LEN_FOR_VERSION(RQR_MAX_VERSION)
#define RQR_PAYLOAD_MAX 96

// Panel geometry: 3 px per module (the app's AP QR is ~2.5 px), the standard
// 4-module quiet zone. Largest symbol = (33+8)*3 = 123 px, inside the
// RQR_AREA_PX square reserved at the top-right of the 480x320 panel (above
// the "Join Wi-Fi network:" line, right of the status text, which is at most
// 336 px wide).
#define RQR_MODULE_PX 3
#define RQR_QUIET     4
#define RQR_AREA_PX   124

// Builds the payload with the Wi-Fi QR escaping (backslash, semicolon, comma,
// colon and double quote get a backslash). Returns the length, or 0 if an
// argument is NULL, the SSID is empty, or it does not fit (out is then wiped).
size_t rqr_build_payload(const char *ssid, const char *pass, char *out, size_t cap);

// Encodes ssid/pass into `qr`; `tmp` is scratch (both RQR_BUF_LEN bytes).
// Returns the module count per side (21..33) or 0 if it cannot be encoded.
int rqr_encode(const char *ssid, const char *pass, uint8_t *qr, uint8_t *tmp);

// Pixel extent (square) of a symbol of `size` modules including the quiet zone.
int rqr_extent_px(int size);

// Fills one scanline `y` (0..RQR_AREA_PX-1) of the RQR_AREA_PX-wide area as
// big-endian RGB565: the symbol is right- and top-aligned, quiet zone in
// `light`, everything else in `bg`.
void rqr_scanline(const uint8_t *qr, int size, int y, uint16_t dark, uint16_t light, uint16_t bg,
                  uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_WIFI_QR_H
