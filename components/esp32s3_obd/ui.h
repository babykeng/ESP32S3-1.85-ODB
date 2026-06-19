#ifndef BOOT_LOGO_UI_H
#define BOOT_LOGO_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

extern lv_obj_t *ui_ScreenPageLogo;
extern lv_obj_t *ui_ScreenPageBluetooth;
extern lv_obj_t *ui_ScreenPageClock;
extern lv_obj_t *ui_ScreenPageDashboard;
extern lv_obj_t *ui_ScreenPageDetails;
extern lv_obj_t *ui_ScreenPageIMU;
extern lv_obj_t *ui_ScreenPageUART;
extern lv_obj_t *imageLogo;

void ui_ScreenPageLogo_screen_init(void);
void ui_ScreenPageBluetooth_screen_init(void);
void ui_ScreenPageClock_screen_init(void);
void ui_ScreenPageDashboard_screen_init(void);
void ui_ScreenPageDetails_screen_init(void);
void ui_ScreenPageIMU_screen_init(void);
void ui_ScreenPageUART_screen_init(void);
void boot_logo_ui_init(void);

lv_obj_t *ui_create_outer_ring(lv_obj_t *parent, lv_coord_t size, lv_coord_t width, lv_color_t color);
void _ui_screen_change(lv_obj_t **target, lv_scr_load_anim_t fademode, int spd, int delay,
                       void (*target_init)(void));
void ui_cycle_main_screen(lv_dir_t dir);

void ui_event_imu_background(lv_event_t *e);

LV_IMG_DECLARE(pngLogoSkyGarage);

LV_FONT_DECLARE(ui_font_FontTypoderSize20);
LV_FONT_DECLARE(ui_font_FontTypoderSize24);
LV_FONT_DECLARE(ui_font_FontTypoderSize28);
LV_FONT_DECLARE(ui_font_FontTypoderSize100);
LV_FONT_DECLARE(ui_font_FontTypoderSize140);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
