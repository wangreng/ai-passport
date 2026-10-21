// main/wb_audio.h —— 播放发音槽里的单词 / 词块。后台任务播放,新的请求打断旧的。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t wb_audio_start(void);

// 播放某个槽的单词(chunk=false)或词块。没发音的槽静默忽略。不阻塞。
void wb_audio_play(uint8_t slot, bool chunk);

// 停掉正在播的声音。不阻塞。
void wb_audio_stop(void);
