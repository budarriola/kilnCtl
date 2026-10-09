// recovery_passphrase.c -- see recovery_passphrase.h.
#include "recovery_passphrase.h"

#include <string.h>

void rpass_format(const uint8_t rnd[RPASS_LEN], char out[RPASS_LEN + 1])
{
    static const char alphabet[] = RPASS_ALPHABET;
    for (size_t i = 0; i < RPASS_LEN; i++) {
        out[i] = alphabet[rnd[i] & (RPASS_ALPHABET_LEN - 1u)];
    }
    out[RPASS_LEN] = '\0';
}

bool rpass_is_valid(const char *p)
{
    if (!p || strlen(p) != RPASS_LEN) {
        return false;
    }
    for (size_t i = 0; i < RPASS_LEN; i++) {
        if (!memchr(RPASS_ALPHABET, p[i], RPASS_ALPHABET_LEN)) {
            return false;
        }
    }
    return true;
}
