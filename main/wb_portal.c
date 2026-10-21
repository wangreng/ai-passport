// main/wb_portal.c —— 见 wb_portal.h。
#include "wb_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wb_store.h"

static const char *TAG = "wb_portal";

#define MAX_BODY   (16 * 1024)
#define REPLY_SIZE 512
#define STA_RETRIES 3

static bool s_net_ready;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static bool s_wifi_inited;
static bool s_wifi_started;
static httpd_handle_t s_httpd;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static char s_ap_ssid[20];
static bool s_has_home;
static int s_sta_retries;

static wb_portal_submit_cb_t s_on_submit;
static wb_portal_status_cb_t s_on_status;
static void *s_user;

static void notify(wb_portal_event_t ev) {
    if (s_on_status) s_on_status(ev, s_user);
}

// ---------------------------------------------------------------------------
// 网页
// ---------------------------------------------------------------------------

static const char PAGE_HEAD[] =
    "<!doctype html><html lang=zh><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>单词盒子</title><style>"
    "body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#fbf6ec;color:#3a2e22}"
    "h1{font-size:22px;margin:0 0 4px}p{line-height:1.5}"
    "textarea,input{width:100%;box-sizing:border-box;font-size:16px;padding:10px;"
    "border:2px solid #d9c9ad;border-radius:10px;background:#fff}"
    "textarea{height:260px;font-family:ui-monospace,monospace}"
    "label{display:block;margin:14px 0 6px;font-weight:600}"
    "button{margin-top:16px;width:100%;padding:14px;font-size:18px;border:0;border-radius:12px;"
    "background:#e0743c;color:#fff;font-weight:700}"
    ".hint{font-size:14px;color:#7a6a55}.box{background:#fff;border-radius:12px;padding:12px;margin:12px 0}"
    "</style></head><body>";

static const char PAGE_FORM[] =
    "<h1>单词盒子 · 换词</h1>"
    "<p class=hint>一行一个词：英文 | 词块 | 中文。词块和中文可以不填。</p>"
    "<form method=post action=/save accept-charset=utf-8>"
    "<label>本周新词</label>"
    "<textarea name=words placeholder='borrow | borrow a book | 借&#10;heavy | a heavy bag | 重的&#10;jacket | 夹克'></textarea>"
    "<p class=hint>已有的词会自动跳过；毕业很久的旧卡会给新词腾位置。</p>"
    "<label>家里 Wi-Fi 名称（选填，以后下载发音用）</label>"
    "<input name=ssid autocomplete=off>"
    "<label>Wi-Fi 密码</label>"
    "<input name=pass type=password autocomplete=off>"
    "<button type=submit>存进盒子</button></form>"
    "<p class=hint>保存后设备会显示结果。完成后在设备上长按 OK 退出换词模式。</p>"
    "</body></html>";

static esp_err_t send_page(httpd_req_t *req, const char *middle) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, PAGE_HEAD, HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, middle, HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK) err = httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t handle_root(httpd_req_t *req) {
    return send_page(req, PAGE_FORM);
}

static void html_escape(char *dst, size_t dst_size, const char *src) {
    size_t w = 0;
    for (; *src && w + 6 < dst_size; src++) {
        switch (*src) {
        case '<': memcpy(dst + w, "&lt;", 4); w += 4; break;
        case '>': memcpy(dst + w, "&gt;", 4); w += 4; break;
        case '&': memcpy(dst + w, "&amp;", 5); w += 5; break;
        case '\n': memcpy(dst + w, "<br>", 4); w += 4; break;
        default: dst[w++] = *src; break;
        }
    }
    dst[w] = '\0';
}

static void sta_connect_task(void *arg) {
    (void)arg;
    // 先让回复页发到手机,再切信道:APSTA 下热点会跟随家里路由器的信道。
    vTaskDelay(pdMS_TO_TICKS(1500));
    char ssid[WB_SSID_MAX], pass[WB_PASS_MAX];
    if (s_wifi_started && wb_store_get_wifi(ssid, pass) == ESP_OK && ssid[0]) {
        wifi_config_t sta = {0};
        strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
        strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
        s_has_home = true;
        s_sta_retries = 0;
        esp_wifi_disconnect();
        if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK &&
            esp_wifi_set_config(WIFI_IF_STA, &sta) == ESP_OK) {
            notify(WB_PORTAL_EV_HOME_CONNECTING);
            esp_wifi_connect();
        } else {
            notify(WB_PORTAL_EV_HOME_FAILED);
        }
    }
    vTaskDelete(NULL);
}

