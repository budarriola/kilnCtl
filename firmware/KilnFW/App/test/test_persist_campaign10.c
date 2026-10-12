// Host test campaign 10 (docs/audits/HOST_TEST_CAMPAIGN_FINDINGS_2026-10-09.md):
// persist stores and parsers the coverage-gaps doc lists as untested/lightly
// tested: touch_cal_store.c (row 15), backup_json.c (row 21), ct_verify_store.c
// and the pref_cfg_fs.c read/resolve layer they sit on. Real modules over
// fake_kv + a real cfg_fs scratch directory.
//
// "CHARACTERIZATION K10-xx" checks pin CURRENT behavior that campaign 10 judged
// a defect; they keep the suite green and fail loudly if the behavior changes,
// at which point the finding should be marked fixed and the check inverted.
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "test_common.h"

#include "esp_err.h"
#include "fake_kv.h"
#include "cfg_fs.h"
#include "pref_cfg_fs.h"
#include "backup_json.h"
#include "touch_cal_store.h"
#include "ct_verify_store.h"

#ifdef _WIN32
#include <direct.h>
#define C10_MKDIR(p) _mkdir(p)
#define C10_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define C10_MKDIR(p) mkdir((p), 0755)
#define C10_RMDIR(p) rmdir(p)
#endif

int g_test_failures = 0;
int g_test_count = 0;

size_t persist_scratch_test_fail_size = 0;
int persist_scratch_test_fail_nth = 0;
int persist_scratch_test_seen = 0;

static void arm_oom(size_t size, int nth)
{
    persist_scratch_test_fail_size = size;
    persist_scratch_test_fail_nth = nth;
    persist_scratch_test_seen = 0;
}
static void disarm_oom(void)
{
    persist_scratch_test_fail_nth = 0;
    persist_scratch_test_seen = 0;
}

/* ======================= backup_json ======================= */

