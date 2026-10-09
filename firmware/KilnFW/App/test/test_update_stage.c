// Host tests for App/drivers/update/update_stage.c
// (docs/GITHUB_RELEASE_UPDATE_PLAN.md WP4). The stager core runs against an
// in-memory NOR-flash model (erase to 0xFF, writes may only clear bits, so a
// write into a non-erased byte is counted as a violation) and a real SHA-256
// implemented here (the host psa stub is not a real hash).
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

#include "../drivers/update/update_stage.h"
#include "../drivers/update/update_wr_arb.h"

#define PART_SIZE (4096u + 98304u) // header sector + 96 KB image area (1.5 erase units)
#define CAP 98304u

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

// ---- in-memory flash + fault injection -------------------------------------
typedef struct {
    uint8_t mem[PART_SIZE];
    // Fault injection: fail the Nth mutating op (1-based; 0 = never). A failing
    // write with partial=true first lands the first half of its bytes.
    unsigned fail_mut_at;
    bool partial;
    int fail_read_at; // fail the Nth read (1-based; 0 = never)
    int fail_sha_update_at; // fail the Nth sha_update (1-based; 0 = never)
    // Counters.
    unsigned mut_ops, erase_ops, write_ops, read_ops, sha_updates;
    unsigned violations; // writes into non-erased bytes
    unsigned lock_n, unlock_n, sha_starts, sha_aborts;
    uint32_t last_write_off, last_write_len;
    bool last_mut_was_write;
    sha_t sha;
    bool sha_open;
} fl_t;

static int fl_erase(void *c, uint32_t off, uint32_t len)
{
    fl_t *f = c;
    f->erase_ops++;
    f->mut_ops++;
    if (f->fail_mut_at && f->mut_ops == f->fail_mut_at) {
        return -1;
    }
    if ((off % 4096u) || (len % 4096u) || off + len > PART_SIZE) {
        f->violations++;
        return -1;
    }
    memset(f->mem + off, 0xFF, len);
    f->last_mut_was_write = false;
    return 0;
}

static void fl_program(fl_t *f, uint32_t off, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if ((f->mem[off + i] & d[i]) != d[i]) {
            f->violations++;
        }
        f->mem[off + i] &= d[i];
    }
}

static int fl_write(void *c, uint32_t off, const void *data, size_t len)
{
    fl_t *f = c;
    f->write_ops++;
    f->mut_ops++;
    if (off + len > PART_SIZE) {
        f->violations++;
        return -1;
    }
    if (f->fail_mut_at && f->mut_ops == f->fail_mut_at) {
        if (f->partial && len > 1) {
            fl_program(f, off, data, len / 2);
        }
        return -1;
    }
    fl_program(f, off, data, len);
    f->last_write_off = off;
    f->last_write_len = (uint32_t)len;
    f->last_mut_was_write = true;
    return 0;
}

static int fl_read(void *c, uint32_t off, void *data, size_t len)
{
    fl_t *f = c;
    f->read_ops++;
    if (f->fail_read_at && (int)f->read_ops == f->fail_read_at) {
        return -1;
    }
    if (off + len > PART_SIZE) {
        return -1;
    }
    memcpy(data, f->mem + off, len);
    return 0;
}

static int fl_sha_start(void *c)
{
    fl_t *f = c;
    f->sha_starts++;
    sha_init(&f->sha);
    f->sha_open = true;
    return 0;
}
static int fl_sha_update(void *c, const void *d, size_t n)
{
    fl_t *f = c;
    f->sha_updates++;
    if (f->fail_sha_update_at && (int)f->sha_updates == f->fail_sha_update_at) {
        return -1;
    }
    sha_update(&f->sha, d, n);
    return 0;
}
static int fl_sha_finish(void *c, uint8_t out[STAGE_SHA256_LEN])
{
    fl_t *f = c;
    sha_final(&f->sha, out);
    f->sha_open = false;
    return 0;
}
static void fl_sha_abort(void *c)
{
    fl_t *f = c;
    f->sha_aborts++;
    f->sha_open = false;
}
static void fl_lock(void *c) { ((fl_t *)c)->lock_n++; }
static void fl_unlock(void *c) { ((fl_t *)c)->unlock_n++; }

static fl_t g_fl;
static update_stage_t g_st;
static uint8_t g_scratch[512]; // deliberately small: exercises many re-read chunks

static void reset_board(void)
{
    memset(&g_fl, 0, sizeof(g_fl));
    memset(g_fl.mem, 0xFF, sizeof(g_fl.mem));
    update_stage_io_t io = {
        .ctx = &g_fl,
        .partition_size = PART_SIZE,
        .erase = fl_erase,
        .write = fl_write,
        .read = fl_read,
        .sha_start = fl_sha_start,
        .sha_update = fl_sha_update,
        .sha_finish = fl_sha_finish,
        .sha_abort = fl_sha_abort,
        .lock = fl_lock,
        .unlock = fl_unlock,
        .yield = NULL,
    };
    update_stage_init(&g_st, &io);
}

// A fresh stager over the SAME flash: what the board sees after a reboot
// (no cached verification).
static void reboot_stager(void)
{
    update_stage_io_t io = g_st.io;
    update_stage_init(&g_st, &io);
}

// ---- image builder ---------------------------------------------------------
static uint8_t g_img[CAP + 16];

static void make_image(size_t len, const char *app_version)
{
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < len; i++) {
        x = x * 1664525u + 1013904223u;
        g_img[i] = (uint8_t)(x >> 24);
    }
    g_img[0] = 0xE9; // ESP image magic
    g_img[12] = 9;   // chip id ESP32-S3 (little-endian u16)
    g_img[13] = 0;
    // esp_app_desc_t at offset 32: magic, secure_version, reserv1[2], version[32]
    uint32_t m = 0xABCD5432u;
    memcpy(g_img + 32, &m, 4);
    memset(g_img + 48, 0, 32);
    if (app_version) {
        memcpy(g_img + 48, app_version, strlen(app_version));
    }
    // project_name[32] at offset 80 (esp_app_desc_t +0x30): the stager requires "KilnCtrl".
    memset(g_img + 80, 0, 32);
    memcpy(g_img + 80, "KilnCtrl", 8);
}

static update_stage_err_t upload(size_t len, size_t chunk, const char *semver, const char *commit)
{
    update_stage_err_t e = update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), (uint32_t)len, semver, commit,
                                                      STAGE_SOURCE_UPLOAD);
    if (e != UPDATE_STAGE_OK) {
        return e;
    }
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        e = update_stage_upload_write(&g_st, g_img + off, n);
        if (e != UPDATE_STAGE_OK) {
            return e;
        }
    }
    return update_stage_upload_finish(&g_st);
}

