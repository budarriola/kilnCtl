// test_recovery_apply.c -- host test for recovery_apply.c (WP5 of
// docs/GITHUB_RELEASE_UPDATE_PLAN.md). The apply core runs against two
// in-memory NOR-flash models (`stage`, `app`; erase to 0xFF, a write into a
// non-erased byte is a violation) with a reference SHA-256, fault injection,
// and a power cut at every mutating step. Built and run by
// check_recovery_apply.ps1 (MSVC). Prints "RESULT pass=N fail=N"; exit code is
// the fail count.
#define _CRT_SECURE_NO_WARNINGS
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recovery_apply.h"
#include "recovery_image_check.h"
#include "stage_header.h"

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

// The shared stage_header.c calls ota_image_crc32(); on target that is the
// esp_rom wrapper, here the zlib CRC-32 recovery already carries.
uint32_t ota_image_crc32(const uint8_t *buf, size_t len)
{
    return ric_crc32(buf, len);
}

#define APP_SIZE (2u * 65536u)
#define STAGE_SIZE (4096u + APP_SIZE)
#define IMG_LEN 70000u // not a multiple of 4096: exercises the tail chunk

// ---- SHA-256 (reference implementation, FIPS 180-4) -----------------------
typedef struct {
    uint32_t h[8];
    uint8_t buf[64];
    uint32_t buflen;
    uint64_t total;
} sha_t;

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_init(sha_t *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv));
    s->buflen = 0;
    s->total = 0;
}

