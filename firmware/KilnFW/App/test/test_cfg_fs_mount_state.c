// Host test for cfg_fs_mount.c's FORMAT STATE MACHINE (round 2 gap R2-F,
// docs/audits/HOST_TEST_COVERAGE_GAPS_ROUND2_2026-10-10.md).
//
// test_cfg_fs_format_gate.c covers the pure gate verdicts and
// test_cfg_fs_mount_reentrancy.c covers cfg_fs_write_atomic_device()'s
// dispatch; nothing drove the code in between: cfg_fs_mount_device()'s
// register-fail -> partition scan -> pending-confirmation / deferred
// auto-format decision, the cfg_autofmt task body, and
// cfg_fs_confirm_format_device(). This file #includes cfg_fs_mount.c so the
// static helpers are reachable, links the REAL cfg_fs_format_gate.c (so the
// verdicts and the pending reason strings come from the real gate), and
// scripts every ESP-IDF/littlefs/partition/FreeRTOS seam.
#include "test_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "fake_time.h"

// malloc seam: the gate allocation is malloc call #1 and the scan chunk
// buffer is call #2 inside maybe_auto_format_and_remount().
static unsigned s_malloc_calls = 0;
static unsigned s_malloc_fail_on = 0; // 1-based call index to fail, 0 = never
static void *test_malloc(size_t n)
{
    s_malloc_calls++;
    if (s_malloc_fail_on != 0 && s_malloc_calls == s_malloc_fail_on) {
        return NULL;
    }
    return malloc(n);
}
#define malloc(n) test_malloc(n)

#include "../drivers/persist/cfg_fs_mount.c"

#undef malloc

int g_test_failures = 0;
int g_test_count = 0;

#define LFS_MKTAG(type, id, size) (((uint32_t)(type) << 20) | ((uint32_t)(id) << 10) | (uint32_t)(size))
#define LFS_TYPE_CREATE       0x401u
#define LFS_TYPE_SUPERBLOCK   0x0ffu
#define LFS_TYPE_INLINESTRUCT 0x201u
#define LFS_TYPE_CCRC         0x500u

static uint32_t ref_crc(uint32_t crc, const uint8_t *data, size_t size)
{
    static const uint32_t rtable[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    for (size_t i = 0; i < size; i++) {
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 0)) & 0xf];
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 4)) & 0xf];
    }
    return crc;
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)(v);
}

/* Appends one tag+data to `block` at *off, XORing against *ptag exactly as
 * lfs_dir_commitattr() does, and folds it into *crc. Returns the new tag
 * (caller passes it back in as ptag for the next append). */
static uint32_t append_tag(uint8_t *block, size_t *off, uint32_t ptag, uint32_t *crc,
                            uint32_t type, uint32_t id, const uint8_t *data, size_t size)
{
    uint32_t tag = LFS_MKTAG(type, id, (uint32_t)size);
    uint8_t raw[4];
    wr_be32(raw, tag ^ ptag);
    memcpy(block + *off, raw, 4);
    *crc = ref_crc(*crc, raw, 4);
    *off += 4;
    if (size > 0) {
        memcpy(block + *off, data, size);
        *crc = ref_crc(*crc, data, size);
        *off += size;
    }
    return tag;
}

/* Fills `block` (must be >= CFG_FS_FORMAT_GATE_BLOCK_SIZE) with a valid
 * superblock commit, erased (0xFF) beyond it. If `corrupt_crc` is set, the
 * final stored CRC word is deliberately wrong (simulates a filesystem whose
 * superblock commit was interrupted mid-write -- a real, if damaged,
 * filesystem, not coincidental bytes). */
