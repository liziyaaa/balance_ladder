# 平衡梯软件开发说明

> 目标读者：后续编写 ESP32-C3 固件的软件工程师  
> 对应硬件：平衡梯 V1.0，最终原理图日期 2026-06-09  
> 开发环境：VSCode + ESP-IDF

本文件定义平衡梯固件第一版工程的功能、引脚映射、控制流程和调试接口。软件目标是让 H 型 PCB 在竖直附近保持平衡，并支持物理按键解锁、蓝牙调参、姿态遥测、目标角度设置、OLED 表情显示、GPIO8 状态灯和安全停机。

## 1. 开发平台

本项目软件使用 **ESP-IDF** 开发，不使用 Arduino 框架。推荐使用 VSCode 的 Espressif IDF 插件进行工程创建、编译、烧录和日志查看。

建议技术栈：

| 功能 | ESP-IDF 模块 |
|---|---|
| GPIO 按键 | `driver/gpio.h` |
| 电机 PWM | `driver/ledc.h` |
| MPU6050 I2C | 第一版代码使用 `driver/i2c.h` legacy I2C；后续可按 ESP-IDF 版本迁移到新 I2C Driver |
| BLE 调试 | NimBLE，`esp_nimble_hci`、`host/ble_hs.h` |
| 定时控制环 | `esp_timer` 或高优先级 FreeRTOS task |
| 日志 | `esp_log.h`，输出到 USB Serial/JTAG 下载口 |

ESP32-C3 只支持 Bluetooth LE，不支持蓝牙经典 SPP。因此蓝牙调试应按 BLE GATT 实现，不能按传统串口蓝牙模块的 SPP 方式设计。

## 2. 引脚定义

固件中建议统一放在 `main/board_pins.h`：

```cpp
#pragma once

#include "driver/gpio.h"

static constexpr gpio_num_t PIN_SPARE_GPIO0  = GPIO_NUM_0;  // 当前软件不使用
static constexpr gpio_num_t PIN_ARM_KEY      = GPIO_NUM_1;  // 按下为低电平

static constexpr gpio_num_t PIN_MPU_INT      = GPIO_NUM_2;
static constexpr gpio_num_t PIN_MPU_SCL      = GPIO_NUM_3;
static constexpr gpio_num_t PIN_MPU_SDA      = GPIO_NUM_4;

static constexpr gpio_num_t PIN_DRV_SLEEP    = GPIO_NUM_5;
static constexpr gpio_num_t PIN_DRV_IN1      = GPIO_NUM_6;
static constexpr gpio_num_t PIN_DRV_IN2      = GPIO_NUM_7;

static constexpr gpio_num_t PIN_STATUS_LED   = GPIO_NUM_8;  // 板载蓝色 LED

static constexpr gpio_num_t PIN_OLED_SDA     = GPIO_NUM_9;
static constexpr gpio_num_t PIN_OLED_SCL     = GPIO_NUM_10;

static constexpr gpio_num_t PIN_UART_RX      = GPIO_NUM_20;
static constexpr gpio_num_t PIN_UART_TX      = GPIO_NUM_21;
```

最终硬件引脚分配：

| 功能 | GPIO | 电平/接口 | 说明 |
|---|---:|---|---|
| 预留 GPIO | GPIO0 | 未使用 | 当前软件已删除电池检测 |
| 启动/解锁按键 | GPIO1 | 低电平有效 | 平衡梯扶正后，按下才允许进入闭环 |
| MPU6050 INT | GPIO2 | 输入 | 经 `R7=10 kΩ` 连接，可不用中断，先轮询 |
| MPU6050 SCL | GPIO3 | I2C SCL | 硬件 I2C |
| MPU6050 SDA | GPIO4 | I2C SDA | 硬件 I2C |
| DRV8837 nSLEEP | GPIO5 | 输出 | 高电平使能电机驱动 |
| DRV8837 IN1 | GPIO6 | LEDC PWM | 电机方向/PWM 输入 1 |
| DRV8837 IN2 | GPIO7 | LEDC PWM | 电机方向/PWM 输入 2 |
| 板载状态灯 | GPIO8 | 输出 | SuperMini 板载蓝色 LED，默认低电平点亮 |
| OLED SDA | GPIO9 | 软件 I2C | OLED 低频刷新 |
| OLED SCL | GPIO10 | 软件 I2C | OLED 低频刷新 |
| 外部 UART RX | GPIO20 | UART | 预留接口，作为备用调试通道 |
| 外部 UART TX | GPIO21 | UART | 预留接口，作为备用调试通道 |

