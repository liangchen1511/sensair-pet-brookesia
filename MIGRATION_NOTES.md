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
5. C5 上 Emote 播放默认限制为 12 FPS，并启用音频优先的超时丢帧策略，降低刷新和音频并发压力。
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

- 主应用：`build/sensair_pet_brookesia.bin`，5,742,416 字节；
- 本地唤醒模型：`build/srmodels/srmodels.bin`，125,951 字节；
- 动态表情资源：`build/anim_icon.bin`，312,926 字节；
- XiaoZhi 激活播报资源：`build/littlefs_data.bin`，512,000 字节；
- 7 MB 应用分区剩余约 22%。

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

## 自定义黑白 EAF 已替换官方 320 × 240 表情

当前构建不再选择官方 `320_240` 表情目录，而是通过官方
`build_speaker_assets_bin()` 接入项目外部资源：

```text
assets/sensair_bw/
└─ 240_284/
   ├─ config.json
   ├─ emote.json
   └─ layout.json
```

`Emoji/emote-assets.bin` 已被解析为独立 EAF 文件，并放入
`assets/sensair_bw/emoji_sensair_bw/`。检查结果表明源 EAF 实际是
`240 × 240`，不是 `320 × 240`；它们被放在 `240 × 284` 屏幕画布中央，
上下各保留约 22 像素空间。`layout.json` 中眼睛和紧急表情均使用
`GFX_ALIGN_CENTER`，且 `x/y` 偏移为 0，用于修正此前表情偏右的问题。

当前 Agent 情绪名已经映射到黑白 EAF 循环片段，包括：待机、开心、悲伤、
愤怒、惊讶、思考、困倦和眨眼。第一版优先使用每组 `step2` 循环段，
原始 `step1/step2/step3` 文件均已保留，后续可继续实现“进入—循环—退出”时序。

最终 `anim_icon.bin` 已反向检查，共包含 17 个资源，其中 9 个为实际使用的
`240 × 240` EAF；旧的 `Happy.eaf` 等官方 320 × 240 表情不再存在。
在此基础上继续移除主表情页字体、文字标签和时钟定时器后，最终资源包由
1,178,705 字节缩小到 312,926 字节，进一步减少约 73.5%。

替换表情资源后必须重新烧录 `anim_icon` 分区。完整烧录最稳妥；若只更新资源，
则应按当前分区表把 `build/anim_icon.bin` 写入 `0x8aa000`，不要只烧主应用。

## 当前已知边界

- 黑白 EAF 的内容尺寸是 `240 × 240`，当前采用居中显示而非拉伸到 `240 × 284`。
- 视觉居中已经在资源布局层修正，但仍需上板确认 LCD 方向、面板偏移和素材自身
  的视觉重心；若仍有轻微偏移，只调整 `layout.json`，不再修改显示驱动。
- `listen.eaf` 当前复用待机循环素材，后续可替换为专门的倾听/声波动画。
- 三段式进入和退出动画尚未串联，当前优先保证表情、语音和网络并发稳定。

## 本次优化：无文字主界面与音频优先丢帧

主表情页现在采用无文字模式：

- `config.json` 的表情文字字体设为 `none`；
- 移除电量文字、Toast 文字、时钟文字和时钟刷新定时器；
- 保留黑白 EAF、Wi-Fi/状态/充电图标和配网二维码；
- 保留独立的 LVGL 设置页，因此重新配网、音量和 Reset 操作不受影响；
- XiaoZhi 激活码不再显示为屏幕文字，但 `activation.mp3` 和数字 MP3 播报仍保留。

同时启用 `CONFIG_SENSAIR_PET_AUDIO_FIRST_RENDERING`：

- Audio Mixer 保持高优先级 20，XiaoZhi 音频任务保持优先级 5；
- LVGL 显示任务降到优先级 4；
- Emote 渲染和触摸手势任务降到优先级 3；
- ESP32-C5 上 EAF 刷新上限由 15 FPS 调整为素材原生的 12 FPS；
- 动画时序改为按真实经过时间推进。如果显示来不及，不补画积压的历史帧，
  而是直接跳到当前时间应显示的最新帧，每次只解码和提交一帧；
- 无限循环片段会在片段范围内取模，不会因为一次长延迟越界；进入和退出片段
  仍保留原有排空语义，避免状态切换被彻底跳过；
- 丢帧累计达到日志周期时会输出
  `audio-first: skipped ... overdue animation frames`，便于实机观察系统压力。

新增两个默认开启的 Kconfig 开关：

```text
CONFIG_SENSAIR_PET_TEXTLESS_EMOTE=y
CONFIG_SENSAIR_PET_AUDIO_FIRST_RENDERING=y
```

最终链接映射已确认不含 Emote 默认字体符号，反向解包也确认资源分区不含字体文件。
相对优化前，主应用减少
139,600 字节（约 2.4%），`anim_icon.bin` 减少 865,779 字节（约 73.5%）。
主要运行时收益来自取消每秒时钟文字更新、减少 GFX 标签绘制，以及显示积压时
不再追赶历史动画帧；音频繁忙时允许动画视觉上轻微跳帧，以换取语音采集和播放连续。

这次主应用和 `anim_icon` 资源都发生了变化，上板时必须同时烧录两者，建议直接
执行完整 `flash`。验收时重点听音频是否连续，并观察动画在压力下只跳帧、不减慢
整段时间轴，也不出现显示任务看门狗。

## 下一阶段

官方基线通过实机验收后，再把旧工程中的 Home Assistant 能力迁移为独立
Brookesia Service，并通过 XiaoZhi MCP 注册灯光、空调、场景和状态查询工具。
这样 Home Assistant 不会侵入官方 Agent、配网、音频和表情显示链路。
