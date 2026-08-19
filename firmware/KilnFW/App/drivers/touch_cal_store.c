#include "touch_cal_store.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "touch_cal_store";

/* Same shared partition/pattern as ota_record.c/run_state.c/relay_cycles.c --
 * own namespace, own key, duplicated nvs_partition_init() rather than
 * shared for the same isolation reason those modules give: a corrupt/
 * rejected record here must never be able to take another module's config
 * down with it. */
#define NVS_PARTITION "kiln_nvs"
#define NVS_NAMESPACE "touch_cal"
#define NVS_KEY_CAL "affine_v1"

#define TOUCH_CAL_RECORD_VERSION 1u

/* On-disk layout. Explicit version + reserved padding, same discipline
 * ota_record.h insists on -- a silent field add/reorder would otherwise
 * look like corruption to the loader rather than fail the build. */
typedef struct {
    uint8_t version;
    uint8_t reserved[3];
    float a, b, c;
    float d, e, f;
} touch_cal_record_t;

_Static_assert(sizeof(touch_cal_record_t) == 28,
               "touch_cal_record_t layout changed -- bump TOUCH_CAL_RECORD_VERSION");

static esp_err_t nvs_partition_init(const char *partition)
{
    esp_err_t err = nvs_flash_init_partition(partition);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition '%s' needs erase (%s) -- erasing THAT PARTITION ONLY and retrying",
                 partition, esp_err_to_name(err));
        err = nvs_flash_erase_partition(partition);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(partition);
        }
    }
    return err;
}

static void set_uncalibrated(touch_cal_t *out)
{
    memset(out, 0, sizeof(*out));
    out->calibrated = false;
}

esp_err_t touch_cal_store_load(touch_cal_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    set_uncalibrated(out);

    esp_err_t part_err = nvs_partition_init(NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS partition '%s' init failed: %s -- treating as uncalibrated",
                 NVS_PARTITION, esp_err_to_name(part_err));
        return ESP_OK;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* ESP_ERR_NVS_NOT_FOUND on a fresh board is the expected steady
         * state, not a fault -- see this file's header comment. */
        return ESP_OK;
    }

    touch_cal_record_t rec;
    size_t len = sizeof(rec);
    err = nvs_get_blob(h, NVS_KEY_CAL, &rec, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(rec) || rec.version != TOUCH_CAL_RECORD_VERSION) {
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "stored calibration record size/version mismatch -- treating as uncalibrated");
        }
        return ESP_OK;
    }

    out->calibrated = true;
    out->a = rec.a;
    out->b = rec.b;
    out->c = rec.c;
    out->d = rec.d;
    out->e = rec.e;
    out->f = rec.f;
    return ESP_OK;
}

bool touch_cal_store_is_calibrated(void)
{
    touch_cal_t cal;
    touch_cal_store_load(&cal);
    return cal.calibrated;
}

esp_err_t touch_cal_store_save(const touch_cal_t *cal)
{
    if (!cal) return ESP_ERR_INVALID_ARG;

    esp_err_t part_err = nvs_partition_init(NVS_PARTITION);
    if (part_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS partition '%s' init failed: %s -- calibration not saved", NVS_PARTITION,
                 esp_err_to_name(part_err));
        return part_err;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(NVS_PARTITION, NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open_from_partition failed: %s -- calibration not saved",
                 esp_err_to_name(err));
        return err;
    }

    touch_cal_record_t rec = {
        .version = TOUCH_CAL_RECORD_VERSION,
        .reserved = { 0, 0, 0 },
        .a = cal->a, .b = cal->b, .c = cal->c,
        .d = cal->d, .e = cal->e, .f = cal->f,
    };
    err = nvs_set_blob(h, NVS_KEY_CAL, &rec, sizeof(rec));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not persist touch calibration: %s -- will not survive a reboot",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "touch calibration saved: x=%.5f*rx+%.5f*ry+%.2f y=%.5f*rx+%.5f*ry+%.2f",
                 (double)rec.a, (double)rec.b, (double)rec.c, (double)rec.d, (double)rec.e,
                 (double)rec.f);
    }
    return err;
}

