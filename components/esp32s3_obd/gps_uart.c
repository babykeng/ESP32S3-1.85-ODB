#include "gps_uart.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "bsp_obd_dsp/pcf85063/pcf85063.h"

static const char *TAG = "gps_uart";

#define GPS_UART_BUF_SIZE 1024
#define GPS_UART_TASK_STACK_SIZE 3072
#define GPS_UART_TASK_PRIORITY 8
#define GPS_RTC_UPDATE_THRESHOLD_SEC 60

typedef struct {
    uart_port_t port;
    gpio_num_t tx_gpio;
    gpio_num_t rx_gpio;
    bool started;
} gps_uart_port_config_t;

typedef struct {
    bool nmea_active;
    size_t nmea_len;
    char nmea[128];
    uint8_t ubx_state;
    uint8_t ubx_class;
    uint8_t ubx_id;
    uint16_t ubx_len;
    uint16_t ubx_idx;
    uint8_t ubx_ck_a;
    uint8_t ubx_ck_b;
    uint8_t ubx_payload[128];
    bool ubx_overflow;
} gps_decoder_t;

static gps_uart_port_config_t s_ports[] = {
    { UART_NUM_0, NEWFEATURES_GPS_UART0_TX_GPIO, NEWFEATURES_GPS_UART0_RX_GPIO, false },
    { UART_NUM_1, NEWFEATURES_GPS_UART1_TX_GPIO, NEWFEATURES_GPS_UART1_RX_GPIO, false },
    { UART_NUM_2, NEWFEATURES_GPS_UART2_TX_GPIO, NEWFEATURES_GPS_UART2_RX_GPIO, false },
};
static gps_decoder_t s_decoders[3];

