# 吉利 P416 当前刷写流程

更新日期：2026-08-12

## 当前结论

当前 `geely_p416` Workflow 支持 APP→APP 与 PLS/FT→APP 两个入口，按 VBF 处理 SBL、APP、ESS 三类镜像。Profile 未开启通用 CAL 下载；ESS 虽通过 `cal_file` 字段承载，但属于 P416 专用流程对象，不能等同于通用 CAL 模式。

## 当前配置

配置文件：`profiles/geely_p416.ini`

| 项目 | 当前值 |
|---|---|
| APP 物理端点 | `0x716/0x616` |
| PLS/FT 端点 | `0x701/0x761` |
| 功能地址 | `0x7FF` |
| CAN 通道 | 1 |
| 总线 | CAN FD 通道配置；UDS 帧使用经典 CAN（`uds_fd=false`、`uds_brs=false`） |
| 安全算法 | `geely_p416_builtin` |
| 通用 CAL 能力 | `supports_cal_download=false` |

当前资源由 `P416_SBL_reconstructed.vbf`、`P416_APP_reconstructed.vbf` 和 `P416_ESS_reconstructed.vbf` 组成，运行时由 `src/flash/geely_p416_workflow.cpp` 解析并交给 P416 专用流程。

## 当前实现

- APP→APP 直接使用 `0x716/0x616`；
- PLS/FT→APP 先使用 `0x701/0x761`，再进入 APP 物理端点；
- `src/flash/geely_p416_flow.cpp` 负责 P416 专用服务顺序、块下载和校验；
- VBF 的地址、块布局、调用地址和校验信息必须通过解析结果约束，不能只按文件名判断可刷性。

## 当前验收边界

当前状态是 Profile、Workflow 和重建 VBF 资源已接入。未取得当前版本在对应 P416 ECU 上的完整 Trace、报告、刷后版本及输入哈希前，不能写成真实 ECU 已通过；APP 与 FT 两个入口需分别验收。
