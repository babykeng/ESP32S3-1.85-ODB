#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "nvs.h"
#include "../ui.h"
#include "bsp_obd_dsp/pcf85063/pcf85063.h"

static const char *TAG = "clock_ui";
static const char *NVS_NAMESPACE = "clock";
static const char *NVS_THEME_KEY = "theme_idx";

static lv_obj_t *s_sec_hand_layer = NULL;
static lv_obj_t *s_subdial_hand_layer = NULL;
static lv_obj_t *s_time_box = NULL;
static lv_obj_t *s_time_digits[4][7];
static lv_obj_t *s_time_colon[2];
static lv_obj_t *s_set_marker = NULL;
static lv_obj_t *s_outer_disc = NULL;
static lv_obj_t *s_mid_disc = NULL;
static lv_obj_t *s_inner_disc = NULL;
static lv_obj_t *s_subdial = NULL;
static lv_obj_t *s_subdial_hub = NULL;
static lv_obj_t *s_main_hub = NULL;
static lv_obj_t *s_tick_layer = NULL;
static uint8_t s_theme_idx = 0;
static int64_t s_time_offset_sec = 0;
static uint32_t s_last_click_tick = 0;
static uint16_t s_last_sec_angle = UINT16_MAX;
static uint16_t s_last_min_angle = UINT16_MAX;
static uint16_t s_last_hour_angle = UINT16_MAX;
static uint16_t s_sec_angle = 0;
static uint16_t s_subdial_min_angle = 0;
static uint16_t s_subdial_hour_angle = 0;
static uint8_t s_last_digits[4] = {0xFF, 0xFF, 0xFF, 0xFF};

static void clock_timer_cb(lv_timer_t *timer);

typedef enum {
    SET_MODE_NONE,
    SET_MODE_HOUR,
    SET_MODE_MINUTE,
} set_mode_t;

static set_mode_t s_set_mode = SET_MODE_NONE;

typedef struct {
    uint32_t outer;
    uint32_t outer_border;
    uint32_t mid;
    uint32_t mid_border;
    uint32_t inner;
    uint32_t inner_border;
} theme_t;

static const theme_t s_themes[] = {
    {0xD70713, 0x7D0007, 0xDB1019, 0xF65A60, 0xCF0912, 0x7D0007},
    {0x165CFF, 0x06256D, 0x1F74FF, 0x78AAFF, 0x1056D8, 0x06256D},
    {0x10A34A, 0x04451E, 0x16B957, 0x75E39B, 0x0D9340, 0x04451E},
    {0xF08A00, 0x733700, 0xF59B16, 0xFFD070, 0xD97800, 0x733700},
};

static size_t theme_count(void)
{
    return sizeof(s_themes) / sizeof(s_themes[0]);
}

static void polar(lv_coord_t cx, lv_coord_t cy, lv_coord_t r, uint16_t angle, lv_coord_t *x, lv_coord_t *y)
{
    *x = cx + ((r * lv_trigo_sin(angle)) >> LV_TRIGO_SHIFT);
    *y = cy - ((r * lv_trigo_sin(angle + 90)) >> LV_TRIGO_SHIFT);
}

static lv_coord_t min_coord(lv_coord_t a, lv_coord_t b)
{
    return a < b ? a : b;
}

static lv_coord_t max_coord(lv_coord_t a, lv_coord_t b)
{
    return a > b ? a : b;
}