static SemaphoreHandle_t s_lock;
static bool s_started;
static bool s_rtc_synced_from_gps;
static newfeatures_gps_uart_snapshot_t s_snapshot = {
    .active_port = UART_NUM_MAX,
    .current_baud = NEWFEATURES_GPS_UART_BAUD_RATE,
};

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static int hex_value(char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static double nmea_coord_to_deg(const char *value, const char *hemisphere)
{
    if(value == NULL || value[0] == '\0') return 0.0;

    double raw = atof(value);
    int deg = (int)(raw / 100.0);
    double min = raw - (double)deg * 100.0;
    double dec = (double)deg + min / 60.0;

    if(hemisphere != NULL && (hemisphere[0] == 'S' || hemisphere[0] == 'W')) {
        dec = -dec;
    }
    return dec;
}

static bool field_has_value(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static bool is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_before_month(int year, int month)
{
    static const uint16_t days_before[] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    int days = days_before[month - 1];
    if(month > 2 && is_leap_year(year)) days++;
    return days;
}

static time_t gps_utc_to_unix(int year, int month, int day, int hour, int min, int sec)
{
    int64_t days = 0;
    for(int y = 1970; y < year; y++) {
        days += is_leap_year(y) ? 366 : 365;
    }
    days += days_before_month(year, month);
    days += day - 1;
    return (time_t)(days * 86400 + hour * 3600 + min * 60 + sec);
}

static bool gps_datetime_valid(int year, int month, int day, int hour, int min, int sec)
{
    static const uint8_t mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if(year < 2024 || year > 2099) return false;
    if(month < 1 || month > 12) return false;
    uint8_t max_day = mdays[month - 1];
    if(month == 2 && is_leap_year(year)) max_day = 29;
    if(day < 1 || day > max_day) return false;
    if(hour < 0 || hour > 23) return false;
    if(min < 0 || min > 59) return false;
    if(sec < 0 || sec > 59) return false;
    return true;
}

static bool parse_nmea_time(const char *hhmmss, int *hour, int *min, int *sec)
{
    if(hhmmss == NULL || strlen(hhmmss) < 6) return false;
    for(uint8_t i = 0; i < 6; i++) {
        if(!isdigit((unsigned char)hhmmss[i])) return false;
    }
    *hour = (hhmmss[0] - '0') * 10 + (hhmmss[1] - '0');
    *min = (hhmmss[2] - '0') * 10 + (hhmmss[3] - '0');
    *sec = (hhmmss[4] - '0') * 10 + (hhmmss[5] - '0');
    return *hour <= 23 && *min <= 59 && *sec <= 59;
}

static bool parse_nmea_date(const char *ddmmyy, int *year, int *month, int *day)
{
    if(ddmmyy == NULL || strlen(ddmmyy) < 6) return false;
    for(uint8_t i = 0; i < 6; i++) {
        if(!isdigit((unsigned char)ddmmyy[i])) return false;
    }
    *day = (ddmmyy[0] - '0') * 10 + (ddmmyy[1] - '0');
    *month = (ddmmyy[2] - '0') * 10 + (ddmmyy[3] - '0');
    *year = 2000 + (ddmmyy[4] - '0') * 10 + (ddmmyy[5] - '0');
    return true;
}

static void sync_gps_time_locked(const char *source, int year, int month, int day,
                                 int hour, int min, int sec)
{
    if(s_rtc_synced_from_gps) return;
    if(!gps_datetime_valid(year, month, day, hour, min, sec)) return;

    time_t gps_time = gps_utc_to_unix(year, month, day, hour, min, sec);
    time_t now = 0;
    time(&now);
    int64_t sys_diff = (int64_t)gps_time - (int64_t)now;
    if(sys_diff < 0) sys_diff = -sys_diff;

    bool system_time_matches_gps = sys_diff <= 1;
    if(sys_diff > 1) {
        struct timeval tv = {
            .tv_sec = gps_time,
            .tv_usec = 0,
        };
        if(settimeofday(&tv, NULL) == 0) {
            system_time_matches_gps = true;
            ESP_LOGI(TAG, "system time synced from %s GPS: %04d-%02d-%02d %02d:%02d:%02d UTC",
                     source, year, month, day, hour, min, sec);
        } else {
            ESP_LOGW(TAG, "GPS system time sync failed");
        }
    }

    if(!system_time_matches_gps) return;

    bool update_rtc = true;
    struct tm rtc_timeinfo = {0};
    esp_err_t err = pcf85063_read_time(&rtc_timeinfo);
    if(err == ESP_OK) {
        time_t rtc_time = mktime(&rtc_timeinfo);
        if(rtc_time >= 0) {
            int64_t rtc_diff = (int64_t)gps_time - (int64_t)rtc_time;
            if(rtc_diff < 0) rtc_diff = -rtc_diff;
            update_rtc = rtc_diff > GPS_RTC_UPDATE_THRESHOLD_SEC;
        }
    }

    if(update_rtc) {
        err = pcf85063_write_system_time();
        if(err == ESP_OK) {
            s_rtc_synced_from_gps = true;
            ESP_LOGI(TAG, "RTC updated from %s GPS time", source);
        } else {
            ESP_LOGW(TAG, "RTC update from %s GPS failed: %s", source, esp_err_to_name(err));
        }
    } else {
        s_rtc_synced_from_gps = true;
        ESP_LOGI(TAG, "RTC already close to %s GPS time, GPS sync complete", source);
    }
}

static bool nmea_checksum_ok(const char *line)
{
    if(line[0] != '$') return false;

    const char *star = strchr(line, '*');
    if(star == NULL || star[1] == '\0' || star[2] == '\0') return true;

    uint8_t sum = 0;
    for(const char *p = line + 1; p < star; p++) {
        sum ^= (uint8_t)*p;
    }

    int hi = hex_value(star[1]);
    int lo = hex_value(star[2]);
    if(hi < 0 || lo < 0) return false;
    return sum == (uint8_t)((hi << 4) | lo);
}

static void set_utc_time_locked(const char *hhmmss)
{
    if(hhmmss == NULL || strlen(hhmmss) < 6) return;
    snprintf(s_snapshot.utc_time, sizeof(s_snapshot.utc_time), "%.2s:%.2s:%.2s",
             hhmmss, hhmmss + 2, hhmmss + 4);

    int hour = ((hhmmss[0] - '0') * 10 + (hhmmss[1] - '0') + 8) % 24;
    snprintf(s_snapshot.local_time, sizeof(s_snapshot.local_time), "%02d:%.2s:%.2s",
             hour, hhmmss + 2, hhmmss + 4);
}

static void set_decoded_locked(const char *protocol, const char *text, bool valid)
{
    bool was_ok = s_snapshot.decoded_ok;
    strlcpy(s_snapshot.protocol, protocol, sizeof(s_snapshot.protocol));
    strlcpy(s_snapshot.decoded, text, sizeof(s_snapshot.decoded));
    s_snapshot.decoded_ok = valid;
    if(valid && !was_ok) {
        ESP_LOGI(TAG, "%s decoded: %s", protocol, text);
    } else {
        ESP_LOGD(TAG, "%s decoded: %s", protocol, text);
    }
}

static void parse_nmea_locked(uart_port_t port, const char *line)
{
    if(!nmea_checksum_ok(line)) {
        if(s_snapshot.decoded_ok) return;
        char decoded[96];
        snprintf(decoded, sizeof(decoded), "NMEA checksum error\n%s", line);
        set_decoded_locked("ERR", decoded, false);
        return;
    }

    char copy[128];
    strlcpy(copy, line[0] == '$' ? line + 1 : line, sizeof(copy));
    char *star = strchr(copy, '*');
    if(star != NULL) *star = '\0';

    char *fields[24] = {0};
    size_t count = 0;
    char *p = copy;
    while(count < sizeof(fields) / sizeof(fields[0])) {
        fields[count++] = p;
        char *comma = strchr(p, ',');
        if(comma == NULL) break;
        *comma = '\0';
        p = comma + 1;
    }
    if(count == 0 || fields[0] == NULL) return;

    const char *type = fields[0];
    size_t type_len = strlen(type);
    if(type_len >= 3) type += type_len - 3;

    char decoded[256];
    if(strcmp(type, "GGA") == 0 && count >= 10) {
        uint8_t fix_type = (uint8_t)atoi(fields[6]);
        uint8_t sats = (uint8_t)atoi(fields[7]);
        s_snapshot.fix_type = fix_type;
        s_snapshot.fix_valid = fix_type > 0;
        s_snapshot.satellites = sats;
        double lat = s_snapshot.latitude;
        double lon = s_snapshot.longitude;
        if(field_has_value(fields[2]) && field_has_value(fields[4])) {
            lat = nmea_coord_to_deg(fields[2], fields[3]);
            lon = nmea_coord_to_deg(fields[4], fields[5]);
            s_snapshot.latitude = lat;
            s_snapshot.longitude = lon;
            s_snapshot.has_position = true;
        }
        set_utc_time_locked(fields[1]);
        snprintf(decoded, sizeof(decoded),
                 "NMEA GGA U%d\nfix:%s sats:%s hdop:%s\nlat:%.6f\nlon:%.6f\nalt:%sm",
                 (int)port, fields[6], fields[7], fields[8], lat, lon, fields[9]);
        set_decoded_locked("NMEA", decoded, true);
    } else if(strcmp(type, "RMC") == 0 && count >= 10) {
        double lat = s_snapshot.latitude;
        double lon = s_snapshot.longitude;
        bool status_valid = fields[2] != NULL && fields[2][0] == 'A';
        s_snapshot.fix_valid = status_valid;
        if(field_has_value(fields[3]) && field_has_value(fields[5])) {
            lat = nmea_coord_to_deg(fields[3], fields[4]);
            lon = nmea_coord_to_deg(fields[5], fields[6]);
            s_snapshot.latitude = lat;
            s_snapshot.longitude = lon;
            s_snapshot.has_position = true;
        }
        if(field_has_value(fields[7])) {
            s_snapshot.speed_kmh = atof(fields[7]) * 1.852;
            s_snapshot.has_speed = true;
        }
        set_utc_time_locked(fields[1]);
        int year;
        int month;
        int day;
        int hour;
        int minute;
        int sec;
        if(parse_nmea_date(fields[9], &year, &month, &day) &&
           parse_nmea_time(fields[1], &hour, &minute, &sec)) {
            sync_gps_time_locked("NMEA RMC", year, month, day, hour, minute, sec);
        }
        snprintf(decoded, sizeof(decoded),
                 "NMEA RMC U%d\nstatus:%s date:%s time:%s\nlat:%.6f\nlon:%.6f\nspd:%skn crs:%s",
                 (int)port, fields[2], fields[9], fields[1], lat, lon, fields[7], fields[8]);
        set_decoded_locked("NMEA", decoded, true);
    } else if(strcmp(type, "GLL") == 0 && count >= 7) {
        double lat = s_snapshot.latitude;
        double lon = s_snapshot.longitude;
        s_snapshot.fix_valid = fields[6] != NULL && fields[6][0] == 'A';
        if(field_has_value(fields[1]) && field_has_value(fields[3])) {
            lat = nmea_coord_to_deg(fields[1], fields[2]);
            lon = nmea_coord_to_deg(fields[3], fields[4]);
            s_snapshot.latitude = lat;
            s_snapshot.longitude = lon;
            s_snapshot.has_position = true;
        }
        set_utc_time_locked(fields[5]);
        snprintf(decoded, sizeof(decoded),
                 "NMEA GLL U%d\nstatus:%s time:%s\nlat:%.6f\nlon:%.6f",
                 (int)port, fields[6], fields[5], lat, lon);
        set_decoded_locked("NMEA", decoded, true);
    } else if(strcmp(type, "VTG") == 0 && count >= 8) {
        if(field_has_value(fields[7])) {
            s_snapshot.speed_kmh = atof(fields[7]);
            s_snapshot.has_speed = true;
        }
        snprintf(decoded, sizeof(decoded),
                 "NMEA VTG U%d\ncourse:%s deg\nspeed:%s km/h",
                 (int)port, fields[1], fields[7]);
        set_decoded_locked("NMEA", decoded, true);
    } else {
        if(s_snapshot.decoded_ok || s_snapshot.decoded[0] != '\0') return;
        snprintf(decoded, sizeof(decoded), "NMEA %s U%d\n%s", type, (int)port, line);
        set_decoded_locked("NMEA", decoded, true);
    }
}

static uint16_t get_u2(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_u4(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t get_i4(const uint8_t *p)
{
    return (int32_t)get_u4(p);
}

static void parse_ubx_locked(uart_port_t port, uint8_t msg_class, uint8_t msg_id,
                             const uint8_t *payload, uint16_t len)
{
    char decoded[256];

    if(msg_class == 0x01 && msg_id == 0x07 && len >= 92) {
        uint16_t year = get_u2(payload + 4);
        uint8_t month = payload[6];
        uint8_t day = payload[7];
        uint8_t hour = payload[8];
        uint8_t min = payload[9];
        uint8_t sec = payload[10];
        uint8_t fix_type = payload[20];
        uint8_t num_sv = payload[23];
        double lon = (double)get_i4(payload + 24) * 1e-7;
        double lat = (double)get_i4(payload + 28) * 1e-7;
        int32_t height_m = get_i4(payload + 36) / 1000;
        uint32_t hacc_m = get_u4(payload + 40) / 1000;
        uint32_t gspeed = get_u4(payload + 60);
        s_snapshot.fix_type = fix_type;
        s_snapshot.fix_valid = fix_type >= 2;
        s_snapshot.satellites = num_sv;
        s_snapshot.latitude = lat;
        s_snapshot.longitude = lon;
        s_snapshot.speed_kmh = (double)gspeed * 0.0036;
        s_snapshot.has_position = true;
        s_snapshot.has_speed = true;
        snprintf(s_snapshot.utc_time, sizeof(s_snapshot.utc_time), "%02u:%02u:%02u",
                 (unsigned)hour, (unsigned)min, (unsigned)sec);
        snprintf(s_snapshot.local_time, sizeof(s_snapshot.local_time), "%02u:%02u:%02u",
                 (unsigned)((hour + 8) % 24), (unsigned)min, (unsigned)sec);
        uint8_t valid = payload[11];
        if((valid & 0x03) == 0x03) {
            sync_gps_time_locked("UBX NAV-PVT", year, month, day, hour, min, sec);
        }
        snprintf(decoded, sizeof(decoded),
                 "UBX NAV-PVT U%d\n%04u-%02u-%02u %02u:%02u:%02u\nfix:%u sats:%u hAcc:%lum\nlat:%.7f\nlon:%.7f\nalt:%ldm spd:%lumm/s",
                 (int)port, (unsigned)year, (unsigned)month, (unsigned)day,
                 (unsigned)hour, (unsigned)min, (unsigned)sec,
                 (unsigned)fix_type, (unsigned)num_sv,
                 hacc_m, lat, lon, (long)height_m, gspeed);
        set_decoded_locked("UBX", decoded, true);
    } else if(msg_class == 0x01 && msg_id == 0x02 && len >= 28) {
        double lon = (double)get_i4(payload + 4) * 1e-7;
        double lat = (double)get_i4(payload + 8) * 1e-7;
        int32_t height_m = get_i4(payload + 16) / 1000;
        uint32_t hacc_m = get_u4(payload + 20) / 1000;
        s_snapshot.latitude = lat;
        s_snapshot.longitude = lon;
        s_snapshot.has_position = true;
        snprintf(decoded, sizeof(decoded),
                 "UBX NAV-POSLLH U%d\nlat:%.7f\nlon:%.7f\nalt:%ldm hAcc:%lum",
                 (int)port, lat, lon, (long)height_m, hacc_m);
        set_decoded_locked("UBX", decoded, true);
    } else if(msg_class == 0x01 && msg_id == 0x03 && len >= 16) {
        snprintf(decoded, sizeof(decoded),
                 "UBX NAV-STATUS U%d\nfix:%u flags:0x%02X",
                 (int)port, (unsigned)payload[4], (unsigned)payload[5]);
        s_snapshot.fix_type = payload[4];
        s_snapshot.fix_valid = payload[4] >= 2;
        set_decoded_locked("UBX", decoded, true);
    } else if(msg_class == 0x01 && msg_id == 0x12 && len >= 36) {
        int32_t speed = get_i4(payload + 20);
        int32_t heading = get_i4(payload + 24) / 100000;
        s_snapshot.speed_kmh = (double)speed * 0.0036;
        s_snapshot.has_speed = true;
        snprintf(decoded, sizeof(decoded),
                 "UBX NAV-VELNED U%d\nspeed:%ldmm/s\nheading:%lddeg",
                 (int)port, (long)speed, (long)heading);
        set_decoded_locked("UBX", decoded, true);
    } else {
        snprintf(decoded, sizeof(decoded), "UBX %02X %02X U%d\nlen:%u",
                 (unsigned)msg_class, (unsigned)msg_id, (int)port, (unsigned)len);
        set_decoded_locked("UBX", decoded, true);
    }
}

static void ubx_ck_add(gps_decoder_t *decoder, uint8_t byte)
{
    decoder->ubx_ck_a += byte;
    decoder->ubx_ck_b += decoder->ubx_ck_a;
}

static void decoder_feed_locked(uart_port_t port, uint8_t byte)
{
    if(port < UART_NUM_0 || port > UART_NUM_2) return;
    gps_decoder_t *decoder = &s_decoders[port];

    if(byte == '$') {
        decoder->nmea_active = true;
        decoder->nmea_len = 0;
        decoder->nmea[decoder->nmea_len++] = (char)byte;
    } else if(decoder->nmea_active) {
        if(decoder->nmea_len < sizeof(decoder->nmea) - 1) {
            decoder->nmea[decoder->nmea_len++] = (char)byte;
        }
        if(byte == '\n') {
            decoder->nmea[decoder->nmea_len] = '\0';
            parse_nmea_locked(port, decoder->nmea);
            decoder->nmea_active = false;
        } else if(decoder->nmea_len >= sizeof(decoder->nmea) - 1) {
            decoder->nmea_active = false;
        }
    }

    switch(decoder->ubx_state) {
    case 0:
        decoder->ubx_state = (byte == 0xB5) ? 1 : 0;
        break;
    case 1:
        decoder->ubx_state = (byte == 0x62) ? 2 : 0;
        decoder->ubx_ck_a = 0;
        decoder->ubx_ck_b = 0;
        decoder->ubx_idx = 0;
        decoder->ubx_overflow = false;
        break;
    case 2:
        decoder->ubx_class = byte;
        ubx_ck_add(decoder, byte);
        decoder->ubx_state = 3;
        break;
    case 3:
        decoder->ubx_id = byte;
        ubx_ck_add(decoder, byte);
        decoder->ubx_state = 4;
        break;
    case 4:
        decoder->ubx_len = byte;
        ubx_ck_add(decoder, byte);
        decoder->ubx_state = 5;
        break;
    case 5:
        decoder->ubx_len |= (uint16_t)byte << 8;
        ubx_ck_add(decoder, byte);
        decoder->ubx_state = decoder->ubx_len == 0 ? 7 : 6;
        if(decoder->ubx_len > sizeof(decoder->ubx_payload)) decoder->ubx_overflow = true;
        break;
    case 6:
        if(!decoder->ubx_overflow && decoder->ubx_idx < sizeof(decoder->ubx_payload)) {
            decoder->ubx_payload[decoder->ubx_idx] = byte;
        }
        decoder->ubx_idx++;
        ubx_ck_add(decoder, byte);
        if(decoder->ubx_idx >= decoder->ubx_len) decoder->ubx_state = 7;
        break;
    case 7:
        if(byte == decoder->ubx_ck_a) {
            decoder->ubx_state = 8;
        } else {
            decoder->ubx_state = 0;
        }
        break;
    case 8:
        if(byte == decoder->ubx_ck_b && !decoder->ubx_overflow) {
            parse_ubx_locked(port, decoder->ubx_class, decoder->ubx_id,
                             decoder->ubx_payload, decoder->ubx_len);
        }
        decoder->ubx_state = 0;
        break;
    default:
        decoder->ubx_state = 0;
        break;
    }
}

static void append_char_locked(char c)
{
    size_t len = strlen(s_snapshot.text);
    if(len >= sizeof(s_snapshot.text) - 1) {
        size_t keep = sizeof(s_snapshot.text) / 2;
        memmove(s_snapshot.text, s_snapshot.text + len - keep, keep + 1);
        len = keep;
    }

    s_snapshot.text[len] = c;
    s_snapshot.text[len + 1] = '\0';
}

static void append_hex_locked(uint8_t byte)
{
    static const char hex[] = "0123456789ABCDEF";
    append_char_locked('<');
    append_char_locked(hex[(byte >> 4) & 0x0F]);
    append_char_locked(hex[byte & 0x0F]);
    append_char_locked('>');
}

static void append_rx_locked(uart_port_t port, const uint8_t *data, size_t len)
{
    s_snapshot.active_port = port;
    s_snapshot.total_bytes += len;
    if(port >= UART_NUM_0 && port <= UART_NUM_2) {
        uint32_t old_port_bytes = s_snapshot.port_bytes[port];
        s_snapshot.port_bytes[port] += len;
        if(old_port_bytes == 0) {
            char preview[64];
            size_t pos = 0;
            size_t preview_len = len < 12 ? len : 12;
            for(size_t i = 0; i < preview_len && pos < sizeof(preview); i++) {
                pos += snprintf(preview + pos, sizeof(preview) - pos, "%02X ", data[i]);
            }
            ESP_LOGI(TAG, "UART%d first RX len=%u bytes=%s", port, (unsigned)len, preview);
        }
    }
    s_snapshot.last_rx_ms = now_ms();

    for(size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        decoder_feed_locked(port, byte);
        if(byte == '\r') {
            continue;
        }
        if(byte == '\n' || byte == '\t' || isprint(byte)) {
            append_char_locked((char)byte);
        } else {
            append_hex_locked(byte);
        }
    }
}

static void gps_uart_rx_task(void *arg)
{
    gps_uart_port_config_t *config = (gps_uart_port_config_t *)arg;
    uint8_t *data = heap_caps_malloc(256, MALLOC_CAP_8BIT);
    if(data == NULL) {
        ESP_LOGE(TAG, "No memory for UART%d RX buffer", config->port);
        vTaskDelete(NULL);
        return;
    }

    while(1) {
        int len = uart_read_bytes(config->port, data, 256, pdMS_TO_TICKS(200));
        if(len > 0) {
            if(xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                append_rx_locked(config->port, data, (size_t)len);
                xSemaphoreGive(s_lock);
            }
        } else if(xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            if(config->port >= UART_NUM_0 && config->port <= UART_NUM_2) {
                s_snapshot.timeout_count[config->port]++;
            }
            xSemaphoreGive(s_lock);
        }
    }
}

static esp_err_t start_port(gps_uart_port_config_t *config)
{
    uart_config_t uart_config = {
        .baud_rate = s_snapshot.current_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(config->port, &uart_config),
                        TAG, "config UART%d failed", config->port);
    ESP_RETURN_ON_ERROR(uart_set_pin(config->port, config->tx_gpio, config->rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "set UART%d pins failed", config->port);

    esp_err_t err = uart_driver_install(config->port, GPS_UART_BUF_SIZE, GPS_UART_BUF_SIZE,
                                        0, NULL, 0);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "install UART%d driver failed: %s", config->port, esp_err_to_name(err));
        return err;
    }
    ESP_RETURN_ON_ERROR(uart_flush_input(config->port),
                        TAG, "flush UART%d failed", config->port);

    char task_name[16];
    snprintf(task_name, sizeof(task_name), "gps_uart%d_rx", config->port);
    BaseType_t ok = xTaskCreate(gps_uart_rx_task, task_name, GPS_UART_TASK_STACK_SIZE,
                                config, GPS_UART_TASK_PRIORITY, NULL);
    if(ok != pdPASS) {
        uart_driver_delete(config->port);
        return ESP_ERR_NO_MEM;
    }

    config->started = true;
    ESP_LOGI(TAG, "UART%d GPS RX started, baud=%lu, rx=%d, tx=%d",
             config->port, s_snapshot.current_baud,
             config->rx_gpio, config->tx_gpio);
    return ESP_OK;
}

esp_err_t newfeatures_gps_uart_start(void)
{
    if(s_started) return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if(s_lock == NULL) return ESP_ERR_NO_MEM;

    esp_err_t first_err = ESP_OK;
    bool any_started = false;

    for(size_t i = 0; i < sizeof(s_ports) / sizeof(s_ports[0]); i++) {
        esp_err_t err = start_port(&s_ports[i]);
        if(err == ESP_OK) {
            any_started = true;
        } else if(first_err == ESP_OK) {
            first_err = err;
            ESP_LOGW(TAG, "UART%d GPS RX not started: %s",
                     s_ports[i].port, esp_err_to_name(err));
        }
    }

    if(xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_snapshot.uart0_started = s_ports[0].started;
        s_snapshot.uart1_started = s_ports[1].started;
        s_snapshot.uart2_started = s_ports[2].started;
        xSemaphoreGive(s_lock);
    }

    s_started = any_started;
    return any_started ? ESP_OK : first_err;
}

void newfeatures_gps_uart_get_snapshot(newfeatures_gps_uart_snapshot_t *snapshot)
{
    if(snapshot == NULL) return;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->active_port = UART_NUM_MAX;

    if(s_lock == NULL) return;
    if(xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        *snapshot = s_snapshot;
        xSemaphoreGive(s_lock);
    }
}

void newfeatures_gps_uart_clear(void)
{
    if(s_lock == NULL) return;
    if(xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_snapshot.text[0] = '\0';
        s_snapshot.protocol[0] = '\0';
        s_snapshot.decoded[0] = '\0';
        s_snapshot.decoded_ok = false;
        s_snapshot.total_bytes = 0;
        s_snapshot.last_rx_ms = 0;
        s_snapshot.active_port = UART_NUM_MAX;
        memset(s_snapshot.port_bytes, 0, sizeof(s_snapshot.port_bytes));
        memset(s_snapshot.timeout_count, 0, sizeof(s_snapshot.timeout_count));
        xSemaphoreGive(s_lock);
    }
}
