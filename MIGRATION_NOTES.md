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
