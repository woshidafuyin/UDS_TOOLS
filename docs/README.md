# 文档目录与当前状态

更新日期：2026-08-19

本目录只描述当前工程状态。证据口径统一为：源码或公盘资料存在、功能已实现、候选包已包含、离线测试通过、真实 ECU 台架通过是五个不同层级，不能互相替代。

## 架构与维护

| 文档 | 用途 | 状态 |
|---|---|---|
| `ARCHITECTURE.md` | 当前模块边界和主流程 | 当前基线 |
| `VERSION_READ_CONFIGURATION.md` | 一键版本读取 Profile 格式、解码器、项目覆盖与验证边界 | 当前配置基线 |

## 项目流程

| 文档 | 项目 | 状态 |
|---|---|---|
| `CHERY_ARS1_33_FLOW_PARITY.md` | 奇瑞 ARS1.33 | 当前流程与验收边界 |
| `CHERY_KP31_FLOW_PARITY.md` | 奇瑞 KP31 | 当前流程与验收边界 |
| `CHUNENG_331_FLOW_PARITY.md` | 楚能 ARC331 | 已按当前双目标 Profile 更新，待台架验收 |
| `C857_B216_FLOW_PARITY.md` | 长安 C857 / B216 | 已按当前四模式能力更新 |
| `LONGMA_ARS1_31_FLOW_PARITY.md` | 长马 J90K ARS1.31 | 当前 APP/FT 流程，从雷达待验收 |
| `LP_ARC_FLOW_PARITY.md` | 零跑 LP-ARC | 当前 APP/FT 流程 |
| `LP_ARF_FLOW_PARITY.md` | 零跑 LP-ARF | 当前 631 统一入口流程 |
| `GEELY_P416_FLOW_PARITY.md` | 吉利 P416 | 当前专用 VBF 流程，待真实 ECU 验收 |
| `XIZHONG_RSMR_FLOW_PARITY.md` | 犀重 RSMR | 当前流程与验收边界 |

## 项目接入

| 文档 | 用途 | 状态 |
|---|---|---|
| `SHIDAIXINAN_ARF232_PROJECT_INTEGRATION.md` | 时代新安相关项目接入说明 | 当前接入说明 |

## 硬件与发布说明

| 文档 | 用途 | 状态 |
|---|---|---|
| `KVASER_BENCH_CHECKLIST.md` | Kvaser 台架检查清单 | 发布包使用 |

工程当前实现以根目录 `README.md` 为入口；本文件负责指向各模块和项目的详细文档。
