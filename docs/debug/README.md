# Balance Ladder Debug

平衡梯项目的手机端调试工具，部署在 GitHub Pages，可直接用手机浏览器打开并添加到主屏幕。

访问地址：

```text
https://liziyaaa.github.io/balance_ladder/
```

## 主要功能

- 通过 Web Bluetooth 搜索并选择附近 BLE 设备，再连接 ESP32-C3 固件中的 `BalanceLadder` 调试服务。
- 显示状态、角度、目标角、误差、角速度、电机输出、按键和故障状态。当前固件 BLE Notify 限速约 10 Hz。
- 使用正方体显示当前姿态，并在 3D 场景中标出 X/Y/Z 坐标轴。
- 提供误差和电机输出曲线，方便观察调参过程中的响应趋势。
- 支持快捷命令、PID 参数、目标角、正反向风力补偿、基准输出和手动电机测试命令。
- 预留 WiFi WebSocket 和 HTTP 命令入口，方便后续扩展 ESP32 本地网页或 AP 调试。

## BLE 协议

设备名：

```text
BalanceLadder
```

NUS 风格 UUID：

```text
Service  : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
RX Write : 6E400002-B5A3-F393-E0A9-E50E24DCCA9E
TX Notify: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E
```

遥测格式：

```text
T,<ms>,<state>,<angle>,<target>,<error>,<gyro>,<cmd>,<key>,<fault>
```

示例：

```text
T,83996,ARMED,269.73,270.00,0.27,-0.30,-0.016,0,NONE
```

## 支持命令

快捷命令：

```text
status
defaults
level
arm
stop
fault_clear
cal
auto_on
auto_off
```

参数命令：

```text
target=270
kp=0.02
ki=0
kd=0.004
limit=1
gain_pos=1.00
gain_neg=1.35
min_pos=0.18
min_neg=0.24
kick_pos=0.24
kick_neg=0.34
baseline=0
motor=0.3
```

命令行也可以直接输入固件支持的其他命令，例如 `motor_stop`、`motor_full`、`motor_full_rev`、`force_wake_on`。当前固件目标角会限制在 `250..290 deg`，直立附近默认目标为 `270 deg`。

## 手机使用

1. 使用 Android Chrome 或 Android Edge 打开 GitHub Pages 地址。
2. 点击 `搜索蓝牙设备`，在浏览器弹出的蓝牙列表中选择 ESP32-C3 设备。
3. 设备会出现在页面里的已选择设备列表中，点击 `连接选中设备`。
4. 连接成功后页面会自动发送 `status`，随后显示遥测数据。
5. 点击右上角 `校准视图` 可以把当前姿态作为 3D 显示参考。
6. 在浏览器菜单中选择“添加到主屏幕”，后续可以像 App 一样打开。

注意：iPhone Safari 通常不支持 Web Bluetooth，因此 BLE 调试主要面向 Android 手机。

Web Bluetooth 不允许网页在未授权的情况下静默扫描并直接显示完整附近设备列表；搜索按钮会调用浏览器原生蓝牙选择器，用户选择并授权后，页面才会显示该设备并允许连接。

## WiFi 模式

当前页面预留了：

- WebSocket：`ws://192.168.4.1/ws`
- HTTP 命令：`http://192.168.4.1/cmd?c=<command>`

GitHub Pages 是 HTTPS 页面，浏览器可能会拦截普通 `http://` 和 `ws://` 连接。WiFi 调试更适合后续把本页面放到 ESP32 本地 HTTP 服务中，或为 ESP32 端实现 HTTPS/WSS。

## 本地预览

在 `docs/debug` 目录启动一个静态服务器：

```bash
python -m http.server 8765
```

然后打开：

```text
http://127.0.0.1:8765/
```
