# 小喵掌机 音频输出说明

> 本文档为**实现现状说明**（随代码更新）：`components/drivers/audio/audio.c` 的 I2S 输出链路、DSP 链、音量模型与双路由。

## 1. 硬件链路

```text
MP3 解码（helix） ──> hw_audio_write_pcm() → 按路由分发
 ├─ 蓝牙已连接 + 开启 → 音量（~5ms 平滑，全频段）→ bluetooth_audio_write_pcm() → BT 环形缓冲(128KB) → A2DP
 └─ 喇叭 → 800Hz 保护高通 → 音量（平滑）→ 响度低音架 → ReplayGain/主增益 → 双频段 DRC（400Hz 分频，各段软拐点压缩）→ 硬顶棚 → (L+R)/2 单声道下混 → 复制到 L/R 两个 slot → xRingbufferSend() 送 PCM 环形缓冲
                                       → DMA writer 任务 → i2s_channel_write() → I2S(MAX98357)
```

- 输出 DAC：MAX98357 单声道 Class-D，I2S 标准模式（16-bit stereo slot），引脚 BCLK=32 / LRC=15 / DIN=21，无 MCLK。MAX98357 的 `SD_MODE` 可选左/右/(L+R)/2；固件在喇叭路径统一做 `(L+R)/2` 下混并把同一 mono 样本写进两个 slot，因此不依赖板上 `SD_MODE` 接法，左右声道交替的音源也不会听成静音。蓝牙路径保留完整立体声。
- 蓝牙输出：A2DP Source，SBC 编码由 Bluedroid 完成（见 `docs/bluetooth.md`）。
- **路由互斥**：蓝牙连接时只走蓝牙，I2S 不喂数据（喇叭静音），解码任务由 BT 环形缓冲的阻塞发送单一时钟驱动，避免双时钟漂移丢音。
- **环形缓冲 + 独立 DMA writer 任务（无 ADF 中间层）**：MP3 解码任务在 DSP 后把 PCM 推入一个 32KB 的 FreeRTOS 字节环形缓冲（`xRingbufferSend()`，100ms 有界超时，满则阻塞——天然背压；~186ms @44.1kHz，叠加 DMA 的 ~279ms 共 ~465ms 生产者停摆裕量，可吸收一次 SD 读重试退避）；一个专用 writer 任务（`i2s_wr`，优先级 8，绑 Core 1，高于同核 `lvgl` 的 7）独立地从 ring 取出 PCM 喂 `i2s_channel_write()`。I2S DMA（12×1024 帧，`auto_clear`）即抖动缓冲，writer 任务按硬件时钟定速取走 ring 中的数据——解码快则 ring 背压堵住解码，解码慢则 writer 发数字静音填空，解码与 DMA 不会互相跑飞。
- **通道驻车与无缝切换（由 writer 任务控制）**：`i2s_std` 频道在 `hw_audio_init()` 时启用一次并常驻；speaker 路由下暂停/曲间（`player_active=false`）不关闭，只以静音填充。只有切到蓝牙路由或整段停止（`hw_audio_park()`）才由 writer 关掉通道（停 BCLK/LRC、MAX98357 掉电）。切歌/重播的接缝不再只靠 `auto_clear` 自然排净：`i2s_seam_rearm()` 先用 2KB 静音块循环**写满整圈 48KB DMA 描述符环**（约 280ms），再 `disable()+enable()` 重置 out-link，最后补 2KB 静音 prime；硬切换前还有 64 阶 Q15 余弦表的 20ms 淡出，避免直接丢弃旧 PCM 产生 click；park 排空改为 20ms 分步循环。所有 I2S 通道操作都收敛到唯一 writer 任务内部，天然串行，无需互斥锁；跨任务停止安全。

## 2. 音量模型

