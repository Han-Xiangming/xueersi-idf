# 小喵掌机 ESP-IDF 固件

这是给学而思小喵掌机（ESP32-WROVER-B 版本）移植的 **ESP-IDF / LVGL 9.5 C 固件工程**，提供 MP3 播放、SD 卡浏览、蓝牙 A2DP 音频输出、TXT 电子书阅读、背光/音量设置等功能的分页 UI。

> 注意：本工程是原生 ESP-IDF C 固件，源码位于 `main/`、`components/`，构建系统为 CMake + ESP-IDF v6.1（正式版，IDF 路径 `D:\esp\v6.1\esp-idf`）。

## 目录

1. [快速开始](#1-快速开始)
2. [功能总览](#2-功能总览)
3. [总体架构](#3-总体架构)
4. [硬件与引脚分配](#4-硬件与引脚分配)
5. [功能说明](#5-功能说明)
6. [目录结构与模块接口](#6-目录结构与模块接口)
7. [已知限制与备注](#7-已知限制与备注)
8. [参考文档](#8-参考文档)

---

## 1. 快速开始

本工程不提供预编译固件，需自行从源码构建。

### 1.1 从源码构建

需要 ESP-IDF v6.1（正式版，已验证）。本机开发请统一使用仓库根目录的 `dev.ps1`：它会定位/再生成本机 IDF activate 脚本并设置 `IDF_TOOLS_PATH`，避免手工 export 造成工具链路径不一致。

```powershell
.\dev.ps1 build                          # 编译
.\dev.ps1 build+flash                    # 编译并烧录（自动探测串口）
.\dev.ps1 monitor                        # 串口监视（Ctrl+] 退出）
.\dev.ps1 build+flash+monitor -p COM3    # 指定串口
.\dev.ps1 menuconfig                     # 配置
.\dev.ps1 size / clean / reconfigure     # 体积 / 清理 / 重新 cmake
```

`flash` 动作只烧录不编译，请用 `build+flash`。通用 ESP-IDF 环境（已正确 export 且与本机工具链一致）下也可直接 `idf.py build` / `idf.py -p (PORT) flash monitor`；`dev.bat` 仅作为旧入口保留。

关键默认配置位于 `sdkconfig.defaults`：

- CPU 主频 240 MHz，4 MB flash，启用 PSRAM（Quad SPI，80 MHz）
- LVGL 9.5，RGB565 颜色深度，刷屏周期 16 ms，DPI 60
- 蓝牙经典 BR/EDR（Bluedroid 主机），启用 A2DP 与 AVRCP；不启用 BLE
- FATFS 启用长文件名（堆分配）+ UTF-8 文件名编码，以支持中文文件名

### 1.2 PC 端工具脚本

- `rsgain-scan.ps1` —— 调用 rsgain 给 SD 卡曲库写入 ReplayGain 2.0 标签：目标 -18 LUFS、统一 ID3v2.3、true peak / 峰值上限 -1 dBTP、总是启用削波保护。安全参数由运行时临时 preset 提供（不落盘到仓库），每目录输出 `replaygain.csv`，末尾汇总最热 / 正增益 / 应用增益后峰值逼近 -1 dBTP 的曲目。三种模式：`-Mode Full`（命令行默认，全量重扫）、`-Mode Smart`（智能增量，只重扫新增 / 修改 / 不在校验清单里的文件）、`-Mode Incremental`（传统 `-S`，跳过任何已有标签的文件）。双击且不带参数时先询问目录，再弹模式菜单（默认 Smart）；Smart 会把成功扫描过的曲目以路径 + 大小 + mtime + preset 签名记录在曲库根目录的隐藏文件 `.rsgain_scan_state.tsv` 中，preset 变更会自动失效并退化为全量。待扫文件会先删掉旧 ReplayGain 标签再用 `-S` 重扫，避免外部旧基准混入；失败不更新清单，下次自动重试。`rsgain-scan.bat` 仅保留为转发 wrapper。
- `clear-cover.bat` / `clear-cover.ps1` —— 批量清除 mp3 内嵌封面（视频流）并保留全部音频标签：用 ffmpeg 转封装剥离封面流后回写。ReplayGain 等 TXXX 标签在转封装中原样保留，不影响固件按标签做响度归一。
- 上述两个脚本在资源管理器里**双击运行**时，结束后会停在「按 Enter 关闭」提示上（沿父进程链识别 explorer.exe 双击启动），避免窗口一闪而过看不到汇总；从终端或自动化调用时不阻塞，可用 `-Pause` / `-NoPause` 强制。

---

## 2. 功能总览

- 固件用 LVGL 9.5 驱动 ST7789 显示屏，提供分页 UI：播放器 / 电子书 / 蓝牙 / 设置。蓝牙为独立顶级页，进入后才懒加载启动协议栈（见 5.2）。
- 显示：ST7789（旋转 90° 后逻辑分辨率 320×240，实际面板为 240×320 原生），SPI2 最高 60 MHz；采用部分刷新 + 双 DMA 绘制缓冲（buf2 分配失败时降级单缓冲），按 4 行窗口批量提交 SPI 事务，每屏只重绘脏区。由于屏幕 TE 引脚未接 MCU，无法做垂直同步，但部分刷新已规避明显撕裂。
- 背光由 GPIO14 的 LEDC PWM 驱动（5 kHz，10 位分辨率），可在设置页调节亮度（0..100%，gamma 2.2 曲线）。
- 自动息屏：设置页可选空闲超时（永不 / 15 秒 / 30 秒 / 60 秒 / 2 分钟 / 5 分钟，默认 30 秒）；超时后关闭背光并让 ST7789 进入 DISPOFF 省电，任意按键即时唤醒恢复亮度。
- 已接入功能：按键（9 键）、MicroSD（SDSPI，热插拔探活）、I2S 音频（MAX98357 Class-D DAC）、MP3 播放（libhelix-mp3 解码 + LAME/Xing gapless + ReplayGain 2.0 响度归一 + 曲库缓存）、蓝牙 A2DP 音频输出（SOURCE 角色，AVRCP TG+CT 双向音量）、TXT 电子书阅读、单节锂电电量检测。
- 蓝牙在用户打开蓝牙页时才懒加载启动（开机不广播），关掉蓝牙开关时彻底断电。蓝牙输出为显式路由切换，不会因蓝牙连接上来而静默抢占本地扬声器。
- 启动器（Launcher）为 3×2 图标网格，当前填充 4 个磁贴：第一排 音乐 / 电子书 / 蓝牙，第二排 设置。方向键在网格内移动选中（左右按行主序回绕、上下两排循环切换），选中态以右上角 accent 圆点光标标示并随选择平滑滑动；A 键进入所选页面。

---

## 3. 总体架构

```text
ESP32-WROVER-B (主控)
  ├─ SPI2 (共用，分时 CS)
  │   ├─ ST7789 TFT 显示屏   (CS=GPIO5,  DC=GPIO4,  BL=GPIO14)
  │   └─ MicroSD 卡          (CS=GPIO22)
  ├─ I2S 音频输出 (MAX98357)
  │   ├─ BCLK = GPIO32
  │   ├─ LRC  = GPIO15
  │   └─ DIN  = GPIO21        (无 MCLK，MAX98357 自带 PLL)
  ├─ 9 个按键 (UP/DOWN/LEFT/RIGHT/A/B=GPIO2/13/27/35/34/12,
  │           SELECT/START/MENU=GPIO25/26/33)
  ├─ 电池电压采样 (GPIO39, ADC1_CH3, 100k/100k 分压 1/2)
  └─ 蓝牙 (Bluedroid, A2DP SOURCE + AVRCP TG+CT)
```

代码分层：

```text
main/main.c            —— 启动装配，初始化各驱动、创建 LVGL 与 UI、运行主循环
components/drivers/    —— 硬件层：buttons / lcd / audio / bluetooth_audio / battery / sd
components/app/        —— 应用层：ui（LVGL 界面） / player（MP3） / ebook（TXT）
components/board/      —— 板级硬件配置（board_config.h）
components/fonts/      —— 中文 CJK 点阵字体（lv_font_cn_16，16px）
```

---

## 4. 硬件与引脚分配

### 4.1 ST7789 TFT 显示屏

| 功能    | ESP32 引脚     |
| ----- | ------------ |
| SPI SCK | GPIO18     |
| SPI MOSI | GPIO23    |
| SPI MISO | GPIO19    |
| TFT DC | GPIO4        |
| TFT CS | GPIO5        |
| 背光 BL | GPIO14 (LEDC PWM) |

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

### 4.2 MicroSD 卡

| 功能    | ESP32 引脚     |
| ----- | ------------ |
| SPI SCK | GPIO18     |
| SPI MOSI | GPIO23    |
| SPI MISO | GPIO19    |
| SD CS | GPIO22        |

TFT 与 MicroSD 共用 SPI2，通过不同 CS 分时使用。SD 挂载在 `/sdcard`：`hw_sd_try_mount()` 在启动时挂载，主循环每秒调用 `hw_sd_poll()` 做探活/重挂载。拔卡时先释放播放器与电子书的全部文件句柄，再 `esp_vfs_fat_sdcard_unmount()`；重新插入后自动挂载并刷新曲库/书库。

### 4.3 按键

| 按键 | ESP32 引脚 |
| --- | ---------- |
| 上  | GPIO2      |
| 下  | GPIO13     |
| 左  | GPIO27     |
| 右  | GPIO35     |
| A   | GPIO34     |
| B   | GPIO12     |
| SELECT | GPIO25  |
| START  | GPIO26  |
| MENU   | GPIO33  |

按键为低电平有效，带 10 ms 消抖，经 LVGL keypad 输入设备接入 UI：SELECT 在播放器切循环模式 / 阅读页呼出跳转 / 蓝牙页重扫；START 当前只用于唤醒（无页面功能）；MENU 在电子书页呼出悬浮播控面板、在播放器页返回主菜单。

注意：

```text
GPIO34、GPIO35 是输入专用脚。
GPIO12 是启动相关敏感脚。
```

### 4.4 I2S 音频（MAX98357 Class-D DAC）

| 功能  | ESP32 引脚 |
| ---- | ---------- |
| BCLK | GPIO32     |
| LRC  | GPIO15     |
| DIN  | GPIO21     |

MAX98357 由 BCLK 内部派生主时钟，因此 **不需要 MCLK**。

### 4.5 电池电压采样

| 功能  | ESP32 引脚       |
| --- | -------------- |
| 分压输入 | GPIO39 (ADC1_CH3) |

板载两颗 100 kΩ 电阻分压（1/2），将单节锂电电压降到 ADC 量程内。电量通过开路电压经验查表（非线性）换算为 0..100%，详见 `components/drivers/battery/battery.c`。

---

## 5. 功能说明

### 5.1 播放器（MP3）

- 解码库：`esp-libhelix-mp3`（libhelix C API），运行在独立 FreeRTOS 任务中，不阻塞 UI。
- 音源：`/sdcard` 递归扫描（最多 256 首，`PLAYER_SCAN_MAX`；超出时 UI 提示「仅前 N 首(更多)」），支持「全部音乐（`/sdcard/Music`）/ 指定文件夹」源选择；首次扫描结果写入 `/sdcard/.xueersi_playlist.cache` 缓存（M3U8 文本格式，可被 VLC 等直接导入），下次进入播放器秒开，无需重新遍历 FATFS。缓存带 `##SIG` 源签名（两层目录内文件的名字 + 文件大小的 FNV-1a 哈希）与 `##EOF <count>` 尾部计数：关机后往卡里加 / 删 / 改歌，或缓存尾部计数与实际条目不符，都会自动重扫并更新缓存；写入先落临时文件再 rename 就位，避免掉电留下半写缓存。无需手动「刷新播放列表」。
- 支持播放 / 暂停切换 / 停止；播放时上下键调节音量，A 播放/继续，B 停止。
- 循环模式：列表循环（默认）/ 单曲循环，播放页按 Select 键切换，右上角状态栏显示当前模式。
- LAME/Xing gapless：解析首帧 side-info 后的 Xing/Info + LAME 标签，取 encoder delay 高 12 位加解码器 529 样本延迟，在首帧内按每声道样本数裁掉预采样/脏样本；无标签曲目跳过。换曲与单曲重播都会重新解析。
- ReplayGain 2.0：自动读取歌曲 ID3 标签中的 `REPLAYGAIN_TRACK_GAIN`（loudgain / rsgain 写入），在进入主音量前以约 5 ms 平滑曲线应用分轨响度增益，未打标歌曲按 0 dB 处理。
- 输出路由：`AUDIO_ROUTE_SPEAKER`（本地 MAX98357）与 `AUDIO_ROUTE_BT`（蓝牙 sink）二选一；蓝牙 sink 连接成功时自动接管输出（连接即接管），断链后交还扬声器。
- 故障兜底：无同步字 / 解码错误过多 / **解码成功但零进度（连续 16 帧既没消耗字节也没产出样本）的畸形帧**判定为损坏并自动跳到下一首（单曲循环下也跳过坏文件，避免死循环）——零进度护栏专门防 libhelix 在特定帧上原地自旋卡死（CPU 跑满、把停顿看门狗也饿死、且无任何日志的「静默冻结」）。连续失败过多则停止并提示。解码心跳被停顿看门狗监视，且解码任务已订阅 ESP 任务看门狗（5s 卡死即带 backtrace 重启）作为最后兜底（见 5.6）。

### 5.2 蓝牙音频（A2DP SOURCE）

- 设备作为 **A2DP SOURCE**：把解码出的 PCM 通过 SBC 编码后推流给配对的蓝牙音箱/耳机，SBC 编码由 Bluedroid 完成。
- 懒加载启动：进入「蓝牙」页才拉起蓝牙控制器/协议栈；关闭开关时彻底断电。
- 扫描：GAP 通用查询（约 12.8 s，`BT_INQ_LEN=10`），仅保留 COD 有效的设备（不再要求 RENDERING 服务位，手机等也能列出），按地址去重，名称取自 inquiry 的 BDNAME/EIR，列表最多 8 个（`BT_MAX_DEVICES`）。
- 配对：纯 Just Works 零交互（声明 `NoInputNoOutput`）；常态显示「配对中 <名>」，仅当非合规对端强制数值比较时才显示 6 位配对码。
- 远程控制：作为 AVRCP TG，接受远端耳机的播放/暂停/停止/上下曲媒体键，以及远端绝对音量（0..127）映射回本地蓝牙音量槽位；同时作为 AVRCP CONTROLLER 把本机蓝牙音量以 0..127 推送给 sink，实现双向镜像（入站音量不回显，避免环路）。AVRCP 上下曲通过队列投递到 LVGL 任务执行，与按键切歌共用 `player_next()/player_prev()`，列表光标会自动跟随正在播放的曲目；空闲时忽略上下曲。
- 连接成功时自动把输出路由切到蓝牙（连接即接管）；断链时音频层把路由交还扬声器。

### 5.3 电子书（TXT）

- 读取 SD 卡 `/sdcard/eBook` 下的 `.txt` 文件（UTF-8 纯文本，递归扫描最多 512 本；超出时 UI 提示「[来源] N 本+ (超出上限)」）。
- 用 16px 中文点阵字体按固定 8 行/页确定性排版，前后翻页与后台页数统计始终一致；支持续读位置保存与百分比跳转（详见 `docs/ebook.md`）。

### 5.4 设置

设置页为两级视图（先选「音频 / 显示 / 系统」分组，再选组内项），共 10 项：

```text
音频组：音量（档位 24/40 喇叭、6/12 蓝牙，分轨存 NVS，默认 30%）
        总音量补偿（-12..+12 dB，1 dB 步进，默认 0 dB，存 NVS）
        路由变更暂停（开/关，默认关，存 NVS）
        循环模式（列表/单曲/随机，存 NVS）
        蓝牙音质（稳定/均衡/高质 = SBC bitpool 33/45/53，存 NVS）
显示组：背光（0..100%，gamma 2.2 亮度曲线，存 NVS）
        息屏（永不/15s/30s/60s/2min/5min，默认 30s，存 NVS）
系统组：刷新播放列表（丢弃缓存、重新扫描）
        恢复出厂设置（清除已保存配置）
        清除阅读进度（丢弃 TXT 续读位置）
```

蓝牙入口已提升到 Launcher 顶层，蓝牙总开关由蓝牙页自身管理并持久化；设置页只保留上述 10 项。

### 5.5 电池

- GPIO39（ADC1_CH3）经板载 100kΩ/100kΩ 分压（固件按 ÷2 还原）采集锂电池电压，满电基准 4.10V（带载离充电器）、空电 3.30V。
- oneshot ADC + 12bit + 16 次突发均值 + 中值滤波 + 非对称 EMA；播放中按负载做电压补偿，避免误报低电量。

### 5.6 稳定性与故障兜底

- **非阻塞日志钩子**：日志输出直写 UART0 硬件 FIFO（HAL LL 层，IRAM，无驱动/无 esp_log 递归）；格式化缓冲为调用栈局部、FIFO 写入由短临界区串行化，多任务并发不会使日志行交错。发送缓冲打满时丢弃剩余行而不是挂起调用方——日志路径不可能再把任何任务卡死。
- **RTC 看门狗**（30 s 系统复位兜底）：走独立 RTC 时钟，不受 CPU 中断/任务调度影响；由播放器看门狗任务每 2 s 喂一次，健康状态下永远不会触发。只有整机级冻结（如 PSRAM 总线停滞）才会停止喂狗并自动复位，复位后启动日志会打印两核 PC 定位冻结点。
- **解码停顿看门狗**：播放中每 2 s 检查解码心跳，连续 12 s 无新帧即判定停摆，干净地停止播放并提示「播放无响应」，而不是让设备假播放静音（例如 SD 卡读卡卡死时）。
- **任务看门狗（防自旋）**：解码任务已显式订阅 ESP 任务看门狗（`CONFIG_ESP_TASK_WDT_EN`，5 s 超时即带 backtrace 重启）。停顿看门狗只是普通优先级任务，若解码任务在某一帧上 100% CPU 自旋会被它饿死而无法触发；任务看门狗由硬件定时器 ISR 驱动、与任务调度无关，能兜住这种「静默冻结」。配合解码层的零进度护栏（见 5.1），畸形帧不会再让设备卡死无响应。

---

## 6. 目录结构与模块接口

| 路径                               | 说明                                       |
| -------------------------------- | ---------------------------------------- |
| `main/main.c`                     | 启动装配 + LVGL 主循环                        |
| `components/board/board_config.h` | 板级硬件引脚、显示几何、分压系数                      |
| `components/drivers/buttons/`     | 9 键输入 + 10ms 消抖，LVGL keypad 读取回调          |
| `components/drivers/lcd/`         | ST7789 驱动 + LVGL 显示绑定 + 背光 PWM          |
| `components/drivers/audio/`       | I2S → MAX98357，路由/音量/采样率/ PCM 写入         |
| `components/drivers/bluetooth_audio/`    | 蓝牙 A2DP SOURCE + AVRCP + 扫描/配对/连接管理      |
| `components/drivers/battery/`     | 电池电压采样 + 开路电压查表              |
| `components/drivers/sd/`          | SDSPI 挂载状态与访问                          |
| `components/app/ui/`              | LVGL 分页 UI（播放器/电子书/设置/蓝牙）            |
| `components/app/player/`          | MP3 解码任务 + 后台曲库扫描                      |
| `components/app/ebook/`           | TXT 电子书读取与确定性分页                       |
| `components/fonts/`               | 中文+日文+韩文 CJK 点阵字体（16px，覆盖 4E00-9FFF 及假名） |

各模块的公共接口见其头文件（`*.h`），软件层（app）只通过硬件层（drivers）的公共 API 访问外设，不直接操作寄存器或 LVGL 显示内部。

---

## 7. 已知限制与备注

- 屏幕 TE 引脚未接 MCU，无法垂直同步；依赖双缓冲 + 部分刷新规避撕裂。
- 蓝牙路由下 `hw_audio_set_volume()` 改本地 BT 增益，并通过 AVRCP 绝对音量把等级镜像给 sink；远端用媒体键调节时本地只接收同步、不回显（避免环路）。若 sink 不支持绝对音量，则只有本地增益生效。
- AVRCP NEXT/PREV 仅在有曲目加载（播放中/暂停中）时切歌；空闲时忽略，不会用遥控器移动 UI 光标。
- 非阻塞日志钩子在 UART 发送堵塞（如 USB 转串口异常）时会**丢弃日志行**——这是刻意取舍：宁可丢日志也不让任何任务被日志路径卡死。
- 原 MicroPython 时代的 GD32 协处理器、电机、MPU6050、SugarASR 等功能不在本 ESP-IDF 固件范围内。

---

## 8. 参考文档

详见 `docs/`：

- `docs/ui.md` —— UI 页面与交互
- `docs/player.md` —— MP3 播放器
- `docs/ebook.md` —— 电子书阅读器
- `docs/audio.md` —— I2S/DSP/音量与输出链路
- `docs/bluetooth.md` —— A2DP/AVRCP 蓝牙音频
- `docs/hardware.md` —— 板级配置与驱动时序