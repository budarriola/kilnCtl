// Host tests for App/drivers/update/update_install_record.c: the one-shot
// unattended-install request/result codec and the recovery-side boot logic.
#include <string.h>

#include "test_common.h"

#include "../drivers/update/update_install_record.h"
#include "ota_image_crc.h"

typedef struct {
    int events[16];
    int n_events;
    int have_req;
    uint8_t req[UPD_INST_REQ_SIZE];
    size_t req_len;
    int read_rc;
    int clear_rc;
    int res_rc;
    int apply_rc;
    bool stage_ok;
    uint32_t stage_len;
    uint8_t stage_sha[UPD_INST_SHA_LEN];
    uint8_t res_blob[UPD_INST_RES_SIZE];
    int res_writes;
    int write_rc;
    int write_calls;
    int clear_calls;
    int apply_calls;
    int apply_attempts_seen;
} fake_t;

enum { EV_READ = 1, EV_CLEAR, EV_STAGE, EV_APPLY, EV_RES, EV_WRITE };

static void ev(fake_t *f, int e)
{
    if (f->n_events < 16) {
        f->events[f->n_events++] = e;
    }
}

static int f_read(void *c, uint8_t *buf, size_t cap, size_t *len)
{
    fake_t *f = c;
    ev(f, EV_READ);
    if (f->read_rc != 0) {
        return f->read_rc;
    }
    if (!f->have_req) {
        return 1;
    }
    memcpy(buf, f->req, f->req_len < cap ? f->req_len : cap);
    *len = f->req_len;
    return 0;
}

static int f_clear(void *c)
{
    fake_t *f = c;
    ev(f, EV_CLEAR);
    f->clear_calls++;
    if (f->clear_rc == 0) {
        f->have_req = 0;
    }
    return f->clear_rc;
}

static int f_write(void *c, const uint8_t *b, size_t n)
{
    fake_t *f = c;
    ev(f, EV_WRITE);
    f->write_calls++;
    if (f->write_rc == 0 && n <= sizeof(f->req)) {
        memcpy(f->req, b, n);
        f->req_len = n;
    }
    return f->write_rc;
}

static int f_res(void *c, const uint8_t *b, size_t n)
{
    fake_t *f = c;
    ev(f, EV_RES);
    f->res_writes++;
    if (f->res_rc == 0 && n == UPD_INST_RES_SIZE) {
        memcpy(f->res_blob, b, n);
    }
    return f->res_rc;
}

static bool f_stage(void *c, uint32_t *len, uint8_t sha[UPD_INST_SHA_LEN])
{
    fake_t *f = c;
    ev(f, EV_STAGE);
    *len = f->stage_len;
    memcpy(sha, f->stage_sha, UPD_INST_SHA_LEN);
    return f->stage_ok;
}

static int f_apply(void *c)
{
    fake_t *f = c;
    ev(f, EV_APPLY);
    f->apply_calls++;
    {
        update_install_req_t seen;
        f->apply_attempts_seen = update_install_req_decode(f->req, f->req_len, &seen) ? (int)seen.attempts : -1;
    }
    return f->apply_rc;
}

static void fake_init(fake_t *f)
{
    memset(f, 0, sizeof(*f));
    update_install_req_t r;
    memset(&r, 0, sizeof(r));
    r.image_length = 2956144u;
    for (int i = 0; i < (int)UPD_INST_SHA_LEN; i++) {
        r.sha256[i] = (uint8_t)(0xA0 + i);
    }
    strcpy(r.semver, "1.0.0-pre.3");
    f->req_len = update_install_req_encode(&r, f->req);
    f->have_req = 1;
    f->stage_ok = true;
    f->stage_len = r.image_length;
    memcpy(f->stage_sha, r.sha256, UPD_INST_SHA_LEN);
}

static int f_res_read(void *c, uint8_t *buf, size_t cap, size_t *len)
{
    fake_t *f = c;
    if (!f->res_writes) {
        return 1;
    }
    memcpy(buf, f->res_blob, cap < UPD_INST_RES_SIZE ? cap : UPD_INST_RES_SIZE);
    *len = UPD_INST_RES_SIZE;
    return 0;
}

static update_install_ops_t ops_for(fake_t *f)
{
    update_install_ops_t o;
    o.ctx = f;
    o.req_read = f_read;
    o.req_clear = f_clear;
    o.req_write = f_write;
    o.res_write = f_res;
    o.res_read = f_res_read;
    o.stage_verified = f_stage;
    o.apply_start = f_apply;
    return o;
}

static int index_of(const fake_t *f, int e)
{
    for (int i = 0; i < f->n_events; i++) {
        if (f->events[i] == e) {
            return i;
        }
    }
    return -1;
}

