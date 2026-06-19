#include <stdio.h>

#include "../gps_uart.h"
#include "../ui.h"

static lv_obj_t *s_status_dot = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_lat_label = NULL;
static lv_obj_t *s_lon_label = NULL;
static lv_obj_t *s_speed_label = NULL;
static uint32_t s_last_total_bytes = 0;

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            lv_color_t color, lv_coord_t x, lv_coord_t y,
                            lv_coord_t width, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, x, y);
    return label;
}

static lv_obj_t *make_bottom_metric(lv_obj_t *parent, const char *caption, lv_coord_t y)
{
    make_label(parent, caption, &ui_font_FontTypoderSize20, lv_color_hex(0x6FAF97),
               -86, y, 78, LV_TEXT_ALIGN_RIGHT);
    return make_label(parent, "--", &ui_font_FontTypoderSize20, lv_color_hex(0xFFFFFF),
                      42, y, 170, LV_TEXT_ALIGN_LEFT);
}

static void format_fixed(char *buf, size_t buf_size, double value, uint32_t decimals, const char *suffix)
{
    uint32_t multiplier = 1;
    for(uint32_t i = 0; i < decimals; i++) multiplier *= 10;

    bool negative = value < 0.0;
    double abs_value = negative ? -value : value;
    long scaled = (long)(abs_value * (double)multiplier + 0.5);
    long whole = scaled / (long)multiplier;
    long fraction = scaled % (long)multiplier;

    if(decimals == 0) {
        lv_snprintf(buf, buf_size, "%s%ld%s", negative ? "-" : "",
                    whole, suffix ? suffix : "");
    } else {
        char frac[8];
        frac[decimals] = '\0';
        for(int i = (int)decimals - 1; i >= 0; i--) {
            frac[i] = (char)('0' + (fraction % 10));
            fraction /= 10;
        }
        lv_snprintf(buf, buf_size, "%s%ld.%s%s", negative ? "-" : "",
                    whole, frac, suffix ? suffix : "");
    }
}

static void uart_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if(timer != NULL && (ui_ScreenPageUART == NULL || lv_scr_act() != ui_ScreenPageUART)) {
        return;
    }
    if(s_status_dot == NULL || s_time_label == NULL || s_lat_label == NULL ||
       s_lon_label == NULL || s_speed_label == NULL) {
        return;
    }

    newfeatures_gps_uart_snapshot_t snapshot;
    newfeatures_gps_uart_get_snapshot(&snapshot);

    bool receiving = snapshot.total_bytes > 0;
    bool fixed = snapshot.fix_valid;

    lv_color_t dot_color = fixed ? lv_color_hex(0x22FF88) :
                           (receiving ? lv_color_hex(0xFFB000) : lv_color_hex(0xD93A3A));
    lv_obj_set_style_bg_color(s_status_dot, dot_color, LV_PART_MAIN);

    char buf[64];
    lv_label_set_text(s_time_label, snapshot.local_time[0] != '\0' ? snapshot.local_time : "--:--:--");

    if(snapshot.has_position) {
        format_fixed(buf, sizeof(buf), snapshot.latitude, 5, "");
        lv_label_set_text(s_lat_label, buf);
        format_fixed(buf, sizeof(buf), snapshot.longitude, 5, "");
        lv_label_set_text(s_lon_label, buf);
    } else {
        lv_label_set_text(s_lat_label, "--");
        lv_label_set_text(s_lon_label, "--");
    }

    if(snapshot.has_speed) {
        format_fixed(buf, sizeof(buf), snapshot.speed_kmh, 0, "");
        lv_label_set_text(s_speed_label, buf);
    } else {
        lv_label_set_text(s_speed_label, "00");
    }

    lv_color_t border_color = snapshot.total_bytes != s_last_total_bytes ?
                              lv_color_hex(0x22FF88) : lv_color_hex(0x265A48);
    lv_obj_set_style_border_color(s_status_dot, border_color, LV_PART_MAIN);
    s_last_total_bytes = snapshot.total_bytes;
}

static void gesture_cb(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_GESTURE) return;

    lv_indev_t *indev = lv_indev_get_act();
    if(!indev) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if(dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        ui_cycle_main_screen(dir);
        lv_indev_wait_release(indev);
    }
}

void ui_ScreenPageUART_screen_init(void)
{
    if(ui_ScreenPageUART != NULL) {
        return;
    }

    ui_ScreenPageUART = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageUART, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui_ScreenPageUART, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(ui_ScreenPageUART, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ScreenPageUART, lv_color_hex(0x050807), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ScreenPageUART, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_create_outer_ring(ui_ScreenPageUART, 360, 10, lv_color_hex(0x24D18F));

    s_status_dot = lv_obj_create(ui_ScreenPageUART);
    lv_obj_remove_style_all(s_status_dot);
    lv_obj_set_size(s_status_dot, 18, 18);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_status_dot, lv_color_hex(0xD93A3A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_status_dot, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_status_dot, lv_color_hex(0x5A2626), LV_PART_MAIN);
    lv_obj_align(s_status_dot, LV_ALIGN_CENTER, -84, -134);
    lv_obj_clear_flag(s_status_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_time_label = make_label(ui_ScreenPageUART, "--:--:--", &ui_font_FontTypoderSize24,
                              lv_color_hex(0xFFFFFF), 34, -134, 150, LV_TEXT_ALIGN_LEFT);

    s_speed_label = make_label(ui_ScreenPageUART, "00", &ui_font_FontTypoderSize140,
                               lv_color_hex(0xFFFFFF), 0, -22, 320, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_height(s_speed_label, 110);
    make_label(ui_ScreenPageUART, "km/h", &ui_font_FontTypoderSize20, lv_color_hex(0x6FAF97),
               0, 58, 96, LV_TEXT_ALIGN_CENTER);

    s_lat_label = make_bottom_metric(ui_ScreenPageUART, "Lat", 104);
    s_lon_label = make_bottom_metric(ui_ScreenPageUART, "Lon", 130);

    lv_obj_add_event_cb(ui_ScreenPageUART, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_timer_create(uart_timer_cb, 250, NULL);
    uart_timer_cb(NULL);
}