static update_stage_info_t status(void)
{
    update_stage_info_t info;
    memset(&info, 0xAA, sizeof(info));
    (void)update_stage_get_status(&g_st, g_scratch, sizeof(g_scratch), &info);
    return info;
}

static bool is_staged(void)
{
    return status().staged;
}

// ---- tests -----------------------------------------------------------------
static void test_sha_reference(void)
{
    TEST_SECTION("update_stage -- reference SHA-256 sanity (so the other tests mean something)");
    uint8_t out[32];
    sha_oneshot((const uint8_t *)"abc", 3, out);
    TEST_CHECK(out[0] == 0xba && out[1] == 0x78 && out[2] == 0x16 && out[3] == 0xbf && out[31] == 0xad,
               "SHA-256(abc) matches FIPS vector");
}

static void test_happy_path(void)
{
    TEST_SECTION("update_stage -- upload, verify, report staged");
    reset_board();
    make_image(70000, "v1.2.3");
    TEST_CHECK(update_stage_capacity(&g_st) == CAP, "capacity = partition minus header sector");
    TEST_CHECK(!is_staged(), "blank flash is not staged");
    update_stage_info_t bi = status();
    TEST_CHECK(bi.hdr_status == STAGE_HDR_BLANK && strcmp(bi.reason, "blank") == 0, "blank flash reports reason blank");

    update_stage_err_t e = upload(70000, 777, NULL, NULL);
    TEST_CHECK(e == UPDATE_STAGE_OK, "upload of a valid image succeeds");
    TEST_CHECK(g_fl.violations == 0, "no write ever landed on a non-erased byte (erase-ahead is complete)");
    TEST_CHECK(g_fl.last_mut_was_write && g_fl.last_write_off == 0 && g_fl.last_write_len == STAGE_HEADER_SIZE,
               "the header is the LAST mutating operation");
    TEST_CHECK(!g_fl.sha_open, "no hash left open");
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE, "idle after finish");
    TEST_CHECK(g_fl.lock_n == g_fl.unlock_n && g_fl.lock_n > 0, "lock/unlock balanced");

    uint8_t want[32];
    sha_oneshot(g_img, 70000, want);
    TEST_CHECK(memcmp(g_fl.mem + STAGE_IMAGE_OFFSET, g_img, 70000) == 0, "image bytes are on flash at STAGE_IMAGE_OFFSET");

    update_stage_info_t i = status();
    TEST_CHECK(i.staged && i.hdr_status == STAGE_HDR_OK && i.reason[0] == '\0', "staged after a good upload");
    TEST_CHECK(i.image_length == 70000 && i.state == STAGE_STATE_VERIFIED, "length and VERIFIED state reported");
    TEST_CHECK(memcmp(i.sha256, want, 32) == 0, "header sha256 equals the independent hash of the image");
    TEST_CHECK(strcmp(i.semver, "1.2.3") == 0, "semver taken from the app descriptor, leading v stripped");
    TEST_CHECK(i.source == STAGE_SOURCE_UPLOAD && i.commit[0] == '\0', "source upload, no commit");

    reboot_stager();
    i = status();
    TEST_CHECK(i.staged, "still staged after a reboot (re-hash from flash matches)");
    TEST_CHECK(g_fl.read_ops > 100, "the post-reboot status really re-read the image through the small scratch");
    unsigned reads = g_fl.read_ops;
    TEST_CHECK(status().staged && g_fl.read_ops == reads + 1, "second status uses the cache (only the header read)");
}

static void test_semver_and_commit_args(void)
{
    TEST_SECTION("update_stage -- client supplied version and commit");
    reset_board();
    make_image(20000, "ignored-by-client-version");
    TEST_CHECK(upload(20000, 4096, "v2.0.1-rc.1", "0123456789abcdef0123456789abcdef01234567") == UPDATE_STAGE_OK,
               "explicit version and commit accepted");
    update_stage_info_t i = status();
    TEST_CHECK(i.staged && strcmp(i.semver, "2.0.1-rc.1") == 0, "client version wins, v stripped");
    TEST_CHECK(strcmp(i.commit, "0123456789abcdef0123456789abcdef01234567") == 0, "commit stored");

    reset_board();
    make_image(20000, "v1.0.0");
    unsigned before = g_fl.mut_ops;
    TEST_CHECK(upload(20000, 4096, "banana", NULL) == UPDATE_STAGE_ERR_BAD_VERSION, "unparsable client version refused");
    TEST_CHECK(g_fl.mut_ops == before && g_fl.read_ops == 0, "refused before any flash access");
    TEST_CHECK(upload(20000, 4096, "1.0.0", "ABCDEF") == UPDATE_STAGE_ERR_BAD_COMMIT, "short commit refused");
    TEST_CHECK(upload(20000, 4096, "1.0.0", "0123456789ABCDEF0123456789abcdef01234567") == UPDATE_STAGE_ERR_BAD_COMMIT,
               "uppercase commit refused");
    TEST_CHECK(g_fl.mut_ops == before, "bad commit refused before any flash access");
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE, "refusals leave the stager idle");

    // No client version and an app descriptor version that is not semver.
    make_image(20000, "not-a-version");
    TEST_CHECK(upload(20000, 4096, NULL, NULL) == UPDATE_STAGE_ERR_BAD_VERSION, "unusable image version refused");
    TEST_CHECK(!is_staged(), "nothing staged after the refusal");
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE && !g_fl.sha_open, "idle, hash closed");
}

static void test_size_limits(void)
{
    TEST_SECTION("update_stage -- empty, tiny and oversize uploads");
    reset_board();
    make_image(30000, "v1.0.0");
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_OK, "first image staged");
    unsigned muts = g_fl.mut_ops;
    unsigned reads = g_fl.read_ops;

    TEST_CHECK(update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), CAP + 1, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_ERR_OVERSIZE,
               "one byte over capacity refused");
    TEST_CHECK(update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 0xFFFFFFFFu, NULL, NULL,
                                         STAGE_SOURCE_UPLOAD) == UPDATE_STAGE_ERR_OVERSIZE,
               "4 GiB declared refused");
    TEST_CHECK(update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 0, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_ERR_EMPTY,
               "zero length refused");
    TEST_CHECK(update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 79, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_ERR_BAD_IMAGE,
               "shorter than the image head refused");
    TEST_CHECK(g_fl.mut_ops == muts && g_fl.read_ops == reads, "oversize/empty/tiny touched no flash at all");
    TEST_CHECK(is_staged(), "the previously staged image survived every refused begin");

    // Exactly capacity (not a multiple of the 64 KB erase unit) is accepted.
    reset_board();
    make_image(CAP, "v3.0.0");
    TEST_CHECK(upload(CAP, 4000, NULL, NULL) == UPDATE_STAGE_OK, "an image of exactly the capacity is accepted");
    TEST_CHECK(g_fl.violations == 0, "clamped final erase covers it with no violation");
    TEST_CHECK(is_staged(), "and reports staged");
    update_stage_info_t i = status();
    TEST_CHECK(i.image_length == CAP, "full-capacity length reported");
}

