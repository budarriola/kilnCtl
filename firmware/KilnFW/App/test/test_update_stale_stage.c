// Host tests for App/drivers/update/update_stale_stage.c
// (docs/GITHUB_RELEASE_UPDATE_PLAN.md section 4, OT-G06): the decision that
// clears a stage which is the image already running. Flash is two byte arrays;
// the "hash" is a deterministic 32-byte fold (the decision only compares
// digests, so a real SHA-256 adds nothing here).
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/update/update_stale_stage.h"

#define ST_PART (4096u + 262144u) // header sector + 256 KB image area
#define ST_CAP 262144u
#define APP_PART 262144u
#define IMG_LEN 150000u

static uint8_t g_stage[ST_PART];
static uint8_t g_app[APP_PART];
static uint8_t g_scratch[4096];

typedef struct {
    int stage_reads, app_reads, sha_starts, sha_aborts, sha_finishes, clears, yields;
    int fail_stage_read, fail_app_read_at, fail_sha_finish, clear_rc;
    int claim_begins, claim_ends, claim_rc, claim_after_hash_finishes, mutate_in_claim;
    uint8_t acc[32];
    bool hashing;
} fake_t;
static fake_t g_f;

static const uint8_t ELF[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
                                 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };

static int f_stage_read(void *c, uint32_t off, void *d, size_t n)
{
    (void)c;
    g_f.stage_reads++;
    if (g_f.fail_stage_read || (size_t)off + n > sizeof(g_stage)) {
        return -1;
    }
    memcpy(d, g_stage + off, n);
    return 0;
}
static int f_app_read(void *c, uint32_t off, void *d, size_t n)
{
    (void)c;
    g_f.app_reads++;
    if ((g_f.fail_app_read_at && g_f.app_reads >= g_f.fail_app_read_at) || (size_t)off + n > sizeof(g_app)) {
        return -1;
    }
    memcpy(d, g_app + off, n);
    return 0;
}
static int f_sha_start(void *c)
{
    (void)c;
    g_f.sha_starts++;
    memset(g_f.acc, 0, sizeof(g_f.acc));
    g_f.hashing = true;
    return 0;
}
static void fold(uint8_t acc[32], const uint8_t *p, size_t n, uint32_t base)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t k = (base + (uint32_t)i);
        acc[k % 32] = (uint8_t)(acc[k % 32] * 31u + p[i] + (k & 0xFFu));
    }
}
static uint32_t g_fold_pos;
static int f_sha_update(void *c, const void *d, size_t n)
{
    (void)c;
    fold(g_f.acc, d, n, g_fold_pos);
    g_fold_pos += (uint32_t)n;
    return 0;
}
static int f_sha_finish(void *c, uint8_t out[32])
{
    (void)c;
    g_f.sha_finishes++;
    g_f.hashing = false;
    g_fold_pos = 0;
    if (g_f.fail_sha_finish) {
        return -1;
    }
    memcpy(out, g_f.acc, 32);
    return 0;
}
static void f_sha_abort(void *c)
{
    (void)c;
    g_f.sha_aborts++;
    g_f.hashing = false;
    g_fold_pos = 0;
}
static int f_claim_begin(void *c)
{
    (void)c;
    g_f.claim_begins++;
    g_f.claim_after_hash_finishes = g_f.sha_finishes;
    if (g_f.claim_rc != 0) {
        return g_f.claim_rc;
    }
    if (g_f.mutate_in_claim) {
        g_stage[200] ^= 0x01; // an upload touched the header between hash and claim
    }
    return 0;
}
static void f_claim_end(void *c)
{
    (void)c;
    g_f.claim_ends++;
}
static int f_clear(void *c)
{
    (void)c;
    g_f.clears++;
    if (g_f.clear_rc == 0) {
        memset(g_stage, 0xFF, STAGE_HEADER_SECTOR);
    }
    return g_f.clear_rc;
}
static void f_yield(void *c)
{
    (void)c;
    g_f.yields++;
}

static void image_digest(const uint8_t *img, uint32_t len, uint8_t out[32])
{
    uint8_t acc[32] = { 0 };
    for (uint32_t off = 0; off < len; off += 4096) {
        uint32_t n = len - off < 4096 ? len - off : 4096;
        fold(acc, img + off, n, off);
    }
    memcpy(out, acc, 32);
}

