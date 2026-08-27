# UDS 通用刷写工具

更新日期：2026-08-26
当前正式程序：`CH_FLASH_tools.exe`

## 1. 文档定位

本文是当前交付版本的完整功能说明，描述程序、Profile、Workflow、资源、界面、日志和报告已经实现的能力。相对公共盘上一正式版本的变化单独记录在压缩包内 `CHANGE_LIST.txt`，README 不再混入历史版本对比。

本工具是 Profile 驱动的 C++/Qt UDS 通用刷写工具。项目差异集中在 Profile、项目 Workflow 和资源中；CAN、ISO-TP、UDS、操作互斥、监听、日志和报告由通用模块复用。

## 2. 当前交付组成

- 21 个可执行项目 Profile；
- 18 个已注册 Workflow ID；
- 4 类 CAN 硬件后端：Vector XL、ZLG/ZCAN、TOSUN/TSMaster、Kvaser；
- 4 个用户功能页：刷写作业、版本读取、诊断报文、总线监听；
- 统一主程序 `CH_FLASH_tools.exe`；
- Profile、项目运行资源、硬件运行驱动和用户说明文档；
- 当前交付目录为 `dist`，运行产生的 `logs` 和 `Configuration` 不预置在干净发布包中。

## 3. 软件分层

| 模块 | 职责 |
| --- | --- |
| `src/ui/qt` | 主窗口、刷写作业、版本读取、诊断报文、总线监听和交互状态 |
| `src/app` | 在线探测、刷写调度、版本读取、单次诊断请求、操作互斥、审计和报告 |
| `src/core` | Profile、S-record、VBF、ASC/HEX、CBF、Trace 和公共数据结构 |
| `src/transport` | ISO-TP、UDS 请求响应、超时和 NRC 处理 |
| `src/drivers/can` | 四类 CAN 后端、设备枚举、通道配置和共享通道 |
| `src/flash` | 项目 Workflow、刷写状态机、文件约束和项目协议契约 |
| `profiles` | 项目、目标、诊断端点、通信参数、文件和能力配置 |
| `resources` | Driver、APP、CAL、CDD、SeedKey DLL、签名及参考资源 |
| `tests` | 核心、适配器、应用状态、探测、Fake ECU 和 Qt UI 离线测试 |

## 4. 刷写作业

### 4.1 项目和目标配置

- 按厂商、项目和设备选择 Profile 与 Target；
- 从 Profile 加载 CAN 后端、通道、仲裁波特率、数据波特率、CAN/CAN FD、Tx ID、Rx ID、功能 ID、扩展寻址、Padding、默认文件和能力开关；
- 项目可声明 APP、FT、CAL、APP+CAL、自动检测等入口，界面只显示该 Profile 支持的模式；
- 所有项目均允许按台架需要修改 Tx/Rx ID，修改值会贯穿在线探测、刷写、版本读取和诊断请求；切换项目或目标时加载对应默认端点，双击 Tx ID 或 Rx ID 标签可分别恢复默认值；
- `lock_diagnostic_ids` 仅作为历史 Profile 配置元数据保留，不再阻止通用界面覆盖诊断端点；
- 各 CAN 后端分别保存和恢复自己的通道选择；切换后端时，刷写页、版本读取页和监听页同步更新。

### 4.2 文件输入和预检

- 支持浏览和选择 Driver、Driver 校验、APP、APP 校验、CAL、SeedKey 等项目声明的输入；
- 支持 S19/SREC、ASC/HEX 文本、VBF 和 CBF 1.0；
- 在访问 CAN 前检查必需文件、格式、大小、地址窗口、数据段、Hash、签名、CRC 和项目专用约束；
- 预检失败时不开始 UDS 下载；
- 文件解析成功只表示能够提取数据，仍必须通过对应 Workflow 的完整项目约束。

### 4.3 能否刷写与正式刷写