static void test_bad_images(void)
{
    TEST_SECTION("update_stage -- not an ESP32-S3 app image");
    reset_board();
    make_image(30000, "v1.0.0");
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_OK, "baseline staged");
    TEST_CHECK(is_staged(), "baseline staged");

    make_image(30000, "v1.0.0");
    g_img[0] = 0x00;
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_ERR_BAD_IMAGE, "wrong image magic refused");
    TEST_CHECK(is_staged(), "a refused upload leaves the previously staged image intact (header erased only after the checks)");
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE && !g_fl.sha_open, "idle and hash closed after a mid-upload refusal");
    TEST_CHECK(g_fl.mem[STAGE_IMAGE_OFFSET] == 0xFF || g_fl.mem[STAGE_IMAGE_OFFSET] == 0xE9,
               "no image byte was written for the refused head");

    make_image(30000, "v1.0.0");
    g_img[12] = 5; // ESP32-C3 chip id
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_ERR_BAD_IMAGE, "wrong chip id refused");
    make_image(30000, "v1.0.0");
    g_img[32] ^= 0xFF; // app descriptor magic
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_ERR_BAD_IMAGE, "missing app descriptor refused");
    TEST_CHECK(is_staged(), "still the baseline stage, untouched");
    // Head is held back until validated: a head split across many tiny writes behaves the same.
    make_image(30000, "v1.0.0");
    g_img[0] = 0x01;
    update_stage_err_t e = update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 30000, NULL, NULL, STAGE_SOURCE_UPLOAD);
    for (size_t off = 0; e == UPDATE_STAGE_OK && off < 30000; off += 7) {
        size_t n = 30000 - off < 7 ? 30000 - off : 7;
        e = update_stage_upload_write(&g_st, g_img + off, n);
    }
    TEST_CHECK(e == UPDATE_STAGE_ERR_BAD_IMAGE, "refusal also fires when the head arrives 7 bytes at a time");
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE, "idle again");
}

// Owner decision 2026-10-07: release signing removed; an image is accepted when it is for this project.
static void test_wrong_project_refused(void)
{
    TEST_SECTION("update_stage -- project identity (replaces release signing)");
    reset_board();
    make_image(30000, "v1.0.0");
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_OK, "KilnCtrl image staged");
    TEST_CHECK(is_staged(), "baseline staged");

    // Negative: another project's name is refused, nothing staged, no image byte written.
    make_image(30000, "v1.0.0");
    memset(g_img + 80, 0, 32);
    memcpy(g_img + 80, "OtherProject", 12);
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_ERR_WRONG_PROJECT, "wrong project_name refused");
    TEST_CHECK(is_staged(), "wrong-project upload leaves the previous stage intact");
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE && !g_fl.sha_open, "idle and hash closed after wrong-project refusal");
    TEST_CHECK(strcmp(update_stage_err_name(UPDATE_STAGE_ERR_WRONG_PROJECT), "wrong_project") == 0, "error name");

    // A prefix or an extension of the right name is not the right name.
    make_image(30000, "v1.0.0");
    memset(g_img + 80, 0, 32);
    memcpy(g_img + 80, "KilnCtr", 7);
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_ERR_WRONG_PROJECT, "prefix of the name refused");
    make_image(30000, "v1.0.0");
    memset(g_img + 80, 0, 32);
    memcpy(g_img + 80, "KilnCtrlX", 9);
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_ERR_WRONG_PROJECT, "extended name refused");
    make_image(30000, "v1.0.0");
    memset(g_img + 80, 0, 32); // empty name
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_ERR_WRONG_PROJECT, "empty name refused");

    // Same refusal when the head arrives in tiny writes (the fetch path streams like this).
    make_image(30000, "v1.0.0");
    memset(g_img + 80, 0, 32);
    memcpy(g_img + 80, "OtherProject", 12);
    update_stage_err_t e = update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 30000, NULL, NULL, STAGE_SOURCE_UPLOAD);
    for (size_t off = 0; e == UPDATE_STAGE_OK && off < 30000; off += 5) {
        size_t n = 30000 - off < 5 ? 30000 - off : 5;
        e = update_stage_upload_write(&g_st, g_img + off, n);
    }
    TEST_CHECK(e == UPDATE_STAGE_ERR_WRONG_PROJECT, "wrong project refused with a 5-byte-at-a-time head");

    // Restored: the right name stages again.
    make_image(30000, "v1.0.0");
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_OK, "right name stages again");
}

