# 文档资料

本目录保存平衡梯项目的参考资料、课程计划书和手机端调试工具。

## 文件说明

| 文件/目录 | 说明 |
|---|---|
| [debug/](debug/) | 手机端 BLE/WiFi 调试网页，GitHub Pages 发布源 |
| [ESP32C3SuperMini 入门.pdf](ESP32C3SuperMini%20入门.pdf) | ESP32-C3 SuperMini 核心板参考手册 |
| [C29781275_电池管理_FM5324HJ1_规格书_WJ1200378.PDF](C29781275_%E7%94%B5%E6%B1%A0%E7%AE%A1%E7%90%86_FM5324HJ1_%E8%A7%84%E6%A0%BC%E4%B9%A6_WJ1200378.PDF) | FM5324H 电源芯片/模块资料 |
| [《自动控制综合实习》2025-2026实习要求.pdf](%E3%80%8A%E8%87%AA%E5%8A%A8%E6%8E%A7%E5%88%B6%E7%BB%BC%E5%90%88%E5%AE%9E%E4%B9%A0%E3%80%8B2025-2026%E5%AE%9E%E4%B9%A0%E8%A6%81%E6%B1%82.pdf) | 课程实习要求 |
| [平衡梯主动姿态控制系统项目计划书.docx](%E5%B9%B3%E8%A1%A1%E6%A2%AF%E4%B8%BB%E5%8A%A8%E5%A7%BF%E6%80%81%E6%8E%A7%E5%88%B6%E7%B3%BB%E7%BB%9F%E9%A1%B9%E7%9B%AE%E8%AE%A1%E5%88%92%E4%B9%A6.docx) | 项目计划书 Word 版本 |
| [风力摆平衡梯控制系统计划书.pdf](%E9%A3%8E%E5%8A%9B%E6%91%86%E5%B9%B3%E8%A1%A1%E6%A2%AF%E6%8E%A7%E5%88%B6%E7%B3%BB%E7%BB%9F%E8%AE%A1%E5%88%92%E4%B9%A6.pdf) | 项目计划书 PDF 版本 |

## 调试网页

在线地址：

```text
https://liziyaaa.github.io/balance_ladder/
```

调试网页支持：

- Web Bluetooth 搜索并选择附近 BLE 设备，再连接 `BalanceLadder` 调试服务。
- 显示状态、角度、目标角、误差、角速度和电机输出；当前 BLE Notify 限速约 2 Hz。
- 发送 `target`、`kp`、`ki`、`kd`、`limit`、`min_cmd`、`baseline`、`motor` 等调参命令。
- 使用正方体和 X/Y/Z 坐标轴显示当前姿态。

GitHub Pages 由 [`.github/workflows/pages.yml`](../.github/workflows/pages.yml) 自动部署 `docs/debug/`。
