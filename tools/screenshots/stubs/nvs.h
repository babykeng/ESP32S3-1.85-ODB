#ifndef NVS_H
#define NVS_H

#include <stdint.h>
#include "esp_err.h"

typedef int nvs_handle_t;

#define NVS_READONLY 1
#define NVS_READWRITE 2

static inline esp_err_t nvs_open(const char *name, int open_mode, nvs_handle_t *out_handle)
{
    (void)name;
    (void)open_mode;
    if(out_handle) *out_handle = 0;
    return ESP_ERR_NOT_FOUND;
}

static inline esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out_value)
{
    (void)handle;
    (void)key;
    if(out_value) *out_value = 0;
    return ESP_ERR_NOT_FOUND;
}

static inline esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return ESP_OK;
}

static inline esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

static inline void nvs_close(nvs_handle_t handle)
{
    (void)handle;
}

#endif