static lv_obj_t *circle(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t size,
                        lv_color_t fill, lv_coord_t border_width, lv_color_t border_color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, size, size);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, fill, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, border_width, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, border_color, LV_PART_MAIN);
    lv_obj_align(obj, LV_ALIGN_CENTER, x, y);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *seg_rect(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_radius(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xF3F3F3), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, 255, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void create_digit(lv_obj_t *parent, uint8_t idx, lv_coord_t x, lv_coord_t y)
{
    s_time_digits[idx][0] = seg_rect(parent, x + 4, y, 14, 5);
    s_time_digits[idx][1] = seg_rect(parent, x + 18, y + 4, 5, 13);
    s_time_digits[idx][2] = seg_rect(parent, x + 18, y + 22, 5, 13);
    s_time_digits[idx][3] = seg_rect(parent, x + 4, y + 36, 14, 5);
    s_time_digits[idx][4] = seg_rect(parent, x, y + 22, 5, 13);
    s_time_digits[idx][5] = seg_rect(parent, x, y + 4, 5, 13);
    s_time_digits[idx][6] = seg_rect(parent, x + 4, y + 18, 14, 5);
}

static void set_segment_color(lv_color_t color)
{
    if(s_time_colon[0] == NULL || s_time_colon[1] == NULL) return;

    for(uint8_t d = 0; d < 4; d++) {
        for(uint8_t s = 0; s < 7; s++) {
            if(s_time_digits[d][s] == NULL) return;
            lv_obj_set_style_bg_color(s_time_digits[d][s], color, LV_PART_MAIN);
        }
    }
    lv_obj_set_style_bg_color(s_time_colon[0], color, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_time_colon[1], color, LV_PART_MAIN);
}

static void set_digit(uint8_t idx, uint8_t value)
{
    if(idx >= 4) return;

    static const uint8_t masks[] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66,
        0x6D, 0x7D, 0x07, 0x7F, 0x6F,
    };
    uint8_t mask = masks[value % 10];

    for(uint8_t s = 0; s < 7; s++) {
        if(s_time_digits[idx][s] == NULL) return;
        lv_obj_set_style_bg_opa(s_time_digits[idx][s], (mask & (1 << s)) ? 255 : 28, LV_PART_MAIN);
    }
}

static void set_time_display(uint32_t hour, uint32_t min)
{
    uint8_t digits[] = {
        hour / 10,
        hour % 10,
        min / 10,
        min % 10,
    };

    for(uint8_t i = 0; i < 4; i++) {
        if(s_last_digits[i] == digits[i]) continue;
        set_digit(i, digits[i]);
        s_last_digits[i] = digits[i];
    }
}

static uint8_t lerp_u8(uint8_t start, uint8_t end, uint8_t step, uint8_t steps)
{
    return start + ((int16_t)(end - start) * step) / steps;
}

static lv_color_t lerp_color(uint32_t start, uint32_t end, uint8_t step, uint8_t steps)
{
    return lv_color_make(lerp_u8((start >> 16) & 0xFF, (end >> 16) & 0xFF, step, steps),
                         lerp_u8((start >> 8) & 0xFF, (end >> 8) & 0xFF, step, steps),
                         lerp_u8(start & 0xFF, end & 0xFF, step, steps));
}

static void draw_tapered_gradient_hand(lv_draw_ctx_t *draw_ctx, lv_coord_t cx, lv_coord_t cy,
                                       uint16_t angle, lv_coord_t len, lv_coord_t tail,
                                       lv_coord_t base_width, uint32_t base_color,
                                       uint32_t tip_color)
{
    const uint8_t segments = 3;
    lv_coord_t start_r = -tail;
    lv_coord_t span = len + tail;

    for(uint8_t i = 0; i < segments; i++) {
        lv_coord_t r0 = start_r + (span * i) / segments;
        lv_coord_t r1 = start_r + (span * (i + 1)) / segments;
        lv_coord_t width = base_width - ((base_width - 1) * (i + 1)) / segments;
        if(width < 1) width = 1;

        lv_point_t p0;
        lv_point_t p1;
        lv_draw_line_dsc_t line_dsc;

        polar(cx, cy, r0, angle, &p0.x, &p0.y);
        polar(cx, cy, r1, angle, &p1.x, &p1.y);

        lv_draw_line_dsc_init(&line_dsc);
        line_dsc.color = lerp_color(base_color, tip_color, i + 1, segments);
        line_dsc.width = width;
        line_dsc.round_start = false;
        line_dsc.round_end = false;
        lv_draw_line(draw_ctx, &line_dsc, &p0, &p1);
    }
}

