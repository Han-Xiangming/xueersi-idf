# 小喵掌机 硬件层与板级配置说明

> 本文档为**实现现状说明**（随代码更新）：`board_config.h` 与 `hardware/` 各驱动的职责、引脚与时序。

## 1. 板级总览

```text
ESP32（WROVER-B，PSRAM 8MB）@ 240MHz
  ├─ ST7735  TFT 160×128（原生 128×160，旋转 90°）  SPI2 @ 60MHz
  ├─ MicroSD 卡（SDSPI，与 LCD 共用 SPI2）          CS=22 @ 10MHz
  ├─ MAX98357 单声道 Class-D DAC                    I2S，无 MCLK
  ├─ 6 键矩阵（低有效）                             上/下/左/右/A/B
  └─ 蓝牙（BT Classic，A2DP Source / AVRCP TG）
```

## 2. 引脚分配（board_config.h）

| 功能 | 引脚 | 说明 |
| ---- | ---- | ---- |
| LCD SCLK | 18 | SPI2 |
| LCD MOSI | 23 | SPI2 |
| LCD MISO | 19 | SPI2（未用） |
| LCD CS | 5 | |
| LCD DC | 4 | |
| SD CS | 22 | SDSPI（SCLK/MOSI/MISO 与 LCD 共用） |
| I2S BCLK | 25 | 帧时钟输出 |
| I2S LRC | 32 | 声道选择 |
| I2S DIN | 33 | PCM 数据 |
| 上 / 下 / 左 / 右 | 2 / 13 / 27 / 35 | 低有效 |
| A / B | 34 / 12 | 低有效（34/35 外部上拉） |

## 3. 显示驱动（hardware/lcd.c）

- ST7735 初始化采用 MicroPython `init(2)` 兼容序列（SWRESET → SLPOUT → FRMCTR/PWCTR/GMCTR 全套寄存器 → NORON）。
- 旋转 90°（MADCTL `MX|MV`），显示区 = 原生 128×160 → 160×128，无偏移（X/Y gap = 0）。
- **三重整屏 DMA 缓冲**（`LCD_DRAW_BUF_COUNT=3`）：前两块走 `heap_caps_malloc(SPIRAM|DMA)`，第三块走 `lv_display_set_3rd_draw_buffer`，均整屏 160×128；SPI 事务由 esp_lcd panel_io 驱动（`trans_queue_depth=10`）。
- 首帧 flush 完成后 `hw_lcd_display_on()` 点亮背光（避免开机黑屏/白屏闪烁）。
- 刷新模式 `LV_DISPLAY_RENDER_MODE_FULL`，颜色 `LV_COLOR_FORMAT_RGB565_SWAPPED`，DPI 60。

## 4. 按键驱动（hardware/buttons.c）

- 6 键低有效，硬件消抖 25ms（`BUTTON_DEBOUNCE_MS`）。
- 上升沿采样 + 稳定窗口判定：`raw_changed_ms` 记变化时刻，稳定 25ms 后才进入已按状态。
- 映射为 LVGL key：上=LV_KEY_UP、下=DOWN、左=LEFT、右=RIGHT、A=ENTER、B=ESC，经 keypad 输入设备回调 `hw_buttons_read()` 喂给 LVGL。

## 5. SD 卡（hardware/sd.c）

- SDSPI 复用 SPI2（SCLK/MOSI/MISO 与 LCD 共用），CS=22，上限 10MHz（`SD_SPI_MAX_FREQ_KHZ=10000`）。
- `hw_sd_try_mount()` 启动时尝试挂载；挂载配置 `max_files=3`、`format_if_mount_failed=false`；失败/卸载后名称显示 "NO CARD"。
- 暴露 `hw_sd_is_mounted()` / `hw_sd_name()` / `hw_sd_mb()` / `hw_sd_last_err()`，软件层不触碰卡内部。
- 挂载点 `/sdcard`：播放器扫 `/sdcard/*.mp3`，电子书扫 `/sdcard/*.txt`。

## 6. 音频输出（hardware/audio.c）

- I2S 输出到 MAX98357（BCLK=25/LRC=32/DIN=33，无 MCLK；MAX98357 由 BCLK 自行派生主时钟）。
- **解耦管线**：MP3 解码写入 256KB PCM 环形缓冲（优先 PSRAM，>1s @44.1kHz），`audio_feed` 任务（4KB 栈，优先级 6）负责从环形缓冲灌 I2S DMA——防爆音与欠载。
- 输出前对 PCM 做 700Hz 喇叭保护高通（Q15 定点，就地滤波）。
- 音量：初始化时预计算 101 项 Q15 对数锥度表（0~-40dB），`hw_audio_set_volume()` 即时生效，15ms 平滑渐变；`hw_audio_set_sample_rate()` 延迟到 feed 任务应用。
- 路由互斥：蓝牙开启且已连接 → 仅走 BT（I2S 不喂数据，喇叭静音）；否则走喇叭；播放器经 `hw_audio_set_player_active()` 声明总线归属。
- 详见 `docs/audio.md`。

## 7. 蓝牙（hardware/bt_audio.c）

- BT Classic + Bluedroid，A2DP Source + AVRCP Target，懒启动（进蓝牙页才拉栈），见 `docs/bluetooth.md`。

## 8. 启动顺序（main.c）

```text
nvs_flash_init
  → hw_buttons_init()        GPIO + 消抖任务
  → hw_lcd_init()            SPI/panel/ST7735 初始化
  → hw_audio_init()          I2S + MAX98357 + 环形缓冲 + feed 任务
  → bt_audio_init()          环形缓冲（不碰蓝牙控制器）
  → 注册 AVRCP 回调（cmd / volume）
  → hw_sd_try_mount()        SD 挂载（可失败）
  → player_init() / ebook_init()   后台任务
  → lv_init → hw_lcd_create_display → ui_input_init → ui_start_tick_timer
  → xTaskCreate(lvgl_task, "lvgl", 10KB, 优先级 5)
```

`lvgl_task`：`ui_create` → `lv_refr_now` → 等待 LCD 首帧 flush 完成后 `hw_lcd_display_on()` 点亮背光（避免开机白屏）；随后 ~60Hz 循环：每 `UI_REFRESH_PERIOD_MS=16ms` 调 `ui_refresh()`，`lv_timer_handler()` 延迟钳制在 1~16ms（`LVGL_TASK_MIN/MAX_DELAY_MS`）。
