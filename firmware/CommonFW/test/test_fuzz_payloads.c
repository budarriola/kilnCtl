/* Fuzz harness over every kilnlink payload decoder -- the other half of
 * GUARD_TEST_MATRIX.md's "Fuzz over every decoder" row. test_fuzz.c already
 * covers the two framing-layer decoders (kilnlink_frame_decode,
 * kilnlink_unstuff); this file covers the ~27 payload codecs one layer up
 * (kilnlink_context_decode, kilnlink_status_decode, ... kilnlink_fw_version_
 * decode), which is the layer where LINK_PROTOCOL.md's documented hazard
 * lives: an offset/length mismatch that silently misdecodes a temperature
 * instead of failing loud.
 *
 * Every decoder here shares one call shape: (const uint8_t *payload, size_t
 * len, T *out) -> status_t, "never read past payload[len-1], never write
 * past *out, always return a status rather than guess". This file drives
 * that contract with, per decoder:
 *
 *   - a fixed, deterministic corpus: empty, 1 byte, exactly the shortest
 *     length that could plausibly be accepted minus one/exactly/plus one,
 *     exactly the longest such length minus one/exactly/plus one, all-zero
 *     and all-0xFF buffers at several lengths, a real valid-looking frame
 *     (built with the matching _encode()/_pack()) with one byte flipped at
 *     every position, and that same valid frame truncated at every offset
 *     from 0 to its length;
 *   - randomized bytes (uniform and structurally-biased, same technique as
 *     test_fuzz.c) at every length from 0 through a generous margin past
 *     the decoder's longest legal frame.
 *
 * The property under test, at every one of the calls above: no crash, no
 * hang, and -- since this host toolchain's plain `cl` build has no ASan/
 * UBSan available (no `/fsanitize=address` flag on the MSVC Build Tools
 * version this repo pins; see build_host_tests.ps1) -- no write past the
 * output struct's bounds, checked explicitly with canary bytes planted
 * before and after every `out` struct and re-checked after every call. This
 * turns "did not crash" into "provably did not write out of bounds", which
 * is the fallback CommonFW/README.md rule 6 calls for when a sanitizer
 * build isn't available. The input buffer itself is a fixed-size local
 * array sized to the largest case tested, addressed by a length always
 * respected by the caller (this harness), so an input-side OOB read would
 * show up as a length-independent decode result, not a crash -- decoders
 * are additionally required to respect `len` exactly (see MAX_LEN-bounded
 * loops below), which is what catches "ignored len, read past it" bugs.
 *
 * Deterministic: a fixed-seed xorshift32, override with the
 * KILNLINK_FUZZ_SEED environment variable (decimal). The per-length random
 * iteration count defaults to a small, CI-friendly value; override with
 * KILNLINK_FUZZ_ITERS (decimal) for a longer soak run. The seed actually
 * used is always printed, so a failure is reproducible by re-running with
 * that seed pinned via the environment variable.
 *
 * Build: same pattern as test_fuzz.c, linked against the whole `kilnlink`
 * library (every codec's .c file) rather than just kilnlink_frame.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kilnlink/kilnlink_announce.h"
#include "kilnlink/kilnlink_announce_reboot.h"
#include "kilnlink/kilnlink_ceiling.h"
#include "kilnlink/kilnlink_clear_trip.h"
#include "kilnlink/kilnlink_test_trip.h"
#include "kilnlink/kilnlink_test_trip_result.h"
#include "kilnlink/kilnlink_commit_config.h"
#include "kilnlink/kilnlink_commit_config_rejected.h"
#include "kilnlink/kilnlink_apply_config_volatile.h"
#include "kilnlink/kilnlink_config_page.h"
#include "kilnlink/kilnlink_context.h"
#include "kilnlink/kilnlink_ct_cal.h"
#include "kilnlink/kilnlink_ct_auto_zero_begin.h"
#include "kilnlink/kilnlink_ct_auto_zero_status.h"
#include "kilnlink/kilnlink_get_ct_auto_zero.h"
#include "kilnlink/kilnlink_diag.h"
#include "kilnlink/kilnlink_fw_version.h"
#include "kilnlink/kilnlink_get_config_page.h"
#include "kilnlink/kilnlink_get_ct_cal.h"
#include "kilnlink/kilnlink_get_fw_version.h"
#include "kilnlink/kilnlink_get_param.h"
#include "kilnlink/kilnlink_inject_tc.h"
#include "kilnlink/kilnlink_param.h"
#include "kilnlink/kilnlink_param_value.h"
#include "kilnlink/kilnlink_power.h"
#include "kilnlink/kilnlink_rollback.h"
#include "kilnlink/kilnlink_reboot.h"
#include "kilnlink/kilnlink_reboot_result.h"
#include "kilnlink/kilnlink_rollback_result.h"
#include "kilnlink/kilnlink_set_clock.h"
#include "kilnlink/kilnlink_set_config.h"
#include "kilnlink/kilnlink_set_ct_cal.h"
#include "kilnlink/kilnlink_set_log_level.h"
#include "kilnlink/kilnlink_set_param.h"
#include "kilnlink/kilnlink_get_stack_margin.h"
#include "kilnlink/kilnlink_stack_margin.h"
#include "kilnlink/kilnlink_status.h"
#include "kilnlink/kilnlink_trip.h"

/* --------------------------------------------------------------------- */
/* Deterministic PRNG -- same xorshift32 as test_fuzz.c, independent state
 * so the two binaries stay reproducible on their own. */

static uint32_t g_rng_state = 0x9E3779B9u;

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

static void fill_structured_random(uint8_t *buf, size_t len)
{
    fill_random(buf, len);
    if (len == 0) {
        return;
    }
    uint32_t biased_count = next_rand() % (uint32_t)(len + 1);
    for (uint32_t i = 0; i < biased_count; ++i) {
        size_t pos = next_rand() % len;
        switch (next_rand() % 3) {
            case 0: buf[pos] = 0x00u; break;
            case 1: buf[pos] = 0xFFu; break;
            default: buf[pos] = (uint8_t)(next_rand() & 0xFFu); break;
        }
    }
}

/* --------------------------------------------------------------------- */
/* Canary-guarded output structs: every decoder's `out` is embedded between
 * two fixed sentinel regions. A decoder that writes even one byte past its
 * own struct corrupts a canary, caught by check_canaries() after every
 * call -- the explicit-bounds-assertion fallback this file's header comment
 * promises in place of ASan. */

#define CANARY_LEN 64u
#define CANARY_BYTE 0xA5u

