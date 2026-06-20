#ifndef REMOTE_CONTROL_H
#define REMOTE_CONTROL_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t remote_control_start(void);
void remote_control_ui_init(void);

#ifdef __cplusplus
}
#endif

#endif