static void test_interrupted_and_blank(void)
{
    TEST_SECTION("update_stage -- interrupted, short and overrun uploads never read as staged");
    reset_board();
    make_image(40000, "v1.0.0");
    TEST_CHECK(upload(40000, 4096, NULL, NULL) == UPDATE_STAGE_OK && is_staged(), "baseline staged");

    // Interrupted: the previous stage is gone the moment a new upload begins.
    make_image(50000, "v1.1.0");
    TEST_CHECK(update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 50000, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_OK,
               "begin");
    TEST_CHECK(update_stage_upload_write(&g_st, g_img, 25000) == UPDATE_STAGE_OK, "half the bytes arrive");
    unsigned reads = g_fl.read_ops, muts = g_fl.mut_ops;
    update_stage_info_t bi;
    update_stage_err_t be = update_stage_get_status(&g_st, g_scratch, sizeof(g_scratch), &bi);
    TEST_CHECK(be == UPDATE_STAGE_ERR_BUSY && bi.busy && !bi.staged, "status during an upload: busy, not staged");
    TEST_CHECK(bi.phase == UPDATE_STAGE_UPLOADING && bi.bytes_done == 25000 && bi.bytes_total == 50000,
               "progress reported");
    TEST_CHECK(g_fl.read_ops == reads && g_fl.mut_ops == muts, "busy status touched no flash");
    TEST_CHECK(update_stage_clear(&g_st) == UPDATE_STAGE_ERR_BUSY, "clear refused while uploading");
    TEST_CHECK(update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 50000, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_ERR_BUSY,
               "second begin refused while uploading");
    TEST_CHECK(g_fl.mut_ops == muts, "refusals touched no flash");
    // Connection drops here: the HTTP layer aborts.
    update_stage_upload_abort(&g_st);
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE && !g_fl.sha_open, "abort returns to idle and closes the hash");
    TEST_CHECK(!is_staged(), "half-uploaded stage is not staged");
    reboot_stager();
    update_stage_info_t i = status();
    TEST_CHECK(!i.staged && i.hdr_status == STAGE_HDR_BLANK && strcmp(i.reason, "blank") == 0,
               "even after a reboot the abandoned stage reads blank");
    update_stage_upload_abort(&g_st); // idempotent when idle
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE, "abort when idle is harmless");

    // Power cut with no abort at all: nothing but the header sector was ever erased/written first.
    reset_board();
    make_image(40000, "v1.0.0");
    TEST_CHECK(upload(40000, 4096, NULL, NULL) == UPDATE_STAGE_OK, "stage a first image");
    TEST_CHECK(update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 40000, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_OK,
               "begin second");
    TEST_CHECK(update_stage_upload_write(&g_st, g_img, 40000 - 1) == UPDATE_STAGE_OK, "all but one byte");
    reboot_stager(); // power cut
    TEST_CHECK(!is_staged(), "power cut one byte short: not staged");

    // Short finish and overrun.
    reset_board();
    make_image(40000, "v1.0.0");
    TEST_CHECK(update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 40000, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_OK,
               "begin");
    TEST_CHECK(update_stage_upload_write(&g_st, g_img, 39999) == UPDATE_STAGE_OK, "one byte short");
    TEST_CHECK(update_stage_upload_finish(&g_st) == UPDATE_STAGE_ERR_SHORT, "finish short refused");
    TEST_CHECK(!is_staged() && g_st.phase == UPDATE_STAGE_IDLE, "not staged, idle");

    TEST_CHECK(update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 40000, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_OK,
               "begin again");
    TEST_CHECK(update_stage_upload_write(&g_st, g_img, 40000) == UPDATE_STAGE_OK, "all bytes");
    TEST_CHECK(update_stage_upload_write(&g_st, g_img, 1) == UPDATE_STAGE_ERR_OVERRUN, "one byte too many refused");
    TEST_CHECK(!is_staged() && g_st.phase == UPDATE_STAGE_IDLE && !g_fl.sha_open, "overrun: not staged, idle, hash closed");

    // Out-of-sequence calls.
    TEST_CHECK(update_stage_upload_write(&g_st, g_img, 1) == UPDATE_STAGE_ERR_STATE, "write without begin refused");
    TEST_CHECK(update_stage_upload_finish(&g_st) == UPDATE_STAGE_ERR_STATE, "finish without begin refused");
}

static void test_http_buffer_is_the_shared_internal_chunk(void)
{
    TEST_SECTION("update_http.c -- uses ota_http_esp.c's static internal chunk buffer, never PSRAM or a new allocation (source-text scan)");
    static const char *const candidates[] = {
        "../drivers/update/update_http.c",
        "App/drivers/update/update_http.c",
        "firmware/KilnFW/App/drivers/update/update_http.c",
    };
    char *text = test_read_source_anchored(__FILE__, "../drivers/update/update_http.c", candidates,
                                           sizeof(candidates) / sizeof(candidates[0]));
    TEST_CHECK(text != NULL, "update_http.c is readable");
    if (text == NULL) {
        return;
    }
    TEST_CHECK(strstr(text, "ota_http_esp_chunk_buf(") != NULL, "takes the shared static internal chunk buffer");
    TEST_CHECK(strstr(text, "MALLOC_CAP_SPIRAM") == NULL && strstr(text, "heap_caps_malloc") == NULL &&
                   strstr(text, "malloc(") == NULL,
               "no PSRAM / heap buffer (bounce-through-stack-temp and hidden internal temp)");
    free(text);
}
static void test_status_never_trusts_a_header_alone(void)
{
    TEST_SECTION("update_stage -- staged needs a valid header AND a matching sha256");
    reset_board();
    make_image(40000, "v1.0.0");
    TEST_CHECK(upload(40000, 4096, NULL, NULL) == UPDATE_STAGE_OK, "stage an image");
    uint8_t good_hdr[STAGE_HEADER_SIZE];
    memcpy(good_hdr, g_fl.mem, sizeof(good_hdr));

    // Image bit rot with an intact header.
    g_fl.mem[STAGE_IMAGE_OFFSET + 12345] ^= 0x01;
    reboot_stager();
    update_stage_info_t i = status();
    TEST_CHECK(!i.staged && strcmp(i.reason, "sha_mismatch") == 0, "one flipped image bit: sha_mismatch, not staged");
    TEST_CHECK(i.hdr_status == STAGE_HDR_OK, "header itself still decodes");
    g_fl.mem[STAGE_IMAGE_OFFSET + 12345] ^= 0x01;
    reboot_stager();
    TEST_CHECK(is_staged(), "restoring the bit restores staged");

    // Header corruption (CRC).
    g_fl.mem[20] ^= 0x10;
    reboot_stager();
    i = status();
    TEST_CHECK(!i.staged && strcmp(i.reason, "bad_header") == 0 && i.hdr_status == STAGE_HDR_BAD_CRC,
               "header byte flipped: CRC failure, not staged");
    memcpy(g_fl.mem, good_hdr, sizeof(good_hdr));

    // Header that claims more image than was written (sha cannot match).
    reboot_stager();
    TEST_CHECK(is_staged(), "header restored: staged again");

    // Not VERIFIED: a WRITING header with a matching image is still not installable.
    stage_header_t h;
    TEST_CHECK(stage_header_decode(good_hdr, sizeof(good_hdr), CAP, &h) == STAGE_HDR_OK, "decode the good header");
    h.state = STAGE_STATE_WRITING;
    uint8_t wr[STAGE_HEADER_SIZE];
    TEST_CHECK(stage_header_encode(&h, CAP, wr) == STAGE_HDR_OK, "encode a WRITING header");
    memset(g_fl.mem, 0xFF, 4096);
    memcpy(g_fl.mem, wr, sizeof(wr));
    reboot_stager();
    i = status();
    TEST_CHECK(!i.staged && strcmp(i.reason, "not_verified") == 0, "WRITING state with a good image: not staged");

    // Valid header whose sha is for different bytes.
    h.state = STAGE_STATE_VERIFIED;
    h.sha256[0] ^= 0xFF;
    TEST_CHECK(stage_header_encode(&h, CAP, wr) == STAGE_HDR_OK, "encode header with a wrong sha");
    memset(g_fl.mem, 0xFF, 4096);
    memcpy(g_fl.mem, wr, sizeof(wr));
    reboot_stager();
    i = status();
    TEST_CHECK(!i.staged && strcmp(i.reason, "sha_mismatch") == 0, "valid header, wrong sha: not staged");

    // A mismatch is cached too: a repeat GET re-reads only the header, never the image.
    {
        unsigned reads_before = g_fl.read_ops;
        i = status();
        TEST_CHECK(!i.staged && strcmp(i.reason, "sha_mismatch") == 0 && g_fl.read_ops == reads_before + 1,
                   "repeat status on a bad stage uses the negative cache (header read only)");
        TEST_CHECK(update_stage_clear(&g_st) == UPDATE_STAGE_OK, "clear the bad stage");
        TEST_CHECK(!g_st.bad_valid, "clear invalidates the negative cache");
        TEST_CHECK(strcmp(status().reason, "blank") == 0, "cleared stage reads blank");
    }

    // Garbage in the header sector.
    for (int k = 0; k < 256; k++) {
        g_fl.mem[k] = (uint8_t)(k * 37 + 11);
    }
    reboot_stager();
    TEST_CHECK(!is_staged() && strcmp(status().reason, "bad_header") == 0, "garbage header: not staged");

    // Read failures are reported as failures, never as staged.
    memset(g_fl.mem, 0xFF, 4096);
    memcpy(g_fl.mem, good_hdr, sizeof(good_hdr));
    reboot_stager();
    g_fl.read_ops = 0;
    g_fl.fail_read_at = 3; // header read ok, image re-read fails early
    update_stage_info_t fi;
    update_stage_err_t e = update_stage_get_status(&g_st, g_scratch, sizeof(g_scratch), &fi);
    TEST_CHECK(e == UPDATE_STAGE_ERR_FLASH && !fi.staged && strcmp(fi.reason, "read_error") == 0,
               "a failing image re-read reports read_error, not staged");
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE && !g_fl.sha_open, "idle and hash closed after the failure");
    g_fl.fail_read_at = 0;
    TEST_CHECK(is_staged(), "and a clean retry reports staged");
}

