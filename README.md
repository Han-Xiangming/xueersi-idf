# 小喵掌机 ESP-IDF 固件

这是给学而思小喵掌机（ESP32-WROVER-B 版本）移植的 **ESP-IDF / LVGL 9.5 C 固件工程**，提供 MP3 播放、SD 卡浏览、蓝牙 A2DP 音频输出、TXT 电子书阅读、背光/音量设置等功能的分页 UI。

> 注意：本工程是原生 ESP-IDF C 固件（不是 MicroPython），源码位于 `main/`、`components/`，构建系统为 CMake + ESP-IDF v6.1（当前使用 v6.1-beta1）。

## 固件下载与刷入

已经编译好的 merged bin 放在本项目的 [Releases](https://github.com/ZyoungInc/xueersi-idf/releases/latest) 页面。普通用户可以直接下载 release 里的 `xiaomiao-merged.bin` 并从 `0x0` 地址刷入，不需要自己搭建 ESP-IDF 编译环境。

示例命令：

```bash
esptool.py --chip esp32 -b 460800 write_flash 0x0 xiaomiao-merged.bin
```

刷入前请确认目标硬件是 ESP32-WROVER-B 版本的小喵掌机，并确认串口连接正常。

## 从源码构建

需要 ESP-IDF v6.1（已针对 v6.1-beta1 验证）。

```bash
# 在 ESP-IDF 环境下
idf.py set-target esp32
idf.py build
idf.py -p (PORT) flash monitor
```

关键默认配置位于 `sdkconfig.defaults`：

- CPU 主频 240 MHz，4 MB flash，启用 PSRAM（Quad SPI，80 MHz）
- LVGL 9.5，RGB565 颜色深度，刷屏周期 16 ms，DPI 60
- 蓝牙经典 BR/EDR（Bluedroid 主机），启用 A2DP 与 AVRCP；不启用 BLE
- FATFS 启用长文件名（堆分配）+ UTF-8 文件名编码，以支持中文文件名

## 当前状态

- 固件用 LVGL 9.5 驱动 ST7789 显示屏，提供分页 UI：播放器 / 电子书 / 设置（蓝牙管理作为设置页的子页）。
- 显示：ST7789（旋转 90° 后逻辑分辨率 320×240，实际面板为 240×320 原生），SPI2 最高 60 MHz；采用部分刷新 + 双 DMA 缓冲（每屏只重绘脏区）。由于屏幕 TE 引脚未接 MCU，无法做垂直同步，但双缓冲部分刷新已规避明显撕裂。
- 背光由 GPIO14 的 LEDC PWM 驱动（5 kHz，10 位分辨率），可在设置页调节亮度（0..100%）。
- 自动息屏：设置页可选空闲超时（永不 / 15 秒 / 30 秒 / 60 秒 / 2 分钟 / 5 分钟，默认 30 秒）；超时后关闭背光并让 ST7789 进入 DISPOFF 省电，任意按键即时唤醒恢复亮度。
- 已接入功能：按键（6 键）、MicroSD（SDSPI）、I2S 音频（MAX98357 Class-D DAC）、MP3 播放（libhelix-mp3 解码）、蓝牙 A2DP 音频输出（SOURCE 角色）、TXT 电子书阅读、单节锂电电量检测与低电量保护。详见 `docs/` 下各模块文档。
- 蓝牙在用户打开蓝牙页时才懒加载启动（开机不广播），关掉蓝牙开关时彻底断电。蓝牙输出为显式路由切换，不会因蓝牙连接上来而静默抢占本地扬声器。

## 原理图与鸣谢

原理图文件已整理为 [`docs/xueersi-xiaomiao-schematic.pdf`](docs/xueersi-xiaomiao-schematic.pdf)。

感谢 ID「我为电波狂」对硬件进行测量并制作原理图，这部分资料对后续移植和维护非常关键。


## 1. 总体架构

```text
ESP32-WROVER-B (主控)
  ├─ SPI2 (共用，分时 CS)
  │   ├─ ST7789 TFT 显示屏   (CS=GPIO5,  DC=GPIO4,  BL=GPIO14)
  │   └─ MicroSD 卡          (CS=GPIO22)
  ├─ I2S 音频输出 (MAX98357)
  │   ├─ BCLK = GPIO32
  │   ├─ LRC  = GPIO15
  │   └─ DIN  = GPIO21        (无 MCLK，MAX98357 自带 PLL)
  ├─ 6 个按键 (GPIO2/13/27/35/34/12)
  ├─ 电池电压采样 (GPIO39, ADC1_CH3, 电阻分压 1/2)
  └─ 蓝牙 (Bluedroid, A2DP SOURCE + AVRCP TARGET)
```

代码分层：

```text
main/main.c            —— 启动装配，初始化各驱动、创建 LVGL 与 UI、运行主循环
components/drivers/    —— 硬件层：buttons / lcd / audio / bt_audio / battery / sd
components/app/        —— 应用层：ui（LVGL 界面） / player（MP3） / ebook（TXT）
components/board/      —— 板级硬件配置（board_config.h）
components/fonts/      —— 中文 CJK 点阵字体（lv_font_cn_10 ~ lv_font_cn_16）
```

***

## 2. ESP32 引脚分配

### 2.1 ST7789 TFT 显示屏

| 功能        | ESP32 引脚     |
| --------- | ------------ |
| SPI SCK   | GPIO18       |
| SPI MOSI  | GPIO23       |
| SPI MISO  | GPIO19       |
| TFT DC    | GPIO4        |
| TFT CS    | GPIO5        |
| 背光 BL     | GPIO14 (LEDC PWM) |

显示对象信息：

```text
显示面板：ST7789
原生分辨率：240 × 320
旋转 90° 后逻辑分辨率：320 × 240
SPI：SPI2 (LCD_HOST)
SPI 频率：60 MHz
SCK：GPIO18
MOSI：GPIO23
MISO：GPIO19
DC：GPIO4
CS：GPIO5
背光：GPIO14，LEDC PWM 5 kHz / 10 位
```

### 2.2 MicroSD 卡

| 功能      | ESP32 引脚     |
| ------- | ------------ |
| SPI SCK | GPIO18       |
| SPI MOSI | GPIO23       |
| SPI MISO | GPIO19       |
| SD CS   | GPIO22       |

TFT 与 MicroSD 共用 SPI2，通过不同 CS 分时使用。SD 挂载在 `/sdcard`，由 `hw_sd_try_mount()` 在启动和插入时尝试挂载。

### 2.3 按键

| 按键  | ESP32 引脚     |
| --- | ------------ |
| 上   | GPIO2        |
| 下   | GPIO13       |
| 左   | GPIO27       |
| 右   | GPIO35       |
| A   | GPIO34       |
| B   | GPIO12       |

按键为低电平有效，带 25 ms 消抖，经 LVGL keypad 输入设备接入 UI。

注意：

```text
GPIO34、GPIO35 是输入专用脚。
GPIO12 是启动相关敏感脚。
```

### 2.4 I2S 音频（MAX98357 Class-D DAC）

| 功能    | ESP32 引脚     |
| ----- | ------------ |
| BCLK  | GPIO32       |
| LRC   | GPIO15       |
| DIN   | GPIO21       |

MAX98357 由 BCLK 内部派生主时钟，因此 **不需要 MCLK**。

### 2.5 电池电压采样

| 功能  | ESP32 引脚       |
| --- | -------------- |
| 分压输入 | GPIO39 (ADC1_CH3) |

板载两颗 100 kΩ 电阻分压（1/2），将单节锂电电压降到 ADC 量程内。电量通过开路电压经验查表（非线性）换算为 0..100%，详见 `components/drivers/battery/battery.c`。

***

## 3. 功能说明

### 3.1 播放器（MP3）

- 解码库：`esp-libhelix-mp3`（libhelix C API），运行在独立 FreeRTOS 任务中，不阻塞 UI。
- 音源：SD 卡 `/sdcard/Music` 目录下的 `.mp3` 文件（后台扫描，最多 64 首），以及一首内嵌 ROM 测试曲。
- 支持播放 / 暂停切换 / 停止；播放时上下键调节音量，A 播放/继续，B 停止。
- 输出路由：`AUDIO_ROUTE_SPEAKER`（本地 MAX98357）与 `AUDIO_ROUTE_BT`（蓝牙 sink）二选一，由 UI 显式切换。

### 3.2 蓝牙音频（A2DP SOURCE）

- 设备作为 **A2DP SOURCE**：把解码出的 PCM 通过 SBC 编码后推流给配对的蓝牙音箱/耳机，SBC 编码由 Bluedroid 完成。
- 懒加载启动：进入「蓝牙」页才拉起蓝牙控制器/协议栈；关闭开关时彻底断电。
- 扫描：GAP 通用查询（约 12.8 s，`BT_INQ_LEN=10`），只收录带 RENDERING 类的设备，按地址去重，名称取自 inquiry 的 BDNAME/EIR，列表最多 8 个（`BT_MAX_DEVICES`）。
- 配对：支持 SSP 数字比对（配对时显示 6 位配对码）。
- 远程控制：作为 AVRCP TARGET，接受远端耳机的播放/暂停/停止媒体键，以及远端绝对音量（0..127）映射回本地主音量。
- 连接状态变化不会自动抢占本地扬声器路由；反之断链时音频层会把路由交还扬声器。

### 3.3 电子书（TXT）

- 读取 SD 卡 `/sdcard` 下的 `.txt` 文件（UTF-8 纯文本）以及内嵌 ROM 测试书。
- 用 16px 中文点阵字体按固定 5 行/页确定性排版，前后翻页与后台页数统计始终一致。

### 3.4 设置

设置页可上下选择、左右调整的项目：

```text
音量   —— 当前激活路由音量（蓝牙路由取 BT 音量，否则取扬声器音量），分轨存 NVS
背光   —— 屏幕亮度 0..100%，存 NVS
蓝牙   —— 蓝牙输出总开关（开/关），存 NVS；进入后展开蓝牙管理子页
重置NVS —— 清除已保存配置
```

### 3.5 电池与低电量保护

- 电压经 ADC 采样 + 开路电压查表换算为百分比。
- 低电量保护（`hw_battery_set_low_warn`，阈值 15%）：下穿阈值时暂停播放并把背光调暗至不高于 20%（记住用户设定）；回升超过阈值 + 3% 迟滞后恢复用户背光，不干预播放。

***

## 4. 目录结构与模块接口

| 路径                                  | 说明                                       |
| ----------------------------------- | ---------------------------------------- |
| `main/main.c`                        | 启动装配 + LVGL 主循环                        |
| `components/board/board_config.h`    | 板级硬件引脚、显示几何、分压系数                      |
| `components/drivers/buttons/`        | 6 键输入 + 消抖，LVGL keypad 读取回调             |
| `components/drivers/lcd/`            | ST7789 驱动 + LVGL 显示绑定 + 背光 PWM          |
| `components/drivers/audio/`          | I2S → MAX98357，路由/音量/采样率/ PCM 写入         |
| `components/drivers/bt_audio/`       | 蓝牙 A2DP SOURCE + AVRCP + 扫描/配对/连接管理      |
| `components/drivers/battery/`        | 电池电压采样 + 开路电压查表 + 低电量回调              |
| `components/drivers/sd/`             | SDSPI 挂载状态与访问                          |
| `components/app/ui/`                 | LVGL 分页 UI（播放器/电子书/设置/蓝牙）            |
| `components/app/player/`             | MP3 解码任务 + 后台曲库扫描                      |
| `components/app/ebook/`              | TXT 电子书读取与确定性分页                       |
| `components/fonts/`                  | 中文 CJK 点阵字体（10~16px，覆盖 4E00-9FFF 及假名） |

各模块的公共接口见其头文件（`*.h`），软件层（app）只通过硬件层（drivers）的公共 API 访问外设，不直接操作寄存器或 LVGL 显示内部。

***

## 5. 已知限制与备注

- 屏幕 TE 引脚未接 MCU，无法垂直同步；依赖双缓冲 + 部分刷新规避撕裂。
- 蓝牙路由下 `hw_audio_set_volume()` 仅改本地 BT 增益，未通过 AVRCP 主动下发远端音箱音量（远端音量由远端媒体键控制）。
- 当前播放器无播放列表导航，AVRCP 的 NEXT/PREV 被忽略。
- 原 MicroPython 时代的 GD32 协处理器、电机、MPU6050、SugarASR 等功能不在本 ESP-IDF 固件范围内。

详见 `docs/`：

- `docs/ui.md` —— UI 页面与交互
- `docs/player.md` —— MP3 播放器
- `docs/ebook.md` —— 电子书阅读器
