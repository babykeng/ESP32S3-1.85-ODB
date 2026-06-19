#include "bsp_obd_dsp/pcf85063/pcf85063.h"

#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "bsp_obd_dsp/i2c_driver/I2C_Driver.h"

#define PCF85063_ADDR          0x51
#define PCF85063_CTRL1         0x00
#define PCF85063_SECONDS       0x04
#define PCF85063_CTRL1_STOP    0x20
#define PCF85063_SECONDS_OS    0x80

static const char *TAG = "pcf85063";
static bool s_available = false;

static uint8_t bcd_to_dec(uint8_t value)
{
    return ((value >> 4) * 10) + (value & 0x0F);
}

static uint8_t dec_to_bcd(uint8_t value)
{
    return ((value / 10) << 4) | (value % 10);
}

static bool rtc_time_valid(const struct tm *timeinfo)
{
    if(!timeinfo) return false;
    if(timeinfo->tm_year < (2024 - 1900) || timeinfo->tm_year > (2099 - 1900)) return false;
    if(timeinfo->tm_mon < 0 || timeinfo->tm_mon > 11) return false;
    if(timeinfo->tm_mday < 1 || timeinfo->tm_mday > 31) return false;
    if(timeinfo->tm_hour < 0 || timeinfo->tm_hour > 23) return false;
    if(timeinfo->tm_min < 0 || timeinfo->tm_min > 59) return false;
    if(timeinfo->tm_sec < 0 || timeinfo->tm_sec > 59) return false;
    return true;
}

esp_err_t pcf85063_init(void)
{
    uint8_t ctrl1 = 0;
    esp_err_t err = I2C_Read(PCF85063_ADDR, PCF85063_CTRL1, &ctrl1, 1);
    if(err != ESP_OK) {
        s_available = false;
        ESP_LOGW(TAG, "PCF85063 not found at 0x%02X: %s", PCF85063_ADDR, esp_err_to_name(err));
        return err;
    }

    ctrl1 &= ~PCF85063_CTRL1_STOP;
    err = I2C_Write(PCF85063_ADDR, PCF85063_CTRL1, &ctrl1, 1);
    if(err != ESP_OK) {
        s_available = false;
        ESP_LOGW(TAG, "PCF85063 start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_available = true;
    ESP_LOGI(TAG, "PCF85063 ready at 0x%02X", PCF85063_ADDR);
    return ESP_OK;
}

bool pcf85063_is_available(void)
{
    return s_available;
}

esp_err_t pcf85063_read_time(struct tm *timeinfo)
{
    if(!timeinfo) return ESP_ERR_INVALID_ARG;
    if(!s_available) return ESP_ERR_INVALID_STATE;

    uint8_t buf[7] = {0};
    esp_err_t err = I2C_Read(PCF85063_ADDR, PCF85063_SECONDS, buf, sizeof(buf));
    if(err != ESP_OK) {
        s_available = false;
        return err;
    }

    if(buf[0] & PCF85063_SECONDS_OS) {
        ESP_LOGW(TAG, "RTC oscillator stop flag set, time is invalid");
        return ESP_ERR_INVALID_STATE;
    }

    memset(timeinfo, 0, sizeof(*timeinfo));
    timeinfo->tm_sec = bcd_to_dec(buf[0] & 0x7F);
    timeinfo->tm_min = bcd_to_dec(buf[1] & 0x7F);
    timeinfo->tm_hour = bcd_to_dec(buf[2] & 0x3F);
    timeinfo->tm_mday = bcd_to_dec(buf[3] & 0x3F);
    timeinfo->tm_wday = bcd_to_dec(buf[4] & 0x07);
    timeinfo->tm_mon = bcd_to_dec(buf[5] & 0x1F) - 1;
    timeinfo->tm_year = bcd_to_dec(buf[6]) + 100;
    timeinfo->tm_isdst = -1;

    if(!rtc_time_valid(timeinfo)) {
        ESP_LOGW(TAG, "RTC time is out of range");
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

esp_err_t pcf85063_write_time(const struct tm *timeinfo)
{
    if(!timeinfo) return ESP_ERR_INVALID_ARG;
    if(!s_available) return ESP_ERR_INVALID_STATE;
    if(!rtc_time_valid(timeinfo)) return ESP_ERR_INVALID_ARG;

    uint8_t buf[7] = {
        dec_to_bcd((uint8_t)timeinfo->tm_sec),
        dec_to_bcd((uint8_t)timeinfo->tm_min),
        dec_to_bcd((uint8_t)timeinfo->tm_hour),
        dec_to_bcd((uint8_t)timeinfo->tm_mday),
        dec_to_bcd((uint8_t)timeinfo->tm_wday),
        dec_to_bcd((uint8_t)(timeinfo->tm_mon + 1)),
        dec_to_bcd((uint8_t)(timeinfo->tm_year - 100)),
    };

    esp_err_t err = I2C_Write(PCF85063_ADDR, PCF85063_SECONDS, buf, sizeof(buf));
    if(err != ESP_OK) {
        s_available = false;
        return err;
    }
    return ESP_OK;
}

esp_err_t pcf85063_set_system_time_from_rtc(void)
{
    struct tm timeinfo = {0};
    esp_err_t err = pcf85063_read_time(&timeinfo);
    if(err != ESP_OK) return err;

    time_t t = mktime(&timeinfo);
    if(t < 0) return ESP_ERR_INVALID_RESPONSE;

    struct timeval tv = {
        .tv_sec = t,
        .tv_usec = 0,
    };
    err = settimeofday(&tv, NULL);
    if(err != ESP_OK) return err;

    ESP_LOGI(TAG, "system time restored from RTC: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return ESP_OK;
}

esp_err_t pcf85063_write_system_time(void)
{
    if(!s_available) return ESP_ERR_INVALID_STATE;

    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);

    if(!rtc_time_valid(&timeinfo)) return ESP_ERR_INVALID_STATE;

    esp_err_t err = pcf85063_write_time(&timeinfo);
    if(err == ESP_OK) {
        ESP_LOGI(TAG, "RTC updated from system time: %04d-%02d-%02d %02d:%02d:%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }
    return err;
}

