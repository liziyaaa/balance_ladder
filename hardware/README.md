# 平衡梯硬件说明

> 硬件版本：V1.0  
> 更新时间：2026-06-09  
> 工程状态：原理图和 PCB 已完成并提交打板

本目录保存平衡梯项目的最终硬件资料。硬件采用 H 型 PCB 作为结构主体，底部尖脚形成不稳定支撑，8520 空心杯电机和螺旋桨提供双向控制力，ESP32-C3 SuperMini 运行闭环控制程序。

## 文件说明

| 文件 | 说明 |
|---|---|
| [balanceladder_2026-06-09.epro](balanceladder_2026-06-09.epro) | 嘉立创 EDA 工程文件 |
| [SCH_Schematic1_1-P1_2026-06-09.svg](SCH_Schematic1_1-P1_2026-06-09.svg) | 最终原理图 SVG |
| [SCH_Schematic1_2026-06-09.pdf](SCH_Schematic1_2026-06-09.pdf) | 最终原理图 PDF |
| [3D_PCB1_2026-06-09.png](3D_PCB1_2026-06-09.png) | PCB 3D 正视图 |
| [3D_PCB1_2026-06-09 (1).png](3D_PCB1_2026-06-09%20%281%29.png) | PCB 3D 斜视图 |

## 功能分区

最终原理图分为四个功能区：

| 分区 | 主要器件 | 作用 |
|---|---|---|
| `Power` | `BT1`、`U2 FM5324H`、`USB1`、`SW1/SW2`、`LED1-LED4` | 18650 电池、Type-C 充电、升压到 +5 V、电量指示和电源按键 |
| `MCU` | `U1 ESP32C3-SuperMini`、`H1`、GPIO1 按键 | 主控、USB 下载、BLE 调试、外部串口预留和闭环解锁按键 |
| `Display&Sensor` | `U4 MPU6050`、`OLED1` | 姿态测量和状态显示 |
| `Motor` | `U5 DRV8837`、`M1 8520` | 电机正反转驱动和推力输出 |

## 关键器件

| 位号 | 器件 | 说明 |
|---|---|---|
| `U1` | ESP32-C3 SuperMini | 主控模块，板载 USB-C 下载口和 3.3 V 稳压 |
| `U2` | FM5324H | 单节锂电充放电管理和 5 V 升压 |
| `U3` | 1 uH 电感 | FM5324H 升压电感 |
| `U4` | MPU6050 模块 | 姿态传感器 |
| `U5` | DRV8837 | 单路 H 桥电机驱动 |
| `OLED1` | 0.96 inch OLED | I2C OLED 显示屏 |
| `M1` | 8520 空心杯电机 | 带螺旋桨的推力执行器 |
| `BT1` | 18650 电池座 | 单节锂电池供电 |
| `USB1` | USB Type-C | 充电/供电输入 |
| `H1` | 1x4 排针 | `VCC/GND/TX/RX` 备用串口接口 |
| `H2/H3` | 1x3 排针 | 底部尖脚/支撑接触点 |

## 电源网络

| 网络 | 来源 | 去向 |
|---|---|---|
| `BAT` | 18650 电池座 `BT1` | FM5324H 输入、电池电压分压 |
| `+5V` | FM5324H 升压输出 | ESP32-C3 SuperMini `5V`、DRV8837 `VM` |
| `VCC` | ESP32-C3 SuperMini `3V3` | MPU6050、OLED、DRV8837 逻辑电源、H1 |
| `GND` | 公共地 | 全板 |

板上有两个 Type-C 口：`USB1` 是整板充电/供电口；ESP32-C3 SuperMini 模块自带的 Type-C 口用于下载程序和日志输出。BLE 是主要调参和姿态遥测通道，H1 串口作为备用接口。

## ESP32-C3 引脚表

