# 小喵掌机 MP3 播放器说明

> 本文档为**实现现状说明**（随代码更新）：`components/app/player/player.c` 负责 MP3 文件扫描、解码与播放控制。

## 1. 职责与边界

- 解码：`libhelix-mp3`（chmorgan/esp-libhelix-mp3 1.0.3）软解码器。
- 输出：解码 PCM 交给 `components/drivers/audio/audio.c`（I2S + DSP + 音量，见 `docs/audio.md`）。
- 播放器**不触碰** LVGL；UI 通过 `player.h` 的公开 API 轮询状态。

```text
player.c
  ├─ player_task（16KB 栈，优先级 5）
  │    解码循环：读文件 → helix 解码 → hw_audio_write_pcm()（带背压）
  ├─ scan_task（4KB 栈，优先级 4）
  │    后台扫描 /sdcard/Music 的 .mp3 → 双缓冲快照发布曲目列表
  └─ 状态机       IDLE / PLAYING / PAUSED
```

- 解码 PCM 缓冲（`s_pcm` / `s_stereo`）放 PSRAM（`EXT_RAM_BSS_ATTR`），不占内部 DRAM。

## 2. 曲目列表（后台扫描）

FATFS 目录遍历在 SDSPI 上耗时数十 ms，故列表由**独立扫描任务**构建，UI 永不阻塞：

| API | 说明 |
| ---- | ---- |
| `player_scan_start()` | 请求后台扫描 `/sdcard/Music` 的 `.mp3`（忽略大小写）；扫描进行中的请求合并为一次后续扫描 |
| `player_scan_busy()` | 扫描中或已排队 |
| `player_scan_version()` | 每次完成扫描递增；UI 轮询以检测新列表 |
| `player_scan_count()` | 最近一次扫描的曲目数（最多 64 首，`PLAYER_SCAN_MAX`） |
| `player_scan_name(i)` | 第 i 首曲目名（越界返回 ""） |

- 扫描结果按 `strcasecmp` 排序（qsort），跨扫描稳定；双缓冲快照发布：先填工作区 → 复制到活动数组 → bump 版本。
- `player_init()` 启动时预扫一次（此时 SD 已挂载）；UI 进播放页及检测到 SD 挂载变化（`ui_external_changed`）时再请求重扫。
- 曲目全部来自 SD 卡（内置 ROM 曲目已随固件瘦身移除）。

## 3. 播放控制

| API | 说明 |
| ---- | ---- |
| `player_init()` | 创建解码任务 + 扫描任务（app_main 启动时一次） |
| `player_state()` | 当前状态（IDLE / PLAYING / PAUSED） |
| `player_current_name()` | 当前载入曲目名（空闲为 ""） |
| `player_play(name)` | 播放 `/sdcard/Music/<name>`；运行中调用则切换曲目 |
| `player_toggle()` | 播放 ⇄ 暂停 |
| `player_stop()` | 停止；先释放 I2S 归属（`hw_audio_set_player_active(false)`）使解码背压立即退出，再通知任务 |
| `player_repeat_mode()` | 当前循环模式（`PLAYER_REPEAT_ALL` 列表循环 / `PLAYER_REPEAT_ONE` 单曲循环） |
| `player_repeat_toggle()` | 切换循环模式（UI 播放页按 Select 键触发） |

- 暂停/停止不阻塞：`hw_audio_write_pcm()` 的环形缓冲背压在 `s_player_active=false` 时立即放弃剩余数据。
- 播放进度（字节偏移百分比）对 VBR MP3 不准确，UI 不展示（`player.h` 注释明示）。
- 采样率随首帧变化：`hw_audio_set_sample_rate()` 由解码任务调用，I2S 重配在 audio feed 任务内串行执行。

## 4. 循环模式与曲目切换

- 曲目**自然播放结束（EOF）**时的行为由循环模式决定（播放页按 Select 键切换，右上角状态栏显示当前模式）：
  - `PLAYER_REPEAT_ALL`（列表循环，默认）：切到列表下一首，末尾回绕到第一首；当前曲目不在列表（`s_index < 0`）或列表为空时停止。
  - `PLAYER_REPEAT_ONE`（单曲循环）：按当前曲目路径从头重播，不依赖播放列表。
- 播放器没有手动播放列表导航 API：`player_play()` 每次都是新曲目；AVRCP 的 NEXT/PREV 回调在 main.c 中被忽略（仅记日志）。

## 5. 与其它模块的接口

```text
main.c:       player_init()（创建任务）；AVRCP PLAY/PAUSE/STOP → player_toggle/stop
ui.c:         播放器页构建/轮询时读 player_scan_count/name/version；按键 → player_play/toggle/stop
audio.c:      hw_audio_set_sample_rate() 随 MP3 采样率重配 I2S；PCM 经 256KB 环形
              缓冲由 feed 任务写 I2S；蓝牙连接时路由到 A2DP（见 audio.md）
bt_audio.c:   蓝牙开启且已连接时，PCM 经音量后路由到 A2DP 而非 I2S（见 audio.md）
```
