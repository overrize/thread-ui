#pragma once
#include "esp_err.h"

/* 启动 QuantDriver 整屏 UI（需先 disp_init）。创建渲染任务后立即返回 */
esp_err_t qd_app_start(void);
