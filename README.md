# thread-ui —— ESP32-S31 电动螺丝刀机身 AMOLED 界面 / ESP-IDF

当前硬件：**华夏彩 H0160S003AM，1.6" 228×460 AMOLED，驱动 CO5300，QSPI**（Korvo-1 + 杜邦线验证）。
当前界面：**QuantDriver UI v1.4.5**（整屏 228×460，四模式拧紧演示），约 **30 fps，TE 同步无撕裂**。

旧的 120×240 SH8501A 螺纹界面仍保留在代码里，`app_main.c` 里 `UI_QUANTDRIVER 0` 即切回。

```
thread-ui/
├── main/
│   ├── display.c/.h      CO5300 QSPI 驱动：上电时序、GRAM 偏移、整帧推送、TE 同步、面板掉线恢复、偏移校准模式
│   ├── qd_app.c/.h       新 UI 帧循环：整帧缓冲、双核渲染、TE 推屏流水线、BOOT 键、串口调试命令
│   ├── qd/               新 UI 渲染与状态机（来自 QuantDriver 点亮包，见下）
│   │   ├── ui_render.c/.h   228×460 渲染器（转子、槽格带景深、文字），已拆成可双核并行的分步接口
│   │   ├── demo_state.c/.h  演示状态机（转速改为按时间线性，见「转子转速约定」）
│   │   └── gen/             转子查表、字模（工具生成，勿手改）
│   ├── app_main.c        入口；UI_QUANTDRIVER=0 时跑旧 120×240 UI（主循环、三模式、棘轮咬合）
│   ├── thread_anim.c/.h  旧 UI：贴图展开 + 调色板 + 分数行混合
│   ├── input_wheel.c/.h  PCNT 编码器 + 短按/双击/长按判定
│   ├── ui_widgets.c/.h   旧 UI：状态条/压条 + 3x5 抗锯齿文本
│   ├── menu.c/.h         旧 UI：两级设置菜单
│   └── knob_uart.c/.h    触觉旋钮板 UART 客户端
├── components/esp_lcd_sh8501/  旧屏 SH8501 组件（旧 UI 用）
├── docs/
│   ├── quantdriver/      新 UI 规格（SPEC.md）与实现说明（IMPLEMENTATION.md）
│   ├── SPEC.md / AGENT_PROMPT.md / roadmap.md   旧 UI 文档
├── prototypes/
│   ├── screwdriver-ui-v1.4.5.html   新 UI HTML 原型（渲染的唯一参照）
│   └── …                            旧 UI 原型
├── tools/sim_ui.py       旧 UI 的 PC 模拟器
└── sdkconfig.defaults
```

## 构建

- 目标 **esp32s31**，ESP-IDF **master**（v6.0.x 不含 S31，编不过）。本机 `idf-build.bat` 已封装环境：
  ```powershell
  .\idf-build.bat -p COM3 build flash
  ```
- Flash 用 **QIO**（`sdkconfig.defaults` 已设）：新 UI 有 116 KB 查找表在 Flash 里每帧扫一遍，DIO 会拖慢合成。

## 接线（Korvo-1 ↔ sunvay FPC-DIP ADAPTER(03) ↔ 屏 31P FPC）

转接板排针第 c 列（FPC 座朝上、从左数）：**下排 = FPC 脚 2c−1，上排 = 2c**。屏 FPC 第 N 脚 = 转接板第 N 脚。

| FPC 脚 | 信号 | 转接板 | 接到 | 备注 |
|:-:|---|---|---|---|
| 1, 7, 12, 22 | GND | 1列下、4列下、6列上、11列上 | GND | 都接，VBAT 回流走 12 脚 |
| 8 | IOVCC | 4列上 | 3V3 | |
| 9 | VCI | 5列下 | 3V3 | |
| 10 | **VBAT** | 5列上 | **见下「VBAT 供电」** | 规格 3.7~4.2 V |
| 11 | VCI_EN | 6列下 | 3V3 | PMIC 使能 |
| 13 | MTP PWR | 7列下 | **悬空** | 8 V 烧录脚，绝不能接 |
| 14 | RESET | 7列上 | GPIO8 | |
| 15 | CSX | 8列下 | GPIO10 | |
| 16 | SDI/RDX = **D0** | 8列上 | GPIO11 | |
| 17 | DCX = **D1** | 9列下 | GPIO13 | |
| 18 | WRX/SCL = **CLK** | 9列上 | GPIO12 | 旁边并一根 GND |
| 19 | D(1) = **D3** | 10列下 | GPIO9 | 屏上 D(1) 是第 4 根数据线 |
| 20 | D(0) = **D2** | 10列上 | GPIO14 | 屏上 D(0) 是第 3 根数据线 |
| 21 | TE | 11列下 | GPIO18 | 帧同步，开机自检数脉冲 |
| 2~6, 23~31 | TP / MIPI | — | 不接 | |

