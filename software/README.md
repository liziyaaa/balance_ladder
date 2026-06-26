# 平衡梯软件开发说明

> 对应硬件：平衡梯 V1.0，最终原理图日期 2026-06-09  
> 开发环境：VSCode + ESP-IDF v5.x
> 当前状态：可烧录联调版本，后续主要工作是 PID 参数和结构响应联调

本目录是 ESP32-C3 固件工程。固件负责读取 MPU6050 姿态、运行闭环控制、驱动 DRV8837 和 8520 电机、处理 GPIO1 解锁按键、通过 BLE 上传遥测和接收调参命令，并用 OLED/GPIO8 显示本地状态。

本项目不使用 Arduino 框架。所有说明以当前 `software/main/` 中的代码为准。

## 1. 当前角度约定

由于 MPU6050 的安装方向已经按实物调过，当前固件使用的控制角度不是课程报告里的“直立 90 deg”写法，而是：

| 项目 | 当前固件约定 |
|---|---|
| 直立目标角默认值 | `270.0 deg` |
| BLE 可设置目标范围 | `250.0..290.0 deg` |
| 角度误差计算 | 使用 0..360 deg 环绕后的最短角度误差 |
| 控制轴 | 绕 MPU6050 `Y` 轴的前后倾倒 |
| 加速度角度平面 | `atan2(accel_x, accel_z)` |

课程报告或验收可以继续把物理直立写成 90 deg；但当前固件、BLE 调试工具和日志里应按 `270 deg` 作为直立附近目标。后续如果重新标定角度体系，优先只改 `imu_mpu6050.cpp` 和 `app_state.cpp` 中的角度映射，不要让不同模块混用两套角度定义。

## 2. 引脚定义

固件统一在 `main/board_pins.h` 中定义引脚。

| 功能 | GPIO | 当前用途 |
|---|---:|---|
| 预留 GPIO | GPIO0 | 硬件接了 `BAT_ADC`，当前固件不读取电池电压 |
| 启动/解锁按键 | GPIO1 | 输入上拉，低电平有效 |
| MPU6050 INT | GPIO2 | 硬件已连接，当前固件轮询读取 |
| MPU6050 SCL | GPIO3 | 硬件 I2C SCL |
| MPU6050 SDA | GPIO4 | 硬件 I2C SDA |
| DRV8837 nSLEEP | GPIO5 | 输出，高电平使能驱动 |
| DRV8837 IN1 | GPIO6 | LEDC PWM |
| DRV8837 IN2 | GPIO7 | LEDC PWM |
| 板载蓝色 LED | GPIO8 | 状态灯，默认低电平点亮 |
| OLED SDA | GPIO9 | 软件 I2C SDA |
| OLED SCL | GPIO10 | 软件 I2C SCL |
| 外部 UART RX | GPIO20 | 备用 |
| 外部 UART TX | GPIO21 | 备用 |

设备地址：

| 设备 | 地址 |
|---|---|
| MPU6050 | `0x68` |
| SSD1306 OLED | `0x3C` |

## 3. 工程结构

```text
software/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── app_main.cpp
│   ├── app_state.h / app_state.cpp
│   ├── balance_controller.h / balance_controller.cpp
│   ├── ble_debug.h / ble_debug.cpp
│   ├── board_pins.h
│   ├── imu_mpu6050.h / imu_mpu6050.cpp
│   ├── motor_drv8837.h / motor_drv8837.cpp
│   ├── status_led.h / status_led.cpp
│   └── ui_oled.h / ui_oled.cpp
└── README.md
```

| 模块 | 职责 |
|---|---|
| `app_main` | 初始化硬件、创建任务、处理按键事件和 BLE 命令 |
| `app_state` | 系统状态、故障码、控制参数、遥测缓存、NVS 参数保存 |
| `imu_mpu6050` | MPU6050 初始化、陀螺仪零偏校准、互补滤波、当前姿态标定 |
| `balance_controller` | PID 计算、非线性 P 增益、积分抗饱和、输出限幅 |
| `motor_drv8837` | DRV8837 nSLEEP、LEDC PWM、死区补偿、手动电机测试 |
| `ble_debug` | BLE GATT 服务、命令接收、遥测 Notify |
| `ui_oled` | SSD1306 软件 I2C 和动态表情 |
| `status_led` | GPIO8 状态灯闪烁规则 |

## 4. 编译和烧录

进入 ESP-IDF 环境后：

