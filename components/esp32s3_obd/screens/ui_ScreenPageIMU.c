// QMI8658 six-axis sensor page

#include "../ui.h"
#include "bsp_obd_dsp/qmi8658/qmi8658.h"

static lv_obj_t *s_dial_layer = NULL;
static lv_obj_t *s_static_bg = NULL;
static lv_obj_t *s_dot = NULL;
static lv_obj_t *s_max_label = NULL;
static lv_obj_t *s_left_label = NULL;
static lv_obj_t *s_right_label = NULL;
static lv_obj_t *s_bottom_label = NULL;
static uint32_t s_last_click_tick = 0;
static uint32_t s_max_total_centi_g = 0;
static uint32_t s_max_left_centi_g = 0;
static uint32_t s_max_right_centi_g = 0;
static uint32_t s_max_brake_centi_g = 0;

#if !defined(IMU_DISABLE_STATIC_BG) && !defined(IMU_SCREENSHOT_STATIC_BG)
#define IMU_USE_STATIC_BG 1
#endif

#if IMU_USE_STATIC_BG
extern const lv_img_dsc_t imuStaticBg;
#endif

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            lv_color_t color, lv_coord_t x, lv_coord_t y,
                            lv_coord_t width, lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_CENTER, x, y);
    return label;
}

static void draw_line(lv_draw_ctx_t *draw_ctx, lv_coord_t x1, lv_coord_t y1,
                      lv_coord_t x2, lv_coord_t y2, lv_coord_t width,
                      lv_color_t color, lv_opa_t opa)
{
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.opa = opa;
    dsc.width = width;
    dsc.round_end = true;
    dsc.round_start = true;

    lv_point_t p1 = { x1, y1 };
    lv_point_t p2 = { x2, y2 };
    lv_draw_line(draw_ctx, &dsc, &p1, &p2);
}

static void draw_arc(lv_draw_ctx_t *draw_ctx, lv_coord_t cx, lv_coord_t cy, uint16_t radius,
                     uint16_t start, uint16_t end, lv_coord_t width,
                     lv_color_t color, bool rounded)
{
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.rounded = rounded;
    dsc.opa = LV_OPA_COVER;

    lv_point_t center = { cx, cy };
    lv_draw_arc(draw_ctx, &dsc, &center, radius, start, end);
}

static void dial_draw_cb(lv_event_t *event)
{
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    lv_area_t coords;
    lv_obj_get_coords(lv_event_get_target(event), &coords);

    lv_coord_t ox = coords.x1;
    lv_coord_t oy = coords.y1;
    lv_coord_t cx = ox + 180;
    lv_coord_t cy = oy + 180;

    draw_arc(draw_ctx, cx, cy, 164, 135, 45, 12, lv_color_hex(0x555655), false);
    draw_arc(draw_ctx, cx, cy, 164, 45, 135, 8, lv_color_hex(0xEF3C36), false);
    draw_arc(draw_ctx, cx, cy - 2, 67, 0, 360, 2, lv_color_hex(0xFFFFFF), false);

    const lv_coord_t tick_y = cy - 2;
    const lv_coord_t tick_x = cx;
    const lv_coord_t tick_pos[] = { -48, -32, -16, 16, 32, 48 };
    for(size_t i = 0; i < sizeof(tick_pos) / sizeof(tick_pos[0]); i++) {
        draw_line(draw_ctx, cx + tick_pos[i], tick_y - 4, cx + tick_pos[i], tick_y + 4,
                  3, lv_color_hex(0xFFFFFF), LV_OPA_COVER);
        draw_line(draw_ctx, tick_x - 3, cy - 2 + tick_pos[i],
                  tick_x + 3, cy - 2 + tick_pos[i],
                  3, lv_color_hex(0xFFFFFF), LV_OPA_COVER);
    }
}

static void set_g_label(lv_obj_t *label, uint32_t centi_g)
{
    char buf[16];
    lv_snprintf(buf, sizeof(buf), "%lu.%02lu", centi_g / 100, centi_g % 100);
    lv_label_set_text(label, buf);
}

