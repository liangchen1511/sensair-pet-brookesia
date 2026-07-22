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

## 说话卡顿：片内 DMA 保留池

实机 XiaoZhi 播报时曾出现片内空闲跌到约 1%、并伴随
`Received audio packet with wrong sequence`。将
`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 从 8192 提高到 **16384**，
为 I2S/Wi‑Fi 等 DMA 与必须放片内的分配预留更多池子。该项写在
`sdkconfig` 与 `sdkconfig.defaults`。若仍丢包可再试 32768；若启动失败
或普通堆告警增多，再回退。

无文字主界面下也不再推送 `Speak` 事件消息（左上角喇叭图标），且不订阅
Agent/User 说话字幕事件，避免无字 UI 下多余的 `SetEventMessage(..., text)`。
倾听麦克风图标仍保留。对 CPU 收益有限，主要减少说话时状态栏脏区刷新。

## 第一阶段性能优化：异步表情 DMA 与 40 MHz LCD SPI

针对 C5 上“动画和语音并发时仍有卡顿”的问题，本阶段完成了显示主链路优化：

- Emote 的 `FlushReady` 不再调用 `esp_lv_adapter_dummy_draw_blit(..., true)` 同步等待每个分块传输完成；
- 改为 `wait=false` 异步提交，由 LVGL Adapter 已持有的 LCD IO 回调转发真实 DMA 完成事件；
- 只在 `on_color_trans_done` 中调用 `native_notify_flush_finished()`，不会另行注册或覆盖底层 LCD 回调；
- 参数解析失败、dummy draw 未启用或异步提交失败时仍会主动通知 Emote，避免渲染线程永久等待；
- ILI9341 SPI 时钟由 20 MHz 提高至 40 MHz，240×240 RGB565 完整帧的理论线速传输时间由约 46 ms 降为约 23 ms；
- SPI `max_transfer_sz` 由 3600 提高至 12288 字节，可完整容纳当前 16 行 Emote 块和 20 行 LVGL 块；
- 保留 12 FPS、双 DMA 缓冲、音频优先级和过期动画帧主动跳过策略。

每 5 秒会输出一条低频统计：

```text
Emote perf/5s: submit=..., done=..., err=..., DMA avg/max=.../... us,
max submit gap=... us, internal free/largest=.../...
```

`submit` 与 `done` 应大致相等，`err` 应为 0。若 40 MHz 下出现花屏、错色或触摸/显示不稳定，优先把
`pclk_hz` 回退为 30000000，而不是恢复同步刷屏。本阶段构建通过，应用大小为 `0x578920`，7 MB 应用分区剩余约 22%。

## 实机日志修复：上电白块与 XiaoZhi UDP 丢包

第一阶段实机日志显示异步显示链路本身稳定：`submit` 与 `done` 始终相等且 `err=0`，因此照片中的白块不是
40 MHz SPI 传输出错。根因是进入 dummy draw 之前，LCD GRAM 仍保留上电或 LVGL 的白色背景；240×240 EAF
只刷新自己的脏区，于是白块会被动画逐步覆盖，看起来像逐渐缩小。

修复方式是在每次进入表情页时，先加载一个纯黑 LVGL 过渡屏并调用 `lv_refr_now()` 完整刷新 240×284，完成后
再启用 dummy draw 和 `RefreshAll`。这样也能清除从设置页返回时可能残留在 EAF 范围外的像素。

同时修正第一阶段 DMA 统计：双缓冲可能同时存在两个待完成传输，原先单时间戳会把不同传输错配，导致日志出现
几十到数百毫秒的虚假 DMA 最大值。现在改为 4 项单生产者/单消费者时间戳队列，非 Emote 的 dummy draw 完成事件
不会错误地释放 Emote 缓冲。

串口中持续出现 `Received audio packet with wrong sequence`，因此进入第二阶段的低内存风险部分：

- XiaoZhi UDP 接收任务启动时一次性分配解密缓冲，后续所有音频包复用，取消逐包 `calloc/free`；
- GMF Opus feeder FIFO 从 5 项提高到 8 项，为解码偶发停顿提供更大吸收空间；
- 每 5 秒输出 `Audio feeder/5s`，统计送入解码器的平均/最大阻塞时间及超过 20 ms 的次数；
- 暂不扩大 lwIP UDP mailbox 或 Socket 接收缓冲，因为实机片内空闲约 18 KB、最大连续块约 14 KB，需要先验证本轮收益。

本轮构建通过，应用大小为 `0x5792b0`，7 MB 应用分区仍剩余约 22%。

## 下一阶段

官方基线通过实机验收后，再把旧工程中的 Home Assistant 能力迁移为独立
Brookesia Service，并通过 XiaoZhi MCP 注册灯光、空调、场景和状态查询工具。
这样 Home Assistant 不会侵入官方 Agent、配网、音频和表情显示链路。
# 2026-07-17：C5 音频优先半双工阶段

最新串口日志显示 Wi-Fi RSSI 约为 `-34 dBm`，但对话期间总 CPU 达到约 `91%–92%`。其中 `audio_recorder_` 约占 `52%–53%`，`audio_feeder_ta` 约占 `20%`，同时持续出现 `Received audio packet with wrong sequence` 和大量动画过期帧。由此判断主要瓶颈是 ESP32-C5 单核 CPU 饱和，不是弱网或 SPI DMA 提交失败。

工程本来已经使用 Brookesia `HalfDuplex`，但官方默认实现只在 TTS 期间丢弃麦克风编码数据，AFE 和 recorder 仍持续运行。本阶段新增可配置项：

```text
CONFIG_BROOKESIA_AGENT_MANAGER_HALF_DUPLEX_STOP_ENCODER_WHILE_SPEAKING=y
```

启用后：

- Agent 开始说话时调用 `stop_audio_encoder()`，完整停止 recorder、AFE 和编码器。
- Agent 结束说话后自动调用 `start_audio_encoder()`，恢复本地唤醒。
- Agent 正在 Stop 流程时不会错误重启编码器。
- 保留原有音频优先显示策略，动画来不及时继续丢弃过期帧。

产品取舍：TTS 播报期间无法使用本地唤醒词打断，首版使用触摸打断；这是单核 C5 原型的明确限制。如果必须支持全双工语音打断，应评估双核 ESP32-S3/S31。

新的产品范围、优先级和验收指标见 `PRODUCT_REQUIREMENTS_CN.md`。

## 截至 2026-07-17 的工程修改总览

当前 `sensair-pet-brookesia/` 已不再是官方 Chatbot 示例的简单复制，而是形成了面向
ESP-SensairShuttle 的桌面宠物固件基线。相对官方示例和最初迁移版本，累计完成了以下改动：

1. **硬件与系统基线**
   - 目标芯片固定为 ESP32-C5 N16R8，工具链固定为 ESP-IDF 5.5.4。
   - 复用官方 ESP-SensairShuttle HAL，接入 ILI9341、CST816S、ADC 麦克风和 I2S 扬声器。
   - 修正 C5 单核兼容性，避免任务绑定到不存在的 CPU 1。
   - 保留 Brookesia ServiceManager、NVS、SNTP、Device、Audio、Emote 和 AgentManager 架构。

2. **小智与配网**
   - 使用官方 `brookesia_agent_xiaozhi`，没有继续使用旧工程自建的 `XiaozhiBridge`。
   - 接入 SoftAP 首次配网、断线恢复和设置页 Reset 重新配网。
   - 接入官方激活码事件、激活语音资源、MCP 工具和 Agent 状态事件。
   - 使用 WakeNet 本地唤醒词“你好小智”。
   - C5 上采用半双工音频策略：小智说话时停止 AFE/录音编码，结束后恢复，以降低单核 CPU 压力。

3. **动态表情与显示**
   - 将官方 320×240 表情替换为自定义黑底白眼 EAF 资源。
   - EAF 内容为 240×240，居中放入 240×284 LCD 画布，修复表情偏右问题。
   - 主表情页移除字体、字幕、时钟和无用标签，保留设置页、图标和配网二维码。
   - LCD SPI 提升到 40 MHz，Emote 使用异步 DMA flush；不覆盖 LVGL Adapter 已注册的 LCD 回调。
   - 上电或从设置页返回时先全屏刷黑，修复 LCD GRAM 未清空导致的上方白块。
   - C5 动画限制为 12 FPS；显示来不及时按真实时间轴跳过过期帧，优先保证语音。
   - 新增 Emote DMA、提交间隔和片内内存统计，便于实机定位卡顿。

4. **语音性能与内存**
   - XiaoZhi UDP 解密缓冲改为启动时一次分配并复用，避免逐包 `calloc/free`。
   - Opus feeder FIFO 从 5 项提高到 8 项，并增加每 5 秒音频 feeder 统计。
   - 片内 DMA 保留池提高到 16 KB。
   - 取消无文字界面下不必要的字幕和说话图标刷新。
   - 保留“音频优先、动画主动丢帧”的产品策略；C5 首版不承诺 TTS 期间语音唤醒打断。

5. **环境感知与本地问询**
   - 新增 BME690 温度、湿度、气压和气体阻值采样。
   - 新增短期趋势缓存和小智 MCP 环境问询。
   - 传感器缺失时降级运行，不应影响显示、Wi-Fi、小智或设置功能。

Home Assistant 尚未迁移到本工程。旧工程 `sensair-pet/` 中的 HA REST、别名映射和确认机制仍作为
后续实现参考，计划最终以独立 Brookesia Service 接入。

## BME690 环境上下文与 XiaoZhi MCP

### 数据链路

新增文件：

```text
main/modules/sensor_context.hpp
main/modules/sensor_context.cpp
ENVIRONMENT_QUERY_PLAN_CN.md
```

实现内容：

- 复用 `examples/factory_demo/common_components/brookesia_app_temperature/` 中的 Bosch BME69x SensorAPI；
- 使用 Board Manager 已初始化的 I2C0，不重复创建 I2C 总线；
- GPIO9 将 BME690 SDO 拉低，正常地址为 `0x76`；驱动同时自动探测 `0x76` 和 `0x77`，兼容不同 SDO 电平或子板版本；
- 采用 forced mode，每 30 秒读取一次；
- 缓存最近 20 个有效样本，只保存在 RAM，不频繁写 NVS；
- 当前数据包括温度、湿度、气压、气体阻值和数据时间；
- 趋势包括温度、湿度、气压、气阻以及由气阻方向推断的 VOC 趋势；
- 提供基于温湿度和气阻趋势的舒适度描述。

当前没有接入 Bosch BSEC，因此不能把原始气体阻值包装成准确的 VOC ppm 或 IAQ 数值。第一版采用保守表达：

- 气体阻值明显下降：可能的 VOC/异味负荷上升；
- 气体阻值明显上升：可能的 VOC/异味负荷下降；
- 变化较小：趋势稳定。

### MCP 工具精简

最初实现注册了三个环境工具：

```text
GetEnvironmentSnapshot
GetEnvironmentTrend
GetComfortStatus
```

实机日志显示，小智连接后发送包含 11 个工具的 `tools/list` 时，片内最大连续内存仅约 5 KB，随后出现：

```text
esp-aes: Failed to allocate memory
mqtt_client: Writing failed
AgentXiaoZhi: disconnected
```

为降低 MCP schema、JSON 序列化和 TLS 连续内存压力，现已合并为一个工具：

```text
Service.CustomService.GetEnvironmentStatus
```

单次调用可同时返回当前快照、短期趋势和舒适度。音量和静音可在本地设置页完成，因此 Device MCP 已全部取消。
XiaoZhi 当前只暴露一个环境查询 MCP。板卡信息、存储查询、Device 数据重置、音量和显示控制均不再进入
`tools/list`；重新配网、清除数据和音量控制继续由设置页负责。

当前串口会输出 `Backlight is not available, skip`，说明 ESP-SensairShuttle HAL 尚未提供可用的背光接口。
所以设置页亮度控制目前也不会生效，删除亮度 MCP 不会损失已有能力。若后续需要软件调光，应先在板级 HAL
实现 Backlight interface；MCP 只应调用已有能力，不能代替底层背光驱动。

## BME690 缺失导致黑屏/失联的两轮修复

### 第一轮：I2C device handle 泄漏

第一版在 BME690 `0x76` 无应答时，每 5 秒执行一次：

1. `i2c_master_bus_add_device()` 创建 handle；
2. 再调用 `i2c_master_probe()`；
3. 探测失败直接返回，没有移除 handle。

因此未插好传感器时会持续泄漏内部内存，最终导致 AES、MQTT、显示和音频无法继续分配资源，外部表现为
黑屏、无响应和小智反复断线。

修复后流程为：

1. 按 factory demo 的顺序先创建候选地址的 I2C device handle；
2. 直接读取 BME690 `0xD0` 芯片 ID 寄存器，期望值为 `0x61`，不再依赖单独的 `i2c_master_probe()`；
3. 依次尝试 `0x76` 和 `0x77`，任一地址读到正确芯片 ID 即采用；
4. 地址无应答、ID 不匹配或 Bosch 初始化失败都会立即移除 handle，不重新引入内存泄漏；
5. 缺失传感器时每 30 秒重试一次。

### 第二轮：专用任务栈和 MCP 清单占用过高

修复 handle 泄漏后，日志仍显示 XiaoZhi 启动音频和 TLS 时片内空闲约 10 KB、最大连续块约 5 KB，
`esp-aes` 依然可能分配失败。进一步优化：

- 删除 BME690 专用 FreeRTOS 任务，不再常驻占用 5 KB 内部任务栈；
- 使用工程已有 `TaskScheduler::post()` 和 `post_periodic()` 在 BackendWorker 上执行首次及周期采样；
- 首次采样通过 post 排到启动队列后部，让显示、小智和配网初始化先完成；
- 传感器采样周期维持 30 秒，单次 forced-mode 测量约百毫秒；
- 环境 MCP 三合一，Device MCP 全部取消，只保留一个环境查询工具。

本轮最终构建已生成：

```text
build/sensair_pet_brookesia.bin  5,777,952 bytes
```

最新构建包含 BME690 `0x76/0x77` 芯片 ID 自动识别，并移除了全部 Device MCP。完整烧录仍必须同时包含 `srmodels`、`littlefs_data` 和 `anim_icon`
资源分区，不能只把主应用视为完整产品固件。

## 当前实机状态与下一步验收

最新日志已经证明：

- LCD、触摸、LVGL Adapter 和 Emote 资源均能正常初始化；
- `anim_icon` 中 20 个 emoji、8 个 icon、6 个 layout 能正常加载；
- Emote flush 的 `submit` 与 `done` 相等，`err=0`；
- Wi-Fi 能连接，SNTP 能同步，小智能完成激活并连接服务器；
- 第一轮 I2C handle 泄漏已经消失。

改为芯片 ID 直读前的最新硬件日志返回：

```text
probe(0x76) -> ESP_ERR_NOT_FOUND
probe(0x77) -> ESP_ERR_NOT_FOUND
BME690 not found at either I2C address
```

这证明旧探测方式下两个合法地址都没有收到应答。为排除 `i2c_master_probe()` 与共享总线行为差异，最新固件已
按 factory demo 的顺序改为“先添加 device handle，再读取 `0xD0` 芯片 ID”，失败立即移除 handle。下一轮必须
烧录这版固件后再判断；插拔传感器子板前需要断电，不能热插拔。日志验收应确认：

```text
Sensor context sampling scheduled on backend worker
chip-id read at 0x76 -> ESP_OK, value=0x61
# 或 chip-id read at 0x77 -> ESP_OK, value=0x61
BME690 detected at 0x76
BME690 initialized
BME690 sample: ...
Added environment service tools: [Service.CustomService.GetEnvironmentStatus]
AgentXiaoZhi: connected
```

同时持续观察是否还出现：

```text
esp-aes: Failed to allocate memory
mqtt ... disconnected
task_wdt
Guru Meditation
```

若第二阶段固件下 TLS 仍因最大连续片内内存不足失败，下一优先级不是继续降低动画 FPS，而是进一步减少
启动时常驻的 Device/Profiler 对象，或把允许放入 PSRAM 的任务栈和非 DMA 缓冲迁移到外部内存。

## 本轮日志结论：性能可接受，优先完成传感器

本轮实机日志中：

- XiaoZhi 成功连接并完成 `tools/list`，没有再次出现 `esp-aes: Failed to allocate memory`；
- 服务器实际调用了 `Service.CustomService.GetEnvironmentStatus`，证明 MCP 注册和调用链已经打通；
- Emote 的 `submit` 与 `done` 持续相等，`err=0`，显示 DMA 链路正常；
- 片内空闲随录音器启停在约 12–18 KB 波动，最大连续块约 5–7.5 KB，仍偏紧但能运行；
- 仍能看到少量 `Received audio packet with wrong sequence` 和动画过期帧，用户确认当前轻微卡顿可以接受；
- 当前产品优先级转为 BME690 可用性，不继续为了轻微卡顿牺牲表情效果。

因此当前 MCP 产品边界为：

```text
XiaoZhi MCP：只保留 Service.CustomService.GetEnvironmentStatus
本地设置页：音量、静音、重新配网和 Reset
暂不可用：软件背光亮度（缺少 HAL Backlight interface）
```

最新主应用为 `build/sensair_pet_brookesia.bin`，大小 5,777,952 字节。下一次实机日志若两个地址的
芯片 ID 直读仍都失败，即可把问题收敛为传感器子板方向、插槽、接触、供电或硬件本体，而不是 XiaoZhi MCP、
JSON 返回或 BME69x 上层算法问题。

## 本轮最终实现：BME690 可视诊断与三页界面

### 对 `ESP_ERR_INVALID_STATE` 的判断修正

最新日志中，`0x76` 和 `0x77` 的芯片 ID 读取都返回 `ESP_ERR_INVALID_STATE`。进一步核对当前工程实际使用的
ESP-IDF 5.5.4 I2C 驱动及其测试用例后确认：开启 ACK 检查的普通 transmit/receive 在从设备无应答时，驱动
本来就可能返回 `ESP_ERR_INVALID_STATE`。因此这条错误不能单独证明“Board Manager 的 I2C 总线句柄损坏”，
更符合两个地址均未收到 BME690 应答的现象。

为消除歧义，本轮不再编译示例目录的 `examples/common/common.c`，新增工程自己的轻量端口层：

```text
main/modules/bme69x_port.hpp
main/modules/bme69x_port.cpp
```

新流程为：

1. 继续复用 Board Manager 已初始化的 I2C0，不重复创建总线；
2. GPIO9 拉低，优先探测标准地址 `0x76`，同时兼容 `0x77`；
3. 先调用 `i2c_master_probe()`，无应答明确归类为 `ESP_ERR_NOT_FOUND`；
4. 只在探测成功后创建 device handle，并复核 `0xD0` 芯片 ID 是否为 `0x61`；
5. 初始化成功后在整个运行周期复用同一个 device handle；
6. 读取失败时释放失效 handle，下次采样重新探测；无新数据但 I2C 正常时保留 handle；
7. I2C 采样由互斥锁串行化，UI 不直接访问 I2C。

串口中的新诊断日志应类似：

```text
BME69xPort: Probe 0x76: ESP_OK
BME69xPort: Detected BME69x at 0x76, chip id 0x61
SensorContext: BME690 initialized at 0x76, chip id: 0x61
SensorContext: BME690 sample: ...
```

若硬件仍无应答，则日志会明确显示：

```text
BME69xPort: Probe 0x76: ESP_ERR_NOT_FOUND
BME69xPort: Probe 0x77: ESP_ERR_NOT_FOUND
SensorContext: BME690 unavailable: BME690 not detected at 0x76/0x77...
```

此时应断电后重新检查传感器子板方向、插座接触和子板本体；不能热插拔。环境页面会显示 `NO SENSOR`，不会
用温度 `0`、湿度 `0` 或未初始化的芯片 ID 冒充有效数据。

### SensorContext 状态模型

`SensorContext` 新增线程安全的 `ViewData` 快照与以下状态：

```text
Stopped / Waiting / Ready / NotDetected / BusUnavailable / InitFailed / ReadFailed
```

显示层只读取缓存快照。有效样本包含：

- 温度；
- 湿度；
- 气压；
- 气体阻值及 gas-valid 标志；
- I2C 地址、样本数量、数据年龄；
- 由气体阻值方向推断的 VOC 趋势。

隐藏页面时仍维持 30 秒周期采样；环境页打开后每 5 秒向 BackendWorker 请求一次采样。请求有 pending 保护，
不会因为 LVGL 的 1 秒刷新计时器重复堆积 I2C 任务。

### 三页 LVGL 交互

新增：

```text
main/modules/display/screens/sensors.hpp
main/modules/display/screens/sensors.cpp
```

手势按手指运动方向定义：

```text
表情主页 --向右滑--> 设置页
表情主页 --向左滑--> 环境页
设置页   --向左滑--> 表情主页
环境页   --向右滑--> 表情主页
```

垂直滑动不再触发页面状态机，解决之前设置页普通滑动产生 `No transition for action` 的无效错误。

环境页采用 240×284 黑色界面：

- 顶部为 `ENVIRONMENT`、状态灯、状态文字和手动重试按钮；
- 四张深色卡片显示 `TEMP`、`HUM`、`PRESS` 和 `GAS`；
- 底部显示 VOC 趋势、数据年龄、BME690 地址/样本数或具体错误；
- 正常、等待、故障分别使用绿、黄、红状态色；
- 首版不使用实时曲线、渐变和持续动画，避免加重 C5 的 LVGL 刷新负担；
- 环境页进入时创建、退出时销毁，避免长期占用当前已经很紧张的片内 RAM；
- 离开 Emote 主界面时 Agent 按现有生命周期暂停，返回主界面后恢复，优先保证页面与音频资源稳定。

### 构建结果与实机验收

两次构建均通过。最新主应用为：

```text
build/sensair_pet_brookesia.bin  5,793,968 bytes (0x5868b0)
```

7 MiB 应用分区剩余 `0x179750`，约 21%。完整烧录参数仍包含 bootloader、partition table、SR 模型、
LittleFS 和 `anim_icon` 表情资源。

实机下一轮重点验收：

1. 表情主页向左滑能进入环境页，向右滑能进入设置页；
2. 未插传感器时环境页稳定显示 `NO SENSOR` 和断电插入提示；
3. 正常硬件应显示 `Probe 0x76: ESP_OK`、芯片 ID `0x61` 和四项实时数值；
4. 环境页 5 秒刷新期间触摸不卡死、无 task watchdog；
5. 返回表情主页后 Agent 能恢复，小智仍可调用 `GetEnvironmentStatus`；
6. 持续观察片内最大连续块、音频错序、Emote submit/done 以及是否出现 AES/MQTT 分配失败。
