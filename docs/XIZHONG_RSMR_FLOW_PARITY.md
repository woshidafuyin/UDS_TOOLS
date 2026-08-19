# 犀重 RSMR（HQ001）CANoe / Qt 流程对齐

## 结论与证据边界

- CANoe 基准工程：`D:\project\10_客户项目\犀重\09-去除F002的XZ_test (2)`
- 权威入口：`CAPL\Flash.can::Download()`，本次成功选择 `RaderID=0`、`Protocol=APP`
- C++ 流程 ID：`xizhong_rsmr`
- Profile：`profiles\xizhong_rsmr.ini`
- Qt 打包资源：`resources\xizhong_rsmr`
- 成功报告：`CAPL\Report\Flash_report0002.xml`
- 成功总线记录：`logging\Logging2026-07-22_11-31-16.blf`

报告记录 Test Module `Flash` 于 2026-07-22 11:31:21 启动，Test Case `Download`
于 11:31:24 开始、11:35:28 结束，最终 verdict 为 `pass`。Test Case 时间戳从
8.238413 s 到 251.953897 s，实际用时约 **243.715 s**，与成功截图中的 243.72 s 一致。
报告明确关联上述 BLF、CDD 和 `Flash.can`，因此这组 XML/BLF 是同一轮可追溯的成功基线。

CANoe 基线之外，2026-07-22 已用 Qt/C++ APP 入口在同一台架完成一次真实刷写并冻结在
`validation/2026-07-22_xizhong_rsmr_qt_pass/`；报告、Qt EXE 和同时间窗口 BLF 均可追溯。
本次 Qt 实刷依赖 CANoe 测量提供网络唤醒/周期环境，仍需后续工作将该依赖独立化。成功 BLF
中没有 `0x701 / 0x761` 报文，所以 FT/PLS 入口也未在本轮成功记录中得到验证；FT 只能按
CAPL 静态逻辑迁移，并保持“待台架验证”。

## 冻结基线与资源哈希

| 基线项 | 文件 | SHA-256 |
| --- | --- | --- |
| CAPL 流程 | `CAPL\Flash.can` | `40C37CD5F08076A4EA57476D5D3B2C22342E4F275803A140A12684302CAE5F96` |
| CDD | `CDD\EP32_V1.7.110.100.cdd` | `5D96FB20D6B7FDD29E77AAAC01CEBC70C6D881FE90E908FA29B6E4C379D7012A` |
| Seed/Key DLL | `DLL\XZ_GenerateKeyEx_RSMR.dll` | `92B9C5D4AB9DE679BEC218272D1C76D368387E64F34DC795005908215D49B2F6` |
| 面板文件配置 | `Download_File.Ini` | `77690962FD6D940D60E36B7D8EF16A920D7ADDFF37B2A091D54819147BAE253C` |
| 成功报告 | `CAPL\Report\Flash_report0002.xml` | `E27E3F4677564A25BE85BD294AEAA8163D8CA6F1BB00235536842DCFE8E37415` |
| 成功 Trace | `logging\Logging2026-07-22_11-31-16.blf` | `687B2CFBC2276F5B66592A2908F3C7BFE0670DBA0D6A01F819B7F65BF3829172` |

Qt 目录中的三份 S19 和 DLL 与成功工程使用的源文件逐字节一致：

| 用途 | 文件 | 长度 | SHA-256 |
| --- | --- | ---: | --- |
| Flash Driver | `ARC2.33C1_HQ001A_FlashDrv.s19` | 2,550 B | `85BBFEC1F6662CC81CA6346D94ABCDCF6EADB47FA6802117E37A3DAF46B64B27` |
| APP | `RSMR_AA_APP1_V09.13.00.s19` | 7,667,766 B | `01CD25327A872AFA53BC997B61910B3ECE8C6619DB54D6B1C3B80B5241E0E44B` |
| APP Hash | `RSMR_AA_APP1_V09.13.00_Hash.s19` | 128 B | `842048B1DEA55120E2CC55F7D7FEB9DFB2ECEF81203CF1F477D34505412E9E94` |
| Seed/Key DLL | `XZ_GenerateKeyEx_RSMR.dll` | 856,576 B | `92B9C5D4AB9DE679BEC218272D1C76D368387E64F34DC795005908215D49B2F6` |

CDD 是成功基线和诊断参数的来源，但 C++ 运行时不解析 CDD；它应作为来源证据保存，不能替代
Profile、流程代码和台架 Trace。

## 通信与 CAN FD 混合适配