static void build_superblock_block(uint8_t *block, bool corrupt_crc)
{
    memset(block, 0xFF, CFG_FS_FORMAT_GATE_BLOCK_SIZE);

    uint32_t rev = 1;
    wr_le32(block, rev);
    size_t off = 4;
    uint32_t crc = ref_crc(0xffffffffu, block, 4);
    uint32_t ptag = 0xffffffffu;

    ptag = append_tag(block, &off, ptag, &crc, LFS_TYPE_CREATE, 0, NULL, 0);
    ptag = append_tag(block, &off, ptag, &crc, LFS_TYPE_SUPERBLOCK, 0,
                       (const uint8_t *)"littlefs", 8);

    uint8_t superblock[24];
    memset(superblock, 0, sizeof(superblock));
    wr_le32(superblock + 0, 0x00020001u);  /* version: major 2, minor 1 -- LFS_DISK_VERSION */
    wr_le32(superblock + 4, CFG_FS_FORMAT_GATE_BLOCK_SIZE); /* block_size */
    wr_le32(superblock + 8, 128);           /* block_count (512 KiB / 4096) */
    wr_le32(superblock + 12, 255);          /* name_max */
    wr_le32(superblock + 16, 2147483647u);  /* file_max */
    wr_le32(superblock + 20, 1022);         /* attr_max */
    ptag = append_tag(block, &off, ptag, &crc, LFS_TYPE_INLINESTRUCT, 0, superblock, sizeof(superblock));

    /* Closing CCRC tag: id 0x3ff, size 4 (just the CRC word, no padding). */
    uint32_t ccrc_tag = LFS_MKTAG(LFS_TYPE_CCRC, 0x3ff, 4);
    uint8_t raw[4];
    wr_be32(raw, ccrc_tag ^ ptag);
    memcpy(block + off, raw, 4);
    crc = ref_crc(crc, raw, 4);
    off += 4;

    if (corrupt_crc) {
        crc ^= 0xFFFFFFFFu; /* deliberately wrong */
    }
    uint8_t crc_bytes[4];
    wr_le32(crc_bytes, crc);
    memcpy(block + off, crc_bytes, 4);
}

// ---- scripted seams -------------------------------------------------------
static char s_seq[256];
static void seq(const char *tok)
{
    size_t n = strlen(s_seq);
    snprintf(s_seq + n, sizeof(s_seq) - n, "%s%s", n ? "," : "", tok);
}

static esp_err_t s_register_result;
static unsigned s_register_calls;
static char s_reg_base[16], s_reg_label[16];
static bool s_reg_format_if_fail;
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *conf)
{
    s_register_calls++;
    snprintf(s_reg_base, sizeof(s_reg_base), "%s", conf->base_path);
    snprintf(s_reg_label, sizeof(s_reg_label), "%s", conf->partition_label);
    s_reg_format_if_fail = conf->format_if_mount_failed;
    seq("reg");
    return s_register_result;
}

static unsigned s_unreg_calls;
static char s_unreg_label[16];
esp_err_t esp_vfs_littlefs_unregister(const char *label)
{
    s_unreg_calls++;
    snprintf(s_unreg_label, sizeof(s_unreg_label), "%s", label);
    seq("unreg");
    return ESP_OK;
}

static esp_err_t s_format_result;
static unsigned s_format_calls;
static char s_fmt_label[16];
esp_err_t esp_littlefs_format(const char *label)
{
    s_format_calls++;
    snprintf(s_fmt_label, sizeof(s_fmt_label), "%s", label);
    seq("fmt");
    return s_format_result;
}
esp_err_t esp_littlefs_info(const char *l, size_t *t, size_t *u) { (void)l; (void)t; (void)u; return ESP_FAIL; }
esp_err_t esp_partition_write(const esp_partition_t *p, size_t o, const void *s, size_t n)
{
    (void)p; (void)o; (void)s; (void)n; return ESP_FAIL;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t o, size_t n)
{
    (void)p; (void)o; (void)n; return ESP_FAIL;
}
uint32_t esp_partition_get_main_flash_sector_size(void) { return 4096u; }
esp_partition_iterator_t esp_partition_find(esp_partition_type_t t, esp_partition_subtype_t s, const char *l)
{
    (void)t; (void)s; (void)l; return NULL;
}
const esp_partition_t *esp_partition_get(esp_partition_iterator_t i) { (void)i; return NULL; }
esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t i) { (void)i; return NULL; }
esp_err_t esp_partition_iterator_release(esp_partition_iterator_t i) { (void)i; return ESP_OK; }

#define FAKE_PART_BYTES (4u * 4096u)
static uint8_t s_flash[FAKE_PART_BYTES];
static esp_partition_t s_part;
static bool s_part_present;
static int64_t s_read_fail_at = -1; // offset whose read fails, -1 = never
static unsigned s_read_calls;
static char s_find_label[16];
const esp_partition_t *esp_partition_find_first(esp_partition_type_t t, esp_partition_subtype_t s, const char *l)
{
    (void)t; (void)s;
    snprintf(s_find_label, sizeof(s_find_label), "%s", l);
    return s_part_present ? &s_part : NULL;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n)
{
    (void)p;
    s_read_calls++;
    if (s_read_fail_at >= 0 && (int64_t)off == s_read_fail_at) {
        return ESP_ERR_NOT_FOUND;
    }
    memcpy(dst, s_flash + off, n);
    return ESP_OK;
}

