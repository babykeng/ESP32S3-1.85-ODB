#include "remote_control.h"

#include <stdbool.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"

#include "ui.h"

#define REMOTE_AP_SSID       "SCR-CTRL"
#define REMOTE_AP_PASS       ""
#define REMOTE_AP_CHANNEL    1
#define REMOTE_AP_MAX_CONN   4
#define REMOTE_QUEUE_LEN     8

static const char *TAG = "remote_ctl";

typedef enum {
    REMOTE_CMD_NEXT,
    REMOTE_CMD_PREV,
    REMOTE_CMD_SCREEN,
} remote_cmd_type_t;

typedef enum {
    REMOTE_SCREEN_CLOCK,
    REMOTE_SCREEN_IMU,
    REMOTE_SCREEN_DASHBOARD,
    REMOTE_SCREEN_GPS,
    REMOTE_SCREEN_DETAILS,
    REMOTE_SCREEN_BLUETOOTH,
} remote_screen_t;

typedef struct {
    remote_cmd_type_t type;
    remote_screen_t screen;
} remote_cmd_t;

static QueueHandle_t s_cmd_queue;
static httpd_handle_t s_httpd;
static bool s_wifi_started;
static bool s_wifi_handler_registered;

static const char s_index_html[] =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>HUD Control</title>"
    "<style>"
    "body{margin:0;background:#050807;color:#fff;font-family:-apple-system,BlinkMacSystemFont,sans-serif}"
    ".wrap{max-width:420px;margin:0 auto;padding:24px}"
    "h1{font-size:24px;margin:0 0 18px;color:#24d18f}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}"
    "button{display:block;width:100%;padding:14px 12px;border:1px solid #245a48;border-radius:8px;color:#fff;text-align:center;background:#0b1511;font:inherit}"
    "button:active{background:#123225}"
    ".wide{grid-column:span 2}"
    ".status{min-height:20px;margin:14px 0 0;color:#72cba8;font-size:14px;text-align:center}"
    "</style></head><body><div class='wrap'><h1>HUD Control</h1><div class='grid'>"
    "<button data-url='/prev'>Prev</button><button data-url='/next'>Next</button>"
    "<button data-url='/screen?name=clock'>Clock</button><button data-url='/screen?name=imu'>IMU</button>"
    "<button data-url='/screen?name=dashboard'>Dashboard</button><button data-url='/screen?name=gps'>GPS UART</button>"
    "<button data-url='/screen?name=details'>OBD Details</button><button data-url='/screen?name=bluetooth'>Bluetooth</button>"
    "<button class='wide' data-url='/'>Refresh</button>"
    "</div><div class='status' id='status'></div></div>"
    "<script>"
    "const statusEl=document.getElementById('status');"
    "document.querySelectorAll('button[data-url]').forEach(btn=>btn.addEventListener('click',async()=>{"
    "statusEl.textContent='Sending...';"
    "try{const r=await fetch(btn.dataset.url,{cache:'no-store'});"
    "statusEl.textContent=r.ok?'OK':'Error '+r.status;}"
    "catch(e){statusEl.textContent='Network error';}"
    "}));"
    "</script></body></html>";

static void ensure_queue(void)
{
    if(s_cmd_queue == NULL) {
        s_cmd_queue = xQueueCreate(REMOTE_QUEUE_LEN, sizeof(remote_cmd_t));
    }
}

static esp_err_t queue_cmd(remote_cmd_t cmd)
{
    ensure_queue();
    if(s_cmd_queue == NULL) return ESP_ERR_NO_MEM;

    if(xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "remote command queue full");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static esp_err_t send_text(httpd_req_t *req, const char *text)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, text);
}

static bool parse_screen_name(const char *name, remote_screen_t *screen)
{
    if(strcmp(name, "clock") == 0) {
        *screen = REMOTE_SCREEN_CLOCK;
    } else if(strcmp(name, "imu") == 0) {
        *screen = REMOTE_SCREEN_IMU;
    } else if(strcmp(name, "dashboard") == 0) {
        *screen = REMOTE_SCREEN_DASHBOARD;
    } else if(strcmp(name, "gps") == 0 || strcmp(name, "uart") == 0) {
        *screen = REMOTE_SCREEN_GPS;
    } else if(strcmp(name, "details") == 0 || strcmp(name, "obd") == 0) {
        *screen = REMOTE_SCREEN_DETAILS;
    } else if(strcmp(name, "bluetooth") == 0 || strcmp(name, "ble") == 0) {
        *screen = REMOTE_SCREEN_BLUETOOTH;
    } else {
        return false;
    }
    return true;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t next_handler(httpd_req_t *req)
{
    esp_err_t err = queue_cmd((remote_cmd_t){ .type = REMOTE_CMD_NEXT });
    if(err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_text(req, "queue full\n");
    }
    return send_text(req, "ok next\n");
}

static esp_err_t prev_handler(httpd_req_t *req)
{
    esp_err_t err = queue_cmd((remote_cmd_t){ .type = REMOTE_CMD_PREV });
    if(err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_text(req, "queue full\n");
    }
    return send_text(req, "ok prev\n");
}

static esp_err_t screen_handler(httpd_req_t *req)
{
    char query[64] = {0};
    char name[20] = {0};

    if(httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
       httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "missing name\n");
    }

    remote_screen_t screen;
    if(!parse_screen_name(name, &screen)) {
        httpd_resp_set_status(req, "404 Not Found");
        return send_text(req, "unknown screen\n");
    }

    esp_err_t err = queue_cmd((remote_cmd_t){ .type = REMOTE_CMD_SCREEN, .screen = screen });
    if(err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return send_text(req, "queue full\n");
    }
    return send_text(req, "ok screen\n");
}

