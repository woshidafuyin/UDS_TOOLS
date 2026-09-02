# 犀重 LSMR 当前接入状态与安全边界

更新日期：2026-08-25

## 当前结论

LSMR 已使用独立的 `xizhong_lsmr` Profile 和 Workflow 工厂，不再把
`xizhong_rsmr` 的工厂对象直接当作 LSMR。公共 CAN/ISO-TP/UDS 下载引擎仍可复用，
但所有目标差异由独立的 LSMR 契约约束。

本次接入是“代码和离线门禁完成、真实 ECU 待验证”，不是 CANoe LSMR 流程复刻 PASS。
源工程 `CAPL/Flash.can::Download()` 的 `RaderID == 1` 分支为空；源工程只给出了
LSMR 选择框架、诊断端点、NM 标识和安全 DLL，没有给出可直接执行的 LSMR 下载 Case。

## 来源证据

- 本地只读来源：`客户项目测试工程/犀重/09-Flash_XZ_test .zip`；
- ZIP SHA-256：`D1DA40D2947B1087B5D897ADC726433744988F1F02AC038A499DA664C59299F8`；
- `CAPL/Flash.can` SHA-256：`40C37CD5F08076A4EA57476D5D3B2C22342E4F275803A140A12684302CAE5F96`；
- `RaderID.can` 给出的 LSMR FT 端点为 `0x714/0x71C`；
- `Download_File.Ini` 只引用 `RSMR_AA_APP1_0801.s19`、RSMR Hash 和
  `ARC2.33C1_HQ001A_FlashDrv.s19`，没有 LSMR 固件组合；
- `XZ_GenerateKeyEx_LSMR.dll` 的离线已知向量为
  `FDBAAF18 -> 2A984258`，SecurityAccess level `0x11`。

## LSMR 独立契约

| 项目 | 当前约束 |
|---|---|
| Workflow ID | `xizhong_lsmr` |
| APP 请求/响应 | `0x18DAB6F1 / 0x18DAF1B6`，29 位扩展 ID |
| 功能请求 | `0x18DBFFF1` |
| 源工程 FT 请求/响应 | `0x714 / 0x71C`；因下载分支为空，工具不开放 FT |
| NM | `0x18FFA0B6`，Classic CAN |
| 身份 | `22 F150` 必须返回 `LSMR_AA` |
| 安全访问 | `27 11/12`，LSMR DLL，已知向量必须通过 |
| Driver 窗口 | `0x00080000 / 0x400` |
| APP 窗口 | `0x000C0000 / 0x300000` |
| APP 校验 | APP 窗口 SHA-256 必须等于 Hash S19 的 32 字节 |
| 总线 | nominal 500 kbit/s、data 2 Mbit/s、物理请求 CAN FD+BRS |

## 失败关闭规则

工具在创建 CAN 通道之前完成以下检查：

1. Profile 的 Workflow、诊断端点、FT 能力和 LSMR 契约完全匹配；
2. 入口只能是 `app`；`ft` 因源 CANoe LSMR 分支为空而明确拒绝；
3. Driver、APP 和 Hash 三份文件必须全部由用户选择，且应来自同一 LSMR 发布包；
4. Driver/APP 地址窗口完整；
5. Driver 必须通过当前犀重引擎固定 SHA-256 门禁；
6. APP SHA-256 必须与所选 Hash S19 一致；
7. LSMR SeedKey DLL/broker 必须通过离线已知向量；
8. 在线执行后先读取 `F187/F150/F189`，并以 `LSMR_AA` 身份检查阻止误刷 RSMR。

Profile 不再默认选择源工程中的 RSMR Driver。源目录保留该 Driver 仅用于来源审计和固定
Hash 回归；没有同一 LSMR 发布包的 Driver、APP 和 Hash 时，工具不会访问 CAN。

## 验收边界

当前完成项：独立 Workflow 注册、独立 Profile、目标级 `pending_validation=true`、端点/NM/
SeedKey/身份契约、缺文件和非法入口失败关闭、离线单元测试及发布资源门禁。

仍需真实 LSMR ECU 验收：

1. 冻结同一发布包的 Driver、APP、Hash 和各自 SHA-256；
2. 使用当前 `CH_Diagnostic_Studio.exe`、`xizhong_lsmr.ini` 和 LSMR SeedKey DLL；
3. 记录硬件后端、设备、通道、位率和供电条件；
4. 完整执行 APP 下载并保存 ASC、HTML 报告和执行日志；
5. 核对刷前身份、所有最终 Routine 状态、复位后的恢复服务和刷后版本；
6. 将证据与当前 EXE/Profile/固件哈希绑定后，才可标记 LSMR APP 实刷 PASS。

在上述证据完成前，RSMR 的 Vector/TOSUN APP PASS、RSMR FT 静态逻辑以及 LSMR 的
离线测试均不得外推成 LSMR 台架通过。