static void test_clear(void)
{
    TEST_SECTION("update_stage -- clear");
    reset_board();
    make_image(40000, "v1.0.0");
    TEST_CHECK(upload(40000, 4096, NULL, NULL) == UPDATE_STAGE_OK && is_staged(), "staged");
    TEST_CHECK(update_stage_clear(&g_st) == UPDATE_STAGE_OK, "clear");
    update_stage_info_t i = status();
    TEST_CHECK(!i.staged && i.hdr_status == STAGE_HDR_BLANK, "not staged after clear");
    reboot_stager();
    TEST_CHECK(!is_staged(), "not staged after clear and reboot");
    TEST_CHECK(update_stage_clear(&g_st) == UPDATE_STAGE_OK && !is_staged(), "clear is idempotent");
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE, "idle");

    // The cache must not outlive a clear.
    reset_board();
    make_image(40000, "v1.0.0");
    TEST_CHECK(upload(40000, 4096, NULL, NULL) == UPDATE_STAGE_OK && is_staged(), "staged with cache warm");
    memset(g_fl.mem, 0xFF, 4096); // header vanishes behind the stager's back
    TEST_CHECK(!is_staged(), "a vanished header is never reported staged from the cache");

    reset_board();
    g_fl.fail_mut_at = 1;
    TEST_CHECK(update_stage_clear(&g_st) == UPDATE_STAGE_ERR_FLASH, "erase failure reported");
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE, "idle after a failed clear");
}

static void test_flash_and_hash_failures(void)
{
    TEST_SECTION("update_stage -- flash/hash failures leave nothing staged");
    reset_board();
    make_image(40000, "v1.0.0");
    g_fl.fail_mut_at = 1; // the header-sector erase in begin
    TEST_CHECK(upload(40000, 4096, NULL, NULL) == UPDATE_STAGE_ERR_FLASH, "begin erase failure reported");
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE && !is_staged(), "idle, nothing staged");

    reset_board();
    g_fl.fail_sha_update_at = 2;
    TEST_CHECK(upload(40000, 4096, NULL, NULL) == UPDATE_STAGE_ERR_HASH, "hash failure mid-stream reported");
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE && !g_fl.sha_open && g_fl.sha_aborts > 0, "idle, hash aborted");
    TEST_CHECK(!is_staged(), "nothing staged");

    // Image corrupted between the last write and the verify re-read.
    reset_board();
    TEST_CHECK(update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 40000, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_OK,
               "begin");
    TEST_CHECK(update_stage_upload_write(&g_st, g_img, 40000) == UPDATE_STAGE_OK, "stream all bytes");
    g_fl.mem[STAGE_IMAGE_OFFSET + 30000] ^= 0x01; // a stuck bit on flash
    TEST_CHECK(update_stage_upload_finish(&g_st) == UPDATE_STAGE_ERR_READBACK, "readback mismatch caught by the re-read");
    TEST_CHECK(!is_staged() && g_st.phase == UPDATE_STAGE_IDLE, "not staged, idle");
    TEST_CHECK(g_fl.mem[0] == 0xFF, "no header was written");

    // Verify re-read failure.
    reset_board();
    g_fl.fail_read_at = 2;
    TEST_CHECK(upload(40000, 4096, NULL, NULL) == UPDATE_STAGE_ERR_FLASH, "re-read failure during verify reported");
    TEST_CHECK(!is_staged(), "not staged");

    // Header write fails outright / tears halfway: the header is erased again.
    for (int partial = 0; partial < 2; partial++) {
        reset_board();
        make_image(20000, "v1.0.0");
        // Count mutating ops of a clean run, then fail the last one (the header write).
        TEST_CHECK(upload(20000, 4096, NULL, NULL) == UPDATE_STAGE_OK, "clean run to count ops");
        unsigned total = g_fl.mut_ops;
        reset_board();
        g_fl.fail_mut_at = total;
        g_fl.partial = partial != 0;
        update_stage_err_t e = upload(20000, 4096, NULL, NULL);
        TEST_CHECK(e == UPDATE_STAGE_ERR_HEADER, "header write failure reported");
        reboot_stager();
        update_stage_info_t i = status();
        TEST_CHECK(!i.staged, "torn/failed header write is not staged");
        TEST_CHECK(i.hdr_status == STAGE_HDR_BLANK || i.hdr_status != STAGE_HDR_OK, "header does not decode");
    }
}

