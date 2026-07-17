# 小智本地环境问询计划

## 目标

让小智能通过本地 MCP 工具读取 ESP-SensairShuttle 的环境参数，并用自然语言回答：

- 当前温度
- 当前湿度
- 当前气压
- BME690 气体阻值
- 基于气体阻值变化推断的 VOC/异味趋势
- 当前舒适度判断

## 第一阶段实现

第一阶段采用轻量、稳定优先的方案：

```text
BME690 低频后台采样
        ↓
SensorContext 内存缓存最近 20 个点
        ↓
CustomService 注册本地函数
        ↓
XiaoZhi MCP 工具调用
```

已注册给小智的本地函数：

- `CustomService.GetEnvironmentSnapshot`
- `CustomService.GetEnvironmentTrend`
- `CustomService.GetComfortStatus`

采样策略：

- 默认每 30 秒采样一次。
- 最近 20 个点仅保存在内存中，不写 NVS。
- 传感器缺失或初始化失败时，不影响 Wi-Fi、小智和表情启动。
- 小智问询时只读缓存，不在语音对话线程里阻塞采样。

## VOC 表达原则

当前阶段不直接输出 VOC ppm，也不声称绝对空气质量等级。

BME690 原始气体阻值受温湿度、预热时间、传感器基线和环境影响较大，因此第一阶段只表达趋势：

- 气体阻值下降：可能 VOC/异味负荷上升，返回 `voc_trend = rising`
- 气体阻值上升：可能空气状况改善，返回 `voc_trend = falling`
- 变化不明显：返回 `voc_trend = stable`

后续如果接入 BSEC，再补充 IAQ、breath VOC equivalent、CO2 equivalent 等更接近用户理解的指标。

## 实机验证

烧录后重点看串口：

```text
Sensor context task started
BME690 initialized, chip id: 0x..
BME690 sample: xx.x C, xx.x %, xxxx.x hPa, gas xxxxxx ohm
Added environment service tools: [...]
```

可以问小智：

- “现在房间温度多少？”
- “湿度怎么样？”
- “气压是多少？”
- “空气是不是有点闷？”
- “VOC 趋势怎么样？”

如果传感器没有插入，小智工具应返回 `available=false` 和原因，而不是导致小智初始化失败。

## 下一阶段

1. 根据串口确认 BME690 是否能稳定初始化。
2. 如果气阻读数稳定，再把环境状态映射到桌宠表情，例如闷热进入 `concerned` 或 `alert`。
3. 评估是否接入 BSEC；只有需要 IAQ/等效 VOC/等效 CO2 时才进入 BSEC 阶段。
4. 增加 Home Assistant 联动，例如“空气变差时打开空气净化器”。