/* 3x3 determinant, expansion by the first row -- n is always exactly 3 here
 * (never a general NxN solver), so this is clearer than a generic
 * elimination routine for the one shape this is ever called with. */
static double det3(const double m[3][3])
{
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

/* Solves M * x = rhs for a 3x3 M via Cramer's rule (M is the same normal-
 * equations matrix for both the screen_x and screen_y fits -- see this
 * file's header comment -- so det_m is computed once by the caller and
 * passed in rather than recomputed per solve). Returns false if M is
 * singular (collinear calibration points). */
static bool solve3_cramer(const double m[3][3], double det_m, const double rhs[3], double x[3])
{
    if (fabs(det_m) < 1e-9) return false;

    for (int col = 0; col < 3; col++) {
        double mc[3][3];
        memcpy(mc, m, sizeof(mc));
        for (int row = 0; row < 3; row++) mc[row][col] = rhs[row];
        x[col] = det3(mc) / det_m;
    }
    return true;
}

esp_err_t touch_cal_fit(const uint16_t *raw_x, const uint16_t *raw_y, const int32_t *screen_x,
                         const int32_t *screen_y, size_t n, touch_cal_t *out)
{
    if (!raw_x || !raw_y || !screen_x || !screen_y || !out || n < 3) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Normal-equations sums for design rows [rx, ry, 1]. */
    double sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0;
    double sx_tx = 0, sy_tx = 0, s_tx = 0; /* against screen_x */
    double sx_ty = 0, sy_ty = 0, s_ty = 0; /* against screen_y */

    for (size_t i = 0; i < n; i++) {
        double rx = (double)raw_x[i];
        double ry = (double)raw_y[i];
        double tx = (double)screen_x[i];
        double ty = (double)screen_y[i];

        sxx += rx * rx;
        sxy += rx * ry;
        syy += ry * ry;
        sx += rx;
        sy += ry;

        sx_tx += rx * tx;
        sy_tx += ry * tx;
        s_tx += tx;

        sx_ty += rx * ty;
        sy_ty += ry * ty;
        s_ty += ty;
    }

    double m[3][3] = {
        { sxx, sxy, sx },
        { sxy, syy, sy },
        { sx, sy, (double)n },
    };
    double det_m = det3(m);

    double coeff_x[3], coeff_y[3];
    double rhs_x[3] = { sx_tx, sy_tx, s_tx };
    double rhs_y[3] = { sx_ty, sy_ty, s_ty };

    if (!solve3_cramer(m, det_m, rhs_x, coeff_x) || !solve3_cramer(m, det_m, rhs_y, coeff_y)) {
        ESP_LOGE(TAG, "touch_cal_fit: degenerate calibration points (collinear raw samples)");
        return ESP_ERR_INVALID_ARG;
    }

    out->a = (float)coeff_x[0];
    out->b = (float)coeff_x[1];
    out->c = (float)coeff_x[2];
    out->d = (float)coeff_y[0];
    out->e = (float)coeff_y[1];
    out->f = (float)coeff_y[2];
    return ESP_OK;
}

void touch_cal_apply(const touch_cal_t *cal, uint16_t raw_x, uint16_t raw_y, uint16_t width,
                      uint16_t height, int32_t *out_x, int32_t *out_y)
{
    double rx = (double)raw_x;
    double ry = (double)raw_y;
    double x = (double)cal->a * rx + (double)cal->b * ry + (double)cal->c;
    double y = (double)cal->d * rx + (double)cal->e * ry + (double)cal->f;

    if (x < 0) x = 0;
    if (x > (double)(width - 1)) x = (double)(width - 1);
    if (y < 0) y = 0;
    if (y > (double)(height - 1)) y = (double)(height - 1);

    *out_x = (int32_t)(x + 0.5);
    *out_y = (int32_t)(y + 0.5);
}
