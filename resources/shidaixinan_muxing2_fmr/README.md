# 时代新安木星2代 FMR 资源与证据边界

- 公共盘来源：`时代新安/112_时代新安木星2代_ARF2.32_30147/flash_boot_HJZJ_MX2D.7z`；只读提取，源包未修改。
- 源包 SHA-256：`5E94E9A529713395CA4B9CEA4DFCA953A9B2B81ACF798185F217A3135A34FAE9`。
- `Reference/PLS` 保存项目 PLS 来源文件，仅用于来源冻结、布局核对和恢复场景分析，不是新增 Profile 的默认目标 APP。
- PLS S19 为单段 `0x000C0000 / 0x17C000`。
- 历史 APP 日志可见 `SDMX2A ... CHF0356N` 完整主体并有 `DownLoadOk` 记录，但本目录未取得对应 APP S19，运行时必须人工选择正确 APP。
- 历史 PLS 日志在下载、校验、依赖检查和 `11 01` 后，复位后的功能 `10 03` 超时并记为 `DownLoadError`；因此 FT 仍待真实 ECU 验收。
