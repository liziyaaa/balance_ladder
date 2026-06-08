# 平衡梯 Balance Ladder

这是一个用于自动控制原理课程设计的主动平衡平台项目。结构上采用 H 型 PCB 作为主体，底部使用尖点支撑，中部/顶部安装空心杯电机和螺旋桨，通过 MPU6050 测量姿态，ESP32-C3 运行控制算法，DRV8833 驱动电机产生双向推力，使平衡梯在不稳定支撑条件下保持直立或跟踪指定角度。

项目目标不是做一把真实可用的梯子，而是搭建一个“窄支撑、高重心、开环不稳定结构”的控制实验平台，用来展示开环不稳定、闭环反馈、PID 参数整定、抗扰动和后续双层叠放控制效果。

## 当前方案

| 模块 | 当前选型 |
|---|---|
| 主控 | ESP32-C3 SuperMini |
| 姿态传感器 | GY-521 MPU6050 |
| 电机 | 8520 空心杯电机，单电机方案 |
| 电机驱动 | DRV8833 |
| 电池 | 1S 3.7 V LiPo，约 450-650 mAh，25C/30C |
| 电源 | FM5324 充放电 + 5 V 升压供主控，电机侧电池直供 |
| 结构 | H 型 PCB，底部 M2 金属尖脚 |
| 螺旋桨 | 65-75 mm 正反桨/对称桨，后续实测确定 |

## 引脚分配

按 ESP32-C3 SuperMini 的物理位置分组，尽量减少 PCB 走线交叉。

| 功能 | ESP32-C3 GPIO | 连接目标 |
|---|---:|---|
| MPU6050 SDA | GPIO4 | MPU6050 SDA |
| MPU6050 SCL | GPIO3 | MPU6050 SCL |
| MPU6050 INT | GPIO2 | MPU6050 INT，可选，建议串 1 kΩ 或预留 0 Ω |
| DRV8833 nSLEEP | GPIO5 | DRV8833 nSLEEP |
| 电机 PWM_A | GPIO6 | DRV8833 AIN1 |
| 电机 PWM_B | GPIO7 | DRV8833 AIN2 |
| 电池电压 ADC | GPIO0 | 电池分压采样 |
| 串口预留 | GPIO20 / GPIO21 | 后续 UART 调试 |

DRV8833 没有单独的 PWM 脚，PWM 直接打到 `AIN1` 或 `AIN2`：

```text
正向推力：AIN1 = PWM，AIN2 = LOW
反向推力：AIN1 = LOW，AIN2 = PWM
滑行停止：AIN1 = LOW，AIN2 = LOW
刹车停止：AIN1 = HIGH，AIN2 = HIGH
```

## 仓库结构

```text
.
├── README.md                 项目说明和当前状态
├── docs/                     器件手册、参考资料
├── hardware/                 原理图、PCB、封装库、BOM 等硬件资料
├── softwave/                 ESP32-C3 固件和控制算法代码
├── report/                   课程设计报告、调研文档
├── imagine/                  图片、截图、结构示意图
└── .gitignore                Git 忽略规则
```

主要文档：

- [平衡梯硬件方案调研](report/平衡梯硬件方案调研.md)
- [ESP32C3SuperMini 入门手册](docs/ESP32C3SuperMini%20入门.pdf)
- [ESP32-C3 核心板引脚图](imagine/esp32C3核心板.png)

## 开发计划

- [x] 确定主控、姿态传感器、电机驱动和基本引脚分配
- [x] 完成硬件方案调研文档
- [ ] 绘制第一版 H 型 PCB 原理图
- [ ] 完成 PCB 外形、尖脚、电池绑带和电机座布局
- [ ] 编写 MPU6050 姿态读取与互补滤波代码
- [ ] 编写 DRV8833 电机正反转 PWM 控制代码
- [ ] 完成单梯闭环控制和 PID 调参
- [ ] 记录推力测试、角度响应曲线和抗扰动实验
- [ ] 尝试双层平衡梯叠放演示

## Git 使用建议

常用流程：

```bash
git status
git add README.md report/ hardware/ softwave/
git commit -m "docs: add project readme"
git log --oneline
```

建议提交粒度：

- `docs:` 文档、报告、说明文件
- `hardware:` 原理图、PCB、BOM、封装
- `firmware:` ESP32 固件、控制算法
- `test:` 测试记录、实验数据
- `chore:` 目录整理、忽略规则、工程配置

不建议提交的内容：

- 临时文件、缓存目录、软件自动生成的大量中间文件
- 本地编辑器缓存
- 编译输出文件
- 与项目无关的下载资料

## 当前注意事项

- `GPIO2` 是 ESP32-C3 启动配置相关脚，接 MPU6050 `INT` 时建议串联电阻或预留 0 Ω 电阻位。
- `GPIO8` 是 SuperMini 板载蓝灯，不建议接 MPU6050 I2C。
- 电机供电和 MPU6050/I2C 走线要分区，避免电机噪声导致姿态数据跳变。
- 单电机正反转螺旋桨方案需要实测反向推力；如果反向推力太弱，需要换桨或调整结构。
