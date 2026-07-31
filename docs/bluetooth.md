# 小喵掌机 蓝牙音频说明

> 本文档为**实现现状说明**（随代码更新）：`hardware/bt_audio.c` 把掌机变成蓝牙音频**源**（A2DP Source），并向远端暴露 AVRCP 媒体键 / 音量。

## 1. 角色与架构

```text
掌机（A2DP Source / AVRCP Target）
  │
  ├─ A2DP：解码 PCM（路由自 audio.c）→ 内部重采样 → SBC（Bluedroid 编码）→ 蓝牙耳机/音箱
  └─ AVRCP：耳机媒体键（播放/暂停/上/下曲）+ 绝对音量 → 本地回调

PCM 环形缓冲（bt_audio_init 分配）
```

- 目标 ESP-IDF v5.x（实测 5.5），`CONFIG_BT_ENABLED` + `CONFIG_BT_A2DP_ENABLE`。
- 所有入口在蓝牙被裁剪时均为安全 no-op。
- **懒启动**：`bt_audio_init()` 只分配环形缓冲，不碰控制器；`bt_audio_enable()`（进入蓝牙页时调用）才拉起控制器 + Bluedroid + A2DP Source，避免开机即广播 A2DP。

## 2. 生命周期

| API | 说明 |
| ---- | ---- |
| `bt_audio_init()` | 分配 PCM 环形缓冲（启动时一次） |
| `bt_audio_enable()` | 懒启动蓝牙栈（幂等；首次做实事） |
| `bt_audio_disable()` | 拆栈并关控制器（总开关关时调用，无线电真正静默；状态全复位） |
| `bt_audio_set_enabled(bool)` / `bt_audio_is_enabled()` | 开关/查询解码 PCM → 蓝牙路由 |
| `bt_audio_is_connected()` | A2DP 已连接 |

## 3. 设备发现与连接（驱动蓝牙 UI 页）

```text
bt_audio_scan_start()     清空列表 + GAP inquiry（发现音频 sink）
bt_audio_is_scanning()    扫描中
bt_audio_device_count()   已发现设备数（扫描期间增长）
bt_audio_device_version() 列表变更计数（UI 只在递增时重格式化）
bt_audio_device_name(i)   设备名（无名设备回退 MAC）
bt_audio_connect_index(i) 连接第 i 台（取消扫描、开启 BT 输出）
bt_audio_disconnect()     断开但保留栈
bt_audio_peer_name()      已连/连接中设备名
```

- 配对进度 `bt_pair_state_t`：`CONNECTING → PAIRING（SSP 数值比较，`bt_audio_passkey()` 取 6 位密钥给 UI 显示）→ OK / FAIL`。

## 4. PCM 输入与重采样

```text
bt_audio_write_pcm(frames, n)   仅当 开启 + 已连接 + 流式中 才入队，否则 no-op
bt_audio_set_sample_rate(hz)    A2DP/SBC 恒 44.1kHz，其它采样率内部重采样
                                （否则远端会变速变调）
```

- 路由由 audio.c 决定：蓝牙连接且开启时，`hw_audio_write_pcm()` 只喂蓝牙（音量、全频段），I2S 完全静默；未连接/关闭时只走喇叭（见 `docs/audio.md` §1）。

## 5. AVRCP 远端控制（TG 角色）

Bluedroid 自动 ACK passthrough / 绝对音量，本模块只翻译成回调：

```c
bt_audio_set_avrc_cmd_cb(cb);      /* BT_AVRC_CMD_PLAY/PAUSE/STOP/NEXT/PREV */
bt_audio_set_avrc_volume_cb(cb);   /* 绝对音量 0..127，应用侧映射到 0..100% */
```

- 音量映射：AVRCP 满刻度 0..127 → `((v × 100) + 63) / 127` % 写入主音量（与本地音量共用同一 dB 锥度表，见 `docs/audio.md`）。
- main.c 回调：PLAY/PAUSE 恢复/暂停播放，STOP 停止，NEXT/PREV 忽略（播放器无列表导航，仅记日志）。

## 6. 与其它模块接口

```text
audio.c：   hw_audio_write_pcm() 内按路由分发 → 蓝牙走 bt_audio_write_pcm()，
            喇叭走 I2S 环形；采样率变更同时通知 bt_audio_set_sample_rate()
main.c：    bt_audio_init() 启动时调用；AVRCP 回调注册（媒体键 → player，
            音量 → hw_audio_set_avrc_volume）
ui.c：      蓝牙页刷新设备列表/配对状态/连接名；设置页 BT 总开关 → set_enabled/disable
```

## 7. 已知限制

- 源角色一次只能连一台 sink。
- 扫描为 GAP inquiry，范围/速度受蓝牙射频环境限制。
- SBC 编码 CPU 开销随 44.1kHz 固定，重采样在内部完成。