static cfg_fs_status_t s_status;
static esp_err_t s_mount_result;
static unsigned s_mount_calls;
static bool s_mount_recovery_arg;
static char s_mount_base[16];
static size_t *s_mount_reaped_arg;
esp_err_t cfg_fs_mount_or_skip(bool recovery_mode, const char *base_dir, size_t *out_tmp_reaped)
{
    s_mount_calls++;
    s_mount_recovery_arg = recovery_mode;
    snprintf(s_mount_base, sizeof(s_mount_base), "%s", base_dir);
    s_mount_reaped_arg = out_tmp_reaped;
    seq("mount");
    return s_mount_result;
}
cfg_fs_status_t cfg_fs_get_status(void) { return s_status; }
static unsigned s_deinit_calls;
void cfg_fs_deinit(void)
{
    s_deinit_calls++;
    s_status = CFG_FS_STATUS_UNMOUNTED;
    seq("deinit");
}
esp_err_t cfg_fs_write_atomic(const char *p, const void *d, size_t n) { (void)p; (void)d; (void)n; return ESP_OK; }

static bool s_recovery_mode;
bool boot_guard_is_recovery_mode(void) { return s_recovery_mode; }

static bool s_on_worker;
static bool s_worker_started;
static esp_err_t s_dispatch_result;
static unsigned s_dispatch_calls;
static int s_dispatch_advance_ms;
bool uart_bridge_ext_is_on_flash_worker(void) { return s_on_worker; }
bool uart_bridge_ext_flash_worker_started(void) { return s_worker_started; }
esp_err_t uart_bridge_ext_run_on_flash_worker(void (*fn)(void *arg), void *arg)
{
    s_dispatch_calls++;
    seq("disp");
    if (s_dispatch_result != ESP_OK) {
        return s_dispatch_result;
    }
    fn(arg);
    if (s_dispatch_advance_ms > 0) {
        fake_time_advance_ms((uint32_t)s_dispatch_advance_ms);
    }
    return ESP_OK;
}

static bool s_wait_result;
static unsigned s_wait_calls;
bool flash_worker_wait_until_started(bool (*started_fn)(void), uint32_t poll_ms, uint32_t ceiling_ms)
{
    (void)started_fn; (void)poll_ms; (void)ceiling_ms;
    s_wait_calls++;
    seq("wait");
    return s_wait_result;
}
bool flash_worker_wait_default(void) { return s_wait_result; }

static zones_cfg_fs_write_fn_t s_zw;
static pref_cfg_fs_write_fn_t s_pw;
static profiles_cfg_fs_write_fn_t s_prw;
void zones_config_cfg_fs_set_write_fn(zones_cfg_fs_write_fn_t fn) { s_zw = fn; }
zones_cfg_fs_write_fn_t zones_config_cfg_fs_get_write_fn(void) { return s_zw; }
void pref_cfg_fs_set_write_fn(pref_cfg_fs_write_fn_t fn) { s_pw = fn; }
pref_cfg_fs_write_fn_t pref_cfg_fs_get_write_fn(void) { return s_pw; }
void profiles_cfg_fs_set_write_fn(profiles_cfg_fs_write_fn_t fn) { s_prw = fn; }
profiles_cfg_fs_write_fn_t profiles_cfg_fs_get_write_fn(void) { return s_prw; }

// ---- per-case reset -------------------------------------------------------
static void reset_all(void)
{
    s_seq[0] = '\0';
    s_malloc_calls = 0;
    s_malloc_fail_on = 0;
    s_register_result = ESP_OK;
    s_register_calls = 0;
    s_unreg_calls = 0;
    s_format_result = ESP_OK;
    s_format_calls = 0;
    s_reg_base[0] = s_reg_label[0] = s_unreg_label[0] = s_fmt_label[0] = s_find_label[0] = s_mount_base[0] = '\0';
    s_reg_format_if_fail = true;
    memset(s_flash, 0xFF, sizeof(s_flash));
    memset(&s_part, 0, sizeof(s_part));
    s_part.size = FAKE_PART_BYTES;
    snprintf(s_part.label, sizeof(s_part.label), "cfg");
    s_part_present = true;
    s_read_fail_at = -1;
    s_read_calls = 0;
    s_status = CFG_FS_STATUS_UNMOUNTED;
    s_mount_result = ESP_OK;
    s_mount_calls = 0;
    s_mount_recovery_arg = true;
    s_mount_reaped_arg = NULL;
    s_deinit_calls = 0;
    s_recovery_mode = false;
    s_on_worker = false;
    s_worker_started = true;
    s_dispatch_result = ESP_OK;
    s_dispatch_calls = 0;
    s_dispatch_advance_ms = 0;
    s_wait_result = true;
    s_wait_calls = 0;
    s_zw = NULL;
    s_pw = NULL;
    s_prw = NULL;
    g_test_stub_xtaskcreate_result = 1;
    s_format_confirmation_pending = false;
    s_format_pending_reason[0] = '\0';
    s_auto_format_ever_started = false;
    s_auto_format_in_progress = false;
    s_auto_format_completed = false;
    s_auto_format_result = ESP_ERR_INVALID_STATE;
    s_auto_format_start_us = 0;
    s_auto_format_end_us = 0;
    fake_time_reset_all();
}

