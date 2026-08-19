# 犀重 LSMR 资源清单

- 来源：`\\njdatasrv\测试部公共盘\01_软件测试组\02_项目管理\客户项目测试工程\犀重\09-Flash_XZ_test .zip`
- CAPL 基线：`CAPL\Flash.can`，SHA-256 `40C37CD5F08076A4EA57476D5D3B2C22342E4F275803A140A12684302CAE5F96`。
- 该 CAPL 与已验收的 RSMR `Flash.can` 内容相同；LSMR 仍使用独立的诊断端点、安全 DLL、NM 帧及身份检查。
- 固定 Driver：`ARC2.33C1_HQ001A_FlashDrv.s19`，地址窗口 `0x00080000`、长度 `0x400`。
- APP 与 Hash 不随工程包提供；刷写前必须从同一 LSMR 发布包手动选择并完成工具预检。
- 安全 DLL：`XZ_GenerateKeyEx_LSMR.dll`，SHA-256 `CF2CC207879AA899F51D2AB84C2D54496E5667C226820829D572EEBA5107336C`；离线已知向量 `FDBAAF18 -> 2A984258`（level `0x11`）。
- 该资源与离线检查不等同于实际 ECU 刷写验收。