static void test_codec(void)
{
    TEST_SECTION("update_install_record -- codecs");
    fake_t f;
    fake_init(&f);
    update_install_req_t r;
    TEST_CHECK(update_install_req_decode(f.req, f.req_len, &r), "request round trips");
    TEST_CHECK(r.image_length == 2956144u && strcmp(r.semver, "1.0.0-pre.3") == 0 && r.sha256[3] == 0xA3,
               "request fields preserved");
    uint8_t bad[UPD_INST_REQ_SIZE];
    memcpy(bad, f.req, sizeof(bad));
    bad[20] ^= 1;
    TEST_CHECK(!update_install_req_decode(bad, sizeof(bad), &r), "flipped byte fails CRC");
    TEST_CHECK(!update_install_req_decode(f.req, f.req_len - 1, &r), "short blob refused");
    update_install_req_t z;
    memset(&z, 0, sizeof(z));
    uint8_t out[UPD_INST_REQ_SIZE];
    TEST_CHECK(update_install_req_encode(&z, out) == 0, "zero-length image not encodable");
    memset(z.semver, 'x', sizeof(z.semver));
    z.image_length = 1;
    TEST_CHECK(update_install_req_encode(&z, out) == 0, "unterminated semver not encodable");

    update_install_res_t s, d;
    memset(&s, 0, sizeof(s));
    s.code = UPD_INST_RES_FAIL;
    s.reason = UPD_INST_REASON_SHA_MISMATCH;
    strcpy(s.semver, "1.0.0");
    uint8_t rb[UPD_INST_RES_SIZE];
    TEST_CHECK(update_install_res_encode(&s, rb) == UPD_INST_RES_SIZE, "result encodes");
    TEST_CHECK(update_install_res_decode(rb, sizeof(rb), &d) && d.code == UPD_INST_RES_FAIL &&
                   d.reason == UPD_INST_REASON_SHA_MISMATCH && strcmp(d.semver, "1.0.0") == 0,
               "result round trips");
    {
        update_install_res_t o2 = s;
        uint8_t ob[UPD_INST_RES_SIZE];
        o2.code = (uint8_t)(UPD_INST_RES_FAIL + 1);
        TEST_CHECK(update_install_res_encode(&o2, ob) == UPD_INST_RES_SIZE &&
                       !update_install_res_decode(ob, sizeof(ob), &d),
                   "valid-CRC result with out-of-range code refused");
        o2 = s;
        o2.reason = UPD_INST_REASON_COUNT;
        TEST_CHECK(update_install_res_encode(&o2, ob) == UPD_INST_RES_SIZE &&
                       !update_install_res_decode(ob, sizeof(ob), &d),
                   "valid-CRC result with out-of-range reason refused");
    }
    rb[5] = 9;
    TEST_CHECK(!update_install_res_decode(rb, sizeof(rb), &d), "tampered result refused (CRC)");
    TEST_CHECK(strcmp(update_install_reason_name(UPD_INST_REASON_SHA_MISMATCH), "sha_mismatch") == 0,
               "reason name");
    TEST_CHECK(strcmp(update_install_reason_name(UPD_INST_REASON_OUTCOME_UNKNOWN),
                      "outcome_unknown_do_not_power_cycle") == 0,
               "outcome-unknown reason name");
    {
        update_install_res_t u;
        uint8_t ub[UPD_INST_RES_SIZE];
        memset(&u, 0, sizeof(u));
        u.code = UPD_INST_RES_FAIL;
        u.reason = UPD_INST_REASON_OUTCOME_UNKNOWN;
        strcpy(u.semver, "1.0.0");
        TEST_CHECK(update_install_res_encode(&u, ub) == UPD_INST_RES_SIZE &&
                       update_install_res_decode(ub, sizeof(ub), &d) && d.reason == UPD_INST_REASON_OUTCOME_UNKNOWN,
                   "outcome-unknown result round-trips (inside the range check)");
    }
    TEST_CHECK(strcmp(update_install_reason_name(200), "unknown") == 0, "unknown reason name");
}