static void test_backup_json(void)
{
    TEST_SECTION("backup_json -- malformed, nested, escaped, truncated");
    const char *o = "{\"a\":1,\"n\":{\"a\":2,\"x\":[1,{\"a\":3}]},\"s\":\"q\\\"k\",\"z\":4}";
    double d = 0;
    TEST_CHECK(backup_json_field_num(o, "a", &d) && d == 1, "top-level key wins over nested same-name key");
    TEST_CHECK(backup_json_field_num(o, "z", &d) && d == 4, "key after nested object/array and escaped quote is found");
    TEST_CHECK(!backup_json_obj_find(o, "missing"), "absent key -> NULL");
    char s[16];
    TEST_CHECK(backup_json_field_str(o, "s", s, sizeof(s)) && strcmp(s, "q\"k") == 0, "escaped quote unescaped");

    TEST_CHECK(!backup_json_obj_find("[1,2]", "a"), "non-object rejected");
    TEST_CHECK(!backup_json_obj_find("{\"a\" 1}", "a"), "missing colon rejected");
    TEST_CHECK(!backup_json_obj_find("{a:1}", "a"), "unquoted key rejected");
    TEST_CHECK(!backup_json_obj_find("{\"a\":1 \"b\":2}", "b"), "missing comma rejected");
    TEST_CHECK(!backup_json_obj_find("{\"a\":", "b"), "truncated object terminates (no hang)");
    d = -1;
    TEST_CHECK(!backup_json_field_num("{\"a\":", "a", &d) && d == -1, "truncated number rejected, out untouched");
    TEST_CHECK(!backup_json_field_num("{\"a\":\"5\"}", "a", &d), "string where number expected rejected");
    TEST_CHECK(!backup_json_field_num("{\"a\":nan}", "a", &d), "nan rejected");
    TEST_CHECK(!backup_json_field_num("{\"a\":1e999}", "a", &d), "overflow to inf rejected");
    TEST_CHECK(!backup_json_field_num("{\"a\":null}", "a", &d), "null rejected as number");

    const char *arr = "[1, [2,3], \"x,]\", {\"k\":[4]}]";
    const char *e = backup_json_arr_first(arr);
    int n = 0;
    while (e) {
        n++;
        e = backup_json_arr_next(e);
    }
    TEST_CHECK(n == 4, "array of 4 mixed elements iterated (commas in strings/nesting ignored)");
    TEST_CHECK(backup_json_arr_first("[ ]") == NULL, "empty array -> NULL");
    TEST_CHECK(backup_json_arr_first("{}") == NULL, "non-array -> NULL");
    TEST_CHECK(backup_json_arr_first(NULL) == NULL, "NULL array -> NULL");
    e = backup_json_arr_first("[1,2");
    n = 0;
    while (e && n < 10) {
        n++;
        e = backup_json_arr_next(e);
    }
    TEST_CHECK(n == 2 && e == NULL, "truncated array terminates");

    static char deep[20005];
    memset(deep, '[', 10000);
    memset(deep + 10000, ']', 10000);
    deep[20000] = 0;
    TEST_CHECK(*backup_json_skip_value(deep) == '\0', "10000-deep nesting skipped iteratively");
    const char *g = "\x01\x02";
    TEST_CHECK(backup_json_skip_value(g) > g, "garbage still advances");

    char err[64] = "";
    bool has = true;
    TEST_CHECK(backup_json_field_opt_num("{}", "k", 0, 10, &d, &has, "k", err, sizeof(err), 0) && !has,
               "absent optional ok, has=false");
    TEST_CHECK(!backup_json_field_opt_num("{\"k\":11}", "k", 0, 10, &d, &has, "k", err, sizeof(err), 2) && err[0],
               "out-of-range optional is an error");
    TEST_CHECK(!backup_json_field_opt_num("{\"k\":\"x\"}", "k", 0, 10, &d, &has, "k", err, sizeof(err), 2),
               "present-but-malformed optional is an error");
    TEST_CHECK(backup_json_field_opt_num("{\"k\":10}", "k", 0, 10, &d, &has, "k", err, sizeof(err), 0) && has && d == 10,
               "inclusive upper bound");

    /* ---- characterization of defects ---- */
    char small[4];
    bool r = backup_json_field_str("{\"s\":\"abcdefgh\"}", "s", small, sizeof(small));
    TEST_CHECK(!r && small[0] == '\0', "K10-01: field_str over-long string is an error, not a truncation");
    r = backup_json_field_str("{\"s\":\"abc\"}", "s", small, sizeof(small));
    TEST_CHECK(r && strcmp(small, "abc") == 0, "K10-01: exact-fit string accepted");
    r = backup_json_field_str("{\"s\":\"abc", "s", small, sizeof(small));
    TEST_CHECK(r == false, "K10-02: field_str rejects an unterminated string (truncated backup)");
    r = backup_json_field_str("{\"s\":\"A\\u0042\"}", "s", s, sizeof(s));
    TEST_CHECK(r && strcmp(s, "AB") == 0, "K10-03: \\u0042 decodes to 'B'");
    r = backup_json_field_str("{\"s\":\"\\u00e9\\u20ac\"}", "s", s, sizeof(s));
    TEST_CHECK(r && strcmp(s, "\xC3\xA9\xE2\x82\xAC") == 0, "K10-03: \\u escapes above 0x7F decode to UTF-8");
    TEST_CHECK(!backup_json_field_str("{\"s\":\"\\u00zz\"}", "s", s, sizeof(s)), "K10-03: bad hex digits rejected");
    TEST_CHECK(!backup_json_field_str("{\"s\":\"\\q\"}", "s", s, sizeof(s)), "K10-03: unknown escape rejected");
    TEST_CHECK(backup_json_field_str("{\"s\":\"a\\n\\\"b\"}", "s", s, sizeof(s)) && strcmp(s, "a\n\"b") == 0,
               "standard escapes still decode");
    d = 0;
    r = backup_json_field_num("{\"a\":0x1F}", "a", &d);
    TEST_CHECK(!r, "K10-04: field_num rejects hex syntax");
    TEST_CHECK(!backup_json_field_num("{\"a\":inf}", "a", &d) && !backup_json_field_num("{\"a\":+1}", "a", &d),
               "K10-04: inf and leading '+' rejected");
    d = 0;
    r = backup_json_field_num("{\"a\":12abc}", "a", &d);
    TEST_CHECK(!r, "K10-05: field_num rejects trailing garbage after the number");
    TEST_CHECK(backup_json_field_num("{\"a\":-1.5e2}", "a", &d) && d == -150.0 &&
                   backup_json_field_num("{\"a\": 7 }", "a", &d) && d == 7.0 &&
                   backup_json_field_num("{\"a\":0}", "a", &d) && d == 0.0,
               "normal numbers (exponent, spaces, zero) still accepted");
    r = backup_json_field_num("{\"a\":1,\"a\":2}", "a", &d);
    TEST_CHECK(r && d == 1.0, "duplicate key: first wins (documented)");
}

/* ======================= touch_cal_store ======================= */

typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    float a, b, c, d, e, f;
} tc_rec_t;

static void put_tc_raw(const void *p, size_t len)
{
    hal_kv_handle_t h;
    hal_kv_init_partition("kiln_nvs");
    TEST_CHECK(hal_kv_open(&h, "touch_cal", HAL_KV_MODE_READ_WRITE, "kiln_nvs") == HAL_OK, "open for seed");
    TEST_CHECK(hal_kv_set_blob(&h, "affine_v1", p, len) == HAL_OK, "seed blob");
    hal_kv_commit(&h);
    hal_kv_close(&h);
}

