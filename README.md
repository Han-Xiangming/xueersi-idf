# 小喵掌机 ESP-IDF 固件与硬件资料

这是给学而思小喵掌机移植的 ESP-IDF / LVGL 固件工程，同时整理了屏幕、按键和传感器等硬件资料。

## 固件下载与刷入

已经编译好的 merged bin 会放在本项目的 [Releases](https://github.com/ZyoungInc/xueersi-idf/releases/latest) 页面。普通用户可以直接下载 release 里的 `xiaomiao-merged.bin` 并从 `0x0` 地址刷入，不需要自己搭建 ESP-IDF 编译环境。

示例命令：

```bash
esptool.py --chip esp32 -b 460800 write_flash 0x0 xiaomiao-merged.bin
```

刷入前请确认目标硬件是 ESP32-WROVER-B 版本的小喵掌机，并确认串口连接正常。

## 当前状态

- ESP32 侧固件已经移植到 ESP-IDF，使用 LVGL 9.x 驱动 ST7735 SPI 屏幕，并提供硬件状态分页 UI。
- 最佳的性能优化，240mhz频率，高速SPI，PSRAM，FLASH频率，三重缓冲，稳定60fps UI
- 由于屏幕的TE引脚没有连接到MCU，无法做垂直同步。抗撕裂。由于背光引脚直连cc，无法调节背光亮度。
- 光照、热敏、蜂鸣器、按键、MicroSD、I2C 设备探测等功能已经接入 ESP32 侧固件。


## 原理图与鸣谢

原理图文件已整理为 [`xueersi-xiaomiao-schematic.pdf`](xueersi-xiaomiao-schematic.pdf)。

感谢 ID「我为电波狂」对硬件进行测量并制作原理图，这部分资料对后续移植和维护非常关键。

## 参与项目

如果这个项目对你有帮助，欢迎 Star。遇到问题、发现硬件差异、或者有协议兼容建议，可以提交 Issue。也欢迎提交 PR，我会审核后合并。
本项目针对官方原厂硬件，不考虑对硬件的魔改因素。对硬件改动的支持需要另外开branch或复制到自己的仓库（但需要注意下一章的要求）。

## 使用与署名要求

本项目由ZYoungInc（wechat/tel：15657325738）完全用爱发电并完全免费提供给爱好者们学习和交流等非营利目的。如有侵权，请联系本人。二次开发、转载、分发、商用或以任何形式使用本项目内容时，请务必保留并明确引用原作者与本项目来源，以尊重劳动成果。违反者将依法追究责任；本人保留所有权利。也欢迎举报滥用。

## 1. 总体架构

```text
PC USB
  │
  │ USB CDC
  ▼

ESP32-WROVER-B
  ├─ SPI TFT 显示屏
  ├─ MicroSD 卡
  ├─ 6 个按键
  ├─ 蜂鸣器 PWM
  ├─ 光照 ADC
  ├─ 热敏 ADC
  ├─ I2C 主机
  │   └─ MPU6050：0x68
  └─ 预留扩展 IO
```

核心关系：

```text
ESP32 = 主控 / UI / Python 运行环境 / 屏幕 / SD / 按键 / 传感器
```

***

## 2. ESP32 引脚分配

### 2.1 TFT 显示屏

| 功能             | ESP32 引脚   |
| -------------- | ---------- |
| SPI SCK        | GPIO18     |
| SPI MOSI       | GPIO23     |
| SPI MISO       | GPIO19     |
| TFT DC         | GPIO4      |
| TFT CS         | GPIO5      |
| TFT RES / 相关复用 | GPIO19     |

显示对象信息：

```text
显示分辨率：160 × 128
SPI：SPI2
SPI 频率：40 MHz
SCK：GPIO18
MOSI：GPIO23
MISO：GPIO19
DC：GPIO4
```

屏幕底层通过 `FrameBuffer` 和 `SCREEN` 对象刷新。

***

### 2.2 MicroSD 卡

| 功能         | ESP32 引脚   |
| ---------- | ---------- |
| SPI SCK    | GPIO18     |
| SPI MOSI   | GPIO23     |
| SPI MISO   | GPIO19     |
| SD CS      | GPIO22     |

TFT 与 MicroSD 共用 SPI2，通过不同 CS 分时使用。

***

### 2.3 按键

| 按键         | ESP32 引脚   |
| ---------- | ---------- |
| 上          | GPIO2      |
| 下          | GPIO13     |
| 左          | GPIO27     |
| 右          | GPIO35     |
| A          | GPIO34     |
| B          | GPIO12     |

注意：

```text
GPIO34、GPIO35 是输入专用脚。
GPIO12 是启动相关敏感脚。
```

***

### 2.4 ADC 传感器

| 功能         | ESP32 引脚   |
| ---------- | ---------- |
| 光照传感器      | GPIO36     |
| 热敏电阻       | GPIO39     |

已确认：

```text
sensor.getLight() = ADC(GPIO36).read()
sensor.getTemp()  = ADC(GPIO39) 后换算
```

***

### 2.5 蜂鸣器

| 功能         | ESP32 引脚   |
| ---------- | ---------- |
| 无源蜂鸣器      | GPIO14     |

底层对象：

```text
PWM(14, freq=440, duty=0)
```

也就是：

```text
GPIO14 → PWM → 无源蜂鸣器
```

***

### 2.6 I2C 总线

| 功能         | ESP32 引脚   |
| ---------- | ---------- |
| I2C SCL    | GPIO15     |
| I2C SDA    | GPIO21     |

I2C 设备：

| 地址         | 设备         |
| ---------- | ---------- |
| 0x68       | MPU6050    |

当前已确认：

```text
MPU6050：0x68，未安装时不会出现在 scan 结果中
```

***

### 2.7 UART0

| 功能         | ESP32 引脚   |
| ---------- | ---------- |
| UART0 TX   | GPIO1      |
| UART0 RX   | GPIO3      |

UART0 通常连到主机 USB 串口桥，用于日志、终端和上传。

***

## 3. 板上连接关系

板上各子系统与 ESP32 的连接如下：

| 功能          | 连接对象                     |
| ----------- | ------------------------ |
| USB D+ / D- | USB 接口                   |
| UART 桥      | ESP32 GPIO1 / GPIO3      |
| I2C         | ESP32 GPIO15 / GPIO21    |
| 电机 PWM      | HR8833 / DRV8833 (扩展)
| LED 控制      | 板载 LED 接口 (扩展)
| SWD         | TMS / TCK / RST / GND 焊盘 |

***

## 4. I2C 地址与设备

### 4.1 I2C 总线

```text
I2C 控制器：ESP32 I2C(0)
SCL：GPIO15
SDA：GPIO21
频率：100 kHz
```

### 4.2 地址表

| I2C 地址     | 设备         | 说明         |
| ---------- | ---------- | ---------- |
| 0x68       | MPU6050    | 加速度计 / 陀螺仪 |

***



已移除与外部协处理器相关的 LED / 电机 协议细节。如需此类硬件协议文档，请在 issue 中讨论或参考单独的外设文档。
| 1          | 16         | 0x0010     | 0x10       | 0x00       |
| 10         | 160        | 0x00A0     | 0xA0       | 0x00       |
| 50         | 800        | 0x0320     | 0x20       | 0x03       |
| 100        | 1600       | 0x0640     | 0x40       | 0x06       |
| 128        | 2048       | 0x0800     | 0x00       | 0x08       |
| 200        | 3200       | 0x0C80     | 0x80       | 0x0C       |
| 255        | 4080       | 0x0FF0     | 0xF0       | 0x0F       |

***

## 5.5 电机 1 数据格式

### Motor 1，方向 1

```text
[
  0x0E,
  0x00, 0x00, PWM_L, PWM_H,
  0x00, 0x00, 0x00, 0x00
]
```

### Motor 1，方向 0

```text
[
  0x0E,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, PWM_L, PWM_H
]
```

***

## 5.6 电机 2 数据格式

### Motor 2，方向 1

```text
[
  0x06,
  0x00, 0x00, PWM_L, PWM_H,
  0x00, 0x00, 0x00, 0x00
]
```

### Motor 2，方向 0

```text
[
  0x06,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, PWM_L, PWM_H
]
```

***

已移除涉及外部协处理器（原项目中的 GD32）及其 I2C/LED/电机协议的详细描述。
本仓库保留与 ESP32 直接相关的硬件信息（显示、按键、ADC、I2C 总线、蜂鸣器、SD 等）。
如果你仍然需要外协处理器的协议细节，请在 Issue 中说明，我们可以把这些文档移入单独的子目录或备份文件中。

### 11.4 蜂鸣器

```text
GPIO14
PWM 输出
无源蜂鸣器
```

### 11.5 光照 / 温度

```text
光照：
  GPIO36
  ADC

热敏：
  GPIO39
  ADC
```

### 11.6 按键

```text
up    = GPIO2
down  = GPIO13
left  = GPIO27
right = GPIO35
a     = GPIO34
b     = GPIO12
```

### 11.7 显示

```text
分辨率：160 × 128
SPI：SPI2
SCK：GPIO18
MOSI：GPIO23
MISO：GPIO19
DC：GPIO4
CS：GPIO5
```