static void test_boot_logic(void)
{
    TEST_SECTION("update_install_record -- recovery boot logic");
    fake_t f;
    update_install_res_t res;
    update_install_ops_t o;
    update_install_req_t r;

    fake_init(&f);
    f.have_req = 0;
    o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_NO_REQUEST && f.apply_calls == 0 &&
                   f.res_writes == 0,
               "no request: nothing happens, no result written");

    fake_init(&f);
    o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_STARTED, "good request starts the apply");
    TEST_CHECK(f.apply_calls == 1 && f.res_writes == 0, "apply started once, no result yet");
    TEST_CHECK(f.have_req == 1 && f.clear_calls == 0, "request stays present through the apply");
    TEST_CHECK(update_install_req_decode(f.req, f.req_len, &r) && r.attempts == 1, "attempt counter raised to 1");
    TEST_CHECK(index_of(&f, EV_WRITE) >= 0 && index_of(&f, EV_WRITE) < index_of(&f, EV_APPLY),
               "attempt counter written BEFORE the apply starts");
    TEST_CHECK(f.apply_attempts_seen == 1, "stored counter already raised when the apply starts");
    TEST_CHECK(strcmp(res.semver, "1.0.0-pre.3") == 0, "started outcome carries the target version");

    // Idempotent re-run after a simulated power cut mid-copy: request still there.
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_STARTED && f.apply_calls == 2 &&
                   update_install_req_decode(f.req, f.req_len, &r) && r.attempts == 2,
               "second boot re-applies and counts attempt 2");
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_STARTED && f.apply_calls == 3 &&
                   update_install_req_decode(f.req, f.req_len, &r) && r.attempts == 3,
               "third boot re-applies and counts attempt 3");
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_REFUSED && f.apply_calls == 3,
               "bound reached: no fourth apply");
    TEST_CHECK(res.reason == UPD_INST_REASON_ATTEMPTS_EXCEEDED && f.have_req == 0 && f.res_writes == 1,
               "bound reached: reason recorded and request cleared");
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_NO_REQUEST, "no loop after the bound");

    fake_init(&f);
    f.write_rc = -1;
    o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_REFUSED && f.apply_calls == 0 &&
                   res.reason == UPD_INST_REASON_COUNTER_FAILED,
               "counter write failure never applies");

    fake_init(&f);
    f.stage_sha[5] ^= 0xFF;
    o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_REFUSED && f.apply_calls == 0 &&
                   f.write_calls == 0,
               "sha mismatch refuses, counter untouched");
    TEST_CHECK(res.reason == UPD_INST_REASON_SHA_MISMATCH && f.have_req == 0,
               "sha mismatch recorded, request gone");

    fake_init(&f);
    f.stage_len += 1;
    o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_REFUSED && f.apply_calls == 0 &&
                   res.reason == UPD_INST_REASON_SIZE_MISMATCH && f.have_req == 0,
               "size mismatch refuses");

    fake_init(&f);
    f.stage_ok = false;
    o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_REFUSED && f.apply_calls == 0 &&
                   res.reason == UPD_INST_REASON_STAGE_NOT_READY && f.have_req == 0,
               "unverified stage refuses");

    fake_init(&f);
    f.req[30] ^= 1;
    o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_REFUSED && f.apply_calls == 0 &&
                   res.reason == UPD_INST_REASON_RECORD_BAD && f.have_req == 0,
               "corrupt record refused and cleared");

    fake_init(&f);
    f.apply_rc = -1;
    o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_REFUSED &&
                   res.reason == UPD_INST_REASON_APPLY_FAILED && f.res_writes == 1 && f.have_req == 1,
               "apply start failure recorded, request kept for retry");

    fake_init(&f);
    f.read_rc = -1;
    o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_REFUSED && f.apply_calls == 0 &&
                   res.reason == UPD_INST_REASON_NVS_UNAVAILABLE && f.clear_calls == 0,
               "unreadable NVS refuses, nothing cleared");

    fake_init(&f);
    f.res_rc = -1;
    o = ops_for(&f);
    TEST_CHECK(update_install_record_result(&o, UPD_INST_RES_FAIL, UPD_INST_REASON_APPLY_FAILED, "x", NULL) != 0,
               "failed result write is reported by record_result");

    fake_init(&f);
    o = ops_for(&f);
    TEST_CHECK(update_install_record_result(&o, UPD_INST_RES_OK, UPD_INST_REASON_NONE, "1.0.0-pre.3", NULL) == 0,
               "ok result writes");
    update_install_res_t d;
    TEST_CHECK(update_install_res_decode(f.res_blob, sizeof(f.res_blob), &d) && d.code == UPD_INST_RES_OK &&
                   strcmp(d.semver, "1.0.0-pre.3") == 0,
               "written ok result decodes");
}

static int g_guard_rc;
static fake_t *g_guard_f;
static int guard_fn(void)
{
    ev(g_guard_f, 99);
    return g_guard_rc;
}

static const uint8_t id_a[UPD_INST_ID_LEN] = { 0xA0, 0xA1, 0xA2, 0xA3 };

static void test_result_identity(void)
{
    TEST_SECTION("update_install_record -- result identity (sha256 prefix of the request)");
    update_install_res_t s, d;
    uint8_t rb[UPD_INST_RES_SIZE];
    memset(&s, 0, sizeof(s));
    s.code = UPD_INST_RES_OK;
    strcpy(s.semver, "1.0.0-pre.3");
    memcpy(s.id, id_a, UPD_INST_ID_LEN);
    TEST_CHECK(update_install_res_encode(&s, rb) == UPD_INST_RES_SIZE && update_install_res_decode(rb, sizeof(rb), &d) &&
                   memcmp(d.id, id_a, UPD_INST_ID_LEN) == 0,
               "identity round trips through the result blob");
    rb[8 + UPD_INST_SEMVER_LEN] ^= 0x01u;
    TEST_CHECK(!update_install_res_decode(rb, sizeof(rb), &d), "identity bytes are covered by the CRC");
    memset(&s, 0, sizeof(s));
    s.code = UPD_INST_RES_OK;
    TEST_CHECK(update_install_res_encode(&s, rb) == UPD_INST_RES_SIZE && update_install_res_decode(rb, sizeof(rb), &d) &&
                   memcmp(d.id, "\0\0\0\0", UPD_INST_ID_LEN) == 0,
               "no identity decodes as zeros");
    char hex[UPD_INST_ID_HEX_LEN];
    update_install_id_hex(id_a, hex);
    TEST_CHECK(strcmp(hex, "a0a1a2a3") == 0, "identity hex");

    // The recorded results carry the request's sha256 prefix.
    fake_t f;
    update_install_res_t res;
    update_install_ops_t o;
    fake_init(&f);
    o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_STARTED && memcmp(res.id, id_a, UPD_INST_ID_LEN) == 0,
               "started outcome carries the request identity");
    fake_init(&f);
    f.stage_sha[0] ^= 0xFFu;
    o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_REFUSED && memcmp(res.id, id_a, UPD_INST_ID_LEN) == 0 &&
                   update_install_res_decode(f.res_blob, sizeof(f.res_blob), &d) && memcmp(d.id, id_a, UPD_INST_ID_LEN) == 0,
               "a refusal records the request identity too");
    fake_init(&f);
    o = ops_for(&f);
    g_guard_f = &f;
    g_guard_rc = 0;
    bool ok = false;
    TEST_CHECK(update_install_pre_boot(&o, guard_fn) == 0 &&
                   update_install_post_boot(&o, "1.0.0-pre.3", id_a, &ok) == 0 && ok &&
                   update_install_res_decode(f.res_blob, sizeof(f.res_blob), &d) && memcmp(d.id, id_a, UPD_INST_ID_LEN) == 0,
               "the pre-boot OK result carries the identity");
}