static void sha_block(sha_t *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_update(sha_t *s, const uint8_t *p, size_t n)
{
    s->total += n;
    while (n > 0) {
        size_t take = 64 - s->buflen;
        if (take > n) {
            take = n;
        }
        memcpy(s->buf + s->buflen, p, take);
        s->buflen += (uint32_t)take;
        p += take;
        n -= take;
        if (s->buflen == 64) {
            sha_block(s, s->buf);
            s->buflen = 0;
        }
    }
}

static void sha_final(sha_t *s, uint8_t out[32])
{
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->buflen != 56) {
        sha_update(s, &z, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    sha_update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

static void sha_oneshot(const uint8_t *p, size_t n, uint8_t out[32])
{
    sha_t s;
    sha_init(&s);
    sha_update(&s, p, n);
    sha_final(&s, out);
}


// ---- flash models + fault injection ----------------------------------------
typedef struct {
    uint8_t stage[STAGE_SIZE];
    uint8_t app[APP_SIZE];
    sha_t sha;
    bool sha_open;
    bool boot_set;     // otadata now selects `app`
    // Fault injection. Mutating ops are: stage_erase, app_erase, app_write,
    // set_boot (an otadata write), counted together. The Nth fails (1-based);
    // `partial` lets part of the op land first. `dead` freezes the board.
    unsigned fail_at;
    bool partial;
    bool dead;
    unsigned corrupt_write_at; // Nth app_write silently flips a bit, returns 0
    unsigned fail_read_at;     // Nth read (stage or app) fails
    bool fail_sha_start;
    bool fail_verify;
    // Counters.
    unsigned mut_ops, writes, reads, violations, bad_ranges;
    unsigned stage_erases, set_boots, verifies, aborts;
} fl_t;

static uint8_t g_image[IMG_LEN];
static const uint8_t g_old_app_marker = 0x3C;

static int mutation(fl_t *f)
{
    f->mut_ops++;
    if (f->fail_at && f->mut_ops == f->fail_at) {
        f->dead = true;
        return 1;
    }
    return 0;
}

static int rd_gate(fl_t *f)
{
    f->reads++;
    if (f->dead || (f->fail_read_at && f->reads == f->fail_read_at)) {
        return -1;
    }
    return 0;
}

static int do_erase(fl_t *f, uint8_t *mem, uint32_t size, uint32_t off, uint32_t len)
{
    if (f->dead) {
        return -1;
    }
    if ((off % 4096u) || (len % 4096u) || off + len > size) {
        f->bad_ranges++;
        return -1;
    }
    if (mutation(f)) {
        if (f->partial) { // power cut mid-erase: the first half is scribbled, never clean
            memset(mem + off, 0x5A, len / 2);
        }
        return -1;
    }
    memset(mem + off, 0xFF, len);
    return 0;
}

static int cb_stage_read(void *c, uint32_t off, void *buf, size_t len)
{
    fl_t *f = c;
    if (rd_gate(f) || off + len > STAGE_SIZE) {
        return -1;
    }
    memcpy(buf, f->stage + off, len);
    return 0;
}
static int cb_stage_erase(void *c, uint32_t off, uint32_t len)
{
    fl_t *f = c;
    f->stage_erases++;
    if (off != 0 || len != 4096u) { // the stage may only ever lose its header sector
        f->bad_ranges++;
        return -1;
    }
    return do_erase(f, f->stage, STAGE_SIZE, off, len);
}
static int cb_app_read(void *c, uint32_t off, void *buf, size_t len)
{
    fl_t *f = c;
    if (rd_gate(f) || off + len > APP_SIZE) {
        return -1;
    }
    memcpy(buf, f->app + off, len);
    return 0;
}
static int cb_app_erase(void *c, uint32_t off, uint32_t len)
{
    fl_t *f = c;
    return do_erase(f, f->app, APP_SIZE, off, len);
}
static int cb_app_write(void *c, uint32_t off, const void *buf, size_t len)
{
    fl_t *f = c;
    if (f->dead) {
        return -1;
    }
    if (off + len > APP_SIZE) {
        f->bad_ranges++;
        return -1;
    }
    f->writes++;
    size_t n = len;
    int rc = 0;
    if (mutation(f)) {
        if (!f->partial) {
            return -1;
        }
        n = len / 2;
        rc = -1;
    }
    const uint8_t *p = buf;
    for (size_t i = 0; i < n; i++) {
        if (f->app[off + i] != 0xFF) {
            f->violations++;
        }
        f->app[off + i] = p[i];
    }
    if (rc == 0 && f->corrupt_write_at && f->writes == f->corrupt_write_at) {
        f->app[off] ^= 0x01;
    }
    return rc;
}
static int cb_sha_start(void *c)
{
    fl_t *f = c;
    if (f->dead || f->fail_sha_start) {
        return -1;
    }
    sha_init(&f->sha);
    f->sha_open = true;
    return 0;
}
static int cb_sha_update(void *c, const void *b, size_t n)
{
    fl_t *f = c;
    if (!f->sha_open) {
        return -1;
    }
    sha_update(&f->sha, b, n);
    return 0;
}
static int cb_sha_finish(void *c, uint8_t out[32])
{
    fl_t *f = c;
    if (!f->sha_open) {
        return -1;
    }
    sha_final(&f->sha, out);
    f->sha_open = false;
    return 0;
}
static void cb_sha_abort(void *c)
{
    fl_t *f = c;
    f->sha_open = false;
    f->aborts++;
}
// esp_image_verify model: the image in `app` must be exactly the staged image.
static int cb_app_verify(void *c, uint32_t len)
{
    fl_t *f = c;
    f->verifies++;
    if (f->dead || f->fail_verify || len != IMG_LEN) {
        return -1;
    }
    return memcmp(f->app, g_image, IMG_LEN) == 0 ? 0 : -1;
}
static int cb_set_boot(void *c)
{
    fl_t *f = c;
    f->set_boots++;
    if (f->dead) {
        return -1;
    }
    if (mutation(f)) {
        if (f->partial) {
            f->boot_set = true; // the otadata write landed, the caller never learned
        }
        return -1;
    }
    f->boot_set = true;
    return 0;
}

static void io_init(recovery_apply_io_t *io, fl_t *f)
{
    memset(io, 0, sizeof(*io));
    io->ctx = f;
    io->stage_size = STAGE_SIZE;
    io->app_size = APP_SIZE;
    io->stage_read = cb_stage_read;
    io->stage_erase = cb_stage_erase;
    io->app_read = cb_app_read;
    io->app_erase = cb_app_erase;
    io->app_write = cb_app_write;
    io->sha_start = cb_sha_start;
    io->sha_update = cb_sha_update;
    io->sha_finish = cb_sha_finish;
    io->sha_abort = cb_sha_abort;
    io->app_verify = cb_app_verify;
    io->set_boot = cb_set_boot;
}

// ---- fixtures ----------------------------------------------------------------
static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// A plausible KilnCtrl ESP32-S3 image: valid first chunk, pseudo-random tail.
static void build_image(uint8_t *b, const char *project, unsigned chip)
{
    uint32_t x = 12345u;
    for (size_t i = 0; i < IMG_LEN; i++) {
        x = x * 1664525u + 1013904223u;
        b[i] = (uint8_t)(x >> 24);
    }
    memset(b, 0, 4096);
    b[0] = 0xE9;
    b[1] = 5;
    b[12] = (uint8_t)chip;
    b[13] = 0;
    put_u32(b + 24, 0x3C000000u);
    put_u32(b + 28, 0x1000u);
    put_u32(b + 32, 0xABCD5432u);
    strcpy((char *)b + 32 + 16, "v1.2.3");
    memset(b + 32 + 48, 0, 32);
    strcpy((char *)b + 32 + 48, project);
}

static void write_header(fl_t *f, stage_state_t st, const uint8_t *img, uint32_t len)
{
    stage_header_t h;
    memset(&h, 0, sizeof(h));
    h.state = st;
    h.image_length = len;
    sha_oneshot(img, len, h.sha256);
    strcpy(h.semver, "1.2.3");
    h.commit[0] = '\0';
    h.source = STAGE_SOURCE_GITHUB;
    uint8_t out[STAGE_HEADER_SIZE];
    stage_hdr_status_t s = stage_header_encode(&h, (STAGE_SIZE - 4096u) & ~4095u, out);
    CHECK(s == STAGE_HDR_OK, "fixture header encodes");
    memcpy(f->stage, out, sizeof(out));
}

// Fresh board: app holds an "old" image (all marker bytes), stage holds g_image
// with a VERIFIED header.
static void fresh(fl_t *f)
{
    memset(f, 0, sizeof(*f));
    memset(f->app, g_old_app_marker, APP_SIZE);
    memset(f->stage, 0xFF, STAGE_SIZE);
    memcpy(f->stage + 4096u, g_image, IMG_LEN);
    write_header(f, STAGE_STATE_VERIFIED, g_image, IMG_LEN);
}

static bool app_untouched(const fl_t *f)
{
    for (size_t i = 0; i < APP_SIZE; i++) {
        if (f->app[i] != g_old_app_marker) {
            return false;
        }
    }
    return true;
}
static bool stage_image_intact(const fl_t *f)
{
    return memcmp(f->stage + 4096u, g_image, IMG_LEN) == 0;
}
static bool stage_header_verified(const fl_t *f)
{
    stage_header_t h;
    return stage_header_decode(f->stage, STAGE_HEADER_SIZE, (STAGE_SIZE - 4096u) & ~4095u, &h) == STAGE_HDR_OK &&
           stage_header_is_installable(&h) && h.image_length == IMG_LEN;
}
static bool stage_blank(const fl_t *f)
{
    for (size_t i = 0; i < 4096; i++) {
        if (f->stage[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static uint8_t g_scratch[RECOVERY_APPLY_CHUNK];

static recovery_apply_result_t run(fl_t *f, recovery_apply_progress_t *p)
{
    recovery_apply_io_t io;
    io_init(&io, f);
    return recovery_apply_run(&io, g_scratch, sizeof(g_scratch), p);
}

// A rejected apply that never reached `app`.
static void expect_refused(const char *what, fl_t *f, recovery_apply_result_t want)
{
    recovery_apply_progress_t p;
    recovery_apply_result_t r = run(f, &p);
    char msg[128];
    snprintf(msg, sizeof(msg), "%s: result", what);
    CHECK(r == want && p.result == (int)want && p.phase == RECOVERY_APPLY_FAILED, msg);
    snprintf(msg, sizeof(msg), "%s: app untouched, not modified", what);
    CHECK(app_untouched(f) && !p.app_modified, msg);
    snprintf(msg, sizeof(msg), "%s: boot partition not changed", what);
    CHECK(!f->boot_set && f->set_boots == 0, msg);
    snprintf(msg, sizeof(msg), "%s: nothing erased/written", what);
    CHECK(f->mut_ops == 0 && f->violations == 0 && f->bad_ranges == 0, msg);
}

// A failure after `app` was touched: boot unchanged, stage intact and still installable.
static void expect_failed_after_copy(const char *what, fl_t *f, recovery_apply_result_t want)
{
    recovery_apply_progress_t p;
    recovery_apply_result_t r = run(f, &p);
    char msg[128];
    snprintf(msg, sizeof(msg), "%s: result", what);
    CHECK(r == want && p.phase == RECOVERY_APPLY_FAILED, msg);
    snprintf(msg, sizeof(msg), "%s: boot partition not changed", what);
    CHECK(!f->boot_set, msg);
    snprintf(msg, sizeof(msg), "%s: stage header and image intact", what);
    CHECK(stage_header_verified(f) && stage_image_intact(f), msg);
    snprintf(msg, sizeof(msg), "%s: flagged app_modified, no NOR violations", what);
    CHECK(p.app_modified && f->violations == 0 && f->bad_ranges == 0, msg);
}

static void test_happy(void)
{
    fl_t *f = malloc(sizeof(*f));
    fresh(f);
    recovery_apply_progress_t p;
    recovery_apply_result_t r = run(f, &p);
    CHECK(r == RECOVERY_APPLY_OK && p.phase == RECOVERY_APPLY_DONE && p.result == 0, "happy: ok/done");
    CHECK(memcmp(f->app, g_image, IMG_LEN) == 0, "happy: app holds the staged image");
    CHECK(f->boot_set && f->set_boots == 1, "happy: boot partition set exactly once");
    CHECK(p.stage_cleared && stage_blank(f) && f->stage_erases == 1, "happy: stage header erased after apply");
    CHECK(stage_image_intact(f), "happy: only the header sector was erased");
    CHECK(p.total_bytes == IMG_LEN && p.done_bytes == IMG_LEN && p.app_modified, "happy: progress counters");
    CHECK(f->violations == 0 && f->bad_ranges == 0 && !f->sha_open, "happy: clean NOR use, no open hash");
    CHECK(f->verifies == 1, "happy: full-image verify ran once");
    CHECK(f->app[IMG_LEN] == 0xFF && f->app[APP_SIZE - 1] == 0xFF, "happy: erased tail beyond the image");
    // Idempotence: a second apply finds nothing staged.
    recovery_apply_progress_t p2;
    CHECK(run(f, &p2) == RECOVERY_APPLY_ERR_NOT_STAGED, "happy: re-apply after success reports not_staged");
    free(f);
}

static void test_refusals(void)
{
    fl_t *f = malloc(sizeof(*f));

    fresh(f);
    memset(f->stage, 0xFF, 4096);
    expect_refused("blank stage", f, RECOVERY_APPLY_ERR_NOT_STAGED);

    fresh(f);
    f->stage[100] ^= 0x01; // corrupt a header byte without fixing the CRC
    expect_refused("bad header crc", f, RECOVERY_APPLY_ERR_BAD_HEADER);

    fresh(f);
    write_header(f, STAGE_STATE_WRITING, g_image, IMG_LEN);
    expect_refused("state WRITING", f, RECOVERY_APPLY_ERR_NOT_VERIFIED);

    fresh(f);
    write_header(f, STAGE_STATE_APPLYING, g_image, IMG_LEN);
    expect_refused("state APPLYING", f, RECOVERY_APPLY_ERR_NOT_VERIFIED);

    fresh(f);
    f->stage[4096u + 30000u] ^= 0x10; // image byte flipped after verification
    expect_refused("staged image bit flip", f, RECOVERY_APPLY_ERR_STAGE_HASH);

    fresh(f);
    f->stage[4096u + IMG_LEN - 1u] ^= 0x01; // last byte
    expect_refused("staged image last byte", f, RECOVERY_APPLY_ERR_STAGE_HASH);

    {
        static uint8_t other[IMG_LEN];
        fresh(f);
        build_image(other, "SomethingElse", 9);
        memcpy(f->stage + 4096u, other, IMG_LEN);
        write_header(f, STAGE_STATE_VERIFIED, other, IMG_LEN); // hash is right, identity is wrong
        expect_refused("wrong project", f, RECOVERY_APPLY_ERR_BAD_IMAGE);

        fresh(f);
        build_image(other, "KilnCtrl", 5); // ESP32 chip id, not S3
        memcpy(f->stage + 4096u, other, IMG_LEN);
        write_header(f, STAGE_STATE_VERIFIED, other, IMG_LEN);
        expect_refused("wrong chip", f, RECOVERY_APPLY_ERR_BAD_IMAGE);

        fresh(f);
        memset(other, 0xA5, IMG_LEN); // not an image at all
        memcpy(f->stage + 4096u, other, IMG_LEN);
        write_header(f, STAGE_STATE_VERIFIED, other, IMG_LEN);
        expect_refused("garbage image", f, RECOVERY_APPLY_ERR_BAD_IMAGE);
    }

    // Image larger than the app partition (stage capacity allows it).
    {
        fresh(f);
        recovery_apply_io_t io;
        io_init(&io, f);
        io.app_size = 65536u; // IMG_LEN 70000 > 64 KiB
        recovery_apply_progress_t p;
        CHECK(recovery_apply_run(&io, g_scratch, sizeof(g_scratch), &p) == RECOVERY_APPLY_ERR_TOO_BIG &&
                  f->mut_ops == 0 && !p.app_modified,
              "image larger than app: too_big, nothing touched");
    }

    // Argument handling.
    {
        recovery_apply_progress_t p;
        recovery_apply_io_t io;
        fresh(f);
        io_init(&io, f);
        CHECK(recovery_apply_run(NULL, g_scratch, sizeof(g_scratch), &p) == RECOVERY_APPLY_ERR_ARG, "NULL io refused");
        CHECK(recovery_apply_run(&io, g_scratch, RECOVERY_APPLY_CHUNK - 1, &p) == RECOVERY_APPLY_ERR_ARG,
              "short scratch refused");
        CHECK(recovery_apply_run(&io, NULL, sizeof(g_scratch), &p) == RECOVERY_APPLY_ERR_ARG, "NULL scratch refused");
        CHECK(recovery_apply_run(&io, g_scratch, sizeof(g_scratch), NULL) == RECOVERY_APPLY_ERR_ARG,
              "NULL progress refused");
        io.app_size = 65536u + 4096u;
        CHECK(recovery_apply_run(&io, g_scratch, sizeof(g_scratch), &p) == RECOVERY_APPLY_ERR_ARG,
              "app size not a multiple of the erase block refused");
        io_init(&io, f);
        io.set_boot = NULL;
        CHECK(recovery_apply_run(&io, g_scratch, sizeof(g_scratch), &p) == RECOVERY_APPLY_ERR_ARG,
              "missing set_boot callback refused");
        CHECK(f->mut_ops == 0 && app_untouched(f), "argument refusals touch nothing");
    }

    // Read / sha failures before the copy leave app alone.
    fresh(f);
    f->fail_read_at = 1; // the header read
    expect_refused("header read fails", f, RECOVERY_APPLY_ERR_STAGE_READ);
    fresh(f);
    f->fail_read_at = 5; // mid image hash
    expect_refused("stage hash read fails", f, RECOVERY_APPLY_ERR_STAGE_READ);
    CHECK(f->aborts == 1, "failed hash read aborts the open hash");
    fresh(f);
    f->fail_sha_start = true;
    expect_refused("sha_start fails", f, RECOVERY_APPLY_ERR_SHA);
    free(f);
}

static void test_failures_after_copy_started(void)
{
    fl_t *f = malloc(sizeof(*f));

    fresh(f);
    f->fail_at = 1; // first mutating op is the first app erase
    expect_failed_after_copy("first erase fails", f, RECOVERY_APPLY_ERR_ERASE);

    fresh(f);
    f->fail_at = 2; // first write
    expect_failed_after_copy("first write fails", f, RECOVERY_APPLY_ERR_WRITE);

    fresh(f);
    f->fail_at = 10;
    f->partial = true;
    expect_failed_after_copy("mid write fails with a partial landing", f, RECOVERY_APPLY_ERR_WRITE);

    fresh(f);
    f->corrupt_write_at = 7; // a write that returns success but stored a flipped bit
    expect_failed_after_copy("silently corrupt write", f, RECOVERY_APPLY_ERR_APP_HASH);
    CHECK(f->verifies == 0, "readback mismatch stops before the full-image verify");

    // Reads: header (1), stage hash (18), first chunk (1), copy (18), then app readback.
    fresh(f);
    f->fail_read_at = 1 + 18 + 1 + 18 + 4;
    expect_failed_after_copy("app readback read fails", f, RECOVERY_APPLY_ERR_APP_READ);

    fresh(f);
    f->fail_verify = true;
    expect_failed_after_copy("esp_image_verify rejects the copy", f, RECOVERY_APPLY_ERR_APP_VERIFY);

    // set_boot failing: the last two mutations are set_boot then stage_erase.
    {
        fl_t *c = malloc(sizeof(*c));
        fresh(c);
        recovery_apply_progress_t p;
        run(c, &p);
        unsigned total = c->mut_ops;
        free(c);
        fresh(f);
        f->fail_at = total - 1; // set_boot
        expect_failed_after_copy("set_boot fails", f, RECOVERY_APPLY_ERR_SET_BOOT);
        CHECK(f->set_boots == 1, "set_boot failure is not retried inside one apply");

        // The stage erase failing after a successful set_boot is not an apply failure.
        fresh(f);
        f->fail_at = total; // stage_erase
        recovery_apply_progress_t q;
        recovery_apply_result_t r = run(f, &q);
        CHECK(r == RECOVERY_APPLY_OK && q.phase == RECOVERY_APPLY_DONE && !q.stage_cleared,
              "stage erase failure after set_boot: still ok, reported as not cleared");
        CHECK(f->boot_set && memcmp(f->app, g_image, IMG_LEN) == 0, "stage erase failure: app good and selected");
    }
    free(f);
}

// Order invariant with a recording of every relevant op: the full-image verify
// after the last write, set_boot after the verify, the stage header erase after
// set_boot and last of all.
static unsigned g_seq_stage_erase_at, g_seq_set_boot_at, g_seq_last_write_at, g_seq_verify_at, g_seq_n;
static int seq_set_boot(void *c)
{
    g_seq_set_boot_at = ++g_seq_n;
    return cb_set_boot(c);
}
static int seq_stage_erase(void *c, uint32_t o, uint32_t l)
{
    g_seq_stage_erase_at = ++g_seq_n;
    return cb_stage_erase(c, o, l);
}
static int seq_app_write(void *c, uint32_t o, const void *b, size_t l)
{
    g_seq_last_write_at = ++g_seq_n;
    return cb_app_write(c, o, b, l);
}
static int seq_verify(void *c, uint32_t l)
{
    g_seq_verify_at = ++g_seq_n;
    return cb_app_verify(c, l);
}

static void test_order(void)
{
    fl_t *f = malloc(sizeof(*f));
    fresh(f);
    g_seq_n = g_seq_stage_erase_at = g_seq_set_boot_at = g_seq_last_write_at = g_seq_verify_at = 0;
    recovery_apply_io_t io;
    io_init(&io, f);
    io.set_boot = seq_set_boot;
    io.stage_erase = seq_stage_erase;
    io.app_write = seq_app_write;
    io.app_verify = seq_verify;
    recovery_apply_progress_t p;
    CHECK(recovery_apply_run(&io, g_scratch, sizeof(g_scratch), &p) == RECOVERY_APPLY_OK, "order: run ok");
    CHECK(g_seq_last_write_at < g_seq_verify_at, "order: full-image verify after the last write");
    CHECK(g_seq_verify_at < g_seq_set_boot_at, "order: set_boot only after the verify");
    CHECK(g_seq_set_boot_at < g_seq_stage_erase_at, "order: stage header erased only after set_boot");
    CHECK(g_seq_stage_erase_at == g_seq_n, "order: stage erase is the last operation");
    free(f);
}

// Power cut at every mutating step, both "nothing landed" and "half landed".
// Whatever the cut point: the board must be bootable (recovery is never touched
// and otadata selects app only once app is the complete verified image), the
// stage must stay installable until set_boot, and applying again must succeed.
static void test_power_cut_at_every_step(void)
{
    fl_t *f = malloc(sizeof(*f));
    fresh(f);
    recovery_apply_progress_t p;
    run(f, &p);
    const unsigned total = f->mut_ops;
    CHECK(total >= 20u, "sweep: apply has enough mutating steps to be a meaningful sweep");

    unsigned bad_boot = 0, bad_stage = 0, bad_retry = 0, bad_nor = 0, bad_result = 0, cuts_after_boot = 0;
    for (int partial = 0; partial < 2; partial++) {
        for (unsigned k = 1; k <= total; k++) {
            fresh(f);
            f->fail_at = k;
            f->partial = partial != 0;
            recovery_apply_result_t r = run(f, &p);
            if (r == RECOVERY_APPLY_OK && k != total) {
                bad_result++; // only a cut in the very last step (stage erase) may still report ok
            }
            if (f->violations || f->bad_ranges) {
                bad_nor++;
            }
            // Reboot. What would the bootloader do?
            if (f->boot_set) {
                cuts_after_boot++;
                if (memcmp(f->app, g_image, IMG_LEN) != 0) {
                    bad_boot++; // otadata selects an app that is not the complete image
                }
            } else {
                // Boots recovery. The stage must still be a complete installable one.
                if (!stage_header_verified(f) || !stage_image_intact(f)) {
                    bad_stage++;
                }
            }
            // Reboot done: dead flag cleared, boot_set carries over.
            f->dead = false;
            f->fail_at = 0;
            f->mut_ops = 0;
            f->sha_open = false;
            if (!f->boot_set) {
                recovery_apply_result_t rr = run(f, &p);
                if (rr != RECOVERY_APPLY_OK || memcmp(f->app, g_image, IMG_LEN) != 0 || !f->boot_set) {
                    bad_retry++;
                }
                if (f->violations || f->bad_ranges) {
                    bad_nor++;
                }
            }
        }
    }
    CHECK(bad_boot == 0, "sweep: app selected only when it is the complete image");
    CHECK(bad_stage == 0, "sweep: stage stays installable at every cut before set_boot");
    CHECK(bad_retry == 0, "sweep: apply after any cut completes the update");
    CHECK(bad_nor == 0, "sweep: no write into non-erased flash, no out-of-range op");
    CHECK(bad_result == 0, "sweep: a cut run never reports success");
    CHECK(cuts_after_boot >= 2u, "sweep: cuts at/after set_boot were exercised");
    free(f);
}

int main(void)
{
    build_image(g_image, "KilnCtrl", 9);
    test_happy();
    test_refusals();
    test_failures_after_copy_started();
    test_order();
    test_power_cut_at_every_step();
    printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    return g_fail;
}