- “能否刷写”使用当前 Profile、Target、入口、CAN 后端、通道和诊断端点执行项目定义的在线条件检查；
- 在线探测只执行该项目规定的会话、唤醒或条件例程，不进入擦除和数据下载；
- 正式刷写按项目 Workflow 执行会话切换、安全访问、擦除、RequestDownload、TransferData、TransferExit、完整性检查、复位和恢复；
- 支持设置刷写次数；每次执行创建新的 Workflow，某次失败后停止余下重复次数；
- 在线探测、版本读取和刷写互斥，不允许并发占用同一硬件；
- 执行期间锁定可能改变目标和通信上下文的控件。

### 4.4 停止与风险边界

- 刷写和版本读取支持停止请求；按钮会锁定并提示等待当前 UDS 请求结束及报告生成；
- 停止是协作式取消，不等于 ECU 已恢复到应用态，也不是紧急断电；
- 在擦除或编程阶段停止可能使 ECU 留在 Boot/SBL 或不完整状态。停止后应保持供电，查看最终报告并按项目恢复流程处理；
- 工具不会仅因界面显示“已中止”就宣称 ECU 状态安全。

## 5. 执行日志、审计和报告

- 运行日志显示时间、TX/RX 方向、UDS 数据、步骤、进度、警告、错误和最终结果；
- 按 `End` 跳到日志末尾并进入持续尾随，新日志到达后继续自动下拉；
- 按 `Home` 或向上滚动阅读历史时暂停尾随；再次按 `End` 或滚到底部后恢复；
- 每次探测和刷写记录厂商、项目、设备显示名、Profile ID、Target ID、Flow ID、入口模式和重复次数；
- 记录 CAN 硬件后端、通道、Tx/Rx ID、仲裁/数据波特率、CAN FD、Padding 等通信配置；
- 记录 Driver、校验、APP、CAL、SeedKey 等文件路径、存在性和大小；
- 记录最近一次“能否刷写”的结果、完成时间及配置指纹；探测后改变配置会标记 `STALE_CONFIG`，不会把旧结果当成当前配置证明；
- 每次执行生成 HTML 报告，并提供“打开最新报告”；
- 报告和日志用于追溯，但不替代 ECU 状态确认和台架验收。

## 6. 一键版本读取

- 版本读取页自动跟随刷写页的厂商、项目、设备、后端、通道和当前 Tx/Rx ID；
- 21 个可执行 Profile 均配置 `[version_check]`，当前合计 142 个读取项；
- 每项由 Profile 配置请求、正响应前缀、解码器、期望值和必读属性，不在界面中按项目复制 DID 逻辑；
- 读取前显示 DID、请求、含义和必读属性，读取后显示成功/错误、ASCII 或解析值及原始 UDS 通信；
- 支持 ASCII、十六进制、犀重结构化版本、计数 ASCII、BCD+ASCII 零件号和计数零件号列表解码；
- 必读项全部成功才判定整体成功；选读项失败单独显示；
- 支持停止、ASC 通信记录和 HTML 报告；
- 项目前置条件由 Profile 指定：例如楚能维持 `0x520`，犀重维持项目 NM，相关 ARS1.31 项目维持 `0x400`；
- Profile 中存在 DID 仅证明工具已配置，不等于所有 ECU 均已实车返回并通过验收。

当前版本读取数量：

| 项目 | 读取项 |
| --- | ---: |
| 楚能 ARC331 | 5 |
| 奇瑞 ARS1.33、KP31、E0Y、T22、T1EJ | 各 7 |
| 长安 C857、B216 | 各 3 |
| 长马 J90K / ARS1.31 | 7 |
| 犀重 RSMR、LSMR | 各 3 |
| 时代新安 HJZJ FMR | 6 |
| 时代新安 天王星、木星2代、庆铃 FMR | 各 9 |
| 零跑 ARC | 10 |
| 零跑 ARF（A12/B11 ARF2.31、ARF6.31统一入口） | 8 |
| 吉利 P416 | 13 |
| 吉利 P611 | 13（复用 P416 读取计划） |
| 吉利 P417 | 13（复用 P416 读取计划） |
| 北汽 N61AB | 9（5 个必读、4 个可选） |
| 北汽 BQB41 / B41V | 10（5 个必读、5 个可选） |

