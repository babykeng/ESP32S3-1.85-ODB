#include <stdio.h>
#include <string.h>

#include "../obd_ble.h"
#include "../ui.h"

#define BT_FONT (&lv_font_montserrat_14)

static lv_obj_t *s_status_label;
static lv_obj_t *s_saved_label;
static lv_obj_t *s_list;
static lv_obj_t *s_empty_label;
static char s_button_names[OBD_BLE_MAX_DEVICES][OBD_BLE_DEVICE_NAME_LEN];
static uint8_t s_rendered_count = 0xff;

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                            lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, BT_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_MID, x, y);
    return label;
}

static void connect_event_cb(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    const char *name = lv_event_get_user_data(event);
    obd_ble_connect_by_name(name);
}

static void scan_event_cb(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    s_rendered_count = 0xff;
    obd_ble_scan_all();
}

static void back_gesture_cb(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_GESTURE) return;
    lv_indev_t *indev = lv_indev_get_act();
    if(!indev) return;
    if(lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) return;

    _ui_screen_change(&ui_ScreenPageDetails, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                      &ui_ScreenPageDetails_screen_init);
    lv_indev_wait_release(indev);
}

static void open_dashboard(void)
{
    _ui_screen_change(&ui_ScreenPageDashboard, LV_SCR_LOAD_ANIM_FADE_ON, 250, 0,
                      &ui_ScreenPageDashboard_screen_init);
}

static void render_devices(const obd_ble_device_list_t *devices)
{
    if(s_list == NULL || s_empty_label == NULL) return;

    if(devices->count == s_rendered_count) {
        bool same = true;
        for(uint8_t i = 0; i < devices->count; i++) {
            if(strcmp(s_button_names[i], devices->devices[i].name) != 0) {
                same = false;
                break;
            }
        }
        if(same) return;
    }

    lv_obj_clean(s_list);
    s_rendered_count = devices->count;

    if(devices->count == 0) {
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);

    for(uint8_t i = 0; i < devices->count; i++) {
        strlcpy(s_button_names[i], devices->devices[i].name, sizeof(s_button_names[i]));

        lv_obj_t *btn = lv_btn_create(s_list);
        lv_obj_set_size(btn, 260, 34);
        lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x163C34), LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x216B5A), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x24D18F), LV_PART_MAIN);
        lv_obj_add_event_cb(btn, connect_event_cb, LV_EVENT_CLICKED, s_button_names[i]);

        char text[64];
        snprintf(text, sizeof(text), "%s  %ddBm", devices->devices[i].name, devices->devices[i].rssi);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, text);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, 224);
        lv_obj_set_style_text_font(label, BT_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_center(label);
    }
}

static void update_bluetooth_cb(lv_timer_t *timer)
{
    (void)timer;
    if(ui_ScreenPageBluetooth == NULL || lv_scr_act() != ui_ScreenPageBluetooth ||
       s_status_label == NULL || s_saved_label == NULL) {
        return;
    }

    obd_ble_snapshot_t snapshot;
    obd_ble_device_list_t devices;
    obd_ble_get_snapshot(&snapshot);
    obd_ble_get_devices(&devices);

    if(snapshot.state == OBD_BLE_STATE_READY) {
        open_dashboard();
        return;
    }

    lv_label_set_text(s_status_label, snapshot.status);
    lv_obj_set_style_text_color(s_status_label,
                                snapshot.state == OBD_BLE_STATE_ERROR ? lv_color_hex(0xFF605C) :
                                snapshot.state == OBD_BLE_STATE_CONNECTING ? lv_color_hex(0x36E68A) :
                                lv_color_hex(0xF2C14E),
                                LV_PART_MAIN);

    char saved[64];
    snprintf(saved, sizeof(saved), "Saved: %s", snapshot.saved_name[0] ? snapshot.saved_name : "--");
    lv_label_set_text(s_saved_label, saved);

    render_devices(&devices);
}

void ui_ScreenPageBluetooth_screen_init(void)
{
    if(ui_ScreenPageBluetooth != NULL) {
        return;
    }

    ui_ScreenPageBluetooth = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageBluetooth, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui_ScreenPageBluetooth, back_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_radius(ui_ScreenPageBluetooth, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ScreenPageBluetooth, lv_color_hex(0x050807), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ScreenPageBluetooth, 255, LV_PART_MAIN);

    lv_obj_t *ring = lv_obj_create(ui_ScreenPageBluetooth);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, 344, 344);
    lv_obj_center(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(ring, 6, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring, lv_color_hex(0x24D18F), LV_PART_MAIN);
    lv_obj_set_style_border_opa(ring, 140, LV_PART_MAIN);

    make_label(ui_ScreenPageBluetooth, "Bluetooth", 0, 26, lv_color_hex(0xFFFFFF));
    s_status_label = make_label(ui_ScreenPageBluetooth, "Scanning", 0, 52, lv_color_hex(0xF2C14E));
    s_saved_label = make_label(ui_ScreenPageBluetooth, "Saved: --", 0, 76, lv_color_hex(0x82BFA8));

    lv_obj_t *scan_btn = lv_btn_create(ui_ScreenPageBluetooth);
    lv_obj_set_size(scan_btn, 98, 30);
    lv_obj_align(scan_btn, LV_ALIGN_TOP_MID, 0, 102);
    lv_obj_set_style_radius(scan_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(scan_btn, lv_color_hex(0x235146), LV_PART_MAIN);
    lv_obj_add_event_cb(scan_btn, scan_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *scan_label = lv_label_create(scan_btn);
    lv_label_set_text(scan_label, "Scan All");
    lv_obj_set_style_text_font(scan_label, BT_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(scan_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_center(scan_label);

    s_list = lv_obj_create(ui_ScreenPageBluetooth);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 270, 172);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 145);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_list, 8, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    s_empty_label = make_label(ui_ScreenPageBluetooth, "No named BLE devices", 0, 210, lv_color_hex(0x60756D));

    lv_timer_create(update_bluetooth_cb, 500, NULL);
}