按键 `GPIO1` 要配置为输入上拉：

```cpp
gpio_config_t io_conf = {};
io_conf.pin_bit_mask = 1ULL << PIN_ARM_KEY;
io_conf.mode = GPIO_MODE_INPUT;
io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
io_conf.intr_type = GPIO_INTR_DISABLE;
gpio_config(&io_conf);
```

建议软件做 20-50 ms 消抖。第一版可以用轮询检测短按，不必一开始就使用 GPIO 中断。

## 3. I2C 方案

最终硬件中 MPU6050 和 OLED 没有接在同一条 I2C 总线上：

| 设备 | SDA | SCL | 建议实现 |
|---|---:|---:|---|
| MPU6050 | GPIO4 | GPIO3 | ESP-IDF 硬件 I2C，控制环高频读取 |
| OLED | GPIO9 | GPIO10 | 软件 I2C，5-10 Hz 低频刷新 |

ESP32-C3 通常只有一组硬件 I2C 控制器。MPU6050 是闭环控制的关键传感器，优先占用硬件 I2C；OLED 只负责显示状态，可以用软件 I2C 或后续自己实现简单 bit-bang I2C。

`PIN_MPU_INT` 接在 GPIO2 上，硬件中串了 `R7=10 kΩ`。软件第一版建议按控制周期轮询 MPU6050，等闭环稳定后再考虑使用 INT 做数据就绪同步。

## 4. 工程结构

当前 ESP-IDF 工程结构：

```text
software/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── app_main.cpp
│   ├── board_pins.h
│   ├── app_state.h
│   ├── app_state.cpp
│   ├── status_led.h
│   ├── status_led.cpp
│   ├── imu_mpu6050.h
│   ├── imu_mpu6050.cpp
│   ├── motor_drv8837.h
│   ├── motor_drv8837.cpp
│   ├── balance_controller.h
│   ├── balance_controller.cpp
│   ├── ble_debug.h
│   ├── ble_debug.cpp
│   ├── ui_oled.h
│   └── ui_oled.cpp
└── README.md
```

模块职责：

| 模块 | 职责 |
|---|---|
| `app_state` | 系统状态机、按键解锁、安全停机 |
| `status_led` | GPIO8 板载蓝色 LED 状态提示 |
| `imu_mpu6050` | 初始化 MPU6050，读取加速度和陀螺仪，完成零偏校准和角度融合 |
| `motor_drv8837` | 封装电机驱动，把 `-1.0..1.0` 控制量转换为 IN1/IN2 PWM |
| `balance_controller` | PID 控制、目标角设置、限幅、积分抗饱和 |
| `ble_debug` | BLE GATT 服务、姿态遥测、接收目标角和 PID 参数 |
| `ui_oled` | OLED 表情刷新，当前刷新频率 5 Hz |
| `app_main` | 初始化、任务创建、控制周期调度 |

## 5. 上电流程

当前实现的上电流程：

```text
1. 关闭电机输出：nSLEEP=HIGH，IN1=LOW，IN2=LOW；当前调试阶段保持 DRV8837 常醒
2. 初始化 GPIO、按键、日志
3. 初始化 OLED 软件 I2C，显示启动状态
4. 初始化 MPU6050 硬件 I2C
5. 静止约 2 s，校准陀螺仪零偏
6. 初始化 BLE，开始广播设备名 BalanceLadder
7. 进入 DISARMED 状态，电机保持关闭
8. 用户扶正平衡梯，短按 GPIO1 按键后，才允许进入 ARMED
```

上电后严禁自动启动电机。物理按键是闭环启动的最后一道确认：即使 BLE 已连接并下发了 `arm`，也必须满足角度接近目标值并检测到按键动作，才允许真正进入闭环。

## 6. 状态机

当前实现的状态机：

| 状态 | 电机 | 行为 |
|---|---|---|
| `BOOT` | 关闭 | 初始化硬件 |
| `CALIBRATING` | 关闭 | MPU6050 静止校准 |
| `DISARMED` | 关闭 | 读取传感器、显示状态、允许 BLE 调参 |
| `READY` | 关闭 | 角度已接近目标，等待 GPIO1 按键确认 |
| `ARMED` | 闭环输出 | 运行姿态控制 |
| `FAULT` | 关闭 | 故障停机，等待人工处理 |

