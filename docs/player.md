# 小喵掌机 MP3 播放器说明

> 本文档为**实现现状说明**（随代码更新）：`components/app/player/player.c` 负责 MP3 文件扫描、解码与播放控制。

## 1. 职责与边界

- 解码：`libhelix-mp3`（chmorgan/esp-libhelix-mp3 1.0.3）软解码器。
- 输出：解码 PCM 交给 `components/drivers/audio/audio.c`（I2S + DSP + 音量，见 `docs/audio.md`）。
- 播放器**不触碰** LVGL；UI 通过 `player.h` 的公开 API 轮询状态。

```text
player.c
  ├─ mp3_player（16KB 栈，优先级 8，绑 Core 0）
  │    解码循环：读文件 → helix 解码 → hw_audio_write_pcm()（带背压）
  ├─ mp3_scan（4KB 栈，优先级 4，不绑核）
  │    后台扫描 PLAYER_ROOT(/sdcard) 的 .mp3 → 双缓冲快照发布曲目列表
  ├─ mp3_watch（4KB 栈，优先级 2，不绑核）
  │    解码停顿看门狗 + 喂 RTC WDT
  └─ 状态机       IDLE / PLAYING / PAUSED
```

- **双核分配**：Core 0 = `mp3_player`(8) 解码 + BT 控制器/协议栈 + main/`esp_timer` 等系统任务；Core 1 = `lvgl`(7) UI 渲染 + `i2s_wr`(8) 写 I2S DMA（`audio.c`，优先级高于同核 `lvgl`）。`mp3_scan`(4)、`mp3_watch`(2) 以及 ebook 的三个 4KB 后台任务都用 `xTaskCreate` 不绑核，由调度器在两核间迁移。
- **优先级只在同核内有效**：Core 1 上 `i2s_wr`(8) > `lvgl`(7)，保证调音量触发的全页重绘不会抢占 writer 超过 32KB PCM ring 的 ~186ms 裕量；Core 0 上解码 (8) 优先于落到该核的未绑核任务，但与 `lvgl` 不互相抢占。把 `lvgl`/`i2s_wr` 绑到 Core 1 是为了避开 Core 0 上高优先级的 BT 负载。
- 解码 PCM 缓冲：`s_pcm`（立体声，常见情况）**刻意留在内部 DRAM**——它是每 ~40Hz 交给 `hw_audio_write_pcm` 的缓冲，放 PSRAM 会把 cache-workaround 拷贝压到热路径上（蓝牙控制器负载下的已知崩溃源）；仅单声道上混用的 `s_stereo` 放 PSRAM（`EXT_RAM_BSS_ATTR`）以节省 DRAM 预算。

## 2. 曲目列表（后台扫描）

FATFS 目录遍历在 SDSPI 上耗时数十 ms，故列表由**独立扫描任务**构建，UI 永不阻塞：

| API | 说明 |
| ---- | ---- |
| `player_scan_start()` | 兼容别名，等价于 `player_load(PL_SRC_FOLDER, PLAYER_ROOT)`（全卡扫描）；新代码优先用下面的缓存/显式入口 |
| `player_scan_with_cache()` | 启动时调用：先校验 `/sdcard/.xueersi_playlist.cache` 的 `##SIG`/`##EOF`，命中则直接发布缓存列表，否则后台全卡扫描并重写缓存 |
| `player_rescan()` | 强制全卡重扫并更新缓存（设置页「刷新播放列表」） |
| `player_load(src, root)` | 加载指定来源/目录（UI 来源页用 `/sdcard/Music` 子树）；扫描在后台任务进行 |
| `player_scan_busy()` | 扫描中或已排队 |
| `player_scan_version()` | 每次完成扫描递增；UI 轮询以检测新列表 |
| `player_scan_count()` | 最近一次扫描的曲目数（最多 256 首，`PLAYER_SCAN_MAX`） |
| `player_scan_truncated()` | 扫描到上限时仍有更多 `.mp3` 则为 true；UI 显示「仅前 N 首(更多)」提示 |
| `player_scan_name(i)` | 第 i 首曲目名（越界返回 ""） |

- 扫描结果按 `strcasecmp` 排序（qsort），跨扫描稳定；同名文件再按完整路径比较，保证同名曲目相邻且顺序确定。双缓冲快照发布：先填工作区 → 复制到活动数组 → bump 版本。
- 缓存为 M3U8 文本 + `##SIG`（源签名）+ `##EOF <count>`（尾部条目数）；写入先落临时文件，逐项检查 `fprintf`/`fflush`/`fclose`，成功后再 rename 就位，避免掉电半写。读取时签名或计数不符即判损坏并回退全卡扫描。
- SD 卸载前由 `player_sd_release()` 停播并限时等待解码/扫描任务释放句柄；重插后经 `player_notify_sd_remount()` 放行，`hw_sd_generation()` 变化触发 UI 重扫。
- `player_init()` 启动时经 `player_scan_with_cache()` 预扫一次（此时 SD 已挂载）；UI 进播放页及检测到 SD 挂载/换卡变化时再请求重扫。
- 曲目全部来自 SD 卡（内置 ROM 曲目已随固件瘦身移除）。

## 3. 播放控制

