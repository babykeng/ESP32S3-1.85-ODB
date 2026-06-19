#include "ui.h"

#include "esp_log.h"

lv_obj_t *ui_ScreenPageLogo = NULL;
lv_obj_t *ui_ScreenPageBluetooth = NULL;
lv_obj_t *ui_ScreenPageClock = NULL;
lv_obj_t *ui_ScreenPageDashboard = NULL;
lv_obj_t *ui_ScreenPageDetails = NULL;
lv_obj_t *ui_ScreenPageIMU = NULL;
lv_obj_t *ui_ScreenPageUART = NULL;
lv_obj_t *imageLogo = NULL;

#define UI_SCREEN_CHANGE_GUARD_MS 80

static const char *TAG = "ui";
static uint32_t s_last_screen_change_tick = 0;

typedef struct {
    lv_obj_t **screen;
    void (*init)(void);
} ui_main_page_t;

lv_obj_t *ui_create_outer_ring(lv_obj_t *parent, lv_coord_t size, lv_coord_t width, lv_color_t color)
{
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, size, size);
    lv_obj_set_align(ring, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ring, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ring, color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(ring, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ring, width, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return ring;
}

void _ui_screen_change(lv_obj_t **target, lv_scr_load_anim_t fademode, int spd, int delay,
                       void (*target_init)(void))
{
    if(*target == NULL) {
        target_init();
    }

    lv_obj_t *target_scr = *target;
    if(target_scr == NULL || lv_scr_act() == target_scr) {
        return;
    }

    lv_disp_t *disp = lv_obj_get_disp(target_scr);
    if(disp != NULL && disp->scr_to_load != NULL) {
        return;
    }

    uint32_t now = lv_tick_get();
    if(s_last_screen_change_tick != 0 &&
       lv_tick_elaps(s_last_screen_change_tick) < UI_SCREEN_CHANGE_GUARD_MS) {
        return;
    }

    s_last_screen_change_tick = now;
    ESP_LOGI(TAG, "screen change %p -> %p", lv_scr_act(), target_scr);
    lv_scr_load_anim(target_scr, fademode, spd, delay, false);
}

void ui_cycle_main_screen(lv_dir_t dir)
{
    static const ui_main_page_t pages[] = {
        { &ui_ScreenPageClock, &ui_ScreenPageClock_screen_init },
        { &ui_ScreenPageIMU, &ui_ScreenPageIMU_screen_init },
        { &ui_ScreenPageDashboard, &ui_ScreenPageDashboard_screen_init },
        { &ui_ScreenPageUART, &ui_ScreenPageUART_screen_init },
        { &ui_ScreenPageDetails, &ui_ScreenPageDetails_screen_init },
    };

    int delta = 0;
    if(dir == LV_DIR_LEFT) {
        delta = 1;
    } else if(dir == LV_DIR_RIGHT) {
        delta = -1;
    } else {
        return;
    }

    lv_obj_t *active = lv_scr_act();
    size_t current = 0;
    for(size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        if(*pages[i].screen == active) {
            current = i;
            break;
        }
    }

    size_t count = sizeof(pages) / sizeof(pages[0]);
    size_t next = (current + count + delta) % count;
    _ui_screen_change(pages[next].screen, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0, pages[next].init);
}

void ui_event_imu_background(lv_event_t *e)
{
    if(lv_event_get_code(e) != LV_EVENT_GESTURE) return;

    lv_indev_t *indev = lv_indev_get_act();
    if(!indev) return;

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if(dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        ui_cycle_main_screen(dir);
        lv_indev_wait_release(indev);
    }
}

static void logo_timer_cb(lv_timer_t *timer)
{
    if(timer) lv_timer_del(timer);
    _ui_screen_change(&ui_ScreenPageClock, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0,
                      &ui_ScreenPageClock_screen_init);
}

void boot_logo_ui_init(void)
{
    ui_ScreenPageLogo_screen_init();
    lv_disp_load_scr(ui_ScreenPageLogo);
    lv_timer_create(logo_timer_cb, 1500, NULL);
}