// Build a running app image in g_app and the matching VERIFIED stage; the
// stage image carries `stage_elf` in its descriptor.
static void make_world(const uint8_t stage_elf[32])
{
    memset(&g_f, 0, sizeof(g_f));
    g_fold_pos = 0;
    memset(g_stage, 0xFF, sizeof(g_stage));
    memset(g_app, 0xFF, sizeof(g_app));
    for (uint32_t i = 0; i < IMG_LEN; i++) {
        g_app[i] = (uint8_t)((i * 7u) ^ (i >> 8));
    }
    g_app[0] = 0xE9;
    g_app[32] = 0x32; // ESP_APP_DESC_MAGIC_WORD 0xABCD5432, little endian
    g_app[33] = 0x54;
    g_app[34] = 0xCD;
    g_app[35] = 0xAB;
    memcpy(g_app + 32 + UPDATE_STALE_APP_DESC_ELF_SHA_OFFSET, ELF, 32);

    // Stage: the same bytes, optionally with a different elf hash.
    memcpy(g_stage + STAGE_IMAGE_OFFSET, g_app, IMG_LEN);
    memcpy(g_stage + STAGE_IMAGE_OFFSET + 32 + UPDATE_STALE_APP_DESC_ELF_SHA_OFFSET, stage_elf, 32);

    stage_header_t h;
    memset(&h, 0, sizeof(h));
    h.state = STAGE_STATE_VERIFIED;
    h.image_length = IMG_LEN;
    image_digest(g_stage + STAGE_IMAGE_OFFSET, IMG_LEN, h.sha256);
    strcpy(h.semver, "1.2.3");
    h.source = STAGE_SOURCE_UPLOAD;
    uint8_t enc[STAGE_HEADER_SIZE];
    stage_hdr_status_t es = stage_header_encode(&h, ST_CAP, enc);
    TEST_CHECK(es == STAGE_HDR_OK, "fixture header encodes");
    memcpy(g_stage, enc, sizeof(enc));
}

static update_stale_io_t make_io(void)
{
    update_stale_io_t io;
    memset(&io, 0, sizeof(io));
    io.stage_size = ST_PART;
    io.app_size = APP_PART;
    memcpy(io.running_elf_sha256, ELF, 32);
    io.running_elf_valid = true;
    io.stage_read = f_stage_read;
    io.app_read = f_app_read;
    io.sha_start = f_sha_start;
    io.sha_update = f_sha_update;
    io.sha_finish = f_sha_finish;
    io.sha_abort = f_sha_abort;
    io.stage_clear = f_clear;
    io.claim_begin = f_claim_begin;
    io.claim_end = f_claim_end;
    io.yield = f_yield;
    return io;
}

static void test_match_clears(void)
{
    TEST_SECTION("stale stage: VERIFIED stage identical to the running image is cleared");
    make_world(ELF);
    update_stale_io_t io = make_io();
    update_stale_result_t r = update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch));
    TEST_CHECK(r == UPDATE_STALE_CLEARED, "cleared");
    TEST_CHECK(update_stale_result_cleared(r), "cleared predicate");
    TEST_CHECK(g_f.clears == 1, "cleared exactly once");
    TEST_CHECK(g_f.claim_begins == 1 && g_f.claim_ends == 1, "claim taken and released once");
    TEST_CHECK(g_f.claim_after_hash_finishes == 1, "claim taken only AFTER the hash finished");
    TEST_CHECK(g_f.sha_finishes == 1 && g_f.sha_aborts == 0, "one clean hash");
    TEST_CHECK(g_f.yields >= 2, "yielded during the 150 KB hash");
    TEST_CHECK(g_stage[0] == 0xFF && g_stage[100] == 0xFF, "header erased");
    // A second boot sees nothing staged.
    memset(&g_f, 0, sizeof(g_f));
    r = update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch));
    TEST_CHECK(r == UPDATE_STALE_KEEP_NO_STAGE && g_f.clears == 0, "second run: nothing staged");
}

