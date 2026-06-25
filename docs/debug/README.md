# Balance Ladder Debug

这是平衡梯项目的手机端调试工具，设计为 GitHub Pages 可直接发布的静态网页。

## 功能

- Web Bluetooth 连接 `BalanceLadder` BLE 设备。
- 使用 NUS 风格 UUID：
  - Service: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
  - RX Write: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
  - TX Notify: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`
- 解析固件遥测：
  - `T,<ms>,<state>,<angle>,<target>,<error>,<gyro>,<cmd>,<key>,<fault>`
- 支持快捷命令、PID 参数、目标角、手动电机命令。
- Three.js 3D 姿态显示和平面曲线显示。
- 预留 WiFi WebSocket/HTTP 命令调试入口。

## 手机兼容性

Web Bluetooth 主要支持 Android Chrome/Edge。iOS Safari 通常不支持网页蓝牙。

GitHub Pages 是 HTTPS 页面，浏览器可能拦截普通 `http://` 或 `ws://` 的 WiFi 调试连接。WiFi 模式更适合后续把本页面放到 ESP32 本地 HTTP 服务中使用，或给 ESP32 端实现 `wss://`。
