// main/main.c —— 单词盒子(KET 背词)固件入口:BSP 初始化 + 按键转发给应用任务。
//
// 应用逻辑、界面与存储在 wb_*.c;本文件只负责启动顺序和按键回调。
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "key_guard.h"
#include "wb_app.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"

static const char *TAG = "main";

static volatile bool s_input_ready;

// 深睡按键唤醒时,那颗键在整个启动期间还按着,按键组件会把它判成长按。
// 守卫在它松手前吞掉按键动作;状态机见 key_guard.h。
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// button callbacks run on the shared esp_timer task; enqueue only and return immediately.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // Consume the guard before the readiness check: a release can arrive after the
    // post-init voltage sample but before input dispatch is ready, and dropping it
    // here would leave the guard armed to swallow the next real press.
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        return;
    }
    if (!s_input_ready) return;
    wb_app_post_key(btn, ev);
}

void app_main(void) {
    ESP_LOGI(TAG, "单词盒子启动");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
    }

    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败。检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用,右上角不显示电量");
    }

    esp_err_t err = wb_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "应用启动失败: %s", esp_err_to_name(err));
        return;
    }

    err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(err));
        return;
    }
    if (s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }
    s_input_ready = true;
}
