/* Fuzz harness over kilnlink_frame's two decoders: kilnlink_frame_decode and
 * kilnlink_unstuff. Both take untrusted bytes off an isolated link a second
 * processor drives -- CommonFW/README.md rule 6 says every decoder must be
 * bounds-checked and return a status, never read past its input. This throws
 * random and structured-random garbage at both, at every length from 0 up
 * through a few times the max frame size, and the only thing it asserts is
 * "did not crash" -- a decoder that returns an error for nonsense input is
 * doing its job; one that reads out of bounds, or loops forever, is not.
 *
 * Deterministic (a fixed PRNG seed): a fuzz failure needs to be reproducible,
 * not "run it again and hope". Runs under the same host toolchain as
 * test_frame.c (MSVC/xtensa-gcc/arm-none-eabi-gcc), no external fuzzing
 * library, so it works everywhere the rest of the host suite does. This is a
 * bounded, seeded random walk, not a coverage-guided fuzzer (no libFuzzer/
 * AFL integration) -- good enough to catch an out-of-bounds read or an
 * infinite loop, not a substitute for one if this code ever needs that level
 * of scrutiny.
 *
 * Build: same as test_frame.c, just swap the main source file:
 *   cl /nologo /W4 /std:c17 /I ..\include test_fuzz.c ..\src\kilnlink_frame.c ..\src\kilnlink_crc.c
 */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_frame.h"

/* xorshift32 -- small, dependency-free, deterministic from a fixed seed.
 * Not cryptographic; doesn't need to be. */
static uint32_t g_rng_state = 0x9E3779B9u; /* fixed seed: reproducible runs */

static uint32_t next_rand(void)
{
    uint32_t x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

static void fill_random(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        buf[i] = (uint8_t)(next_rand() & 0xFFu);
    }
}

/* Occasionally biases toward "structured" garbage -- a real delimiter or
 * escape byte in a plausible header position -- rather than pure noise,
 * since those are the inputs most likely to trip an off-by-one that uniform
 * random bytes would almost never land on by chance. */
static void fill_structured_random(uint8_t *buf, size_t len)
{
    fill_random(buf, len);
    if (len == 0) {
        return;
    }
    uint32_t biased_count = next_rand() % (len + 1);
    for (uint32_t i = 0; i < biased_count; ++i) {
        size_t pos = next_rand() % len;
        switch (next_rand() % 4) {
            case 0: buf[pos] = KILNLINK_FRAME_DELIM; break;
            case 1: buf[pos] = KILNLINK_FRAME_ESC; break;
            case 2: buf[pos] = (uint8_t)KILNLINK_MSG_DATA; break; /* plausible type byte at pos 0 */
            default: buf[pos] = (uint8_t)(KILNLINK_FRAME_MAX_PAYLOAD + next_rand() % 8); break;
        }
    }
}

#define FUZZ_MAX_LEN (KILNLINK_FRAME_STUFFED_MAX + 32u)
#define ITERATIONS_PER_LENGTH 200u

int main(void)
{
    uint8_t input[FUZZ_MAX_LEN];
    uint8_t unstuffed[KILNLINK_FRAME_RAW_MAX + 32];
    kilnlink_frame_t decoded;
    kilnlink_frame_status_t status;
    unsigned long total = 0;

    for (size_t len = 0; len <= FUZZ_MAX_LEN; ++len) {
        for (unsigned iter = 0; iter < ITERATIONS_PER_LENGTH; ++iter) {
            if (iter % 2 == 0) {
                fill_random(input, len);
            } else {
                fill_structured_random(input, len);
            }

            /* kilnlink_frame_decode must never read past `input[0..len)`
             * regardless of what LENGTH claims -- it validates raw_len
             * against the declared length before trusting either. */
            (void)kilnlink_frame_decode(input, len, &decoded);

            /* kilnlink_unstuff must never write past `unstuffed`'s bound
             * and must always report a status rather than silently
             * misbehaving on an unterminated escape. */
            (void)kilnlink_unstuff(input, len, unstuffed, sizeof(unstuffed), &status);

            total++;
        }
    }

    /* Getting here at all means neither decoder crashed, hung, or (per
     * AddressSanitizer/UBSan if this binary is ever built with them)
     * touched memory it shouldn't have across every length from 0 through
     * well past the largest legal frame, fed both uniform and
     * structurally-biased garbage. */
    printf("fuzzed %lu inputs (lengths 0..%u, %u iterations each) -- no crash, no hang\n",
           total, (unsigned)FUZZ_MAX_LEN, ITERATIONS_PER_LENGTH);
    printf("ALL PASS\n");
    return 0;
}
