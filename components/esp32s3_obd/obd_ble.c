#include "obd_ble.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "obd_data_cache.h"

void ble_store_config_init(void);

static const char *TAG = "obd_ble";
static const char *NVS_NAMESPACE = "obd_ble";
static const char *NVS_LAST_NAME_KEY = "last_name";

#define BLE_SCAN_RESTART_STACK_SIZE 4096

static const ble_uuid16_t CCCD_UUID = BLE_UUID16_INIT(BLE_GATT_DSC_CLT_CFG_UUID16);

static const uint8_t CMD_ATZ[] = "ATZ\r";
static const uint8_t CMD_ATE0[] = "ATE0\r";
static const uint8_t CMD_ATL0[] = "ATL0\r";
static const uint8_t CMD_ATS1[] = "ATS1\r";
static const uint8_t CMD_ATH0[] = "ATH0\r";
static const uint8_t CMD_ATAT1[] = "ATAT1\r";
static const uint8_t CMD_ATST19[] = "ATST19\r";
static const uint8_t CMD_ATSP0[] = "ATSP0\r";
static const uint8_t CMD_PID_SUPPORT[] = "0100\r";
static const uint8_t CMD_RPM[] = "010C\r";
static const uint8_t CMD_SPEED[] = "010D\r";
static const uint8_t CMD_FUEL[] = "012F\r";
static const uint8_t CMD_TEMP[] = "0105\r";
static const uint8_t CMD_INTAKE_TEMP[] = "010F\r";
static const uint8_t CMD_ENGINE_LOAD[] = "0104\r";
static const uint8_t CMD_THROTTLE[] = "0111\r";
static const uint8_t CMD_VOLTAGE[] = "0142\r";
static const uint8_t CMD_OIL_TEMP[] = "015C\r";

typedef struct {
    bool found;
    uint16_t start;
    uint16_t end;
} service_range_t;

static SemaphoreHandle_t s_snapshot_lock;
static obd_ble_snapshot_t s_snapshot = {
    .state = OBD_BLE_STATE_IDLE,
    .status = "Idle",
};

static uint16_t s_conn_handle;
static uint16_t s_service_start;
static uint16_t s_service_end;
static uint16_t s_all_attr_end;
static uint16_t s_write_handle;
static uint16_t s_notify_handle;
static uint16_t s_notify_dsc_end;
static bool s_write_no_rsp;
static bool s_notify_indicate;
static bool s_cccd_found;
static service_range_t s_svc_fff0;
static service_range_t s_svc_18f0;
static service_range_t s_svc_ff12;
static uint8_t s_own_addr_type;
static bool s_started;
static bool s_ready;
static volatile bool s_elm_ready = true;
static bool s_host_synced;
static bool s_scan_restart_pending;
static bool s_scan_restart_task_running;
static bool s_scan_target_only;
static char s_target_name[OBD_BLE_DEVICE_NAME_LEN];
static char s_connecting_name[OBD_BLE_DEVICE_NAME_LEN];
static obd_ble_device_list_t s_devices;

#define RX_ACCUM_SIZE 512
static char s_rx_accum[RX_ACCUM_SIZE];
static size_t s_rx_accum_len;

static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void start_scan(void);

static void scan_restart_task(void *arg)
{
    (void)arg;
    for(uint8_t i = 0; i < 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if(!s_host_synced || !s_scan_restart_pending) break;
        if(!ble_gap_disc_active()) {
            s_scan_restart_pending = false;
            start_scan();
            break;
        }
    }
    s_scan_restart_task_running = false;
    vTaskDelete(NULL);
}

static void restart_scan(void)
{
    if(!s_host_synced) return;
    if(ble_gap_disc_active()) {
        s_scan_restart_pending = true;
        int rc = ble_gap_disc_cancel();
        if(rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "scan cancel failed rc=%d, restart directly", rc);
            s_scan_restart_pending = false;
            start_scan();
            return;
        }

        if(!s_scan_restart_task_running) {
            s_scan_restart_task_running = true;
            if(xTaskCreate(scan_restart_task, "ble_scan_restart", BLE_SCAN_RESTART_STACK_SIZE,
                           NULL, 3, NULL) != pdPASS) {
                s_scan_restart_task_running = false;
                s_scan_restart_pending = false;
                ESP_LOGW(TAG, "scan restart task create failed");
            }
        }
    } else {
        start_scan();
    }
}