static void hand_invalidate_area(lv_area_t *area, lv_coord_t cx, lv_coord_t cy,
                                 uint16_t angle, lv_coord_t len, lv_coord_t tail,
                                 lv_coord_t width)
{
    lv_point_t p0;
    lv_point_t p1;
    lv_coord_t pad = width + 4;

    polar(cx, cy, -tail, angle, &p0.x, &p0.y);
    polar(cx, cy, len, angle, &p1.x, &p1.y);
    area->x1 = min_coord(p0.x, p1.x) - pad;
    area->y1 = min_coord(p0.y, p1.y) - pad;
    area->x2 = max_coord(p0.x, p1.x) + pad;
    area->y2 = max_coord(p0.y, p1.y) + pad;
}

static void area_join(lv_area_t *area, const lv_area_t *other)
{
    area->x1 = min_coord(area->x1, other->x1);
    area->y1 = min_coord(area->y1, other->y1);
    area->x2 = max_coord(area->x2, other->x2);
    area->y2 = max_coord(area->y2, other->y2);
}

static void invalidate_hand_move(lv_obj_t *layer, lv_coord_t cx, lv_coord_t cy,
                                 uint16_t old_angle, uint16_t new_angle,
                                 lv_coord_t len, lv_coord_t tail, lv_coord_t width)
{
    if(layer == NULL) return;

    lv_area_t area;
    hand_invalidate_area(&area, cx, cy, new_angle, len, tail, width);
    if(old_angle != UINT16_MAX) {
        lv_area_t old_area;
        hand_invalidate_area(&old_area, cx, cy, old_angle, len, tail, width);
        area_join(&area, &old_area);
    }
    lv_obj_invalidate_area(layer, &area);
}

static void draw_tick(lv_draw_ctx_t *draw_ctx, lv_coord_t cx, lv_coord_t cy,
                      lv_coord_t outer, lv_coord_t inner, uint16_t angle, lv_coord_t width)
{
    lv_point_t p1;
    lv_point_t p2;
    lv_draw_line_dsc_t line_dsc;

    polar(cx, cy, outer, angle, &p1.x, &p1.y);
    polar(cx, cy, inner, angle, &p2.x, &p2.y);

    lv_draw_line_dsc_init(&line_dsc);
    line_dsc.color = lv_color_hex(0xFFFFFF);
    line_dsc.width = width;
    line_dsc.round_start = false;
    line_dsc.round_end = false;
    lv_draw_line(draw_ctx, &line_dsc, &p1, &p2);
}

static void tick_layer_draw_cb(lv_event_t *event)
{
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    lv_area_t coords;

    lv_obj_get_coords(lv_event_get_target(event), &coords);
    lv_coord_t ox = coords.x1;
    lv_coord_t oy = coords.y1;

    for(uint16_t i = 0; i < 240; i++) {
        bool major = (i % 20) == 0;
        bool mid = (i % 10) == 0;
        uint16_t angle = (i * 3) / 2;
        draw_tick(draw_ctx, ox + 180, oy + 180, (major || mid) ? 176 : 170, 163,
                  angle, major ? 4 : (mid ? 2 : 1));
    }

    for(uint8_t i = 0; i < 60; i++) {
        bool major = (i % 5) == 0;
        draw_tick(draw_ctx, ox + 180, oy + 269, 48, major ? 42 : 44,
                  i * 6, major ? 3 : 1);
    }
}

