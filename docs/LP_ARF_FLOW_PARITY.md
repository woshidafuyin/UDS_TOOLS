# 零跑 LP-ARF 当前刷写流程

更新日期：2026-08-12

## 当前结论

当前 `lp_arf` Workflow 是 631 系列统一 APP/FT 入口，APP 与配套 ASC 可替换；不支持 CAL。

## 当前配置

配置文件：`profiles/lp_arf.ini`

| 项目 | 当前值 |
|---|---|
| APP 物理端点 | `0x751/0x759` |
| PLS/FT 端点 | `0x701/0x761` |
| 功能地址 | `0x7DF` |
| CAN 通道 | 1 |
| 总线 | CAN FD 通道配置；UDS 帧使用经典 CAN（`uds_fd=false`、`uds_brs=false`） |
| 安全等级/算法 | `0x11` / `lingpao` |
| APP 窗口 | `0x000C0000/0x00180000` |
| CAL | `supports_cal_download=false` |

## 当前实现

- 编排入口：`src/flash/lp_arf_workflow.cpp`；
- 流程实现：`src/flash/lp_arf_flow.cpp`；
- APP 模式直接进入 APP 端点；FT 模式从 `0x701/0x761` 恢复后切换到 APP 端点；
- 当前 Profile 默认 APP 与验证 ASC 必须配套使用，替换其中一个时必须同步确认另一个；
- Driver 文件当前为空，由该项目流程和资源约束决定，不能套用其他项目 Driver。

## 当前验收边界

“统一入口”只表示代码复用，不代表所有 631 设备身份、APP/ASC 组合和 FT 恢复场景自动等价。每一种实际设备与文件组合都需用当前 EXE 重新完成台架验证并冻结证据。
