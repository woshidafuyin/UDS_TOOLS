# 长安 C857 / B216 当前刷写流程

更新日期：2026-08-12

## 当前结论

C857 与 B216 使用独立 Profile 和资源目录，但复用 ARS1.31 刷写内核。当前实现支持 APP、FT、CAL、APP+CAL 四种模式。

## 当前 Profile

- C857：`profiles/changan_c857.ini`，流程标识 `changan_c857`；
- B216：`profiles/lingyao_b216.ini`，流程标识 `lingyao_b216`；
- 两个 Profile 均设置 `supports_ft_entry=true`、`supports_cal_download=true`；
- 总线为经典 CAN，通道 2，500 kbit/s，填充值 `0x00`。

| 目标 | APP TX/RX | FT TX/RX |
|---|---|---|
| 主雷达 | `0x744/0x74C` | `0x715/0x71D` |
| 从雷达 | `0x760/0x768` | `0x714/0x71C` |

CAL 窗口为 `0xC0180000/0x270`，两个 Profile 均已配置 CAL 文件；CAL 不再是空白占位项。

## 当前四种模式

| 模式 | 当前处理内容 |
|---|---|
| APP | Driver + APP |
| FT | 从 FT 端点进入，再在 APP 端点执行 Driver + APP |
| CAL | Driver + CAL |
| APP+CAL | Driver + APP + CAL |

模式解析和共用服务序列位于 `src/flash/longma_ars1_31_flow.cpp`，C857/B216 的 Workflow 使用各自 Profile、资源和项目标签调用该内核。`src/flash/longma_ars1_31_workflow.cpp` 会在 CAL 模式下检查 Profile 能力及固定 CAL 窗口，避免把不支持 CAL 的项目误路由到 CAL 下载。

## 当前验收边界

Profile 中主、从目标目前均未标记待验证，但这只是配置状态，不等于四种模式均已完成当前版本台架验收。正式结论仍需按“项目 × 主从目标 × 模式”记录本次版本的 Trace、报告、刷后版本和输入文件哈希。缺少本次证据的组合只能写成“已实现”。
