# 吉利 P416 / P417 / P611 当前刷写流程

更新日期：2026-08-26

## 当前结论

当前 `geely_p416` Workflow 供 P416、P417 和 P611 共用，支持 APP→APP 与 PLS/FT→APP 两个入口，按 VBF 处理 SBL、APP、ESS 三类镜像。三个项目的协议流程和 VBF 参数处理完全一致，但分别使用 `resources/geely_p416`、`resources/geely_p417`、`resources/geely_p611`，互不覆盖默认文件。

## 当前配置

配置文件：`profiles/geely_p416.ini`、`profiles/geely_p417.ini`、`profiles/geely_p611.ini`

| 项目 | 当前值 |
|---|---|
| APP 物理端点 | `0x716/0x616` |
| PLS/FT 端点 | `0x701/0x761` |
| 功能地址 | `0x7FF` |
| CAN 通道 | 1 |
| 总线 | CAN FD 通道配置；UDS 帧使用经典 CAN（`uds_fd=false`、`uds_brs=false`） |
| 安全算法 | `geely_p416_builtin` |
| 通用 CAL 能力 | `supports_cal_download=false` |

当前资源由 `P416_SBL_reconstructed.vbf`、`P416_APP_reconstructed.vbf` 和原始 `ess_out.VBF` 组成。ESS原文件的数据块、CRC和签名已逐字节确认与成功BLF一致，因此直接使用，不再生成额外ESS重建文件。

## 当前实现

- APP→APP 直接使用 `0x716/0x616`；
- PLS/FT→APP 先使用 `0x701/0x761`，再进入 APP 物理端点；
- `src/flash/geely_p416_flow.cpp` 负责 P416 专用服务顺序、块下载和校验；
- P416、P417、P611 均保留各自 VBF 的 dataFormatIdentifier；当前默认 ESS VBF 为 `0x00`，三个项目的两个 ESS 数据块均发送 `34 00 44 00 13 C0 00 ...` 和 `34 00 44 00 13 C1 00 ...`；
- VBF 的地址、块布局、调用地址和校验信息必须通过解析结果约束，不能只按文件名判断可刷性。

## 当前验收边界

当前状态是 P416/P417/P611 Profile、共用 Workflow 和三套独立资源目录已接入。P416 有原始成功 BLF 流程依据；P417、P611 在取得对应 ECU 的完整 Trace、报告、刷后版本及输入哈希前，不能写成真实 ECU 已通过，APP 与 FT 两个入口需分别验收。
