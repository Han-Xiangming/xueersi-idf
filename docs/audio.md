# 小喵掌机 音频输出说明

> 本文档为**实现现状说明**（随代码更新）：`components/drivers/audio/audio.c` 的 I2S 输出链路、DSP 链、音量模型与双路由。

## 1. 硬件链路

```text
MP3 解码（helix） ──> hw_audio_write_pcm() → 按路由分发
  ├─ 蓝牙已连接 + 开启 → 音量（~5ms 平滑，全频段）→ bt_audio_write_pcm() → BT 环形缓冲(128KB) → A2DP
  └─ 喇叭 → 700Hz 保护高通 → 响度低音架 → 音量（平滑）→ 软限幅 → 256KB 环形缓冲
                                        → audio_feed 任务 → I2S(MAX98357)
```

- 输出 DAC：MAX98357 单声道 Class-D，I2S 标准模式（16-bit 立体声，只写 DOUT 声道），引脚 BCLK=25 / LRC=32 / DIN=33，无 MCLK。
- 蓝牙输出：A2DP Source，SBC 编码由 Bluedroid 完成（见 `docs/bluetooth.md`）。
- **路由互斥**：蓝牙连接时只走蓝牙，I2S 完全不喂数据（喇叭静音），解码任务由 BT 环形缓冲的阻塞发送单一时钟驱动，避免双时钟漂移丢音。
- **通道驻车**：空闲或蓝牙路由时 `audio_feed` 任务禁用 I2S 通道（停 BCLK/LRC），MAX98357 在时钟停止后进入掉电（约 64k BCLK 周期后关断）——菜单待机不空耗；快速续播有 3s 驻车宽限窗口，窗口内不重启时钟（避免 MAX98357 上电咔哒声）。

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
700Hz 保护高通 → 响度低音架（250Hz，boost 0→+9dB 随音量下降） → 主音量（~5ms 平滑） → 软限幅
```

- **喇叭保护高通**：板载喇叭为小尺寸中频扬声器（有效 ~800Hz..6kHz，谐振 820..860Hz）。每声道一阶 DC 阻断高通，截止 700Hz（`SPEAKER_HPF_FC_HZ`），Q15 递归系数仅采样率变化时重算：

  ```text
  y[n] = (x[n] - x[n-1]) + lambda × y[n-1]
  lambda = cos(w) - sqrt((1-cos(w)) × (3-cos(w))),  w = 2π·fc/fs
  ```

  滤除 DC 偏移与深低音，避免扬声器振膜无效行程与失真。
- **响度补偿（低音架）**：低音量时人耳对低频不敏感，一阶低通（`SPEAKER_LOUDNESS_FC_HZ=250Hz`）与主路并联，回加量 `boost(v) = 10^(dB/20) - 1`，`dB = (1 - v/100) × 9dB`——满音量 0dB，最小音量 +9dB，安静播放时补足低音。
- **软限幅**：峰值包络（瞬时起音 `LIM_ATT_Q15` ~0.5ms；`LIM_REL_Q15` ~100ms 释放）驱动限幅增益——包络低于阈值 30000（FS=32767）时 0dB 平直，高于阈值线性下降至满幅 ~0.9（`LOUD_LIMIT_SLOPE_Q15`），防热曲目削波，且不产生泵动。
- 换曲/路由切换时 `audio_dsp_reset()` 清空全部滤波历史（限幅器回到 0dB，新曲首帧不被上一曲的峰值包络压制）。

## 4. 解码/输出解耦（防爆音与欠载）

```text
PCM 环形缓冲 256KB（优先 PSRAM，失败退内部堆；>1s @44.1kHz 立体声）
  hw_audio_write_pcm() 阻塞写入（50ms 步长背压，播放停止时放弃剩余）
  audio_feed 任务（4KB 栈，优先级 6）→ I2S DMA 连续输出
```

- 停止/暂停：`hw_audio_set_player_active(false)` → feed 任务把环内剩余数据丢弃，立即静音（不等 1.5s 缓冲放完）。
- 采样率变更：`hw_audio_set_sample_rate()` 只写 `s_pending_rate`，由 feed 任务串行执行 `i2s_channel_reconfig_std_clock`（禁用→重配→启用），同时通知蓝牙管线内部重采样。
- 蓝牙路由时 feed 任务只清空 256KB 环（不入 I2S），并保持通道驻车（见 §1）。

## 5. 接口摘要

```text
components/drivers/audio/audio.c
  hw_audio_init()                        I2S 初始化 + 音量表 + DSP 系数 + feed 任务
  hw_audio_set/get_volume(pct)           当前生效路由的音量 0..100
  hw_audio_set/get_speaker_volume(pct)   喇叭路由音量槽位（NVS 恢复用）
  hw_audio_set/get_bt_volume(pct)        蓝牙路由音量槽位
  hw_audio_set_avrc_volume(v)            AVRCP 绝对音量 0..127（写蓝牙槽位）
  hw_audio_set_sample_rate(hz)           请求重配 I2S（feed 任务内串行执行）
  hw_audio_set_player_active(bool)       MP3 播放器声明 I2S 总线归属（停止即释放）
  hw_audio_write_pcm(frames,n)           就地 DSP 后送入生效路由（BT 或喇叭环）
```