## 7. 单次诊断报文

- “诊断报文”页自动跟随刷写作业当前的厂商、项目、目标、CAN 后端、通道、仲裁/数据波特率、CAN/CAN FD、物理 Tx/Rx ID、功能 ID、Padding 和 ISO-TP 参数；不维护第二套独立硬件配置；
- 支持输入十六进制 UDS 数据并通过 ISO-TP 单次发送，展示完整 TX、RX、耗时、错误详情和 NRC；超时可在 100～30000 ms 范围内设置；
- 支持物理寻址，以及“功能请求 ID＋当前 ECU 物理响应 ID”的定向功能寻址；
- 每次请求生成独立 ASC 原始通信 Trace，并同步写入运行日志；
- 诊断请求与探测、刷写、版本读取和电源操作共用全局操作门禁，不能并发抢占同一 ECU 响应；被动监听继续通过共享 CAN Provider 观察通信；
- `10/11/27/28/2E/31/34/36/37/3D/85` 等可能改变 ECU 状态、存储或刷写状态的服务发送前必须二次确认；
- 该页面只执行用户明确输入的一条请求，不自动补充项目唤醒、会话、安全访问、TesterPresent、恢复或完整刷写 Workflow；不能用它替代项目恢复流程；
- 当前实现不提供任意原始 CAN 帧、周期发送、脚本批量发送或安全算法自动解锁，避免绕过项目 Workflow 和审计边界。

## 8. 被动总线监听

### 8.1 自动跟随和共享通道

- 工具启动后自动监听刷写页当前 CAN 后端和通道，不需要手工点击开始；
- 监听上下文完整跟随后端、物理通道、仲裁/数据波特率和 CAN/CAN FD；
- 切换项目、目标、后端或通道时释放旧监听并在新上下文自动恢复；
- 监听页明确显示当前后端、通道和速率；探测、读取或刷写前检查监听上下文与当前配置一致；
- 总线监听只被动接收，不主动发送 CAN/UDS 报文；
- 相同硬件、物理通道和速率由共享 CAN Provider 复用，监听、探测、读取和刷写是相互隔离的逻辑客户端；
- ZLG 明确报告单帧发送 0 帧时，底层共享通道会关闭重建并安全重试一次；不确定的多帧/部分发送不会整批自动重放，以避免重复 UDS 操作。

### 8.2 实时显示和过滤

- 表格按批次实时刷新并显示时间、方向、ID、帧类型、长度、数据和诊断提示；
- 默认“仅显示诊断 ID”，覆盖当前物理 Tx/Rx、功能 ID 和项目声明的 FT 端点；
- 手工 ID 过滤支持精确值、范围、半字节掩码和排除项，例如 `772,7DF`、`700-7FF`、`18DAxxxx`、`!520`；
- 支持中文/英文分隔符，并对非法条件标红；修正前继续使用上一有效条件；
- 提供当前项目诊断 ID、功能寻址、物理寻址和周期帧快捷过滤；
- 可叠加数据、TX/RX、标准/扩展帧、CAN/CAN FD 和 BRS 过滤；
- 显示总接收、当前显示和内存淘汰帧数；UI 仅保留最近 10,000 帧，避免长期监听无限占用内存；
- 最终 NRC 和项目已知 RoutineControl 结果会按规则提示；`7F xx 78` 作为 ResponsePending 保留但不误判为最终失败。

### 8.3 完整 Trace

