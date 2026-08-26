# 零跑 LP-ARC 当前刷写流程

更新日期：2026-08-12

## 当前结论

当前 `lp_arc` Workflow 统一承载 ARC 四设备，支持 APP→APP 与 PLS/FT→APP 两个入口，只下载 Driver + APP，不支持 CAL。四个设备共用预置 Driver、APP、校验文件和安全 DLL，设备选择只切换 APP 物理诊断端点。

## 当前配置

配置文件：`profiles/lp_arc.ini`

| 项目 | 当前值 |
|---|---|
| APP 物理端点 | `0x772/0x77A` |
| PLS/FT 端点 | `0x701/0x761` |
| 功能地址 | `0x7DF` |
| CAN 通道 | 1 |
| 总线 | CAN FD 通道配置；UDS 帧使用经典 CAN（`uds_fd=false`、`uds_brs=false`） |
| 安全等级/算法 | `0x11` / `lingpao` |
| CAL | `supports_cal_download=false` |

## 当前实现

- 运行编排：`src/flash/lp_arc_workflow.cpp`；
- 流程接口：`src/flash/lp_arc_flow.hpp` 与当前实现 `src/flash/lp_arc_flow_configurable.cpp`；
- APP→APP 直接使用 APP 物理端点；
- PLS/FT→APP 先使用恢复端点，然后切换到 APP 端点执行共同下载主体；
- Driver 窗口为 `0x00000000/0x00004000`，APP 窗口为 `0x000C0000/0x00180000`；
- APP 与验签 ASC 可以成套替换，但替换后必须重新校验地址窗口和配套关系。

## 当前验收边界

当前文档只确认最新 Profile 与源码路由。APP 和 FT 两个入口都应使用当前发布 EXE、实际目标 ECU 及本次选择的 APP/ASC 重新留存 Trace、报告、刷后版本和文件哈希，之后才能写成已验收。
