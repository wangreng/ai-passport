// main/wb_portal.h —— 换词模式:设备开热点 + 本地网页,手机粘词表、填家里 Wi-Fi。
//
// 热点名 WordBox-XXXX(MAC 后四位),密码 WB_PORTAL_PASS,网页 http://192.168.4.1/ 。
// 存过家里 Wi-Fi 时同时以 STA 去连(APSTA),连接结果经 status 回调报告。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define WB_PORTAL_PASS "ket12345"
#define WB_PORTAL_IP   "192.168.4.1"

typedef enum {
    WB_PORTAL_EV_PHONE_JOINED = 0,  // 有手机连上热点
    WB_PORTAL_EV_PHONE_LEFT,
    WB_PORTAL_EV_HOME_CONNECTING,
    WB_PORTAL_EV_HOME_CONNECTED,    // 连上家里 Wi-Fi 并拿到 IP
    WB_PORTAL_EV_HOME_FAILED,
    WB_PORTAL_EV_SUBMITTED,         // 网页提交已处理完
} wb_portal_event_t;

// 运行在 Wi-Fi 事件任务或 httpd 任务里:只能入队或做同等级的轻操作。
typedef void (*wb_portal_status_cb_t)(wb_portal_event_t ev, void *user);

// 网页提交时在 httpd 任务里调用。body 是原始 urlencoded 表单(已 NUL 结尾)。
// 往 reply 写一段给家长看的结果(纯文本,UTF-8)。返回 true 表示提交里有新的
// Wi-Fi 凭据,portal 会在回复发出后用新凭据重连。
typedef bool (*wb_portal_submit_cb_t)(const char *body, size_t len,
                                      char *reply, size_t reply_size, void *user);

esp_err_t wb_portal_start(wb_portal_submit_cb_t on_submit, wb_portal_status_cb_t on_status,
                          void *user);
// 停止网页和 Wi-Fi,释放资源。未启动时返回 ESP_OK。
esp_err_t wb_portal_stop(void);

// 热点名,wb_portal_start 成功后有效。
const char *wb_portal_ap_ssid(void);
