# 小喵掌机 音频输出说明

> 本文档为**实现现状说明**（随代码更新）：`hardware/audio.c` 的 I2S 输出链路、保护滤波、音量模型与双路由。

## 1. 硬件链路

```text
MP3 解码（helix） ──> hw_audio_write_pcm() → 音量（15ms 平滑）
                    └─> 蓝牙已连接+开启 → A2DP（bt_audio_write_pcm，仅音量，全频段）
                    └─> 否则喇叭 → 700Hz 保护高通 → 256KB 环形缓冲
                                              → audio_feed 任务 → I2S(MAX98357)
```

- 输出 DAC：MAX98357 单声道 Class-D，I2S 标准模式（16-bit 立体声，只写 DOUT 声道），引脚 BCLK=25 / LRC=32 / DIN=33，无 MCLK。
- 蓝牙输出：A2DP Source，SBC 编码由 Bluedroid 完成（见 `docs/bluetooth.md`）。
- **路由互斥**：蓝牙连接时只走蓝牙，I2S 完全不喂数据（喇叭静音），解码任务由 BT 环形缓冲的阻塞发送单一时钟驱动，避免双时钟漂移丢音。

## 2. 音量模型

| 概念 | 说明 |
| ---- | ---- |
| 百分比 0..100 | UI 层音量（`hw_audio_get/set_volume`），NVS 持久化 |
| dB 锥度 | `gain_dB(v) = (v/100 - 1) × (-40dB)`，0dB@100%，每 5% ≈ 2dB，全量程恒定可闻 |
| 增益表 | 初始化时预计算 101 项 Q15 线性增益（`s_vol_tab`），运行时零浮点 |
| 平滑 | `VOL_RAMP_MS=15ms` 线性渐变到目标，避免音量突变爆音（点击）；路由切换/换曲时快照直达 |

- 蓝牙路由与喇叭路由**共用同一主音量**（无独立槽位；AVRCP 绝对音量 0..127 由应用层映射到 0..100% 后同样写入主音量）。

## 3. 喇叭保护高通（speaker HPF）

- 板载喇叭为小尺寸中频扬声器（有效 ~800Hz..6kHz，谐振 820..860Hz）。
- 每声道一阶 DC 阻断高通，截止 700Hz（`SPEAKER_HPF_FC_HZ`），Q15 递归系数仅采样率变化时重算：

```text
y[n] = (x[n] - x[n-1]) + lambda × y[n-1]
lambda = cos(w) - sqrt((1-cos(w)) × (3-cos(w))),  w = 2π·fc/fs
```

- 效果：滤除 DC 偏移与深低音，避免扬声器振膜无效行程与失真；换曲/路由切换时清空滤波器历史。

## 4. 解码/输出解耦（防爆音与欠载）

```text
PCM 环形缓冲 256KB（优先 PSRAM，失败退内部堆；>1s @44.1kHz 立体声）
  hw_audio_write_pcm() 阻塞写入（50ms 步长背压，播放停止时放弃剩余）
  audio_feed 任务（4KB 栈，优先级 6）→ I2S DMA 连续输出
```

- 停止/暂停：`hw_audio_set_player_active(false)` → feed 任务把环内剩余数据丢弃，立即静音（不等 1.5s 缓冲放完）。
- 采样率变更：`hw_audio_set_sample_rate()` 只写 `s_pending_rate`，由 feed 任务串行执行 `i2s_channel_reconfig_std_clock`（禁用→重配→启用），同时通知蓝牙管线内部重采样。

## 5. 接口摘要

```text
audio.c
  hw_audio_init()                    I2S 初始化 + 音量表 + HPF 系数 + feed 任务
  hw_audio_ready()                   初始化成功
  hw_audio_set/get_volume(pct)       主音量 0..100
  hw_audio_set_sample_rate(hz)       请求重配 I2S（feed 任务内串行执行）
  hw_audio_set_player_active(bool)   MP3 播放器声明 I2S 总线归属（停止即释放）
  hw_audio_write_pcm(frames,n)       就地滤波/音量后送入生效路由（BT 或喇叭环）
```
