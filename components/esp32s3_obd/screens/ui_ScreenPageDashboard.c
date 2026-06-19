#include <stdio.h>
#include <math.h>

#include "../obd_ble.h"
#include "../ui.h"

static lv_obj_t *s_speed_label;
static lv_obj_t *s_temp_label;
static lv_obj_t *s_temp_arc;
static lv_obj_t *s_rpm_needle_layer;
static float s_rpm_needle_deg = 150.0f;

#define DASH_FONT (&ui_font_FontTypoderSize20)
#define DASH_RPM_MAX 10000

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                            const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_MID, x, y);
    return label;
}

static lv_obj_t *make_center_label(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                                   lv_coord_t width, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = make_label(parent, text, x, y, font, color);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    return label;
}

static lv_obj_t *make_arc(lv_obj_t *parent, lv_coord_t size, lv_coord_t width,
                          lv_color_t color, uint16_t start, uint16_t end, lv_opa_t opa)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc, size, size);
    lv_obj_center(arc);
    lv_arc_set_bg_angles(arc, start, end);
    lv_arc_set_angles(arc, start, end);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, color, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    return arc;
}

static void set_text_metric(lv_obj_t *label, bool valid, const char *fmt, int value,
                            const char *empty)
{
    char text[24];
    if(valid) {
        snprintf(text, sizeof(text), fmt, value);
        lv_label_set_text(label, text);
    } else {
        lv_label_set_text(label, empty);
    }
}

static void set_speed_metric(lv_obj_t *label, bool valid, int value)
{
    char text[24];
    if(valid) {
        snprintf(text, sizeof(text), "%d", value);
        lv_label_set_text(label, text);
    } else {
        lv_label_set_text(label, "00");
    }
}

static void set_temp_arc_color(bool valid, int temp_c)
{
    if(s_temp_arc == NULL) return;

    lv_color_t color = valid && temp_c > 95 ? lv_color_hex(0xBF3038) : lv_color_hex(0x23AA50);
    lv_obj_set_style_arc_color(s_temp_arc, color, LV_PART_MAIN);
}

static void set_rpm_needle(bool valid, uint16_t rpm)
{
    if(s_rpm_needle_layer == NULL) return;

    uint16_t clamped = valid && rpm < DASH_RPM_MAX ? rpm : valid ? DASH_RPM_MAX : 0;
    s_rpm_needle_deg = 150.0f + (206.0f * (float)clamped / (float)DASH_RPM_MAX);
    lv_obj_invalidate(s_rpm_needle_layer);
}

static void rpm_needle_draw_cb(lv_event_t *event)
{
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    lv_area_t coords;
    lv_obj_get_coords(lv_event_get_target(event), &coords);

    float rad = s_rpm_needle_deg * (float)M_PI / 180.0f;
    float ux = cosf(rad);
    float uy = sinf(rad);
    float px = -uy;
    float py = ux;
    lv_coord_t cx = coords.x1 + 180;
    lv_coord_t cy = coords.y1 + 180;

    lv_point_t points[5];
    points[0].x = cx + (lv_coord_t)(ux * 84.0f + px * 7.0f);
    points[0].y = cy + (lv_coord_t)(uy * 84.0f + py * 7.0f);
    points[1].x = cx + (lv_coord_t)(ux * 134.0f + px * 5.0f);
    points[1].y = cy + (lv_coord_t)(uy * 134.0f + py * 5.0f);
    points[2].x = cx + (lv_coord_t)(ux * 154.0f);
    points[2].y = cy + (lv_coord_t)(uy * 154.0f);
    points[3].x = cx + (lv_coord_t)(ux * 134.0f - px * 5.0f);
    points[3].y = cy + (lv_coord_t)(uy * 134.0f - py * 5.0f);
    points[4].x = cx + (lv_coord_t)(ux * 84.0f - px * 7.0f);
    points[4].y = cy + (lv_coord_t)(uy * 84.0f - py * 7.0f);

    lv_draw_rect_dsc_t draw_dsc;
    lv_draw_rect_dsc_init(&draw_dsc);
    draw_dsc.bg_color = lv_color_hex(0xFF625F);
    draw_dsc.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(draw_ctx, &draw_dsc, points, 5);
}