```bash
cd software
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

当前固件日志输出到 ESP32-C3 的 USB Serial/JTAG 下载口。GPIO20/GPIO21 只是备用串口，不作为主日志口。

## 5. 上电流程

当前启动流程：

1. 初始化 NVS 和应用状态，读取上次保存的 PID/目标角等参数。
2. 初始化 GPIO8 状态灯、DRV8837、OLED。
3. 初始化 MPU6050 硬件 I2C，读取 `WHO_AM_I`，配置 DLPF、陀螺仪和加速度计量程。
4. 静止采样约 2 s，完成陀螺仪零偏校准。
5. 初始化 BLE，广播设备名 `BalanceLadder`。
6. 进入 `DISARMED`，电机关闭。
7. 用户扶正平衡梯并短按 GPIO1 后，固件把当前姿态标定到当前目标角，再进入 `ARMED`。

上电后不会自动启动电机。BLE 的 `arm` 命令只把状态切到 `READY`，真正进入闭环仍依赖 GPIO1 物理按键。

## 6. 状态机和安全逻辑

| 状态 | 电机 | 行为 |
|---|---|---|
| `BOOT` | 关闭 | 启动初始化 |
| `CALIBRATING` | 关闭 | MPU6050 静止校准 |
| `DISARMED` | 关闭 | 允许读取传感器、BLE 调参、手动电机测试 |
| `READY` | 关闭 | 提示姿态接近目标或 BLE 请求等待按键确认 |
| `ARMED` | 闭环输出 | 运行 200 Hz 控制环 |
| `FAULT` | 关闭 | 故障停机，等待 `fault_clear` 或复位 |

GPIO1 按键逻辑：

| 操作 | 行为 |
|---|---|
| `DISARMED/READY` 下短按 | 若 IMU 正常且无故障，执行 `imu_set_current_angle(target)`，复位控制器，唤醒电机，进入 `ARMED` |
| `ARMED` 下短按 | 立即停机并回到 `DISARMED` |
| 任意状态长按约 1.5 s | 进入 `FAULT`，故障码 `KEY_LONG` |

角度故障保护：

| 条件 | 处理 |
|---|---|
| `ARMED` 或 `auto_on` 下，`abs(error) > 35 deg` 持续超过 150 ms | 进入 `FAULT`，电机滑行停机 |
| IMU 读取失败 | `ARMED` 下进入 `FAULT`，非闭环状态保持停机 |
| BLE 收到 `stop` | 停机并回到 `DISARMED` |
| BLE 收到 `fault_clear` | 清除故障，回到 `DISARMED` |

当前固件已经删除电池检测逻辑，因此没有低电压停机。硬件仍保留 GPIO0 分压采样点，后续可重新加入。

## 7. GPIO8 状态灯

GPIO8 是 ESP32-C3 SuperMini 板载蓝色 LED。当前 `STATUS_LED_ACTIVE_LOW = true`，即默认低电平点亮。

| 状态 | LED 行为 |
|---|---|
| `BOOT` / `CALIBRATING` | 2 Hz 闪烁 |
| `DISARMED` | 0.5 Hz 慢闪 |
| `READY` | 4 Hz 快闪 |
| `ARMED` | 常亮，每 1 s 熄灭 50 ms 做心跳 |
| `FAULT` | 10 Hz 急闪 |

若实测板载 LED 极性相反，只改 `board_pins.h` 中的 `STATUS_LED_ACTIVE_LOW`。

## 8. 姿态解算

当前 MPU6050 配置：

| 项目 | 当前值 |
|---|---|
| I2C | `I2C_NUM_0`，400 kHz |
| 采样周期 | 控制环 5 ms，即 200 Hz |
| 互补滤波系数 | `alpha = 0.98` |
| 陀螺仪量程 | ±500 deg/s |
| 加速度计量程 | ±2 g |
| 控制角速度 | `gyro_y` |
| 加速度角 | `90 + mount_offset + atan2(accel_x, accel_z)` |

互补滤波核心逻辑：

```cpp
predicted = wrap_360(angle + gyro_y * dt);
angle = wrap_360(predicted + (1.0f - alpha) * shortest_angle_error(acc_angle, predicted));
```

`level` 或按键启动时会调用：

```cpp
imu_set_current_angle(target_angle_deg);
```

它会把当前静止姿态标定为目标角。这样实物略有装配误差时，不需要马上改代码；但如果角度变化方向或前后倾倒轴完全不对，应回到 `imu_mpu6050.cpp` 修改轴向和符号。

## 9. 控制器

当前控制周期为 200 Hz，默认参数在 `ControlParams` 中：

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `target_angle_deg` | `270.0` | 当前固件直立目标 |
| `kp` | `0.02` | 比例系数 |
| `ki` | `0.0` | 积分系数 |
| `kd` | `0.004` | 微分系数 |
| `output_limit` | `1.0` | 输出限幅 |
| `min_cmd` | `0.12` | 小输出死区补偿 |
| `baseline_cmd` | `0.0` | 基准电机命令补偿 |

参数会保存到 NVS，复位后会继续使用上次通过 BLE 设置的值。如果需要恢复默认值，可以擦除 NVS 或在代码中增加恢复默认命令。

当前 PWM 计算公式在 `balance_controller.cpp`：

```cpp
error = shortest_angle_error(target_angle_deg, angle_deg);
d_error = (error - last_error) / dt;
boost = 1.0f + 2.0f * exp(-abs(error) / 10.0f);
kp_eff = kp * boost;
candidate_integral = integral + error * dt;
unsat = kp_eff * error + ki * candidate_integral + kd * d_error;
u = clamp(unsat, -output_limit, output_limit);
```

如果输出已经饱和并且误差方向会让饱和更严重，积分项不会继续累加。随后在 `app_main.cpp` 中叠加 `baseline_cmd`，再送入 `motor_set()`：

```cpp
final_cmd = clamp(u + baseline_cmd, -1.0f, 1.0f);
motor_set(final_cmd);
```

调参建议：

1. `ki=0`，先用 `motor=...` 确认电机方向和推力足够。
2. 只调 `kp`，看它是否有明确回正趋势。
3. 加 `kd` 抑制快速振荡和过冲。
4. 如果小误差附近电机不动，先调 `min_cmd`，不要急着大幅增加 `kp`。
5. 若存在固定偏置，再小幅调整 `baseline` 或最后加入很小的 `ki`。

## 10. DRV8837 电机输出

当前电机驱动配置：

| 项目 | 当前值 |
|---|---|
| PWM 外设 | ESP-IDF LEDC low speed |
| PWM 频率 | 20 kHz |
| PWM 分辨率 | 10 bit，最大 duty `1023` |
| 命令范围 | `-1.0..1.0` |
| 单次变化限制 | 每次 `motor_set()` 最大变化 `0.3` |
| 小输出补偿 | `abs(cmd) < min_cmd` 时提升到 `min_cmd` |
| nSLEEP 读回异常 | 默认忽略读回失败，以实测电压为准 |

当前固件方向映射已经按实物调试反向过：

| `cmd` | IN1 | IN2 |
|---|---|---|
| `cmd > 0` | LOW | PWM |
| `cmd < 0` | PWM | LOW |
| `cmd = 0` | LOW | LOW，滑行 |

如果更换电机线序、桨叶方向或驱动芯片封装后方向变了，优先修改 `motor_drv8837.cpp` 中正负命令对应的 IN1/IN2，而不是在 PID 里硬改符号。

## 11. BLE 调试协议

ESP32-C3 只支持 Bluetooth LE，不支持经典蓝牙 SPP。当前固件使用 NimBLE 实现 NUS 风格 GATT 服务。

| 项目 | 当前值 |
|---|---|
| 设备名 | `BalanceLadder` |
| Service UUID | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX Write UUID | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX Notify UUID | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

遥测格式：

```text
T,<ms>,<state>,<angle>,<target>,<error>,<gyro>,<cmd>,<key>,<fault>
```

示例：

```text
T,83996,ARMED,269.73,270.00,0.27,-0.30,-0.016,0,NONE
```

`gyro` 当前是 `gyro_y`，单位 deg/s。BLE 任务每 50 ms 检查一次遥测，但 `ble_debug.cpp` 对 Notify 做了 500 ms 限速，因此当前实际最大发送频率约为 2 Hz。控制环本身仍是 200 Hz。

支持命令：

| 命令 | 作用 |
|---|---|
| `status` | 立即回传状态和参数 |
| `arm` | 进入 `READY`，等待 GPIO1 按键确认 |
| `stop` | 立即停机，回到 `DISARMED` |
| `fault_clear` | 清除故障，回到 `DISARMED` |
| `cal` | 重新执行 IMU 陀螺仪零偏校准 |
| `level` | 将当前姿态标定为当前目标角 |
| `level=270` | 将当前姿态标定为指定角 |
| `target=270` | 设置目标角，固件会限制到 `250..290` |
| `270` | 直接输入数字，也等价于设置目标角 |
| `kp=0.02` | 设置比例系数 |
| `ki=0` | 设置积分系数 |
| `kd=0.004` | 设置微分系数 |
| `limit=1` | 设置输出限幅，范围 `0.05..1.00` |
| `min_cmd=0.12` | 设置电机最小有效命令补偿 |
| `baseline=0` | 设置基准输出补偿 |
| `auto_on` / `auto_off` | 是否不按键也自动输出闭环控制，用于调试，正式演示慎用 |
| `auto=1` / `auto=0` | 同上 |
| `force_wake_on` / `force_wake_off` | 是否忽略 nSLEEP 读回失败 |
| `force_wake=1` / `force_wake=0` | 同上 |
| `motor=0.3` | `DISARMED` 下手动电机测试，持续约 5 s |
| `motor=0` | 停止手动电机测试 |
| `motor_full` | `DISARMED` 下正向满输出测试，约 2 s 后自动停止 |
| `motor_full_rev` | 反向满输出测试，需要手动 `motor_stop` 停止 |
| `motor_stop` | 停止手动电机测试并关闭电机 |

当前固件没有 BLE 在线切换 IMU 轴向的 `axis=...` 命令。若需要重新试轴，应直接修改 `imu_mpu6050.cpp`。

## 12. OLED 显示

OLED 为 SSD1306 128x64，地址 `0x3C`，使用 GPIO9/GPIO10 软件 I2C。刷新任务周期约 200 ms，即 5 Hz。

当前 OLED 不显示详细文字，只显示简单动态表情：

| 状态 | 表情 |
|---|---|
| `DISARMED` / `READY` | 等待表情 |
| `ARMED` 且误差小于 2 deg 持续 1 s | 愉快表情 |
| `ARMED` 调整中 | 努力表情 |
| `FAULT` | 沮丧表情 |

OLED 绝不能放进 200 Hz 控制环里刷新；当前实现已经放在低优先级 `ui_task`。

## 13. USB 日志

固件每 500 ms 输出一次诊断日志，示例：

```text
diag state=ARMED angle=269.73 target=270.00 error=0.27 gyro_y=-0.30 accel_plane=179.20 accel_angle=269.20 cmd=-0.016 drv_slp_set=1 drv_slp_read=1 wake_fail=0 in1=0/1023 in2=16/1023 key=0 ble=1 fault=NONE
```

重点字段：

| 字段 | 含义 |
|---|---|
| `state` | 当前状态机 |
| `angle/target/error` | 当前角、目标角、误差 |
| `gyro_y` | 控制轴角速度 |
| `accel_plane` | 原始加速度平面角 |
| `accel_angle` | 加上偏置后的加速度角 |
| `cmd` | 控制器最终送给电机的归一化命令 |
| `drv_slp_set/read` | nSLEEP 写入值和读回值 |
| `in1/in2` | 当前 duty / 最大 duty |
| `key` | GPIO1 是否按下 |
| `ble` | BLE 是否连接 |
| `fault` | 故障码 |

## 14. 任务划分

| 任务 | 周期/触发 | 优先级 | 说明 |
|---|---|---:|---|
| `control_task` | 200 Hz | 10 | 读取 IMU、判断安全、更新 PID、输出电机 |
| `key_task` | 10 ms 轮询 | 8 | GPIO1 消抖、短按/长按事件 |
| `ble_notify_task` | 50 ms 检查 | 4 | 发送 BLE 遥测，实际 Notify 约 2 Hz |
| `ui_task` | 200 ms | 3 | OLED 表情刷新 |
| `diagnostic_task` | 500 ms | 2 | USB 日志输出 |
| `status_led_task` | 20 ms | 1 | GPIO8 LED 模式刷新 |

控制环中不要增加打印、OLED 绘制、BLE 发送等慢操作。需要新增调试量时，先放进共享遥测结构或诊断任务。

## 15. 联调顺序

1. 不装螺旋桨，确认固件能编译、烧录、USB 日志正常。
2. 确认 GPIO1 未按为高、按下为低，短按和长按状态正确。
3. 确认 MPU6050 `WHO_AM_I=0x68`，静止时角度稳定。
4. 前后倾斜 PCB，确认 `angle` 和 `gyro_y` 随前后倾倒变化，左右倾倒不应是主控制轴。
5. 连接 BLE，先发 `status`，确认目标角、PID、`min_cmd`、`baseline` 与预期一致。
6. 在 `DISARMED` 下用 `motor=0.2`、`motor=0.5`、`motor=1` 确认推力方向和 PWM 响应。
7. 装桨后手持平衡梯，观察倾倒时 `cmd` 的方向是否能抵消倾倒。
8. 扶正后短按 GPIO1 进入 `ARMED`，先只调 `kp/kd/min_cmd`。
9. 能基本站住后，再测试 `target=260`、`target=280` 这类目标角跟踪。
10. 最后再考虑加入小的 `ki` 或调整 `baseline` 处理重心偏置。

## 16. 给后续协作的注意事项

- 如果只是调参，优先通过 BLE 命令改，不要频繁改代码。
- 如果实物方向反了，优先判断是 IMU 角度方向反，还是电机推力方向反。
- 如果电机小占空比抖动不转，先调 `min_cmd` 或手动 `motor=...` 找最小可转命令。
- 如果 OLED 或 GPIO9 导致启动异常，可以先断开 OLED 或检查上拉。
- 如果 BLE 遥测频率不够，可以降低 `ble_debug.cpp` 中的 500 ms 限速，但不要影响 200 Hz 控制任务。