typedef struct {
    uint8_t pre[CANARY_LEN];
    union {
        kilnlink_context_t context;
        kilnlink_status_t status;
        kilnlink_power_t power;
        kilnlink_announce_t announce;
        kilnlink_diag_t diag;
        kilnlink_trip_t trip;
        kilnlink_ceiling_t ceiling;
        kilnlink_clear_trip_t clear_trip;
        kilnlink_test_trip_t test_trip;
        kilnlink_test_trip_result_t test_trip_result;
        kilnlink_set_config_t set_config;
        kilnlink_rollback_t rollback;
        kilnlink_get_fw_version_t get_fw_version;
        kilnlink_set_clock_t set_clock;
        kilnlink_announce_reboot_t announce_reboot;
        kilnlink_set_ct_cal_t set_ct_cal;
        kilnlink_get_ct_cal_t get_ct_cal;
        kilnlink_ct_cal_t ct_cal;
        kilnlink_ct_auto_zero_begin_t ct_auto_zero_begin;
        kilnlink_get_ct_auto_zero_t get_ct_auto_zero;
        kilnlink_ct_auto_zero_status_t ct_auto_zero_status;
        kilnlink_set_log_level_t set_log_level;
        kilnlink_set_param_t set_param;
        kilnlink_commit_config_t commit_config;
        kilnlink_commit_config_rejected_t commit_config_rejected;
        kilnlink_apply_config_volatile_t apply_config_volatile;
        kilnlink_inject_tc_t inject_tc;
        kilnlink_get_param_t get_param;
        kilnlink_param_t param;
        kilnlink_get_config_page_t get_config_page;
        kilnlink_config_page_t config_page;
        kilnlink_rollback_result_t rollback_result;
        kilnlink_reboot_t reboot;
        kilnlink_reboot_result_t reboot_result;
        kilnlink_fw_version_t fw_version;
        kilnlink_get_stack_margin_t get_stack_margin;
        kilnlink_stack_margin_t stack_margin;
    } out;
    uint8_t post[CANARY_LEN];
} guarded_out_t;

static void arm_canaries(guarded_out_t *g)
{
    memset(g->pre, CANARY_BYTE, sizeof(g->pre));
    /* Poison the union too, so an "accidentally correct because it happened
     * to be zero already" false negative can't hide a missed write. */
    memset(&g->out, 0x5A, sizeof(g->out));
    memset(g->post, CANARY_BYTE, sizeof(g->post));
}

static void check_canaries(guarded_out_t *g, const char *decoder_name, unsigned long call_idx)
{
    for (size_t i = 0; i < CANARY_LEN; ++i) {
        if (g->pre[i] != CANARY_BYTE) {
            fprintf(stderr,
                    "FUZZ FAIL: %s wrote before its output struct "
                    "(pre-canary byte %zu corrupted, call #%lu)\n",
                    decoder_name, i, call_idx);
            exit(1);
        }
        if (g->post[i] != CANARY_BYTE) {
            fprintf(stderr,
                    "FUZZ FAIL: %s wrote past its output struct "
                    "(post-canary byte %zu corrupted, call #%lu)\n",
                    decoder_name, i, call_idx);
            exit(1);
        }
    }
}

/* --------------------------------------------------------------------- */
/* One adapter per decoder: identical (payload, len) -> void call shape so
 * they can share the generic corpus+random driver below. Each also carries
 * a "build a valid-looking frame" function (via the matching _encode()/
 * _pack(), a real codec call, not hand-rolled bytes) used for the
 * bit-flip/truncate corpus; a handful of decoders that carry no meaningful
 * payload (fixed 1-byte "cmd only" frames) report 0 for "no interesting
 * valid-frame corpus beyond the length sweep already covering them". */

typedef int (*decode_fn_t)(const uint8_t *payload, size_t len); /* returns 0 iff decoder reported *_OK */
typedef size_t (*build_valid_fn_t)(uint8_t *out, size_t out_cap);