| 项目 | 成功基线 |
| --- | --- |
| 物理请求 / 响应 | `0x18DAB7F1 / 0x18DAF1B7`，29-bit 扩展 ID |
| 功能请求 | `0x18DBFFF1`，29-bit 扩展 ID |
| FT 请求 / 响应 | `0x701 / 0x761`，11-bit Classic CAN；本次未使用 |
| 现场通道 | CAN 1；Qt `channel` 仍须按实际 Vector 接线确认 |
| 位率 | nominal 500 kbit/s、data 2 Mbit/s |
| 发送 padding | APP物理请求 `0xCC`；功能寻址与FT raw分支 `0x00` |
| CAN FD 参数 | DLC `FD <= 8`，BRS 开启 |
| 接收流控参数 | `BS=0`、`STmin=0 ms`、FC delay `10 ms`、Max length `4095` |
| 诊断超时 | S3 client/server `4000/5000 ms`；普通服务 P2 client `100 ms`；`0x36 TransferData` 首响应等待 `2000 ms`（对齐原始 CAPL）；P2* client `5000 ms`；P2/P2* server `50/4900 ms` |
| 其他帧类型 | CAN 2.0 与 CAN FD 混合接收选择 `adapt` |

成功 BLF 证明该 ECU 使用的是非常规混合发送方式，不能把一次 UDS 请求的所有 ISO-TP 帧都
统一标成 CAN FD：

- Tester 发出的 UDS Single Frame 和 First Frame 是 **CAN FD + BRS**，DLC 8；
- Tester 发出的 Consecutive Frame 是 **Classic CAN**；
- Tester 给 ECU 多帧响应发送的 Flow Control 也是 **Classic CAN**；
- ECU 的诊断响应和 ECU 发出的 Flow Control 均为 **Classic CAN**。

因此 Qt ISO-TP 必须分别控制 SF/FF、CF 和 FC 的 FD/BRS 标志。只配置一个全局 `uds_fd=true`
会把 CF/FC 也发成 FD，和成功基线不一致。

CAPL 的 4 s 定时器以 Classic CAN 原始帧发送功能寻址 `02 3E 80 00 00 00 00 00`；CDD 自动
TesterPresent 同时产生了另一条 CAN FD+BRS 的 `3E 80`，所以成功 BLF 中可以看到两条保活流。
冻结 BLF 中两种格式各有 61 帧，周期中位数分别约 3.999991 s 和 4.001400 s。Qt 没有 CDD
自动发送器，因此由同一个后台任务显式复现 **Classic CAN + CAN FD/BRS 双路、4 s 周期**保活；
两条流之间的相位差属于当次 CANoe 调度结果，不固化为协议常量。后台任务在擦除、校验或连续
`7F xx 78` 等长操作期间持续运行，任一发送错误会终止刷写。

CANoe 的 CAN IG 在 Test Case 开始前已经持续发送 `NM_ICG`。独立工具没有此前置运行期，
因此启动 200 ms NM 流后先等待 1000 ms，再发送首个只读 `22 F187`；该等待只复现环境
预热条件，不改变 UDS 服务顺序。2026-07-22 的适配层首次台架尝试在“单帧 NM 后立即
F187”时超时，而相同硬件上的在线探测在维持 NM 1 s 后立即收到 `50 01`，由此增加该前置
等待。

## APP 成功流程顺序

所有物理服务均接受任意次数的 `7F <SID> 78`，直到收到最终正响应；不能把 Response Pending
当成失败，也不能只收到 `0x78` 就判定步骤成功。

1. 物理读取 `22 F187`、`22 F150`、`22 F189`。F150 必须确认 `RSMR_AA`；F189
   正响应同样校验身份，但 `7F 22 31` 记录为 `WARN` 后继续，以匹配 CAPL DID 调用不抛异常、
   testcase 继续执行的控制流。F189 的其他 NRC、超时或错误身份仍中止；
2. 物理 `10 03`，要求最终响应 `50 03`；
3. 功能 `85 02` 关闭 DTC 设置，要求 `C5 02`；
4. 功能 `28 03 01` 关闭通信，要求 `68 03`；
5. 物理 `10 02` 进入编程会话，要求最终响应 `50 02`；
6. 安全访问 `27 11 / 27 12`：Seed 和 Key 均为 4 字节，调用 x86
   `XZ_GenerateKeyEx_RSMR.dll::GenerateKeyEx`，等级 `0x11`、Variant 为空；
7. `2E F184` 写编程指纹。成功总线 payload 为 16 字节 `00`，随后是当前日期 BCD
   `20 26 07 22`，要求 `6E F1 84`；
8. Driver 执行 `34 / 36 / 37`，再发送
   `31 01 02 02 + 32-byte Driver Hash`，要求完整状态 `71 01 02 02 04`；
9. APP 擦除：`31 01 FF 00 44 00 0C 00 00 00 30 00 00`，要求
   `71 01 FF 00 04`；