static void make_rpm_number(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 46);
    lv_obj_set_style_text_font(label, DASH_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xDDEEEB), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, x, y);
}

static void update_dashboard_cb(lv_timer_t *timer)
{
    (void)timer;
    if(ui_ScreenPageDashboard == NULL || lv_scr_act() != ui_ScreenPageDashboard ||
       s_speed_label == NULL || s_temp_label == NULL) {
        return;
    }

    obd_ble_snapshot_t snapshot;
    obd_ble_get_snapshot(&snapshot);

    set_speed_metric(s_speed_label, snapshot.has_speed, snapshot.speed_kmh);
    set_text_metric(s_temp_label, snapshot.has_coolant_temp, "%d'C", snapshot.coolant_temp_c, "00'C");
    set_temp_arc_color(snapshot.has_coolant_temp, snapshot.coolant_temp_c);
    set_rpm_needle(snapshot.has_rpm, snapshot.rpm);
}

static void dashboard_gesture_cb(lv_event_t *event)
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

void ui_ScreenPageDashboard_screen_init(void)
{
    if(ui_ScreenPageDashboard != NULL) {
        return;
    }

    ui_ScreenPageDashboard = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageDashboard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ui_ScreenPageDashboard, dashboard_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_radius(ui_ScreenPageDashboard, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ScreenPageDashboard, lv_color_hex(0x050807), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_ScreenPageDashboard, 255, LV_PART_MAIN);

    make_arc(ui_ScreenPageDashboard, 384, 64, lv_color_hex(0x34353A), 150, 316, LV_OPA_COVER);
    make_arc(ui_ScreenPageDashboard, 384, 64, lv_color_hex(0xBF3038), 316, 356, LV_OPA_COVER);
    s_temp_arc = make_arc(ui_ScreenPageDashboard, 332, 20, lv_color_hex(0x23AA50), 65, 115, LV_OPA_COVER);

    static const struct {
        const char *text;
        lv_coord_t x;
        lv_coord_t y;
    } rpm_numbers[] = {
        {"0", -150, 54},
        {"1", -159, -5},
        {"2", -144, -65},
        {"3", -111, -115},
        {"4", -70, -143},
        {"5", -27, -157},
        {"6", 33, -155},
        {"7", 84, -135},
        {"8", 126, -97},
        {"9", 147, -59},
        {"10", 158, -19},
    };
    for(uint8_t i = 0; i < sizeof(rpm_numbers) / sizeof(rpm_numbers[0]); i++) {
        make_rpm_number(ui_ScreenPageDashboard, rpm_numbers[i].text,
                        rpm_numbers[i].x, rpm_numbers[i].y);
    }

    s_rpm_needle_layer = lv_obj_create(ui_ScreenPageDashboard);
    lv_obj_remove_style_all(s_rpm_needle_layer);
    lv_obj_set_size(s_rpm_needle_layer, 360, 360);
    lv_obj_center(s_rpm_needle_layer);
    lv_obj_clear_flag(s_rpm_needle_layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_rpm_needle_layer, rpm_needle_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    set_rpm_needle(false, 0);

    s_speed_label = make_label(ui_ScreenPageDashboard, "00", 0, 122,
                               &ui_font_FontTypoderSize100, lv_color_hex(0xF0A934));
    lv_obj_set_width(s_speed_label, 320);
    lv_obj_set_height(s_speed_label, 86);
    lv_obj_set_style_text_align(s_speed_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    make_center_label(ui_ScreenPageDashboard, "km/h", 0, 220, 100, &ui_font_FontTypoderSize24,
                      lv_color_hex(0xDADADD));

    s_temp_label = make_center_label(ui_ScreenPageDashboard, "00'C", 0, 276, 150,
                                     &ui_font_FontTypoderSize24, lv_color_hex(0xD6D6D9));

    lv_timer_create(update_dashboard_cb, 250, NULL);
}
