/* fake_sysinfo.c -- host fake backend for hal_sysinfo.h. See fake_sysinfo.h. */
#include "fake_sysinfo.h"

#include <string.h>

#define FAKE_SYSINFO_MAX_RANDOM_SEQUENCE 32u
#define FAKE_SYSINFO_RANDOM_FALLBACK 0xA5A5A5A5u

static hal_reset_reason_t           s_reset_reason;
static bool                         s_partition_set;
static hal_sysinfo_partition_info_t s_partition;
static hal_sysinfo_build_info_t     s_build_info;

static bool     s_temp_initialized;
static float    s_temp_celsius;
static bool     s_temp_init_status_armed;
static hal_status_t s_temp_init_status;
static bool     s_temp_read_status_armed;
static hal_status_t s_temp_read_status;

static uint32_t s_random_seq[FAKE_SYSINFO_MAX_RANDOM_SEQUENCE];
static uint32_t s_random_seq_count;
static uint32_t s_random_seq_next;

static bool s_coredump_present;

void fake_sysinfo_reset_all(void) {
    s_reset_reason = HAL_RESET_POWERON;

    s_partition_set = false;
    memset(&s_partition, 0, sizeof(s_partition));

    memset(&s_build_info, 0, sizeof(s_build_info));
    s_build_info.valid = false;

    s_temp_initialized = false;
    s_temp_celsius = 0.0f;
    s_temp_init_status_armed = false;
    s_temp_init_status = HAL_OK;
    s_temp_read_status_armed = false;
    s_temp_read_status = HAL_OK;

    memset(s_random_seq, 0, sizeof(s_random_seq));
    s_random_seq_count = 0;
    s_random_seq_next = 0;

    s_coredump_present = false;
}

void fake_sysinfo_set_reset_reason(hal_reset_reason_t reason) {
    s_reset_reason = reason;
}

void fake_sysinfo_set_running_partition(const hal_sysinfo_partition_info_t *info) {
    if (info == NULL) {
        s_partition_set = false;
        memset(&s_partition, 0, sizeof(s_partition));
        return;
    }
    s_partition = *info;
    s_partition_set = true;
}

void fake_sysinfo_set_build_info(const hal_sysinfo_build_info_t *info) {
    if (info == NULL) {
        fake_sysinfo_set_build_info_invalid();
        return;
    }
    s_build_info = *info;
    s_build_info.valid = true;
}

void fake_sysinfo_set_build_info_invalid(void) {
    memset(&s_build_info, 0, sizeof(s_build_info));
    s_build_info.valid = false;
}

void fake_sysinfo_set_temp_celsius(float celsius) {
    s_temp_celsius = celsius;
}

void fake_sysinfo_script_temp_init_status(hal_status_t status) {
    if (status == HAL_OK) {
        s_temp_init_status_armed = false;
        return;
    }
    s_temp_init_status_armed = true;
    s_temp_init_status = status;
}

void fake_sysinfo_script_temp_read_status(hal_status_t status) {
    if (status == HAL_OK) {
        s_temp_read_status_armed = false;
        return;
    }
    s_temp_read_status_armed = true;
    s_temp_read_status = status;
}

void fake_sysinfo_script_random_sequence(const uint32_t *values, uint32_t count) {
    if (count > FAKE_SYSINFO_MAX_RANDOM_SEQUENCE) {
        count = FAKE_SYSINFO_MAX_RANDOM_SEQUENCE;
    }
    if (count == 0 || values == NULL) {
        s_random_seq_count = 0;
        s_random_seq_next = 0;
        return;
    }
    memcpy(s_random_seq, values, count * sizeof(uint32_t));
    s_random_seq_count = count;
    s_random_seq_next = 0;
}

void fake_sysinfo_set_coredump_present(bool present) {
    s_coredump_present = present;
}

/* --- hal_sysinfo.h implementation --- */

hal_reset_reason_t hal_sysinfo_reset_reason(void) {
    return s_reset_reason;
}

hal_status_t hal_sysinfo_get_running_partition(hal_sysinfo_partition_info_t *out) {
    if (out == NULL) {
        return HAL_INVALID_ARG;
    }
    if (!s_partition_set) {
        /* Matches the real backend's "esp_ota_get_running_partition()
         * returned NULL" case. */
        return HAL_IO;
    }
    *out = s_partition;
    return HAL_OK;
}

void hal_sysinfo_get_build_info(hal_sysinfo_build_info_t *out) {
    if (out == NULL) {
        return;
    }
    *out = s_build_info;
}

hal_status_t hal_sysinfo_temp_init(void) {
    if (s_temp_init_status_armed) {
        s_temp_init_status_armed = false;
        return s_temp_init_status;
    }
    if (s_temp_initialized) {
        return HAL_OK; /* real backend's own already-up no-op */
    }
    s_temp_initialized = true;
    return HAL_OK;
}

hal_status_t hal_sysinfo_temp_read_celsius(float *out_c) {
    if (out_c == NULL) {
        return HAL_INVALID_ARG;
    }
    if (!s_temp_initialized) {
        return HAL_NOT_READY;
    }
    if (s_temp_read_status_armed) {
        s_temp_read_status_armed = false;
        return s_temp_read_status;
    }
    *out_c = s_temp_celsius;
    return HAL_OK;
}

hal_status_t hal_sysinfo_temp_deinit(void) {
    if (!s_temp_initialized) {
        return HAL_NOT_READY;
    }
    s_temp_initialized = false;
    return HAL_OK;
}

uint32_t hal_sysinfo_random_u32(void) {
    if (s_random_seq_next < s_random_seq_count) {
        return s_random_seq[s_random_seq_next++];
    }
    return FAKE_SYSINFO_RANDOM_FALLBACK;
}

void hal_sysinfo_fill_random(void *buf, size_t len) {
    if (buf == NULL || len == 0) {
        return;
    }
    uint8_t *out = (uint8_t *)buf;
    size_t i = 0;
    while (i < len) {
        uint32_t word = hal_sysinfo_random_u32();
        size_t chunk = (len - i < sizeof(word)) ? (len - i) : sizeof(word);
        memcpy(out + i, &word, chunk);
        i += chunk;
    }
}

bool hal_sysinfo_coredump_present(void) {
    return s_coredump_present;
}

hal_status_t hal_sysinfo_coredump_erase(void) {
    s_coredump_present = false;
    return HAL_OK;
}