// ---- cases ----------------------------------------------------------------
static void test_initial_state(void)
{
    reset_all();
    TEST_CHECK(!cfg_fs_mount_format_confirmation_pending(), "initially no confirmation pending");
    TEST_CHECK(strcmp(cfg_fs_mount_format_pending_reason(), "") == 0, "initially empty pending reason");
    TEST_CHECK(!cfg_fs_mount_format_ever_started(), "initially auto-format never started");
    TEST_CHECK(!cfg_fs_mount_format_in_progress(), "initially not in progress");
    TEST_CHECK(!cfg_fs_mount_format_completed(), "initially not completed");
    TEST_CHECK(cfg_fs_mount_format_result() == ESP_ERR_INVALID_STATE, "initial result is INVALID_STATE");
    TEST_CHECK(cfg_fs_mount_format_elapsed_ms() == 0, "elapsed is 0 when never started");
}

static void test_mount_idempotent_and_recovery(void)
{
    reset_all();
    s_status = CFG_FS_STATUS_MOUNTED;
    TEST_CHECK(cfg_fs_mount_device() == ESP_OK, "already MOUNTED returns ESP_OK");
    TEST_CHECK(s_register_calls == 0 && s_mount_calls == 0, "already MOUNTED touches neither register nor mount");

    reset_all();
    s_recovery_mode = true;
    s_mount_result = ESP_ERR_NOT_FOUND;
    TEST_CHECK(cfg_fs_mount_device() == ESP_ERR_NOT_FOUND, "recovery mode returns cfg_fs_mount_or_skip's result");
    TEST_CHECK(s_mount_calls == 1 && s_mount_recovery_arg, "recovery mode calls cfg_fs_mount_or_skip(true, ...)");
    TEST_CHECK(strcmp(s_mount_base, "/cfg") == 0, "recovery skip names /cfg");
    TEST_CHECK(s_mount_reaped_arg == NULL, "recovery skip passes a NULL reaped counter");
    TEST_CHECK(s_register_calls == 0 && s_format_calls == 0, "recovery mode never registers or formats");
    TEST_CHECK(s_zw == NULL && s_pw == NULL && s_prw == NULL, "recovery mode installs no device write fns");
}

static void test_mount_happy_and_mount_fail(void)
{
    reset_all();
    TEST_CHECK(cfg_fs_mount_device() == ESP_OK, "register OK + mount OK returns ESP_OK");
    TEST_CHECK(s_register_calls == 1, "registered once");
    TEST_CHECK(strcmp(s_reg_base, "/cfg") == 0 && strcmp(s_reg_label, "cfg") == 0,
               "register names base /cfg and label cfg");
    TEST_CHECK(!s_reg_format_if_fail, "register never lets the VFS auto-format (format_if_mount_failed=false)");
    TEST_CHECK(s_mount_calls == 1 && !s_mount_recovery_arg && strcmp(s_mount_base, "/cfg") == 0,
               "cfg_fs_mount_or_skip(false, \"/cfg\") called once");
    TEST_CHECK(s_mount_reaped_arg != NULL, "a reaped-temp counter is passed in normal mode");
    TEST_CHECK(s_zw == cfg_fs_write_atomic_device && s_pw == cfg_fs_write_atomic_device &&
                   s_prw == cfg_fs_write_atomic_device,
               "all three bridges get the flash-worker-dispatching writer");
    TEST_CHECK(strcmp(s_seq, "reg,mount") == 0, "order is register then mount");
    TEST_CHECK(s_read_calls == 0 && !cfg_fs_mount_format_ever_started(), "no scan or format on the happy path");

    reset_all();
    s_mount_result = ESP_ERR_INVALID_STATE;
    TEST_CHECK(cfg_fs_mount_device() == ESP_ERR_INVALID_STATE, "mount failure after a good register is returned");
    TEST_CHECK(s_zw == NULL && s_pw == NULL && s_prw == NULL, "no write fns installed when the mount failed");
    TEST_CHECK(s_read_calls == 0 && !cfg_fs_mount_format_ever_started(), "mount failure does not scan or format");
}