static void test_touch_cal(void)
{
    TEST_SECTION("touch_cal_store -- load/save/version/size/corruption");
    touch_cal_t c;
    fake_kv_reset_all();
    TEST_CHECK(touch_cal_store_load(&c) == ESP_OK && !c.calibrated, "fresh board: ESP_OK, uncalibrated");
    TEST_CHECK(touch_cal_store_load(NULL) == ESP_ERR_INVALID_ARG, "NULL out rejected");
    TEST_CHECK(touch_cal_store_save(NULL) == ESP_ERR_INVALID_ARG, "NULL save rejected");

    touch_cal_t in = { .calibrated = true, .a = 1.5f, .b = 0.25f, .c = -3.f, .d = 0.1f, .e = 2.f, .f = 7.f };
    TEST_CHECK(touch_cal_store_save(&in) == ESP_OK, "save ok");
    TEST_CHECK(touch_cal_store_load(&c) == ESP_OK && c.calibrated && c.a == 1.5f && c.b == 0.25f && c.c == -3.f &&
                   c.d == 0.1f && c.e == 2.f && c.f == 7.f,
               "round trip is bit exact");
    TEST_CHECK(touch_cal_store_is_calibrated(), "is_calibrated true");

    tc_rec_t r = { .version = 2, .a = 1, .e = 1 };
    put_tc_raw(&r, sizeof(r));
    TEST_CHECK(touch_cal_store_load(&c) != ESP_OK && !c.calibrated, "newer version -> uncalibrated, error code");
    r.version = 0;
    put_tc_raw(&r, sizeof(r));
    TEST_CHECK(touch_cal_store_load(&c) != ESP_OK && !c.calibrated, "version 0 -> uncalibrated, error code");
    r.version = 1;
    put_tc_raw(&r, sizeof(r) - 4);
    TEST_CHECK(touch_cal_store_load(&c) != ESP_OK && !c.calibrated, "truncated record -> uncalibrated, error code");
    uint8_t big[64];
    memset(big, 0, sizeof(big));
    big[0] = 1;
    put_tc_raw(big, sizeof(big));
    TEST_CHECK(touch_cal_store_load(&c) != ESP_OK && !c.calibrated, "oversized record -> uncalibrated, error code");

    touch_cal_store_save(&in);
    fake_kv_script_corrupt_key("kiln_nvs", "touch_cal", "affine_v1");
    TEST_CHECK(touch_cal_store_load(&c) != ESP_OK && !c.calibrated,
               "K10-06: corrupt (HAL_IO) record is uncalibrated WITH an error code");
    fake_kv_reset_all();
    TEST_CHECK(touch_cal_store_load(&c) == ESP_OK && !c.calibrated, "K10-06: fresh board still ESP_OK uncalibrated");

    fake_kv_reset_all();
    fake_kv_script_next_write_status(HAL_NO_MEM);
    TEST_CHECK(touch_cal_store_save(&in) != ESP_OK, "set_blob failure reported");
    TEST_CHECK(!touch_cal_store_is_calibrated(), "failed save left nothing behind");
    fake_kv_reset_all();
    fake_kv_script_write_status_after(1, HAL_IO);
    TEST_CHECK(touch_cal_store_save(&in) != ESP_OK, "commit failure reported");
    fake_kv_reset_all();
    fake_kv_script_silent_set_noops(1);
    esp_err_t se = touch_cal_store_save(&in);
    TEST_CHECK(se != ESP_OK && !touch_cal_store_is_calibrated(),
               "K10-07: save reads back; a write that lies OK is reported as a failure");

    fake_kv_reset_all();
    touch_cal_t nan_in = in;
    nan_in.a = NAN;
    TEST_CHECK(touch_cal_store_save(&nan_in) != ESP_OK && !touch_cal_store_is_calibrated(),
               "K10-08: NaN coefficient refused at save");
    {
        tc_rec_t nr = { .version = 1, .a = NAN, .e = 1 };
        put_tc_raw(&nr, sizeof(nr));
        TEST_CHECK(touch_cal_store_load(&c) != ESP_OK && !c.calibrated, "K10-08: NaN record loads uncalibrated");
    }
    fake_kv_reset_all();
    fake_kv_reset_all();
    memset(&r, 0, sizeof(r));
    r.version = 1;
    put_tc_raw(&r, sizeof(r));
    TEST_CHECK(touch_cal_store_load(&c) != ESP_OK && !c.calibrated,
               "K10-08b: all-zero v1 record (degenerate map) is not calibrated");

    uint16_t rx[3] = { 100, 900, 100 }, ry[3] = { 100, 100, 900 };
    int32_t sx[3] = { 0, 319, 0 }, sy[3] = { 0, 0, 479 };
    TEST_CHECK(touch_cal_fit(rx, ry, sx, sy, 3, &c) == ESP_OK, "3-point fit ok");
    int32_t ox, oy;
    touch_cal_apply(&c, 900, 900, 320, 480, &ox, &oy);
    TEST_CHECK(ox == 319 && oy == 479, "fit maps 4th corner");
    uint16_t cx[3] = { 100, 200, 300 }, cy[3] = { 100, 200, 300 };
    TEST_CHECK(touch_cal_fit(cx, cy, sx, sy, 3, &c) == ESP_ERR_INVALID_ARG, "collinear points rejected");
    TEST_CHECK(touch_cal_fit(rx, ry, sx, sy, 2, &c) == ESP_ERR_INVALID_ARG, "n<3 rejected");
}

/* ======================= pref_cfg_fs / ct_verify_store ======================= */

static const char *BASE = "cfg_fs_test_persist_c10";