static void set_status(obd_ble_state_t state, const char *status)
{
    ESP_LOGI(TAG, "state=%d status=%s", state, status);
    if(s_snapshot_lock) xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    s_snapshot.state = state;
    strlcpy(s_snapshot.status, status, sizeof(s_snapshot.status));
    if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);
}

static void load_saved_name(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if(err != ESP_OK) return;

    size_t len = sizeof(s_snapshot.saved_name);
    err = nvs_get_str(nvs, NVS_LAST_NAME_KEY, s_snapshot.saved_name, &len);
    if(err != ESP_OK) s_snapshot.saved_name[0] = '\0';
    nvs_close(nvs);
}

static void save_connected_name(const char *name)
{
    if(!name || name[0] == '\0') return;
    ESP_LOGI(TAG, "save connected device name: %s", name);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "open nvs failed err=%s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_str(nvs, NVS_LAST_NAME_KEY, name);
    if(err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if(err != ESP_OK) {
        ESP_LOGW(TAG, "save device name failed err=%s", esp_err_to_name(err));
        return;
    }

    if(s_snapshot_lock) xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    strlcpy(s_snapshot.saved_name, name, sizeof(s_snapshot.saved_name));
    strlcpy(s_snapshot.connected_name, name, sizeof(s_snapshot.connected_name));
    if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);
}

static void clear_saved_name(void)
{
    ESP_LOGW(TAG, "clear saved bluetooth device name");
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if(err == ESP_OK) {
        nvs_erase_key(nvs, NVS_LAST_NAME_KEY);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    if(s_snapshot_lock) xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    s_snapshot.saved_name[0] = '\0';
    if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);
}

static void stop_target_scan(const char *status)
{
    ESP_LOGW(TAG, "stop target scan: %s", status);
    if(s_snapshot_lock) xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    s_target_name[0] = '\0';
    s_connecting_name[0] = '\0';
    s_scan_target_only = false;
    if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);
    set_status(OBD_BLE_STATE_ERROR, status);
}

