# 固件实现说明

读者：要改这份固件的嵌入式工程师或编码 agent。界面规格见 `SPEC.md`（v1.4.5），点亮步骤见 `BRINGUP_ESP32S31.md`。

## 1. 文件

```
firmware/
├── CMakeLists.txt            ESP-IDF 工程（idf.py --preview ...）
├── sdkconfig.defaults        320 MHz、-O2、1000 Hz tick、SPI ISR 进 IRAM、大应用分区
└── main/
    ├── board_config.h        引脚、时钟、偏移、运行模式 —— 唯一需要按板子改的文件
    ├── main.c                任务、帧循环、双核分工、按键、测试图、偏移探测
    ├── lcd_panel.c/.h        CO5300 QSPI：上电、初始化、异步推屏、TE、亮度
    ├── ui_render.c/.h        渲染器（纯 C，无 IDF 依赖）
    ├── demo_state.c/.h       演示状态机（纯 C，无 IDF 依赖，从原型逐行移植）
    ├── idf_component.yml     依赖 espressif/esp_lcd_co5300 ^2.2.0
    └── gen/
        ├── rotor_lut.h       tools/gen_luts.py 生成：相机/转子/槽格查表
        └── ui_font.h         tools/gen_font.py 生成：文字 alpha 位图
tools/
├── gen_luts.py               只用 Python 标准库
├── gen_font.py               Pillow + numpy；字体路径在文件开头（默认 Linux 路径，Mac/Windows 改成本机字体）
├── host_sim/                 主机模拟器：同一份 ui_render.c / demo_state.c，输出 PPM
└── compare_with_prototype.py 原型 vs 固件逐帧对照（Playwright）
```

`ui_render.c` 和 `demo_state.c` 不依赖 ESP-IDF，所以界面问题可以全部在电脑上用 `host_sim` 复现和修，不用每次烧录。

## 2. 帧循环

```
demo_task（核 1）                                   helper_task（核 0）
 ├ dt = 真实经过时间（上限 0.1 s）
 ├ 按键 / 自动换模式
 ├ qd_demo_tick → qd_state_step → qd_cells_prep     （状态机，double，每帧一次）
 ├ ui_frame_begin
 │   ├ 每帧常量：转子相位、发热、TURBO 系数、15 格的颜色/亮度/光晕系数
 │   ├ 文字绘制列表（≤ 64 个操作）
 │   └ 槽格带 73 行：转子着色 → 合成 → 2D 景深   （单核，约占每帧 15%）
 ├ [有 TE] 等 TE
 └ 10 条带 × 46 行：
     ├ 等这块缓冲传完（两块轮转）
     ├ 上 22 行自己渲染 ─────────────────────┐   下 24 行 ← 信号量
     ├ 等 helper 完成 ←──────────────────────┘
     └ esp_lcd_panel_draw_bitmap（DMA 异步） → 下一条带开始渲染时它在后台传输
```

esp_lcd 的 SPI 面板 IO 在发下一条命令（CASET/RASET）前会等上一次颜色传输结束，所以"渲染第 N+1 条"与"传输第 N 条"天然重叠，不需要额外同步。

## 3. 每像素做了什么

**转子**（窗口 y 90–336，与原型 `shadeRotor()` 对应）：

```
phase = A[x] + sy·B[x] − θ                    Q16，1.0 = 一个槽距；A/B 只跟列有关
prof  = PROFILE[景深档位[x]][phase 高 8 位]    r,g,b,齿面权重,槽底辉光（8 档预模糊）
wire  = 横纹相位第 15 位 ? 0.92 : 1.0
色    = prof.rgb × (齿面 ? wire × 发热 : 1) × 亮度[x] + 模式色 × 辉光 × 负载 + 高光[x]
TURBO：r×(1+.1h) g×(1−.3h) b×(1−.22h) + 槽缝余烬
× 窗口渐隐[y] × 两侧暗边[x] + 玻璃反光
```

全是整数乘加和查表，没有三角函数、没有除法。

**槽格带**（y 270–342 合成，277–335 输出）：每像素查 `BAND_PX` 得到"属于第几格 / 唇光强度 / 是否峰值框描边 / 到相邻三格的距离"，然后按 15 格的当前亮度合成（样式 A），最后按列做三角核 2D 景深（与原型 `blurInto()` 同核）。

**文字**：固定字符串整串预渲染（字距、对齐在 `gen_font.py` 里按原型 `measureRuns()` 算好），数字逐字贴图（等宽数字）。每行遍历绘制列表做 alpha 混合。TURBO 的前倾直接烘焙进字模，辉光是单独的模糊 alpha 图。

**输出**：RGB565，高字节在前（SPI 先发高字节），直接给 DMA。

## 4. 与原型的对应关系

| 原型（HTML） | 固件 |
|---|---|
| `step()` / `ratchetStep()` | `qd_state_step()` / `ratchet_step()` |
| `prepCells()` | `qd_cells_prep()` |
| `shadeRotor()` | `rotor_px()` + `ROT_PROFILE` |
| `composite()`（样式 A） | `composite()` |
| `blurInto()` | `render_band()` 的两次卷积 |
| `base()` / `statusBar()` | `build_ops()` |
| `frame()` / `corners()` | `ui_render_rows()` 第 2、4 步 |
| 自动演示 6.5 s 周期 | `qd_demo_tick()` |

改行为时两边一起改，然后跑 `tools/compare_with_prototype.py`。

## 5. 后续可做的优化（按收益排序）

1. **PIE SIMD**：S31 一个核带 128 位 SIMD。`rotor_px()` 的相位乘加、移位、查表地址计算适合 8 像素一组并行；需要用乐鑫的 PIE 内联汇编或 esp-dsp 风格的手写汇编
2. **整帧缓冲 + TE 整帧推送**（消除撕裂）：205 KB 帧缓冲放 PSRAM（S31 的 PSRAM 支持 DMA），渲染全部完成后在 TE 到来时一次推完（40 MHz 约 10.5 ms，80 MHz 约 5.2 ms，都小于 16.7 ms 的刷新周期）
3. **只重绘变化的文字区**：状态条和标签每帧都一样，可以跳过（转子窗口和槽格带每帧都在变，不能跳）
4. **QSPI 80 MHz**：需要 IOMUX 引脚和短线，实测稳定再用
