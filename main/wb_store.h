// main/wb_store.h —— 单词盒子的持久化:卡片、进度、家庭 Wi-Fi 凭据。
//
// 存在独立的 wbdata NVS 分区(见 partitions.csv)。只烧 app 更新固件时进度保留;
// 烧合并镜像 full.bin 会把它清空。
// 所有函数都会碰 Flash,只能在应用任务里调,不能在按键回调或 LVGL 任务里调。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "wb_model.h"

#define WB_SSID_MAX 33
#define WB_PASS_MAX 65

esp_err_t wb_store_init(void);

// 读不到(全新设备)时给出空卡组并返回 ESP_OK。
esp_err_t wb_store_load(wb_deck_t *deck);

esp_err_t wb_store_save_meta(const wb_deck_t *deck);
esp_err_t wb_store_save_card(const wb_deck_t *deck, uint16_t idx);
// 合并新词后卡片下标会变,整副重写并删掉多余的旧条目。
esp_err_t wb_store_save_all(const wb_deck_t *deck);

// 没存过返回 ESP_ERR_NVS_NOT_FOUND,ssid/pass 置空。
esp_err_t wb_store_get_wifi(char ssid[WB_SSID_MAX], char pass[WB_PASS_MAX]);
esp_err_t wb_store_set_wifi(const char *ssid, const char *pass);