static int hex_value(char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool parse_hex_byte(const char *text, uint8_t *value)
{
    int hi = hex_value(text[0]);
    int lo = hex_value(text[1]);
    if(hi < 0 || lo < 0) return false;
    *value = (uint8_t)((hi << 4) | lo);
    return true;
}

static bool response_has_error(const char *text)
{
    return text &&
           (strstr(text, "ERROR") ||
            strstr(text, "UNABLE") ||
            strstr(text, "BUSINIT") ||
            strstr(text, "CANERROR") ||
            strstr(text, "STOPPED") ||
            strstr(text, "BUFFERFULL"));
}

static void compact_obd_text(char *dst, size_t dst_size, const uint8_t *data, size_t len)
{
    size_t out = 0;
    for(size_t i = 0; i < len && out + 1 < dst_size; i++) {
        char c = (char)data[i];
        if(c == '\r' || c == '\n' || c == '>' || c == ' ') continue;
        if(isprint((unsigned char)c)) dst[out++] = (char)toupper((unsigned char)c);
    }
    dst[out] = '\0';
}

static void parse_obd_response(const uint8_t *data, size_t len)
{
    char text[96];
    compact_obd_text(text, sizeof(text), data, len);
    if(text[0] == '\0') return;

    if(s_snapshot_lock) xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    strlcpy(s_snapshot.last_response, text, sizeof(s_snapshot.last_response));

    if(response_has_error(text) || strstr(text, "NODATA")) {
        s_snapshot.can_error = true;
        if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);
        return;
    }

    char *frame = strstr(text, "41");
    if(!frame || strlen(frame) < 6) {
        if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);
        return;
    }

    uint8_t pid = 0;
    if(!parse_hex_byte(frame + 2, &pid)) {
        if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);
        return;
    }

    uint8_t a = 0;
    uint8_t b = 0;
    switch(pid) {
    case 0x0D:
        if(strlen(frame) >= 6 && parse_hex_byte(frame + 4, &a)) {
            s_snapshot.speed_kmh = a;
            s_snapshot.has_speed = true;
            s_snapshot.can_error = false;
            obd_data_set_speed(a);
        }
        break;
    case 0x0C:
        if(strlen(frame) >= 8 && parse_hex_byte(frame + 4, &a) && parse_hex_byte(frame + 6, &b)) {
            s_snapshot.rpm = (uint16_t)((((uint16_t)a << 8) | b) / 4U);
            s_snapshot.has_rpm = true;
            s_snapshot.can_error = false;
            obd_data_set_rpm(s_snapshot.rpm);
        }
        break;
    case 0x2F:
        if(strlen(frame) >= 6 && parse_hex_byte(frame + 4, &a)) {
            s_snapshot.fuel_percent = (uint8_t)(((uint16_t)a * 100U) / 255U);
            s_snapshot.has_fuel = true;
            s_snapshot.can_error = false;
            obd_data_set_fuel_percent(s_snapshot.fuel_percent);
        }
        break;
    case 0x05:
        if(strlen(frame) >= 6 && parse_hex_byte(frame + 4, &a)) {
            s_snapshot.coolant_temp_c = (int16_t)a - 40;
            s_snapshot.has_coolant_temp = true;
            s_snapshot.can_error = false;
            obd_data_set_coolant_temp(s_snapshot.coolant_temp_c);
        }
        break;
    case 0x0F:
        if(strlen(frame) >= 6 && parse_hex_byte(frame + 4, &a)) {
            s_snapshot.intake_temp_c = (int16_t)a - 40;
            s_snapshot.has_intake_temp = true;
            s_snapshot.can_error = false;
            obd_data_set_intake_temp(s_snapshot.intake_temp_c);
        }
        break;
    case 0x04:
        if(strlen(frame) >= 6 && parse_hex_byte(frame + 4, &a)) {
            s_snapshot.engine_load_percent = (uint8_t)(((uint16_t)a * 100U) / 255U);
            s_snapshot.has_engine_load = true;
            s_snapshot.can_error = false;
            obd_data_set_engine_load(s_snapshot.engine_load_percent);
        }
        break;
    case 0x11:
        if(strlen(frame) >= 6 && parse_hex_byte(frame + 4, &a)) {
            s_snapshot.throttle_percent = (uint8_t)(((uint16_t)a * 100U) / 255U);
            s_snapshot.has_throttle = true;
            s_snapshot.can_error = false;
            obd_data_set_throttle(s_snapshot.throttle_percent);
        }
        break;
    case 0x42:
        if(strlen(frame) >= 8 && parse_hex_byte(frame + 4, &a) && parse_hex_byte(frame + 6, &b)) {
            s_snapshot.voltage_mv = (uint16_t)((((uint16_t)a << 8) | b));
            s_snapshot.has_voltage = true;
            s_snapshot.can_error = false;
            obd_data_set_voltage(s_snapshot.voltage_mv);
        }
        break;
    case 0x5C:
        if(strlen(frame) >= 6 && parse_hex_byte(frame + 4, &a)) {
            s_snapshot.oil_temp_c = (int16_t)a - 40;
            s_snapshot.has_oil_temp = true;
            s_snapshot.can_error = false;
            obd_data_set_oil_temp(s_snapshot.oil_temp_c);
        }
        break;
    default:
        break;
    }

    if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);
}

void obd_ble_get_snapshot(obd_ble_snapshot_t *snapshot)
{
    if(!snapshot) return;
    if(s_snapshot_lock) xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    *snapshot = s_snapshot;
    if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);

    snapshot->has_speed = obd_data_has_speed();
    if(snapshot->has_speed) snapshot->speed_kmh = obd_data_get_speed();
    snapshot->has_rpm = obd_data_has_rpm();
    if(snapshot->has_rpm) snapshot->rpm = obd_data_get_rpm();
    snapshot->has_fuel = obd_data_has_fuel_percent();
    if(snapshot->has_fuel) snapshot->fuel_percent = obd_data_get_fuel_percent();
    snapshot->has_coolant_temp = obd_data_has_coolant_temp();
    if(snapshot->has_coolant_temp) snapshot->coolant_temp_c = obd_data_get_coolant_temp();
    snapshot->has_intake_temp = obd_data_has_intake_temp();
    if(snapshot->has_intake_temp) snapshot->intake_temp_c = obd_data_get_intake_temp();
    snapshot->has_engine_load = obd_data_has_engine_load();
    if(snapshot->has_engine_load) snapshot->engine_load_percent = obd_data_get_engine_load();
    snapshot->has_throttle = obd_data_has_throttle();
    if(snapshot->has_throttle) snapshot->throttle_percent = obd_data_get_throttle();
    snapshot->has_voltage = obd_data_has_voltage();
    if(snapshot->has_voltage) snapshot->voltage_mv = obd_data_get_voltage();
    snapshot->has_oil_temp = obd_data_has_oil_temp();
    if(snapshot->has_oil_temp) snapshot->oil_temp_c = obd_data_get_oil_temp();
}