进入 `ARMED` 的建议条件：

```text
BLE 或默认配置给出 targetAngleDeg
abs(measuredAngleDeg - targetAngleDeg) < ARM_WINDOW_DEG
上述条件连续保持 ARM_STABLE_MS
检测到 GPIO1 按键短按
IMU 状态正常
```

建议初始参数：

| 参数 | 建议值 |
|---|---:|
| `ARM_WINDOW_DEG` | 5-8 deg |
| `ARM_STABLE_MS` | 300-800 ms |
| 按键消抖 | 20-50 ms |
| 角度故障阈值 | 35-45 deg |

按键逻辑建议：

| 操作 | 状态 | 行为 |
|---|---|---|
| DISARMED/READY 下短按 | 姿态接近目标 | 进入 `ARMED` |
| DISARMED/READY 下短按 | 姿态偏离目标 | 保持关闭，并通过 USB 日志输出拒绝原因 |
| ARMED 下短按 | 任意 | 退出闭环，进入 `DISARMED` |
| 任意状态长按 | 任意 | 急停，进入 `FAULT` 或 `DISARMED` |

GPIO8 板载蓝色 LED 状态建议：

| 状态 | LED 行为 |
|---|---|
| `BOOT` / `CALIBRATING` | 2 Hz 闪烁 |
| `DISARMED` | 0.5 Hz 慢闪 |
| `READY` | 4 Hz 快闪，提示已经扶正、可按键启动 |
| `ARMED` | 常亮，每 1 s 熄灭 50 ms 做心跳 |
| `FAULT` | 10 Hz 急闪 |

LED 极性封装为 `STATUS_LED_ACTIVE_LOW` 常量，默认低电平点亮。如果实测相反，只改这个常量。

## 7. 姿态解算

MPU6050 是竖着跟 PCB 一起安装的，不要求平放。软件要根据实际安装方向建立“传感器坐标轴 -> 平衡梯倾角”的转换关系。

第一版建议实现互补滤波：

```cpp
// dt: 控制周期，单位 s
// gyroRateDeg: 控制轴角速度，单位 deg/s
// accAngleDeg: 由加速度计计算出的倾角，单位 deg
angleDeg = alpha * (angleDeg + gyroRateDeg * dt)
         + (1.0f - alpha) * accAngleDeg;
```

当前互补滤波默认参数：

| 参数 | 初始值 |
|---|---:|
| 控制周期 | 5 ms，即 200 Hz |
| `alpha` | 0.98 |
| 陀螺仪量程 | ±500 deg/s |
| 加速度计量程 | ±2 g |

当前代码已按实测结论使用 MPU6050 的 Z 轴作为控制轴：`kGyroControlAxis = 2`。由于当前安装姿态下 MPU6050 原始直立角约为 `270°`，代码内置 `180°` 安装偏置，将对外显示和 BLE 控制角度换算为“直立约 `90°`”。若实测发现角度或控制方向反了，优先调整 `kAccelAngleSign` 或 `kGyroRateSign`。

角度定义要在代码里固定下来。建议外部显示和 BLE 命令使用直观角度，例如竖直为 `90 deg`：

```cpp
float measuredAngleDeg = readLadderAngle();   // 竖直约为 90 deg
float targetAngleDeg   = 90.0f;               // 可由 BLE 改成 85 deg
float errorDeg         = targetAngleDeg - measuredAngleDeg;
```

如果内部控制更喜欢“竖直为 0 deg”，可以在 IMU 模块里完成转换，但不要让不同模块混用两套角度定义。

## 8. 电机驱动

`motor_drv8837` 建议对外只暴露归一化命令：

```cpp
void motorInit();
void motorSet(float cmd);    // cmd: -1.0 .. 1.0
void motorCoast();
void motorBrake();
void motorSleep(bool sleep);
```

控制逻辑：

```cpp
if (cmd > 0) {
    IN1 = PWM(abs(cmd));
    IN2 = LOW;
} else if (cmd < 0) {
    IN1 = LOW;
    IN2 = PWM(abs(cmd));
} else {
    IN1 = LOW;
    IN2 = LOW;
}
```

当前实现细节：

