#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lvgl.h"

#include "bsp_obd_dsp/exio/TCA9554PWR.h"
#include "bsp_obd_dsp/i2c_driver/I2C_Driver.h"
#include "bsp_obd_dsp/lcd_driver/ST77916.h"
#include "bsp_obd_dsp/pcf85063/pcf85063.h"
#include "bsp_obd_dsp/qmi8658/qmi8658.h"
#include "bsp_obd_dsp/touch_driver/CST816.h"
#include "gps_uart.h"
#include "newfeatures.h"
#include "obd_ble.h"
#include "ui.h"

static const char *TAG = "logo_app";

#define LCD_H_RES               EXAMPLE_LCD_WIDTH
#define LCD_V_RES               EXAMPLE_LCD_HEIGHT
#define LVGL_BUFF_SIZE          (LCD_H_RES * 20)
#define LVGL_TICK_PERIOD_MS     2
#define LVGL_TASK_MAX_DELAY_MS  500
#define LVGL_TASK_MIN_DELAY_MS  2
#define LVGL_TASK_STACK_SIZE    (10 * 1024)
#define LVGL_TASK_PRIORITY      2

#ifndef NEWFEATURES_WIFI_SSID
#define NEWFEATURES_WIFI_SSID   ""
#endif

#ifndef NEWFEATURES_WIFI_PASS
#define NEWFEATURES_WIFI_PASS   ""
#endif

static SemaphoreHandle_t s_lvgl_mux;

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    lv_disp_flush_ready((lv_disp_drv_t *)user_ctx);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

static void lvgl_rounder_cb(lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    (void)disp_drv;
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void lvgl_monitor_cb(lv_disp_drv_t *disp_drv, uint32_t time_ms, uint32_t px)
{
    (void)disp_drv;
    if(time_ms > 1000) {
        ESP_LOGW(TAG, "slow LVGL refresh: %lums %lu px", time_ms, px);
    }
}

static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t touch = (esp_lcd_touch_handle_t)drv->user_data;
    uint8_t tp_cnt = 0;
    esp_lcd_touch_point_data_t tp_data = {0};

    if(touch == NULL) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_read_data(touch);
    if(esp_lcd_touch_get_data(touch, &tp_data, &tp_cnt, 1) == ESP_OK && tp_cnt > 0) {
        data->point.x = tp_data.x;
        data->point.y = tp_data.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void increase_lvgl_tick(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static bool lvgl_lock(int timeout_ms)
{
    assert(s_lvgl_mux);
    TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lvgl_mux, timeout_ticks) == pdTRUE;
}

static void lvgl_unlock(void)
{
    assert(s_lvgl_mux);
    xSemaphoreGive(s_lvgl_mux);
}

static void lvgl_port_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "LVGL task started");
    uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;

    while(1) {
        if(lvgl_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            lvgl_unlock();
        }
        if(task_delay_ms > LVGL_TASK_MAX_DELAY_MS) task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
        if(task_delay_ms < LVGL_TASK_MIN_DELAY_MS) task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs reset: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;
    static lv_indev_drv_t indev_drv;

    ESP_LOGI(TAG, "Starting boot logo firmware");

    init_nvs();
    setenv("TZ", "CST-8", 1);
    tzset();

    I2C_Init();

    esp_err_t rtc_err = pcf85063_init();
    if(rtc_err == ESP_OK) {
        rtc_err = pcf85063_set_system_time_from_rtc();
    }
    if(rtc_err != ESP_OK) {
        ESP_LOGW(TAG, "RTC unavailable: %s", esp_err_to_name(rtc_err));
    }

    ESP_ERROR_CHECK(EXIO_Init());

    LCD_SetFlushCallback(notify_lvgl_flush_ready, &disp_drv);
    LCD_Backlight = 70;
    LCD_Init();
    ESP_ERROR_CHECK(panel_handle == NULL ? ESP_FAIL : ESP_OK);
    Touch_Init();

    esp_err_t imu_err = qmi8658_init();
    if(imu_err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 unavailable: %s", esp_err_to_name(imu_err));
    }

    if(NEWFEATURES_WIFI_SSID[0] != '\0') {
        newfeatures_start_wifi_ntp(NEWFEATURES_WIFI_SSID, NEWFEATURES_WIFI_PASS);
    }

    esp_err_t gps_uart_err = newfeatures_gps_uart_start();
    if(gps_uart_err != ESP_OK) {
        ESP_LOGW(TAG, "GPS UART receiver unavailable: %s", esp_err_to_name(gps_uart_err));
    }

    lv_init();

    lv_color_t *buf1 = heap_caps_malloc(LVGL_BUFF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_color_t *buf2 = heap_caps_malloc(LVGL_BUFF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1 && buf2);
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, LVGL_BUFF_SIZE);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_H_RES;
    disp_drv.ver_res = LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.rounder_cb = lvgl_rounder_cb;
    disp_drv.monitor_cb = lvgl_monitor_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    const esp_timer_create_args_t tick_args = {
        .callback = increase_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = lvgl_touch_cb;
    indev_drv.user_data = tp;
    lv_indev_drv_register(&indev_drv);

    s_lvgl_mux = xSemaphoreCreateMutex();
    assert(s_lvgl_mux);
    xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    if(lvgl_lock(-1)) {
        boot_logo_ui_init();
        lvgl_unlock();
    }

    obd_ble_start();
}