static esp_err_t handle_save(httpd_req_t *req) {
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too large");
        return ESP_FAIL;
    }
    char *body = malloc(req->content_len + 1);
    char *reply = malloc(REPLY_SIZE);
    char *escaped = malloc(REPLY_SIZE * 2);
    char *page = malloc(REPLY_SIZE * 2 + 200);
    if (!body || !reply || !escaped || !page) {
        free(body); free(reply); free(escaped); free(page);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    body[got] = '\0';

    reply[0] = '\0';
    bool new_wifi = false;
    if (got == req->content_len && s_on_submit) {
        new_wifi = s_on_submit(body, got, reply, REPLY_SIZE, s_user);
    } else {
        snprintf(reply, REPLY_SIZE, "没收全，请再提交一次。");
    }
    html_escape(escaped, REPLY_SIZE * 2, reply);
    snprintf(page, REPLY_SIZE * 2 + 200,
             "<h1>保存好了</h1><div class=box>%s</div>"
             "<p><a href=/>再加一批</a></p></body></html>", escaped);
    esp_err_t err = send_page(req, page);
    free(body); free(reply); free(escaped); free(page);

    notify(WB_PORTAL_EV_SUBMITTED);
    if (new_wifi) {
        xTaskCreate(sta_connect_task, "wb_sta", 3072, NULL, 4, NULL);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)data;
    switch (id) {
    case WIFI_EVENT_AP_STACONNECTED:
        notify(WB_PORTAL_EV_PHONE_JOINED);
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        notify(WB_PORTAL_EV_PHONE_LEFT);
        break;
    case WIFI_EVENT_STA_START:
        if (s_has_home) {
            notify(WB_PORTAL_EV_HOME_CONNECTING);
            esp_wifi_connect();
        }
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        if (!s_has_home) break;
        if (s_sta_retries < STA_RETRIES) {
            s_sta_retries++;
            esp_wifi_connect();
        } else {
            notify(WB_PORTAL_EV_HOME_FAILED);
        }
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)data;
    if (id == IP_EVENT_STA_GOT_IP) {
        s_sta_retries = 0;
        notify(WB_PORTAL_EV_HOME_CONNECTED);
    }
}

static esp_err_t net_prepare(void) {
    if (s_net_ready) return ESP_OK;
    // 默认 nvs 分区由 Wi-Fi 驱动内部使用(校准数据等)。
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_net_ready = true;
    return ESP_OK;
}

const char *wb_portal_ap_ssid(void) {
    return s_ap_ssid;
}

esp_err_t wb_portal_start(wb_portal_submit_cb_t on_submit, wb_portal_status_cb_t on_status,
                          void *user) {
    if (s_wifi_inited) return ESP_ERR_INVALID_STATE;
    s_on_submit = on_submit;
    s_on_status = on_status;
    s_user = user;

    esp_err_t err = net_prepare();
    if (err != ESP_OK) goto fail;

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_ap_netif || !s_sta_netif) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) goto fail;
    s_wifi_inited = true;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event,
                                              NULL, &s_wifi_handler);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event,
                                                  NULL, &s_ip_handler);
    }
    if (err != ESP_OK) goto fail;

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "WordBox-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    strncpy((char *)ap.ap.password, WB_PORTAL_PASS, sizeof(ap.ap.password));
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    char ssid[WB_SSID_MAX], pass[WB_PASS_MAX];
    s_has_home = wb_store_get_wifi(ssid, pass) == ESP_OK && ssid[0];
    s_sta_retries = 0;

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err == ESP_OK && s_has_home) {
        wifi_config_t sta = {0};
        strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
        strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
        err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    }
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) goto fail;
    s_wifi_started = true;

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.max_open_sockets = 3;
    hcfg.stack_size = 6144;
    hcfg.lru_purge_enable = true;
    err = httpd_start(&s_httpd, &hcfg);
    if (err != ESP_OK) goto fail;
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = handle_root };
    const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = handle_save };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);

    ESP_LOGI(TAG, "换词热点 %s 已开启", s_ap_ssid);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "换词模式启动失败: %s", esp_err_to_name(err));
    wb_portal_stop();
    return err;
}

esp_err_t wb_portal_stop(void) {
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    s_has_home = false;
    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
    }
    if (s_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler = NULL;
    }
    if (s_wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    if (s_wifi_inited) {
        esp_wifi_deinit();
        s_wifi_inited = false;
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    s_on_submit = NULL;
    s_on_status = NULL;
    return ESP_OK;
}