10. APP 执行 `34 / 36 / 37`，再发送
    `31 01 02 02 + 32-byte APP Hash`，要求完整状态 `71 01 02 02 04`；
11. 编程依赖检查 `31 01 FF 01`，要求 `71 01 FF 01 04`；
12. 物理 `11 01` 硬复位，等待约 1 s，再发物理 `10 03`；
13. 功能 `28 00 01`、`85 01`、`10 01` 恢复通信、DTC 和默认会话，成功 BLF 中分别收到
    `68 00`、`C5 01`、`50 01`。

这里的三个恢复请求不是 suppress-positive-response 形式；Qt 侧必须等待并校验最终正响应。

### CAPL—C++—测试映射

| CANoe/CAPL 基线 | C++ 实现 | 自动验证 |
| --- | --- | --- |
| `Download()` APP 分支入口 | `XizhongRsmrWorkflow::run` | `xizhong_rsmr_profile_and_resources` |
| `Sever22(F187/F150/F189)` | `XizhongRsmrFlow::run_app` 身份检查 | `xizhong_rsmr_protocol_baseline` |
| `Sever10/85/28` 会话、DTC、通信控制 | `XizhongRsmrFlow` 对应请求常量 | 请求字节与物理/功能 padding 断言 |
| `Sever27(11/12)` | x86 SeedKey broker 调用 | `FDBAAF18 -> 29984258` 打包 KAT |
| `DriverDownload/APPDownload` 的 `34/36/37` | `XizhongRsmrFlow::download_region` | Driver/APP 窗口、块长及 ISO-TP 混合帧断言 |
| `Sever31(0202/FF00/FF01)` | Driver/APP 校验、擦除、依赖检查 | 服务字节、固定等待和成功状态断言 |
| `Sever11` 后恢复 `10/28/85/10` | 复位与恢复序列 | 恢复请求常量与台架报告检查 |
| CAN IG `NM_ICG` 200 ms + 双 `3E80` 4 s | 后台 NM/TesterPresent 发送器 | ID、周期、Classic/FD+BRS 属性断言 |

硬件适配重构只替换 `ICanBus` 的创建来源，不修改上述流程代码中的服务
顺序、响应判定、分块、等待或恢复策略。

CAPL 中会影响总线顺序的显式等待也作为流程契约复现：APP `10 02` 正响应后 200 ms；Driver
和 APP 的 `34` 正响应到首个 `36` 各 50 ms；Driver `37` 到 Driver 校验无显式等待；Driver
校验到擦除 50 ms；擦除到 APP `34` 50 ms；APP `37` 到 APP 校验 1000 ms；APP 校验到
`31 FF01` 50 ms；`31 FF01` 到 `11 01` 50 ms；复位正响应到 `10 03` 1000 ms。`36` 数据块
之间没有额外固定等待。所有等待均检查用户中止和后台保活发送状态。

## 下载窗口、分块与校验数据

| 区域 | 连续窗口 | `34` 响应 / 分块 | `36` 数量 | 校验数据 |
| --- | --- | --- | ---: | --- |
| Driver | `0x00080000 / 0x00000400` | `74 20 08 02`；一次传 1024 B | 1 | CAPL 固定 32 B Driver Hash |
| APP | `0x000C0000 / 0x00300000` | `74 20 08 02`；每块传 2048 B | 1536 | Hash S19 中地址 `0x00..0x1F` 的 32 B |

`34` 请求统一为 `34 00 44 + address[4] + length[4]`。ECU 返回的 MaxNumberOfBlockLength
为 `0x0802`，包含 `36` SID 和 BlockSequenceCounter，因此数据区最大为 2048 字节。
BlockSequenceCounter 从 `01` 开始，`FF` 后回卷为 `00`。成功 BLF 共包含 1537 个
TransferData 请求：Driver 1 个、APP 1536 个。

Driver 校验数据为：

`6A AB 53 98 12 CF 22 B0 35 8B 84 E2 09 62 45 89 D2 1D DF D1 93 5F 38 FD 89 69 E8 2A FB B9 50 C6`

APP Hash S19 的 32 字节为：

`AA 83 62 17 32 14 7D 84 08 00 D3 52 08 88 4A A2 30 D1 D6 AA 46 36 37 E2 86 4E 2D A1 64 4D 42 84`

Driver 与 APP S19 都覆盖上述完整连续窗口；不能按文件中“实际存在的若干段”缩短下载长度，
也不能用其他项目的 Driver、APP、Hash 或 Seed/Key DLL 替换。

## FT/PLS 入口边界

当 CAPL 面板选择 `Protocol=FT(0)` 时，`Download()` 先按 raw Classic CAN 发送：