static void test_register_fail_no_partition(void)
{
    reset_all();
    s_register_result = ESP_ERR_NOT_FOUND;
    s_part_present = false;
    TEST_CHECK(cfg_fs_mount_device() == ESP_ERR_NOT_FOUND, "absent partition returns the original register error");
    TEST_CHECK(strcmp(s_find_label, "cfg") == 0, "partition looked up by label cfg");
    TEST_CHECK(s_read_calls == 0 && s_malloc_calls == 0, "absent partition: no scan, no allocation");
    TEST_CHECK(!cfg_fs_mount_format_confirmation_pending() && !cfg_fs_mount_format_ever_started(),
               "absent partition: no pending flag, no format");
    TEST_CHECK(s_mount_calls == 0, "absent partition: no mount attempt");
}

static void test_register_fail_scan_failures(void)
{
    reset_all();
    s_register_result = ESP_ERR_INVALID_STATE;
    s_malloc_fail_on = 1;
    TEST_CHECK(cfg_fs_mount_device() == ESP_ERR_INVALID_STATE, "gate alloc failure returns the original error");
    TEST_CHECK(cfg_fs_mount_format_confirmation_pending(), "gate alloc failure forces confirmation pending");
    TEST_CHECK(strcmp(cfg_fs_mount_format_pending_reason(), "partition could not be scanned: out of memory") == 0,
               "gate alloc failure reason text");
    TEST_CHECK(s_read_calls == 0 && !cfg_fs_mount_format_ever_started(), "gate alloc failure: no read, no format");

    reset_all();
    s_register_result = ESP_ERR_INVALID_STATE;
    s_malloc_fail_on = 2;
    TEST_CHECK(cfg_fs_mount_device() == ESP_ERR_INVALID_STATE, "scan buffer alloc failure returns original error");
    TEST_CHECK(cfg_fs_mount_format_confirmation_pending(), "scan buffer alloc failure forces pending");
    TEST_CHECK(strcmp(cfg_fs_mount_format_pending_reason(),
                      "partition could not be read back: ESP_ERR_NO_MEM") == 0,
               "scan buffer alloc failure reports the NO_MEM name");
    TEST_CHECK(!cfg_fs_mount_format_ever_started(), "scan buffer alloc failure never formats");

    reset_all();
    s_register_result = ESP_ERR_INVALID_STATE;
    s_read_fail_at = 4096; // second chunk fails
    TEST_CHECK(cfg_fs_mount_device() == ESP_ERR_INVALID_STATE, "read error returns the original error");
    TEST_CHECK(s_read_calls == 2, "scan stops at the first failing read (chunk 0 ok, chunk 1 fails)");
    TEST_CHECK(cfg_fs_mount_format_confirmation_pending(), "read error forces pending");
    TEST_CHECK(strcmp(cfg_fs_mount_format_pending_reason(),
                      "partition could not be read back: ESP_ERR_NOT_FOUND") == 0,
               "read error reason names the esp_err");
    TEST_CHECK(!cfg_fs_mount_format_ever_started(), "read error never formats even though the data was blank");
}

