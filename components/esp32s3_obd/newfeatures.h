#ifndef NEWFEATURES_H
#define NEWFEATURES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "ui.h"

typedef struct {
    const char *wifi_ssid;
    const char *wifi_pass;
    const char *timezone;
} newfeatures_env_config_t;

esp_err_t newfeatures_env_init(const newfeatures_env_config_t *config);
void newfeatures_start_wifi_ntp(const char *ssid, const char *pass);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