- GPIO 9~14 是 S31 SPI2 的八线 IO_MUX 组，QSPI 目前跑 **40 MHz**（杜邦线实测可用）。
- 其他已占用：编码器 15/16/17，旋钮板 UART 33/34，控制台 58/59。避开 strapping 36~40、60/61 与 Flash 26~32。

## VBAT 供电（重要，踩过的坑）

| 现象 | 原因 |
|---|---|
| 正点原子数控电源接 VBAT 报 **REP**、不出电 | 模组从 VCI/IOVCC 往 VBAT 漏 0.15~0.3 V，电源开输出时判为反灌 |
| VBAT 接 3V3 能亮，但亮画面（TURBO 红）时**偶发黑屏且不恢复** | 3.3 V 低于规格（3.7~4.2 V），大电流时 PMIC 欠压锁定（UVLO），只有复位能拉起 |

黑屏时驱动 IC 仍在出 TE、帧缓冲内容正常（日志 `TE miss 0`、`lit ≈70%`），**主控察觉不到**；串口发 `r` 重新初始化即恢复，证实是 PMIC 锁死。

参考：osptek / Waveshare 的 CO5300 方案 VBAT 直接接 3.3 V，是因为**它们的 PMIC 支持 3.3~5.5 V**；本模组不行。

**正确做法**（任选，均需拔掉 VBAT 接 3V3 的线，并在 VBAT 加 10 µF + 100 nF）：

| 方案 | VBAT | 说明 |
|---|---|---|
| **5 V → LDO 4.0 V**（推荐） | 4.0 V 稳定 | 低压差、≥300 mA，可调型设 4.0 V |
| 5 V → 1 颗硅二极管（1N4007/1N4148） | 4.1~4.4 V | 临时验证用；**不要用肖特基**（≈4.7 V 超限） |
| 数控电源 → 肖特基 → VBAT，电源设 4.2 V | ≈3.9 V | 肖特基只为挡住漏电压、避免 REP |

VBAT 未改好前，亮度临时降到 50%（`QD_BRIGHTNESS 128`，自检亮度 0x80）；改好后可回 80%（204）。

## 屏参数（实测）

- GRAM 偏移 **X=36、Y=0**（`display.c` 的 `X_OFFSET/Y_OFFSET`）。颜色顺序 RGB 正确。
- 刷新窗口起点、宽高需为偶数。
- **寄存器读不出**：QSPI 读 0x04/0xDA/0x0A 恒为 0（模组的 SDO 应未引出）。不要用读 ID 判断通信，用 TE 或画面。
- 初始化序列是 CO5300 通用最小序列（规格书第 8 节空白）：`FE 00 / C4 80 / 3A 55 / 35 00 / 36 00 / 53 20 / 63 FF / 51 00 / 11 / 写图 / 29`。**仍需向模组厂要正式 init code**（gamma/电压）。

### 换屏或换批次时重新校准偏移

`display.c` 里 `CALIB_OFFSET 1` 后烧录：开机进入校准图（白框 + 四角色块），UI 不启动。
串口按键实时移动、不用重启：`a/d` 左右 ±2、`A/D` ±10，`w/s` 上下 ±2、`W/S` ±10。
调到四边白框贴边、右侧无杂纹后，把打印的值填回 `X_OFFSET/Y_OFFSET` 并改回 0。

## 新 UI：QuantDriver v1.4.5

渲染与状态机（`main/qd/`）来自 QuantDriver 点亮包，**画面与 HTML 原型逐帧一致**；推屏、引脚、偏移、时序以本工程为准。
目前是自动演示：四模式（精密 / 标准 / 棘轮 / TURBO）轮流「拧紧 → 到位 → 泄能 → 松出」。BOOT 键（GPIO61）短按换模式，长按切换「减少动态」。旋钮/编码器尚未接入 `qd_state`。