static void test_register_fail_content_found(void)
{
    reset_all();
    s_register_result = ESP_ERR_INVALID_STATE;
    build_superblock_block(s_flash, false);
    TEST_CHECK(cfg_fs_mount_device() == ESP_ERR_INVALID_STATE, "valid superblock returns the original error");
    TEST_CHECK(s_read_calls == FAKE_PART_BYTES / 4096u, "scan reads the whole partition in 4096-byte chunks");
    TEST_CHECK(cfg_fs_mount_format_confirmation_pending(), "valid superblock: confirmation pending");
    TEST_CHECK(strcmp(cfg_fs_mount_format_pending_reason(),
                      "LittleFS superblock signature found and its structure validates") == 0,
               "valid superblock reason comes from the real gate describe");
    TEST_CHECK(!cfg_fs_mount_format_ever_started() && g_test_stub_xtaskcreate_result == 1,
               "valid superblock: no deferred format started");
    TEST_CHECK(s_format_calls == 0, "valid superblock: esp_littlefs_format never called");

    reset_all();
    s_register_result = ESP_ERR_INVALID_STATE;
    build_superblock_block(s_flash, true);
    TEST_CHECK(cfg_fs_mount_device() == ESP_ERR_INVALID_STATE, "corrupt superblock returns the original error");
    TEST_CHECK(cfg_fs_mount_format_confirmation_pending(), "corrupt superblock: confirmation pending too");
    char want[96];
    snprintf(want, sizeof(want), "%s",
             "LittleFS superblock signature found but its commit failed CRC/version validation (corrupt filesystem)");
    TEST_CHECK(strcmp(cfg_fs_mount_format_pending_reason(), want) == 0,
               "corrupt superblock reason is the gate text truncated to the 96-byte buffer, NUL-terminated");
    TEST_CHECK(strlen(cfg_fs_mount_format_pending_reason()) == 95, "truncated reason is exactly 95 chars + NUL");
    TEST_CHECK(!cfg_fs_mount_format_ever_started(), "corrupt superblock: no format");

    // Only the second metadata block holds the superblock.
    reset_all();
    s_register_result = ESP_ERR_INVALID_STATE;
    build_superblock_block(s_flash + 4096, false);
    (void)cfg_fs_mount_device();
    TEST_CHECK(cfg_fs_mount_format_confirmation_pending() && !cfg_fs_mount_format_ever_started(),
               "a superblock in metadata block 1 also blocks the auto-format");
}

static void test_register_fail_safe_to_format(void)
{
    reset_all();
    s_register_result = ESP_ERR_INVALID_STATE;
    TEST_CHECK(cfg_fs_mount_device() == ESP_ERR_INVALID_STATE,
               "blank partition: mount_device still returns the ORIGINAL error (format is deferred)");
    TEST_CHECK(!cfg_fs_mount_format_confirmation_pending(), "blank partition: no confirmation pending");
    TEST_CHECK(cfg_fs_mount_format_ever_started(), "blank partition: deferred format started");
    TEST_CHECK(cfg_fs_mount_format_in_progress(), "blank partition: in progress until the task finishes");
    TEST_CHECK(!cfg_fs_mount_format_completed(), "blank partition: not completed yet");
    TEST_CHECK(cfg_fs_mount_format_result() == ESP_ERR_INVALID_STATE, "result stays INVALID_STATE until done");
    TEST_CHECK(s_format_calls == 0 && s_mount_calls == 0,
               "boot path itself never formats or mounts (the task does, later)");

    // Non-blank bytes that are not a filesystem are also safe.
    reset_all();
    s_register_result = ESP_ERR_INVALID_STATE;
    memset(s_flash, 0xA5, sizeof(s_flash));
    (void)cfg_fs_mount_device();
    TEST_CHECK(cfg_fs_mount_format_ever_started() && !cfg_fs_mount_format_confirmation_pending(),
               "garbage with no superblock is safe to format");

    // elapsed while running = now - start (start_us is 0 until the task runs).
    reset_all();
    s_register_result = ESP_ERR_INVALID_STATE;
    (void)cfg_fs_mount_device();
    fake_time_advance_ms(1500);
    TEST_CHECK(cfg_fs_mount_format_elapsed_ms() == 1500, "elapsed while in progress tracks the live clock");

    // xTaskCreate failure.
    reset_all();
    s_register_result = ESP_ERR_INVALID_STATE;
    g_test_stub_xtaskcreate_result = 0;
    TEST_CHECK(cfg_fs_mount_device() == ESP_ERR_NO_MEM, "task creation failure returns ESP_ERR_NO_MEM");
    TEST_CHECK(cfg_fs_mount_format_ever_started(), "task creation failure: ever_started stays true");
    TEST_CHECK(!cfg_fs_mount_format_in_progress(), "task creation failure: not in progress");
    TEST_CHECK(cfg_fs_mount_format_completed(), "task creation failure: marked completed");
    TEST_CHECK(cfg_fs_mount_format_result() == ESP_ERR_NO_MEM, "task creation failure: result NO_MEM");

    // A re-start clears the previous run's outcome.
    reset_all();
    s_auto_format_completed = true;
    s_auto_format_result = ESP_FAIL;
    s_auto_format_end_us = 77;
    TEST_CHECK(start_deferred_auto_format(), "start_deferred_auto_format succeeds with pdPASS");
    TEST_CHECK(!s_auto_format_completed && s_auto_format_result == ESP_ERR_INVALID_STATE &&
                   s_auto_format_end_us == 0 && s_auto_format_in_progress,
               "a new start clears completed/result/end and sets in_progress");
}