static void test_power_cut_at_every_step(void)
{
    TEST_SECTION("update_stage -- a fault at ANY mutating step is never reported staged");
    reset_board();
    make_image(20000, "v1.0.0");
    TEST_CHECK(upload(20000, 3000, NULL, NULL) == UPDATE_STAGE_OK, "clean run");
    unsigned total = g_fl.mut_ops;
    TEST_CHECK(total >= 8, "enough mutating ops to make the sweep meaningful");
    unsigned wrongly_staged = 0, wrongly_unstaged = 0;
    for (unsigned k = 1; k <= total + 1; k++) {
        for (int partial = 0; partial < 2; partial++) {
            reset_board();
            g_fl.fail_mut_at = k;
            g_fl.partial = partial != 0;
            update_stage_err_t e = upload(20000, 3000, NULL, NULL);
            reboot_stager(); // a power cut: everything in RAM is lost
            bool staged = is_staged();
            if (e != UPDATE_STAGE_OK && staged) {
                wrongly_staged++;
            }
            if (e == UPDATE_STAGE_OK && !staged) {
                wrongly_unstaged++;
            }
        }
    }
    TEST_CHECK(wrongly_staged == 0, "no failed upload ever reads as staged");
    TEST_CHECK(wrongly_unstaged == 0, "an upload that reported success reads as staged");
}

static void test_bad_arguments(void)
{
    TEST_SECTION("update_stage -- argument validation");
    reset_board();
    uint8_t tiny[16];
    TEST_CHECK(update_stage_upload_begin(NULL, g_scratch, sizeof(g_scratch), 1000, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_ERR_ARG,
               "NULL stager");
    TEST_CHECK(update_stage_upload_begin(&g_st, NULL, sizeof(g_scratch), 1000, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_ERR_ARG,
               "NULL scratch");
    TEST_CHECK(update_stage_upload_begin(&g_st, tiny, sizeof(tiny), 1000, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_ERR_ARG,
               "scratch below the minimum");
    update_stage_info_t i;
    TEST_CHECK(update_stage_get_status(&g_st, tiny, sizeof(tiny), &i) == UPDATE_STAGE_ERR_ARG && g_st.phase == UPDATE_STAGE_IDLE,
               "status with a tiny scratch refused and idle");
    TEST_CHECK(update_stage_get_status(&g_st, g_scratch, sizeof(g_scratch), NULL) == UPDATE_STAGE_ERR_ARG, "NULL out");
    TEST_CHECK(g_fl.mut_ops == 0 && g_fl.read_ops == 0, "argument errors touched no flash");

    // A partition no larger than the header sector has no image area.
    update_stage_t bad;
    update_stage_io_t io = g_st.io;
    io.partition_size = 4096;
    update_stage_init(&bad, &io);
    TEST_CHECK(update_stage_capacity(&bad) == 0, "no image capacity");
    TEST_CHECK(update_stage_upload_begin(&bad, g_scratch, sizeof(g_scratch), 1000, NULL, NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_ERR_ARG,
               "unusable partition refused");

    TEST_CHECK(strcmp(update_stage_err_name(UPDATE_STAGE_ERR_OVERSIZE), "image_too_large") == 0, "err name");
    for (int e = UPDATE_STAGE_OK; e <= UPDATE_STAGE_ERR_STATE; e++) {
        TEST_CHECK(strcmp(update_stage_err_name((update_stage_err_t)e), "unknown") != 0, "every error has a name");
    }
    for (int p = UPDATE_STAGE_IDLE; p <= UPDATE_STAGE_CHECKING; p++) {
        TEST_CHECK(strcmp(update_stage_phase_name((update_stage_phase_t)p), "unknown") != 0, "every phase has a name");
    }
}

typedef struct {
    int calls;
    char semver[STAGE_SEMVER_FIELD_LEN + 1];
    update_stage_err_t verdict;
    bool have_id;
    update_image_id_t id;
} gate_rec_t;

static update_stage_err_t rec_gate(void *ctx, const char *semver, const char *commit, const update_image_id_t *id)
{
    gate_rec_t *r = ctx;
    (void)commit;
    r->calls++;
    r->have_id = id != NULL;
    if (id != NULL) {
        r->id = *id;
    }
    strncpy(r->semver, semver, STAGE_SEMVER_FIELD_LEN);
    r->semver[STAGE_SEMVER_FIELD_LEN] = '\0';
    return r->verdict;
}

static update_stage_err_t upload_gated(size_t len, size_t chunk, const char *semver, update_stage_gate_fn gate, void *ctx)
{
    update_stage_err_t e = update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), (uint32_t)len, semver, NULL,
                                                      STAGE_SOURCE_UPLOAD);
    if (e != UPDATE_STAGE_OK) {
        return e;
    }
    update_stage_set_gate(&g_st, gate, ctx);
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = len - off < chunk ? len - off : chunk;
        e = update_stage_upload_write(&g_st, g_img + off, n);
        if (e != UPDATE_STAGE_OK) {
            return e;
        }
    }
    return update_stage_upload_finish(&g_st);
}

// The install gate (downgrade policy hook) runs once, after the project check, with the resolved version.
static void test_install_gate(void)
{
    TEST_SECTION("update_stage -- install gate (downgrade policy hook)");
    reset_board();
    gate_rec_t r;
    memset(&r, 0, sizeof(r));
    make_image(30000, "v1.2.3");
    TEST_CHECK(upload_gated(30000, 4096, NULL, rec_gate, &r) == UPDATE_STAGE_OK, "gate OK: staged");
    TEST_CHECK(r.calls == 1 && strcmp(r.semver, "1.2.3") == 0, "gate called once with the image's own version, v stripped");
    TEST_CHECK(is_staged(), "staged after an allowing gate");

    reset_board();
    r.calls = 0;
    r.verdict = UPDATE_STAGE_ERR_POLICY;
    make_image(30000, "v9.9.9");
    TEST_CHECK(upload_gated(30000, 7, "v9.9.9", rec_gate, &r) == UPDATE_STAGE_ERR_POLICY, "gate refusal surfaces as policy");
    TEST_CHECK(r.calls == 1 && strcmp(r.semver, "9.9.9") == 0, "gate saw the declared version once, even with 7-byte writes");
    TEST_CHECK(!is_staged() && g_st.phase == UPDATE_STAGE_IDLE && !g_fl.sha_open, "refused: nothing staged, idle, hash closed");
    TEST_CHECK(g_fl.mem[STAGE_IMAGE_OFFSET] == 0xFF, "refused: no image byte written");
    TEST_CHECK(strcmp(update_stage_err_name(UPDATE_STAGE_ERR_POLICY), "policy_refused") == 0, "error name");

    // The project check still comes first: a wrong project never reaches the gate.
    r.calls = 0;
    make_image(30000, "v1.2.3");
    memset(g_img + 80, 0, 32);
    memcpy(g_img + 80, "OtherProject", 12);
    TEST_CHECK(upload_gated(30000, 4096, NULL, rec_gate, &r) == UPDATE_STAGE_ERR_WRONG_PROJECT && r.calls == 0,
               "wrong project refused before the gate");

    // F1: the identity record is read from the held-back head and handed to the gate; absent = NULL.
    reset_board();
    memset(&r, 0, sizeof(r));
    make_image(30000, "v1.2.3");
    TEST_CHECK(upload_gated(30000, 4096, NULL, rec_gate, &r) == UPDATE_STAGE_OK && !r.have_id,
               "image without a record: gate gets NULL");
    reset_board();
    memset(&r, 0, sizeof(r));
    make_image(30000, "v1.2.3");
    {
        update_image_id_t id;
        update_image_id_make(&id, 24, 16, 13, "abc1234");
        memcpy(g_img + UPDATE_STAGE_IMAGE_ID_FROM, &id, sizeof(id));
    }
    TEST_CHECK(upload_gated(30000, 4096, NULL, rec_gate, &r) == UPDATE_STAGE_OK && r.have_id &&
                   r.id.zones_cfg_version == 24 && r.id.kilnlink_version == 16 && r.id.uart_version == 13,
               "record straight after the app descriptor is read and passed to the gate");

    // F2: a declared version may not override a valid descriptor version (gated uploads).
    reset_board();
    memset(&r, 0, sizeof(r));
    make_image(30000, "v1.2.3");
    TEST_CHECK(upload_gated(30000, 4096, "99.0.0", rec_gate, &r) == UPDATE_STAGE_ERR_VERSION_MISMATCH && r.calls == 0,
               "F2: declared 99.0.0 on a 1.2.3 image: mismatch, gate never reached");
    TEST_CHECK(!is_staged() && g_fl.mem[STAGE_IMAGE_OFFSET] == 0xFF, "F2: nothing written");
    TEST_CHECK(strcmp(update_stage_err_name(UPDATE_STAGE_ERR_VERSION_MISMATCH), "version_mismatch") == 0, "F2: error name");
    reset_board();
    memset(&r, 0, sizeof(r));
    make_image(30000, "v1.2.3");
    TEST_CHECK(upload_gated(30000, 4096, "v1.2.3", rec_gate, &r) == UPDATE_STAGE_OK && r.calls == 1,
               "F2: declared equal to the descriptor (v ignored): fine");
    reset_board();
    memset(&r, 0, sizeof(r));
    make_image(30000, "not-a-version");
    TEST_CHECK(upload_gated(30000, 4096, "2.0.0", rec_gate, &r) == UPDATE_STAGE_OK && r.calls == 1,
               "F2: an invalid descriptor version leaves the declared one in force");
    reset_board();
    make_image(30000, "v1.2.3");
    TEST_CHECK(upload(30000, 4096, "99.0.0", NULL) == UPDATE_STAGE_OK, "F2: ungated (fetch) path is not subject to the check");

    // A later ungated upload is unaffected (begin clears the gate).
    r.calls = 0;
    make_image(30000, "v1.2.3");
    update_stage_set_gate(&g_st, rec_gate, &r);
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_OK && r.calls == 0, "begin clears a stale gate");
}

