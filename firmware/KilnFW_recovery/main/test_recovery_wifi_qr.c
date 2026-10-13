// test_recovery_wifi_qr.c -- host test for recovery_wifi_qr.c. Built and run by
// check_recovery_wifi_qr.ps1 (MSVC). Prints "RESULT pass=N fail=N".
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>

#include "recovery_wifi_qr.h"

static int g_pass, g_fail;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            g_pass++;                                                         \
        } else {                                                              \
            g_fail++;                                                         \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, name); \
        }                                                                     \
    } while (0)

static uint8_t qr[RQR_BUF_LEN], tmp[RQR_BUF_LEN], ref[RQR_BUF_LEN], reft[RQR_BUF_LEN];

static void test_payload(void)
{
    char out[RQR_PAYLOAD_MAX + 1];
    size_t n = rqr_build_payload("kilnctl-recovery", "ABCDEFGH2345", out, sizeof(out));
    CHECK(n == strlen(out) && n > 0, "length matches");
    CHECK(strcmp(out, "WIFI:T:WPA;S:kilnctl-recovery;P:ABCDEFGH2345;;") == 0, "plain payload");

    // Each special character is backslash-escaped, in SSID and passphrase.
    n = rqr_build_payload("a;b,c:d\\e\"f", "p;q,r:s\\t\"u", out, sizeof(out));
    CHECK(strcmp(out, "WIFI:T:WPA;S:a\\;b\\,c\\:d\\\\e\\\"f;P:p\\;q\\,r\\:s\\\\t\\\"u;;") == 0,
          "all five specials escaped");
    CHECK(n == strlen(out), "escaped length matches");
    // Characters that need no escaping stay as they are.
    rqr_build_payload("a b-c_d.e'f", "x/y=z+", out, sizeof(out));
    CHECK(strcmp(out, "WIFI:T:WPA;S:a b-c_d.e'f;P:x/y=z+;;") == 0, "non-specials untouched");

    // Bad arguments and overflow return 0 and leave no partial credential.
    CHECK(rqr_build_payload(NULL, "p", out, sizeof(out)) == 0, "NULL ssid");
    CHECK(rqr_build_payload("", "p", out, sizeof(out)) == 0, "empty ssid");
    CHECK(rqr_build_payload("s", NULL, out, sizeof(out)) == 0, "NULL pass");
    CHECK(rqr_build_payload("s", "p", NULL, 10) == 0, "NULL out");
    CHECK(rqr_build_payload("s", "p", out, 0) == 0, "zero cap");
    memset(out, 'x', sizeof(out));
    CHECK(rqr_build_payload("kilnctl-recovery", "ABCDEFGH2345", out, 20) == 0, "overflow rejected");
    int clean = 1;
    for (size_t i = 0; i < 20; i++) {
        clean &= out[i] == 0;
    }
    CHECK(clean, "overflow wipes the buffer");
    // Exact fit: the terminator must still fit.
    const char *exact = "WIFI:T:WPA;S:s;P:p;;";
    char small[32];
    CHECK(rqr_build_payload("s", "p", small, strlen(exact) + 1) == strlen(exact), "exact fit");
    CHECK(rqr_build_payload("s", "p", small, strlen(exact)) == 0, "one byte short rejected");
}

static void test_encode_matches_reference(void)
{
    // The module matrix must equal an independent encode of the expected text.
    const char *text = "WIFI:T:WPA;S:kilnctl-recovery;P:ABCDEFGH2345;;";
    int size = rqr_encode("kilnctl-recovery", "ABCDEFGH2345", qr, tmp);
    CHECK(qrcodegen_encodeText(text, reft, ref, qrcodegen_Ecc_LOW, 1, 4, qrcodegen_Mask_AUTO, true),
          "reference encodes");
    CHECK(size == 29, "default credentials give a version 3 symbol");
    CHECK(size == qrcodegen_getSize(ref), "size matches reference");
    int same = size > 0;
    for (int y = 0; y < size && same; y++) {
        for (int x = 0; x < size; x++) {
            if (qrcodegen_getModule(qr, x, y) != qrcodegen_getModule(ref, x, y)) {
                same = 0;
                break;
            }
        }
    }
    CHECK(same, "matrix equals the reference encode");
    CHECK(qrcodegen_getModule(qr, 0, 0) && qrcodegen_getModule(qr, size - 1, 0) &&
              qrcodegen_getModule(qr, 0, size - 1),
          "finder corners dark");
    // Scratch is wiped (no credential-bearing residue beyond the matrix).
    int zero = 1;
    for (size_t i = 0; i < sizeof(tmp); i++) {
        zero &= tmp[i] == 0;
    }
    CHECK(zero, "scratch buffer wiped");

    // A ';' in the SSID changes the symbol.
    int size2 = rqr_encode("kilnctl;recovery", "ABCDEFGH2345", ref, reft);
    CHECK(size2 > 0 && memcmp(ref, qr, sizeof(qr)) != 0, "different SSID gives different matrix");

    // A short payload has spare capacity in its version: ECC is boosted there.
    int sh = rqr_encode("s", "p", qr, tmp);
    CHECK(qrcodegen_encodeText("WIFI:T:WPA;S:s;P:p;;", reft, ref, qrcodegen_Ecc_LOW, 1, 4,
                               qrcodegen_Mask_AUTO, true),
          "short reference encodes");
    int shsame = sh > 0 && sh == qrcodegen_getSize(ref);
    for (int y = 0; shsame && y < sh; y++) {
        for (int x = 0; x < sh; x++) {
            shsame &= qrcodegen_getModule(qr, x, y) == qrcodegen_getModule(ref, x, y);
        }
    }
    CHECK(shsame, "short payload boosts ECC");

    // 32 plain chars: 13+32+3+12+2 = 62 bytes -> version 4.
    int s32 = rqr_encode("0123456789abcdef0123456789abcdef", "ABCDEFGH2345", qr, tmp);
    CHECK(s32 == 33, "32-char SSID gives version 4 (33 modules)");
    CHECK(rqr_extent_px(s32) <= RQR_AREA_PX, "largest symbol fits the reserved area");
    CHECK(rqr_extent_px(29) == (29 + 2 * RQR_QUIET) * RQR_MODULE_PX, "extent formula");
    // 32 escaped chars (94 bytes) exceed version 4: no symbol, text-only fallback.
    CHECK(rqr_encode(";;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;", "ABCDEFGH2345", qr, tmp) == 0,
          "too long for version 4 -> 0");
    CHECK(rqr_encode("", "ABCDEFGH2345", qr, tmp) == 0, "empty ssid -> 0");
    CHECK(rqr_encode("a", "b", NULL, tmp) == 0, "NULL qr -> 0");
}

