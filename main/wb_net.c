// main/wb_net.c —— 见 wb_net.h。
#include "wb_net.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "wb_store.h"

static const char *TAG = "wb_net";

#define BIT_GOT_IP  BIT0
#define BIT_FAILED  BIT1
#define STA_RETRIES 3

static bool s_prepared;
static bool s_up;
static esp_netif_t *s_netif;
static EventGroupHandle_t s_events;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static int s_retries;
static bool s_time_ok;

esp_err_t wb_net_prepare(void) {
    if (s_prepared) return ESP_OK;
    // 默认 nvs 分区由 Wi-Fi 驱动内部使用(校准数据等)。
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) return err;
    err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    setenv("TZ", "CST-8", 1);
    tzset();
    s_prepared = true;
    return ESP_OK;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)data;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries < STA_RETRIES) {
            s_retries++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, BIT_FAILED);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)data;
    if (id == IP_EVENT_STA_GOT_IP) {
        s_retries = 0;
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

esp_err_t wb_net_connect(uint32_t timeout_ms) {
    if (s_up) return ESP_OK;
    char ssid[WB_SSID_MAX], pass[WB_PASS_MAX];
    if (wb_store_get_wifi(ssid, pass) != ESP_OK || !ssid[0]) return ESP_ERR_NOT_FOUND;

    esp_err_t err = wb_net_prepare();
    if (err != ESP_OK) return err;
    if (!s_events) s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAILED);
    s_retries = 0;
    s_up = true;   // 从这里起失败都走 wb_net_disconnect 清理

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event,
                                              NULL, &s_wifi_handler);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event,
                                                  NULL, &s_ip_handler);
    }
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, ssid, sizeof(sta.sta.ssid));
    strncpy((char *)sta.sta.password, pass, sizeof(sta.sta.password));
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) goto fail;

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP | BIT_FAILED, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & BIT_GOT_IP) {
        ESP_LOGI(TAG, "已连上 %s", ssid);
        return ESP_OK;
    }
    err = (bits & BIT_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT;

fail:
    ESP_LOGW(TAG, "连不上家里 Wi-Fi: %s", esp_err_to_name(err));
    wb_net_disconnect();
    return err;
}

void wb_net_disconnect(void) {
    if (!s_up) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (s_ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_handler);
        s_ip_handler = NULL;
    }
    if (s_wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_handler);
        s_wifi_handler = NULL;
    }
    esp_wifi_deinit();
    if (s_netif) {
        esp_netif_destroy_default_wifi(s_netif);
        s_netif = NULL;
    }
    s_up = false;
}

esp_err_t wb_net_get(const char *url, uint8_t *buf, size_t cap, size_t *len_out) {
    *len_out = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        if (status != 200) {
            ESP_LOGW(TAG, "%s -> HTTP %d", url, status);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    size_t got = 0;
    while (err == ESP_OK) {
        if (got == cap) {
            // 缓冲满了还有数据就是文件太大。
            char probe;
            if (esp_http_client_read(c, &probe, 1) > 0) err = ESP_ERR_INVALID_SIZE;
            break;
        }
        int r = esp_http_client_read(c, (char *)buf + got, (int)(cap - got));
        if (r < 0) {
            err = ESP_FAIL;
        } else if (r == 0) {
            if (esp_http_client_is_complete_data_received(c)) break;
            err = ESP_ERR_TIMEOUT;
        } else {
            got += (size_t)r;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (err == ESP_OK) *len_out = got;
    return err;
}

static bool read_clock(uint32_t *date_out, uint8_t *wday_out) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2025) return false;
    if (date_out) {
        *date_out = (uint32_t)(t.tm_year + 1900) * 10000u + (uint32_t)(t.tm_mon + 1) * 100u +
                    (uint32_t)t.tm_mday;
    }
    if (wday_out) *wday_out = (uint8_t)t.tm_wday;
    return true;
}

bool wb_net_sync_time(uint32_t timeout_ms, uint32_t *date_out, uint8_t *wday_out) {
    if (!s_up) return false;
    // 国内优先用阿里云的 NTP,再退到 pool.ntp.org。
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
    if (esp_netif_sntp_init(&cfg) != ESP_OK) return false;
    bool ok = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) == ESP_OK;
    esp_netif_sntp_deinit();
    if (ok) ok = read_clock(date_out, wday_out);
    if (ok) s_time_ok = true;
    ESP_LOGI(TAG, "对时%s", ok ? "成功" : "失败");
    return ok;
}

bool wb_net_today(uint32_t *date_out, uint8_t *wday_out) {
    return s_time_ok && read_clock(date_out, wday_out);
}