static void test_mismatch_keeps(void)
{
    TEST_SECTION("stale stage: a different image is kept");
    uint8_t other[32];
    memcpy(other, ELF, 32);
    other[5] ^= 0x40;
    make_world(other);
    update_stale_io_t io = make_io();
    update_stale_result_t r = update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch));
    TEST_CHECK(r == UPDATE_STALE_KEEP_DIFFERENT, "different elf hash kept");
    TEST_CHECK(g_f.clears == 0 && g_f.app_reads == 0 && g_f.sha_starts == 0, "prefilter: no hash, no app read, no clear");
    TEST_CHECK(g_stage[0] == (STAGE_HEADER_MAGIC & 0xFF), "header intact");

    // Same elf hash in the descriptor but different bytes elsewhere: the full
    // hash must refuse, the elf hash alone never authorises the erase.
    make_world(ELF);
    g_app[90000] ^= 0x01;
    r = update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch));
    TEST_CHECK(r == UPDATE_STALE_KEEP_HASH_MISMATCH, "same elf, different bytes -> hash mismatch");
    TEST_CHECK(g_f.clears == 0, "not cleared on a hash mismatch");
    TEST_CHECK(g_f.sha_finishes == 1, "the full hash did run");

    // Descriptor magic wrong in the stage.
    make_world(ELF);
    g_stage[STAGE_IMAGE_OFFSET + 32] ^= 0xFF;
    r = update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch));
    TEST_CHECK(r == UPDATE_STALE_KEEP_DIFFERENT && g_f.clears == 0, "no app descriptor magic -> kept");
}

static void test_header_states_keep(void)
{
    TEST_SECTION("stale stage: only a VERIFIED, valid header is a candidate");
    update_stale_io_t io = make_io();

    make_world(ELF);
    memset(g_stage, 0xFF, STAGE_HEADER_SECTOR);
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_KEEP_NO_STAGE,
               "blank header");
    TEST_CHECK(g_f.clears == 0 && g_f.app_reads == 0, "blank: nothing else touched");

    make_world(ELF);
    g_stage[100] ^= 0x55; // corrupt a field; CRC no longer matches
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_KEEP_BAD_HEADER,
               "corrupt header");
    TEST_CHECK(g_f.clears == 0 && g_f.app_reads == 0, "corrupt: nothing else touched");

    // WRITING and APPLYING are valid headers but not installable.
    stage_state_t states[2] = { STAGE_STATE_WRITING, STAGE_STATE_APPLYING };
    for (int i = 0; i < 2; i++) {
        make_world(ELF);
        stage_header_t h;
        TEST_CHECK(stage_header_decode(g_stage, STAGE_HEADER_SIZE, ST_CAP, &h) == STAGE_HDR_OK, "fixture decodes");
        h.state = states[i];
        uint8_t enc[STAGE_HEADER_SIZE];
        TEST_CHECK(stage_header_encode(&h, ST_CAP, enc) == STAGE_HDR_OK, "re-encode");
        memcpy(g_stage, enc, sizeof(enc));
        update_stale_result_t r = update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch));
        TEST_CHECK(r == UPDATE_STALE_KEEP_NOT_VERIFIED, "non-VERIFIED state kept");
        TEST_CHECK(g_f.clears == 0 && g_f.app_reads == 0 && g_f.sha_starts == 0, "non-VERIFIED: no hash, no clear");
    }

    // image_length bigger than the running slot, and too small for a descriptor.
    make_world(ELF);
    io.app_size = IMG_LEN - 1;
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_KEEP_DIFFERENT,
               "image larger than the running slot kept");
    io.app_size = APP_PART;
    TEST_CHECK(g_f.clears == 0 && g_f.app_reads == 0, "oversize: nothing touched");
}

static void test_not_confirmed_touches_nothing(void)
{
    TEST_SECTION("stale stage: an unconfirmed (pending-verify) image never touches anything");
    make_world(ELF);
    update_stale_io_t io = make_io();
    update_stale_result_t r = update_stale_stage_run(&io, false, g_scratch, sizeof(g_scratch));
    TEST_CHECK(r == UPDATE_STALE_KEEP_NOT_CONFIRMED, "kept while not confirmed valid");
    TEST_CHECK(g_f.stage_reads == 0 && g_f.app_reads == 0 && g_f.sha_starts == 0 && g_f.clears == 0,
               "zero flash access, zero hash, zero clear");
    TEST_CHECK(g_stage[0] == (STAGE_HEADER_MAGIC & 0xFF), "header intact");

    io.running_elf_valid = false;
    r = update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch));
    TEST_CHECK(r == UPDATE_STALE_KEEP_NO_IDENTITY, "unknown running identity kept");
    TEST_CHECK(g_f.stage_reads == 0 && g_f.clears == 0, "unknown identity: nothing touched");
}