/* recfx leftover: a refusal whose request clear fails reports CLEAR_FAILED (STAY), not the specific reason. */
static void test_refusal_clear_failure(void)
{
    fake_t f;
    update_install_res_t res;
    fake_init(&f);
    f.stage_ok = false;
    f.clear_rc = -1;
    update_install_ops_t o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_REFUSED && f.clear_calls == 1 &&
                   res.reason == UPD_INST_REASON_CLEAR_FAILED && f.apply_calls == 0,
               "REFUSAL CLEAR: failed request clear is reported as clear_failed");
    fake_init(&f);
    f.stage_ok = false;
    o = ops_for(&f);
    TEST_CHECK(update_install_boot_run(&o, &res) == UPD_INST_BOOT_REFUSED &&
                   res.reason == UPD_INST_REASON_STAGE_NOT_READY && f.have_req == 0,
               "REFUSAL CLEAR: a working clear keeps the specific reason");
}

static int g_erase_calls;
static int g_erase_rc;
static int stale_erase(void) { g_erase_calls++; return g_erase_rc; }

static void refresh_crc(uint8_t b[UPD_INST_RES_SIZE])
{
    uint32_t c = ota_image_crc32(b, UPD_INST_RES_SIZE - 4);
    for (int i = 0; i < 4; i++) {
        b[UPD_INST_RES_SIZE - 4 + i] = (uint8_t)(c >> (8 * i));
    }
}

/* MED-3: result record v1/v2. */
static void test_res_versions(void)
{
    update_install_res_t s, d;
    uint8_t b[UPD_INST_RES_SIZE];
    memset(&s, 0, sizeof(s));
    s.code = UPD_INST_RES_FAIL;
    s.reason = UPD_INST_REASON_SHA_MISMATCH;
    strcpy(s.semver, "1.2.3");
    TEST_CHECK(update_install_res_encode(&s, b) == UPD_INST_RES_SIZE && b[4] == UPD_INST_RES_VERSION_1,
               "RES VERSION: ordinary reason stays v1 (older images still read it)");
    TEST_CHECK(update_install_res_decode(b, sizeof(b), &d) && d.reason == UPD_INST_REASON_SHA_MISMATCH,
               "RES VERSION: v1 decodes");
    s.reason = UPD_INST_REASON_ROLLED_BACK;
    TEST_CHECK(update_install_res_encode(&s, b) == UPD_INST_RES_SIZE && b[4] == UPD_INST_RES_VERSION_2,
               "RES VERSION: rolled_back is written as v2");
    TEST_CHECK(update_install_res_decode(b, sizeof(b), &d) && d.reason == UPD_INST_REASON_ROLLED_BACK &&
                   strcmp(update_install_reason_name(d.reason), "rolled_back") == 0,
               "RES VERSION: v2 decodes");
    b[4] = UPD_INST_RES_VERSION_1; /* forged v1 carrying a v2 reason, CRC recomputed */
    refresh_crc(b);
    TEST_CHECK(!update_install_res_decode(b, sizeof(b), &d), "RES VERSION: v1 with a v2 reason refused");
    b[4] = 3;
    refresh_crc(b);
    TEST_CHECK(!update_install_res_decode(b, sizeof(b), &d), "RES VERSION: unknown version refused");
    /* MED-3 rewrite */
    memset(&s, 0, sizeof(s));
    s.code = UPD_INST_RES_OK;
    strcpy(s.semver, "2.0.0");
    TEST_CHECK(!update_install_res_apply_rollback(&s, false) && s.code == UPD_INST_RES_OK,
               "ROLLBACK: app not rolled back leaves OK alone");
    TEST_CHECK(update_install_res_apply_rollback(&s, true) && s.code == UPD_INST_RES_FAIL &&
                   s.reason == UPD_INST_REASON_ROLLED_BACK,
               "ROLLBACK: OK on a rolled-back app becomes FAIL/rolled_back");
    TEST_CHECK(!update_install_res_apply_rollback(&s, true), "ROLLBACK: a FAIL record is not rewritten");
}

/* recfx leftover: the LCD warning line (40 columns) holds "INSTALL FAILED: " plus the reason. */
static void test_reason_lcd_fits(void)
{
    for (uint8_t r = 0; r < UPD_INST_REASON_COUNT; r++) {
        TEST_CHECK(strlen(update_install_reason_lcd(r)) <= UPD_INST_REASON_LCD_MAX, "LCD REASON: fits the warning line");
    }
    TEST_CHECK(strstr(update_install_reason_lcd(UPD_INST_REASON_OUTCOME_UNKNOWN), "power") != NULL,
               "LCD REASON: outcome_unknown keeps the do-not-power-cycle hint");
    TEST_CHECK(strcmp(update_install_reason_lcd(UPD_INST_REASON_SHA_MISMATCH), "sha_mismatch") == 0,
               "LCD REASON: ordinary reasons unchanged");
}

