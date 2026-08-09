# thread-ui —— 120×240 AMOLED (SH8501A) 螺纹界面 / ESP-IDF

```
thread-ui/
├── main/
│   ├── display.c/.h      esp_lcd panel_io + esp_lcd_sh8501 厂商组件，4 线 SPI @40MHz
│   ├── thread_anim.c/.h  贴图展开 + 调色板 + 分数行混合 + 步进
│   ├── input_wheel.c/.h  PCNT 编码器 + 短按/双击/长按判定
│   ├── ui_widgets.c/.h   状态条/压条绘制 + 3x5 抗锯齿文本
│   ├── menu.c/.h         两级设置菜单（列表/编辑/回滚/确认条）
│   ├── knob_uart.c/.h    触觉旋钮板 UART 客户端（magic 解锁 + status 轮询）
│   ├── app_main.c        60fps 主循环、三模式、棘轮咬合
│   └── thread_tile.h     贴图（占位，用 tools 导出后替换）
├── components/esp_lcd_sh8501/  SH8501 面板组件（含 120x240 玻璃厂商初始化表）
├── tools/export_tile.html  渲染贴图 → 直接吐 C 数组
├── tools/sim_ui.py       PC 端渲染/交互模拟器（见下）
├── docs/
│   ├── SPEC.md           实现规格（唯一事实来源）
│   ├── AGENT_PROMPT.md   交给编码 agent 的开发提示
│   └── roadmap.md        背景、硬件调研、资源
├── prototypes/           三个 HTML 原型，设计的可执行参考
└── sdkconfig.defaults
```

## ESP32-S31 移植要点（与原版差异）

- 目标芯片 esp32s31（IDF v6.2，需 `idf.py --preview`），板型 Korvo-1
- SPI 引脚：SCLK=9, MOSI=11, DC=10, CS=12, RST=8（GPIO matrix，SPI2）
- 编码器引脚：A=15, B=16, BTN=17（原 4/5/6 与 Korvo 板载 I2S 冲突）
- 初始化序列用厂商表（components/esp_lcd_sh8501），命令为 32 位帧
- S31 GPSPI 单次事务有长度上限，`max_transfer_sz` 必须 ≤4096，esp_lcd 自动分块
- 窗口偏移实测为 0（X_OFFSET/Y_OFFSET 无需修改）；屏幕底部约 6 行被模组外壳遮挡，
  压条内容已上移避开
- 推屏纪律：每次 blit 只产生一个 DMA 完成信号，每帧只在合成前 wait 一次；
  不要追加第二次 wait（信号量失衡会每帧白等 100ms）

## 旋钮板 UART（knob_uart.c）

- UART1，115200 8N1；S31 TX=GPIO33 → 旋钮板 RX(GPIO16)，S31 RX=GPIO34 ← 旋钮板 TX(GPIO17)，共地
- 上电自动 `magic\n` 解锁（收到 `OK MAGIC`），之后每 100ms 轮询 `status\n`
- STAT 应答的 `pos` 增量自动注入为 UI 滚动事件
- 发控制命令用 `knob_send("wheelvp 1.0")`（解锁后生效）

## PC 模拟器（tools/sim_ui.py）

改布局/交互逻辑后**先在 PC 上验证再烧录**。模拟器直接解析 `main/thread_tile.h`
的贴图数据，字库/抗锯齿/调色板/菜单状态机与固件同一套逻辑。

```powershell
cd E:\ESP\esp32-s3\thread_ui

# 默认演示序列（动画 -> 滚扭矩 -> 换模式 -> 进菜单 -> 编辑 -> 确认条）
python tools/sim_ui.py

# 自定义事件脚本
python tools/sim_ui.py click scroll scroll click scroll -1 long
```

- 事件：`scroll <n>` / `click` / `double` / `long` / `frames <n>`（动画推进 n 帧）
- 每个事件输出一帧 PNG 到 `sim_out/`，文件名带序号和事件名
- 改 C 侧渲染逻辑（ui_widgets.c / thread_anim.c / menu.c）时同步改 sim_ui.py，
  两者保持对拍

## 从 Volos 的做法映射过来

他那套是 Arduino + LilyGo T-Display S3 AMOLED（RM67162，**QSPI**），
TFT_eSPI 的 Sprite 整帧画好再一次推屏。思路照搬，两处必须换：

| Volos / Arduino | 这里 / ESP-IDF |
|---|---|
| `TFT_eSprite spr; spr.createSprite(w,h)` | `disp_alloc()` 拿 DMA 可达内部 SRAM 当帧缓冲 |
| `spr.fillRect / drawString` 逐帧重画 | 贴图行拷贝（螺纹）+ 脏矩形（数字） |
| `lcd_PushColors(x,y,w,h,buf)` | `disp_blit()` = CASET(2Ah) + PASET(2Bh) + RAMWR(2Ch) |
| 厂商库封装好的 QSPI 初始化 | `esp_lcd_new_panel_io_spi()` + 自己的 init 表 |
| 每帧整屏推 | 只推动画区 182 行，状态条/数字变化时才推 |

**不能照搬的**：他那块屏是 QSPI，带宽是你的 4 倍，所以他可以整帧无脑刷。
你这块是单根 SDA，必须分区刷。

## 上手事项（S31 实测结论）

1. ~~向模块卖家要 init code~~ **已解决**：厂商 120×240 初始化表在
   `components/esp_lcd_sh8501`（osptek 来源），命令为 32 位帧，已实测点亮。
2. ~~确认 GRAM 偏移~~ **已实测为 0**：校准尺验证行列完整无偏移。
   但屏幕底部约 6 行被模组外壳遮挡，压条内容不要贴底画。
3. **确认编码器一格几个脉冲。** `input_wheel.c` 的 `DETENT_PULSES` 默认 4，
   按你的轮子实测改，不然一格会跳两条。

## 帧预算（S31 @40MHz SPI，实测）

| 项 | 耗时 | 说明 |
|---|---|---|
| 合成动画区 | <1 ms | 182 行 LUT 查表（分数行混合） |
| 推屏 43.7 KB | ~8.7 ms | DMA 分块（单次 ≤4096B），与下帧合成流水 |
| 数字脏矩形 | ~0.9 ms | 仅数值变化时，5~10Hz |
| **实测** | **62.5 FPS，4.65 ms/帧** | 串口每 2s 打印 |

不够的话切 SPI 1-1-1 调色板模式（COLMOD 3Ah + COLSET 70h~7Fh），
数据量降到 1/4，2.3ms 一帧。贴图本来就是索引存的，改起来只动 `disp_blit`。

## 实测方法

别信估算。在 `thread_render()` 前后翻转一个 GPIO，接逻辑分析仪，
**把合成和推屏分开计时**——瓶颈在 CPU 还是在总线，处理方式完全不同。

## 已知坑

- **撕裂**：模块没引出 TE。IC 有（TEON 35h / TESCAN 44h），改板时务必引出，
  否则满屏动画的撕裂会很明显。
- **倒转**：显示转速超过 ~1800rpm 触发 Nyquist，螺纹看起来反向。
  `thread_advance()` 里已经把步进卡在 0.45×牙距以内。
- **烧屏**：状态条和大数字位置固定。加息屏超时、定期 1~2px 整体偏移、
  用 `disp_brightness()` 降亮度。
- **帧缓冲不要放 PSRAM**，`disp_alloc()` 已经强制内部 SRAM。
- **不要在主循环里读寄存器**，读时钟只有 10MHz，是写的五分之一。
