// main/wb_app.h —— 单词盒子应用:状态机 + 工作任务。
//
// 按键:
//   首页   OK 开始今天   上 Boss 战   下 分拣   长按 OK 换词模式
//   卡片   OK 翻面       上 会了/认识  下 不会/不认识   长按 OK 回首页
//   结算   OK 回首页
//   换词   长按 OK 退出
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// BSP 与 LVGL 初始化完成后调用一次。
esp_err_t wb_app_start(void);

// 从按键回调(esp_timer 任务)调用:只入队,不阻塞。
void wb_app_post_key(bsp_btn_t btn, bsp_btn_ev_t ev);