| 功能 | ESP32-C3 GPIO | 原理图网络 | 备注 |
|---|---:|---|---|
| 电池电压检测 | GPIO0 | `BAT_ADC` | ADC 输入，R3/R5 分压 |
| 启动/解锁按键 | GPIO1 | 按键输入 | 低电平有效；扶正平衡梯后按下才允许进入闭环 |
| MPU6050 INT | GPIO2 | `MPU_INT` | 经 `R7=10 kΩ` 连接，可用于数据就绪中断，也可轮询 |
| MPU6050 SCL | GPIO3 | `MPU_SCL` | MPU6050 I2C 时钟 |
| MPU6050 SDA | GPIO4 | `MPU_SDA` | MPU6050 I2C 数据 |
| DRV8837 nSLEEP | GPIO5 | `DRV8837_nSLEEP` | 高电平使能驱动 |
| DRV8837 IN1 | GPIO6 | `DRV8837_IN1` | PWM 输出 |
| DRV8837 IN2 | GPIO7 | `DRV8837_IN2` | PWM 输出 |
| OLED SDA | GPIO9 | `OLED_SDA` | OLED I2C 数据 |
| OLED SCL | GPIO10 | `OLED_SCL` | OLED I2C 时钟 |
| 外部 UART RX | GPIO20 | `UART0_RX` | 接 H1 `RX`，备用调试 |
| 外部 UART TX | GPIO21 | `UART0_TX` | 接 H1 `TX`，备用调试 |

最终原理图中 MPU6050 和 OLED 是两组不同网络，不是同一条 I2C 总线。ESP32-C3 通常只有一组硬件 I2C，软件上建议 MPU6050 用硬件 I2C，OLED 用软件 I2C。若后续改版，可以把 OLED 和 MPU6050 合并到同一组 SDA/SCL，以减少软件复杂度。

## 电池 ADC 分压

电池电压由 `R3=33 kΩ` 和 `R5=100 kΩ` 分压后接到 `GPIO0`，ADC 端并联 `C8=100 nF` 滤波。

```text
BAT+ -- R3 33 kΩ -- BAT_ADC/GPIO0 -- R5 100 kΩ -- GND
                         |
                       C8 100 nF
                         |
                        GND
```

换算关系：

```text
Vadc = Vbat * 100 / (33 + 100) = Vbat * 0.752
Vbat = Vadc * 1.33
```

单节锂电满电 `4.2 V` 时，`Vadc` 约为 `3.16 V`，低于 3.3 V ADC 范围。软件读取时还需要做 ADC 校准和多次平均。

## DRV8837 控制方式

| 状态 | `IN1` | `IN2` | `nSLEEP` |
|---|---|---|---|
| 正向推力 | PWM | LOW | HIGH |
| 反向推力 | LOW | PWM | HIGH |
| 滑行停机 | LOW | LOW | HIGH |
| 刹车停机 | HIGH | HIGH | HIGH |
| 驱动休眠 | 任意 | 任意 | LOW |

推荐 PWM 频率设置在 18-25 kHz，避开可闻噪声。单电机反转推力会弱一些，软件中需要允许正反向 PWM 补偿系数不同。

## 上电检查

首块板焊接完成后建议按顺序检查：

1. 不插电池，检查 `BAT`、`+5V`、`VCC`、`GND` 是否短路。
2. 插入 18650 电池，按电源键，测量 `+5V` 是否正常。
3. 测量 ESP32-C3 `3V3`，确认 `VCC` 约为 3.3 V。
4. 测量 `BAT_ADC`，满电附近应约为 3.16 V。
5. 先不装螺旋桨，下载最小程序，确认 GPIO1 按键、OLED、MPU6050 和 BLE 能工作。
6. 限幅测试 DRV8837 和电机正反转。
7. 装桨后先低占空比测试推力，再进入闭环调试。

## 调试注意事项

- `GPIO9` 用作 OLED SDA，属于 ESP32-C3 启动相关引脚之一。如果出现上电启动或下载异常，优先检查 OLED 模块的上拉电阻和焊接状态。
- `GPIO1` 是用户启动/解锁按键，按下为低电平。软件应默认关闭电机，只有扶正后检测到按键动作才进入闭环。
- `GPIO2` 用作 MPU6050 INT，最终板已串 `R7=10 kΩ`。如果后续发现启动异常，可以不使用中断，软件改为轮询读取 MPU6050。
- `GPIO8` 是 SuperMini 板载蓝色 LED，本版没有分配给外设。
- `VCC` 是 3.3 V 逻辑电源，不要把 MPU6050/OLED 的 I2C 上拉到 5 V。
- 电机电流回路和 MPU6050/OLED 的 I2C 走线应尽量分开，减少电机噪声影响姿态测量。
- 首次调参必须设置 PWM 上限和角度超限停机，避免平衡梯倒下后电机继续满功率反转。
