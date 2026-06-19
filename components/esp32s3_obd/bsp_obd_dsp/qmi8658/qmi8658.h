#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int16_t acc_raw[3];
    int16_t gyro_raw[3];
    float acc_g[3];
    float gyro_dps[3];
} qmi8658_data_t;

typedef struct {
    uint8_t i2c_port;
    uint8_t addr;
    uint8_t who_am_i;
} qmi8658_status_t;

esp_err_t qmi8658_init(void);
bool qmi8658_is_available(void);
qmi8658_status_t qmi8658_get_status(void);
esp_err_t qmi8658_calibrate_current(void);
void qmi8658_clear_calibration(void);
esp_err_t qmi8658_read(qmi8658_data_t *data);
