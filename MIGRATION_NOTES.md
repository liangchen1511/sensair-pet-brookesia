# Sensair Pet 官方 Brookesia 基线迁移说明

## 为什么新建工程

本工程 `sensair-pet-brookesia/` 直接以 ESP-Brookesia 官方
`examples/agent/chatbot` 为产品基线，而不是继续扩展旧工程中自建的
`XiaozhiBridge`、LVGL fallback 和自定义 Emote 显示链路。

这样可以直接复用官方已经联调过的完整链路：

- `brookesia_agent_xiaozhi` XiaoZhi Agent；
- AFE、VAD 和本地 WakeNet 唤醒；
- XiaoZhi 激活码获取、屏幕显示和数字语音播报；
- SoftAP Wi-Fi 配网、断线恢复和设置页重新配网；
- LVGL 与 Emote 通过 `esp_lvgl_adapter` 协作的显示链路；
- Agent 状态、对话文字和情绪到动态表情的映射；
- MCP 工具注册和设备服务调用；
- NVS、SNTP、音频服务和设备服务。

旧工程 `sensair-pet/` 没有删除，仍可用于回看已经实现的 Home Assistant
接口、桌宠状态服务和旧版实验代码。

## 本次相对官方示例的改动

1. 项目名改为 `sensair_pet_brookesia`。
2. Brookesia HAL、Service、Agent 和 Expression 组件通过本地路径引用
   `../esp-brookesia-master`，便于继续同步和调试官方实现。
3. 生成并接入 ESP-SensairShuttle 的 Board Manager 配置，使用板载：
   - ESP32-C5 N16R8；
   - ILI9341 240 × 284 LCD；
   - CST816S 触摸；
   - ADC 麦克风；
   - I2S 扬声器。
4. 修正 ESP32-C5 单核兼容性：显示初始化、后台调度器和服务工作线程不再绑定不存在的 CPU 1。
5. C5 上 Emote 播放默认限制为 15 FPS，降低刷新和音频并发压力。
6. 本地唤醒词固定为“你好小智”，启用 WakeNet 模型
   `CONFIG_SR_WN_WN9S_NIHAOXIAOZHI`。
7. 增加 `tools-build-local.bat`，固定使用本机 ESP-IDF 5.5.4、Python 环境和 RISC-V 工具链。
8. 启动串口标识改为 `Sensair Pet (Brookesia)`。

## 已验证的构建结果

执行：

```powershell
.\tools-build-local.bat build
```

完整构建已经通过：

- 主应用：`build/sensair_pet_brookesia.bin`，5,882,016 字节；
- 本地唤醒模型：`build/srmodels/srmodels.bin`，125,951 字节；
- 动态表情资源：`build/anim_icon.bin`，2,739,934 字节；
- XiaoZhi 激活播报资源：`build/littlefs_data.bin`，512,000 字节；
- 7 MB 应用分区剩余约 20%。

完整烧录会写入：

```text
0x2000    bootloader/bootloader.bin
0x8000    partition_table/partition-table.bin
0x10000   srmodels/srmodels.bin
0xb0000   sensair_pet_brookesia.bin
0x82d000  littlefs_data.bin
0x8aa000  anim_icon.bin
```

不能只烧主应用，否则本地唤醒、激活语音或动态表情会缺少对应资源。

## 配网和重新配网

- 没有已保存 Wi-Fi 时，设备自动启动 SoftAP 配网，并在表情页显示二维码和 AP 信息。
- Wi-Fi 断开且无法恢复时，官方流程会切回 SoftAP 配网。
- 设置页的 Reset 会清除 Wi-Fi、Agent 和 Device 数据并重启，可用于更换网络或重新绑定 XiaoZhi。

## 仍需上板验证的事项

1. 首次完整烧录后，确认 SoftAP 配网页和触摸设置页正常。
2. 说“你好小智”，确认 AFE 能唤醒并进入监听状态。
3. 首次 XiaoZhi 激活时，确认激活码能显示且 `activation.mp3` 和数字音频正常播放。
4. 连续观察动态表情和语音并发时是否有看门狗、音频欠载或内存下降。
5. 验证设置页 Reset 后确实能重新配网、重新激活。

## 当前已知边界

官方表情素材目前只有 `320 × 240`、`360 × 360` 和 `1024 × 600` 三种预制规格；
ESP-SensairShuttle 的屏幕是 `240 × 284`，当前构建会使用 `320 × 240` 资源。
因此官方表情可以用于验证完整链路，但不能视为最终居中效果已经解决。
后续需要把 `Emoji/` 中的自定义素材按 240 × 284 画布重新生成官方 Emote 资源，
并在实际屏幕上校正缩放、裁切和中心点。

## 下一阶段

官方基线通过实机验收后，再把旧工程中的 Home Assistant 能力迁移为独立
Brookesia Service，并通过 XiaoZhi MCP 注册灯光、空调、场景和状态查询工具。
这样 Home Assistant 不会侵入官方 Agent、配网、音频和表情显示链路。
