#include "obd_data_cache.h"

#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

#define RPM_SMOOTH_TIME_MS        650U
#define SPEED_SMOOTH_TIME_MS      600U
#define RPM_FAST_SMOOTH_TIME_MS   180U
#define SPEED_FAST_SMOOTH_TIME_MS 220U
#define RPM_FAST_DELTA            700.0f
#define SPEED_FAST_DELTA          8.0f
#define FALL_TO_ZERO_MS           300U

#define FINAL_DRIVE_RATIO      4.052f
#define TIRE_ROLLING_RADIUS_M  0.298f
#define CONSTANT_C             0.377f
#define CALCULATION_CONSTANT   5.128f

typedef struct {
    float min_ratio;
    float max_ratio;
    obd_gear_t gear;
} gear_ratio_range_t;

static const gear_ratio_range_t s_gear_ranges[] = {
    {22.0f, 26.0f, OBD_GEAR_1},
    {12.0f, 14.0f, OBD_GEAR_2},
    {8.0f, 9.5f, OBD_GEAR_3},
    {5.8f, 6.8f, OBD_GEAR_4},
    {4.8f, 5.5f, OBD_GEAR_5},
};

static volatile bool s_has_speed;
static volatile bool s_has_rpm;
static volatile bool s_has_fuel;
static volatile bool s_has_coolant_temp;
static volatile bool s_has_intake_temp;
static volatile bool s_has_engine_load;
static volatile bool s_has_throttle;
static volatile bool s_has_voltage;
static volatile bool s_has_oil_temp;

static volatile uint16_t s_speed_kmh;
static volatile uint16_t s_rpm;
static volatile uint8_t s_fuel_percent;
static volatile int16_t s_coolant_temp_c = -40;
static volatile int16_t s_intake_temp_c = -40;
static volatile uint8_t s_engine_load_percent;
static volatile uint8_t s_throttle_percent;
static volatile uint16_t s_voltage_mv;
static volatile int16_t s_oil_temp_c = -100;
static volatile uint32_t s_cache_generation;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint16_t smooth_u16(uint16_t raw, float *smooth, TickType_t *last_tick,
                           uint32_t *seen_generation, uint32_t generation,
                           uint32_t smooth_time_ms, uint32_t fast_smooth_time_ms,
                           float fast_delta)
{
    TickType_t now_tick = xTaskGetTickCount();
    uint32_t dt_ms = 0;
    if(*seen_generation != generation) {
        *seen_generation = generation;
        *last_tick = 0;
    } else if(*last_tick != 0) {
        dt_ms = (uint32_t)((now_tick - *last_tick) * portTICK_PERIOD_MS);
        if(dt_ms > 1000U) dt_ms = 1000U;
    }

    uint32_t tc = raw == 0 ? FALL_TO_ZERO_MS : smooth_time_ms;
    if(raw != 0 && fast_delta > 0.0f && fast_smooth_time_ms < tc) {
        float delta = (float)raw - *smooth;
        if(delta < 0.0f) delta = -delta;
        if(delta >= fast_delta) {
            tc = fast_smooth_time_ms;
        } else {
            float ratio = delta / fast_delta;
            tc = (uint32_t)((float)smooth_time_ms -
                            ((float)(smooth_time_ms - fast_smooth_time_ms) * ratio));
        }
    }
    float alpha = tc == 0 ? 1.0f : (float)dt_ms / (float)tc;
    if(alpha > 1.0f) alpha = 1.0f;

    if(*last_tick == 0) {
        *smooth = (float)raw;
    } else {
        *smooth += alpha * ((float)raw - *smooth);
    }
    *last_tick = now_tick;

    return (uint16_t)(*smooth + 0.5f);
}

