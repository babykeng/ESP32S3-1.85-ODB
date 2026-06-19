#ifndef OBD_DATA_CACHE_H
#define OBD_DATA_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    OBD_GEAR_NEUTRAL = 0,
    OBD_GEAR_1,
    OBD_GEAR_2,
    OBD_GEAR_3,
    OBD_GEAR_4,
    OBD_GEAR_5,
    OBD_GEAR_6,
} obd_gear_t;

void obd_data_cache_clear(void);

void obd_data_set_speed(uint16_t kmh);
void obd_data_set_rpm(uint16_t rpm);
void obd_data_set_fuel_percent(uint8_t percent);
void obd_data_set_coolant_temp(int16_t temp_c);
void obd_data_set_intake_temp(int16_t temp_c);
void obd_data_set_engine_load(uint8_t percent);
void obd_data_set_throttle(uint8_t percent);
void obd_data_set_voltage(uint16_t mv);
void obd_data_set_oil_temp(int16_t temp_c);

bool obd_data_has_speed(void);
bool obd_data_has_rpm(void);
bool obd_data_has_fuel_percent(void);
bool obd_data_has_coolant_temp(void);
bool obd_data_has_intake_temp(void);
bool obd_data_has_engine_load(void);
bool obd_data_has_throttle(void);
bool obd_data_has_voltage(void);
bool obd_data_has_oil_temp(void);

uint16_t obd_data_get_speed(void);
uint16_t obd_data_get_rpm(void);
uint8_t obd_data_get_fuel_percent(void);
int16_t obd_data_get_coolant_temp(void);
int16_t obd_data_get_intake_temp(void);
uint8_t obd_data_get_engine_load(void);
uint8_t obd_data_get_throttle(void);
uint16_t obd_data_get_voltage(void);
int16_t obd_data_get_oil_temp(void);

obd_gear_t obd_data_calculate_gear(float rpm, float speed);

#ifdef __cplusplus
}
#endif

#endif