static void test_auto_format_task(void)
{
    // Worker never starts.
    reset_all();
    s_auto_format_in_progress = true;
    s_worker_started = false;
    s_wait_result = false;
    cfg_fs_auto_format_task(NULL);
    TEST_CHECK(s_wait_calls == 1, "task waits for the flash worker once");
    TEST_CHECK(cfg_fs_mount_format_result() == ESP_ERR_TIMEOUT, "worker timeout -> ESP_ERR_TIMEOUT");
    TEST_CHECK(cfg_fs_mount_format_completed() && !cfg_fs_mount_format_in_progress(),
               "worker timeout still marks completed and clears in_progress");
    TEST_CHECK(s_dispatch_calls == 0 && s_format_calls == 0, "worker timeout: nothing dispatched, nothing formatted");

    // Dispatch itself fails.
    reset_all();
    s_auto_format_in_progress = true;
    s_dispatch_result = ESP_ERR_INVALID_STATE;
    cfg_fs_auto_format_task(NULL);
    TEST_CHECK(cfg_fs_mount_format_result() == ESP_ERR_INVALID_STATE, "dispatch error becomes the result");
    TEST_CHECK(s_format_calls == 0 && cfg_fs_mount_format_completed(), "dispatch error: job never ran, completed");

    // Format fails.
    reset_all();
    s_auto_format_in_progress = true;
    s_format_result = ESP_FAIL;
    cfg_fs_auto_format_task(NULL);
    TEST_CHECK(cfg_fs_mount_format_result() == ESP_FAIL, "format failure becomes the result");
    TEST_CHECK(strcmp(s_fmt_label, "cfg") == 0, "format targets the cfg partition label");
    TEST_CHECK(s_register_calls == 0 && s_mount_calls == 0, "format failure: no register, no mount");
    TEST_CHECK(cfg_fs_mount_format_completed() && !cfg_fs_mount_format_in_progress(), "format failure: completed");

    // Register after format fails.
    reset_all();
    s_auto_format_in_progress = true;
    s_register_result = ESP_ERR_NOT_FOUND;
    cfg_fs_auto_format_task(NULL);
    TEST_CHECK(cfg_fs_mount_format_result() == ESP_ERR_NOT_FOUND, "register-after-format failure is the result");
    TEST_CHECK(s_mount_calls == 0, "register failure: no mount attempted");

    // Mount after format fails.
    reset_all();
    s_auto_format_in_progress = true;
    s_mount_result = ESP_ERR_INVALID_STATE;
    cfg_fs_auto_format_task(NULL);
    TEST_CHECK(cfg_fs_mount_format_result() == ESP_ERR_INVALID_STATE, "mount-after-format failure is the result");
    TEST_CHECK(s_zw == NULL, "mount failure: write fns not installed");

    // Success.
    reset_all();
    s_auto_format_ever_started = true;
    s_auto_format_in_progress = true;
    s_dispatch_advance_ms = 250;
    fake_time_advance_ms(10);
    cfg_fs_auto_format_task(NULL);
    TEST_CHECK(cfg_fs_mount_format_result() == ESP_OK, "success result is ESP_OK");
    TEST_CHECK(cfg_fs_mount_format_completed() && !cfg_fs_mount_format_in_progress(), "success: completed");
    TEST_CHECK(strcmp(s_seq, "wait,disp,fmt,reg,mount") == 0, "success order: wait, dispatch, format, register, mount");
    TEST_CHECK(s_dispatch_calls == 1, "exactly one flash-worker dispatch");
    TEST_CHECK(s_zw == cfg_fs_write_atomic_device && s_pw == cfg_fs_write_atomic_device &&
                   s_prw == cfg_fs_write_atomic_device,
               "success installs the device write fns");
    TEST_CHECK(cfg_fs_mount_format_elapsed_ms() == 250, "completed elapsed = end - start (250 ms in the job)");
    fake_time_advance_ms(5000);
    TEST_CHECK(cfg_fs_mount_format_elapsed_ms() == 250, "completed elapsed freezes at the end time");

    // Negative elapsed clamps to 0.
    reset_all();
    s_auto_format_ever_started = true;
    s_auto_format_completed = true;
    s_auto_format_start_us = 5000000;
    s_auto_format_end_us = 1000;
    TEST_CHECK(cfg_fs_mount_format_elapsed_ms() == 0, "end before start clamps elapsed to 0");
}