#define DECL_ADAPTER(NAME, MAXLEN)                                                              \
    static guarded_out_t g_##NAME;                                                              \
    static unsigned long g_##NAME##_calls;                                                      \
    enum { NAME##_MAX_LEN = (MAXLEN) }

DECL_ADAPTER(context, KILNLINK_CONTEXT_MAX_LEN + 32);
DECL_ADAPTER(status, KILNLINK_STATUS_LEN_V2 + 32);
DECL_ADAPTER(power, KILNLINK_POWER_LEN + 32);
DECL_ADAPTER(announce, KILNLINK_ANNOUNCE_MAX_LEN + 32);
DECL_ADAPTER(diag, KILNLINK_DIAG_LEN_V3 + 32);
DECL_ADAPTER(trip, KILNLINK_TRIP_LEN + 32);
DECL_ADAPTER(ceiling, KILNLINK_CEILING_LEN + 32);
DECL_ADAPTER(clear_trip, KILNLINK_CLEAR_TRIP_LEN_V3 + 32);
DECL_ADAPTER(test_trip, KILNLINK_TEST_TRIP_LEN + 32);
DECL_ADAPTER(test_trip_result, KILNLINK_TEST_TRIP_RESULT_LEN + 32);
DECL_ADAPTER(set_config, KILNLINK_SET_CONFIG_LEN + 32);
DECL_ADAPTER(rollback, KILNLINK_ROLLBACK_LEN + 32);
DECL_ADAPTER(get_fw_version, KILNLINK_GET_FW_VERSION_LEN + 32);
DECL_ADAPTER(set_clock, KILNLINK_SET_CLOCK_LEN + 32);
DECL_ADAPTER(announce_reboot, KILNLINK_ANNOUNCE_REBOOT_LEN + 32);
DECL_ADAPTER(set_ct_cal, KILNLINK_SET_CT_CAL_LEN + 32);
DECL_ADAPTER(get_ct_cal, KILNLINK_GET_CT_CAL_LEN + 32);
DECL_ADAPTER(ct_cal, KILNLINK_CT_CAL_LEN + 32);
DECL_ADAPTER(ct_auto_zero_begin, KILNLINK_CT_AUTO_ZERO_BEGIN_LEN + 32);
DECL_ADAPTER(get_ct_auto_zero, KILNLINK_GET_CT_AUTO_ZERO_LEN + 32);
DECL_ADAPTER(ct_auto_zero_status, KILNLINK_CT_AUTO_ZERO_STATUS_LEN + 32);
DECL_ADAPTER(set_log_level, KILNLINK_SET_LOG_LEVEL_LEN + 32);
DECL_ADAPTER(set_param, KILNLINK_SET_PARAM_MAX_LEN + 32);
DECL_ADAPTER(commit_config, KILNLINK_COMMIT_CONFIG_LEN + 32);
DECL_ADAPTER(commit_config_rejected, KILNLINK_COMMIT_CONFIG_REJECTED_LEN + 32);
DECL_ADAPTER(apply_config_volatile, KILNLINK_APPLY_CONFIG_VOLATILE_LEN + 32);
DECL_ADAPTER(inject_tc, KILNLINK_INJECT_TC_LEN + 32);
DECL_ADAPTER(get_param, KILNLINK_GET_PARAM_LEN + 32);
DECL_ADAPTER(param, KILNLINK_PARAM_MAX_LEN + 32);
DECL_ADAPTER(get_config_page, KILNLINK_GET_CONFIG_PAGE_LEN + 32);
DECL_ADAPTER(config_page, KILNLINK_CONFIG_PAGE_HDR_LEN +
                               KILNLINK_CONFIG_PAGE_MAX_ENTRIES * KILNLINK_CONFIG_PAGE_ENTRY_MAX_LEN + 32);
DECL_ADAPTER(rollback_result, KILNLINK_ROLLBACK_RESULT_LEN + 32);
DECL_ADAPTER(reboot, KILNLINK_REBOOT_LEN + 32);
DECL_ADAPTER(reboot_result, KILNLINK_REBOOT_RESULT_LEN + 32);
DECL_ADAPTER(fw_version, KILNLINK_FW_VERSION_MAX_LEN + 32);
DECL_ADAPTER(get_stack_margin, KILNLINK_GET_STACK_MARGIN_LEN + 32);
DECL_ADAPTER(stack_margin, KILNLINK_STACK_MARGIN_LEN + 32);

static int decode_context(const uint8_t *p, size_t len)
{
    arm_canaries(&g_context);
    int rc = (int)kilnlink_context_decode(p, len, &g_context.out.context);
    check_canaries(&g_context, "kilnlink_context_decode", ++g_context_calls);
    return rc;
}
static int decode_status(const uint8_t *p, size_t len)
{
    arm_canaries(&g_status);
    int rc = (int)kilnlink_status_decode(p, len, &g_status.out.status);
    check_canaries(&g_status, "kilnlink_status_decode", ++g_status_calls);
    return rc;
}
static int decode_power(const uint8_t *p, size_t len)
{
    arm_canaries(&g_power);
    int rc = (int)kilnlink_power_decode(p, len, &g_power.out.power);
    check_canaries(&g_power, "kilnlink_power_decode", ++g_power_calls);
    return rc;
}
static int decode_announce(const uint8_t *p, size_t len)
{
    arm_canaries(&g_announce);
    int rc = (int)kilnlink_announce_decode(p, len, &g_announce.out.announce);
    check_canaries(&g_announce, "kilnlink_announce_decode", ++g_announce_calls);
    return rc;
}
static int decode_diag(const uint8_t *p, size_t len)
{
    arm_canaries(&g_diag);
    int rc = (int)kilnlink_diag_decode(p, len, &g_diag.out.diag);
    check_canaries(&g_diag, "kilnlink_diag_decode", ++g_diag_calls);
    return rc;
}
static int decode_trip(const uint8_t *p, size_t len)
{
    arm_canaries(&g_trip);
    int rc = (int)kilnlink_trip_decode(p, len, &g_trip.out.trip);
    check_canaries(&g_trip, "kilnlink_trip_decode", ++g_trip_calls);
    return rc;
}
static int decode_ceiling(const uint8_t *p, size_t len)
{
    arm_canaries(&g_ceiling);
    int rc = (int)kilnlink_ceiling_decode(p, len, &g_ceiling.out.ceiling);
    check_canaries(&g_ceiling, "kilnlink_ceiling_decode", ++g_ceiling_calls);
    return rc;
}
static int decode_clear_trip(const uint8_t *p, size_t len)
{
    arm_canaries(&g_clear_trip);
    int rc = (int)kilnlink_clear_trip_decode(p, len, &g_clear_trip.out.clear_trip);
    check_canaries(&g_clear_trip, "kilnlink_clear_trip_decode", ++g_clear_trip_calls);
    return rc;
}
static int decode_test_trip(const uint8_t *p, size_t len)
{
    arm_canaries(&g_test_trip);
    int rc = (int)kilnlink_test_trip_decode(p, len, &g_test_trip.out.test_trip);
    check_canaries(&g_test_trip, "kilnlink_test_trip_decode", ++g_test_trip_calls);
    return rc;
}
static int decode_test_trip_result(const uint8_t *p, size_t len)
{
    arm_canaries(&g_test_trip_result);
    int rc = (int)kilnlink_test_trip_result_decode(p, len, &g_test_trip_result.out.test_trip_result);
    check_canaries(&g_test_trip_result, "kilnlink_test_trip_result_decode", ++g_test_trip_result_calls);
    return rc;
}
static int decode_set_config(const uint8_t *p, size_t len)
{
    arm_canaries(&g_set_config);
    int rc = (int)kilnlink_set_config_decode(p, len, &g_set_config.out.set_config);
    check_canaries(&g_set_config, "kilnlink_set_config_decode", ++g_set_config_calls);
    return rc;
}
static int decode_rollback(const uint8_t *p, size_t len)
{
    arm_canaries(&g_rollback);
    int rc = (int)kilnlink_rollback_decode(p, len, &g_rollback.out.rollback);
    check_canaries(&g_rollback, "kilnlink_rollback_decode", ++g_rollback_calls);
    return rc;
}
static int decode_get_fw_version(const uint8_t *p, size_t len)
{
    arm_canaries(&g_get_fw_version);
    int rc = (int)kilnlink_get_fw_version_decode(p, len, &g_get_fw_version.out.get_fw_version);
    check_canaries(&g_get_fw_version, "kilnlink_get_fw_version_decode", ++g_get_fw_version_calls);
    return rc;
}
static int decode_set_clock(const uint8_t *p, size_t len)
{
    arm_canaries(&g_set_clock);
    int rc = (int)kilnlink_set_clock_decode(p, len, &g_set_clock.out.set_clock);
    check_canaries(&g_set_clock, "kilnlink_set_clock_decode", ++g_set_clock_calls);
    return rc;
}
static int decode_announce_reboot(const uint8_t *p, size_t len)
{
    arm_canaries(&g_announce_reboot);
    int rc = (int)kilnlink_announce_reboot_decode(p, len, &g_announce_reboot.out.announce_reboot);
    check_canaries(&g_announce_reboot, "kilnlink_announce_reboot_decode", ++g_announce_reboot_calls);
    return rc;
}
static int decode_set_ct_cal(const uint8_t *p, size_t len)
{
    arm_canaries(&g_set_ct_cal);
    int rc = (int)kilnlink_set_ct_cal_decode(p, len, &g_set_ct_cal.out.set_ct_cal);
    check_canaries(&g_set_ct_cal, "kilnlink_set_ct_cal_decode", ++g_set_ct_cal_calls);
    return rc;
}
static int decode_get_ct_cal(const uint8_t *p, size_t len)
{
    arm_canaries(&g_get_ct_cal);
    int rc = (int)kilnlink_get_ct_cal_decode(p, len, &g_get_ct_cal.out.get_ct_cal);
    check_canaries(&g_get_ct_cal, "kilnlink_get_ct_cal_decode", ++g_get_ct_cal_calls);
    return rc;
}
static int decode_ct_cal(const uint8_t *p, size_t len)
{
    arm_canaries(&g_ct_cal);
    int rc = (int)kilnlink_ct_cal_decode(p, len, &g_ct_cal.out.ct_cal);
    check_canaries(&g_ct_cal, "kilnlink_ct_cal_decode", ++g_ct_cal_calls);
    return rc;
}
static int decode_ct_auto_zero_begin(const uint8_t *p, size_t len)
{
    arm_canaries(&g_ct_auto_zero_begin);
    int rc = (int)kilnlink_ct_auto_zero_begin_decode(p, len, &g_ct_auto_zero_begin.out.ct_auto_zero_begin);
    check_canaries(&g_ct_auto_zero_begin, "kilnlink_ct_auto_zero_begin_decode", ++g_ct_auto_zero_begin_calls);
    return rc;
}
static int decode_get_ct_auto_zero(const uint8_t *p, size_t len)
{
    arm_canaries(&g_get_ct_auto_zero);
    int rc = (int)kilnlink_get_ct_auto_zero_decode(p, len, &g_get_ct_auto_zero.out.get_ct_auto_zero);
    check_canaries(&g_get_ct_auto_zero, "kilnlink_get_ct_auto_zero_decode", ++g_get_ct_auto_zero_calls);
    return rc;
}
static int decode_ct_auto_zero_status(const uint8_t *p, size_t len)
{
    arm_canaries(&g_ct_auto_zero_status);
    int rc = (int)kilnlink_ct_auto_zero_status_decode(p, len, &g_ct_auto_zero_status.out.ct_auto_zero_status);
    check_canaries(&g_ct_auto_zero_status, "kilnlink_ct_auto_zero_status_decode", ++g_ct_auto_zero_status_calls);
    return rc;
}
static int decode_set_log_level(const uint8_t *p, size_t len)
{
    arm_canaries(&g_set_log_level);
    int rc = (int)kilnlink_set_log_level_decode(p, len, &g_set_log_level.out.set_log_level);
    check_canaries(&g_set_log_level, "kilnlink_set_log_level_decode", ++g_set_log_level_calls);
    return rc;
}
static int decode_set_param(const uint8_t *p, size_t len)
{
    arm_canaries(&g_set_param);
    int rc = (int)kilnlink_set_param_decode(p, len, &g_set_param.out.set_param);
    check_canaries(&g_set_param, "kilnlink_set_param_decode", ++g_set_param_calls);
    return rc;
}
static int decode_commit_config(const uint8_t *p, size_t len)
{
    arm_canaries(&g_commit_config);
    int rc = (int)kilnlink_commit_config_decode(p, len, &g_commit_config.out.commit_config);
    check_canaries(&g_commit_config, "kilnlink_commit_config_decode", ++g_commit_config_calls);
    return rc;
}
static int decode_apply_config_volatile(const uint8_t *p, size_t len)
{
    arm_canaries(&g_apply_config_volatile);
    int rc = (int)kilnlink_apply_config_volatile_decode(p, len, &g_apply_config_volatile.out.apply_config_volatile);
    check_canaries(&g_apply_config_volatile, "kilnlink_apply_config_volatile_decode",
                    ++g_apply_config_volatile_calls);
    return rc;
}
static int decode_commit_config_rejected(const uint8_t *p, size_t len)
{
    arm_canaries(&g_commit_config_rejected);
    int rc = (int)kilnlink_commit_config_rejected_decode(p, len, &g_commit_config_rejected.out.commit_config_rejected);
    check_canaries(&g_commit_config_rejected, "kilnlink_commit_config_rejected_decode",
                    ++g_commit_config_rejected_calls);
    return rc;
}
static int decode_inject_tc(const uint8_t *p, size_t len)
{
    arm_canaries(&g_inject_tc);
    int rc = (int)kilnlink_inject_tc_decode(p, len, &g_inject_tc.out.inject_tc);
    check_canaries(&g_inject_tc, "kilnlink_inject_tc_decode", ++g_inject_tc_calls);
    return rc;
}
static int decode_get_param(const uint8_t *p, size_t len)
{
    arm_canaries(&g_get_param);
    int rc = (int)kilnlink_get_param_decode(p, len, &g_get_param.out.get_param);
    check_canaries(&g_get_param, "kilnlink_get_param_decode", ++g_get_param_calls);
    return rc;
}
static int decode_param(const uint8_t *p, size_t len)
{
    arm_canaries(&g_param);
    int rc = (int)kilnlink_param_decode(p, len, &g_param.out.param);
    check_canaries(&g_param, "kilnlink_param_decode", ++g_param_calls);
    return rc;
}
static int decode_get_config_page(const uint8_t *p, size_t len)
{
    arm_canaries(&g_get_config_page);
    int rc = (int)kilnlink_get_config_page_decode(p, len, &g_get_config_page.out.get_config_page);
    check_canaries(&g_get_config_page, "kilnlink_get_config_page_decode", ++g_get_config_page_calls);
    return rc;
}
static int decode_config_page(const uint8_t *p, size_t len)
{
    arm_canaries(&g_config_page);
    int rc = (int)kilnlink_config_page_decode(p, len, &g_config_page.out.config_page);
    check_canaries(&g_config_page, "kilnlink_config_page_decode", ++g_config_page_calls);
    return rc;
}
static int decode_rollback_result(const uint8_t *p, size_t len)
{
    arm_canaries(&g_rollback_result);
    int rc = (int)kilnlink_rollback_result_decode(p, len, &g_rollback_result.out.rollback_result);
    check_canaries(&g_rollback_result, "kilnlink_rollback_result_decode", ++g_rollback_result_calls);
    return rc;
}
static int decode_reboot(const uint8_t *p, size_t len)
{
    arm_canaries(&g_reboot);
    int rc = (int)kilnlink_reboot_decode(p, len, &g_reboot.out.reboot);
    check_canaries(&g_reboot, "kilnlink_reboot_decode", ++g_reboot_calls);
    return rc;
}
static int decode_reboot_result(const uint8_t *p, size_t len)
{
    arm_canaries(&g_reboot_result);
    int rc = (int)kilnlink_reboot_result_decode(p, len, &g_reboot_result.out.reboot_result);
    check_canaries(&g_reboot_result, "kilnlink_reboot_result_decode", ++g_reboot_result_calls);
    return rc;
}
static int decode_fw_version(const uint8_t *p, size_t len)
{
    arm_canaries(&g_fw_version);
    int rc = (int)kilnlink_fw_version_decode(p, len, &g_fw_version.out.fw_version);
    check_canaries(&g_fw_version, "kilnlink_fw_version_decode", ++g_fw_version_calls);
    return rc;
}
static int decode_get_stack_margin(const uint8_t *p, size_t len)
{
    arm_canaries(&g_get_stack_margin);
    int rc = (int)kilnlink_get_stack_margin_decode(p, len, &g_get_stack_margin.out.get_stack_margin);
    check_canaries(&g_get_stack_margin, "kilnlink_get_stack_margin_decode", ++g_get_stack_margin_calls);
    return rc;
}
static int decode_stack_margin(const uint8_t *p, size_t len)
{
    arm_canaries(&g_stack_margin);
    int rc = (int)kilnlink_stack_margin_decode(p, len, &g_stack_margin.out.stack_margin);
    check_canaries(&g_stack_margin, "kilnlink_stack_margin_decode", ++g_stack_margin_calls);
    return rc;
}

/* --- build_valid_*: a real, codec-produced valid frame per decoder, used
 * for the bit-flip/truncate corpus. Field values are arbitrary but
 * in-range (count/index fields kept within their documented caps) --
 * "valid-looking", not domain-meaningful. Returns 0 (no corpus contributed)
 * for the handful of fixed 1-byte "cmd only" frames, where the length sweep
 * above already exercises every byte value at the only length that matters. */

static size_t build_valid_context(uint8_t *out, size_t out_cap)
{
    kilnlink_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.flags = KILNLINK_CONTEXT_FLAG_CONTEXT_VALID;
    ctx.boot_id = 3;
    ctx.seq = 12345;
    ctx.uptime_ms = 99999;
    ctx.relay_now_mask = 0x05;
    ctx.relay_recent_mask = 0x07;
    ctx.recent_window_s = 30;
    ctx.zone_count = KILNLINK_CONTEXT_MAX_ZONES;
    for (unsigned i = 0; i < KILNLINK_CONTEXT_MAX_ZONES; ++i) {
        ctx.zones[i].zone_index = (uint8_t)i;
        ctx.zones[i].flags = KILNLINK_ZONE_FLAG_MEASURED_VALID;
        ctx.zones[i].setpoint_c = 100.0f + (float)i;
        ctx.zones[i].measured_c = 98.0f + (float)i;
        ctx.zones[i].sample_counter = (uint8_t)i;
        ctx.zones[i].tc_type = 3;
        ctx.zones[i].tc_fault = 0;
    }
    kilnlink_context_status_t st;
    return kilnlink_context_encode(&ctx, out, out_cap, &st);
}
static size_t build_valid_status(uint8_t *out, size_t out_cap)
{
    kilnlink_status_t st_msg;
    memset(&st_msg, 0, sizeof(st_msg));
    st_msg.flags = 0x03;
    st_msg.safety_tc_c = 500.0f;
    st_msg.cold_junction_c = 25.0f;
    st_msg.tc_fault = 0;
    st_msg.current1_a = 1.5f;
    st_msg.current2_a = 2.5f;
    st_msg.current3_a = 3.5f;
    /* has_tx_dropped = 0 on purpose: this encodes the shorter, KILNLINK_
     * STATUS_LEN_V1 (23-byte) frame. The V2 (24-byte) frame would make
     * "truncated by one byte" collide with a second, independently valid
     * fixed length (both V1 and V2 are legitimate per kilnlink_status.h),
     * which is not the truncation property this corpus is testing. */
    st_msg.has_tx_dropped = 0;
    st_msg.tx_dropped_sat = 4;
    kilnlink_status_status_t st;
    return kilnlink_status_encode(&st_msg, out, out_cap, &st);
}
static size_t build_valid_power(uint8_t *out, size_t out_cap)
{
    kilnlink_power_t pw;
    memset(&pw, 0, sizeof(pw));
    pw.power_window_s = 120;
    pw.flags = 1;
    pw.mains_voltage_v = 240.0f;
    for (unsigned i = 0; i < KILNLINK_POWER_CHANNELS; ++i) {
        pw.i_conducting_a[i] = 10.0f;
        pw.conduction_fraction[i] = 0.5f;
        pw.p_avg_w[i] = 1200.0f;
    }
    pw.p_total_w = 3600.0f;
    pw.energy_wh = 42.0;
    kilnlink_power_status_t st;
    return kilnlink_power_encode(&pw, out, out_cap, &st);
}
static size_t build_valid_announce(uint8_t *out, size_t out_cap)
{
    kilnlink_announce_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.protocol_version = 7;
    msg.min_compatible = 5;
    msg.dirty = 0;
    msg.commit_len = 7;
    memcpy(msg.commit, "abc1234", 7);
    msg.datetime_len = 8;
    memcpy(msg.datetime, "20260904", 8);
    kilnlink_announce_status_t st;
    return kilnlink_announce_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_diag(uint8_t *out, size_t out_cap)
{
    kilnlink_diag_t dg;
    memset(&dg, 0, sizeof(dg));
    dg.trip_reason = 0;
    dg.warn_mask = 0x0001;
    dg.trip_mask = 0;
    dg.uptime_ms = 123456;
    dg.boot_reason = 1;
    dg.context_age_100ms = 3;
    dg.context_frames_ok = 1000;
    dg.context_frames_bad = 2;
    dg.tx_frames_dropped = 0;
    dg.state = 1;
    dg.flags = 0;
    /* Build the protocol 17 31-byte form so the corpus reaches the trip_seq
     * byte; the 30-byte form is the case's truncation_exception_len. */
    dg.has_trip_seq = true;
    dg.trip_seq = 0x5A;
    kilnlink_diag_status_t st;
    return kilnlink_diag_encode(&dg, out, out_cap, &st);
}
static size_t build_valid_trip(uint8_t *out, size_t out_cap)
{
    kilnlink_trip_t tr;
    memset(&tr, 0, sizeof(tr));
    tr.trip_seq = 1;
    tr.trip_reason = 2;
    tr.uptime_ms = 555;
    tr.safety_tc_c = 999.0f;
    tr.deciding_threshold = 950.0f;
    for (unsigned i = 0; i < KILNLINK_TRIP_CHANNELS; ++i) {
        tr.current_a[i] = 5.0f;
    }
    tr.relay_recent_mask = 0x03;
    tr.context_age_100ms = 2;
    kilnlink_trip_status_t st;
    return kilnlink_trip_encode(&tr, out, out_cap, &st);
}
static size_t build_valid_ceiling(uint8_t *out, size_t out_cap)
{
    kilnlink_ceiling_t msg;
    msg.firing_max_c = 1250.0f;
    kilnlink_ceiling_status_t st;
    return kilnlink_ceiling_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_clear_trip(uint8_t *out, size_t out_cap)
{
    kilnlink_clear_trip_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.trip_mask = 0x0004;
    /* Protocol 17 bound form (4 bytes); the 3-byte legacy form is the
     * case's truncation_exception_len. */
    msg.has_trip_seq = true;
    msg.trip_seq = 0x21;
    kilnlink_clear_trip_status_t st;
    return kilnlink_clear_trip_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_test_trip(uint8_t *out, size_t out_cap)
{
    kilnlink_test_trip_t msg;
    msg.pico_boot_id = 0x07;
    msg.request_id = 0x42;
    msg.magic = KILNLINK_TEST_TRIP_MAGIC;
    kilnlink_test_trip_status_t st;
    return kilnlink_test_trip_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_test_trip_result(uint8_t *out, size_t out_cap)
{
    kilnlink_test_trip_result_t msg;
    msg.request_id = 0x42;
    msg.outcome = KILNLINK_TEST_TRIP_OUTCOME_ACCEPTED;
    msg.trip_seq = 0x09;
    kilnlink_test_trip_result_status_t st;
    return kilnlink_test_trip_result_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_set_config(uint8_t *out, size_t out_cap)
{
    kilnlink_set_config_t msg;
    msg.tc_type = 3;
    kilnlink_set_config_status_t st;
    return kilnlink_set_config_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_set_clock(uint8_t *out, size_t out_cap)
{
    kilnlink_set_clock_t msg;
    msg.epoch_ms = 1700000000000ull;
    kilnlink_set_clock_status_t st;
    return kilnlink_set_clock_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_set_ct_cal(uint8_t *out, size_t out_cap)
{
    kilnlink_set_ct_cal_t msg;
    msg.channel = 1;
    msg.calibrated = 1;
    msg.gain = 1.02f;
    msg.offset = 0.01f;
    kilnlink_set_ct_cal_status_t st;
    return kilnlink_set_ct_cal_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_ct_auto_zero_begin(uint8_t *out, size_t out_cap)
{
    kilnlink_ct_auto_zero_begin_t msg = { .channel = 2 };
    kilnlink_ct_auto_zero_begin_status_t st;
    return kilnlink_ct_auto_zero_begin_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_ct_auto_zero_status(uint8_t *out, size_t out_cap)
{
    kilnlink_ct_auto_zero_status_t msg = {
        .state = KILNLINK_CT_AUTO_ZERO_STATE_DONE, .channel = 1,
        .samples_taken = 200, .samples_target = 200, .zero_counts = 61,
    };
    kilnlink_ct_auto_zero_status_codec_t st;
    return kilnlink_ct_auto_zero_status_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_ct_cal(uint8_t *out, size_t out_cap)
{
    kilnlink_ct_cal_t cal;
    memset(&cal, 0, sizeof(cal));
    for (unsigned i = 0; i < KILNLINK_CT_CAL_NUM_CHANNELS; ++i) {
        cal.channels[i].calibrated = 1;
        cal.channels[i].gain = 1.0f + (float)i * 0.01f;
        cal.channels[i].offset = 0.0f;
    }
    kilnlink_ct_cal_status_t st;
    return kilnlink_ct_cal_encode(&cal, out, out_cap, &st);
}
static size_t build_valid_set_log_level(uint8_t *out, size_t out_cap)
{
    kilnlink_set_log_level_t msg;
    msg.level = 2;
    kilnlink_set_log_level_status_t st;
    return kilnlink_set_log_level_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_set_param(uint8_t *out, size_t out_cap)
{
    kilnlink_set_param_t msg;
    msg.param_id = 7;
    msg.type = KILNLINK_PARAM_TYPE_F32;
    msg.value.f32_val = 1234.5f;
    kilnlink_set_param_status_t st;
    return kilnlink_set_param_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_commit_config_rejected(uint8_t *out, size_t out_cap)
{
    kilnlink_commit_config_rejected_t msg;
    msg.param_id = 9;
    msg.reason = 1;
    kilnlink_commit_config_rejected_status_t st;
    return kilnlink_commit_config_rejected_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_inject_tc(uint8_t *out, size_t out_cap)
{
    kilnlink_inject_tc_t msg;
    msg.valid = 1;
    msg.tc_c = 850.0f;
    msg.cj_c = 24.0f;
    msg.fault_bits = 0;
    kilnlink_inject_tc_status_t st;
    return kilnlink_inject_tc_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_get_param(uint8_t *out, size_t out_cap)
{
    kilnlink_get_param_t msg;
    msg.param_id = 11;
    kilnlink_get_param_status_t st;
    return kilnlink_get_param_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_param(uint8_t *out, size_t out_cap)
{
    kilnlink_param_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.param_id = 11;
    msg.found = 1;
    msg.type = KILNLINK_PARAM_TYPE_U16;
    msg.value.u16_val = 42;
    kilnlink_param_status_t st;
    return kilnlink_param_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_get_config_page(uint8_t *out, size_t out_cap)
{
    kilnlink_get_config_page_t msg;
    msg.page_index = 0;
    kilnlink_get_config_page_status_t st;
    return kilnlink_get_config_page_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_config_page(uint8_t *out, size_t out_cap)
{
    kilnlink_config_page_entry_t entries[3];
    memset(entries, 0, sizeof(entries));
    entries[0].param_id = 1;
    entries[0].type = KILNLINK_PARAM_TYPE_BOOL;
    entries[0].value.bool_val = 1;
    entries[0].set = true;
    entries[1].param_id = 2;
    entries[1].type = KILNLINK_PARAM_TYPE_U16;
    entries[1].value.u16_val = 500;
    entries[1].set = true;
    entries[2].param_id = 3;
    entries[2].type = KILNLINK_PARAM_TYPE_F32;
    entries[2].value.f32_val = 12.5f;
    entries[2].set = false;
    size_t packed = 0;
    kilnlink_config_page_status_t st;
    return kilnlink_config_page_pack(0, entries, 3, out, out_cap, &packed, &st);
}
static size_t build_valid_rollback_result(uint8_t *out, size_t out_cap)
{
    kilnlink_rollback_result_t msg;
    msg.accepted = 0;
    msg.reason = 2;
    kilnlink_rollback_result_status_t st;
    return kilnlink_rollback_result_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_reboot(uint8_t *out, size_t out_cap)
{
    kilnlink_reboot_status_t st;
    return kilnlink_reboot_encode(NULL, out, out_cap, &st);
}
static size_t build_valid_reboot_result(uint8_t *out, size_t out_cap)
{
    kilnlink_reboot_result_t msg;
    msg.accepted = 1;
    msg.reason = KILNLINK_REBOOT_RESULT_REASON_NONE;
    kilnlink_reboot_result_status_t st;
    return kilnlink_reboot_result_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_fw_version(uint8_t *out, size_t out_cap)
{
    kilnlink_fw_version_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.protocol_version = 7;
    msg.min_compatible = 5;
    msg.dirty = 1;
    msg.commit_len = 7;
    memcpy(msg.commit, "deadbee", 7);
    msg.datetime_len = 8;
    memcpy(msg.datetime, "20260904", 8);
    msg.boot_id = 4;
    msg.config_version = 2;
    msg.config_crc = 0xBEEF;
    kilnlink_fw_version_status_t st;
    return kilnlink_fw_version_encode(&msg, out, out_cap, &st);
}
static size_t build_valid_stack_margin(uint8_t *out, size_t out_cap)
{
    kilnlink_stack_margin_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.rounds_completed = 3;
    for (unsigned i = 0; i < KILNLINK_STACK_MARGIN_NUM_TASKS; ++i) {
        msg.entries[i].task_id = (uint8_t)i;
        msg.entries[i].high_water_words = (uint16_t)(100 + i * 37);
        msg.entries[i].stack_total_words = (uint16_t)(256 + i * 128);
    }
    kilnlink_stack_margin_status_t st;
    return kilnlink_stack_margin_encode(&msg, out, out_cap, &st);
}
/* Fixed 1-byte "cmd only" frames -- no variable structure for a bit-flip/
 * truncate corpus to add beyond the length sweep. */
static size_t build_valid_none(uint8_t *out, size_t out_cap)
{
    (void)out;
    (void)out_cap;
    return 0;
}

/* --------------------------------------------------------------------- */

typedef struct {
    const char *name;
    decode_fn_t decode;
    build_valid_fn_t build_valid;
    size_t max_len; /* upper bound for the length sweep */
    /* 0 (the common case) means "no exception". Nonzero names a length that
     * the truncation loop must NOT treat as "must fail": a length this
     * decoder documents as a second, independently valid fixed size for the
     * SAME command byte (kilnlink_power.h's KILNLINK_POWER_LEN_V1, 2026-09-06
     * -- build_valid_power() always encodes the current V2 length, so
     * truncating it by exactly the V1/V2 size difference lands on a frame
     * that is genuinely supposed to decode OK, not a misdecode). build_
     * valid_status() avoids needing this by choosing which length IT builds
     * (V1) instead -- that trick isn't available here because kilnlink_
     * power_encode() itself no longer has a V1-producing mode (see its own
     * doc comment), so the exception is expressed here instead. */
    size_t truncation_exception_len;
} decoder_case_t;

static const decoder_case_t k_cases[] = {
    {"kilnlink_context_decode", decode_context, build_valid_context, context_MAX_LEN, 0},
    {"kilnlink_status_decode", decode_status, build_valid_status, status_MAX_LEN, 0},
    {"kilnlink_power_decode", decode_power, build_valid_power, power_MAX_LEN, KILNLINK_POWER_LEN_V1},
    {"kilnlink_announce_decode", decode_announce, build_valid_announce, announce_MAX_LEN, 0},
    {"kilnlink_diag_decode", decode_diag, build_valid_diag, diag_MAX_LEN, KILNLINK_DIAG_LEN},
    {"kilnlink_trip_decode", decode_trip, build_valid_trip, trip_MAX_LEN, 0},
    {"kilnlink_ceiling_decode", decode_ceiling, build_valid_ceiling, ceiling_MAX_LEN, 0},
    {"kilnlink_clear_trip_decode", decode_clear_trip, build_valid_clear_trip, clear_trip_MAX_LEN,
     KILNLINK_CLEAR_TRIP_LEN},
    {"kilnlink_test_trip_decode", decode_test_trip, build_valid_test_trip, test_trip_MAX_LEN, 0},
    {"kilnlink_test_trip_result_decode", decode_test_trip_result, build_valid_test_trip_result,
     test_trip_result_MAX_LEN, 0},
    {"kilnlink_set_config_decode", decode_set_config, build_valid_set_config, set_config_MAX_LEN, 0},
    {"kilnlink_rollback_decode", decode_rollback, build_valid_none, rollback_MAX_LEN, 0},
    {"kilnlink_get_fw_version_decode", decode_get_fw_version, build_valid_none, get_fw_version_MAX_LEN, 0},
    {"kilnlink_set_clock_decode", decode_set_clock, build_valid_set_clock, set_clock_MAX_LEN, 0},
    {"kilnlink_announce_reboot_decode", decode_announce_reboot, build_valid_none, announce_reboot_MAX_LEN, 0},
    {"kilnlink_set_ct_cal_decode", decode_set_ct_cal, build_valid_set_ct_cal, set_ct_cal_MAX_LEN, 0},
    {"kilnlink_get_ct_cal_decode", decode_get_ct_cal, build_valid_none, get_ct_cal_MAX_LEN, 0},
    {"kilnlink_ct_auto_zero_begin_decode", decode_ct_auto_zero_begin, build_valid_ct_auto_zero_begin,
     ct_auto_zero_begin_MAX_LEN, 0},
    {"kilnlink_get_ct_auto_zero_decode", decode_get_ct_auto_zero, build_valid_none,
     get_ct_auto_zero_MAX_LEN, 0},
    {"kilnlink_ct_auto_zero_status_decode", decode_ct_auto_zero_status, build_valid_ct_auto_zero_status,
     ct_auto_zero_status_MAX_LEN, 0},
    {"kilnlink_ct_cal_decode", decode_ct_cal, build_valid_ct_cal, ct_cal_MAX_LEN, 0},
    {"kilnlink_set_log_level_decode", decode_set_log_level, build_valid_set_log_level, set_log_level_MAX_LEN, 0},
    {"kilnlink_set_param_decode", decode_set_param, build_valid_set_param, set_param_MAX_LEN, 0},
    {"kilnlink_commit_config_decode", decode_commit_config, build_valid_none, commit_config_MAX_LEN, 0},
    {"kilnlink_apply_config_volatile_decode", decode_apply_config_volatile, build_valid_none,
     apply_config_volatile_MAX_LEN, 0},
    {"kilnlink_commit_config_rejected_decode", decode_commit_config_rejected,
     build_valid_commit_config_rejected, commit_config_rejected_MAX_LEN, 0},
    {"kilnlink_inject_tc_decode", decode_inject_tc, build_valid_inject_tc, inject_tc_MAX_LEN, 0},
    {"kilnlink_get_param_decode", decode_get_param, build_valid_get_param, get_param_MAX_LEN, 0},
    {"kilnlink_param_decode", decode_param, build_valid_param, param_MAX_LEN, 0},
    {"kilnlink_get_config_page_decode", decode_get_config_page, build_valid_get_config_page,
     get_config_page_MAX_LEN, 0},
    {"kilnlink_config_page_decode", decode_config_page, build_valid_config_page, config_page_MAX_LEN, 0},
    {"kilnlink_rollback_result_decode", decode_rollback_result, build_valid_rollback_result,
     rollback_result_MAX_LEN, 0},
    {"kilnlink_reboot_decode", decode_reboot, build_valid_reboot, reboot_MAX_LEN, 0},
    {"kilnlink_reboot_result_decode", decode_reboot_result, build_valid_reboot_result,
     reboot_result_MAX_LEN, 0},
    {"kilnlink_fw_version_decode", decode_fw_version, build_valid_fw_version, fw_version_MAX_LEN, 0},
    {"kilnlink_get_stack_margin_decode", decode_get_stack_margin, build_valid_none, get_stack_margin_MAX_LEN, 0},
    {"kilnlink_stack_margin_decode", decode_stack_margin, build_valid_stack_margin, stack_margin_MAX_LEN, 0},
};

#define NUM_CASES (sizeof(k_cases) / sizeof(k_cases[0]))
#define FUZZ_SCRATCH_LEN 4096u

static unsigned long g_total_calls = 0;

static void run_length(const decoder_case_t *c, uint8_t *buf, size_t len, unsigned iters)
{
    for (unsigned i = 0; i < iters; ++i) {
        if (i % 2 == 0) {
            fill_random(buf, len);
        } else {
            fill_structured_random(buf, len);
        }
        c->decode(buf, len);
        g_total_calls++;
    }
}

static void run_corpus(const decoder_case_t *c, uint8_t *buf)
{
    /* Empty, 1 byte, all-zero / all-0xFF at a few interesting lengths.
     * The zero-length case is ASSERTED, not merely called: no payload
     * decoder may report OK for a frame that carried no bytes at all. This
     * is the one truncation assertion that applies to EVERY decoder,
     * including the fixed 1-byte "cmd only" ones whose build_valid()
     * returns 0 and so never reach the truncate corpus below -- without it
     * those five decoders (rollback/get_fw_version/announce_reboot/
     * get_ct_cal/commit_config) would be swept for crashes only, with
     * their decode STATUS never checked anywhere in this harness. */
    if (c->decode(buf, 0) == 0) {
        fprintf(stderr,
                "FUZZ FAIL: %s reported OK for a zero-length payload -- a frame that carried no "
                "bytes cannot have decoded to anything\n",
                c->name);
        exit(1);
    }
    g_total_calls++;

    buf[0] = 0x00;
    c->decode(buf, 1);
    g_total_calls++;
    buf[0] = 0xFF;
    c->decode(buf, 1);
    g_total_calls++;

    size_t interesting[] = {1, 2, 3, 4, 8, 16,
                             c->max_len > 8 ? c->max_len - 8 : 0,
                             c->max_len > 1 ? c->max_len - 1 : 0,
                             c->max_len, c->max_len + 1, c->max_len + 8};
    for (size_t k = 0; k < sizeof(interesting) / sizeof(interesting[0]); ++k) {
        size_t len = interesting[k];
        if (len == 0 || len > FUZZ_SCRATCH_LEN) {
            continue;
        }
        memset(buf, 0x00, len);
        c->decode(buf, len);
        g_total_calls++;
        memset(buf, 0xFF, len);
        c->decode(buf, len);
        g_total_calls++;
    }

    /* Real valid frame: bit-flip every byte, then truncate at every offset.
     * "Length lying in both directions" is exercised implicitly here too --
     * a truncated valid frame is exactly a length field (entry_count/
     * commit_len/zone_count/etc.) claiming more bytes than actually
     * arrived, and re-decoding the untruncated frame at len+1..len+8
     * (already covered by the `interesting` sweep with real trailing bytes
     * appended below) is a length field effectively claiming fewer. */
    uint8_t valid[FUZZ_SCRATCH_LEN];
    size_t vlen = c->build_valid(valid, sizeof(valid));
    if (vlen == 0 || vlen > FUZZ_SCRATCH_LEN) {
        return;
    }

    /* Sanity: the valid frame this harness built must itself decode OK --
     * otherwise the bit-flip/truncate corpus below is testing garbage, not
     * "one bit off from good". This is the harness's own self-check, not a
     * decoder property, so it aborts loudly (a harness bug) rather than
     * silently skipping the corpus. */
    memcpy(buf, valid, vlen);
    if (c->decode(buf, vlen) != 0) {
        fprintf(stderr,
                "FUZZ HARNESS BUG: %s's own build_valid() frame (%zu bytes) did not decode "
                "OK -- the harness's notion of a valid frame is wrong, fix build_valid before "
                "trusting the bit-flip/truncate corpus below\n",
                c->name, vlen);
        exit(1);
    }
    g_total_calls++;

    for (size_t pos = 0; pos < vlen; ++pos) {
        memcpy(buf, valid, vlen);
        buf[pos] ^= 0xFFu; /* flip every bit at this position */
        c->decode(buf, vlen);
        g_total_calls++;
    }

    /* THE property that matters most for the documented hazard ("a
     * malformed or truncated frame that passes CRC and misdecodes"): a
     * frame shorter than the valid one it was cut from must NEVER decode
     * OK. Fill the tail past trunc_len with a fixed poison pattern first
     * (rather than leaving whatever the previous iteration's bytes were)
     * so a decoder that reads past `len` sees a deterministic, obviously-
     * wrong byte rather than stale data that might accidentally still look
     * plausible -- and so this corpus is itself reproducible byte-for-byte
     * run to run. */
    for (size_t trunc_len = 0; trunc_len < vlen; ++trunc_len) {
        if (c->truncation_exception_len != 0 && trunc_len == c->truncation_exception_len) {
            /* Not a real truncation for this decoder -- see decoder_case_t's
             * own field comment (kilnlink_power's V1/V2 dual-length frame,
             * 2026-09-06). A decoder reporting OK here is documented,
             * correct behavior, not a misdecode. */
            continue;
        }
        memset(buf, 0xEE, vlen);
        memcpy(buf, valid, trunc_len);
        int rc = c->decode(buf, trunc_len);
        g_total_calls++;
        if (rc == 0) {
            fprintf(stderr,
                    "FUZZ FAIL: %s reported OK for a frame truncated to %zu of its real %zu "
                    "bytes -- it decoded past the end of what actually arrived, exactly the "
                    "silent-misdecode hazard this harness exists to catch\n",
                    c->name, trunc_len, vlen);
            exit(1);
        }
    }

    /* Length lying long: same valid bytes plus extra trailing garbage,
     * claiming a length the frame's own internal fields don't back up --
     * must also never decode OK (the frame's own length fields say vlen,
     * not vlen+8). */
    if (vlen + 8 <= FUZZ_SCRATCH_LEN) {
        memcpy(buf, valid, vlen);
        memset(buf + vlen, 0x00, 8);
        if (c->decode(buf, vlen + 8) == 0) {
            fprintf(stderr,
                    "FUZZ FAIL: %s reported OK for a valid %zu-byte frame plus 8 trailing "
                    "zero bytes it did not account for\n",
                    c->name, vlen);
            exit(1);
        }
        g_total_calls++;
        memset(buf + vlen, 0xFF, 8);
        if (c->decode(buf, vlen + 8) == 0) {
            fprintf(stderr,
                    "FUZZ FAIL: %s reported OK for a valid %zu-byte frame plus 8 trailing "
                    "0xFF bytes it did not account for\n",
                    c->name, vlen);
            exit(1);
        }
        g_total_calls++;
    }
}

int main(void)
{
    const char *seed_env = getenv("KILNLINK_FUZZ_SEED");
    if (seed_env != NULL) {
        g_rng_state = (uint32_t)strtoul(seed_env, NULL, 10);
        if (g_rng_state == 0) {
            g_rng_state = 0x9E3779B9u; /* xorshift32 can't start at 0 */
        }
    }
    unsigned iters_per_len = 8;
    const char *iters_env = getenv("KILNLINK_FUZZ_ITERS");
    if (iters_env != NULL) {
        unsigned long v = strtoul(iters_env, NULL, 10);
        if (v > 0 && v < 1000000ul) {
            iters_per_len = (unsigned)v;
        }
    }

    printf("kilnlink payload-decoder fuzz: seed=%lu iters_per_len=%u (override with "
           "KILNLINK_FUZZ_SEED / KILNLINK_FUZZ_ITERS)\n",
           (unsigned long)g_rng_state, iters_per_len);

    static uint8_t buf[FUZZ_SCRATCH_LEN];

    for (size_t ci = 0; ci < NUM_CASES; ++ci) {
        const decoder_case_t *c = &k_cases[ci];
        run_corpus(c, buf);
        size_t sweep_max = c->max_len;
        if (sweep_max > FUZZ_SCRATCH_LEN) {
            sweep_max = FUZZ_SCRATCH_LEN;
        }
        for (size_t len = 0; len <= sweep_max; ++len) {
            run_length(c, buf, len, iters_per_len);
        }
        printf("  %-42s ok (%zu bytes/frame ceiling)\n", c->name, c->max_len);
    }

    printf("fuzzed %lu calls across %zu payload decoders -- no crash, no hang, no canary "
           "corruption\n",
           g_total_calls, NUM_CASES);
    printf("ALL PASS\n");
    return 0;
}