static void rm_file(const char *name)
{
    char p[300];
    snprintf(p, sizeof(p), "%s/.tmp/%s", BASE, name);
    remove(p);
    snprintf(p, sizeof(p), "%s/%s", BASE, name);
    remove(p);
}

static void fresh(void)
{
    rm_file("ct_verify.bin");
    rm_file("big.bin");
    rm_file("small.bin");
    char t[300];
    snprintf(t, sizeof(t), "%s/.tmp", BASE);
    C10_RMDIR(t);
    C10_RMDIR(BASE);
    C10_MKDIR(BASE);
    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
    pref_cfg_fs_clear_rev_unknown_for_test();
    cfg_fs_test_inject_read_error(NULL, ESP_OK, 0);
    fake_kv_reset_all();
    fake_kv_set_write_safe_here(true);
    hal_kv_init_partition("kiln_nvs");
    disarm_oom();
    TEST_CHECK(cfg_fs_init(BASE, NULL) == ESP_OK, "cfg_fs mounts");
}

static bool v_first1(const void *b, size_t n)
{
    (void)n;
    return ((const uint8_t *)b)[0] == 1;
}

static esp_err_t failing_write(const char *p, const void *d, size_t n)
{
    (void)p;
    (void)d;
    (void)n;
    return ESP_FAIL;
}

static void test_resolve_io_error_keeps_nvs_and_refuses_saves(void)
{
    TEST_SECTION("pref_cfg_fs M1: I/O error reading a present cfg file keeps the NVS copy, marks rev unknown, "
                 "refuses saves until a clean resolve");
    fresh();
    uint8_t file[8], nvs[8], out[8];
    memset(file, 5, sizeof(file));
    file[0] = 1;
    memset(nvs, 6, sizeof(nvs));
    nvs[0] = 1;
    uint32_t rev = 0;
    bool used = true;
    TEST_CHECK(pref_cfg_fs_save("small.bin", file, sizeof(file), 9) == ESP_OK, "setup: file rev 9");
    cfg_fs_test_inject_read_error("small.bin", ESP_FAIL, 1);
    bool ok = pref_cfg_fs_resolve("small.bin", nvs, sizeof(nvs), true, 3, v_first1, out, &rev, &used);
    TEST_CHECK(ok && !used && rev == 3 && memcmp(out, nvs, sizeof(nvs)) == 0,
               "M1: transient I/O error: valid NVS copy adopted in RAM, not zeroed defaults / rev 0");
    TEST_CHECK(pref_cfg_fs_rev_unknown("small.bin"), "M1: rev marked unknown");
    TEST_CHECK(pref_cfg_fs_save("small.bin", nvs, sizeof(nvs), 4) == ESP_ERR_INVALID_STATE &&
                   pref_cfg_fs_commit("small.bin", nvs, sizeof(nvs), 4, "t") == ESP_ERR_INVALID_STATE,
               "M1: save and commit refused while the rev is unknown");
    uint8_t back[8];
    bool valid = false;
    pref_cfg_fs_load_raw("small.bin", sizeof(back), v_first1, back, &rev, &valid);
    TEST_CHECK(valid && rev == 9 && back[1] == 5, "M1: the unreadable file was not overwritten");
    TEST_CHECK(pref_cfg_fs_save("other.bin", nvs, sizeof(nvs), 1) == ESP_OK, "M1: other paths still save");
    /* No valid NVS: still defaults (false) and still refuses. */
    cfg_fs_test_inject_read_error("small.bin", ESP_FAIL, 1);
    ok = pref_cfg_fs_resolve("small.bin", nvs, sizeof(nvs), false, 0, v_first1, out, &rev, &used);
    TEST_CHECK(!ok && pref_cfg_fs_save("small.bin", nvs, sizeof(nvs), 10) == ESP_ERR_INVALID_STATE,
               "M1: no NVS candidate: false, saves still refused");
    /* A clean resolve (file higher rev) adopts the file and clears the mark. */
    ok = pref_cfg_fs_resolve("small.bin", nvs, sizeof(nvs), true, 3, v_first1, out, &rev, &used);
    TEST_CHECK(ok && used && rev == 9 && !pref_cfg_fs_rev_unknown("small.bin"),
               "M1: a clean resolve clears the mark and adopts the higher-rev file");
    TEST_CHECK(pref_cfg_fs_save("small.bin", out, sizeof(out), 10) == ESP_OK, "M1: saves work again");
}