// A gate refusal (e.g. accidental downgrade) must leave the previously staged image byte-identical.
static void test_gate_refusal_keeps_stage(void)
{
    TEST_SECTION("update_stage -- gate refusal leaves the existing stage byte-identical");
    reset_board();
    make_image(30000, "v2.0.0");
    TEST_CHECK(upload(30000, 4096, NULL, NULL) == UPDATE_STAGE_OK, "image A staged");
    uint8_t before_sha[32];
    update_stage_info_t a = status();
    TEST_CHECK(a.staged && a.hdr_status == STAGE_HDR_OK, "A valid before");
    memcpy(before_sha, a.sha256, 32);
    static uint8_t snap[0x400000];
    TEST_CHECK(sizeof(snap) >= STAGE_IMAGE_OFFSET + 30000, "snapshot buffer large enough");
    memcpy(snap, g_fl.mem, STAGE_IMAGE_OFFSET + 30000);

    gate_rec_t r = { 0, "", UPDATE_STAGE_ERR_POLICY };
    make_image(30000, "v1.0.0");
    TEST_CHECK(upload_gated(30000, 4096, NULL, rec_gate, &r) == UPDATE_STAGE_ERR_POLICY && r.calls == 1,
               "downgrade refused by the gate");
    TEST_CHECK(memcmp(snap, g_fl.mem, STAGE_IMAGE_OFFSET + 30000) == 0, "header sector and image bytes unchanged");
    update_stage_info_t b = status();
    TEST_CHECK(b.staged && b.hdr_status == STAGE_HDR_OK && memcmp(b.sha256, before_sha, 32) == 0 &&
                   strcmp(b.semver, "2.0.0") == 0,
               "stage still reads A: valid header, same sha and version");
    TEST_CHECK(g_st.phase == UPDATE_STAGE_IDLE && !g_fl.sha_open, "idle, hash closed");
}

// MED-2: the GitHub fetch installs update_stage_manifest_gate with release.json's identity.
static void put_record_c(uint32_t z, uint32_t k, uint32_t u, const char *commit)
{
    update_image_id_t id;
    update_image_id_make(&id, z, k, u, commit);
    memcpy(g_img + UPDATE_STAGE_IMAGE_ID_FROM, &id, sizeof(id));
}

