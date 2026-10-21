// test_recovery_passphrase.c -- host test for recovery_passphrase.c. Built and
// run by check_recovery_passphrase.ps1 (MSVC). Prints "RESULT pass=N fail=N".
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "recovery_passphrase.h"

static int g_pass, g_fail;

#define CHECK(cond, name)                                                   \
    do {                                                                    \
        if (cond) {                                                         \
            g_pass++;                                                       \
        } else {                                                            \
            g_fail++;                                                       \
            fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);        \
        }                                                                   \
    } while (0)

static void test_alphabet(void)
{
    CHECK(strlen(RPASS_ALPHABET) == RPASS_ALPHABET_LEN, "alphabet length is 26");
    // RE-03: >= 60 bits of min-entropy (worst symbol hit by 10 of 256 byte values).
    CHECK((double)RPASS_LEN * log2(256.0 / 10.0) >= 60.0, "passphrase has >= 60 bits");
    CHECK(RPASS_LEN >= 10 && RPASS_LEN <= 16, "passphrase length 10..16");
    // No ambiguous symbols, no duplicates.
    const char *bad = "0O1lIoi5S2Z8B";
    for (const char *b = bad; *b; b++) {
        CHECK(strchr(RPASS_ALPHABET, *b) == NULL, "ambiguous symbol absent from alphabet");
    }
    for (size_t i = 0; i < RPASS_ALPHABET_LEN; i++) {
        for (size_t j = i + 1; j < RPASS_ALPHABET_LEN; j++) {
            CHECK(RPASS_ALPHABET[i] != RPASS_ALPHABET[j], "alphabet has no duplicates");
        }
    }
}

static void test_every_byte_maps_into_alphabet(void)
{
    // Every possible random byte, in every position, yields a valid symbol,
    // and all 26 symbols are reachable (each hit by 9 or 10 byte values).
    int hits[256] = {0};
    for (int v = 0; v < 256; v++) {
        uint8_t rnd[RPASS_LEN];
        memset(rnd, v, sizeof(rnd));
        char out[RPASS_LEN + 1];
        rpass_format(rnd, out);
        CHECK(strlen(out) == RPASS_LEN, "output length");
        CHECK(rpass_is_valid(out), "formatted passphrase is valid");
        hits[(unsigned char)out[0]]++;
    }
    int reached = 0;
    for (size_t i = 0; i < RPASS_ALPHABET_LEN; i++) {
        unsigned char c = (unsigned char)RPASS_ALPHABET[i];
        if (hits[c] > 0) {
            reached++;
        }
        CHECK(hits[c] == 9 || hits[c] == 10, "bias bounded: each symbol from 9 or 10 byte values");
    }
    CHECK(reached == (int)RPASS_ALPHABET_LEN, "all symbols reachable");
}

static void test_positions_independent(void)
{
    uint8_t rnd[RPASS_LEN];
    for (size_t i = 0; i < RPASS_LEN; i++) {
        rnd[i] = (uint8_t)i;
    }
    char out[RPASS_LEN + 1];
    rpass_format(rnd, out);
    for (size_t i = 0; i < RPASS_LEN; i++) {
        CHECK(out[i] == RPASS_ALPHABET[i], "each position uses its own random byte");
    }
}

static void test_validator(void)
{
    CHECK(!rpass_is_valid(NULL), "NULL invalid");
    CHECK(!rpass_is_valid(""), "empty invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMN"), "too short invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMNPQ"), "too long invalid");
    CHECK(rpass_is_valid("ACDEFGHJKLMNP"), "13 good symbols valid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMN0"), "digit 0 invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMNO"), "letter O invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMN1"), "digit 1 invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMNl"), "letter l invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMNI"), "letter I invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMN5"), "digit 5 invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMNS"), "letter S invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMN2"), "digit 2 invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMNZ"), "letter Z invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMN8"), "digit 8 invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMNB"), "letter B invalid");
    CHECK(!rpass_is_valid("ACDEFGHJKLMNa"), "lowercase invalid");
}

int main(void)
{
    test_alphabet();
    test_every_byte_maps_into_alphabet();
    test_positions_independent();
    test_validator();
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