static void test_confirm_format(void)
{
    // Not mounted, already on the worker: inline.
    reset_all();
    s_on_worker = true;
    s_format_confirmation_pending = true;
    snprintf(s_format_pending_reason, sizeof(s_format_pending_reason), "needs confirm");
    TEST_CHECK(cfg_fs_confirm_format_device() == ESP_OK, "confirm on the worker succeeds");
    TEST_CHECK(s_dispatch_calls == 0, "on the worker: runs inline, no re-dispatch");
    TEST_CHECK(strcmp(s_seq, "fmt,reg,mount") == 0, "unmounted confirm: format, register, mount (no unmount)");
    TEST_CHECK(!cfg_fs_mount_format_confirmation_pending(), "success clears the pending flag");
    TEST_CHECK(strcmp(cfg_fs_mount_format_pending_reason(), "") == 0, "success clears the pending reason");
    TEST_CHECK(s_zw == cfg_fs_write_atomic_device, "success installs the device write fns");

    // Off the worker: dispatched.
    reset_all();
    s_format_confirmation_pending = true;
    snprintf(s_format_pending_reason, sizeof(s_format_pending_reason), "needs confirm");
    TEST_CHECK(cfg_fs_confirm_format_device() == ESP_OK, "confirm off the worker succeeds");
    TEST_CHECK(s_dispatch_calls == 1 && strcmp(s_seq, "disp,fmt,reg,mount") == 0,
               "off the worker: exactly one dispatch wraps the job");
    TEST_CHECK(!cfg_fs_mount_format_confirmation_pending(), "dispatched success clears pending");

    // Mounted: unmount first.
    reset_all();
    s_on_worker = true;
    s_status = CFG_FS_STATUS_MOUNTED;
    TEST_CHECK(cfg_fs_confirm_format_device() == ESP_OK, "confirm over a mounted fs succeeds");
    TEST_CHECK(strcmp(s_seq, "unreg,deinit,fmt,reg,mount") == 0, "mounted: unregister, deinit, then format+remount");
    TEST_CHECK(strcmp(s_unreg_label, "cfg") == 0, "unregister names the cfg label");

    // Format fails: pending kept.
    reset_all();
    s_on_worker = true;
    s_format_result = ESP_FAIL;
    s_format_confirmation_pending = true;
    snprintf(s_format_pending_reason, sizeof(s_format_pending_reason), "keep me");
    TEST_CHECK(cfg_fs_confirm_format_device() == ESP_FAIL, "format failure is returned");
    TEST_CHECK(cfg_fs_mount_format_confirmation_pending() &&
                   strcmp(cfg_fs_mount_format_pending_reason(), "keep me") == 0,
               "format failure keeps pending and its reason");
    TEST_CHECK(s_register_calls == 0, "format failure: no register");

    // Register fails: pending kept.
    reset_all();
    s_on_worker = true;
    s_register_result = ESP_ERR_NOT_FOUND;
    s_format_confirmation_pending = true;
    snprintf(s_format_pending_reason, sizeof(s_format_pending_reason), "keep me");
    TEST_CHECK(cfg_fs_confirm_format_device() == ESP_ERR_NOT_FOUND, "register failure is returned");
    TEST_CHECK(cfg_fs_mount_format_confirmation_pending(), "register failure keeps pending");
    TEST_CHECK(s_mount_calls == 0, "register failure: no mount");

    // Mount fails: pending kept.
    reset_all();
    s_on_worker = true;
    s_mount_result = ESP_ERR_INVALID_STATE;
    s_format_confirmation_pending = true;
    snprintf(s_format_pending_reason, sizeof(s_format_pending_reason), "keep me");
    TEST_CHECK(cfg_fs_confirm_format_device() == ESP_ERR_INVALID_STATE, "mount failure is returned");
    TEST_CHECK(cfg_fs_mount_format_confirmation_pending() &&
                   strcmp(cfg_fs_mount_format_pending_reason(), "keep me") == 0,
               "mount failure keeps pending and its reason");

    // Dispatch fails: job never ran.
    reset_all();
    s_dispatch_result = ESP_ERR_TIMEOUT;
    s_format_confirmation_pending = true;
    TEST_CHECK(cfg_fs_confirm_format_device() == ESP_ERR_TIMEOUT, "dispatch failure is returned");
    TEST_CHECK(s_format_calls == 0 && cfg_fs_mount_format_confirmation_pending(),
               "dispatch failure: nothing formatted, pending kept");
}

int main(void)
{
    test_initial_state();
    test_mount_idempotent_and_recovery();
    test_mount_happy_and_mount_fail();
    test_register_fail_no_partition();
    test_register_fail_scan_failures();
    test_register_fail_content_found();
    test_register_fail_safe_to_format();
    test_auto_format_task();
    test_confirm_format();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