| API | 说明 |
| ---- | ---- |
| `player_init()` | 创建解码 / 扫描 / 看门狗三个任务（app_main 启动时一次） |
| `player_state()` | 当前状态（IDLE / PLAYING / PAUSED） |
| `player_current_name()` | 当前载入曲目名（空闲为 ""） |
| `player_play(name)` | 以 `/` 开头按绝对路径播放，否则相对 `PLAYER_ROOT`（`/sdcard`）解析；UI 列表通常传 `player_scan_path(i)`。运行中调用即切歌 |
| `player_toggle()` | 播放 ⇄ 暂停 |
| `player_stop()` | 停止；释放 I2S 归属（`hw_audio_set_player_active(false)`）使解码背压立即退出并即时静音，解码循环退出时 `hw_audio_park()` 经 writer 分步排空 + 全环静默重写 + `disable/enable` re-arm 后冻结（避免下一首回放上首尾音），再通知任务 |
| `player_repeat_mode()` | 当前循环模式（`PLAYER_REPEAT_ALL` 列表循环 / `PLAYER_REPEAT_ONE` 单曲循环 / `PLAYER_REPEAT_RANDOM` 随机播放） |
| `player_repeat_toggle()` | 切换循环模式（UI 播放页按 Select 键触发）：列表→单曲→随机→列表 |

- 暂停/停止不阻塞：`hw_audio_write_pcm()` 在 `s_player_active=false` 时直接返回放弃剩余数据。
- 解码健壮性：除「无同步字 / 解码错误过多（512 次）」外，新增**零进度护栏**——连续 16 帧「解码成功但既没消耗字节也没产出样本」的畸形帧判定为损坏并跳过该曲，专门防 libhelix 在特定帧上原地自旋（CPU 跑满、把停顿看门狗饿死、无任何日志的「静默冻结」）。解码任务同时订阅 ESP 任务看门狗（5s 卡死即带 backtrace 重启）作为最后兜底。
- **LAME/Xing gapless**：`parse_gapless()` 在首帧 side-info 后找 Xing/Info + LAME 标签，取 encoder delay 高 12 位 + 解码器固定 529 样本延迟写入 `s_gapless_skip`；`decode_frame()` 按每声道样本数在首帧内裁掉预采样（立体声丢 L/R 对、单声道升混前丢），只丢整帧内部分，避免切到真实音频；无标签则跳过。打开曲目与单曲重播都会重新解析。
- 播放进度（字节偏移百分比）对 VBR MP3 不准确，UI 不展示（`player.h` 注释明示）。
- 采样率随首帧变化：`hw_audio_set_sample_rate()` 由解码任务同步调用。但 **I2S 通道不再重建**：通道在 init 时以 44100 建好并一路保持 RUNNING，非 44100 的音源由 `audio.c` 内的 SpeexDSP 定点重采样器（`quality=1`）软转（见 `docs/audio.md` §2）。重建通道会重置 DMA 描述符 credit 队列且不会 refill，是历史上"切歌后整条 I2S 静默卡死"的根因。

## 4. 循环模式与曲目切换

- 曲目`自然播放结束（EOF）`时的行为由循环模式决定（播放页按 Select 键切换，右上角状态栏显示当前模式）：
  - `PLAYER_REPEAT_ALL`（列表循环，默认）：切到列表下一首，末尾回绕到第一首；当前曲目不在列表（`s_index < 0`）或列表为空时停止。
  - `PLAYER_REPEAT_ONE`（单曲循环）：按当前曲目路径从头重播，不依赖播放列表。实现为**原地重绕**（`rewind_track()`）：`fseek` 回文件头 → 重建 helix 解码器（`MP3FreeDecoder` + `MP3InitDecoder`，避免第二遍复用残留的 bit-reservoir/VBR/重同步状态）→ 复位游标与看门狗 → 重新跳过 ID3v2 标签并拾取 ReplayGain → 重武装 I2S 管线。整个过程不停 I2S 时钟、不重开文件，循环**无缝**；重绕失败（seek/解码器重建失败）则落入错误路径自动切下一首，损坏文件不会死循环。
  - `PLAYER_REPEAT_RANDOM`（随机播放）：播放一个随机条目，列表多于一首时绝不重复刚结束的那首；列表仅一首时退化到该首。手动"下一曲"同样走随机（绝不重复当前），"上一曲"保持顺序以便用户随时回退。模式决策集中在 `decide_next_index()`，自然结束与手动切歌共用同一份逻辑，不会分叉。
- 手动导航：`player_play()` / `player_play_index()` / `player_next()` / `player_prev()` 均可从 UI 触发切歌。AVRCP NEXT/PREV 在 Bluedroid 任务里只投递一个方向值到长度 4 的队列，由 LVGL 任务取出后调用同一对 API（不在 BT 任务里碰播放器的非 volatile 索引状态）；空闲按键忽略。播放页每次刷新会把列表光标同步到 `player_current_index()`，所以遥控器切歌时高亮会跟着走。

## 5. 与其它模块的接口

```text
main.c:       player_init()（创建任务）；AVRCP PLAY/PAUSE/STOP → player_toggle/stop；
              NEXT/PREV → 队列 → LVGL 任务 → player_next/prev
ui.c:         播放器页构建/轮询时读 player_scan_count/name/version；按键 → player_play/toggle/stop
audio.c:      hw_audio_set_sample_rate() 随 MP3 采样率热重配（不重建通道，走 SpeexDSP 软重采样）；
              PCM 经 DSP 后入 32KB ringbuf，由独立 i2s_wr 任务（优先级 8，绑 Core 1）写 I2S DMA；
              切歌 flush 用代际计数确认（1200ms 上限 + 喂狗），路由 s_route 决定走喇叭还是 A2DP（见 audio.md）
bluetooth_audio.c:   路由为 AUDIO_ROUTE_BT 时，PCM 经音量后走 A2DP 而非 I2S（见 audio.md）
```