void obd_ble_get_devices(obd_ble_device_list_t *list)
{
    if(!list) return;
    if(s_snapshot_lock) xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    *list = s_devices;
    if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);
}

static bool get_adv_name(const struct ble_gap_disc_desc *disc, char *name, size_t name_size)
{
    struct ble_hs_adv_fields fields;
    name[0] = '\0';
    if(disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
       disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND &&
       disc->event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
        return false;
    }

    if(ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return false;
    }

    if(!fields.name || fields.name_len == 0) return false;

    size_t copy_len = fields.name_len;
    if(copy_len >= name_size) copy_len = name_size - 1;
    memcpy(name, fields.name, copy_len);
    name[copy_len] = '\0';
    return name[0] != '\0';
}

static void add_scan_device(const char *name, int8_t rssi)
{
    if(!name || name[0] == '\0') return;

    if(s_snapshot_lock) xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    for(uint8_t i = 0; i < s_devices.count; i++) {
        if(strcmp(s_devices.devices[i].name, name) == 0) {
            s_devices.devices[i].rssi = rssi;
            if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);
            return;
        }
    }

    if(s_devices.count < OBD_BLE_MAX_DEVICES) {
        strlcpy(s_devices.devices[s_devices.count].name, name, sizeof(s_devices.devices[s_devices.count].name));
        s_devices.devices[s_devices.count].rssi = rssi;
        s_devices.count++;
        ESP_LOGI(TAG, "scan device[%u]: name=%s rssi=%d", s_devices.count - 1, name, rssi);
    } else {
        ESP_LOGW(TAG, "scan list full, ignore device: name=%s rssi=%d", name, rssi);
    }
    if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);
}

static void uuid_to_text(const ble_uuid_t *uuid, char *out, size_t out_size)
{
    if(!uuid || !out || out_size == 0) return;
    if(uuid->type == BLE_UUID_TYPE_16) {
        snprintf(out, out_size, "0x%04X", ble_uuid_u16(uuid));
    } else {
        snprintf(out, out_size, "uuid%d", uuid->type);
    }
}

static void mark_service_range(uint16_t uuid16, uint16_t start, uint16_t end)
{
    service_range_t *range = NULL;
    if(uuid16 == 0xFFF0) {
        range = &s_svc_fff0;
    } else if(uuid16 == 0x18F0) {
        range = &s_svc_18f0;
    } else if(uuid16 == 0xFF12) {
        range = &s_svc_ff12;
    }

    if(range) {
        range->found = true;
        range->start = start;
        range->end = end;
    }
}

static bool select_service_range(void)
{
    const char *selected = NULL;
    if(s_svc_fff0.found) {
        s_service_start = s_svc_fff0.start;
        s_service_end = s_svc_fff0.end;
        selected = "FFF0";
    } else if(s_svc_18f0.found) {
        s_service_start = s_svc_18f0.start;
        s_service_end = s_svc_18f0.end;
        selected = "18F0";
    } else if(s_svc_ff12.found) {
        s_service_start = s_svc_ff12.start;
        s_service_end = s_svc_ff12.end;
        selected = "FF12";
    } else if(s_all_attr_end > 1) {
        s_service_start = 1;
        s_service_end = s_all_attr_end;
        selected = "full-range";
    }

    if(!selected || s_service_start == 0 || s_service_end <= s_service_start) {
        return false;
    }

        ESP_LOGI(TAG, "selected GATT range=%s handle=0x%04X~0x%04X",
             selected, s_service_start, s_service_end);
    return true;
}

