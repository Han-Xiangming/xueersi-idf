# 小喵掌机 硬件层与板级配置说明

> 本文档为**实现现状说明**（随代码更新）：`components/board/board_config.h` 与 `components/drivers/` 各驱动的职责、引脚与时序。

## 1. 板级总览

```text
ESP32（WROVER-B，PSRAM 8MB）@ 240MHz
  ├─ ST7789  TFT 320×240（原生 240×320，旋转 90°）  SPI2 @ 60MHz
  ├─ MicroSD 卡（SDSPI，与 LCD 共用 SPI2）          CS=22 @ 10MHz
  ├─ MAX98357 单声道 Class-D DAC                    I2S，无 MCLK
  ├─ 9 键（低有效）                                 上/下/左/右/A/B/SELECT/START/MENU
  ├─ 单节锂电检测                                   GPIO39（100k/100k 分压）
  └─ 蓝牙（BT Classic，A2DP Source / AVRCP TG+CT）
```

## 2. 引脚分配（components/board/board_config.h）

| 功能 | 引脚 | 说明 |
| ---- | ---- | ---- |
| LCD SCLK | 18 | SPI2 |
| LCD MOSI | 23 | SPI2 |
| LCD MISO | 19 | SPI2（未用） |
| LCD CS | 5 | |
| LCD DC | 4 | |
| SD CS | 22 | SDSPI（SCLK/MOSI/MISO 与 LCD 共用） |
| I2S BCLK | 32 | 帧时钟输出 |
| I2S LRC | 15 | 声道选择 |
| I2S DIN | 21 | PCM 数据 |
| 上 / 下 / 左 / 右 | 2 / 13 / 27 / 35 | 低有效 |
| A / B | 34 / 12 | 低有效（34/35 外部上拉） |
| Select / Start / Menu | 25 / 26 / 33 | 均低有效（按下接 GND，内部上拉） |
| 电池 ADC | 39 (ADC1_CH3) | 100k/100k 分压，满电基准 4.10V / 空电 3.30V |

电池改装：原板 NTC 热敏已去除，改为 100kΩ/100kΩ 分压接 GPIO39，固件 `BAT_DIV_FACTOR=2.0` 还原电压；oneshot ADC + 12bit + 16 次突发均值 + 中值滤波 + 非对称 EMA，播放期做负载补偿。

## 3. 显示驱动（components/drivers/lcd/lcd.c）

- ST7789 初始化采用 Adafruit 兼容序列（SWRESET → SLPOUT → MADCTL/COLMOD=RGB565 → CASET/RASET → INVON → NORON）。
- 旋转 90°（MADCTL `MX|MV`），显示区 = 原生 240×320 → 320×240，无偏移（X/Y gap = 0）。
- **部分刷新 DMA 缓冲**（`LCD_DRAW_BUF_COUNT=2`）：`LCD_DRAW_BUF_LINES=20`（约 1/6 屏高），前一块在 SPI 刷出时 LVGL 渲染下一块；ESP32 SPI DMA 无法访问 PSRAM，所有缓冲统一通过 `spi_bus_dma_memory_alloc()` 在内部 DMA RAM 分配，buf2 失败降级单缓冲、buf1 失败重启。LVGL 只渲染并刷新脏区域，脏矩形按 `LCD_FLUSH_BATCH_ROWS=4` 行窗口批量提交（每窗一次 CASET+RASET+RAMWR），SPI 事务由 esp_lcd panel_io 驱动（`trans_queue_depth=1`，防止 SD 等 SPI 被长时间占住）。
- 首帧 flush 完成后 `hw_lcd_display_on()` 点亮背光（避免开机黑屏/白屏闪烁）。
- 刷新模式 `LV_DISPLAY_RENDER_MODE_PARTIAL`，颜色 `LV_COLOR_FORMAT_RGB565_SWAPPED`，DPI 60。

## 4. 按键驱动（components/drivers/buttons/buttons.c）

- 9 键低有效，硬件消抖 10ms（`BUTTON_DEBOUNCE_MS`，配合 5ms 轮询把按下到 UI 的延迟压到 ~10-15ms）。
- 上升沿采样 + 稳定窗口判定：`raw_changed_ms` 记变化时刻，稳定 10ms 后才进入已按状态。
- 映射为 LVGL key：上/下/左/右=UP/DOWN/LEFT/RIGHT、A=ENTER、B=ESC、SELECT=HOME、START=END（仅唤醒，无功能）、MENU=自定义 `LV_KEY_MEDIA_PANEL`（悬浮播控面板），经 keypad 输入设备回调 `hw_buttons_read()` 喂给 LVGL。

## 5. SD 卡（components/drivers/sd/sd.c）

- SDSPI 复用 SPI2（SCLK/MOSI/MISO 与 LCD 共用），CS=22，上限 10MHz（`SD_SPI_MAX_FREQ_KHZ=10000`）。
- `hw_sd_try_mount()` 启动时尝试挂载；挂载配置 `max_files=8`、`format_if_mount_failed=false`；失败/卸载后名称显示 "NO CARD"。
- `hw_sd_poll()` 在主循环每秒探活：`sdmmc_get_status()` 连续两次失败才卸载，空槽每秒重试挂载且仅在错误码变化时打印。卸载前经 `hw_sd_set_pre_unmount_cb()` 回调让播放器/电子书停任务、写回进度并释放全部文件句柄；`hw_sd_generation()` 让 UI 识别两次刷新之间的换卡，仅新卡挂载时通知重扫。
- 暴露 `hw_sd_is_mounted()` / `hw_sd_name()` / `hw_sd_mb()` / `hw_sd_last_err()` / `hw_sd_generation()`，软件层不触碰卡内部。
- 挂载点 `/sdcard`：播放器默认扫 `/sdcard/**/*.mp3`（UI 来源收窄到 `/sdcard/Music`），电子书递归扫 `/sdcard/eBook/**/*.txt`。