/* LOW-4: a leftover request/* LOW-4: a leftover request is dropped on a healthy boot, never while the update claim is held. */
static bool g_match_ret;
static int g_match_calls;
static bool match_fn(void *c, const update_install_req_t *r)
{
    (void)c;
    g_match_calls++;
    return g_match_ret && r->image_length == 2956144u;
}

static void test_drop_stale_recorded(void)
{
    fake_t f;
    update_install_ops_t o;
    update_install_res_t d;

    // OA-03: stale request whose image is the running app -> OK recorded, request gone.
    fake_init(&f);
    o = ops_for(&f);
    g_match_ret = true;
    g_match_calls = 0;
    TEST_CHECK(update_install_drop_stale_recorded(true, &o, match_fn, NULL) == 0, "OA-03: match -> 0");
    TEST_CHECK(f.have_req == 0 && f.res_writes == 1, "OA-03: request cleared, one result written");
    TEST_CHECK(update_install_res_decode(f.res_blob, sizeof(f.res_blob), &d) && d.code == UPD_INST_RES_OK &&
                   strcmp(d.semver, "1.0.0-pre.3") == 0 && d.id[0] == 0xA0,
               "OA-03: OK result with the request's semver and id");
    TEST_CHECK(index_of(&f, EV_RES) < index_of(&f, EV_CLEAR), "OA-03: result written before the request is cleared");

    // Running app is not that image -> FAIL outcome_unknown, never OK.
    fake_init(&f);
    o = ops_for(&f);
    g_match_ret = false;
    TEST_CHECK(update_install_drop_stale_recorded(true, &o, match_fn, NULL) == 0, "OA-03: mismatch -> 0");
    TEST_CHECK(update_install_res_decode(f.res_blob, sizeof(f.res_blob), &d) && d.code == UPD_INST_RES_FAIL &&
                   d.reason == UPD_INST_REASON_OUTCOME_UNKNOWN,
               "OA-03: mismatch records FAIL outcome_unknown, not OK");

    // OF-01: recovery's specific FAIL result for the same image survives; only the request is cleared.
    fake_init(&f);
    o = ops_for(&f);
    g_match_ret = false;
    {
        update_install_req_t rq;
        update_install_res_t pre;
        memset(&pre, 0, sizeof(pre));
        TEST_CHECK(update_install_req_decode(f.req, f.req_len, &rq), "OF-01: setup decode");
        TEST_CHECK(update_install_record_result_ex(&o, UPD_INST_RES_FAIL, UPD_INST_REASON_APPLY_FAILED, 3, false,
                                                   rq.semver, rq.sha256) == 0,
                   "OF-01: setup FAIL result");
    }
    int w0 = f.res_writes;
    TEST_CHECK(update_install_drop_stale_recorded(true, &o, match_fn, NULL) == 0 && f.have_req == 0 &&
                   f.res_writes == w0,
               "OF-01: existing FAIL for the same id is kept, request cleared");
    TEST_CHECK(update_install_res_decode(f.res_blob, sizeof(f.res_blob), &d) && d.reason == UPD_INST_REASON_APPLY_FAILED,
               "OF-01: specific reason not overwritten with outcome_unknown");

    // Claim busy: nothing read, nothing written.
    fake_init(&f);
    o = ops_for(&f);
    g_match_calls = 0;
    TEST_CHECK(update_install_drop_stale_recorded(false, &o, match_fn, NULL) == 1 && f.have_req == 1 &&
                   f.res_writes == 0 && g_match_calls == 0,
               "OA-03: claim busy leaves everything alone");

    // No request: nothing recorded.
    fake_init(&f);
    f.have_req = 0;
    o = ops_for(&f);
    TEST_CHECK(update_install_drop_stale_recorded(true, &o, match_fn, NULL) == 0 && f.res_writes == 0,
               "OA-03: no request -> no result");
}

static void test_drop_stale(void)
{
    g_erase_calls = 0;
    g_erase_rc = 0;
    TEST_CHECK(update_install_drop_stale(true, stale_erase) == 0 && g_erase_calls == 1,
               "DROP STALE: claimed -> request erased");
    TEST_CHECK(update_install_drop_stale(false, stale_erase) == 1 && g_erase_calls == 1,
               "DROP STALE: claim busy -> nothing erased");
    g_erase_rc = -1;
    TEST_CHECK(update_install_drop_stale(true, stale_erase) == -1, "DROP STALE: erase failure reported");
    TEST_CHECK(update_install_drop_stale(true, NULL) == -1, "DROP STALE: no erase op fails closed");
}