- 所有原始帧从监听开始持续写入 `logs/bus_monitor/*.asc.partial`，不受 UI 过滤和 10,000 帧上限影响；探测、版本读取、诊断报文和刷写 Trace 分别写入 `logs/traces/probe`、`version`、`diagnostic`、`flash`；
- 正常停止后封口为完整 `.asc`；异常退出遗留的 `.partial` 会在下次启动时恢复；
- “清空列表”只清空当前表格，不删除完整 Trace；
- “导出 ASC”读取磁盘完整证据源，而不是只导出表格中的可见帧。

## 9. CAN 硬件后端

- Vector XL：加载随包 Vector XL 运行库并按选定通道工作；
- ZLG/ZCAN：使用 ZCAN API，支持 CAN/CAN FD 和零发送恢复分类；
- TOSUN/TSMaster：使用 TSCAN 后端；
- Kvaser：使用 CANlib 后端；
- 四类后端共享统一 `ICanBus`/Provider 边界，项目 Workflow 不直接依赖厂商 API。

## 10. 当前项目与模式

| 项目 | Profile / Workflow | 当前公开模式或特性 |
| --- | --- | --- |
| 楚能 ARC331 | `chuneng_331_left_rear.ini` / `chuneng_arc331` | 左/右后雷达；APP、FT；CBF 或成对 S19/ASC |
| 奇瑞 ARS1.33 | `chery_ars1_33.ini` / `chery_ars1_33` | APP、CAL、APP+CAL |
| 奇瑞 KP31 | `chery_kp31.ini` / `chery_kp31` | APP、CAL、APP+CAL |
| 奇瑞 E0Y、T22、T1EJ | 各自 Profile / Workflow | 复用项目内核并保留独立端点和资源 |
| 长安 C857 | `changan_c857.ini` / `changan_c857` | 主/从目标；APP、FT、CAL、APP+CAL |
| 长安 B216 | `lingyao_b216.ini` / `lingyao_b216` | 主/从目标；Profile 声明模式 |
| 长马 J90K / ARS1.31 | `longma_ars1_31.ini` / `longma_ars1_31` | APP、FT |
| 犀重 RSMR | `xizhong_rsmr.ini` / `xizhong_rsmr` | APP、FT；项目 NM 和 ISO-TP 规则 |
| 犀重 LSMR | `xizhong_lsmr.ini` / `xizhong_lsmr` | APP 待验证入口；独立扩展 ID、NM 和 SeedKey；Driver/APP/Hash 必须从同一 LSMR 发布包手动选择，源 CANoe 的 LSMR 下载分支为空 |
| 时代新安 HJZJ FMR | `shidaixinan_hjzj_fmr.ini` / `shidaixinan_hjzj_fmr` | APP、FT |
| 时代新安 天王星、木星2代、庆铃 FMR | 独立 Profile / 复用 HJZJ Workflow | 独立端点和资源 |
| 零跑 ARC | `lp_arc.ini` / `lp_arc` | 四设备；APP、FT |
| 零跑 ARF | `lp_arf.ini` / `lp_arf` | APP、FT（PLS→APP）；TMP单包或S19/SREC/BIN配套ASC/TMP |
| 吉利 P416 | `geely_p416.ini` / `geely_p416` | SBL、APP、ESS VBF；项目 NM 和专用传输；按所选 VBF 元数据刷写，不以文件哈希或固定块布局白名单阻断 |
| 吉利 P417 | `geely_p417.ini` / `geely_p416` | 完整复用 P416 端点、入口、服务顺序和 VBF 参数；使用独立 `resources/geely_p417` 目录 |
| 吉利 P611 | `geely_p611.ini` / `geely_p416` | 完整复用 P416 端点、入口、服务顺序和 VBF 参数；使用独立 `resources/geely_p611` 目录 |
| 北汽 N61AB | `baic_n61ab.ini` / `baic_n61ab` | Classic CAN；归档 CAPL、Driver/APP S19 与 SeedKey 已接入；正常 APP Download |
| 北汽 BQB41 | `baic_bqb41.ini` / `baic_bqb41` | CAN FD；四设备；成功 BLF 流程与已知答案验证的 SeedKey 已接入；Driver/APP需手动选择 |

