// kiln_board_identity -- a small, stable 32-bit identifier for THIS
// controller, minted from the ESP32-S3's factory-burned eFuse MAC address.
//
// WHY THIS EXISTS: docs/KILN_PROFILES_PLAN.md section 5.3 row 2/3 requires
// telling "this board" apart from "some other board" so an imported kiln
// config package's CT calibration (which is a property of the sensor
// physically fitted to ONE controller, never portable) can be forced back
// to uncalibrated when a package crosses boards -- see kiln_cfg_store.c's
// kiln_cfg_store_import_package_json(). Before this module, no board-
// identity concept existed anywhere in this firmware at all (only a chip
// *family* id, esp_chip_info() -- see ui_page_diagnostics.c -- which is
// identical across every board of the same model and therefore useless for
// this purpose).
//
// NOT a security credential: the eFuse MAC is not secret, not signed, and
// this id is carried in the package's JSON envelope in plain, unauthenticated
// text (kiln_package.h's `source_board_id` field) -- exactly as
// unauthenticated as every other envelope field. It exists only to answer
// "did this package originate on the board that is about to import it",
// a compatibility question, not an authentication one.
//
// HOST-TESTABLE via kiln_board_identity_set_test_override() below, since the
// real eFuse read only exists on-target -- test/stubs/esp_mac.h supplies a
// fixed, deterministic fake MAC for host builds so kiln_board_identity_get()
// itself links and returns a stable value even with no override set, but a
// test that needs to prove the "differs" path needs a SECOND, distinct value
// too, which only the override can produce.
#ifndef KILN_BOARD_IDENTITY_H
#define KILN_BOARD_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This board's identity: esp_crc32_le() folded over the 6-byte factory
 * default MAC (esp_efuse_mac_get_default()) into a single uint32_t -- the
 * same CRC32 primitive kiln_package.c already uses, so no new dependency.
 * Never 0 in practice (esp_crc32_le() of a real, non-all-zero MAC), but
 * this module does not special-case 0 as "invalid" the way kiln_cfg_store.c
 * treats pkg_hash==0 -- a board-id comparison is a plain equality test, not
 * a sentinel-bearing field. Cheap to call repeatedly (efuse reads are not
 * hot-path expensive and this is never called from a tight loop), so no
 * caching -- simplicity over an unneeded optimization. */
uint32_t kiln_board_identity_get(void);

/* Test-only seam: when `active` is true, kiln_board_identity_get() returns
 * `value` instead of reading the real eFuse MAC, until the next call with
 * `active=false`. Production code must NEVER call this -- it exists solely
 * so a host test can force two DIFFERENT board identities in the same
 * process to exercise the "package crossed boards" path, which a fixed
 * stub MAC alone cannot do (see this header's own banner comment). */
void kiln_board_identity_set_test_override(bool active, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif // KILN_BOARD_IDENTITY_H
