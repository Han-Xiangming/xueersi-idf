# 小喵掌机 音频输出说明

> 本文档为**实现现状说明**（随代码更新）：`components/drivers/audio/audio.c` 的 I2S 输出链路、DSP 链、音量模型与双路由。

## 1. 硬件链路

```text
MP3 解码（helix） ──> hw_audio_write_pcm() → 按路由分发
  ├─ 蓝牙已连接 + 开启 → 音量（~5ms 平滑，全频段）→ bt_audio_write_pcm() → BT 环形缓冲(128KB) → A2DP
  └─ 喇叭 → 800Hz 保护高通 → 响度低音架 → 音量（平滑）→ 软限幅 → xRingbufferSend() 送 PCM 环形缓冲
                                       → DMA writer 任务 → i2s_channel_write() → I2S(MAX98357)
```

- 输出 DAC：MAX98357 单声道 Class-D，I2S 标准模式（16-bit 立体声，只写 DOUT 声道），引脚 BCLK=32 / LRC=15 / DIN=21，无 MCLK。
- 蓝牙输出：A2DP Source，SBC 编码由 Bluedroid 完成（见 `docs/bluetooth.md`）。
- **路由互斥**：蓝牙连接时只走蓝牙，I2S 不喂数据（喇叭静音），解码任务由 BT 环形缓冲的阻塞发送单一时钟驱动，避免双时钟漂移丢音。
- **环形缓冲 + 独立 DMA writer 任务（无 ADF 中间层）**：MP3 解码任务在 DSP 后把 PCM 推入一个 8KB 的 FreeRTOS 字节环形缓冲（`xRingbufferSend()`，满则阻塞——天然背压）；一个专用 writer 任务（`i2s_wr`，优先级 9、绑定 core 1，高于解码任务 8 与 LVGL 7）独立地从 ring 取出 PCM 喂 `i2s_channel_write()`；钉在 core 1 是为了避开 core 0 上的 WiFi/蓝牙控制器高优先级任务抢占导致 DMA 欠载卡顿。I2S DMA（12×1024 帧，`auto_clear`）即抖动缓冲，writer 任务按硬件时钟定速取走 ring 中的数据——解码快则 ring 背压堵住解码，解码慢则 writer 发数字静音填空，解码与 DMA 不会互相跑飞。
- **通道驻车（由 writer 任务控制，有意为之的代价）**：`i2s_std` 频道在 `hw_audio_init()` 时 `i2s_channel_enable()` 一次后**常驻运行**；只有两种情形会由 **DMA writer 任务**在下一轮循环里 `i2s_channel_disable()` 关掉通道（停 BCLK/LRC、MAX98357 掉电）：(1) **切到蓝牙路由**（喇叭静音、省电）；(2) **整段停止** `hw_audio_park()`（B 键退解码循环 / 看门狗停摆）。speaker 路由下、播放未真正结束（暂停 / 曲间 / 仅 `player_active=false`）时频道**保持启用**，writer 任务以 `auto_clear` 数字静音填充环形缓冲——代价是 BCLK 持续、MAX98357 不进掉电（几 mA），换取绝不触发会卡死 ESP32 DMA 的 out-link stop/start。**停止先排净余音**：`hw_audio_set_player_active(false)` 不立即关通道，而是让解码任务下一帧写返回 `AUDIO_WRITE_ABANDONED` 即时静音；整圈 DMA 环形缓冲（12×1024 帧）靠 `auto_clear` 把已传输描述符清零、在「下次播放之前」自然排净成静音，`hw_audio_park()` 再 `i2s_channel_disable()` 冻结的是静音而非上一首尾音（见 §4）。所有 I2S 通道操作都收敛到这一个 writer 任务内部，天然串行，无需互斥锁；跨任务停止安全（IDF 驱动在 `i2s_channel_disable` 中置 READY 并等待在途写循环退出）。

## 2. 音量模型

| 概念 | 说明 |
| ---- | ---- |
| 百分比 0..100 | UI 层音量（`hw_audio_get/set_volume`），NVS 持久化 |
| 每路由独立槽位 | 喇叭与蓝牙各一个（`hw_audio_set/get_speaker_volume`、`hw_audio_set/get_bt_volume`）；`hw_audio_get/set_volume` 操作**当前生效路由**的槽位，路由切换时 `hw_audio_set_route()` 切换生效槽位 |
| dB 锥度 | `gain_dB(v) = (v/100 - 1) × (-40dB)`，0dB@100%，每 5% ≈ 2dB，全量程恒定可闻 |
| 增益表 | 初始化时预计算 **401 项**（0.1dB 步进，`VOL_TAB_ENTRIES=401`）Q15 线性增益（`s_vol_tab`），运行时零浮点；AVRCP 0..127 满刻度也走同一 0.1dB 表，每步 ≈ 0.32dB |
| 平滑 | 一阶 **~5ms** 时间常数（`VOL_SMOOTH_A_Q15`）线性渐变到目标，避免音量突变爆音（点击）；路由切换/换曲时增益快照直达 |