## 6. 音频输出（components/drivers/audio/audio.c）

- I2S 输出到 MAX98357（BCLK=32/LRC=15/DIN=21，无 MCLK；MAX98357 由 BCLK 自行派生主时钟）。
- **ring + writer**：MP3 解码任务 DSP 后 `xRingbufferSend()` 推入 32KB PCM 环，独立 `i2s_wr` 任务（优先级 8 / Core 1）按 4KB 块拉取喂 `i2s_channel_write()`，软件 SpeexDSP 重采样到固定 44.1kHz，采样率变化不重建通道。
- **DSP 链（喇叭路由，逐样本定点）**：800Hz 保护高通 → 主音量（~5ms 平滑）→ 响度低音架 → ReplayGain/主增益 → 400Hz 双频段 DRC → −2.5dBFS 硬顶棚 → `(L+R)/2` 单声道下混并复制到双 slot。蓝牙路由只加音量、保留完整立体声。
- 音量：初始化预计算两张 Q15 对数锥度表（0.1dB 步进）：喇叭 **401 项**（0~-40dB）+ 蓝牙 **121 项**（0~-12dB，sink 侧还会再衰减故本地只做微调），喇叭与蓝牙各一独立百分比槽位，`hw_audio_set_volume()` 即时生效并平滑渐变。
- **通道驻车与接缝**：暂停不关通道；蓝牙路由或整段停止（B 键 / 看门狗）时 writer 关通道省电。切歌/重播由 `i2s_seam_rearm()` 全量静默重写 48KB DMA 描述符环、disable/enable 后补 2KB prime（硬切换前 20ms 余弦淡出），避免上一首尾音或爆音；flush 用代际计数确认，上限 1200ms 并喂 WDT。所有通道操作收敛到单一 writer 任务内串行，无互斥锁。
- 路由互斥：蓝牙开启且已连接 → 仅走 BT（I2S 不喂数据，喇叭静音）；否则走喇叭；播放器经 `hw_audio_set_player_active()` 声明总线归属。
- 详见 `docs/audio.md`。

## 7. 蓝牙（components/drivers/bluetooth_audio/bluetooth_audio.c）

- BT Classic + Bluedroid，A2DP Source + AVRCP TG+CT（双向音量镜像），懒启动（进蓝牙页才拉栈），见 `docs/bluetooth.md`。

## 8. 启动顺序（main/main.c）

```text
esp_log_level_set("BT_L2CAP", WARN)     降噪（BT 拥塞回调日志泛滥，见 main.c 注释）
  → nvs_flash_init
  → avrc_nav_q = xQueueCreate(...)     AVRCP NEXT/PREV → LVGL 任务的投递队列
  → hw_buttons_init()                  GPIO + 10ms 消抖
  → hw_lcd_init()                      SPI/panel/ST7789 初始化
  → hw_audio_init()                    I2S + MAX98357（32KB ring + i2s_wr(8) 写任务）
  → hw_battery_init()                  电池 ADC 采样
  → bluetooth_audio_init()             环形缓冲（不碰蓝牙控制器）
  → 注册 AVRCP 回调（cmd / volume / sync_vol）+ hw_audio_set_bt_volume_change_cb()
  → hw_sd_set_pre_unmount_cb()         卸载前释放播放器/电子书文件句柄
  → hw_sd_try_mount()                  SD 挂载（可失败）；主循环每秒 hw_sd_poll()
  → player_init()                      mp3_player/mp3_scan/mp3_watch 三任务
  → player_watchdog_started() ? rtc_wdt_arm() : rtc_wdt_disable()
  → ebook_init()                       扫描/计数/保存任务（不绑核）
  → lv_init → hw_lcd_create_display → ui_input_init → ui_start_tick_timer
  → xTaskCreatePinnedToCore(lvgl_task, "lvgl", 优先级 7, Core 1)
```

`lvgl_task`：`ui_create` → `lv_refr_now` → 等待 LCD 首帧 flush 完成后 `hw_lcd_display_on()` 点亮背光（避免开机白屏）；随后 ~60Hz 循环：每 `UI_REFRESH_PERIOD_MS=16ms` 调 `ui_refresh()`，`lv_timer_handler()` 延迟钳制在 1~16ms（`LVGL_TASK_MIN/MAX_DELAY_MS`）。

核分配：Core 0 承载 MP3 解码、BT 控制器/协议栈与系统任务；Core 1 承载 `lvgl`(7) 与 `i2s_wr`(8) 两条应用实时链路；其余后台任务不绑核。

- **`i2s_wr` 必须高于 `lvgl`（8 > 7）**：二者同在 Core 1。若 `i2s_wr`(6) 低于 `lvgl`(7)，调音量触发的全页重绘 + 同步 SPI 刷屏会抢占 writer 超过 PCM ring 的 ~186 ms 裕量，解码侧 `xRingbufferSend()` 超时丢样本，串口报 `PCM ring full: N bytes dropped (writer stalled >100 ms)`、听感为断流卡顿。与 `ui.h` §任务优先级里记录的旧故障（LVGL 高于 consumer 导致 I2S underrun）同源；解码任务已移至 Core 0，writer 也曾漏改，现已一并提到 8。详见 `docs/player.md` §1。