static bool write_obd_now(const uint8_t *cmd, size_t len)
{
    if(!s_ready || s_write_handle == 0 || len == 0 || !cmd) return false;
    ESP_LOGD(TAG, "tx obd: %.*s", (int)len, (const char *)cmd);
    int rc = s_write_no_rsp ?
             ble_gattc_write_no_rsp_flat(s_conn_handle, s_write_handle, cmd, len) :
             ble_gattc_write_flat(s_conn_handle, s_write_handle, cmd, len, NULL, NULL);
    if(rc != 0) {
        ESP_LOGW(TAG, "write failed rc=%d handle=0x%04X no_rsp=%d", rc, s_write_handle, s_write_no_rsp);
        s_elm_ready = true;
    }
    return rc == 0;
}

static bool write_obd_blocking(const uint8_t *cmd, size_t len)
{
    uint32_t waited_ms = 0;
    while(s_ready && !s_elm_ready && waited_ms < 1000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }

    if(!s_ready) return false;
    if(!s_elm_ready) {
        ESP_LOGW(TAG, "timeout waiting ELM prompt, force send: %.*s", (int)len, (const char *)cmd);
        s_elm_ready = true;
    }

    s_elm_ready = false;
    if(!write_obd_now(cmd, len)) {
        s_elm_ready = true;
        return false;
    }
    return true;
}