static void test_pre_boot(void)
{
    TEST_SECTION("update_install_record -- recovery pre/post-boot hooks");
    fake_t f;
    update_install_ops_t o;
    bool ok = false;

    // pre_boot alone never clears the request or writes a result (set_boot may still fail).
    fake_init(&f);
    o = ops_for(&f);
    g_guard_f = &f;
    g_guard_rc = 0;
    TEST_CHECK(update_install_pre_boot(&o, guard_fn) == 0, "pre-boot success returns 0");
    TEST_CHECK(f.have_req == 1 && f.clear_calls == 0 && f.res_writes == 0,
               "pre-boot keeps the request and writes no result (set_boot not done yet)");

    // post_boot (after set_boot OK): request cleared, then OK result.
    TEST_CHECK(update_install_post_boot(&o, "1.0.0-pre.3", id_a, &ok) == 0 && ok, "post-boot success returns 0");
    TEST_CHECK(f.have_req == 0 && f.clear_calls == 1, "request cleared on success");
    TEST_CHECK(f.res_writes == 1, "OK result written");
    update_install_res_t d;
    TEST_CHECK(update_install_res_decode(f.res_blob, sizeof(f.res_blob), &d) && d.code == UPD_INST_RES_OK &&
                   d.reason == UPD_INST_REASON_NONE && strcmp(d.semver, "1.0.0-pre.3") == 0,
               "OK result content");
    TEST_CHECK(index_of(&f, 99) < index_of(&f, EV_CLEAR) && index_of(&f, EV_CLEAR) < index_of(&f, EV_RES),
               "order: boot guard, request clear, then result");

    // set_boot failed: the caller never runs post_boot, so the request survives with no OK record.
    fake_init(&f);
    o = ops_for(&f);
    g_guard_f = &f;
    TEST_CHECK(update_install_pre_boot(&o, guard_fn) == 0, "pre-boot ok");
    /* (set_boot fails here: post_boot is not called) */
    TEST_CHECK(f.have_req == 1 && f.res_writes == 0, "failed set_boot leaves the request and no OK result");

    fake_init(&f);
    f.clear_rc = -1;
    o = ops_for(&f);
    TEST_CHECK(update_install_post_boot(&o, "x", NULL, &ok) == -1 && !ok, "uncleared request is reported");
    TEST_CHECK(f.have_req == 1 && f.res_writes == 0, "request kept, no OK result when clear failed");

    fake_init(&f);
    o = ops_for(&f);
    g_guard_f = &f;
    g_guard_rc = -1;
    TEST_CHECK(update_install_pre_boot(&o, guard_fn) == -1 && f.clear_calls == 0 && f.res_writes == 0,
               "boot guard failure aborts before any clear");
    g_guard_rc = 0;

    fake_init(&f);
    f.res_rc = -1;
    o = ops_for(&f);
    TEST_CHECK(update_install_post_boot(&o, "x", NULL, &ok) == 0 && !ok && f.have_req == 0,
               "result write failure does not block the restart, reported via result_ok");
}

typedef struct {
    int events[8];
    int n;
    bool safe, busy, verified;
    int erase_rc, write_rc;
    int writes;
    bool zero_len;
    bool empty_semver;
    uint8_t blob[UPD_INST_REQ_SIZE];
} app_t;

static void aev(app_t *a, int e) { a->events[a->n++] = e; }
static bool a_safe(void *c) { return ((app_t *)c)->safe; }
static bool a_stage(void *c, bool *busy, uint32_t *len, uint8_t sha[UPD_INST_SHA_LEN], char sv[UPD_INST_SEMVER_LEN])
{
    app_t *a = c;
    *busy = a->busy;
    *len = a->zero_len ? 0 : 1234;
    memset(sha, 0x5A, UPD_INST_SHA_LEN);
    strcpy(sv, a->empty_semver ? "" : "1.0.0-pre.3");
    return a->verified;
}
static int a_erase(void *c) { app_t *a = c; aev(a, 1); return a->erase_rc; }
static int a_write(void *c, const uint8_t *b, size_t n)
{
    app_t *a = c;
    aev(a, 2);
    a->writes++;
    memcpy(a->blob, b, n < sizeof(a->blob) ? n : sizeof(a->blob));
    return a->write_rc;
}