## 11. 楚能 ARC331 专项说明

- 当前目标为右后 `0x72C/0x72D` 和左后 `0x72E/0x72F`，目标切换同步影响探测、版本读取、刷写、监听过滤、日志和报告；
- CBF 输入必须为 Driver CBF + APP CBF；S-record 输入必须为 Driver S19/SREC + Driver ASC + APP S19/SREC + APP ASC；禁止两种来源混搭；
- CBF 预检覆盖版本、Header、类型、数据格式、地址、长度、段 CRC16、整体 CRC32、ABT Hash、主数据 SHA-256、256 字节签名和固定刷写窗口；
- APP“能否刷写”在 `10 03` 后继续执行 `31 01 02 03`；NRC `0x31` 会提示可能处于擦除中断后的 Boot/SBL 恢复态，并阻止把它误判为普通 APP 可刷写；
- 正式刷写维持 10 ms 周期 `0x520`，执行项目规定的前置条件、安全访问、Driver/APP 下载、签名校验、SBL 激活、指纹、复位和恢复；
- Boot 恢复引擎属于内部受控能力。正式发布构建默认关闭 `UDS_EXPOSE_ARC331_BOOT_RECOVERY`，普通 dist 下拉框不显示 Boot 恢复入口；
- 恢复态处理必须使用单独显式启用的受控构建和台架操作，不应把普通 APP 重试当作恢复方案。

## 12. 配置、日志和生成目录

- `profiles`、经过运行时筛选的 `resources` 和 `drivers` 是发布内容；
- `Configuration` 保存当前用户的界面和硬件选择，由程序运行时创建；
- `logs/execution` 保存界面运行日志，`logs/reports` 保存 HTML 报告，`logs/traces` 按 probe/version/diagnostic/flash 分类保存单次操作 ASC，`logs/bus_monitor` 保存持续总线监听 ASC；这些目录均由程序运行时创建；
- 刷写页运行日志按目标持续追加；版本读取开始和结束会写入独立分隔标记，读取失败不会清除此前刷写日志，也不会覆盖刷写页最后结果；
- 版本读取页的原始通信框只展示当前一轮读取，每次开始读取时会清空该区域，但磁盘执行日志和已有刷写报告不受影响；
- 干净发布包不携带开发机历史 `Configuration`、`logs`、`.partial`、`validation`、台架探针、内部流程文档、厂商头文件/导入库、未使用的参考工程、构建目录或 Python 缓存；
- 清理发布包不会清理源码目录或公共盘资料。

## 12. 构建、验证与证据边界

- 构建命令：`scripts\build.ps1 -Config Release -DistPath dist`；
- 当前离线 CTest 共 8 项：核心、P416、CAN 适配、厂商 API 边界、厂商清单、应用状态、Qt 探测桥接和 Qt 主窗口；
- 离线测试覆盖 Profile/Workflow、文件预检、ISO-TP/UDS、Fake ECU、共享通道、监听、日志尾随、审计和 UI 状态等回归；
- 构建成功、CTest PASS、历史报告、硬件探针、真实 CAN 通信和真实 ECU 完整刷写是不同证据层级；
- 某一项目、某一设备或某一模式的 PASS 不自动证明另一设备、FT/CAL、其他后端或相似项目通过；
- 正式交付主程序和压缩包应以 SHA-256 绑定，校验值记录在压缩包旁的 `.sha256.txt` 文件中。

## 13. 文档入口

- `README.md`：当前版本完整功能说明；
- `CHANGE_LIST.txt`：仅记录相对公共盘 8.19 正式包的变化；
- 源码仓库中的 `docs`、`validation`、测试工具和资源来源记录只用于研发、审核与台架验收，不进入正式用户包。