static void change_to_screen(remote_screen_t screen)
{
    switch(screen) {
        case REMOTE_SCREEN_CLOCK:
            _ui_screen_change(&ui_ScreenPageClock, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                              ui_ScreenPageClock_screen_init);
            break;
        case REMOTE_SCREEN_IMU:
            _ui_screen_change(&ui_ScreenPageIMU, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                              ui_ScreenPageIMU_screen_init);
            break;
        case REMOTE_SCREEN_DASHBOARD:
            _ui_screen_change(&ui_ScreenPageDashboard, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                              ui_ScreenPageDashboard_screen_init);
            break;
        case REMOTE_SCREEN_GPS:
            _ui_screen_change(&ui_ScreenPageUART, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                              ui_ScreenPageUART_screen_init);
            break;
        case REMOTE_SCREEN_DETAILS:
            _ui_screen_change(&ui_ScreenPageDetails, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                              ui_ScreenPageDetails_screen_init);
            break;
        case REMOTE_SCREEN_BLUETOOTH:
            _ui_screen_change(&ui_ScreenPageBluetooth, LV_SCR_LOAD_ANIM_FADE_ON, 5, 0,
                              ui_ScreenPageBluetooth_screen_init);
            break;
    }
}

static void remote_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if(s_cmd_queue == NULL) return;

    remote_cmd_t cmd;
    while(xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
        if(cmd.type == REMOTE_CMD_NEXT) {
            ui_cycle_main_screen(LV_DIR_LEFT);
        } else if(cmd.type == REMOTE_CMD_PREV) {
            ui_cycle_main_screen(LV_DIR_RIGHT);
        } else if(cmd.type == REMOTE_CMD_SCREEN) {
            change_to_screen(cmd.screen);
        }
    }
}

void remote_control_ui_init(void)
{
    ensure_queue();
    lv_timer_create(remote_ui_timer_cb, 50, NULL);
}

static esp_err_t start_http_server(void)
{
    if(s_httpd != NULL) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if(err != ESP_OK) return err;

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    const httpd_uri_t next_uri = {
        .uri = "/next",
        .method = HTTP_GET,
        .handler = next_handler,
    };
    const httpd_uri_t prev_uri = {
        .uri = "/prev",
        .method = HTTP_GET,
        .handler = prev_handler,
    };
    const httpd_uri_t screen_uri = {
        .uri = "/screen",
        .method = HTTP_GET,
        .handler = screen_handler,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &next_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &prev_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &screen_uri));
    return ESP_OK;
}

static void remote_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data)
{
    (void)arg;
    if(event_base != WIFI_EVENT) return;

    switch(event_id) {
        case WIFI_EVENT_AP_START:
            ESP_LOGI(TAG, "AP event: started");
            break;
        case WIFI_EVENT_AP_STOP:
            ESP_LOGW(TAG, "AP event: stopped");
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "AP event: station joined aid=%d mac=" MACSTR,
                     event->aid, MAC2STR(event->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGW(TAG, "AP event: station left aid=%d reason=%d mac=" MACSTR,
                     event->aid, event->reason, MAC2STR(event->mac));
            break;
        }
        default:
            ESP_LOGI(TAG, "AP event: id=%ld", (long)event_id);
            break;
    }
}

esp_err_t remote_control_start(void)
{
    ensure_queue();

    esp_err_t err = esp_netif_init();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    static esp_netif_t *ap_netif;
    if(ap_netif == NULL) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    if(!s_wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if(err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) return err;

        if(!s_wifi_handler_registered) {
            err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             remote_wifi_event_handler, NULL);
            if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
            s_wifi_handler_registered = true;
        }

        wifi_config_t ap_config = {0};
        strncpy((char *)ap_config.ap.ssid, REMOTE_AP_SSID, sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen(REMOTE_AP_SSID);
        ap_config.ap.channel = REMOTE_AP_CHANNEL;
        ap_config.ap.max_connection = REMOTE_AP_MAX_CONN;
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        if(REMOTE_AP_PASS[0] != '\0') {
            strncpy((char *)ap_config.ap.password, REMOTE_AP_PASS, sizeof(ap_config.ap.password));
            ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
            ap_config.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
            ap_config.ap.pmf_cfg.required = false;
        }

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
        ESP_LOGI(TAG, "SoftAP started SSID=%s pass=%s URL=http://192.168.4.1/",
                 REMOTE_AP_SSID, REMOTE_AP_PASS);
    }

    return start_http_server();
}