static void obd_poll_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "OBD poll task started");
    vTaskDelay(pdMS_TO_TICKS(700));

    const struct {
        const uint8_t *cmd;
        size_t len;
    } init_cmds[] = {
        {CMD_ATZ, sizeof(CMD_ATZ) - 1},
        {CMD_ATE0, sizeof(CMD_ATE0) - 1},
        {CMD_ATL0, sizeof(CMD_ATL0) - 1},
        {CMD_ATS1, sizeof(CMD_ATS1) - 1},
        {CMD_ATH0, sizeof(CMD_ATH0) - 1},
        {CMD_ATAT1, sizeof(CMD_ATAT1) - 1},
        {CMD_ATST19, sizeof(CMD_ATST19) - 1},
        {CMD_ATSP0, sizeof(CMD_ATSP0) - 1},
        {CMD_PID_SUPPORT, sizeof(CMD_PID_SUPPORT) - 1},
    };

    for(size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]) && s_ready; i++) {
        ESP_LOGD(TAG, "ELM init send: %.*s", (int)init_cmds[i].len, (const char *)init_cmds[i].cmd);
        write_obd_blocking(init_cmds[i].cmd, init_cmds[i].len);
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    uint32_t tick = 0;
    uint32_t slow_tick = 0;
    while(s_ready) {
        switch(tick % 12) {
        case 0:
        case 3:
        case 6:
        case 9:
            write_obd_blocking(CMD_SPEED, sizeof(CMD_SPEED) - 1);
            break;
        case 1:
        case 4:
        case 7:
        case 10:
            write_obd_blocking(CMD_RPM, sizeof(CMD_RPM) - 1);
            break;
        case 2:
            write_obd_blocking(CMD_TEMP, sizeof(CMD_TEMP) - 1);
            break;
        case 5:
        case 8:
        case 11:
            switch(slow_tick++ % 6) {
            case 0:
                write_obd_blocking(CMD_FUEL, sizeof(CMD_FUEL) - 1);
                break;
            case 1:
                write_obd_blocking(CMD_ENGINE_LOAD, sizeof(CMD_ENGINE_LOAD) - 1);
                break;
            case 2:
                write_obd_blocking(CMD_THROTTLE, sizeof(CMD_THROTTLE) - 1);
                break;
            case 3:
                write_obd_blocking(CMD_INTAKE_TEMP, sizeof(CMD_INTAKE_TEMP) - 1);
                break;
            case 4:
                write_obd_blocking(CMD_VOLTAGE, sizeof(CMD_VOLTAGE) - 1);
                break;
            default:
                write_obd_blocking(CMD_OIL_TEMP, sizeof(CMD_OIL_TEMP) - 1);
                break;
            }
            break;
        default:
            break;
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    ESP_LOGI(TAG, "OBD poll task stopped");
    vTaskDelete(NULL);
}

static void finish_obd_ready(void)
{
    if(s_ready) return;
    s_ready = true;
    s_elm_ready = true;
    save_connected_name(s_connecting_name);
    set_status(OBD_BLE_STATE_READY, "OBD Connected");
    xTaskCreate(obd_poll_task, "obd_poll", 4096, NULL, 4, NULL);
}

static int subscribe_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    if(error->status == 0) {
        ESP_LOGI(TAG, "%s subscribed, device ready", s_notify_indicate ? "indicate" : "notify");
        finish_obd_ready();
    } else {
        ESP_LOGE(TAG, "subscribe failed status=%d", error->status);
        set_status(OBD_BLE_STATE_ERROR, "Subscribe error");
    }
    return 0;
}

static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle;
    (void)arg;
    if(error->status == 0 && dsc && ble_uuid_cmp(&dsc->uuid.u, &CCCD_UUID.u) == 0) {
        const uint8_t value[2] = {s_notify_indicate ? 2 : 1, 0};
        s_cccd_found = true;
        ESP_LOGI(TAG, "CCCD found handle=0x%04X, enable %s", dsc->handle,
                 s_notify_indicate ? "indicate" : "notify");
        int rc = ble_gattc_write_flat(conn_handle, dsc->handle, value, sizeof(value), subscribe_cb, NULL);
        if(rc != 0) {
            ESP_LOGE(TAG, "cccd write start failed rc=%d", rc);
            set_status(OBD_BLE_STATE_ERROR, "CCCD error");
        }
        return 0;
    }

    if(error->status == BLE_HS_EDONE) {
        if(!s_cccd_found) {
            ESP_LOGE(TAG, "CCCD not found for notify_handle=0x%04X", s_notify_handle);
            set_status(OBD_BLE_STATE_ERROR, "CCCD missing");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    return 0;
}

static int chr_select_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if(error->status == 0 && chr) {
        char uuid_text[24];
        uuid_to_text(&chr->uuid.u, uuid_text, sizeof(uuid_text));
        ESP_LOGD(TAG, "char found uuid=%s def=0x%04X val=0x%04X prop=0x%02X",
                 uuid_text, chr->def_handle, chr->val_handle, chr->properties);

        if(s_write_handle == 0 &&
           (chr->properties & (BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE))) {
            s_write_handle = chr->val_handle;
            s_write_no_rsp = (chr->properties & BLE_GATT_CHR_F_WRITE_NO_RSP) != 0;
            ESP_LOGI(TAG, "selected WRITE handle=0x%04X type=%s",
                     s_write_handle, s_write_no_rsp ? "NO_RSP" : "RSP");
        }

        if(s_notify_handle == 0 &&
           (chr->properties & (BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE))) {
            s_notify_handle = chr->val_handle;
            s_notify_dsc_end = s_service_end;
            s_notify_indicate = (chr->properties & BLE_GATT_CHR_F_NOTIFY) == 0;
            ESP_LOGI(TAG, "selected %s handle=0x%04X",
                     s_notify_indicate ? "INDICATE" : "NOTIFY", s_notify_handle);
        } else if(s_notify_handle != 0 && chr->def_handle > s_notify_handle &&
                  chr->def_handle - 1 < s_notify_dsc_end) {
            s_notify_dsc_end = chr->def_handle - 1;
        }
        return 0;
    }

    if(error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "char discovery complete write=0x%04X notify=0x%04X",
                 s_write_handle, s_notify_handle);
        if(s_write_handle == 0 || s_notify_handle == 0) {
            ESP_LOGE(TAG, "OBD write/notify characteristic missing");
            clear_saved_name();
            stop_target_scan(s_write_handle == 0 ? "Write char missing" : "Notify char missing");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        ESP_LOGI(TAG, "discover descriptors for notify handle=0x%04X end=0x%04X",
                 s_notify_handle, s_notify_dsc_end);
        int rc = ble_gattc_disc_all_dscs(conn_handle, s_notify_handle, s_notify_dsc_end, dsc_cb, NULL);
        if(rc != 0) {
            ESP_LOGE(TAG, "descriptor discovery start failed rc=%d", rc);
            set_status(OBD_BLE_STATE_ERROR, "Descriptor error");
        }
    } else if(error->status != 0) {
        ESP_LOGE(TAG, "char discovery failed status=%d", error->status);
        set_status(OBD_BLE_STATE_ERROR, "Char error");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int svc_all_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if(error->status == 0 && service) {
        char uuid_text[24];
        uuid_to_text(&service->uuid.u, uuid_text, sizeof(uuid_text));
        ESP_LOGD(TAG, "service found uuid=%s handle=0x%04X~0x%04X",
                 uuid_text, service->start_handle, service->end_handle);
        if(service->end_handle > s_all_attr_end) s_all_attr_end = service->end_handle;
        if(service->uuid.u.type == BLE_UUID_TYPE_16) {
            mark_service_range(ble_uuid_u16(&service->uuid.u),
                               service->start_handle, service->end_handle);
        }
        return 0;
    }

    if(error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "service discovery complete FFF0=%d 18F0=%d FF12=%d max=0x%04X",
                 s_svc_fff0.found, s_svc_18f0.found, s_svc_ff12.found, s_all_attr_end);
        if(!select_service_range()) {
            ESP_LOGE(TAG, "no service range available");
            clear_saved_name();
            stop_target_scan("OBD service missing");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }

        int rc = ble_gattc_disc_all_chrs(conn_handle, s_service_start, s_service_end,
                                         chr_select_cb, NULL);
        if(rc != 0) {
            ESP_LOGE(TAG, "all characteristic discovery start failed rc=%d", rc);
            set_status(OBD_BLE_STATE_ERROR, "Char error");
        }
    } else if(error->status != 0) {
        ESP_LOGE(TAG, "service discovery failed status=%d", error->status);
        set_status(OBD_BLE_STATE_ERROR, "Service error");
    }
    return 0;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch(event->type) {
    case BLE_GAP_EVENT_DISC:
    {
        char name[OBD_BLE_DEVICE_NAME_LEN];
        if(get_adv_name(&event->disc, name, sizeof(name))) {
            add_scan_device(name, event->disc.rssi);
        }

        if(name[0] != '\0' && s_target_name[0] != '\0' && strcmp(name, s_target_name) == 0) {
            ESP_LOGI(TAG, "target bluetooth device found: name=%s rssi=%d", name, event->disc.rssi);
            set_status(OBD_BLE_STATE_CONNECTING, "Connecting");
            strlcpy(s_connecting_name, name, sizeof(s_connecting_name));
            ble_gap_disc_cancel();
            int rc = ble_gap_connect(s_own_addr_type, &event->disc.addr, 30000, NULL, gap_event_cb, NULL);
            if(rc != 0) {
                ESP_LOGE(TAG, "connect start failed rc=%d", rc);
                start_scan();
            } else {
                ESP_LOGI(TAG, "connect started: name=%s", name);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        if(event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE connected: conn_handle=%u name=%s",
                     event->connect.conn_handle, s_connecting_name);
            s_conn_handle = event->connect.conn_handle;
            s_service_start = 0;
            s_service_end = 0;
            s_all_attr_end = 0;
            s_write_handle = 0;
            s_notify_handle = 0;
            s_notify_dsc_end = 0;
            s_write_no_rsp = false;
            s_notify_indicate = false;
            s_cccd_found = false;
            s_svc_fff0 = (service_range_t){0};
            s_svc_18f0 = (service_range_t){0};
            s_svc_ff12 = (service_range_t){0};
            s_rx_accum_len = 0;
            s_rx_accum[0] = '\0';
            s_elm_ready = true;
            s_ready = false;
            obd_data_cache_clear();
            set_status(OBD_BLE_STATE_DISCOVERING, "Discovering");
            int rc = ble_gattc_disc_all_svcs(s_conn_handle, svc_all_cb, NULL);
            if(rc != 0) {
                ESP_LOGE(TAG, "service discovery start failed rc=%d", rc);
                set_status(OBD_BLE_STATE_ERROR, "Service error");
            } else {
                ESP_LOGI(TAG, "discover all services");
            }
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            start_scan();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnect reason=%d", event->disconnect.reason);
        s_ready = false;
        s_elm_ready = true;
        s_conn_handle = 0;
        s_write_handle = 0;
        s_notify_handle = 0;
        s_notify_dsc_end = 0;
        s_cccd_found = false;
        s_rx_accum_len = 0;
        s_rx_accum[0] = '\0';
        s_connecting_name[0] = '\0';
        if(s_target_name[0] != '\0') {
            set_status(OBD_BLE_STATE_DISCONNECTED, "Disconnected");
            start_scan();
        } else if(s_snapshot.state != OBD_BLE_STATE_ERROR) {
            set_status(OBD_BLE_STATE_DISCONNECTED, "Disconnected");
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t buf[128];
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if(len > sizeof(buf)) len = sizeof(buf);
        if(os_mbuf_copydata(event->notify_rx.om, 0, len, buf) == 0) {
            ESP_LOGD(TAG, "rx %s len=%u attr=0x%04X",
                     event->notify_rx.indication ? "indicate" : "notify",
                     len, event->notify_rx.attr_handle);

            if(s_rx_accum_len + len >= sizeof(s_rx_accum)) {
                ESP_LOGW(TAG, "rx accumulation overflow, reset");
                s_rx_accum_len = 0;
            }
            size_t room = sizeof(s_rx_accum) - 1 - s_rx_accum_len;
            size_t copy_len = len < room ? len : room;
            memcpy(s_rx_accum + s_rx_accum_len, buf, copy_len);
            s_rx_accum_len += copy_len;
            s_rx_accum[s_rx_accum_len] = '\0';

            if(memchr(buf, '>', len) != NULL || memchr(s_rx_accum, '>', s_rx_accum_len) != NULL) {
                s_elm_ready = true;
                if(response_has_error(s_rx_accum)) {
                    ESP_LOGW(TAG, "ELM error response: %.160s", s_rx_accum);
                } else {
                    ESP_LOGD(TAG, "ELM full response: %.160s", s_rx_accum);
                }
                parse_obd_response((const uint8_t *)s_rx_accum, s_rx_accum_len);
                s_rx_accum_len = 0;
                s_rx_accum[0] = '\0';
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if(s_scan_restart_pending) {
            s_scan_restart_pending = false;
            start_scan();
        }
        return 0;

    default:
        return 0;
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {0};
    params.filter_duplicates = 1;
    params.passive = 0;

    if(ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        set_status(OBD_BLE_STATE_ERROR, "BLE addr error");
        return;
    }

    set_status(OBD_BLE_STATE_SCANNING, s_scan_target_only ? "Scanning saved" : "Select Bluetooth");
    ESP_LOGI(TAG, "scan start mode=%s target=%s",
             s_scan_target_only ? "saved-target" : "all-named",
             s_target_name[0] ? s_target_name : "-");
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if(rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "scan start failed rc=%d", rc);
        set_status(OBD_BLE_STATE_ERROR, "Scan error");
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset reason=%d", reason);
    set_status(OBD_BLE_STATE_ERROR, "BLE reset");
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    s_host_synced = true;
    if(s_snapshot.saved_name[0] != '\0') {
        strlcpy(s_target_name, s_snapshot.saved_name, sizeof(s_target_name));
        s_scan_target_only = true;
        ESP_LOGI(TAG, "BLE host sync, auto target saved name: %s", s_target_name);
    } else {
        ESP_LOGI(TAG, "BLE host sync, no saved target");
    }
    start_scan();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void obd_ble_start(void)
{
    if(s_started) return;
    s_started = true;

    if(!s_snapshot_lock) {
        s_snapshot_lock = xSemaphoreCreateMutex();
        assert(s_snapshot_lock);
    }

    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    load_saved_name();
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_store_config_init();
    ble_svc_gap_device_name_set("esp32s3-obd-hud");

    nimble_port_freertos_init(host_task);
}

void obd_ble_scan_all(void)
{
    ESP_LOGI(TAG, "user requested scan all");
    if(s_snapshot_lock) xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    s_devices.count = 0;
    s_target_name[0] = '\0';
    s_scan_target_only = false;
    if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);

    restart_scan();
}

void obd_ble_connect_by_name(const char *name)
{
    if(!name || name[0] == '\0') return;
    ESP_LOGI(TAG, "user selected bluetooth device: %s", name);

    if(s_snapshot_lock) xSemaphoreTake(s_snapshot_lock, portMAX_DELAY);
    strlcpy(s_target_name, name, sizeof(s_target_name));
    s_scan_target_only = true;
    if(s_snapshot_lock) xSemaphoreGive(s_snapshot_lock);

    restart_scan();
}
