// Host tests for App/drivers/update/update_install_record.c: the one-shot
// unattended-install request/result codec and the recovery-side boot logic.
#include <string.h>

#include "test_common.h"

#include "../drivers/update/update_install_record.h"

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

static update_install_ops_t ops_for(fake_t *f)
{
    update_install_ops_t o;
    o.ctx = f;
    o.req_read = f_read;
    o.req_clear = f_clear;
    o.req_write = f_write;
    o.res_write = f_res;
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
    rb[5] = 9;
    TEST_CHECK(!update_install_res_decode(rb, sizeof(rb), &d), "tampered result refused (CRC)");
    TEST_CHECK(strcmp(update_install_reason_name(UPD_INST_REASON_SHA_MISMATCH), "sha_mismatch") == 0,
               "reason name");
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
    TEST_CHECK(update_install_record_result(&o, UPD_INST_RES_FAIL, UPD_INST_REASON_APPLY_FAILED, "x") != 0,
               "failed result write is reported by record_result");

    fake_init(&f);
    o = ops_for(&f);
    TEST_CHECK(update_install_record_result(&o, UPD_INST_RES_OK, UPD_INST_REASON_NONE, "1.0.0-pre.3") == 0,
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

static void test_pre_boot(void)
{
    TEST_SECTION("update_install_record -- recovery pre-boot hook (success path)");
    fake_t f;
    update_install_ops_t o;
    bool ok = false;

    fake_init(&f);
    o = ops_for(&f);
    g_guard_f = &f;
    g_guard_rc = 0;
    TEST_CHECK(update_install_pre_boot(&o, guard_fn, "1.0.0-pre.3", &ok) == 0 && ok, "success path returns 0");
    TEST_CHECK(f.have_req == 0 && f.clear_calls == 1, "request cleared on success");
    TEST_CHECK(f.res_writes == 1 && update_install_res_decode(f.res_blob, sizeof(f.res_blob), &(update_install_res_t){0}),
               "OK result written");
    update_install_res_t d;
    TEST_CHECK(update_install_res_decode(f.res_blob, sizeof(f.res_blob), &d) && d.code == UPD_INST_RES_OK &&
                   d.reason == UPD_INST_REASON_NONE && strcmp(d.semver, "1.0.0-pre.3") == 0,
               "OK result content");
    TEST_CHECK(index_of(&f, 99) < index_of(&f, EV_CLEAR) && index_of(&f, EV_CLEAR) < index_of(&f, EV_RES),
               "order: boot guard, request clear, then result");

    fake_init(&f);
    f.clear_rc = -1;
    o = ops_for(&f);
    g_guard_f = &f;
    TEST_CHECK(update_install_pre_boot(&o, guard_fn, "x", &ok) == -1 && !ok, "uncleared request aborts boot switch");
    TEST_CHECK(f.have_req == 1 && f.res_writes == 0, "request kept, no OK result when clear failed");

    fake_init(&f);
    o = ops_for(&f);
    g_guard_f = &f;
    g_guard_rc = -1;
    TEST_CHECK(update_install_pre_boot(&o, guard_fn, "x", &ok) == -1 && f.clear_calls == 0 && f.res_writes == 0,
               "boot guard failure aborts before any clear");
    g_guard_rc = 0;

    fake_init(&f);
    f.res_rc = -1;
    o = ops_for(&f);
    g_guard_f = &f;
    TEST_CHECK(update_install_pre_boot(&o, guard_fn, "x", &ok) == 0 && !ok && f.have_req == 0,
               "result write failure does not block the switch, reported via result_ok");
}

typedef struct {
    int events[8];
    int n;
    bool safe, busy, verified;
    int erase_rc, write_rc;
    int writes;
    uint8_t blob[UPD_INST_REQ_SIZE];
} app_t;

static void aev(app_t *a, int e) { a->events[a->n++] = e; }
static bool a_safe(void *c) { return ((app_t *)c)->safe; }
static bool a_stage(void *c, bool *busy, uint32_t *len, uint8_t sha[UPD_INST_SHA_LEN], char sv[UPD_INST_SEMVER_LEN])
{
    app_t *a = c;
    *busy = a->busy;
    *len = 1234;
    memset(sha, 0x5A, UPD_INST_SHA_LEN);
    strcpy(sv, "1.0.0-pre.3");
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

void run_test_update_install_record(void)
{
    test_codec();
    test_boot_logic();
    test_pre_boot();
    test_prepare();
}
