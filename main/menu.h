#pragma once
#include <stdbool.h>
#include "input_wheel.h"

/* 打开菜单（从主界面短按进入） */
void menu_open(void);

/* 处理输入事件。返回 true = 菜单仍激活；false = 退出菜单回主界面 */
bool menu_handle(wheel_evt_t e, int d);

/* 有改动时重绘并推屏（每帧调用） */
void menu_draw_if_dirty(void);

/* 设置项取值（菜单编辑的结果） */
int  menu_rpm_max(void);      /* 转速上限 100~900 step 50 */
bool menu_reverse(void);      /* 旋向：true = 反转 */
