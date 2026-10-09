#include "estop_verification.h"

#include "esp_log.h"

#include "hal_esp_common.h" /* hal_status_to_esp_err() */
#include "hal_kv.h"
#include "nvs_key_check.h"

static const char *TAG = "estop_verification";

/* Same namespace/partition crash_report.c/run_state.c use -- own key, so a
 * corrupt/rejected record here can never take another module's breadcrumb
 * down with it (crash_report.c's own header comment on this convention).
 * KILN_NVS_PARTITION is erased whole by factory_reset.c's "kiln" AND "all"
 * scopes, which is exactly what invalidates this record on a factory
 * reset -- see estop_verification.h point 2. */
#define NVS_NAMESPACE "kiln_cfg"
#define NVS_KEY_ESTOP_VERIF "estop_verif"
NVS_KEY_LEN_CHECK(NVS_NAMESPACE);
NVS_KEY_LEN_CHECK(NVS_KEY_ESTOP_VERIF);

#define KILN_NVS_PARTITION "kiln_nvs"
NVS_KEY_LEN_CHECK(KILN_NVS_PARTITION);

/* Bumped only if this record's layout changes. An old/unrecognised version
 * is treated as "no record" (unverified), same fail-safe direction as
 * everything else in this module. */
#define ESTOP_VERIF_RECORD_VERSION 1u

typedef struct {
    uint8_t version;
    uint8_t verified; /* 0/1 */
} estop_verif_record_t;

/* Pins the on-disk layout, same portability trick as crash_report.c's
 * crash_report_record_t_size_check and ota_record.c's ota_record_t_size_
 * check: this file is also compiled directly into App/test/build_host_
 * tests.ps1's MSVC host-test binary (via test_estop_verification.c's
 * #include of this file), and that cl.exe invocation compiles in a C mode
 * old enough that a plain _Static_assert is a hard syntax error. The
 * classic negative-array-size trick is portable C89/C99/C11 alike and
 * checks exactly the same thing. Update this literal (and bump
 * ESTOP_VERIF_RECORD_VERSION) whenever estop_verif_record_t's layout
 * changes. */
typedef char estop_verif_record_t_size_check[(sizeof(estop_verif_record_t) == 2) ? 1 : -1];

static esp_err_t nvs_partition_init(void)
{
    return hal_status_to_esp_err(hal_kv_init_partition(KILN_NVS_PARTITION));
}

static bool load(estop_verif_record_t *out)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_ONLY, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return false;
    }
    estop_verif_record_t rec;
    size_t len = sizeof(rec);
    err = hal_kv_get_blob(&h, NVS_KEY_ESTOP_VERIF, &rec, &len);
    hal_kv_close(&h);

    if (err != HAL_OK || len != sizeof(rec)) {
        return false;
    }
    if (rec.version != ESTOP_VERIF_RECORD_VERSION) {
        ESP_LOGW(TAG, "stored estop-verification record has unrecognised version %u -- treating as unverified",
                 (unsigned)rec.version);
        return false;
    }
    *out = rec;
    return true;
}

static esp_err_t persist(const estop_verif_record_t *rec)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return hal_status_to_esp_err(err);
    }
    err = hal_kv_set_blob(&h, NVS_KEY_ESTOP_VERIF, rec, sizeof(*rec));
    if (err == HAL_OK) {
        err = hal_kv_commit(&h);
    }
    hal_kv_close(&h);
    return hal_status_to_esp_err(err);
}

void estop_verification_init(void)
{
    esp_err_t err = nvs_partition_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- E-stop verification cannot be recorded/read",
                 KILN_NVS_PARTITION, esp_err_to_name(err));
    }
}

bool estop_verification_is_verified(void)
{
    estop_verif_record_t rec;
    if (!load(&rec)) {
        return false;
    }
    return rec.verified != 0u;
}

esp_err_t estop_verification_confirm(void)
{
    estop_verif_record_t rec = {
        .version = ESTOP_VERIF_RECORD_VERSION,
        .verified = 1u,
    };
    esp_err_t err = persist(&rec);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not persist E-stop verification: %s -- it will NOT survive a reboot",
                 esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "E-stop interlock marked VERIFIED by operator confirmation");
    }
    return err;
}

/* One erase+commit attempt. Split out so estop_verification_clear() can run
 * it twice (see that function's read-back comment) without duplicating the
 * open/erase/commit/close sequence. */
static hal_status_t erase_once(void)
{
    hal_kv_handle_t h;
    hal_status_t err = hal_kv_open(&h, NVS_NAMESPACE, HAL_KV_MODE_READ_WRITE, KILN_NVS_PARTITION);
    if (err != HAL_OK) {
        return err;
    }
    err = hal_kv_erase_key(&h, NVS_KEY_ESTOP_VERIF);
    if (err == HAL_OK || err == HAL_NOT_FOUND) {
        hal_status_t commit_err = hal_kv_commit(&h);
        err = (commit_err != HAL_OK) ? commit_err : HAL_OK;
    }
    hal_kv_close(&h);
    return err;
}

esp_err_t estop_verification_clear(void)
{
    /* READ-BACK, not the return code alone. boot_guard.c's 2026-09-08 audit
     * (docs/audits/boot_guard_recovery_loop_2026-09-08.md) established that
     * an NVS write on this board can report HAL_OK while the persisted value
     * never changes -- and CLAUDE.md's standing rule from that audit is that
     * no module may trust a boot_guard-style write's return code again. This
     * record is exactly that shape: one meaningful bit whose stale survival
     * is the unsafe direction (a standing "verified" outliving a polarity
     * change is precisely what this function exists to prevent), and the
     * read-back costs one NVS read on a path that runs at most once per
     * commissioning commit. One bounded retry, then give up loudly -- same
     * shape as boot_guard's verified-clear-with-retry helper. */
    hal_status_t err = erase_once();
    if (err == HAL_OK && estop_verification_is_verified()) {
        ESP_LOGW(TAG, "E-stop verification record still reads VERIFIED after an erase that reported "
                      "success -- retrying once");
        err = erase_once();
        if (err == HAL_OK && estop_verification_is_verified()) {
            ESP_LOGE(TAG, "E-stop verification record STILL reads VERIFIED after a second erase -- "
                          "the record is stale and must not be trusted");
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (err != HAL_OK) {
        ESP_LOGW(TAG, "could not clear E-stop verification record: %s", hal_status_to_name(err));
    } else {
        ESP_LOGW(TAG, "E-stop interlock verification invalidated -- re-run the bench procedure before firing");
    }
    return hal_status_to_esp_err(err);
}
