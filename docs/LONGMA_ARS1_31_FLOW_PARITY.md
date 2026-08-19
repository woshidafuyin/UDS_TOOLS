# 长马 J90K ARS1.31 当前刷写流程

更新日期：2026-08-12

## 当前结论

当前 `longma_ars1_31` Workflow 支持 APP 与 FT 两个入口，下载对象为 Driver + APP。长马 Profile 未开启 CAL 能力，CAL 地址和长度均为 0，因此不能因为共用内核具有 CAL 分支就把长马描述成支持 CAL。

## 当前入口与目标

配置文件：`profiles/longma_ars1_31.ini`

| 目标 | APP TX/RX | FT TX/RX | 当前 Profile 状态 |
|---|---|---|---|
| 主雷达 | `0x744/0x74C` | `0x714/0x71C` | `pending_validation=false` |
| 从雷达 | `0x760/0x768` | `0x714/0x71C` | `pending_validation=true` |

总线为经典 CAN，通道 2，500 kbit/s，填充值 `0x00`，功能地址 `0x7DF`。

## 当前实现

- 编排入口：`src/flash/longma_ars1_31_workflow.cpp`；
- 服务序列与模式解析：`src/flash/longma_ars1_31_flow.cpp`；
- APP 模式直接使用所选主/从雷达物理端点；
- FT 模式先通过 `0x714/0x71C` 执行恢复入口，再回到所选 APP 端点；
- 主体包含会话切换、DID 读取、编程条件检查、安全访问 `27 01/27 02`、指纹写入、Driver/APP 擦除下载、依赖检查、复位和默认会话恢复。

## 当前验收边界

从雷达明确处于待验证状态。主雷达的 `pending_validation=false` 也只表示 Profile 未设置阻断标记；若要声称当前构建已通过，仍需对应当前 EXE、Profile、固件和实际台架的完整证据。FT 入口应与主、从目标分别验证。
