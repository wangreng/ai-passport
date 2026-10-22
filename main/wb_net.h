// main/wb_net.h —— 连家里 Wi-Fi(只做 STA),HTTPS 下载,SNTP 对北京时间。
//
// 和换词热点(wb_portal)互斥:同一时间只能开一个,由 wb_app 保证。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// nvs_flash / esp_netif / 默认事件循环,幂等。wb_portal 也用它。
esp_err_t wb_net_prepare(void);

// 用存好的家里 Wi-Fi 连接并等到拿到 IP。没存过返回 ESP_ERR_NOT_FOUND,
// 超时或密码不对返回 ESP_ERR_TIMEOUT / ESP_FAIL。失败时已自行断开。
esp_err_t wb_net_connect(uint32_t timeout_ms);

// 断开并释放 Wi-Fi。未连接时返回 ESP_OK。
void wb_net_disconnect(void);

// 把 url 的内容下载到 buf(最多 cap 字节)。HTTP 非 200 返回 ESP_ERR_INVALID_RESPONSE,
// 内容超过 cap 返回 ESP_ERR_INVALID_SIZE。
esp_err_t wb_net_get(const char *url, uint8_t *buf, size_t cap, size_t *len_out);

// 已连网时对时(北京时间)。成功返回 true,并给出 YYYYMMDD 和星期(0=周日)。
bool wb_net_sync_time(uint32_t timeout_ms, uint32_t *date_out, uint8_t *wday_out);

// 本次开机里对过时就返回 true,并给出当前日期和星期。
bool wb_net_today(uint32_t *date_out, uint8_t *wday_out);