| 项目 | 当前值 |
|---|---|
| PWM 外设 | ESP-IDF LEDC |
| PWM 频率 | 20 kHz |
| PWM 分辨率 | 10 bit |
| 输出范围 | `-1.0..1.0` |
| 初始限幅 | `abs(cmd) <= 0.25` |
| 死区补偿 | 第一版暂不启用；小于 `0.001` 直接滑行 |
| 正反向补偿 | 第一版暂不启用；如实测不对称再加入 `forwardScale` / `reverseScale` |

方向一定要实测。如果平衡梯向前倒，电机输出也让它更向前倒，说明控制方向反了，应调整 `cmd` 符号或电机线序。

## 9. 控制器

第一版已实现 PID，默认参数偏保守，适合无桨和低风险初调：

| 参数 | 默认值 |
|---|---:|
| `target_angle_deg` | `90.0` |
| `Kp` | `0.02` |
| `Ki` | `0.0` |
| `Kd` | `0.004` |
| `outputLimit` | `0.25` |

```cpp
float error = targetAngleDeg - angleDeg;
float dError = (error - lastError) / dt;
integral += error * dt;

float u = Kp * error + Ki * integral + Kd * dError;
u = clampf(u, -outputLimit, outputLimit);
motorSet(u);
```

调参顺序：

1. `Ki=0`，只调 `Kp`，让系统有明显回正趋势。
2. 加 `Kd` 抑制超调和振荡。
3. 稳定后少量加入 `Ki`，用于补偿重心偏置和电机不对称。
4. 每次只改一个参数，并通过 BLE 遥测记录响应。

必须实现积分抗饱和：当 `u` 已经达到输出限幅，或者系统角度超限时，不要继续累积积分项。

## 10. BLE 调试协议

蓝牙调试承担两件事：

1. 上传 MPU6050 姿态、控制输出和状态信息。
2. 接收调参和控制指令，包括目标角度、PID 参数、输出限幅、停机命令。

当前使用 NimBLE 实现一个 NUS 风格的自定义 GATT 服务：

| 项目 | 当前值 |
|---|---|
| 设备名 | `BalanceLadder` |
| Service UUID | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX Characteristic | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`，Write，无响应也可 |
| TX Characteristic | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`，Notify |

RX 命令使用 ASCII 文本，便于手机 BLE 调试工具直接发送：

| 命令 | 作用 |
|---|---|
| `arm` | 请求进入 READY；真正启动仍需 GPIO1 按键确认 |
| `stop` | 立即停机，进入 `DISARMED` |
| `fault_clear` | 清除故障，回到 `DISARMED` |
| `cal` | 重新校准 IMU |
| `level` | 将当前姿态标定为当前目标角，默认目标为 `90°` |
| `level=90` | 将当前姿态标定为指定角度 |
| `85` / `95` | 直接输入数字，设置目标角度 |
| `target=90` | 设置目标角度 |
| `kp=0.03` | 设置 `Kp` |
| `ki=0.0` | 设置 `Ki` |
| `kd=0.005` | 设置 `Kd` |
| `limit=0.30` | 设置输出限幅 |
| `motor=0.25` / `fan=0.25` | 手动风叶测试正向输出，持续 5 s，范围 `-1.00..1.00` |
| `motor=-0.25` / `fan=-0.25` | 手动风叶测试反向输出，持续 5 s |
| `motor=0` / `fan=0` | 停止手动电机测试 |
| `axis=xy` / `axis=xz` / `axis=yz` | 在线切换 IMU 加速度角度计算平面，并把当前姿态标定为目标角 |
| `status` | 立即回传一次当前状态 |

TX 遥测为 20 Hz CSV 文本，前期足够直观：

```text
T,<ms>,<state>,<angle>,<target>,<error>,<gyro_z>,<cmd>,<key>,<fault>
T,12345,ARMED,89.40,90.00,0.60,-2.10,-0.180,1,NONE
```

遥测频率固定为 20 Hz，不会每个 200 Hz 控制周期都发 Notify。BLE 发送放在低优先级任务中，不阻塞控制环。

后续如果文本协议带宽不够，可以改二进制包，但第一版建议先用文本，方便调试和记录。

## 11. OLED 显示

OLED 只负责低频状态显示，不参与控制环。第一版 5 Hz 刷新一次。

第一版显示简单动态表情，不显示文字页面：

```text
DISARMED / READY: 等待表情，两帧交替
ARMED 且误差小于 2 deg 持续 1 s: 愉快表情，两帧交替
ARMED 调整中: 努力表情，两帧交替
FAULT: 沮丧表情，两帧交替
```

