# 犀重 LSMR 资源清单

- 来源：`\\njdatasrv\测试部公共盘\01_软件测试组\02_项目管理\客户项目测试工程\犀重\09-Flash_XZ_test .zip`，ZIP SHA-256 `D1DA40D2947B1087B5D897ADC726433744988F1F02AC038A499DA664C59299F8`。
- CAPL 基线：`CAPL\Flash.can`，SHA-256 `40C37CD5F08076A4EA57476D5D3B2C22342E4F275803A140A12684302CAE5F96`。
- 该 CAPL 与已验收的 RSMR `Flash.can` 内容相同；Panel 通过 `RaderID` 区分目标，`RaderID.can` 给出主雷达 FT `0x701/0x761`、从雷达 FT `0x714/0x71C`。
- `Flash.can::Download()` 的 `RaderID == 1` 分支当前为空，因此 CANoe 工程只提供 LSMR 诊断端点、安全 DLL 和选择框架，没有可直接复刻的 LSMR 下载 Case；工具不得把 RSMR 台架结论继承给 LSMR。
- 工具为 LSMR 创建独立目标 Workflow，并复用公共刷写引擎；LSMR APP 执行方法属于待台架验证实现，不宣称为 CANoe LSMR PASS。
- 工程包中的 `ARC2.33C1_HQ001A_FlashDrv.s19` 只被 `Download_File.Ini` 的 RSMR 路径引用，不能证明是 LSMR 默认 Driver；它仅作为来源参考保存在源码资源目录，不在 LSMR Profile 中自动选择。
- LSMR Driver、APP 与 Hash 均不随工程包提供；刷写前必须从同一 LSMR 发布包手动选择。工具在访问 CAN 前检查 Driver 固定窗口 `0x00080000/0x400`、APP 固定窗口 `0x000C0000/0x300000`、APP SHA-256 与 Hash S19，以及 LSMR SeedKey 已知向量。
- 安全 DLL：`XZ_GenerateKeyEx_LSMR.dll`，SHA-256 `CF2CC207879AA899F51D2AB84C2D54496E5667C226820829D572EEBA5107336C`；离线已知向量 `FDBAAF18 -> 2A984258`（level `0x11`）。
- 该资源与离线检查不等同于实际 ECU 刷写验收。
