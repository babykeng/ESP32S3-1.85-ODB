#include "newfeatures.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "lwip/apps/sntp.h"
#include "nvs_flash.h"

#include "bsp_obd_dsp/i2c_driver/I2C_Driver.h"
#include "bsp_obd_dsp/pcf85063/pcf85063.h"
#include "bsp_obd_dsp/qmi8658/qmi8658.h"

#define WIFI_CONNECTED_BIT BIT0
#define RTC_UPDATE_THRESHOLD_SEC 60

static const char *TAG = "newfeatures";
static EventGroupHandle_t s_wifi_events;
static char s_wifi_ssid[33];
static char s_wifi_pass[65];

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if(s_wifi_events) xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting");
        esp_wifi_connect();
    } else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if(s_wifi_events) xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void update_rtc_if_needed(time_t now)
{
    struct tm rtc_timeinfo = {0};
    esp_err_t err = pcf85063_read_time(&rtc_timeinfo);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "RTC read failed after NTP sync, updating RTC: %s", esp_err_to_name(err));
        err = pcf85063_write_system_time();
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "RTC update failed: %s", esp_err_to_name(err));
        }
        return;
    }

    time_t rtc_time = mktime(&rtc_timeinfo);
    if(rtc_time < 0) {
        ESP_LOGW(TAG, "RTC time conversion failed, updating RTC");
        err = pcf85063_write_system_time();
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "RTC update failed: %s", esp_err_to_name(err));
        }
        return;
    }

    time_t diff_sec = now - rtc_time;
    if(diff_sec < 0) diff_sec = -diff_sec;

    if(diff_sec > RTC_UPDATE_THRESHOLD_SEC) {
        ESP_LOGI(TAG, "RTC differs from NTP by %lld seconds, updating RTC", (long long)diff_sec);
        err = pcf85063_write_system_time();
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "RTC update failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "RTC time is close to NTP, skip update (%lld seconds)", (long long)diff_sec);
    }
}

static void wait_for_ntp_time(void)
{
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    for(uint8_t retry = 0; retry < 30; retry++) {
        time_t now = 0;
        struct tm timeinfo = {0};
        time(&now);
        localtime_r(&now, &timeinfo);
        if(timeinfo.tm_year >= (2020 - 1900)) {
            ESP_LOGI(TAG, "NTP synced: %04d-%02d-%02d %02d:%02d:%02d",
                     timeinfo.tm_year + 1900,
                     timeinfo.tm_mon + 1,
                     timeinfo.tm_mday,
                     timeinfo.tm_hour,
                     timeinfo.tm_min,
                     timeinfo.tm_sec);
            update_rtc_if_needed(now);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGW(TAG, "NTP sync timed out");
}

static void wifi_time_task(void *arg)
{
    (void)arg;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, s_wifi_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, s_wifi_pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", s_wifi_ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if(bits & WIFI_CONNECTED_BIT) {
        wait_for_ntp_time();
    } else {
        ESP_LOGW(TAG, "WiFi connect timed out");
    }

    vTaskDelete(NULL);
}

void newfeatures_start_wifi_ntp(const char *ssid, const char *pass)
{
    if(!ssid || ssid[0] == '\0') return;

    strncpy(s_wifi_ssid, ssid, sizeof(s_wifi_ssid) - 1);
    strncpy(s_wifi_pass, pass ? pass : "", sizeof(s_wifi_pass) - 1);

    if(!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        assert(s_wifi_events);
    }
    xTaskCreate(wifi_time_task, "nf_wifi_time", 6144, NULL, 4, NULL);
}

esp_err_t newfeatures_env_init(const newfeatures_env_config_t *config)
{
    esp_err_t nvs_err = nvs_flash_init();
    if(nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed: %s", esp_err_to_name(nvs_err));
    }

    const char *timezone = (config && config->timezone) ? config->timezone : "CST-8";
    setenv("TZ", timezone, 1);
    tzset();

    I2C_Init();

    esp_err_t rtc_err = pcf85063_init();
    if(rtc_err == ESP_OK) {
        rtc_err = pcf85063_set_system_time_from_rtc();
    }
    if(rtc_err != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 unavailable or invalid: %s", esp_err_to_name(rtc_err));
    }

    esp_err_t imu_err = qmi8658_init();
    if(imu_err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 init failed: %s", esp_err_to_name(imu_err));
    }

    if(config && config->wifi_ssid && config->wifi_ssid[0] != '\0') {
        newfeatures_start_wifi_ntp(config->wifi_ssid, config->wifi_pass);
    }

    return ESP_OK;
}
