# 楚能 ARC331 当前刷写与版本读取流程

更新日期：2026-08-24

## 当前结论

当前 Profile 的注册 Workflow ID 是 `chuneng_arc331`。右后与左后雷达共用一个 `ChunengArc331Workflow`，设备差异只由 Profile 的 `target_N_*` 选择物理端点，不复制第二套刷写流程，也不使用零跑 ARC 的旧 `0x771` 过渡帧。

当前状态是源码、Profile、资源、一键版本读取配置和离线测试已接入。2026-08-24，左后雷达 APP 使用当前 Release 工具和成对 Driver/APP CBF 完成 1 次真实 ECU 完整刷写，`31 01 02 03 -> 7F 31 31` 按项目参考流程 WARN 后继续，Driver/APP 下载、校验、复位与恢复全部 PASS。证据冻结在 `validation/2026-08-24_chuneng_arc331_left_rear_app_pass/`。右后 APP、左右雷达 FT 和其他固件组合仍需分别完成台架验收。

## 当前入口与地址

配置文件：`profiles/chuneng_331_left_rear.ini`

| 入口/目标 | TX | RX | 状态 |
|---|---:|---:|---|
| APP－右后雷达（默认） | `0x72C` | `0x72D` | `pending_validation=true` |
| APP－左后雷达 | `0x72E` | `0x72F` | 2026-08-24 当前 Release + CBF 实刷 PASS；Profile 目标级 `pending_validation=true` 为未验 FT 保留 |
| FT/PLS | `0x701` | `0x761` | 已实现，需与所选目标组合验收 |
| 功能寻址 | `0x7DF` | — | Profile 固定值 |

总线参数为通道 2、500 kbit/s 仲裁速率、2 Mbit/s 数据速率；UDS 填充值为 `0x55`。设备选择决定 APP 物理端点；通用界面允许临时修改 Tx/Rx ID，重新切换设备会恢复对应目标默认值。

## Workflow 路由

| 层级 | 当前实现 |
|---|---|
| Profile ID | `chuneng_331_left_rear` |
| 注册 Workflow ID | `chuneng_arc331` |
| 工厂入口 | `flash_workflow.cpp::create_chuneng_arc331()` |
| 项目 Workflow | `ChunengArc331Workflow` |
| Driver/APP 解析 | `Chuneng331Workflow::run()`（严格双 CBF 或严格双 S-record/ASC） |
| 下载状态机 | `Chuneng331Flow::run()` |
| 目标端点契约 | Profile 当前 `tx_id/rx_id` |
| CBF 路由 | Driver/APP 两份 CBF 分别提取 main、ABT、256 字节 `dev_signature` |
| 公共协议辅助 | `chuneng_331_flow.cpp`、`chuneng_331_protocol.hpp` |

`ChunengArc331Workflow` 只委托楚能专用 Workflow/Flow，不再复用零跑雷达 S19 下载主体。Driver 与 APP 是一个原子输入集合：要么均为 CBF，要么均为 S-record 并各带一份 ASC；混合输入在创建 CAN 通道前拒绝。两种模式都只能使用楚能 `0202 + 256 字节签名` 校验，不能把 1322 字节 LP 证书送入该例程。

## 输入与预检

S19/SREC 模式：

```text
Driver S19/SREC + Driver Verification ASC
APP S19 + APP Verification ASC
```

CBF 模式：

```text
Driver CBF
APP CBF
```

两份 CBF 在访问 CAN 前分别检查容器版本、必需 Header、类型、数据格式、main/ABT 地址与长度、段 CRC16、整体 CRC32、ABT Header、ABT Hash、ABT 对主数据的 SHA-256 映射、256 字节 `dev_signature` 和固定刷写窗口。`FAKE_CN2944_FLASH_DRIVER_RAW_0x4000` 是本项目已确认允许刷写的 Driver 标识，不再按 `FAKE_` 前缀拒绝；其他结构与完整性检查不放宽。离线预检通过不等于 ECU 已接受下载。

## 当前刷写服务

- APP 入口先使用所选目标的物理端点；FT 入口先使用 `0x701/0x761`，随后回到所选 APP 端点；
- APP 前置按 Q/CN A201-2025 附录 C：物理 `10 03` 等待响应、物理 `31 01 02 03` 条件检查、功能抑制响应 `10 83`/`85 82`/`28 83 03` 和物理 `10 02`；
- 下载主体包含安全访问 `27 11/27 12`（16 字节种子/密钥）、Driver 下载与校验、`31 01 03 01` 激活 SBL、激活 SBL 后 `2E F1 84` 写 9 字节指纹（Q/CN A201-2025 5.4.5）、`31 01 FF 00` 擦除、APP 下载与校验、依赖检查和复位；
- 正式刷写和在线探测以 10 ms 周期维持标准 CAN `0x520 00 00 00 00 00 00 00 00` 唤醒；
- `31 01 02 03`、`31 01 02 02`、`31 01 03 01`、`31 01 FF 00`、`31 01 FF 01` 按项目协议检查例程状态；
- Profile 不支持 CAL 下载，本流程只处理 Driver、APP 及对应校验数据；
- 复位恢复、报告和 ASC Trace 只记录本次执行，不能替代 ECU 最终状态确认。

## 一键版本读取

版本读取页跟随刷写作业当前设备、通道和 Tx/Rx ID。Profile 配置 `session=0x01`、`precondition=chuneng_520`，读取期间维持 `0x520` 周期唤醒。

| 请求 | DID | Profile 含义 | 解码器 | 必读 |
|---|---|---|---|---|
| `22 F1 87` | `F187` | ECU零件号 | `ascii_trim` | 是 |
| `22 F1 80` | `F180` | BootLoader版本号 | `ascii_trim` | 是 |
| `22 F1 95` | `F195` | 供应商软件版本号 | `ascii_trim` | 是 |
| `22 F1 89` | `F189` | 整车厂软件版本号 | `ascii_trim` | 是 |
| `22 F1 93` | `F193` | 供应商ECU硬件版本号 | `ascii_trim` | 是 |

五项均成功才报告全部必读项成功。界面显示完整解码值，并在原始通信区域/ASC 中保留完整 UDS 响应。上述含义来自当前 Profile 配置；诊断调查表确认、实际字段长度和真实 ECU 返回仍需随台架证据冻结。

## 当前验收边界

至少分别完成以下组合并保存所用 EXE 哈希、Profile、输入文件哈希、HTML 报告、ASC Trace、版本读取结果和 ECU 身份：

1. 右后雷达 APP；
2. 左后雷达 APP：2026-08-24 当前 Release + 指定 CBF 已完成完整实刷 PASS；刷后版本读取仍未随本次证据冻结；
3. 右后雷达 FT 恢复刷写；
4. 左后雷达 FT 恢复刷写。

构建、8/8 CTest、Fake Bus、CBF 离线重算或既有另一设备的历史 PASS，均不能替代以上组合的真实 ECU 验收。

本次左后 APP PASS 只覆盖证据目录中哈希绑定的 EXE、Profile、Driver/APP CBF 和 SeedKey DLL。它证明当前 `7F 31 31` 容错修改已在真实 ECU 完整流程中生效，不自动证明右后端点、FT 入口或其他输入文件组合。