static void subdial_hand_layer_draw_cb(lv_event_t *event)
{
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    lv_area_t coords;

    lv_obj_get_coords(lv_event_get_target(event), &coords);
    lv_coord_t cx = coords.x1 + 180;
    lv_coord_t cy = coords.y1 + 269;

    draw_tapered_gradient_hand(draw_ctx, cx, cy, s_subdial_min_angle, 45, 8, 5,
                               0xB8B8B8, 0xFFFFFF);
    draw_tapered_gradient_hand(draw_ctx, cx, cy, s_subdial_hour_angle, 33, 7, 7,
                               0x050607, 0xF6F6F6);
}

static void sec_hand_layer_draw_cb(lv_event_t *event)
{
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
    lv_area_t coords;

    lv_obj_get_coords(lv_event_get_target(event), &coords);
    draw_tapered_gradient_hand(draw_ctx, coords.x1 + 180, coords.y1 + 180,
                               s_sec_angle, 170, 0, 3, 0xB8B8B8, 0xFFFFFF);
    draw_tapered_gradient_hand(draw_ctx, coords.x1 + 180, coords.y1 + 180,
                               s_sec_angle, 0, 34, 6, 0x050607, 0x050607);
}

static void subdial_label(lv_obj_t *parent, const char *txt, lv_coord_t radius, uint16_t angle)
{
    lv_coord_t x;
    lv_coord_t y;
    lv_coord_t width = txt[1] == '\0' ? 24 : 36;
    polar(180, 269, radius, angle, &x, &y);

    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, txt);
#if LV_FONT_MONTSERRAT_16
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
#else
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
#endif
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(label, width);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, x - width / 2, y - 10);
}

static void outer_label(lv_obj_t *parent, const char *txt, uint16_t angle)
{
    lv_coord_t radius = 132;
    lv_coord_t x;
    lv_coord_t y;
    if(angle == 150 || angle == 210) radius = 126;
    polar(180, 180, radius, angle, &x, &y);
    if(angle == 150) {
        x += 22;
        y -= 4;
    }
    if(angle == 210) {
        x -= 22;
        y -= 4;
    }

    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, txt);
#if LV_FONT_MONTSERRAT_32
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, LV_PART_MAIN);
#else
    lv_obj_set_style_text_font(label, &ui_font_FontTypoderSize28, LV_PART_MAIN);
#endif
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(label, 70);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, x - 35, y - 16);
}

static void get_clock_time(uint32_t *hour, uint32_t *min, uint32_t *sec)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);

    int64_t total;
    if(timeinfo.tm_year >= (2020 - 1900)) {
        total = (int64_t)timeinfo.tm_hour * 3600 +
                (int64_t)timeinfo.tm_min * 60 +
                (int64_t)timeinfo.tm_sec +
                s_time_offset_sec;
    } else {
        total = (int64_t)(lv_tick_get() / 1000) + s_time_offset_sec;
    }

    total %= 86400;
    if(total < 0) total += 86400;

    *sec = total % 60;
    *min = (total / 60) % 60;
    *hour = (total / 3600) % 24;
}

static void get_clock_time_ms(uint32_t *hour, uint32_t *min, uint32_t *sec, uint32_t *ms)
{
    struct timeval tv = {0};
    struct tm timeinfo = {0};

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &timeinfo);

    int64_t total_ms;
    if(timeinfo.tm_year >= (2020 - 1900)) {
        total_ms = ((int64_t)timeinfo.tm_hour * 3600 +
                    (int64_t)timeinfo.tm_min * 60 +
                    (int64_t)timeinfo.tm_sec +
                    s_time_offset_sec) * 1000 +
                   tv.tv_usec / 1000;
    } else {
        total_ms = (int64_t)lv_tick_get() + s_time_offset_sec * 1000;
    }

    total_ms %= 86400000;
    if(total_ms < 0) total_ms += 86400000;

    *ms = total_ms % 1000;
    *sec = (total_ms / 1000) % 60;
    *min = (total_ms / 60000) % 60;
    *hour = (total_ms / 3600000) % 24;
}