static void test_pref_cfg_fs(void)
{
    TEST_SECTION("pref_cfg_fs -- truncated/oversized/wrong-version/bad-validate, write failure, OOM");
    uint8_t item[40], out[40];
    memset(item, 7, sizeof(item));
    item[0] = 1;
    uint32_t rev;
    bool valid;

    fresh();
    TEST_CHECK(pref_cfg_fs_save("small.bin", item, sizeof(item), 5) == ESP_OK, "save ok");
    pref_cfg_fs_load_raw("small.bin", sizeof(item), v_first1, out, &rev, &valid);
    TEST_CHECK(valid && rev == 5 && memcmp(out, item, sizeof(item)) == 0, "round trip");
    pref_cfg_fs_load_raw("small.bin", sizeof(item) - 1, v_first1, out, &rev, &valid);
    TEST_CHECK(!valid && rev == 0, "reader expecting smaller size: file ignored (size mismatch)");
    pref_cfg_fs_load_raw("small.bin", sizeof(item) + 1, v_first1, out, &rev, &valid);
    TEST_CHECK(!valid, "reader expecting larger size: file ignored");
    item[0] = 9;
    pref_cfg_fs_save("small.bin", item, sizeof(item), 6);
    pref_cfg_fs_load_raw("small.bin", sizeof(item), v_first1, out, &rev, &valid);
    TEST_CHECK(!valid, "validator rejection ignores file");
    TEST_CHECK(pref_cfg_fs_save("small.bin", item, 0, 1) == ESP_ERR_INVALID_ARG, "zero size rejected");
    TEST_CHECK(pref_cfg_fs_save("small.bin", item, PREF_CFG_FS_MAX_LARGE_ITEM + 1, 1) == ESP_ERR_INVALID_SIZE,
               "oversized item refused");
    TEST_CHECK(pref_cfg_fs_save("small.bin", NULL, 4, 1) == ESP_ERR_INVALID_ARG, "NULL item refused");

    item[0] = 1;
    item[1] = 11;
    pref_cfg_fs_save("small.bin", item, sizeof(item), 7);
    pref_cfg_fs_set_write_fn(failing_write);
    uint8_t item2[40];
    memcpy(item2, item, sizeof(item));
    item2[1] = 22;
    TEST_CHECK(pref_cfg_fs_commit("small.bin", item2, sizeof(item2), 8, "t") == ESP_FAIL,
               "commit reports write failure");
    pref_cfg_fs_reset_write_fn_for_test();
    pref_cfg_fs_load_raw("small.bin", sizeof(item), v_first1, out, &rev, &valid);
    TEST_CHECK(valid && rev == 7 && out[1] == 11, "failed write left previous file untouched");

    {
        char p[300];
        snprintf(p, sizeof(p), "%s/small.bin", BASE);
        FILE *f = fopen(p, "wb");
        fwrite("\x07\0\0\0\x01", 1, 5, f);
        fclose(f);
        pref_cfg_fs_load_raw("small.bin", sizeof(item), v_first1, out, &rev, &valid);
        TEST_CHECK(!valid, "truncated file on disk ignored");
        f = fopen(p, "wb");
        for (int i = 0; i < 300; i++) fputc(1, f);
        fclose(f);
        pref_cfg_fs_load_raw("small.bin", sizeof(item), v_first1, out, &rev, &valid);
        TEST_CHECK(!valid, "oversized file on disk ignored");
    }

    /* resolve: NVS/file disagreement */
    fresh();
    uint8_t nvs[40];
    memset(nvs, 3, sizeof(nvs));
    nvs[0] = 1;
    uint8_t fil[40];
    memset(fil, 4, sizeof(fil));
    fil[0] = 1;
    uint8_t res[40];
    bool used;
    pref_cfg_fs_save("small.bin", fil, sizeof(fil), 5);
    TEST_CHECK(pref_cfg_fs_resolve("small.bin", nvs, sizeof(nvs), true, 2, v_first1, res, &rev, &used) && used &&
                   res[1] == 4 && rev == 5,
               "file rev higher wins");
    TEST_CHECK(pref_cfg_fs_resolve("small.bin", nvs, sizeof(nvs), true, 5, v_first1, res, &rev, &used) && !used &&
                   res[1] == 3,
               "equal rev, differing bytes: NVS wins (documented tie-break)");
    pref_cfg_fs_load_raw("small.bin", sizeof(fil), v_first1, out, &rev, &valid);
    TEST_CHECK(valid && out[1] == 3, "...and the file is resynced from NVS");
    TEST_CHECK(pref_cfg_fs_resolve("small.bin", nvs, sizeof(nvs), false, 0, v_first1, res, &rev, &used) && used,
               "NVS invalid, file valid: file used");
    pref_cfg_fs_remove("small.bin");
    TEST_CHECK(!pref_cfg_fs_resolve("small.bin", nvs, sizeof(nvs), false, 0, v_first1, res, &rev, &used),
               "both absent -> false");

    /* ---- OOM on large items (> PREF_CFG_FS_MAX_ITEM) ---- */
    fresh();
    enum { BIG = 200 };
    uint8_t big[BIG], bout[BIG], bnvs[BIG];
    memset(big, 5, BIG);
    big[0] = 1;
    memset(bnvs, 6, BIG);
    bnvs[0] = 1;
    TEST_CHECK(pref_cfg_fs_save("big.bin", big, BIG, 9) == ESP_OK, "big save ok");
    arm_oom(4 + BIG, 1);
    TEST_CHECK(pref_cfg_fs_save("big.bin", big, BIG, 10) == ESP_ERR_NO_MEM, "save alloc failure reports NO_MEM");
    disarm_oom();
    arm_oom(4 + BIG, 1);
    TEST_CHECK(pref_cfg_fs_load_raw_checked("big.bin", BIG, v_first1, bout, &rev, &valid) == ESP_ERR_NO_MEM && !valid,
               "K10-09: load_raw_checked reports alloc failure as NO_MEM, not absent");
    disarm_oom();
    TEST_CHECK(pref_cfg_fs_load_raw_checked("nofile.bin", BIG, v_first1, bout, &rev, &valid) == ESP_OK && !valid,
               "K10-09: truly absent file is ESP_OK / invalid");
    size_t ol;
    arm_oom(4 + BIG, 1);
    TEST_CHECK(pref_cfg_fs_load_var_checked("big.bin", bout, BIG, &ol, &rev) == ESP_ERR_NO_MEM,
               "K10-09b: load_var_checked reports alloc failure as NO_MEM");
    disarm_oom();
    TEST_CHECK(pref_cfg_fs_load_var_checked("nofile.bin", bout, BIG, &ol, &rev) == ESP_ERR_NOT_FOUND,
               "K10-09b: absent file is NOT_FOUND");
    arm_oom(4 + BIG, 1);
    bool rok = pref_cfg_fs_resolve("big.bin", bnvs, BIG, true, 3, v_first1, bout, &rev, &used);
    disarm_oom();
    TEST_CHECK(rok && !used && rev == 3 && bout[1] == 6 && pref_cfg_fs_rev_unknown("big.bin"),
               "M1: unreadable file + valid NVS: the NVS copy is kept (not defaults), rev marked unknown");
    TEST_CHECK(pref_cfg_fs_save("big.bin", bnvs, BIG, 4) == ESP_ERR_INVALID_STATE,
               "M1: a save over an unreadable file is refused");
    pref_cfg_fs_load_raw("big.bin", BIG, v_first1, bout, &rev, &valid);
    TEST_CHECK(valid && rev == 9 && bout[1] == 5,
               "K10-10: transient OOM reading the file leaves the NEWER file (rev 9) untouched");
    fresh();
    pref_cfg_fs_save("big.bin", big, BIG, 9);
    arm_oom(BIG, 1);
    rok = pref_cfg_fs_resolve("big.bin", bnvs, BIG, true, 3, v_first1, bout, &rev, &used);
    disarm_oom();
    pref_cfg_fs_load_raw("big.bin", BIG, v_first1, bout, &rev, &valid);
    TEST_CHECK(rok && valid && rev == 9, "resolve scratch OOM: NVS kept in RAM, file untouched");
    /* persfx LOW-3: the scratch-alloc failure branch must ALSO mark the path rev-unknown and refuse saves. */
    TEST_CHECK(pref_cfg_fs_rev_unknown("big.bin"), "resolve scratch OOM: path marked rev-unknown");
    TEST_CHECK(pref_cfg_fs_save("big.bin", bnvs, BIG, 4) == ESP_ERR_INVALID_STATE,
               "resolve scratch OOM: a save over the unreadable file is refused");
    /* persfx MED-3: a control/safety store whose NVS writer is retired gets defaults, not the frozen copy. */
    {
        uint8_t zero[BIG];
        memset(zero, 0, sizeof(zero));
        memset(bout, 0xEE, sizeof(bout));
        uint32_t rev2 = 77;
        bool used2 = true;
        arm_oom(BIG, 1);
        bool rok2 = pref_cfg_fs_resolve_nvs_retired("big.bin", bnvs, BIG, true, 3, v_first1, bout, &rev2, &used2);
        disarm_oom();
        TEST_CHECK(!rok2 && !used2 && rev2 == 0 && memcmp(bout, zero, BIG) == 0,
                   "MED-3: scratch OOM + retired-NVS store: frozen NVS copy NOT adopted (defaults)");
        TEST_CHECK(pref_cfg_fs_rev_unknown("big.bin"), "MED-3: the path stays rev-unknown");
        /* unreadable file (injected I/O error), same policy */
        memset(bout, 0xEE, sizeof(bout));
        cfg_fs_test_inject_read_error("big.bin", ESP_FAIL, 8);
        rok2 = pref_cfg_fs_resolve_nvs_retired("big.bin", bnvs, BIG, true, 3, v_first1, bout, &rev2, &used2);
        bool rok3 = pref_cfg_fs_resolve("big.bin", bnvs, BIG, true, 3, v_first1, bout, &rev2, &used2);
        cfg_fs_test_inject_read_error(NULL, ESP_OK, 0);
        TEST_CHECK(!rok2, "MED-3: I/O error + retired-NVS store: no adoption");
        TEST_CHECK(rok3, "MED-3 control: the ordinary resolve still adopts the NVS copy (stale is harmless there)");
    }
    {
        char p[300];
        snprintf(p, sizeof(p), "%s/big.bin", BASE);
        FILE *f = fopen(p, "wb");
        uint8_t hdr[4 + BIG + 3];
        memset(hdr, 0, sizeof(hdr));
        hdr[4] = 2;
        fwrite(hdr, 1, sizeof(hdr), f);
        fclose(f);
        uint8_t ver = 0;
        TEST_CHECK(pref_cfg_fs_probe_newer_wrong_size("big.bin", BIG, 0, 1, &ver) && ver == 2,
                   "newer-version wrong-size file detected");
        arm_oom(4 + PREF_CFG_FS_MAX_LARGE_ITEM + 64, 1);
        bool pn = pref_cfg_fs_probe_newer_wrong_size("big.bin", BIG, 0, 1, &ver);
        disarm_oom();
        TEST_CHECK(pn, "K10-11: probe_newer alloc failure answers 'cannot decide' (newer), so the file is kept");
    }
}