- **路由切换**（喇叭⇄蓝牙）：由调用方通过 `hw_audio_set_route()` 完成，内部切到新路由的音量槽位并更新 `s_volume`，再 `audio_dsp_reset()` 清空滤波历史，`s_vol_gain_sm` 快照直达（不淡入），写入侧平滑掩盖切换瞬间。
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
环形缓冲：hw_audio_write_pcm() 就地 DSP → xRingbufferSend() 送 PCM ring（100ms 有界超时，背压阻塞）
  DMA writer 任务 → i2s_channel_write() 喂 I2S DMA（12×1024 帧 ≈ 279ms @44.1kHz，auto_clear 欠载自动静音）
  解码任务被 ring 背压钉在硬件时钟附近（ring 满则阻塞解码；ring 空则 writer 发静音，绝无超前）
```

- 停止/暂停：`hw_audio_set_player_active(false)` 声明播放器不再写数据，解码任务下一帧写返回 `AUDIO_WRITE_ABANDONED` 即时静音；但通道**不立即禁用**——靠 `auto_clear` 把整圈 DMA 环形缓冲在「下次播放之前」排净成静音（否则上一首尾音会在下一首重新使能通道时被回放，即「余音」）。整段停止（B 键 / 看门狗）由 `hw_audio_park()` 在排净后才 `i2s_channel_disable()`。
- **采样率处理（软件重采样，不重建通道）**：I2S 通道在初始化时以**单一固定速率**（`AUDIO_DEFAULT_RATE`，当前 44100 Hz）创建并启用一次，之后整段会话**绝不**因采样率变化而 disable/rebuild。解码任务每曲首帧调用 `hw_audio_set_sample_rate()` 仅声明解码器的原生码率；若该码率与固定 I2S 速率不同，`hw_audio_write_pcm()` 在写 DMA 前用 **SpeexDSP 定点重采样器**（cubic-interpolated sinc，`RESAMP_QUALITY=2`，每曲重建以重置接缝历史，同速率曲库直接旁路，零开销）把 PCM 重采样到固定速率。这样曲目接缝处的换速只是一次重采样，不触碰 I2S 通道——彻底消除了"运行通道 disable→enable"这一会卡死 ESP32 DMA 的 out-link stop/start（即"首曲有声、之后整片无声、仅 I2S 受影响"的失败模式：随机/单曲循环选到不同码率文件时原本会在接缝触发通道重建）。变调问题不存在：重采样保证音高正确，且 DSP 链系数始终按固定 I2S 速率计算。i2s_std 频道在初始化时一次性启用并常驻运行,不再有通道重建/自愈逻辑;独立的 DMA writer 任务喂 DMA,若 DMA 真卡死只能 terminate+reinit(当前未实现自愈,须避免该路径)。
- **播放时钟对齐（取舍说明）**：MP3 解码速率精确而 I2S BCLK（APLL 派生）仅 ppm 级精度，旧架构用 ring+feed+插样主动抵消漂移；新架构（ringbuf + 独立 writer 任务）下该漂移由 DMA 吸收，最坏表现为长时间播放中偶发一次约几十 ms 的欠载静音（`auto_clear` 兜底、自恢复），远轻于旧架构概率性整首无声。
- **通道生命周期**：频道在 `hw_audio_init()` 时启用一次并常驻（init 后 writer 任务即开始以静音驱动 BCLK，无起始 auto-clear 空白）；禁用仅发生在**切到蓝牙路由**或**整段停止 `hw_audio_park()`**——暂停与曲间不禁用，仅以静音填充环形缓冲。全部 I2S 通道操作（enable/disable/写 ring）都收敛到唯一的 DMA writer 任务内部，天然串行，无需互斥锁；跨任务停止安全（IDF 驱动在 `i2s_channel_disable` 中置 READY 并等待在途写循环退出）。
- 蓝牙路由时 I2S 通道保持驻车（见 §1），蓝牙侧由 A2DP 流控自行定钟。

## 5. 接口摘要

```text
components/drivers/audio/audio.c
  hw_audio_init()                        I2S 初始化 + 音量表 + DSP 系数 + 启动 DMA writer 任务（通道常驻）
  hw_audio_set/get_volume(pct)           当前生效路由的音量 0..100
  hw_audio_set/get_speaker_volume(pct)   喇叭路由音量槽位（NVS 恢复用）
  hw_audio_set/get_bt_volume(pct)        蓝牙路由音量槽位
  hw_audio_set_avrc_volume(v)            AVRCP 绝对音量 0..127（写蓝牙槽位）
  hw_audio_set_track_gain_db(dB)         每曲 ReplayGain（两路由通用）
  hw_audio_set_sample_rate(hz)           声明解码器原生码率（内部按固定 I2S 速率重采样，绝不重建通道）
  hw_audio_set_player_active(bool)       MP3 播放器声明 I2S 总线归属（释放后不关通道，靠静音填充；真正停止由 hw_audio_park()）
  hw_audio_write_pcm(frames,n)           就地 DSP 后推入 PCM 环形缓冲（或送 BT 路由），由 writer 任务喂 DMA
```
