#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "lvgl.h"

#include "components/esp32s3_obd/ui.h"

#define SCREEN_W 360
#define SCREEN_H 360

static lv_color_t s_framebuffer[SCREEN_W * SCREEN_H];
static lv_color_t s_draw_buf_1[SCREEN_W * 40];
static lv_color_t s_draw_buf_2[SCREEN_W * 40];
static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t s_disp_drv;

typedef void (*screen_init_fn)(void);

typedef struct {
    const char *name;
    lv_obj_t **screen;
    screen_init_fn init;
    uint32_t settle_ms;
} screen_capture_t;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    (void)drv;
    int32_t w = lv_area_get_width(area);
    for(int32_t y = area->y1; y <= area->y2; y++) {
        if(y < 0 || y >= SCREEN_H) {
            color_map += w;
            continue;
        }
        for(int32_t x = area->x1; x <= area->x2; x++) {
            if(x >= 0 && x < SCREEN_W) {
                s_framebuffer[y * SCREEN_W + x] = *color_map;
            }
            color_map++;
        }
    }
    lv_disp_flush_ready(drv);
}

static void advance(uint32_t ms)
{
    for(uint32_t elapsed = 0; elapsed < ms; elapsed += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

static void force_refresh(void)
{
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
}

static int ensure_dir(const char *path)
{
    if(mkdir(path, 0755) == 0 || errno == EEXIST) return 0;
    fprintf(stderr, "mkdir %s failed: %s\n", path, strerror(errno));
    return -1;
}

static int write_ppm(const char *path)
{
    FILE *fp = fopen(path, "wb");
    if(!fp) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for(int y = 0; y < SCREEN_H; y++) {
        for(int x = 0; x < SCREEN_W; x++) {
            lv_color32_t c32;
            c32.full = lv_color_to32(s_framebuffer[y * SCREEN_W + x]);
            fputc(c32.ch.red, fp);
            fputc(c32.ch.green, fp);
            fputc(c32.ch.blue, fp);
        }
    }

    fclose(fp);
    return 0;
}

static int capture_screen(const screen_capture_t *capture, const char *out_dir)
{
    if(*capture->screen == NULL) {
        capture->init();
    }
    if(*capture->screen == NULL) {
        fprintf(stderr, "%s did not create a screen\n", capture->name);
        return -1;
    }

    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    lv_disp_load_scr(*capture->screen);
    advance(capture->settle_ms);
    force_refresh();

    char ppm_path[512];
    snprintf(ppm_path, sizeof(ppm_path), "%s/%s.ppm", out_dir, capture->name);
    return write_ppm(ppm_path);
}

int main(int argc, char **argv)
{
    const char *out_dir = argc > 1 ? argv[1] : "tools/screenshots/out";

    setenv("TZ", "CST-8", 1);
    tzset();

    if(ensure_dir(out_dir) != 0) return 1;

    lv_init();
    lv_disp_draw_buf_init(&s_disp_buf, s_draw_buf_1, s_draw_buf_2, SCREEN_W * 40);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = SCREEN_W;
    s_disp_drv.ver_res = SCREEN_H;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_disp_buf;
    lv_disp_drv_register(&s_disp_drv);

    const screen_capture_t captures[] = {
        {"01_logo", &ui_ScreenPageLogo, ui_ScreenPageLogo_screen_init, 50},
        {"02_clock", &ui_ScreenPageClock, ui_ScreenPageClock_screen_init, 250},
        {"03_imu", &ui_ScreenPageIMU, ui_ScreenPageIMU_screen_init, 250},
        {"04_dashboard", &ui_ScreenPageDashboard, ui_ScreenPageDashboard_screen_init, 300},
        {"05_gps_uart", &ui_ScreenPageUART, ui_ScreenPageUART_screen_init, 300},
        {"06_obd_details", &ui_ScreenPageDetails, ui_ScreenPageDetails_screen_init, 300},
        {"07_bluetooth", &ui_ScreenPageBluetooth, ui_ScreenPageBluetooth_screen_init, 600},
    };

    for(size_t i = 0; i < sizeof(captures) / sizeof(captures[0]); i++) {
        if(capture_screen(&captures[i], out_dir) != 0) return 1;
        printf("captured %s\n", captures[i].name);
    }

    return 0;
}
