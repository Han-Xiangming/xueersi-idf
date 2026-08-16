# 小喵掌机 音频输出说明

> 本文档为**实现现状说明**（随代码更新）：`components/drivers/audio/audio.c` 的 I2S 输出链路、DSP 链、音量模型与双路由。

## 1. 硬件链路

```text
MP3 解码（helix） ──> hw_audio_write_pcm() → 按路由分发
  ├─ 蓝牙已连接 + 开启 → 音量（~5ms 平滑，全频段）→ bt_audio_write_pcm() → BT 环形缓冲(128KB) → A2DP
  └─ 喇叭 → 800Hz 保护高通 → 响度低音架 → 音量（平滑）→ 软限幅 → i2s_channel_write() 直写 DMA
                                       → I2S(MAX98357)
```

- 输出 DAC：MAX98357 单声道 Class-D，I2S 标准模式（16-bit 立体声，只写 DOUT 声道），引脚 BCLK=32 / LRC=15 / DIN=21，无 MCLK。
- 蓝牙输出：A2DP Source，SBC 编码由 Bluedroid 完成（见 `docs/bluetooth.md`）。
- **路由互斥**：蓝牙连接时只走蓝牙，I2S 不喂数据（喇叭静音），解码任务由 BT 环形缓冲的阻塞发送单一时钟驱动，避免双时钟漂移丢音。
- **直写 I2S（无中间层）**：MP3 解码任务在 DSP 后就地 `i2s_channel_write()` 直写 I2S DMA——没有环形缓冲、没有 feed 任务、没有任务间握手。I2S DMA（8×1024 帧，`auto_clear`）即抖动缓冲，写阻塞的背压把解码节奏钉死在硬件时钟上，解码与硬件不会互相跑飞。
- **通道驻车（简单开关）**：空闲或蓝牙路由时通道保持禁用（停 BCLK/LRC），MAX98357 在时钟停止后进入掉电（约 64k BCLK 周期后关断）——菜单待机不空耗。播放时由首个 PCM 帧的写入启用通道（采样率重配之后，时钟永不先于数据启动）；停止/切路由时立即驻车。所有通道操作由 `s_io_lock` 互斥锁串行化，IDF 驱动本身会在通道禁用时放行在途写调用（`i2s_channel_disable` 置 READY 并等写循环退出），因此任意任务随时可停。

## 2. 音量模型

| 概念 | 说明 |
| ---- | ---- |
| 百分比 0..100 | UI 层音量（`hw_audio_get/set_volume`），NVS 持久化 |
| 每路由独立槽位 | 喇叭与蓝牙各一个（`hw_audio_set/get_speaker_volume`、`hw_audio_set/get_bt_volume`）；`hw_audio_get/set_volume` 操作**当前生效路由**的槽位，路由切换时 `audio_select_route()` 切换生效槽位 |
| dB 锥度 | `gain_dB(v) = (v/100 - 1) × (-40dB)`，0dB@100%，每 5% ≈ 2dB，全量程恒定可闻 |
| 增益表 | 初始化时预计算 **401 项**（0.1dB 步进，`VOL_TAB_ENTRIES=401`）Q15 线性增益（`s_vol_tab`），运行时零浮点；AVRCP 0..127 满刻度也走同一 0.1dB 表，每步 ≈ 0.32dB |
| 平滑 | 一阶 **~5ms** 时间常数（`VOL_SMOOTH_A_Q15`）线性渐变到目标，避免音量突变爆音（点击）；路由切换/换曲时增益快照直达 |

- **路由切换**（喇叭⇄蓝牙）：`hw_audio_write_pcm()` 检测到路由翻转时先 `audio_select_route()` 切到新路由的音量槽位，再 `audio_dsp_reset()` 清空滤波历史，`s_vol_gain_sm` 快照直达（不淡入），写入侧平滑掩盖切换瞬间。
- **AVRCP 绝对音量**（0..127）：`hw_audio_set_avrc_volume()` 直接按 0..127 满刻度查 0.1dB 表（每步 ≈ 0.32dB），写蓝牙槽位并更新百分比视图（映射 `((v×100)+63)/127`），与本地按键共用同一 dB 锥度表。

## 3. DSP 链（喇叭路由，逐样本）

```text
800Hz 保护高通 → 响度低音架（800Hz，boost 0→+9dB 随音量下降） → 主音量（~5ms 平滑) → 软限幅
```

