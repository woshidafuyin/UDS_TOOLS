# 时代新安 ARF2.32 共用运行资源

本目录只存放由 `shidaixinan_hjzj_fmr` Workflow 共用的协议运行资源：

- `Driver/ARF2_32_ERadar_FlashDrv.s19`；
- `dll/FMR.dll`；
- `integrity/HJZJ_CRC32.dll`（来源留档，当前 C++ Workflow 使用已交叉验证的等价 CRC-32 实现）。

天王星、木星2代、庆铃通过独立 Profile 复用同一 Workflow 和本目录资源；各项目固件、来源记录与验证证据保留在各自资源目录，不在这里混放。

这三个新增 Profile 的默认 `app_file` 故意留空：截至 2026-08-12，公共盘项目包能确认各自 PLS S19 和 APP 成功日志，但没有确认与日志同版本的 APP S19 本体。运行前必须由用户选择与当前 ECU/项目匹配的 APP S19，文件解析和 SeedKey 预检通过后才会访问 CAN。

共用资源复制自本地已验证来源 `resources/shidaixinan_hjzj_fmr`；文件哈希见 `SOURCE_MANIFEST.sha256`。
