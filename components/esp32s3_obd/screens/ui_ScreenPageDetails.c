#include <stdio.h>

#include "../obd_ble.h"
#include "../ui.h"

#define DETAILS_FONT (&lv_font_montserrat_14)

static lv_obj_t *s_status_label;
static lv_obj_t *s_device_label;
static lv_obj_t *s_optional_labels[6];
static lv_obj_t *s_response_label;

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                            lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, DETAILS_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_MID, x, y);
    return label;
}

static void details_gesture_cb(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_GESTURE) return;

    lv_indev_t *indev = lv_indev_get_act();
    if(!indev) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if(dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        ui_cycle_main_screen(dir);
        lv_indev_wait_release(indev);
    } else if(dir == LV_DIR_TOP) {
        _ui_screen_change(&ui_ScreenPageBluetooth, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                          &ui_ScreenPageBluetooth_screen_init);
        lv_indev_wait_release(indev);
    }
}

static void set_optional_metric(uint8_t index, bool valid, const char *name, const char *fmt, int value)
{
    if(index >= sizeof(s_optional_labels) / sizeof(s_optional_labels[0]) || !s_optional_labels[index]) return;

    if(valid) {
        char value_text[24];
        char text[48];
        snprintf(value_text, sizeof(value_text), fmt, value);
        snprintf(text, sizeof(text), "%s %s", name, value_text);
        lv_label_set_text(s_optional_labels[index], text);
        lv_obj_clear_flag(s_optional_labels[index], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_optional_labels[index], LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_optional_voltage(uint8_t index, bool valid, uint16_t voltage_mv)
{
    if(index >= sizeof(s_optional_labels) / sizeof(s_optional_labels[0]) || !s_optional_labels[index]) return;

    if(valid) {
        char text[48];
        snprintf(text, sizeof(text), "VOLT %d.%03dV", voltage_mv / 1000, voltage_mv % 1000);
        lv_label_set_text(s_optional_labels[index], text);
        lv_obj_clear_flag(s_optional_labels[index], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_optional_labels[index], LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_details_cb(lv_timer_t *timer)
{
    (void)timer;
    if(ui_ScreenPageDetails == NULL || lv_scr_act() != ui_ScreenPageDetails ||
       s_status_label == NULL || s_device_label == NULL || s_response_label == NULL) {
        return;
    }

    obd_ble_snapshot_t snapshot;
    obd_ble_get_snapshot(&snapshot);

    lv_label_set_text(s_status_label, snapshot.status);
    lv_obj_set_style_text_color(s_status_label,
                                snapshot.state == OBD_BLE_STATE_READY ? lv_color_hex(0x36E68A) :
                                snapshot.state == OBD_BLE_STATE_ERROR ? lv_color_hex(0xFF605C) :
                                lv_color_hex(0xF2C14E),
                                LV_PART_MAIN);

    char device[72];
    const char *name = snapshot.connected_name[0] ? snapshot.connected_name :
                       snapshot.saved_name[0] ? snapshot.saved_name : "--";
    snprintf(device, sizeof(device), "BLE %s", name);
    lv_label_set_text(s_device_label, device);

    set_optional_metric(0, snapshot.has_fuel, "FUEL", "%d%%", snapshot.fuel_percent);
    set_optional_metric(1, snapshot.has_intake_temp, "IAT", "%dC", snapshot.intake_temp_c);
    set_optional_metric(2, snapshot.has_engine_load, "LOAD", "%d%%", snapshot.engine_load_percent);
    set_optional_metric(3, snapshot.has_throttle, "TPS", "%d%%", snapshot.throttle_percent);
    set_optional_voltage(4, snapshot.has_voltage, snapshot.voltage_mv);
    set_optional_metric(5, snapshot.has_oil_temp, "OIL", "%dC", snapshot.oil_temp_c);

    if(snapshot.last_response[0] != '\0') {
        lv_label_set_text(s_response_label, snapshot.last_response);
    }
}

void ui_ScreenPageDetails_screen_init(void)
{
    if(ui_ScreenPageDetails != NULL) {
        return;
    }

    ui_ScreenPageDetails = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageDetails, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui_ScreenPageDetails, details_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_radius(ui_ScreenPageDetails, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ScreenPageDetails, lv_color_hex(0x050807), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ScreenPageDetails, 255, LV_PART_MAIN);

    ui_create_outer_ring(ui_ScreenPageDetails, 360, 10, lv_color_hex(0x24D18F));

    make_label(ui_ScreenPageDetails, "Status", 0, 34, lv_color_hex(0xFFFFFF));
    s_status_label = make_label(ui_ScreenPageDetails, "Starting BLE", 0, 62, lv_color_hex(0xF2C14E));
    s_device_label = make_label(ui_ScreenPageDetails, "BLE --", 0, 90, lv_color_hex(0x82BFA8));
    lv_obj_set_width(s_device_label, 250);
    lv_label_set_long_mode(s_device_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_device_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    const lv_coord_t opt_x[] = {-74, 74, -74, 74, -74, 74};
    const lv_coord_t opt_y[] = {142, 142, 184, 184, 226, 226};
    for(uint8_t i = 0; i < sizeof(s_optional_labels) / sizeof(s_optional_labels[0]); i++) {
        s_optional_labels[i] = make_label(ui_ScreenPageDetails, "", opt_x[i], opt_y[i],
                                          lv_color_hex(0xFFFFFF));
        lv_obj_set_width(s_optional_labels[i], 132);
        lv_label_set_long_mode(s_optional_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(s_optional_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_add_flag(s_optional_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_response_label = make_label(ui_ScreenPageDetails, "", 0, 306, lv_color_hex(0x60756D));
    lv_obj_set_width(s_response_label, 250);
    lv_label_set_long_mode(s_response_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_response_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_timer_create(update_details_cb, 250, NULL);
}
