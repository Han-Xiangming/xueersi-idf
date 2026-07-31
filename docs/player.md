# 小喵掌机 MP3 播放器说明

> 本文档为**实现现状说明**（随代码更新）：`app/player.c` 负责 MP3 文件扫描、解码与播放控制。

## 1. 职责与边界

- 解码：`libhelix-mp3`（chmorgan/esp-libhelix-mp3 1.0.3）软解码器。
- 输出：解码 PCM 交给 `hardware/audio.c`（I2S + 保护高通 + 音量，见 `docs/audio.md`）。
- 播放器**不触碰** LVGL；UI 通过 `player.h` 的公开 API 轮询状态。

```text
player.c
  ├─ player_task（16KB 栈，优先级 5）
  │    解码循环：读文件 → helix 解码 → hw_audio_write_pcm()（带背压）
  └─ 状态机       IDLE / PLAYING / PAUSED
```

## 2. 曲目列表（同步扫描）

| API | 说明 |
| ---- | ---- |
| `player_scan(names, max, &count)` | 同步扫描 `/sdcard` 的 `.mp3`（忽略大小写），最多 `max` 首，返回 0 |

- 由 UI 在进入播放器页时调用一次，结果存 UI 侧 PSRAM（`s_mp3_names`）。
- 内置 ROM 曲目（`EMBED_FILES "tracks/Test.mp3"`，符号按**文件名**生成 `_binary_Test_mp3_*`）自动追加为列表末尾 `(ROM) Test.mp3`，无 SD 卡也可播。

## 3. 播放控制

| API | 说明 |
| ---- | ---- |
| `player_init()` | 创建解码任务（app_main 启动时一次） |
| `player_state()` | 当前状态（IDLE / PLAYING / PAUSED） |
| `player_current_name()` | 当前载入曲目名（空闲为 ""） |
| `player_play(name)` | 播放 `/sdcard/<name>` 或 `(ROM)` 内置曲目；运行中调用则切换曲目 |
| `player_toggle()` | 播放 ⇄ 暂停 |
| `player_stop()` | 停止；先释放 I2S 归属（`hw_audio_set_player_active(false)`）使解码背压立即退出，再通知任务 |

- 暂停/停止不阻塞：`hw_audio_write_pcm()` 的环形缓冲背压在 `s_player_active=false` 时立即放弃剩余数据。
- 播放进度（字节偏移百分比）对 VBR MP3 不准确，UI 不展示（`player.h` 注释明示）。
- 采样率随首帧变化：`hw_audio_set_sample_rate()` 由解码任务调用，I2S 重配在 audio feed 任务内串行执行。

## 4. 曲目切换

- 播放器**没有播放列表导航**：`player_play()` 每次都是新曲目；AVRCP 的 NEXT/PREV 回调在 main.c 中被忽略（仅记日志）。

## 5. 与其它模块的接口

```text
main.c:       player_init()（创建任务）；AVRCP PLAY/PAUSE/STOP → player_toggle/stop
ui.c:         播放器页构建时 player_scan()；按键 → player_play/toggle/stop
audio.c:      hw_audio_set_sample_rate() 随 MP3 采样率重配 I2S；PCM 经 256KB 环形
              缓冲由 feed 任务写 I2S；蓝牙连接时路由到 A2DP（见 audio.md）
bt_audio.c:   蓝牙开启且已连接时，PCM 经音量后路由到 A2DP 而非 I2S（见 audio.md）
```