static ct_verify_blob_t good_blob(void)
{
    ct_verify_blob_t b;
    memset(&b, 0, sizeof(b));
    b.version = CT_VERIFY_BLOB_VERSION;
    b.zone_count = 2;
    b.fingerprint = 0x1234u;
    b.taken_unix = 1000;
    b.zone[0].verdict = 1;
    b.zone[0].responded_ch = 0;
    b.zone[0].measured_a = 5.f;
    b.zone[0].threshold_a = 1.f;
    b.zone[1].verdict = 2;
    b.zone[1].responded_ch = 3;
    return b;
}

static void seed_ct_nvs(const void *p, size_t n)
{
    hal_kv_handle_t h;
    hal_kv_open(&h, "ct_verify", HAL_KV_MODE_READ_WRITE, "kiln_nvs");
    hal_kv_set_blob(&h, "verdict_v1", p, n);
    hal_kv_commit(&h);
    hal_kv_close(&h);
}

static bool ct_nvs_key_exists(void)
{
    hal_kv_handle_t h;
    if (hal_kv_open(&h, "ct_verify", HAL_KV_MODE_READ_ONLY, "kiln_nvs") != HAL_OK) {
        return false;
    }
    bool e = hal_kv_key_exists(&h, "verdict_v1") == HAL_OK;
    hal_kv_close(&h);
    return e;
}