static void test_claim_phase(void)
{
    TEST_SECTION("stale stage: claim is taken after the hash and the header is re-proven");
    update_stale_io_t io = make_io();

    make_world(ELF);
    g_f.claim_rc = 1;
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_KEEP_BUSY,
               "refused claim -> kept busy");
    TEST_CHECK(g_f.clears == 0 && g_f.claim_ends == 0, "refused claim: no clear, no claim_end");
    TEST_CHECK(g_stage[0] == (STAGE_HEADER_MAGIC & 0xFF), "header intact");

    make_world(ELF);
    g_f.mutate_in_claim = 1;
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_KEEP_CHANGED,
               "header changed after the hash -> kept");
    TEST_CHECK(g_f.clears == 0, "changed header is never cleared");
    TEST_CHECK(g_f.claim_begins == 1 && g_f.claim_ends == 1, "claim released after a changed header");

    // The claim is never taken when the prefilter or the proof already refused.
    uint8_t other[32];
    memcpy(other, ELF, 32);
    other[0] ^= 1;
    make_world(other);
    (void)update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch));
    TEST_CHECK(g_f.claim_begins == 0, "different image: claim never taken");
    make_world(ELF);
    g_app[7] ^= 1;
    (void)update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch));
    TEST_CHECK(g_f.claim_begins == 0 && g_f.clears == 0, "hash mismatch: claim never taken");

    // Failed clear and busy clear still release the claim.
    make_world(ELF);
    g_f.clear_rc = -1;
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_ERR_CLEAR, "failed clear");
    TEST_CHECK(g_f.claim_ends == 1, "claim released after a failed clear");

    // Missing claim callbacks are an argument error, never a clear.
    make_world(ELF);
    io.claim_begin = NULL;
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_ERR_IO, "no claim callback refused");
    TEST_CHECK(g_f.clears == 0, "no clear without a claim callback");
}

static void test_failures_never_clear(void)
{
    TEST_SECTION("stale stage: I/O, hash and clear failures");
    update_stale_io_t io = make_io();

    make_world(ELF);
    g_f.fail_stage_read = 1;
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_ERR_IO, "header read fails");
    TEST_CHECK(g_f.clears == 0, "no clear after a failed header read");

    make_world(ELF);
    g_f.fail_app_read_at = 5;
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_ERR_IO, "app read fails mid-hash");
    TEST_CHECK(g_f.clears == 0 && g_f.sha_aborts == 1 && !g_f.hashing, "hash aborted, nothing cleared");

    make_world(ELF);
    g_f.fail_sha_finish = 1;
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_ERR_IO, "hash finish fails");
    TEST_CHECK(g_f.clears == 0 && g_f.sha_aborts == 1, "no clear after a failed hash");

    make_world(ELF);
    g_f.clear_rc = 1;
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_KEEP_BUSY, "busy clear -> retry later");
    TEST_CHECK(g_stage[0] == (STAGE_HEADER_MAGIC & 0xFF), "header intact after a busy clear");
    make_world(ELF);
    g_f.clear_rc = -1;
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_ERR_CLEAR, "failed clear reported");

    make_world(ELF);
    TEST_CHECK(update_stale_stage_run(&io, true, g_scratch, 16) == UPDATE_STALE_ERR_IO, "tiny scratch refused");
    TEST_CHECK(update_stale_stage_run(&io, true, NULL, 4096) == UPDATE_STALE_ERR_IO, "NULL scratch refused");
    TEST_CHECK(update_stale_stage_run(NULL, true, g_scratch, sizeof(g_scratch)) == UPDATE_STALE_ERR_IO, "NULL io refused");
    TEST_CHECK(g_f.clears == 0 && g_f.app_reads == 0, "argument errors touched nothing");

    for (int r = UPDATE_STALE_NOT_RUN; r <= UPDATE_STALE_ERR_CLEAR; r++) {
        TEST_CHECK(strcmp(update_stale_result_name((update_stale_result_t)r), "unknown") != 0, "every result has a name");
    }
}

void run_test_update_stale_stage(void)
{
    test_match_clears();
    test_mismatch_keeps();
    test_header_states_keep();
    test_not_confirmed_touches_nothing();
    test_claim_phase();
    test_failures_never_clear();
}
