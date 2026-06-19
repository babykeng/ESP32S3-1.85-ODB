#ifndef NEWFEATURES_GPS_UART_H
#define NEWFEATURES_GPS_UART_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"

#ifndef NEWFEATURES_GPS_UART_BAUD_RATE
#define NEWFEATURES_GPS_UART_BAUD_RATE 115200
#endif

#ifndef NEWFEATURES_GPS_UART0_RX_GPIO
#define NEWFEATURES_GPS_UART0_RX_GPIO GPIO_NUM_44
#endif

#ifndef NEWFEATURES_GPS_UART0_TX_GPIO
#define NEWFEATURES_GPS_UART0_TX_GPIO GPIO_NUM_43
#endif

#ifndef NEWFEATURES_GPS_UART1_RX_GPIO
#define NEWFEATURES_GPS_UART1_RX_GPIO GPIO_NUM_13
#endif

#ifndef NEWFEATURES_GPS_UART1_TX_GPIO
#define NEWFEATURES_GPS_UART1_TX_GPIO UART_PIN_NO_CHANGE
#endif

#ifndef NEWFEATURES_GPS_UART2_RX_GPIO
#define NEWFEATURES_GPS_UART2_RX_GPIO GPIO_NUM_15
#endif

#ifndef NEWFEATURES_GPS_UART2_TX_GPIO
#define NEWFEATURES_GPS_UART2_TX_GPIO GPIO_NUM_14
#endif

typedef struct {
    uart_port_t active_port;
    uint32_t total_bytes;
    uint32_t last_rx_ms;
    uint32_t current_baud;
    uint32_t port_bytes[3];
    uint32_t timeout_count[3];
    bool uart0_started;
    bool uart1_started;
    bool uart2_started;
    bool decoded_ok;
    bool fix_valid;
    bool has_position;
    bool has_speed;
    uint8_t fix_type;
    uint8_t satellites;
    double latitude;
    double longitude;
    double speed_kmh;
    char utc_time[16];
    char local_time[16];
    char protocol[8];
    char decoded[512];
    char text[1024];
} newfeatures_gps_uart_snapshot_t;

esp_err_t newfeatures_gps_uart_start(void);
void newfeatures_gps_uart_get_snapshot(newfeatures_gps_uart_snapshot_t *snapshot);
void newfeatures_gps_uart_clear(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