static void set_clock_time(uint32_t hour, uint32_t min)
{
    int64_t target = (int64_t)(hour % 24) * 3600 + (int64_t)(min % 60) * 60;

    time_t now = 0;
    struct tm timeinfo = {0};
    time(&now);
    localtime_r(&now, &timeinfo);

    if(timeinfo.tm_year >= (2020 - 1900)) {
        timeinfo.tm_hour = hour % 24;
        timeinfo.tm_min = min % 60;
        timeinfo.tm_sec = 0;
        timeinfo.tm_isdst = -1;
    } else {
        timeinfo.tm_year = 2024 - 1900;
        timeinfo.tm_mon = 0;
        timeinfo.tm_mday = 1;
        timeinfo.tm_hour = hour % 24;
        timeinfo.tm_min = min % 60;
        timeinfo.tm_sec = 0;
        timeinfo.tm_isdst = -1;
    }

    time_t adjusted = mktime(&timeinfo);
    if(adjusted >= 0) {
        struct timeval tv = {
            .tv_sec = adjusted,
            .tv_usec = 0,
        };
        if(settimeofday(&tv, NULL) == 0) {
            s_time_offset_sec = 0;
        }
        localtime_r(&adjusted, &timeinfo);
        (void)pcf85063_write_time(&timeinfo);
        return;
    }

    s_time_offset_sec = target - (int64_t)(lv_tick_get() / 1000);
}