| 概念 | 说明 |
| ---- | ---- |
| 百分比 0..100 | UI 层音量（`hw_audio_get/set_volume`），NVS 持久化 |
| 每路由独立槽位 | 喇叭与蓝牙各一个（`hw_audio_set/get_speaker_volume`、`hw_audio_set/get_bt_volume`）；`hw_audio_get/set_volume` 操作**当前生效路由**的槽位，路由切换时 `hw_audio_set_route()` 切换生效槽位 |
| dB 锥度 | `gain_dB(v) = (v/100 - 1) × depth(route)`，0dB@100%；dB 对百分比线性，故定 dB 步进 = 定百分比步进。**按路由双深度**：喇叭 0..-40dB（功放可用范围，每 5% = 2dB）；蓝牙 0..-12dB（sink 经 AVRCP 镜像同一百分比后会**再衰减一次**，深本地锥度相乘导致旋钮低端无声——25% 曾为 -30dB 本地 × sink 自身 ≈-25dB ≈ -55dB，现为 -9dB 本地） |
| 按键步进 | UI 侧 **1dB/按**，经 `hw_audio_volume_step_db()` 在 dB 域步进后取整回百分比（往返按当前路由深度）；1dB 为人耳 JND，每按可闻且近静音可精细微调；蓝牙路由深度仅 12dB，1dB ≈ 旋钮 8%；按住加速见 `docs/ui.md` §4 |
| 增益表 | 初始化时预计算两张 0.1dB Q15 线性增益表：喇叭 **401 项**（0..-40dB，`s_vol_tab`）+ 蓝牙 **121 项**（0..-12dB，`s_vol_tab_bt`），运行时零浮点；AVRCP 0..127 满刻度走蓝牙表，每步 ≈ 0.09dB（12/127） |
| 平滑 | 一阶 **~5ms** 时间常数（`VOL_SMOOTH_A_Q15`）线性渐变到目标，避免音量突变爆音（点击）；路由切换/换曲时增益快照直达 |

- **路由切换**（喇叭⇄蓝牙）：由调用方通过 `hw_audio_set_route()` 完成，内部切到新路由的音量槽位并更新 `s_volume`，再 `audio_dsp_reset()` 清空滤波历史，`s_vol_gain_sm` 快照直达（不淡入），写入侧平滑掩盖切换瞬间。
- **AVRCP 绝对音量**（0..127）：`hw_audio_set_avrc_volume()` 按 0..127 满刻度查蓝牙路由 0.1dB 表（每步 ≈ 0.09dB，满量程 12dB），写蓝牙槽位并更新百分比视图（映射 `((v×100)+63)/127`）。

## 3. DSP 链（喇叭路由，逐样本）

```text
800Hz 保护高通 → 主音量（~5ms 平滑) → 响度低音架（800Hz，boost 0→+9dB 随音量下降） → ReplayGain → 主增益 → 双频段 DRC → 硬顶棚
```

- **顺序约束（响度架必须在主音量之后）**：响度架最多回补 +9 dB，若跑在满刻度上（= 音量衰减之前）其输出会顶破 int16 并被硬钳削波；且补量随音量下降而增大，**削波量随旋钮位置变化**——调小音量时声音发毛/失真、调大后又恢复正常。音量前置后 `g×(1+boost) ≤ 1` 对任意音量成立，架永远不再削波；限幅器之前全是线性环节，**频响逐位不变**（`g×(y+b·lp(y)) ≡ g·y + b·lp(g·y)`）。

- **喇叭保护高通**：板载喇叭为手机跑道型小单元（有效 ~800Hz..8kHz，谐振峰值 850..920Hz）。每声道一阶 DC 阻断高通，截止频率 800Hz（`SPEAKER_HPF_FC_HZ`），Q15 递归系数仅采样率变化时重算：

  ```text
  y[n] = (x[n] - x[n-1]) + lambda × y[n-1]
  lambda = cos(w) - sqrt((1-cos(w)) × (3-cos(w))),  w = 2π·fc/fs
  ```

  滤除 DC 偏移与深低音，避免扬声器振膜无效行程与失真。