static void test_prepare(void)
{
    TEST_SECTION("update_install_record -- app prepare hook");
    app_t a;
    char msg[96];
    int st = 0;
    update_install_app_ops_t o = { &a, a_safe, a_stage, a_erase, a_write };

    memset(&a, 0, sizeof(a));
    a.safe = true;
    a.verified = true;
    TEST_CHECK(update_install_prepare(&o, msg, sizeof(msg), &st) && a.writes == 1, "verified stage stores a request");
    TEST_CHECK(a.n == 2 && a.events[0] == 1 && a.events[1] == 2, "old result erased BEFORE the request is written");
    update_install_req_t r;
    TEST_CHECK(update_install_req_decode(a.blob, sizeof(a.blob), &r) && r.attempts == 0 && r.image_length == 1234,
               "fresh request, attempts 0, stage length and sha");

    memset(&a, 0, sizeof(a));
    a.safe = true;
    a.verified = false;
    TEST_CHECK(!update_install_prepare(&o, msg, sizeof(msg), &st) && st == 409 && a.writes == 0 && a.n == 0,
               "unverified stage refused 409, nothing erased or written");

    memset(&a, 0, sizeof(a));
    a.safe = true;
    a.verified = true;
    a.busy = true;
    TEST_CHECK(!update_install_prepare(&o, msg, sizeof(msg), &st) && st == 409 && a.writes == 0,
               "busy stage refused 409");

    memset(&a, 0, sizeof(a));
    a.safe = true;
    a.verified = true;
    a.empty_semver = true;
    TEST_CHECK(!update_install_prepare(&o, msg, sizeof(msg), &st) && st == 409 && a.writes == 0 && a.n == 0,
               "verified stage with empty version refused 409, nothing erased or written");

    memset(&a, 0, sizeof(a));
    a.safe = true;
    a.verified = true;
    a.zero_len = true;
    TEST_CHECK(!update_install_prepare(&o, msg, sizeof(msg), &st) && st == 409 && a.writes == 0 && a.n == 0,
               "verified stage with image length 0 refused 409, nothing erased or written");

    memset(&a, 0, sizeof(a));
    a.verified = true;
    TEST_CHECK(!update_install_prepare(&o, msg, sizeof(msg), &st) && st == 500 && a.writes == 0,
               "NVS not writable refused 500");

    memset(&a, 0, sizeof(a));
    a.safe = true;
    a.verified = true;
    a.erase_rc = -1;
    TEST_CHECK(!update_install_prepare(&o, msg, sizeof(msg), &st) && st == 500 && a.writes == 0,
               "result erase failure: request never written");

    memset(&a, 0, sizeof(a));
    a.safe = true;
    a.verified = true;
    a.write_rc = -1;
    TEST_CHECK(!update_install_prepare(&o, msg, sizeof(msg), &st) && st == 500, "request write failure refused 500");
}