static void test_manifest_gate(void)
{
    TEST_SECTION("update_stage -- fetch manifest cross-check gate");
    update_identity_t want;
    memset(&want, 0, sizeof(want));
    want.zones_cfg_version = 24;
    want.kilnlink_version = 16;
    want.uart_version = 13;

    reset_board();
    make_image(30000, "v1.2.3");
    put_record_c(24, 16, 13, "abc1234");
    TEST_CHECK(upload_gated(30000, 4096, "v1.2.3", update_stage_manifest_gate, &want) == UPDATE_STAGE_OK,
               "matching record and version: staged");

    reset_board();
    make_image(30000, "v1.2.3");
    put_record_c(23, 16, 13, "abc1234");
    TEST_CHECK(upload_gated(30000, 4096, "v1.2.3", update_stage_manifest_gate, &want) == UPDATE_STAGE_ERR_POLICY &&
                   !is_staged(),
               "zones_cfg differs from the manifest: refused, stage blank");
    reset_board();
    make_image(30000, "v1.2.3");
    put_record_c(24, 15, 13, "abc1234");
    TEST_CHECK(upload_gated(30000, 4096, "v1.2.3", update_stage_manifest_gate, &want) == UPDATE_STAGE_ERR_POLICY,
               "kilnlink differs: refused");
    reset_board();
    make_image(30000, "v1.2.3");
    put_record_c(24, 16, 12, "abc1234");
    TEST_CHECK(upload_gated(30000, 4096, "v1.2.3", update_stage_manifest_gate, &want) == UPDATE_STAGE_ERR_POLICY,
               "uart differs: refused");
    reset_board();
    make_image(30000, "v1.2.3");
    TEST_CHECK(upload_gated(30000, 4096, "v1.2.3", update_stage_manifest_gate, &want) == UPDATE_STAGE_ERR_POLICY,
               "image with no identity record: refused");
    reset_board();
    make_image(30000, "v1.2.3");
    put_record_c(24, 16, 13, "abc1234");
    TEST_CHECK(upload_gated(30000, 4096, "v1.2.4", update_stage_manifest_gate, &want) == UPDATE_STAGE_ERR_VERSION_MISMATCH,
               "descriptor version differs from the manifest version: refused");

    // Review 5 M1: the IMAGE embedded commit is compared with the manifest commit. The stager declared
    // commit is what production passes (the manifest own), so it is always the matching one here.
    {
        const char *c1 = "0123456789abcdef0123456789abcdef01234567";
        update_identity_t wc = want;
        strcpy(wc.commit, c1);
        static const struct {
            const char *img_commit;
            bool ok;
            const char *what;
        } cases[] = {
            { "0123456", true, "M1: image commit is a prefix of the manifest commit: staged" },
            { "0123456789AB", true, "M1: prefix compare is case-insensitive" },
            { "0123457", false, "M1: image built from another commit than the manifest names: refused" },
            { "", false, "M1: image with no commit, manifest declares one: refused (fail closed)" },
            { "unknown", false, "M1: image built without git (unknown): refused" },
            { "012345", false, "M1: image commit shorter than 7 chars: refused" },
        };
        for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
            reset_board();
            make_image(30000, "v1.2.3");
            put_record_c(24, 16, 13, cases[k].img_commit);
            update_stage_err_t e = update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 30000, "v1.2.3", c1,
                                                             STAGE_SOURCE_GITHUB);
            update_stage_set_gate(&g_st, update_stage_manifest_gate, &wc);
            for (size_t off = 0; e == UPDATE_STAGE_OK && off < 30000; off += 4096) {
                e = update_stage_upload_write(&g_st, g_img + off, 30000 - off < 4096 ? 30000 - off : 4096);
            }
            if (e == UPDATE_STAGE_OK) {
                e = update_stage_upload_finish(&g_st);
            }
            if (cases[k].ok) {
                TEST_CHECK(e == UPDATE_STAGE_OK && is_staged(), cases[k].what);
            } else {
                TEST_CHECK(e == UPDATE_STAGE_ERR_POLICY && !is_staged(), cases[k].what);
            }
        }
        // A manifest without a commit does not demand one of the image.
        reset_board();
        make_image(30000, "v1.2.3");
        put_record_c(24, 16, 13, "");
        TEST_CHECK(upload_gated(30000, 4096, "v1.2.3", update_stage_manifest_gate, &want) == UPDATE_STAGE_OK,
                   "M1: manifest without a commit: image commit not required");
    }
}

// Review 5 L1: the writer-op / timeout race is decided by one arbiter, in either order.
static void test_wr_arb(void)
{
    TEST_SECTION("update_wr_arb -- writer finish vs caller timeout (review 5 L1)");
    update_wr_arb_t a;
    update_wr_arb_issue(&a);
    TEST_CHECK(!update_wr_arb_writer_done(&a) && update_wr_arb_caller_timeout(&a) == false,
               "L1: writer finished first (same tick as the timeout): caller sees completion, not wedged");
    update_wr_arb_issue(&a);
    TEST_CHECK(update_wr_arb_caller_timeout(&a) && update_wr_arb_writer_done(&a),
               "L1: caller timed out first: writer is told it was abandoned and must clean up");
    update_wr_arb_issue(&a);
    TEST_CHECK(!update_wr_arb_writer_done(&a), "L1: finish on a pending op is not abandoned");
}

// Review 5 L2: the wedge replaces only the benign "blank" reason of an unstaged stage.
static void test_status_reason(void)
{
    TEST_SECTION("update_stage -- status reason under a wedged writer (review 5 L2)");
    update_stage_info_t i;
    memset(&i, 0, sizeof(i));
    i.reason = "blank";
    TEST_CHECK(strcmp(update_stage_status_reason(&i, true), "writer_wedged_reboot_required") == 0,
               "L2: wedged + blank stage: wedge reason shown");
    TEST_CHECK(strcmp(update_stage_status_reason(&i, false), "blank") == 0, "L2: not wedged: reason untouched");
    i.reason = "sha_mismatch";
    TEST_CHECK(strcmp(update_stage_status_reason(&i, true), "sha_mismatch") == 0, "L2: a real fault reason is not masked");
    i.reason = "";
    i.staged = true;
    TEST_CHECK(strcmp(update_stage_status_reason(&i, true), "") == 0, "L2: a valid stage is not reported as wedged");
}

// Review 5 L3: an abandoned fetch writer abort must not kill a newer hand upload.
static void test_abort_owned(void)
{
    TEST_SECTION("update_stage -- owned abort (review 5 L3)");
    reset_board();
    make_image(30000, "v1.2.3");
    TEST_CHECK(update_stage_upload_begin(&g_st, g_scratch, sizeof(g_scratch), 30000, "v1.2.3", NULL, STAGE_SOURCE_UPLOAD) ==
                   UPDATE_STAGE_OK,
               "hand upload begun");
    TEST_CHECK(!update_stage_upload_abort_owned(&g_st, STAGE_SOURCE_GITHUB) && g_st.phase == UPDATE_STAGE_UPLOADING,
               "L3: abort scoped to the fetch source leaves a hand upload running");
    TEST_CHECK(update_stage_upload_abort_owned(&g_st, STAGE_SOURCE_UPLOAD) && g_st.phase == UPDATE_STAGE_IDLE,
               "L3: the owner can abort its own upload");
    TEST_CHECK(!update_stage_upload_abort_owned(&g_st, STAGE_SOURCE_UPLOAD), "L3: nothing active: false");
}

void run_test_update_stage(void)
{
    test_manifest_gate();
    test_wr_arb();
    test_status_reason();
    test_abort_owned();
    test_sha_reference();
    test_happy_path();
    test_semver_and_commit_args();
    test_size_limits();
    test_bad_images();
    test_wrong_project_refused();
    test_install_gate();
    test_gate_refusal_keeps_stage();
    test_interrupted_and_blank();
    test_http_buffer_is_the_shared_internal_chunk();
    test_status_never_trusts_a_header_alone();
    test_clear();
    test_flash_and_hash_failures();
    test_power_cut_at_every_step();
    test_bad_arguments();
}
