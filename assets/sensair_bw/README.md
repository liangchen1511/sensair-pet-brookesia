# Sensair Pet 黑白 EAF 资源

- `emoji_sensair_bw/` 中的 EAF 从 `../Emoji/emote-assets.bin` 解包得到。
- EAF 实际画布为 `240 × 240`，在 `240 × 284` LCD 上按中心对齐，上下各留 22 像素。
- `240_284/emote.json` 将 XiaoZhi/Agent 的标准情绪名映射到黑白表情循环段。
- `240_284/layout.json` 关闭官方单眼自动镜像，因为这些 EAF 已经包含完整的双眼画面。
- `listen.eaf` 暂时复用中性眼睛循环段；后续拿到专用倾听 EAF 后可直接替换。
- `step1`、`step2`、`step3` 分别保留进入、循环和回正素材；当前官方情绪接口先使用 `step2`。