static void test_scanline(void)
{
    const uint16_t DARK = 0x0000, LIGHT = 0xFFFF, BG = 0x1234;
    uint8_t row[RQR_AREA_PX * 2];
    int size = rqr_encode("kilnctl-recovery", "ABCDEFGH2345", qr, tmp);
    int ext = rqr_extent_px(size);
    int x0 = RQR_AREA_PX - ext;
    CHECK(x0 >= 0 && x0 < RQR_AREA_PX, "symbol narrower than the area");

#define PX(i) ((uint16_t)((row[2 * (i)] << 8) | row[2 * (i) + 1]))
    // Top quiet-zone row: light from x0 to the right edge, bg left of it.
    rqr_scanline(qr, size, 0, DARK, LIGHT, BG, row);
    CHECK(PX(0) == BG && PX(x0 - 1) == BG && PX(x0) == LIGHT && PX(RQR_AREA_PX - 1) == LIGHT,
          "quiet zone right-aligned, bg to the left");
    // First module row: the top-left finder module is dark at the quiet-zone offset.
    int y = RQR_QUIET * RQR_MODULE_PX;
    rqr_scanline(qr, size, y, DARK, LIGHT, BG, row);
    CHECK(PX(x0 + RQR_QUIET * RQR_MODULE_PX) == DARK, "first dark module at the quiet-zone offset");
    CHECK(PX(x0 + RQR_QUIET * RQR_MODULE_PX - 1) == LIGHT, "pixel before it is quiet zone");
    CHECK(PX(x0 + RQR_QUIET * RQR_MODULE_PX + RQR_MODULE_PX - 1) == DARK, "module is MODULE_PX wide");
    // Every scanline row matches the matrix module-for-module.
    int bad = 0;
    for (int yy = 0; yy < ext; yy++) {
        rqr_scanline(qr, size, yy, DARK, LIGHT, BG, row);
        for (int xx = x0; xx < RQR_AREA_PX; xx++) {
            int mx = (xx - x0) / RQR_MODULE_PX - RQR_QUIET;
            int my = yy / RQR_MODULE_PX - RQR_QUIET;
            int dark = mx >= 0 && my >= 0 && mx < size && my < size && qrcodegen_getModule(qr, mx, my);
            if (PX(xx) != (dark ? DARK : LIGHT)) {
                bad++;
            }
        }
    }
    CHECK(bad == 0, "every pixel matches the matrix");
    // Below the symbol, and with no symbol at all: solid background.
    rqr_scanline(qr, size, ext, DARK, LIGHT, BG, row);
    int allbg = 1;
    for (int i = 0; i < RQR_AREA_PX; i++) {
        allbg &= PX(i) == BG;
    }
    CHECK(allbg, "rows below the symbol are bg");
    rqr_scanline(qr, 0, 10, DARK, LIGHT, BG, row);
    allbg = 1;
    for (int i = 0; i < RQR_AREA_PX; i++) {
        allbg &= PX(i) == BG;
    }
    CHECK(allbg, "size 0 draws nothing");
    // Big-endian byte order (the panel takes the high byte first).
    rqr_scanline(qr, 0, 0, DARK, LIGHT, 0xABCD, row);
    CHECK(row[0] == 0xAB && row[1] == 0xCD, "big-endian RGB565");
#undef PX
}

int main(void)
{
    test_payload();
    test_encode_matches_reference();
    test_scanline();
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