- **喇叭保护高通**：板载喇叭为手机跑道型小单元（有效 ~800Hz..8kHz，谐振峰值 850..920Hz）。每声道一阶 DC 阻断高通，截止频率 800Hz（`SPEAKER_HPF_FC_HZ`），Q15 递归系数仅采样率变化时重算：

  ```text
  y[n] = (x[n] - x[n-1]) + lambda × y[n-1]
  lambda = cos(w) - sqrt((1-cos(w)) × (3-cos(w))),  w = 2π·fc/fs
  ```

  滤除 DC 偏移与深低音，避免扬声器振膜无效行程与失真。
- **响度补偿（低音架）**：低音量时人耳对低频不敏感，一阶低通（`SPEAKER_LOUDNESS_FC_HZ=800Hz`，贴合跑道单元可用频段下沿）与主路并联，回加量 `boost(v) = 10^(dB/20) - 1`，`dB = (1 - v/100) × 9dB`——满音量 0dB，最小音量 +9dB，安静播放时补足低音。
- **软限幅**：峰值包络（瞬时起音 `LIM_ATT_Q15` ~0.5ms；`LIM_REL_Q15` ~100ms 释放）驱动限幅增益——包络低于阈值 27000（FS=32767，比旧值 30000 更早介入，保护 ±0.22mm 小行程单元破音）时 0dB 平直，高于阈值线性下降至满幅 ~0.9（`LOUD_LIMIT_SLOPE_Q15`），防热曲目削波，且不产生泵动。
- 换曲/路由切换时 `audio_dsp_reset()` 清空全部滤波历史（限幅器回到 0dB，新曲首帧不被上一曲的峰值包络压制）。

## 4. 解码/输出解耦（防爆音与欠载）

```text
直写：hw_audio_write_pcm() 就地 DSP → i2s_channel_write()（100ms 有界超时）
  I2S DMA（8×1024 帧 ≈ 186ms @44.1kHz，auto_clear 欠载自动静音）即抖动缓冲
  DMA 写阻塞的背压 = 解码任务的自然节拍（写不满则 DMA 空转，绝无超前）
```

- 停止/暂停：`hw_audio_set_player_active(false)` 立即禁用通道（停 BCLK），解码任务下次写返回 `AUDIO_WRITE_ABANDONED`，瞬时静音、无缓冲残留。
- **采样率即时生效**：`hw_audio_set_sample_rate()` 由解码任务同步执行（运行中先驻车 → `i2s_channel_reconfig_std_clock` → 下个写帧再启用），新曲目的时钟必然先于数据就位；不存在旧架构"待应用 rate 被暂停清掉"导致整曲变调的问题，重配返回值也会被检查并上报。
- **播放时钟对齐（取舍说明）**：MP3 解码速率精确而 I2S BCLK（APLL 派生）仅 ppm 级精度，旧架构用 ring+feed+插样主动抵消漂移；直写模式下该漂移由 DMA 吸收，最坏表现为长时间播放中偶发一次约几十 ms 的欠载静音（`auto_clear` 兜底、自恢复），远轻于旧架构概率性整首无声。
- **通道生命周期**：启用仅发生在首个 PCM 写帧（采样率重配之后），BCLK 永不先于数据启动（无起始 auto-clear 空白）；禁用发生在停止/暂停/切到蓝牙路由时。全部通道操作（enable/disable/重配/写）由 `s_io_lock` 互斥锁串行，跨任务停止安全（IDF 驱动在 `i2s_channel_disable` 中置 READY 并等待在途写循环退出）。
- 蓝牙路由时 I2S 通道保持驻车（见 §1），蓝牙侧由 A2DP 流控自行定钟。

## 5. 接口摘要

```text
components/drivers/audio/audio.c
  hw_audio_init()                        I2S 初始化 + 音量表 + DSP 系数 + io 互斥锁（通道驻车）
  hw_audio_set/get_volume(pct)           当前生效路由的音量 0..100
  hw_audio_set/get_speaker_volume(pct)   喇叭路由音量槽位（NVS 恢复用）
  hw_audio_set/get_bt_volume(pct)        蓝牙路由音量槽位
  hw_audio_set_avrc_volume(v)            AVRCP 绝对音量 0..127（写蓝牙槽位）
  hw_audio_set_track_gain_db(dB)         每曲 ReplayGain（两路由通用）
  hw_audio_set_sample_rate(hz)           立即重配 I2S 采样率（解码任务同步执行）
  hw_audio_set_player_active(bool)       MP3 播放器声明 I2S 总线归属（停止即驻车）
  hw_audio_write_pcm(frames,n)           就地 DSP 后直写 I2S DMA（或送 BT 路由）
```
