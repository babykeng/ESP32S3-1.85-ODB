#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

esp_err_t pcf85063_init(void);
bool pcf85063_is_available(void);
esp_err_t pcf85063_read_time(struct tm *timeinfo);
esp_err_t pcf85063_write_time(const struct tm *timeinfo);
esp_err_t pcf85063_set_system_time_from_rtc(void);
esp_err_t pcf85063_write_system_time(void);