- **响度补偿（低音架）**：低音量时人耳对低频不敏感，一阶低通（`SPEAKER_LOUDNESS_FC_HZ=800Hz`，贴合跑道单元可用频段下沿）与主路并联，回加量 `boost(v) = 10^(dB/20) - 1`，`dB = (1 - v/100) × 9dB`——满音量 0dB，最小音量 +9dB，安静播放时补足低音。
- **双频段 DRC / MBC（替代旧的单段 ∞:1 软限幅）**：旧的峰值归一化把所有频率一起压死（热轨中高频被压 2.4 dB、且是硬拐点突变）。现改为 400 Hz 互补分频（`low = 一阶LP`，`high = x - low`，`low+high` 精确重构 x，分频点无梳状滤波），每频段独立峰值包络 + 独立 Q15 平滑增益，查表来自**软拐点(knee 6 dB)增益曲线**，按包络 log2 索引（16 步/倍频程，无运行时浮点）：

  | 频段 | 阈值 | 压缩比 | 增益下限 | 作用 |
  | ---- | ---- | ------ | -------- | ---- |
  | <400 Hz | −20 dBFS | 4:1 | −12 dB | 抑制吃 ±0.22 mm 行程的低频，实测该段压约 2 dB |
  | >400 Hz | −6 dBFS  | 3:1 | −24 dB | 温和控制可听段动态，实测仅压 0.3 dB，基本透明 |

  增益起音 `DRC_ATT_Q15` ~0.3 ms、释放 `DRC_REL_Q15` ~30 ms（包络释放 ~100 ms），比旧限幅更不易泵动；**硬顶棚 `LOUD_LIMIT_CEIL`=−2.5 dBFS 保留在两段重组之后**作最后防线，满幅白噪声实测输出严格 ≤24576，无溢出/削波。参数用完整链路（HPF→音量→响度架→DRC）逐位仿真在重低音热曲上选定：更狠的 −24 dBFS/6:1 会削掉 5.5 dB 低频且小喇叭听不出差别，故弃用。ALC 未加——ReplayGain 已做按曲目响度归一化，ALC 会抹平音乐动态；MBC 仅两频段，因 800 Hz 高通已切除更低频，多频段收益有限。
- 换曲/路由切换时 `audio_dsp_reset()` 清空全部滤波历史（两个频段的包络归零、增益回 0 dB，新曲首帧不被上一曲的峰值压制）。

## 4. 解码/输出解耦（防爆音与欠载）

```text
环形缓冲：hw_audio_write_pcm() 就地 DSP → xRingbufferSend() 送 32KB PCM ring（100ms 有界超时，背压阻塞）
  DMA writer 任务（i2s_wr，优先级 8 / Core 1）→ i2s_channel_write() 喂 I2S DMA（12×1024 帧 ≈ 279ms @44.1kHz，auto_clear 欠载自动静音）
  解码任务被 ring 背压钉在硬件时钟附近（ring 满则阻塞解码；ring 空则 writer 发静音，绝无超前）
```

