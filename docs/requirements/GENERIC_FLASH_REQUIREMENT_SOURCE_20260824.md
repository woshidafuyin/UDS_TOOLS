# 通用自动化刷写工程需求来源与实现边界

## 需求来源

- 公盘原始目录：`\\njdatasrv\测试部公共盘\01_软件测试组\02_项目管理\通用自动化刷写工程需求资料`
- 只读冻结副本：`D:\project\通用自动化刷写工程需求资料_公盘冻结_20260824`
- 冻结时间：2026-08-24
- 原始文件：72 个，1,630,036,188 字节；源端与冻结副本逐文件 SHA-256 为 72/72 一致。
- 清单：冻结目录下 `_SOURCE_SHA256.csv`，清单 SHA-256 为 `39C9BF528458ED796C8C2F951C1736EEE0C8739B19C56AEB224E5CD095CB5874`。

本仓库实现以冻结资料中 CANoe/CAPL 的正常刷写入口、随附软件包和配置为需求依据；既有通用刷写代码只能作为实现组件，不能反向覆盖项目需求。

## 项目契约

| 项目 | 独立 Workflow | 正常流程关键差异 | 当前证据边界 |
|---|---|---|---|
| 奇瑞 T1EJ ARS1.31 | `chery_t1ej` | APP：F15A、D004不带签名；CAL/TC_7：F184、Driver/CAL以DD02校验、DD03；APP+CAL/TC_2：F184、D004携带APP RSA、三段以D002校验、D005；三模式均为7AF/7BF、27 07/08 | 源码/离线验证，三模式待台架 |
| 奇瑞 T22 ARS1.31 | `chery_t22` | Panel含APP/CAL/APPAndCAL；APP=单次Download1；CAL/TC_7含85/28关闭与恢复、D003、F15A、D004(APP RSA)、Driver/CAL以D002校验，当前注释FF01/DD03；APP+CAL/TC_2以D002校验三段并执行FF01/D005；均为7AF/7BF、27 07/08 | 源码/离线验证，三模式待台架 |
| 奇瑞 E0Y ARS1.31 | `chery_e0y` | APP：0203、无D004、DD02且无安装例程；CAL/TC_7：0203、Driver/CAL以DD02校验、DD03；APP+CAL/TC_2：D003、D004携带APP RSA、三段以D002校验、D005；三模式均为70D/78D、27 11/12 | 源码/离线验证，三模式待台架 |
| 零跑 ARF统一入口 | `lp_arf` | A12/B11 ARF2.31 与 ARF6.31 共用751/759、PLS 701/761、APP 0C0000/180000和1322字节证书流程；支持TMP单包或S19/SREC/BIN配套ASC/TMP；项目资源与来源证据继续独立保存 | 源码/离线验证；A12、B11、ARF6.31仍需分别台架验收 |
| 北汽 | 不注册可执行 Workflow | 公盘目录为空，不推测诊断参数或刷写步骤 | `BLOCKED_REQUIREMENT` |

## 架构约束

1. Workflow 负责：协议契约、Profile 一致性检查、文件预检、报告标题和验收边界；执行方法一致的项目使用同一个参数化 Workflow。
2. 协议引擎负责：UDS 服务编排、NRC 0x78 等待、TransferData、例程响应判断和取消处理。
3. Profile 负责可配置项：CAN 通道/波特率、诊断 ID、地址窗口、安全级别、文件和 DLL 路径、入口模式。
4. A12/B11 ARF2.31 与 ARF6.31 已由现有资料证明正常执行方法一致，因此收敛到单一 ARF Workflow；固件、来源哈希和台架结论不得跨项目继承。
5. 离线单测、构建、安装包冒烟不等于真实 ECU 刷写 PASS。台架结论必须绑定实际 Profile、硬件、通道、软件文件哈希、日志和报告。

## 2026-08-24 离线发布记录

- 唯一发布目录：`D:\project\UDS_tools\UDS_tools\dist`
- 文件数/总字节：343 / 228646525
- `UDS_Tool.exe` SHA-256：`9524DE395107930316E9CB8D4522A3049797A82B2F39F823E39887638ED5E10F`
- `keygen_broker.exe` SHA-256：`7C4071E35A7E4A1AFB5BC5DB92BC66E2E413A9D4984590CBC8994B0D9A396BF0`
- Release x86/x64 与 Qt 构建通过，CTest 8/8 PASS；T1EJ、T22、E0Y 均已补齐 CAL/TC_7 与 APP+CAL/TC_2 的源码、Profile、UI入口和默认 CAL/RSA 资源。
- T1EJ/T22 `GenerateKeyExOpt` level 0x07、E0Y/KP31 level 0x11、A12/B11 level 0x11，以及既有项目 SeedKey/资源完整性发布门禁全部 PASS。
- 未连接 CAN 硬件，未向 ECU 发送 UDS，未执行真实刷写；上述项目仍为 `pending_validation=true`。
- 发布约束：`scripts\build.ps1` 默认且唯一更新仓库根目录下的 `dist`；不再创建 `dist-*` 或 `dist-ui-*` 并行候选目录。
