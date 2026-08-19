# 时代新安天王星 FMR 资源与证据边界

- 公共盘来源：`时代新安/111_时代新安天王星_ARF2.32_30146/flash_boot_HJZJ _TWX.7z`；只读提取，源包未修改。
- 源包 SHA-256：`81AEF34AD7C98E13E28F543A9148662CA1A7362BBB1FDC4A305D794E38B04577`。
- `Reference/PLS` 保存项目 PLS 来源文件，仅用于来源冻结、布局核对和恢复场景分析，不是新增 Profile 的默认目标 APP。
- PLS S19 为单段 `0x000C0000 / 0x17C000`。
- 历史 APP 日志可见 `SDTWXA ... CHF0354N` 完整主体并以 `DownLoadOk` 结束，但本目录未取得对应 APP S19，运行时必须人工选择正确 APP。
- 历史 PLS 日志在下载、校验、依赖检查和 `11 01` 后，复位后的功能 `10 03` 超时并记为 `DownLoadError`；因此 FT 仍待真实 ECU 验收。
