# 楚能 ARC331 左后雷达 APP 实刷 PASS

## 结论

2026-08-24 使用通用刷写工具的 Release `dist`，在 Vector XL Channel 2
上对楚能 ARC331 左后雷达完成 1 次 APP 模式真实 ECU 完整刷写。物理诊断
端点为 `0x72E -> 0x72F`，功能寻址为 `0x7DF`，500 kbit/s / 2 Mbit/s。

本次执行真实覆盖了修复目标：在线探测和正式刷写均收到
`31 01 02 03 -> 7F 31 31`。工具将其记录为“当前 Boot/DCM 未注册
RID 0203”的 WARN 并继续，随后编程会话、Driver/APP 下载与 ABT、签名
校验、依赖检查、复位、通信恢复和清除 DTC 全部得到预期响应。HTML 报告
最终记录：

```text
本次流程结果：成功（PASS）
Q/CN A201-2025 compliant ChuNeng 331 sequence completed
Complete workflow passed
```

这项证据只验收“左后雷达 + APP + 本次 CBF 输入”组合。右后雷达 APP、
左右雷达 FT、其他固件输入和刷后版本读取仍需各自的台架证据，不能由本次
PASS 自动继承。

## 执行基线

- 时间：2026-08-24 14:09:19 至 14:10:30（Asia/Shanghai）
- 源码提交：`3465964bc8fc0687c351697ef6c639480908eff2`
- 工具：`UDS_Tool.exe`
- Profile：`chuneng_331_left_rear.ini`
- 目标：`left_rear` / 左后雷达
- 入口：`APP`
- 物理请求/响应：`0x72E / 0x72F`
- 功能请求：`0x7DF`
- CAN：Vector XL，Channel 2，500000 / 2000000 bit/s，CAN FD
- Driver CBF：`driver_712345678AB.cbf`
- APP CBF：`7052A5023002AB.cbf`
- SeedKey：`ChuNeng_D7_SeednKey_V1.0.dll`

Profile 的 `target_1_pending_validation=true` 暂时保留。该字段是目标级阻断
标记，当前 Profile 同时支持 APP 与 FT；仅 APP PASS 不足以解除左后目标的
FT 待验证状态。

## 关键正响应

```text
ProgrammingPrecondition: 7F 31 31 -> WARN，继续
ProgrammingSession:      50 02 00 32 01 5E
Driver TransferData:     8/8 PASS
Driver ABT:              PASS
DriverVerification:      71 01 02 02 04
EraseAPP:                71 01 FF 00 04
APP TransferData:        768/768 PASS
APP ABT:                 PASS
AppVerification:         71 01 02 02 04
DependencyCheck:         71 01 FF 01 04
ECUReset:                51 01
ClearDTC:                54
```

执行中的 `7F xx 78` 均由 ECU 随后的最终正响应闭合，属于
ResponsePending，不是最终失败。

## 冻结文件及 SHA-256

| 文件 | SHA-256 |
|---|---|
| `UDS_Tool.exe` | `EBFB8234341BC57B56E1E1FBAF42C0C7B2E4536CFAE6BF7054A0604A454BCA81` |
| `chuneng_331_left_rear.ini` | `429008C96965527BBD7984A14867E66B9924916DF49155521CC38BD9B3EFDE17` |
| `report_20260824_141030_688.html` | `2417B7322F7CED5A7569EABB2D8F2016295833D87D3736A99348204784FED1AF` |
| `trace_20260824_140919_chuneng_331_left_rear_left_rear_app.asc` | `BC763ED09913622864BDC4574B094B8AA9A288EA8F4FC8AC8FF80A4BC4414BD4` |
| `execution_20260824_140913_578.log` | `C074C9EC8DF54D9EC6C6E24B23BFBBBA699EC94863E9015E3F821A565CA4CB02` |

本次输入和运行依赖未重复复制，使用仓库/发布资源中的对应文件，以以下
SHA-256 绑定：

| 输入/依赖 | SHA-256 |
|---|---|
| `driver_712345678AB.cbf` | `A3D4B9A5323FDA405400712FA5E46D8CD0CB1D9AD218AEBCC35008716FB933C3` |
| `7052A5023002AB.cbf` | `3EEF5C26084570BD9B1E7C5430025A2A5ED307AEE955793BCC35CC05C8278205` |
| `ChuNeng_D7_SeednKey_V1.0.dll` | `DF4FEBCC26FAE799F9010B101EEDAB09A02CE1BEB866F2292F658B4AA76214FA` |

## 证据文件

- `report_20260824_141030_688.html`：完整流程报告和逐步骤判定。
- `trace_20260824_140919_chuneng_331_left_rear_left_rear_app.asc`：原始 CAN
  总线证据。
- `execution_20260824_140913_578.log`：界面运行、探测、正式刷写和报告路径
  记录。
- `UDS_Tool.exe`：本次实际使用的可执行文件。
- `chuneng_331_left_rear.ini`：本次 Profile 快照。
