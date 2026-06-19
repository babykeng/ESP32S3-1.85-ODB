#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "components/esp32s3_obd/obd_ble.h"
#include "components/esp32s3_obd/gps_uart.h"
#include "components/esp32s3_obd/bsp_obd_dsp/qmi8658/qmi8658.h"
#include "components/esp32s3_obd/bsp_obd_dsp/pcf85063/pcf85063.h"

void obd_ble_start(void)
{
}

void obd_ble_get_snapshot(obd_ble_snapshot_t *snapshot)
{
    if(!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = OBD_BLE_STATE_IDLE;
    strcpy(snapshot->status, "Idle");
}

void obd_ble_get_devices(obd_ble_device_list_t *list)
{
    if(!list) return;
    memset(list, 0, sizeof(*list));
}

void obd_ble_scan_all(void)
{
}

void obd_ble_connect_by_name(const char *name)
{
    (void)name;
}

esp_err_t newfeatures_gps_uart_start(void)
{
    return ESP_OK;
}

void newfeatures_gps_uart_get_snapshot(newfeatures_gps_uart_snapshot_t *snapshot)
{
    if(!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
}

void newfeatures_gps_uart_clear(void)
{
}

esp_err_t qmi8658_init(void)
{
    return ESP_FAIL;
}

bool qmi8658_is_available(void)
{
    return false;
}

qmi8658_status_t qmi8658_get_status(void)
{
    qmi8658_status_t status = {0};
    return status;
}

esp_err_t qmi8658_calibrate_current(void)
{
    return ESP_FAIL;
}

void qmi8658_clear_calibration(void)
{
}

esp_err_t qmi8658_read(qmi8658_data_t *data)
{
    if(data) memset(data, 0, sizeof(*data));
    return ESP_FAIL;
}

esp_err_t pcf85063_init(void)
{
    return ESP_FAIL;
}

bool pcf85063_is_available(void)
{
    return false;
}

esp_err_t pcf85063_read_time(struct tm *timeinfo)
{
    (void)timeinfo;
    return ESP_FAIL;
}

esp_err_t pcf85063_write_time(const struct tm *timeinfo)
{
    (void)timeinfo;
    return ESP_OK;
}

esp_err_t pcf85063_set_system_time_from_rtc(void)
{
    return ESP_FAIL;
}

esp_err_t pcf85063_write_system_time(void)
{
    return ESP_FAIL;
}
