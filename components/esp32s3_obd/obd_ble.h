#ifndef OBD_BLE_H
#define OBD_BLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#define OBD_BLE_DEVICE_NAME_LEN 32
#define OBD_BLE_MAX_DEVICES     8

typedef enum {
    OBD_BLE_STATE_IDLE = 0,
    OBD_BLE_STATE_SCANNING,
    OBD_BLE_STATE_CONNECTING,
    OBD_BLE_STATE_DISCOVERING,
    OBD_BLE_STATE_READY,
    OBD_BLE_STATE_DISCONNECTED,
    OBD_BLE_STATE_ERROR,
} obd_ble_state_t;

typedef struct {
    obd_ble_state_t state;
    bool can_error;
    bool has_speed;
    bool has_rpm;
    bool has_fuel;
    bool has_coolant_temp;
    bool has_intake_temp;
    bool has_engine_load;
    bool has_throttle;
    bool has_voltage;
    bool has_oil_temp;
    uint16_t speed_kmh;
    uint16_t rpm;
    uint8_t fuel_percent;
    int16_t coolant_temp_c;
    int16_t intake_temp_c;
    uint8_t engine_load_percent;
    uint8_t throttle_percent;
    uint16_t voltage_mv;
    int16_t oil_temp_c;
    char status[32];
    char last_response[96];
    char connected_name[OBD_BLE_DEVICE_NAME_LEN];
    char saved_name[OBD_BLE_DEVICE_NAME_LEN];
} obd_ble_snapshot_t;

typedef struct {
    char name[OBD_BLE_DEVICE_NAME_LEN];
    int8_t rssi;
} obd_ble_device_t;

typedef struct {
    uint8_t count;
    obd_ble_device_t devices[OBD_BLE_MAX_DEVICES];
} obd_ble_device_list_t;

void obd_ble_start(void);
void obd_ble_get_snapshot(obd_ble_snapshot_t *snapshot);
void obd_ble_get_devices(obd_ble_device_list_t *list);
void obd_ble_scan_all(void);
void obd_ble_connect_by_name(const char *name);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
