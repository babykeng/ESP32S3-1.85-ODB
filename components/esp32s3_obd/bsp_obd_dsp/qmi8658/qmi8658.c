#include "bsp_obd_dsp/qmi8658/qmi8658.h"

#include <string.h>
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp_obd_dsp/i2c_driver/I2C_Driver.h"

#define QMI8658_ADDR_LOW      0x6A
#define QMI8658_ADDR_HIGH     0x6B

#define QMI8658_WHO_AM_I      0x00
#define QMI8658_CTRL1         0x02
#define QMI8658_CTRL2         0x03
#define QMI8658_CTRL3         0x04
#define QMI8658_CTRL7         0x08
#define QMI8658_AX_L          0x35

#define QMI8658_CTRL1_AUTO_INC    0x60
#define QMI8658_CTRL2_ACC_8G_1K   0x23
#define QMI8658_CTRL3_GYR_512_1K  0x53
#define QMI8658_CTRL7_ACC_GYR_EN  0x03

static const char *TAG = "qmi8658";
static i2c_port_t s_port = I2C_NUM_0;
static uint8_t s_addr = 0;
static uint8_t s_who_am_i = 0;
static bool s_available = false;
static bool s_calibrated = false;
static float s_acc_offset_g[3] = {0};
static float s_gyro_offset_dps[3] = {0};

static esp_err_t read_reg_on(i2c_port_t port, uint8_t addr, uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(port, addr, &reg, 1, data, len,
                                        I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}

static esp_err_t write_reg_on(i2c_port_t port, uint8_t addr, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_write_to_device(port, addr, buf, sizeof(buf),
                                      I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    return write_reg_on(s_port, s_addr, reg, value);
}

static int16_t le16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static esp_err_t read_uncalibrated(qmi8658_data_t *data)
{
    if(!data) return ESP_ERR_INVALID_ARG;
    if(!s_available || s_addr == 0) return ESP_ERR_INVALID_STATE;

    uint8_t buf[12] = {0};
    esp_err_t err = read_reg_on(s_port, s_addr, QMI8658_AX_L, buf, sizeof(buf));
    if(err != ESP_OK) {
        s_available = false;
        return err;
    }

    memset(data, 0, sizeof(*data));
    data->acc_raw[0] = le16(&buf[0]);
    data->acc_raw[1] = le16(&buf[2]);
    data->acc_raw[2] = le16(&buf[4]);
    data->gyro_raw[0] = le16(&buf[6]);
    data->gyro_raw[1] = le16(&buf[8]);
    data->gyro_raw[2] = le16(&buf[10]);

    for(size_t i = 0; i < 3; i++) {
        data->acc_g[i] = (float)data->acc_raw[i] / 4096.0f;
        data->gyro_dps[i] = (float)data->gyro_raw[i] / 64.0f;
    }

    return ESP_OK;
}

esp_err_t qmi8658_init(void)
{
    const i2c_port_t ports[] = { I2C_NUM_0, I2C_NUM_1 };
    const uint8_t addrs[] = { QMI8658_ADDR_HIGH, QMI8658_ADDR_LOW };
    uint8_t who = 0;

    s_available = false;
    s_port = I2C_NUM_0;
    s_addr = 0;
    s_who_am_i = 0;
    qmi8658_clear_calibration();

    for(size_t p = 0; p < sizeof(ports) / sizeof(ports[0]); p++) {
        for(size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
            esp_err_t err = read_reg_on(ports[p], addrs[i], QMI8658_WHO_AM_I, &who, 1);
            ESP_LOGI(TAG, "probe port=%d addr=0x%02X err=%s who=0x%02X",
                     ports[p], addrs[i], esp_err_to_name(err), who);
            if(err == ESP_OK && who != 0x00 && who != 0xFF) {
                s_port = ports[p];
                s_addr = addrs[i];
                s_who_am_i = who;
                break;
            }
        }
        if(s_addr != 0) break;
    }

    if(s_addr == 0) {
        ESP_LOGW(TAG, "QMI8658 not found on I2C0/I2C1 addr 0x6A/0x6B");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = write_reg(QMI8658_CTRL1, QMI8658_CTRL1_AUTO_INC);
    if(err == ESP_OK) err = write_reg(QMI8658_CTRL2, QMI8658_CTRL2_ACC_8G_1K);
    if(err == ESP_OK) err = write_reg(QMI8658_CTRL3, QMI8658_CTRL3_GYR_512_1K);
    if(err == ESP_OK) err = write_reg(QMI8658_CTRL7, QMI8658_CTRL7_ACC_GYR_EN);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 config failed: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    s_available = true;
    ESP_LOGI(TAG, "QMI8658 ready, port=%d addr=0x%02X who=0x%02X", s_port, s_addr, s_who_am_i);
    return ESP_OK;
}

bool qmi8658_is_available(void)
{
    return s_available;
}

qmi8658_status_t qmi8658_get_status(void)
{
    qmi8658_status_t status = {
        .i2c_port = (uint8_t)s_port,
        .addr = s_addr,
        .who_am_i = s_who_am_i,
    };
    return status;
}

esp_err_t qmi8658_calibrate_current(void)
{
    qmi8658_data_t sample;
    float acc_sum[3] = {0};
    float gyro_sum[3] = {0};
    const uint8_t sample_count = 12;

    for(uint8_t n = 0; n < sample_count; n++) {
        esp_err_t err = read_uncalibrated(&sample);
        if(err != ESP_OK) return err;
        for(size_t i = 0; i < 3; i++) {
            acc_sum[i] += sample.acc_g[i];
            gyro_sum[i] += sample.gyro_dps[i];
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    for(size_t i = 0; i < 3; i++) {
        s_acc_offset_g[i] = acc_sum[i] / sample_count;
        s_gyro_offset_dps[i] = gyro_sum[i] / sample_count;
    }
    s_calibrated = true;
    ESP_LOGI(TAG, "calibrated acc=(%.3f,%.3f,%.3f) gyro=(%.3f,%.3f,%.3f)",
             s_acc_offset_g[0], s_acc_offset_g[1], s_acc_offset_g[2],
             s_gyro_offset_dps[0], s_gyro_offset_dps[1], s_gyro_offset_dps[2]);
    return ESP_OK;
}

void qmi8658_clear_calibration(void)
{
    memset(s_acc_offset_g, 0, sizeof(s_acc_offset_g));
    memset(s_gyro_offset_dps, 0, sizeof(s_gyro_offset_dps));
    s_calibrated = false;
}

esp_err_t qmi8658_read(qmi8658_data_t *data)
{
    esp_err_t err = read_uncalibrated(data);
    if(err != ESP_OK) return err;

    if(!s_calibrated) return ESP_OK;
    for(size_t i = 0; i < 3; i++) {
        data->acc_g[i] -= s_acc_offset_g[i];
        data->gyro_dps[i] -= s_gyro_offset_dps[i];
    }
    return ESP_OK;
}
