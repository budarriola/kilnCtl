/* hal_esp_common.c -- see hal_esp_common.h for the mapping rationale. */
#include "hal_esp_common.h"

hal_status_t hal_esp_err_to_status(esp_err_t err) {
    switch (err) {
        case ESP_OK:                 return HAL_OK;
        case ESP_ERR_TIMEOUT:        return HAL_TIMEOUT;
        case ESP_ERR_INVALID_ARG:    return HAL_INVALID_ARG;
        case ESP_ERR_INVALID_STATE:  return HAL_NOT_READY;
        case ESP_ERR_INVALID_SIZE:   return HAL_INVALID_SIZE;
        case ESP_ERR_NO_MEM:         return HAL_NO_MEM;
        case ESP_ERR_NOT_FOUND:      return HAL_NOT_FOUND;
        case ESP_ERR_NOT_SUPPORTED:  return HAL_NOT_SUPPORTED;
        default:                     return HAL_IO;
    }
}

esp_err_t hal_status_to_esp_err(hal_status_t status) {
    switch (status) {
        case HAL_OK:             return ESP_OK;
        case HAL_TIMEOUT:        return ESP_ERR_TIMEOUT;
        case HAL_INVALID_ARG:    return ESP_ERR_INVALID_ARG;
        case HAL_NOT_READY:      return ESP_ERR_INVALID_STATE;
        case HAL_INVALID_SIZE:   return ESP_ERR_INVALID_SIZE;
        case HAL_NO_MEM:         return ESP_ERR_NO_MEM;
        case HAL_NOT_FOUND:      return ESP_ERR_NOT_FOUND;
        case HAL_NOT_SUPPORTED:  return ESP_ERR_NOT_SUPPORTED;
        default:                 return ESP_FAIL;
    }
}