void obd_data_cache_clear(void)
{
    portENTER_CRITICAL(&s_mux);
    s_has_speed = false;
    s_has_rpm = false;
    s_has_fuel = false;
    s_has_coolant_temp = false;
    s_has_intake_temp = false;
    s_has_engine_load = false;
    s_has_throttle = false;
    s_has_voltage = false;
    s_has_oil_temp = false;
    s_speed_kmh = 0;
    s_rpm = 0;
    s_fuel_percent = 0;
    s_coolant_temp_c = -40;
    s_intake_temp_c = -40;
    s_engine_load_percent = 0;
    s_throttle_percent = 0;
    s_voltage_mv = 0;
    s_oil_temp_c = -100;
    s_cache_generation++;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_speed(uint16_t kmh)
{
    portENTER_CRITICAL(&s_mux);
    s_speed_kmh = kmh;
    s_has_speed = true;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_rpm(uint16_t rpm)
{
    portENTER_CRITICAL(&s_mux);
    s_rpm = rpm;
    s_has_rpm = true;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_fuel_percent(uint8_t percent)
{
    portENTER_CRITICAL(&s_mux);
    s_fuel_percent = percent;
    s_has_fuel = true;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_coolant_temp(int16_t temp_c)
{
    portENTER_CRITICAL(&s_mux);
    s_coolant_temp_c = temp_c;
    s_has_coolant_temp = true;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_intake_temp(int16_t temp_c)
{
    portENTER_CRITICAL(&s_mux);
    s_intake_temp_c = temp_c;
    s_has_intake_temp = true;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_engine_load(uint8_t percent)
{
    portENTER_CRITICAL(&s_mux);
    s_engine_load_percent = percent;
    s_has_engine_load = true;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_throttle(uint8_t percent)
{
    portENTER_CRITICAL(&s_mux);
    s_throttle_percent = percent;
    s_has_throttle = true;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_voltage(uint16_t mv)
{
    portENTER_CRITICAL(&s_mux);
    s_voltage_mv = mv;
    s_has_voltage = true;
    portEXIT_CRITICAL(&s_mux);
}

void obd_data_set_oil_temp(int16_t temp_c)
{
    if(temp_c < -20 || temp_c > 150) return;
    portENTER_CRITICAL(&s_mux);
    s_oil_temp_c = temp_c;
    s_has_oil_temp = true;
    portEXIT_CRITICAL(&s_mux);
}

bool obd_data_has_speed(void) { return s_has_speed; }
bool obd_data_has_rpm(void) { return s_has_rpm; }
bool obd_data_has_fuel_percent(void) { return s_has_fuel; }
bool obd_data_has_coolant_temp(void) { return s_has_coolant_temp; }
bool obd_data_has_intake_temp(void) { return s_has_intake_temp; }
bool obd_data_has_engine_load(void) { return s_has_engine_load; }
bool obd_data_has_throttle(void) { return s_has_throttle; }
bool obd_data_has_voltage(void) { return s_has_voltage; }
bool obd_data_has_oil_temp(void) { return s_has_oil_temp; }

uint16_t obd_data_get_speed(void)
{
    static TickType_t last_tick = 0;
    static float smooth = 0.0f;
    static uint32_t seen_generation = 0;
    uint16_t raw;
    uint32_t generation;
    portENTER_CRITICAL(&s_mux);
    raw = s_speed_kmh;
    generation = s_cache_generation;
    portEXIT_CRITICAL(&s_mux);
    return smooth_u16(raw, &smooth, &last_tick, &seen_generation, generation,
                      SPEED_SMOOTH_TIME_MS, SPEED_FAST_SMOOTH_TIME_MS,
                      SPEED_FAST_DELTA);
}

uint16_t obd_data_get_rpm(void)
{
    static TickType_t last_tick = 0;
    static float smooth = 0.0f;
    static uint32_t seen_generation = 0;
    uint16_t raw;
    uint32_t generation;
    portENTER_CRITICAL(&s_mux);
    raw = s_rpm;
    generation = s_cache_generation;
    portEXIT_CRITICAL(&s_mux);
    return smooth_u16(raw, &smooth, &last_tick, &seen_generation, generation,
                      RPM_SMOOTH_TIME_MS, RPM_FAST_SMOOTH_TIME_MS,
                      RPM_FAST_DELTA);
}

uint8_t obd_data_get_fuel_percent(void)
{
    uint8_t value;
    portENTER_CRITICAL(&s_mux);
    value = s_fuel_percent;
    portEXIT_CRITICAL(&s_mux);
    return value;
}

int16_t obd_data_get_coolant_temp(void)
{
    int16_t value;
    portENTER_CRITICAL(&s_mux);
    value = s_coolant_temp_c;
    portEXIT_CRITICAL(&s_mux);
    return value;
}

int16_t obd_data_get_intake_temp(void)
{
    int16_t value;
    portENTER_CRITICAL(&s_mux);
    value = s_intake_temp_c;
    portEXIT_CRITICAL(&s_mux);
    return value;
}

uint8_t obd_data_get_engine_load(void)
{
    uint8_t value;
    portENTER_CRITICAL(&s_mux);
    value = s_engine_load_percent;
    portEXIT_CRITICAL(&s_mux);
    return value;
}

uint8_t obd_data_get_throttle(void)
{
    uint8_t value;
    portENTER_CRITICAL(&s_mux);
    value = s_throttle_percent;
    portEXIT_CRITICAL(&s_mux);
    return value;
}

uint16_t obd_data_get_voltage(void)
{
    uint16_t value;
    portENTER_CRITICAL(&s_mux);
    value = s_voltage_mv;
    portEXIT_CRITICAL(&s_mux);
    return value;
}

int16_t obd_data_get_oil_temp(void)
{
    int16_t value;
    portENTER_CRITICAL(&s_mux);
    value = s_oil_temp_c;
    portEXIT_CRITICAL(&s_mux);
    return value;
}

obd_gear_t obd_data_calculate_gear(float rpm, float speed)
{
    static obd_gear_t s_last_gear = OBD_GEAR_NEUTRAL;

    if(rpm <= 0.0f || speed <= 0.0f) {
        s_last_gear = OBD_GEAR_NEUTRAL;
        return OBD_GEAR_NEUTRAL;
    }

    float total_ratio = rpm / (speed * CALCULATION_CONSTANT);
    for(size_t i = 0; i < sizeof(s_gear_ranges) / sizeof(s_gear_ranges[0]); i++) {
        if(total_ratio >= s_gear_ranges[i].min_ratio &&
           total_ratio <= s_gear_ranges[i].max_ratio) {
            s_last_gear = s_gear_ranges[i].gear;
            return s_last_gear;
        }
    }

    if(rpm > 800.0f && speed < 5.0f) {
        s_last_gear = OBD_GEAR_NEUTRAL;
        return OBD_GEAR_NEUTRAL;
    }

    return s_last_gear;
}