1. `0x701: 02 10 03 00 00 00 00 00`，等待 200 ms；
2. `0x701: 02 10 02 00 00 00 00 00`，等待 2000 ms；
3. 随后继续执行与 APP 分支相同的 29-bit 物理安全访问、下载、校验和恢复流程。

该 CAPL 分支本身没有等待或校验 `0x761` 的正响应。2026-07-22 成功 BLF 中完全没有
`0x701 / 0x761`，因此不能用本次 Passed 结果证明 FT 入口、FT ID、等待时间或故障恢复有效。
Qt 可以保留 `ft` 入口用于后续台架验证，但默认成功基线应明确选择 `app`，不能依赖未验证的
自动探测来代表这次成功流程。

## Qt 验收状态与通过条件

当前迁移的离线验收应至少覆盖：资源哈希、S19 连续窗口、BCD 指纹、32 字节校验数据、
`0x0802` 分块、BSC 回卷、`0x78` 处理、混合 FD/Classic ISO-TP 标志、双路 4 s 保活，以及
三个恢复服务的正响应校验。Release 构建成功和离线单元测试通过只说明代码可构建、编码可复现，
不等于 ECU 已完成刷写。

Qt 工作流在访问 Vector 总线前会计算 Driver/APP 数据窗口的 SHA-256：Driver 必须匹配
`Flash.can` 固定的 32 字节校验值，APP 必须匹配所选 Hash S19 的 32 字节内容；任一不一致
都会在关闭 DTC/通信之前终止。

若用户中止或异常发生在最终 `28 00 01`、`85 01`、`10 01` 全部获得正响应之前，Qt 报告会把
恢复状态标为 `WARN`。此时不能声称“安全停止”，也不能在未知擦除/传输阶段盲目自动发送恢复
命令；应保持供电，根据报告最后成功步骤执行项目恢复流程。

本次 Qt/C++ APP 实刷已经满足以下验收条件，状态可标记为“C++/Qt APP 实刷通过”：

- 实际 EXE、Profile、四份打包资源及各自 SHA-256；
- Vector 设备、物理 Channel、nominal/data 位率和 ECU 供电条件；
- 完整原始总线 Trace，能够核对 1537 个 `36`、所有最终例程状态及复位后的恢复服务；
- 工具日志和最终 PASS 报告，且报告与 Trace 的时间窗口一一对应。

对应证据已冻结在 `validation/2026-07-22_xizhong_rsmr_qt_pass/`。由于该次实刷是在
CANoe 测量保持运行的网络唤醒环境下完成，完全独立 Qt/Vector 环境仍需另做一次验证。

FT/PLS 入口必须另做一次专门的恢复场景验证，不能由 APP Passed 结果自动继承。

## 2026-07-29 TOSUN 独立实刷与根因

TOSUN / libTSCAN 独立 CH1 台架的最初失败稳定发生在 Driver 第一个 `36`：前置唤醒、
身份确认、编程会话、安全访问、`2E F184` 和 Driver `34` 均通过，但工具等待
`76 01` 超时。固件、Hash、CDD 和 SeedKey DLL 的 SHA-256 均与 CANoe 成功基线一致，
因此问题不在刷写资源或 CAPL 服务顺序。

根因位于 TOSUN FIFO 接收参数。libTSCAN 的 `tsfifo_receive_canfd_msgs(..., ARxTx)`
规定 `ARxTx=0` 只返回 RX，`ARxTx>0` 同时返回 TX/RX；适配层原先把名为
`kReceiveOnly` 的值错误设成 `1`。Driver 的一次 ISO-TP 请求产生 146 个 TX 连续帧，
接收循环会先逐个取出并丢弃这些 TX 回显，ECU 正响应被压在 FIFO 后方，超过 100 ms
P2 后被误报为超时。短 UDS 请求的 TX 回显很少，所以在线探测和所有前置服务仍可成功。

修正后：

- TOSUN FIFO 使用 `ARxTx=0`，不再让 TX 回显阻塞 ECU 响应；
- `BS=0 / STmin=0` 的连续帧继续交给 `tscan_transmit_can_sequence`，由控制器按物理
  500 kbit/s 总线速率串行化，不增加 CAPL 中不存在的固定块间等待；
- 2026-07-29 22:42:11 至 22:46:20 完成一次独立 TOSUN APP 实刷：Driver、
  APP 1536 块、APP Hash、依赖检查、复位和 `28 00 01 / 85 01 / 10 01`
  恢复全部通过。

本轮证据：

- 报告：`dist/logs/report_20260729_224620_000.html`
- 原始 ASC：`dist/logs/trace_20260729_224211_xizhong_rsmr_default_app.asc`
- 执行日志：`dist/logs/execution_20260729_224157_869.log`

这次结果可标记为“TOSUN 独立 APP 实刷通过”；仍不能外推为 FT/PLS 入口已验收。