static void test_ct_verify(void)
{
    TEST_SECTION("ct_verify_store -- validate, start (NVS/file), save failures");
    ct_verify_blob_t b = good_blob(), t, f, b2;
    uint32_t frev;
    bool fv, dg;
    TEST_CHECK(ct_verify_blob_validate(&b, sizeof(b)), "good blob valid");
    TEST_CHECK(!ct_verify_blob_validate(&b, sizeof(b) - 1), "truncated rejected");
    TEST_CHECK(!ct_verify_blob_validate(&b, sizeof(b) + 1), "oversized rejected");
    TEST_CHECK(!ct_verify_blob_validate(NULL, sizeof(b)), "NULL rejected");
    t = b; t.version = 0;
    TEST_CHECK(!ct_verify_blob_validate(&t, sizeof(t)), "version 0 rejected");
    t = b; t.version = 2;
    TEST_CHECK(!ct_verify_blob_validate(&t, sizeof(t)), "newer version rejected");
    t = b; t.zone_count = 4;
    TEST_CHECK(!ct_verify_blob_validate(&t, sizeof(t)), "zone_count 4 rejected");
    t = b; t.reserved[1] = 1;
    TEST_CHECK(!ct_verify_blob_validate(&t, sizeof(t)), "reserved nonzero rejected");
    t = b; t.fingerprint = 0;
    TEST_CHECK(!ct_verify_blob_validate(&t, sizeof(t)), "fingerprint NONE rejected");
    t = b; t.zone[2].verdict = 3;
    TEST_CHECK(!ct_verify_blob_validate(&t, sizeof(t)), "verdict 3 rejected (even in unused zone slot)");
    t = b; t.zone[0].responded_ch = 4;
    TEST_CHECK(!ct_verify_blob_validate(&t, sizeof(t)), "responded_ch 4 rejected");
    t = b; t.zone[0].measured_a = NAN;
    TEST_CHECK(!ct_verify_blob_validate(&t, sizeof(t)), "K10-12: NaN measured_a rejected");
    t = b; t.zone[0].threshold_a = INFINITY;
    TEST_CHECK(!ct_verify_blob_validate(&t, sizeof(t)), "K10-12: infinite threshold_a rejected");

    fresh();
    ct_verify_store_start();
    TEST_CHECK(!ct_verify_store_get(NULL), "fresh: no verdict (never a pass)");

    seed_ct_nvs(&b, sizeof(b));
    ct_verify_store_start();
    TEST_CHECK(!ct_verify_store_get(&t), "persfx3 MED-3: frozen NVS blob NOT adopted");
    pref_cfg_fs_load_raw(CT_VERIFY_CFG_FILE_PATH, sizeof(f), ct_verify_blob_validate, &f, &frev, &fv);
    TEST_CHECK(!fv, "NVS verdict NOT migrated into a cfg file");
    TEST_CHECK(ct_nvs_key_exists(), "persfx3 MED-3: no adopted file -> the frozen NVS verdict is kept (erase only once the file is adopted)");

    /* a saved file is adopted; the next boot retires the frozen NVS copy */
    TEST_CHECK(ct_verify_store_save(&b) == ESP_OK, "persfx3 MED-3: seed a cfg file");
    TEST_CHECK(ct_nvs_key_exists(), "the file save does not touch the frozen NVS key");
    ct_verify_store_start();
    TEST_CHECK(ct_verify_store_get(&t), "file verdict adopted");
    TEST_CHECK(!ct_nvs_key_exists(), "persfx3 MED-3: adopting the file erases the frozen NVS verdict");

    fresh();
    seed_ct_nvs(&b, sizeof(b) - 2);
    ct_verify_store_start();
    TEST_CHECK(!ct_verify_store_get(NULL), "truncated NVS blob -> no verdict");
    fresh();
    t = b; t.version = 7;
    seed_ct_nvs(&t, sizeof(t));
    ct_verify_store_start();
    TEST_CHECK(!ct_verify_store_get(NULL), "newer-version NVS blob -> no verdict");
    fresh();
    seed_ct_nvs(&b, sizeof(b));
    fake_kv_script_corrupt_key("kiln_nvs", "ct_verify", "verdict_v1");
    ct_verify_store_start();
    TEST_CHECK(!ct_verify_store_get(NULL), "corrupt NVS blob (HAL_IO) -> no verdict");

    fresh();
    ct_verify_store_start();
    TEST_CHECK(ct_verify_store_save(&b) == ESP_OK, "save ok");
    pref_cfg_fs_load_raw(CT_VERIFY_CFG_FILE_PATH, sizeof(f), ct_verify_blob_validate, &f, &frev, &fv);
    TEST_CHECK(fv && frev == 1 && memcmp(&f, &b, sizeof(b)) == 0, "file rev 1 holds blob");
    b2 = good_blob();
    b2.zone[0].verdict = 2;
    b2.fingerprint = 0x99;
    pref_cfg_fs_set_write_fn(failing_write);
    TEST_CHECK(ct_verify_store_save(&b2) == ESP_FAIL, "write failure returned");
    pref_cfg_fs_reset_write_fn_for_test();
    TEST_CHECK(ct_verify_store_get(&t) && t.fingerprint == 0x1234,
               "K10-13: after a failed save RAM keeps the previous (persisted) verdict");
    pref_cfg_fs_load_raw(CT_VERIFY_CFG_FILE_PATH, sizeof(f), ct_verify_blob_validate, &f, &frev, &fv);
    TEST_CHECK(fv && frev == 1 && f.fingerprint == 0x1234, "file still holds the previous verdict");
    TEST_CHECK(ct_verify_store_save(&b2) == ESP_OK, "retry succeeds");
    pref_cfg_fs_load_raw(CT_VERIFY_CFG_FILE_PATH, sizeof(f), ct_verify_blob_validate, &f, &frev, &fv);
    TEST_CHECK(fv && frev == 2, "rev not advanced by the failed attempt");
    t = b; t.version = 9;
    TEST_CHECK(ct_verify_store_save(&t) == ESP_ERR_INVALID_ARG, "invalid blob refused");
    TEST_CHECK(ct_verify_store_get(&t) && t.fingerprint == 0x99, "refused save leaves RAM verdict alone");

    fresh();
    ct_verify_store_start();
    ct_verify_store_save(&b2);
    seed_ct_nvs(&b, sizeof(b));
    ct_verify_store_start();
    TEST_CHECK(ct_verify_store_get(&t) && t.fingerprint == 0x99, "file(rev1) beats stale NVS(rev0)");
    bool nv = false;
    uint32_t nr = 0;
    ct_verify_store_get_dualwrite_status(&fv, &frev, &nv, &nr, &dg);
    TEST_CHECK(fv && !nv, "persfx3 MED-3: the stale NVS verdict was erased once the file was adopted");
    {
        char p[300];
        snprintf(p, sizeof(p), "%s/ct_verify.bin", BASE);
        FILE *fp = fopen(p, "r+b");
        fseek(fp, 4 + 1, SEEK_SET); /* item byte 1 = zone_count -> 0xFF */
        fputc(0xFF, fp);
        fclose(fp);
    }
    TEST_CHECK(ct_verify_store_start() != ESP_OK && !ct_verify_store_get(NULL),
               "K10-14: corrupt file -> error, no verdict served");
    {
        char p[300];
        snprintf(p, sizeof(p), "%s/ct_verify.bin", BASE);
        FILE *fp = fopen(p, "rb");
        uint8_t chk[4 + sizeof(ct_verify_blob_t)];
        size_t got = fp ? fread(chk, 1, sizeof(chk), fp) : 0;
        if (fp) fclose(fp);
        TEST_CHECK(got == sizeof(chk) && chk[4 + 1] == 0xFF, "K10-14: corrupt file left untouched, not replaced from NVS");
    }
}

int main(void)
{
    test_backup_json();
    test_touch_cal();
    test_pref_cfg_fs();
    test_resolve_io_error_keeps_nvs_and_refuses_saves();
    test_ct_verify();
    fresh();
    cfg_fs_deinit();
    rm_file("ct_verify.bin");
    rm_file("big.bin");
    rm_file("small.bin");
    char t[300];
    snprintf(t, sizeof(t), "%s/.tmp", BASE);
    C10_RMDIR(t);
    C10_RMDIR(BASE);
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    return 0;
}