- 停止/暂停：`hw_audio_set_player_active(false)` 声明播放器不再写数据，解码任务下一帧写返回 `AUDIO_WRITE_ABANDONED` 即时静音；通道**不立即禁用**，writer 先以 20ms 分步排空 ring，再经 `i2s_seam_rearm()` 全环静默重写 + disable/enable 重置 out-link。`hw_audio_park()` 最终冻结的是静音而非上一首尾音；整段停止（B 键 / 看门狗）才走到 park。
- **flush 代际确认**：切歌/重播的 flush 由 `s_flush_req_gen` / `s_flush_done_gen` 代际计数确认，调用方等 `done >= req`；`hw_audio_flush_wait()` 上限 **1200ms**、每 20ms 轮询并 `esp_task_wdt_reset()` 喂狗，覆盖 park 排空 ~350ms + park re-arm ~286ms + fade 20ms + switch re-arm ~286ms（约 940ms）。writer 仅在无更新请求时清标志，避免超时残留 token 吞掉新曲头部；park 排空遇 `!s_parked || s_flush_req` 提前退出，修复快速 stop→play 卡在排空的问题。
- **采样率处理（软件重采样，不重建通道）**：I2S 通道在初始化时以**单一固定速率**（`AUDIO_DEFAULT_RATE`，当前 44100 Hz）创建并启用一次，之后整段会话**绝不因采样率变化**而 disable/rebuild（接缝 re-arm 是独立路径，见上）。解码任务每曲首帧调用 `hw_audio_set_sample_rate()` 仅声明解码器的原生码率；若该码率与固定 I2S 速率不同，`hw_audio_write_pcm()` 在写 DMA 前用 **SpeexDSP 定点重采样器**（cubic-interpolated sinc，`RESAMP_QUALITY=1`，每曲重建以重置接缝历史，同速率曲库直接旁路，零开销）把 PCM 重采样到固定速率。这样曲目接缝处的换速只是一次重采样，不触碰 I2S 通道——彻底消除了"运行通道 disable→enable"这一会卡死 ESP32 DMA 的 out-link stop/start（即"首曲有声、之后整片无声、仅 I2S 受影响"的失败模式：随机/单曲循环选到不同码率文件时原本会在接缝触发通道重建）。正常情况下变调问题不存在：重采样保证音高正确，且 DSP 链系数始终按固定 I2S 速率计算。已知降级：若 SpeexDSP 重采样器初始化失败（内存不足或非法码率），当前实现会旁路重采样并以原码率写入固定 I2S 速率，此时该曲会**变调**并打印 `speex resampler init failed` 错误日志——这是明确的兜底取舍（宁可变调也不静默无声），不是正常路径。i2s_std 通道在初始化时启用并常驻，采样率变化不重建；接缝的 disable/enable 只由 writer 内的 `i2s_seam_rearm()` 执行，无自愈逻辑，DMA 真卡死只能 terminate+reinit（当前未实现，须避免该路径）。
- **播放时钟对齐（取舍说明）**：MP3 解码速率精确而 I2S BCLK（APLL 派生）仅 ppm 级精度，旧架构用 ring+feed+插样主动抵消漂移；新架构（ringbuf + 独立 writer 任务）下该漂移由 DMA 吸收，最坏表现为长时间播放中偶发一次约几十 ms 的欠载静音（`auto_clear` 兜底、自恢复），远轻于旧架构概率性整首无声。
- **通道生命周期**：通道在 `hw_audio_init()` 时启用并常驻；暂停与曲间不禁用，只以静音填充。蓝牙路由切换与整段 `hw_audio_park()` 才由 writer 关通道；切歌/重播则由 `i2s_seam_rearm()` 做「全环静默重写 → disable/enable → 2KB 静音 prime」的 re-arm（约 280ms），这不是采样率重建。全部 I2S 通道操作（enable/disable/写 ring）都收敛到唯一的 DMA writer 任务内部，天然串行，无需互斥锁；跨任务停止安全。
- 蓝牙路由时 I2S 通道保持驻车（见 §1），蓝牙侧由 A2DP 流控自行定钟。

## 5. 接口摘要

```text
components/drivers/audio/audio.c
  hw_audio_init()                        I2S 初始化 + 音量表 + DSP 系数 + 启动 DMA writer 任务（通道常驻）
  hw_audio_set/get_volume(pct)           当前生效路由的音量 0..100
  hw_audio_get_volume_steps()/step()     当前路由档位总数 / 当前档位（喇叭 40 / BT 12，UI 显示 N/40）
  hw_audio_set/get_speaker_volume(pct)   喇叭路由音量槽位（NVS 恢复用）
  hw_audio_set/get_bt_volume(pct)        蓝牙路由音量槽位
  hw_audio_set_bt_volume_change_cb(cb)   本机 BT 音量变化 → AVRCP CT 推送 sink（不入站回声）
  hw_audio_set_avrc_volume(v)            AVRCP 绝对音量 0..127（写蓝牙槽位）
  hw_audio_set_master_gain_db()/get()    全局前置增益 -12..+12 dB（两路由共用）
  hw_audio_set_track_gain_db(dB)         每曲 ReplayGain（两路由通用）
  hw_audio_set_route()/set_route_change_cb()  显式路由切换与切换完成回调
  hw_audio_set_sample_rate(hz)           声明解码器原生码率（内部按固定 I2S 速率重采样，采样率变化不重建通道）
  hw_audio_set_player_active(bool)       MP3 播放器声明 I2S 总线归属（释放后不关通道，靠静音填充；真正停止由 hw_audio_park()）
  hw_audio_pipeline_flush()/switch()     接缝 flush / 硬切歌（静默重写 + re-arm，切歌额外 20ms 淡出）
  hw_audio_pending() / drain_blocking()  查询 PCM 环余量 / 等待 DMA 尾音排净
  hw_audio_is_playing()                  解码任务持有总线且 I2S 真正在出声（电池补偿用）
  hw_audio_write_pcm(frames,n)           就地 DSP 后推入 32KB PCM 环形缓冲（或送 BT 路由），由 writer 任务喂 DMA
```