static void apply_theme(uint8_t idx)
{
    if(s_outer_disc == NULL || s_mid_disc == NULL || s_inner_disc == NULL || s_subdial == NULL) {
        return;
    }

    const theme_t *theme = &s_themes[idx % theme_count()];

    lv_obj_set_style_bg_color(s_outer_disc, lv_color_hex(theme->outer), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_outer_disc, lv_color_hex(theme->outer_border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_mid_disc, lv_color_hex(theme->mid), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_mid_disc, lv_color_hex(theme->mid_border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_inner_disc, lv_color_hex(theme->inner), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_inner_disc, lv_color_hex(theme->inner_border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_subdial, lv_color_hex(theme->outer), LV_PART_MAIN);
}

static void load_theme_idx(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if(err != ESP_OK) return;

    uint8_t value = 0;
    err = nvs_get_u8(nvs, NVS_THEME_KEY, &value);
    nvs_close(nvs);

    if(err == ESP_OK) {
        s_theme_idx = value % theme_count();
    }
}

static void save_theme_idx(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "open NVS failed for theme: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(nvs, NVS_THEME_KEY, s_theme_idx);
    if(err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if(err != ESP_OK) {
        ESP_LOGW(TAG, "save theme failed: %s", esp_err_to_name(err));
    }
}

static void apply_set_mode(void)
{
    if(s_time_box == NULL || s_set_marker == NULL) return;

    bool setting = s_set_mode != SET_MODE_NONE;
    lv_color_t accent = lv_color_hex(0xFFD45A);
    lv_color_t normal = lv_color_hex(0xCFCFCF);

    lv_obj_set_style_border_color(s_time_box, setting ? accent : normal, LV_PART_MAIN);
    set_segment_color(setting ? accent : lv_color_hex(0xF3F3F3));

    if(setting) {
        lv_obj_clear_flag(s_set_marker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_set_marker, LV_ALIGN_BOTTOM_MID, s_set_mode == SET_MODE_HOUR ? -33 : 33, -5);
    } else {
        lv_obj_add_flag(s_set_marker, LV_OBJ_FLAG_HIDDEN);
    }
}

static void advance_set_mode(void)
{
    if(s_set_mode == SET_MODE_NONE) {
        s_set_mode = SET_MODE_HOUR;
    } else if(s_set_mode == SET_MODE_HOUR) {
        s_set_mode = SET_MODE_MINUTE;
    } else {
        s_set_mode = SET_MODE_NONE;
    }
    apply_set_mode();
}

static void adjust_set_time(int delta)
{
    uint32_t hour;
    uint32_t min;
    uint32_t sec;
    (void)sec;
    get_clock_time(&hour, &min, &sec);

    if(s_set_mode == SET_MODE_HOUR) {
        hour = (hour + 24 + delta) % 24;
    } else if(s_set_mode == SET_MODE_MINUTE) {
        min = (min + 60 + delta) % 60;
    }

    set_clock_time(hour, min);
    clock_timer_cb(NULL);
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
    } else if(dir == LV_DIR_TOP) {
        if(s_set_mode == SET_MODE_NONE) {
            s_theme_idx = (s_theme_idx + 1) % theme_count();
            apply_theme(s_theme_idx);
            save_theme_idx();
        } else {
            adjust_set_time(1);
        }
        lv_indev_wait_release(indev);
    } else if(dir == LV_DIR_BOTTOM) {
        if(s_set_mode == SET_MODE_NONE) {
            s_theme_idx = (s_theme_idx + theme_count() - 1) % theme_count();
            apply_theme(s_theme_idx);
            save_theme_idx();
        } else {
            adjust_set_time(-1);
        }
        lv_indev_wait_release(indev);
    }
}

static void click_cb(lv_event_t *event)
{
    if(lv_event_get_code(event) != LV_EVENT_CLICKED) return;

    uint32_t now = lv_tick_get();
    if(s_last_click_tick != 0 && lv_tick_elaps(s_last_click_tick) < 450) {
        s_last_click_tick = 0;
        advance_set_mode();
    } else {
        s_last_click_tick = now;
    }
}

static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if(timer != NULL && (ui_ScreenPageClock == NULL || lv_scr_act() != ui_ScreenPageClock)) {
        return;
    }
    if(s_sec_hand_layer == NULL || s_subdial_hand_layer == NULL) {
        return;
    }

    uint32_t hour;
    uint32_t min;
    uint32_t sec;
    uint32_t ms;
    get_clock_time_ms(&hour, &min, &sec, &ms);

    uint16_t sec_angle = sec * 6 + (ms * 6) / 1000;
    uint16_t min_angle = min * 6 + sec / 10;
    uint16_t hour_angle = (hour % 12) * 30 + min / 2;

    if(sec_angle != s_last_sec_angle) {
        invalidate_hand_move(s_sec_hand_layer, 180, 180, s_last_sec_angle, sec_angle, 170, 34, 6);
        s_sec_angle = sec_angle;
        s_last_sec_angle = sec_angle;
    }
    if(min_angle != s_last_min_angle) {
        invalidate_hand_move(s_subdial_hand_layer, 180, 269, s_last_min_angle, min_angle, 45, 8, 5);
        s_subdial_min_angle = min_angle;
        s_last_min_angle = min_angle;
    }
    if(hour_angle != s_last_hour_angle) {
        invalidate_hand_move(s_subdial_hand_layer, 180, 269, s_last_hour_angle, hour_angle, 33, 7, 7);
        s_subdial_hour_angle = hour_angle;
        s_last_hour_angle = hour_angle;
    }
    set_time_display(hour, min);
}

void ui_ScreenPageClock_screen_init(void)
{
    if(ui_ScreenPageClock != NULL) {
        return;
    }

    load_theme_idx();

    lv_obj_t *screen = lv_obj_create(NULL);
    ui_ScreenPageClock = screen;
    s_last_sec_angle = UINT16_MAX;
    s_last_min_angle = UINT16_MAX;
    s_last_hour_angle = UINT16_MAX;
    for(uint8_t i = 0; i < 4; i++) s_last_digits[i] = 0xFF;
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(screen, 360, LV_PART_MAIN);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x110000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, 255, LV_PART_MAIN);

    s_outer_disc = circle(screen, 0, 0, 360, lv_color_hex(0xD70713), 4, lv_color_hex(0x7D0007));
    s_mid_disc = circle(screen, 0, 0, 324, lv_color_hex(0xDB1019), 2, lv_color_hex(0xF65A60));
    s_inner_disc = circle(screen, 0, 0, 222, lv_color_hex(0xCF0912), 2, lv_color_hex(0x7D0007));

    s_tick_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(s_tick_layer);
    lv_obj_set_size(s_tick_layer, 360, 360);
    lv_obj_set_align(s_tick_layer, LV_ALIGN_CENTER);
    lv_obj_clear_flag(s_tick_layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_tick_layer, tick_layer_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    const char *labels[] = {"60", "5", "10", "15", "20", "25", "", "35", "40", "45", "50", "55"};
    for(uint8_t i = 0; i < 12; i++) {
        if(labels[i][0] != '\0') outer_label(screen, labels[i], i * 30);
    }

    s_time_box = lv_obj_create(screen);
    lv_obj_remove_style_all(s_time_box);
    lv_obj_set_size(s_time_box, 144, 64);
    lv_obj_set_style_radius(s_time_box, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_time_box, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_time_box, 255, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_time_box, lv_color_hex(0xCFCFCF), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_time_box, 1, LV_PART_MAIN);
    lv_obj_align(s_time_box, LV_ALIGN_CENTER, 0, -57);
    lv_obj_clear_flag(s_time_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    create_digit(s_time_box, 0, 18, 11);
    create_digit(s_time_box, 1, 44, 11);
    create_digit(s_time_box, 2, 78, 11);
    create_digit(s_time_box, 3, 104, 11);
    s_time_colon[0] = seg_rect(s_time_box, 70, 24, 5, 5);
    s_time_colon[1] = seg_rect(s_time_box, 70, 37, 5, 5);

    s_set_marker = lv_obj_create(s_time_box);
    lv_obj_remove_style_all(s_set_marker);
    lv_obj_set_size(s_set_marker, 28, 3);
    lv_obj_set_style_radius(s_set_marker, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_set_marker, lv_color_hex(0xFFD45A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_set_marker, 255, LV_PART_MAIN);
    lv_obj_add_flag(s_set_marker, LV_OBJ_FLAG_HIDDEN);

    s_subdial = circle(screen, 0, 89, 106, lv_color_hex(0xD70713), 2, lv_color_hex(0xFFFFFF));
    apply_theme(s_theme_idx);
    lv_obj_move_foreground(s_tick_layer);
    subdial_label(screen, "12", 35, 0);
    subdial_label(screen, "3", 37, 90);
    subdial_label(screen, "6", 37, 180);
    subdial_label(screen, "9", 37, 270);
    s_subdial_hub = circle(screen, 0, 89, 16, lv_color_hex(0xFFFFFF), 0, lv_color_hex(0xFFFFFF));

    s_subdial_hand_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(s_subdial_hand_layer);
    lv_obj_set_size(s_subdial_hand_layer, 360, 360);
    lv_obj_set_align(s_subdial_hand_layer, LV_ALIGN_CENTER);
    lv_obj_clear_flag(s_subdial_hand_layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_subdial_hand_layer, subdial_hand_layer_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_sec_hand_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(s_sec_hand_layer);
    lv_obj_set_size(s_sec_hand_layer, 360, 360);
    lv_obj_set_align(s_sec_hand_layer, LV_ALIGN_CENTER);
    lv_obj_clear_flag(s_sec_hand_layer, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_sec_hand_layer, sec_hand_layer_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_main_hub = circle(screen, 0, 0, 30, lv_color_hex(0x050607), 0, lv_color_hex(0x050607));

    lv_obj_move_foreground(s_subdial_hand_layer);
    lv_obj_move_foreground(s_subdial_hub);
    lv_obj_move_foreground(s_sec_hand_layer);
    lv_obj_move_foreground(s_main_hub);

    lv_obj_add_event_cb(screen, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(screen, click_cb, LV_EVENT_CLICKED, NULL);
    clock_timer_cb(NULL);
    lv_timer_create(clock_timer_cb, 500, NULL);
}
