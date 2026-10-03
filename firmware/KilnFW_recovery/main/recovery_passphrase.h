// recovery_passphrase.h -- pure (no ESP-IDF) formatting of the recovery
// SoftAP's per-boot random WPA2 passphrase. Host-tested by
// check_recovery_passphrase.ps1 (test_recovery_passphrase.c, MSVC).
//
// The recovery image is unauthenticated by owner decision 2026-10-02
// (docs/RECOVERY_IMAGE_PLAN.md): the passphrase, shown only on the LCD, is the
// sole access control. It is generated per boot from the hardware RNG
// (recovery_wifi.c), lives only in RAM, and is never put in an HTTP response,
// a log line, the serial port or NVS.
//
// Alphabet: 32 unambiguous symbols (digits 2-9 and uppercase letters minus
// I and O -- no 0/O/1/l/I). 32 symbols means each random byte maps with
// `& 31` and no modulo bias. 12 symbols = 60 bits, inside WPA2's 8..63 range.
#ifndef RECOVERY_PASSPHRASE_H
#define RECOVERY_PASSPHRASE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RPASS_LEN 12u
#define RPASS_ALPHABET "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"
#define RPASS_ALPHABET_LEN 32u

// Maps RPASS_LEN random bytes to RPASS_LEN alphabet symbols plus a NUL.
void rpass_format(const uint8_t rnd[RPASS_LEN], char out[RPASS_LEN + 1]);

// True when `p` is exactly RPASS_LEN symbols, all from RPASS_ALPHABET.
bool rpass_is_valid(const char *p);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_PASSPHRASE_H