static void test_detail_and_after_fail(void)
{
    TEST_SECTION("update_install_record -- result detail, app_modified, post-failure action");
    update_install_res_t s, d;
    uint8_t rb[UPD_INST_RES_SIZE];
    memset(&s, 0, sizeof(s));
    s.code = UPD_INST_RES_FAIL;
    s.reason = UPD_INST_REASON_APPLY_FAILED;
    s.detail = UPD_INST_DETAIL_APPLY_STAGE_HASH;
    s.app_modified = true;
    strcpy(s.semver, "1.0.0");
    TEST_CHECK(update_install_res_encode(&s, rb) == UPD_INST_RES_SIZE, "detail result encodes");
    TEST_CHECK(rb[6] == UPD_INST_REASON_APPLY_FAILED && rb[7] == (UPD_INST_DETAIL_APPLY_STAGE_HASH | 0x80u),
               "reason stays apply_failed; detail code in the low 7 bits of byte 7, modified in bit 7");
    TEST_CHECK(update_install_res_decode(rb, sizeof(rb), &d) && d.detail == UPD_INST_DETAIL_APPLY_STAGE_HASH &&
                   d.app_modified && d.reason == UPD_INST_REASON_APPLY_FAILED,
               "detail and app_modified round trip");
    s.app_modified = false;
    s.detail = UPD_INST_DETAIL_START_NO_MEM;
    TEST_CHECK(update_install_res_encode(&s, rb) == UPD_INST_RES_SIZE && rb[7] == UPD_INST_DETAIL_START_NO_MEM &&
                   update_install_res_decode(rb, sizeof(rb), &d) && d.detail == UPD_INST_DETAIL_START_NO_MEM &&
                   !d.app_modified,
               "untouched-app detail round trips with the flag clear");
    // An older record (byte 7 zero) reads as no detail, app untouched.
    s.detail = 0;
    TEST_CHECK(update_install_res_encode(&s, rb) == UPD_INST_RES_SIZE && rb[7] == 0 &&
                   update_install_res_decode(rb, sizeof(rb), &d) && d.detail == 0 && !d.app_modified,
               "older record (byte 7 zero) decodes as no detail");
    TEST_CHECK(strcmp(update_install_detail_name(UPD_INST_DETAIL_APPLY_STAGE_HASH), "stage_hash_mismatch") == 0 &&
                   strcmp(update_install_detail_name(UPD_INST_DETAIL_APPLY_SET_BOOT), "set_boot_failed") == 0 &&
                   strcmp(update_install_detail_name(UPD_INST_DETAIL_START_NO_MEM), "start_low_heap") == 0 &&
                   strcmp(update_install_detail_name(0), "none") == 0 && strcmp(update_install_detail_name(99), "unknown") == 0,
               "detail names");

    // record_result_ex stores what it is given; the plain variant records no detail.
    fake_t f;
    fake_init(&f);
    update_install_ops_t o = ops_for(&f);
    TEST_CHECK(update_install_record_result_ex(&o, UPD_INST_RES_FAIL, UPD_INST_REASON_APPLY_FAILED,
                                               UPD_INST_DETAIL_APPLY_SET_BOOT, true, "1.0.0", NULL) == 0 &&
                   update_install_res_decode(f.res_blob, sizeof(f.res_blob), &d) &&
                   d.detail == UPD_INST_DETAIL_APPLY_SET_BOOT && d.app_modified,
               "record_result_ex writes detail and app_modified");
    TEST_CHECK(update_install_record_result(&o, UPD_INST_RES_FAIL, UPD_INST_REASON_APPLY_FAILED, "1.0.0", NULL) == 0 &&
                   update_install_res_decode(f.res_blob, sizeof(f.res_blob), &d) && d.detail == 0 && !d.app_modified,
               "record_result writes no detail");

    // A refused apply_start carries its own detail and leaves app_modified clear (heap floor is not "apply_failed" alone).
    fake_init(&f);
    o = ops_for(&f);
    update_install_res_t out;
    f.apply_rc = UPD_INST_DETAIL_START_NO_MEM;
    TEST_CHECK(update_install_boot_run(&o, &out) == UPD_INST_BOOT_REFUSED && out.reason == UPD_INST_REASON_APPLY_FAILED &&
                   out.detail == UPD_INST_DETAIL_START_NO_MEM && !out.app_modified,
               "apply_start heap refusal reported with its own detail");
    TEST_CHECK(update_install_res_decode(f.res_blob, sizeof(f.res_blob), &d) && d.detail == UPD_INST_DETAIL_START_NO_MEM &&
                   !d.app_modified,
               "apply_start heap refusal persisted with its own detail");
    fake_init(&f);
    o = ops_for(&f);
    f.apply_rc = -1;
    TEST_CHECK(update_install_boot_run(&o, &out) == UPD_INST_BOOT_REFUSED && out.detail == UPD_INST_DETAIL_START_FAILED,
               "unclassified apply_start refusal is start_failed");
    fake_init(&f);
    o = ops_for(&f);
    f.apply_rc = 5; // an apply-result number is not a start code
    TEST_CHECK(update_install_boot_run(&o, &out) == UPD_INST_BOOT_REFUSED && out.detail == UPD_INST_DETAIL_START_FAILED,
               "an out-of-range apply_start code is start_failed");

    // update_install_after_fail: the unattended-recovery decision.
    const uint8_t AF = UPD_INST_REASON_APPLY_FAILED;
    TEST_CHECK(update_install_after_fail(AF, false, true, true) == UPD_INST_AFTER_EXIT_APP,
               "app untouched and verifies: exit to the app");
    TEST_CHECK(update_install_after_fail(AF, false, true, false) == UPD_INST_AFTER_EXIT_APP,
               "app untouched, verifies, no attempts left: exit to the app");
    TEST_CHECK(update_install_after_fail(AF, false, false, true) == UPD_INST_AFTER_RESTART,
               "app untouched but does not verify: never boot it; retry while attempts remain");
    TEST_CHECK(update_install_after_fail(AF, false, false, false) == UPD_INST_AFTER_STAY,
               "app does not verify and no attempts left: stay in recovery");
    TEST_CHECK(update_install_after_fail(AF, true, false, true) == UPD_INST_AFTER_RESTART,
               "app modified, attempts left: restart so the bounded retry advances");
    TEST_CHECK(update_install_after_fail(AF, true, true, true) == UPD_INST_AFTER_RESTART,
               "app modified and verifies but attempts remain: retry the switch");
    TEST_CHECK(update_install_after_fail(AF, true, true, false) == UPD_INST_AFTER_EXIT_APP,
               "attempts exhausted and the app verifies: exit to it");
    TEST_CHECK(update_install_after_fail(AF, true, false, false) == UPD_INST_AFTER_STAY,
               "attempts exhausted and the app does not verify: stay");
    TEST_CHECK(update_install_after_fail(UPD_INST_REASON_STAGE_NOT_READY, false, true, false) == UPD_INST_AFTER_EXIT_APP &&
                   update_install_after_fail(UPD_INST_REASON_SHA_MISMATCH, false, true, false) == UPD_INST_AFTER_EXIT_APP &&
                   update_install_after_fail(UPD_INST_REASON_SIZE_MISMATCH, false, true, false) == UPD_INST_AFTER_EXIT_APP &&
                   update_install_after_fail(UPD_INST_REASON_RECORD_BAD, false, true, false) == UPD_INST_AFTER_EXIT_APP,
               "pre-apply refusals with a verified app exit to it");
    TEST_CHECK(update_install_after_fail(UPD_INST_REASON_ATTEMPTS_EXCEEDED, true, true, false) == UPD_INST_AFTER_EXIT_APP &&
                   update_install_after_fail(UPD_INST_REASON_ATTEMPTS_EXCEEDED, true, false, false) == UPD_INST_AFTER_STAY,
               "attempts_exceeded exits only to a verified app");
    TEST_CHECK(update_install_after_fail(UPD_INST_REASON_NVS_UNAVAILABLE, false, true, true) == UPD_INST_AFTER_STAY &&
                   update_install_after_fail(UPD_INST_REASON_COUNTER_FAILED, false, true, true) == UPD_INST_AFTER_STAY &&
                   update_install_after_fail(UPD_INST_REASON_RESULT_WRITE_FAILED, false, true, true) == UPD_INST_AFTER_STAY &&
                   update_install_after_fail(UPD_INST_REASON_OUTCOME_UNKNOWN, false, true, true) == UPD_INST_AFTER_STAY,
               "storage failures and an unknown outcome never exit or restart (no loop)");
}

void run_test_update_install_record(void)
{
    test_detail_and_after_fail();
    test_codec();
    test_boot_logic();
    test_pre_boot();
    test_drop_stale();
    test_drop_stale_recorded();
    test_reason_lcd_fits();
    test_res_versions();
    test_refusal_clear_failure();
    test_result_identity();
    test_prepare();
}