不要在 200 Hz 控制循环中直接刷新 OLED。OLED 刷新可以放在低优先级任务中。

## 12. 调试输出

当前工程将 ESP-IDF console 配置为 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`，日志从 ESP32-C3 的 USB Serial/JTAG 下载口输出，不再使用 UART0 作为主调试输出。

固件每 500 ms 输出一次诊断日志：

```text
diag state=DISARMED angle=89.80 target=90.00 error=0.20 gyro_z=0.01 cmd=0.000 drv_slp=1 in1=0 in2=0 key=0 ble=0 fault=NONE
```

这些日志用于确认当前状态机、MPU6050 角度、Z 轴角速度、电机命令、按键和 BLE 连接情况。

## 13. 安全逻辑

必须实现以下停机条件：

| 条件 | 处理 |
|---|---|
| `abs(angle error) > 35-45 deg` | 立即停机，进入 `FAULT` |
| IMU 读取失败 | 停机 |
| I2C 超时 | 停机 |
| BLE 收到 `stop` | 停机 |
| ARMED 下 GPIO1 短按 | 停机 |
| 任意状态 GPIO1 长按 | 急停 |
| 复位或刚上电 | 默认 `DISARMED` |

故障停机时应执行：

```text
motorCoast()
motorSleep(true)
清除积分项
记录 fault reason
OLED/BLE 输出故障原因
```

## 14. 任务划分建议

建议 FreeRTOS 任务：

| 任务 | 周期/触发 | 优先级 | 说明 |
|---|---|---:|---|
| `control_task` | 200 Hz | 高 | 读取 IMU、更新控制器、输出电机 |
| `ble_task` | 事件驱动 + 20-50 Hz Notify | 中 | 接收命令、发送遥测 |
| `ui_task` | 5-10 Hz | 低 | OLED 显示 |
| `key_task` | 50-100 Hz | 中 | 按键消抖和状态机事件 |
| `diagnostic_task` | 2 Hz | 低 | USB Serial/JTAG 输出状态诊断 |

控制环不要直接打印日志、刷新 OLED 或发送 BLE Notify。可以把遥测数据写入一个结构体或队列，由 BLE 任务按较低频率发送。

## 15. 最小闭环伪代码

```cpp
constexpr float CONTROL_DT = 0.005f; // 200 Hz

void controlStep() {
    ImuSample imu = imuRead();
    float angle = updateAngleFilter(imu, CONTROL_DT);
    updateSharedTelemetry(angle);

    if (!safetyOk(angle)) {
        enterFault("safety");
        return;
    }

    if (state == AppState::ARMED) {
        float cmd = controllerUpdate(angle, targetAngleDeg, CONTROL_DT);
        motorSet(cmd);
    } else {
        motorCoast();
    }
}

void keyEventShortPress() {
    if (state == AppState::ARMED) {
        enterDisarmed();
        return;
    }

    if ((state == AppState::DISARMED || state == AppState::READY) &&
        angleNearTarget() && imuOk()) {
        enterArmed();
    }
}
```

## 16. 首次联调顺序

1. 先不装螺旋桨，确认 ESP-IDF 工程能编译、烧录、输出日志。
2. 读取 GPIO1，确认未按下为高电平、按下为低电平，消抖正常。
3. 通过 USB Serial/JTAG 日志查看 500 ms 诊断输出。
4. 读取 MPU6050，转动 PCB，确认 Z 轴角速度和角度方向正确。
5. 建立 BLE 连接，确认能收到姿态遥测，能下发 `target=85` 等指令。
6. 点亮 OLED，若没有显示，先看 USB 日志里的 OLED ACK 结果。
7. 不装桨测试电机正反转和 PWM。
8. 装桨后先用 `motor=0.30` / `fan=0.30` 测推力方向，再逐步尝试 `0.50`、`0.70`、`1.00`。
9. 手持平衡梯，看控制输出是否抵消倾斜。
10. 扶正平衡梯，短按 GPIO1 进入闭环，低功率调参。
11. 稳定后逐步提高输出上限，并记录 BLE 遥测数据。

第一版软件的目标不是一次性调到完美，而是先建立可靠的“按键解锁 -> 姿态采样 -> BLE 调参 -> 控制输出 -> 安全停机”链路。链路跑通后，PID 参数和目标角度跟踪才有意义。
