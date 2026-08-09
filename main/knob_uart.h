#pragma once
#include <stdbool.h>

/* STAT 应答解析结果：STAT mode=<int>,m0=<rad>,vel=<rad/s>,pos=<int>,sub=<float>,wv=<rad/s>,uq0=<V> */
typedef struct {
    int   mode;
    float m0;
    float vel;
    int   pos;
    float sub;
    float wv;
    float uq0;
    bool  valid;
} knob_stat_t;

/* 启动 UART1 + 通信任务（自动 magic 解锁、切 mode 1 码盘、监听 KNOB 主动上报） */
void knob_init(void);

/* 发送一条普通命令（自动附加 \n）。仅在解锁后有效，返回是否已发出。
 * 注意：Serial2 只接受 magic/ping/lock/status/0-4，调参命令不要走这里 */
bool knob_send(const char *cmd);

/* 切换旋钮板模式（0 保护/下电，1 码盘/棘轮，2 回中拨轮，3 虚拟飞轮，4 速度测试） */
bool knob_set_mode(int mode);

/* 触发旋钮板震动反馈（CLICK 预设），gain 0..2，按键点击推荐 0.8..1.0 */
bool knob_vib(float gain);

bool knob_is_unlocked(void);

/* 最近一次 status 结果（valid=false 表示还没拿到过） */
void knob_get_status(knob_stat_t *out);

/* UI 主循环调用：取走累积的 KNOB value 增量（0 = 无） */
int knob_take_pos_delta(void);