static uint32_t isqrt32(uint32_t value)
{
    uint32_t res = 0;
    uint32_t bit = 1UL << 30;

    while(bit > value) bit >>= 2;
    while(bit != 0) {
        if(value >= res + bit) {
            value -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

static int32_t clamp_i32(int32_t value, int32_t min, int32_t max)
{
    if(value < min) return min;
    if(value > max) return max;
    return value;
}

static void imu_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if(ui_ScreenPageIMU == NULL || lv_scr_act() != ui_ScreenPageIMU ||
       s_max_label == NULL || s_left_label == NULL ||
       s_right_label == NULL || s_bottom_label == NULL) {
        return;
    }

    qmi8658_data_t data;
    esp_err_t err = qmi8658_read(&data);
    if(err != ESP_OK) {
        if(s_max_label) lv_label_set_text(s_max_label, "--");
        if(s_left_label) lv_label_set_text(s_left_label, "--");
        if(s_right_label) lv_label_set_text(s_right_label, "--");
        if(s_bottom_label) lv_label_set_text(s_bottom_label, "--");
        return;
    }

    int32_t x_centi_g = (int32_t)(data.acc_g[0] * 100.0f);
    int32_t y_centi_g = (int32_t)(data.acc_g[1] * 100.0f);
    uint32_t abs_x = (uint32_t)(x_centi_g < 0 ? -x_centi_g : x_centi_g);
    uint32_t abs_y = (uint32_t)(y_centi_g < 0 ? -y_centi_g : y_centi_g);
    uint32_t total = isqrt32(abs_x * abs_x + abs_y * abs_y);

    if(total > s_max_total_centi_g) s_max_total_centi_g = total;
    if(x_centi_g < 0 && abs_x > s_max_left_centi_g) s_max_left_centi_g = abs_x;
    if(x_centi_g > 0 && abs_x > s_max_right_centi_g) s_max_right_centi_g = abs_x;
    if(y_centi_g < 0 && abs_y > s_max_brake_centi_g) s_max_brake_centi_g = abs_y;

    set_g_label(s_max_label, s_max_total_centi_g);
    set_g_label(s_left_label, s_max_left_centi_g);
    set_g_label(s_right_label, s_max_right_centi_g);
    set_g_label(s_bottom_label, s_max_brake_centi_g);

    if(s_dot) {
        int32_t dot_x = clamp_i32(x_centi_g, -150, 150) * 67 / 150;
        int32_t dot_y = -clamp_i32(y_centi_g, -150, 150) * 67 / 150;
        lv_obj_align(s_dot, LV_ALIGN_CENTER, dot_x, dot_y - 2);
    }
}

static void calibrate_page_zero(void)
{
    esp_err_t err = qmi8658_calibrate_current();
    if(err == ESP_OK) {
        s_max_total_centi_g = 0;
        s_max_left_centi_g = 0;
        s_max_right_centi_g = 0;
        s_max_brake_centi_g = 0;
        if(s_max_label) set_g_label(s_max_label, 0);
        if(s_left_label) set_g_label(s_left_label, 0);
        if(s_right_label) set_g_label(s_right_label, 0);
        if(s_bottom_label) set_g_label(s_bottom_label, 0);
        if(s_dot) lv_obj_align(s_dot, LV_ALIGN_CENTER, 0, -2);
    }
}

static void click_cb(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;

    uint32_t now = lv_tick_get();
    if(s_last_click_tick != 0 && lv_tick_elaps(s_last_click_tick) < 450) {
        s_last_click_tick = 0;
        calibrate_page_zero();
    } else {
        s_last_click_tick = now;
    }
}

static void screen_loaded_cb(lv_event_t *event)
{
    if(lv_event_get_code(event) == LV_EVENT_SCREEN_LOADED) {
        calibrate_page_zero();
    }
}

void ui_ScreenPageIMU_screen_init(void)
{
    if(ui_ScreenPageIMU != NULL) {
        return;
    }

    ui_ScreenPageIMU = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_ScreenPageIMU, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui_ScreenPageIMU, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(ui_ScreenPageIMU, 360, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_ScreenPageIMU, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_ScreenPageIMU, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

#if IMU_USE_STATIC_BG
    s_static_bg = lv_img_create(ui_ScreenPageIMU);
    lv_img_set_src(s_static_bg, &imuStaticBg);
    lv_obj_align(s_static_bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_static_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
#else
    ui_create_outer_ring(ui_ScreenPageIMU, 360, 10, lv_color_hex(0xD8E3DF));

    s_dial_layer = lv_obj_create(ui_ScreenPageIMU);
    lv_obj_remove_style_all(s_dial_layer);
    lv_obj_set_size(s_dial_layer, 360, 360);
    lv_obj_align(s_dial_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(s_dial_layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_dial_layer, dial_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    make_label(ui_ScreenPageIMU, "G-Force Max", &ui_font_FontTypoderSize20, lv_color_hex(0xFFFFFF),
               0, -122, 210, LV_TEXT_ALIGN_CENTER);
#ifdef IMU_SCREENSHOT_STATIC_BG
    return;
#endif
#endif
    s_max_label = make_label(ui_ScreenPageIMU, "0.00", &ui_font_FontTypoderSize28,
                             lv_color_hex(0xFFFFFF), 0, -90, 120, LV_TEXT_ALIGN_CENTER);
    s_left_label = make_label(ui_ScreenPageIMU, "0.00", &ui_font_FontTypoderSize24,
                              lv_color_hex(0xFFFFFF), -108, 0, 82, LV_TEXT_ALIGN_CENTER);
    s_right_label = make_label(ui_ScreenPageIMU, "0.00", &ui_font_FontTypoderSize24,
                               lv_color_hex(0xFFFFFF), 108, 0, 82, LV_TEXT_ALIGN_CENTER);
    s_bottom_label = make_label(ui_ScreenPageIMU, "0.00", &ui_font_FontTypoderSize24,
                                lv_color_hex(0xFFFFFF), 0, 96, 92, LV_TEXT_ALIGN_CENTER);

    s_dot = lv_obj_create(ui_ScreenPageIMU);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 14, 14);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0xFF6A36), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(s_dot, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(s_dot, lv_color_hex(0xFF7A3D), LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_dot, 160, LV_PART_MAIN);
    lv_obj_align(s_dot, LV_ALIGN_CENTER, 0, -2);
    lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(ui_ScreenPageIMU, ui_event_imu_background, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(ui_ScreenPageIMU, click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_ScreenPageIMU, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
    lv_timer_create(imu_timer_cb, 100, NULL);
}