### 帧流水线

```
渲染任务（core1，辅助核 core0 分担）：begin(N+1) → 取 fb_free → rows(N+1) 写整帧缓冲 → 给 frame_ready → …
推屏任务（core1，高优先级）      ：取 frame_ready → 等 TE → 推整帧(10.5 ms@40MHz) → DMA 领先 3~4 ms 后放回 fb_free
```

- **整帧缓冲** 205 KB（内部 SRAM），分配失败自动退回条带模式。
- **TE 同步**：从消隐期开始推，整帧推送快于面板扫描 → 无撕裂。TE 不可用时自动跳过。
- **追着 DMA 写**：DMA 读 fb 的速度约是渲染的 2 倍，给它几 ms 领先后渲染就开始写下一帧，省掉等推完的 ~9 ms。
- **双核**：槽格带合成 / 横向模糊 / 纵向模糊、整帧行渲染都按奇偶交错分给两个核。
- **精确的除法替换**：景深 `/K_SUM` 换成 64 位倒数乘法（已穷举验证与整数除法逐值相同）；模糊用对称核减半乘法。

### 性能（实测，串口每 2 s 打印）

| 模式 | fps | begin | rows |
|---|---|---|---|
| 精密 / 标准 / 棘轮 | 29.9~30.0 | ~10.7 ms | ~19.2 ms |
| TURBO | 28.9~29.5 | ~11.4 ms | ~22.3 ms（发光大数字混合） |

日志字段：`TE miss`（等不到 TE 的次数，面板复位时持续增长并触发自动恢复）、`lit`（帧缓冲非黑像素占比，诊断黑屏是画面还是面板问题）。

### 转子转速约定（替代原 SPEC §3.4）

按时间定义、与 rpm **严格线性**：`rate = 7.2 × rpm/900 槽距/秒`（精密 1.6 · 标准 3.6 · TURBO 7.2），每帧只保留防频闪上限 0.45 槽距；「减少动态」上限 3.0 槽距/秒。
原实现按 60 fps 标定的每帧封顶 0.12 在 30 fps 下把标准与 TURBO 都截成同速。

### 串口调试命令（qd_app.c）

- `r`：重新复位并初始化面板（PMIC 锁死时可手动拉起）

## 旧 UI（120×240，`UI_QUANTDRIVER 0`）

- 主循环 60 fps，三模式、扭矩锁、菜单、自动休眠（`AUTO_SLEEP`，调试期为 0）
- 旧 UI 在新屏上以 120×240 居中显示（原点 54,110）
- 编码器：A=15, B=16, BTN=17；`input_wheel.c` 的 `DETENT_PULSES` 按实际轮子改
- PC 模拟器 `tools/sim_ui.py`：`python tools/sim_ui.py click scroll scroll long`，输出到 `sim_out/`

## 旋钮板 UART（knob_uart.c）

- UART1，115200 8N1；S31 TX=GPIO33 → 旋钮板 RX(GPIO16)，S31 RX=GPIO34 ← 旋钮板 TX(GPIO17)，共地
- 上电自动 `magic\n` 解锁（收到 `OK MAGIC`），之后每 100 ms 轮询 `status\n`
- STAT 应答的 `pos` 增量自动注入为 UI 滚动事件；控制命令 `knob_send("wheelvp 1.0")`

## 已知问题 / 待办

- [ ] **VBAT 改 LDO 4.0 V**（见上），之后亮度回 80%
- [ ] 顶部偶发一条细花线：已把 DMA 领先加到 3~4 ms 观察；若仍有，降 QSPI 到 26.7 MHz
- [ ] 60 fps：需再降 ~45% 渲染量（转子着色查表化、静态行跳过、PIE SIMD）
- [ ] 按画面平均亮度（APL）限流，降低峰值电流、功耗与烧屏
- [ ] 旋钮/编码器接入新 UI 状态机
- [ ] 向模组厂要正式 init code
- **烧屏**：状态条与大数字位置固定，演示勿整夜高亮
- **帧缓冲不要放 PSRAM**，`disp_alloc()` 强制内部 SRAM
