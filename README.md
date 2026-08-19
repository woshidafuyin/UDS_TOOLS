# UDS Tool C++ 通用刷写工具

更新日期：2026-08-19

本工程是独立的 C++/Qt UDS 通用刷写工具。本文只记录当前源码、Profile、Workflow、资源、界面和候选发布包的实现情况。

## 当前组成

- 19 个项目 Profile；
- 16 个已注册 Workflow ID；
- Vector XL、Kvaser、TOSUN、ZLG CAN 后端；
- 在线探测、版本读取、刷写、报告、日志和被动总线监听；
- 当前综合候选发布目录：`dist`。

## 当前发布

工程统一构建并发布通用版 `uds_tool_qt.exe`。`scripts\build.ps1` 加载全部项目 Profile，不再提供客户独立版本构建入口。通用界面的 Tx/Rx ID 当前允许手工修改；切换项目或设备时会恢复所选 Profile/目标的默认端点。Profile 中的 `lock_diagnostic_ids` 当前作为配置元数据加载，通用界面尚未用它锁定输入框。

Profile 保存项目、设备、入口、诊断 ID、CAN 参数、默认文件和能力开关。Workflow 保存对应项目的刷写服务序列、文件解析、地址窗口、完整性校验、周期报文和恢复步骤。多个项目可以复用同一状态机，但仍使用各自的 Profile 和资源。

## 主要模块

| 模块 | 当前职责 |
| --- | --- |
| `src/ui/qt` | 主窗口、刷写作业、版本读取、总线监听和文件选择 |
| `src/app` | 在线探测、刷写调度、版本读取、状态管理和报告生成 |
| `src/core` | Profile、S-record/VBF/ASC/CBF 数据解析、UDS 公共数据结构 |
| `src/transport` | ISO-TP、UDS 会话、请求响应和超时处理 |
| `src/drivers/can` | Vector XL、Kvaser、TOSUN、ZLG 适配及共享 CAN 通道 |
| `src/flash` | 各项目 Workflow、刷写状态机和项目协议契约 |
| `profiles` | 19 个项目配置入口 |
| `resources` | 项目 Driver、CDD、DLL、参考文件、校验文件和来源清单 |
| `tests` | 核心、CAN 适配、应用状态、探测桥接和 Qt 主窗口离线测试 |

## 当前通用能力

### 刷写作业

- 读取当前 Profile 的 CAN 通道、波特率、CAN/CAN FD、Tx ID、Rx ID、功能 ID和扩展寻址参数；
- 支持 APP、FT、CAL、APP+CAL 等由具体 Profile 声明的模式；
- 支持刷写次数、停止请求、进度、步骤日志和 HTML 报告；
- 每次完整执行均重新创建 Workflow，首次失败后停止后续重复次数；
- 刷写前先完成文件类型、长度、地址窗口和项目约束检查，再访问 CAN；
- 项目恢复步骤由对应 Workflow 执行，失败报告不代替 ECU 状态确认。

### 在线探测

- 使用当前 Profile 和界面端点探测 ECU；
- 支持 `app`、`boot`、`ft` 和项目定义的探测入口；
- 项目所需的 NM、功能寻址、物理寻址和周期唤醒由探测服务按 Profile/Workflow 执行；
- 在线探测只确认当前定义的诊断条件，不进入下载传输。

### 一键版本读取

- 版本读取页自动跟随刷写作业页的厂商、项目、设备、CAN 后端、通道及当前 Tx/Rx ID；手工修改 Tx/Rx ID 后，读取使用界面当前值；
- 当前 19 个 Profile 均配置了 `[version_check]`，合计 131 个读取项；其中零跑 A12EV 为 `placeholder=true`，其在线探测、版本读取和刷写均被禁止；
- 页面在读取前集中显示请求、DID、含义和必读属性，读取后显示状态、完整解码值及原始 UDS 响应；
- 每个读取项由 Profile 配置请求、正响应前缀、解码器、期望值及是否必读，不在界面代码中按项目复制读取列表；
- 当前支持 ASCII、十六进制、犀重 F180/F189 结构、计数 ASCII、BCD+ASCII 零件号及计数零件号列表解码；
- 必读项全部成功才报告“全部必读版本信息读取成功”；选读项失败单独显示，不替代必读项判定；
- 支持停止读取、ASC 原始总线记录和打开最新 HTML 报告；在线探测、刷写和版本读取通过共享操作状态互斥，避免并发访问硬件；
- 项目读取前置条件由 Profile 选择：楚能维持 `0x520` 周期唤醒，犀重维持项目 NM，相关 ARS1.31 项目维持 `0x400` 周期报文；
- Profile 中存在 DID 和含义只证明软件已配置；诊断调查表确认、真实 ECU 返回和台架验收仍需分别留证。

当前项目版本读取配置数量：

| 项目 | 读取项 | 当前状态 |
| --- | ---: | --- |
| 楚能 ARC331 | 5 | 右后/左后设备联动，维持 `0x520` |
| 奇瑞 ARS1.33 / KP31 / E0Y / T22 / T1EJ | 各 7 | 已配置 |
| 长安 C857 / B216 | 各 3 | 主/从目标联动，维持 `0x400` |
| 长马 J90K / ARS1.31 | 7 | 维持 `0x400` |
| 犀重 RSMR / LSMR | 各 3 | 维持项目 NM，使用项目结构解码器 |
| 时代新安 HJZJ FMR | 6 | 已配置 |
| 时代新安 天王星 / 木星2代 / 庆铃 FMR | 各 9 | 独立 Profile 配置 |
| 零跑 ARC / ARF631 | 各 8 | 已配置 |
| 零跑 A12EV | 10 | `placeholder=true`，运行能力禁用 |
| 吉利 P416 | 13 | 已配置 |

### CAN 通道与总线监听

- 相同设备、物理 Channel、波特率和 CAN/CAN FD 参数通过共享 CAN Provider 复用一个底层通道；
- 在线探测、刷写和总线监听使用独立逻辑客户端；
- 总线监听页面只被动接收，不发送报文；
- 工具启动后自动监听刷写页当前通道；切换项目、设备、CAN 通道或手工修改 Tx/Rx ID 后，监听上下文和诊断 ID 集合随当前刷写选择同步；
- “仅显示诊断 ID”默认开启：表格只显示当前物理 Tx/Rx、功能 ID，以及项目声明的 FT Tx/Rx；没有配置诊断 ID 时不限制显示；
- “仅显示诊断 ID”只影响表格显示，非诊断帧仍在后台接收并保留于有界缓存中，关闭过滤后可重新显示，导出 ASC 始终包含全部已缓存原始帧；
- 仍可叠加手工 ID、数据、TX/RX、标准/扩展、CAN/CAN FD 和 BRS 过滤；
- 最终 NRC 和 `0202=05` 等最终 RoutineControl 失败在表格中红色强调并解释含义；`7F xx 78` 保留原始帧但不作为失败报警，ARC331 的 `0203=05` 按当前项目参考流程显示为 WARN；
- 监听结果可导出为 ASC 文件。

### 文件格式

| 格式 | 当前用途 |
| --- | --- |
| S19/SREC | Driver、APP、CAL 等 Motorola S-record 数据 |
| ASC/HEX 文本 | Hash、签名、证书和项目校验数据 |
| VBF | 吉利 P416 等 VBF 项目的 SBL/APP/ESS 数据 |
| CBF 1.0 | 楚能 ARC331 成对 Driver/APP 容器 |

文件解析成功只代表数据可提取；必须继续满足对应 Workflow 的类型、地址、长度、Hash、签名和刷写窗口约束。

## 当前项目实现

| 项目 | Profile / Workflow | 当前模式与状态 |
| --- | --- | --- |
| 楚能 ARC331 | `chuneng_331_left_rear.ini` / `chuneng_arc331` | 右后 `0x72C/0x72D`、左后 `0x72E/0x72F` 共用同一雷达刷写流程；无 `0x771` 私有过渡帧 |
| 奇瑞 ARS1.33 | `chery_ars1_33.ini` / `chery_ars1_33` | APP、CAL、APP+CAL；设备和入口由 Profile 选择 |
| 奇瑞 KP31 | `chery_kp31.ini` / `chery_kp31` | APP、CAL、APP+CAL |
| 奇瑞 E0Y | `chery_e0y.ini` / `chery_e0y` | 复用 KP31 内核；正常 APP 流程；固定项目诊断端点 |
| 奇瑞 T22 | `chery_t22.ini` / `chery_t22` | 复用 KP31 内核；正常 APP 流程；固定项目诊断端点 |
| 奇瑞 T1EJ | `chery_t1ej.ini` / `chery_t1ej` | 复用 KP31 内核；`D003/D004/D002/D005` 正常 APP 链路 |
| 长安 C857 | `changan_c857.ini` / `changan_c857` | 主/从设备；APP、FT、CAL、APP+CAL |
| 长安 B216 | `lingyao_b216.ini` / `lingyao_b216` | 主/从设备；Profile 定义可用模式 |
| 长马 J90K / ARS1.31 | `longma_ars1_31.ini` / `longma_ars1_31` | APP、FT |
| 犀重 RSMR | `xizhong_rsmr.ini` / `xizhong_rsmr` | APP、FT；包含项目 NM 和 ISO-TP 规则 |
| 犀重 LSMR | `xizhong_lsmr.ini` / `xizhong_lsmr` | 复用 RSMR 状态机；独立扩展诊断 ID、NM 和 SeedKey 资源；当前为 APP 路径 |
| 时代新安 HJZJ FMR | `shidaixinan_hjzj_fmr.ini` / `shidaixinan_hjzj_fmr` | APP、FT；使用项目周期报文和校验资源 |
| 时代新安 天王星 FMR | `shidaixinan_tianwangxing_fmr.ini` / `shidaixinan_hjzj_fmr` | 复用 HJZJ 状态机；独立 Profile/资源；默认 APP 为空 |
| 时代新安 木星2代 FMR | `shidaixinan_muxing2_fmr.ini` / `shidaixinan_hjzj_fmr` | 复用 HJZJ 状态机；独立 Profile/资源；默认 APP 为空 |
| 时代新安 庆铃 FMR | `shidaixinan_qingling_fmr.ini` / `shidaixinan_hjzj_fmr` | 复用 HJZJ 状态机；独立 Profile/资源；默认 APP 为空 |
| 零跑 ARC | `lp_arc.ini` / `lp_arc` | APP、FT |
| 零跑 ARF631 | `lp_arf.ini` / `lp_arf` | APP、FT |
| 零跑 A12EV | `lp_a12ev.ini` / `lp_a12ev` | 当前 `placeholder=true`；界面可见，在线探测、版本读取和刷写均被禁止 |
| 吉利 P416 | `geely_p416.ini` / `geely_p416` | SBL、APP、ESS VBF 流程；支持项目 NM 唤醒和专用传输规则 |

## 楚能 ARC331 当前实现

当前 Profile 提供“右后雷达”和“左后雷达”两个设备。刷写作业选择变化后，版本读取页跟随同一 Profile、设备、通道和 Tx/Rx ID；版本读取项由 Profile 的 `[version_check]` 集中配置，当前为 `F187` ECU 零件号、`F180` BootLoader 版本号、`F195` 供应商软件版本号、`F189` 整车厂软件版本号和 `F193` 供应商 ECU 硬件版本号。

### 输入模式

S-record 模式：

```text
Driver S19/SREC + Driver Verification ASC
APP S19 + APP Verification ASC
```

CBF 模式（Driver、APP 必须成对选择）：

```text
Driver CBF
APP CBF
```

当前只接受两套完整输入：`Driver CBF + APP CBF`，或者 `Driver S19/SREC + Driver ASC + APP S19/SREC + APP ASC`。不允许一侧为 CBF、另一侧为 S19，以免混用不同来源的主数据与签名。CBF 模式分别解析两份容器的主数据、ABT 和 256 字节 `dev_signature`；S19 模式使用界面选择的两份 256 字节校验 ASC。当前 Profile 默认指向资源目录中的 Driver/APP 双 CBF。两种输入最终进入同一个楚能 `0202 + 256 字节签名` 状态机，不生成中间 S19，也不进入零跑 `6000/6001 + 1322 字节证书` 流程。

CBF 预检包含：版本、必需 Header 字段、类型、数据格式、两段地址和长度、段 CRC16、整体 CRC32、ABT Header、ABT Hash、主数据 SHA-256、256 字节 `dev_signature` 及固定刷写窗口。`FAKE_CN2944_FLASH_DRIVER_RAW_0x4000` 是本项目已确认允许刷写的 Driver 标识，因此不会仅按文件名或主数据前缀拒绝；容器完整性与项目窗口检查仍然执行。任一预检失败时不访问 CAN。

### 在线探测

- 探测期间持续发送标准 CAN `0x520 00 00 00 00 00 00 00 00`，周期 10 ms；
- APP 入口：物理 `10 03` 收到 `50 03` 后，继续执行 `31 01 02 03` 刷新条件检查；
- BOOT 入口：物理 `10 03` 收到 `50 03` 即确认诊断在线，不发送仅 APP 入口适用的 `31 01 02 03`；
- 探测使用当前 Profile/界面配置的物理 Tx/Rx ID，不从 CBF 的 ECU 地址字段推导诊断 ID。

### 正式刷写

- APP 入口按楚能正式规范 Q/CN A201-2025 执行前置条件（物理 `10 03`、物理 `31 01 02 03`、功能 `10 83`/`85 82`/`28 83 03`）、会话切换、安全访问（16 字节种子/密钥）、Driver 下载、Driver 校验、`31 01 03 01` 激活 SBL、激活后写 `2E F1 84` 指纹、APP 擦除/下载/验签和复位恢复；
- BOOT-only 入口使用 Boot 可满足的会话序列进入编程，不执行仅 APP 入口适用的前置步骤；
- 正式刷写期间同样以高精度计时维持 10 ms 的 `0x520` 周期唤醒；
- Driver/APP 主数据仍使用 Workflow 固定并经过预检的传输窗口，CBF 只改变输入解析，不改变既定 UDS 下载服务序列。

详细流程见 `docs/CHUNENG_331_FLOW_PARITY.md`。

## 2026-08-19 两条最新开发支线

当前 Release 可执行程序的功能基线提交为 `7d74a5d`。两条最新开发支线不是互相分叉的两个 Git 分支，而是已经按先后顺序进入同一提交链，最终 Release 同时包含二者：

1. `98f1cbd fix: clarify ARC331 flashing status diagnostics`
   - 楚能 ARC331 Driver/APP CBF 成对输入；
   - 楚能 ARC331 Driver/APP S19、`*_Ver.asc`、`*_ABT.asc` 成对输入；
   - Driver、Driver ABT、APP、APP ABT 四组下载；
   - Driver 和 APP 分别执行 `0202 + 256 字节签名`，并执行最终 `FF01` 和复位；
   - `36 当前块/总块数` 进度；
   - 最终失败报文/NRC 红色解释、`0x78` 等待态降噪及 `0203=05` WARN 语义。
2. `7d74a5d feat: add diagnostic-ID filter to bus monitor`
   - 总线监听新增默认开启的“仅显示诊断 ID”；
   - 诊断 ID 自动取当前界面物理 Tx/Rx、功能 ID和项目 FT Tx/Rx，并随项目、目标和手工 ID 变化同步；
   - 过滤只作用于表格显示，不丢弃后台缓存帧，也不缩减 ASC 导出证据；
   - 保留原有手工过滤、失败红色解释及被动零发送边界。

`98f1cbd` 是 `7d74a5d` 的直接祖先；本节所述 Release EXE 和最终压缩包均以 `7d74a5d` 为功能基线，不存在只打入其中一条支线的情况。后续 README 文档提交只更新交付说明，不改变该 EXE 二进制。

## 相对 2026-08-03 发布包的增量

对比基线：

```text
D:\project\UDS_tools\packages\
UDSD_7_28_dist-ui-vendor-project-device-p2fix-ui-blank_20260803_175639_release.zip
```

该文件后来增加过一份审计 README；以下统计以保留的原始同哈希备份 `..._ORIGINAL_E914D6A1.zip` 为软件基线，原始 ZIP SHA-256 为 `E914D6A14E8BE6BE59F0150CAB4383D1B3F79A860FF44BA5AAA2A4870C0422DC`。去掉老 ZIP 最外层目录差异后，与 `build\release-stage-7d74a5d` 逐文件比较：

| 项目 | 数量 |
| --- | ---: |
| 2026-08-03 原始基线文件 | 307 |
| 当前 Release 暂存文件 | 371 |
| 路径和 SHA-256 完全一致 | 286 |
| 同路径但内容改变 | 15 |
| 当前版本新增 | 70 |
| 老版本存在、当前版本未携带 | 6 |

### 新增项目和资源

- Profile 从 10 个增加到 19 个；新增奇瑞 KP31/E0Y/T22/T1EJ、零跑 A12EV、时代新安天王星/木星2代/庆铃 FMR、犀重 LSMR；
- 新增楚能 D7 ARC331 独立资源集，包含 Driver/APP CBF、Driver/APP S19、Ver ASC、ABT ASC、SeedKey DLL、CDD 和参考工程材料；
- 新增奇瑞 KP31、吉利 P416 正式命名 VBF、零跑 A12EV/ARF、时代新安公共及三个派生项目、犀重 LSMR 的 Profile 配套资源；
- 19 个 Profile 均接入集中式版本读取配置，共 131 个读取项；A12EV 仍为资料占位，在线能力被明确禁止；
- 通用界面当前保留“刷写作业、版本读取、总线监听”三页，不新增并行状态页面。

新增的 9 个 Profile：

```text
chery_e0y.ini
chery_kp31.ini
chery_t1ej.ini
chery_t22.ini
lp_a12ev.ini
shidaixinan_muxing2_fmr.ini
shidaixinan_qingling_fmr.ini
shidaixinan_tianwangxing_fmr.ini
xizhong_lsmr.ini
```

### 同路径发生改变的内容

- 主程序 `uds_tool_qt.exe`、`keygen_broker.exe`、CAN 硬件探测工具和 Kvaser 预检脚本更新；
- `chery_ars1_33.ini` 增加主/从设备和版本读取，原从雷达端点保留，主雷达仍标记待台架验证；
- `geely_p416.ini` 默认切换到正式命名 VBF 并增加版本读取；老 reconstructed VBF 仍在资源目录，可手工选择；
- `longma_ars1_31.ini`、`lp_arc.ini`、`lp_arf.ini`、`shidaixinan_hjzj_fmr.ini` 增加版本读取或项目显示信息，原刷写字段未发现删除；
- `chuneng_331_left_rear.ini` 已从老楚能 331/2944 定义切换为新楚能 D7 ARC331 左/右后雷达定义，属于项目替换而非原端点的普通增强。

### 老版本未原样保留的内容与兼容性边界

当前 Release 未携带老包中的快捷方式，以及以下 `resources\chuneng_2944` 原路径文件：

```text
APP\Awr2944_Es2_sign.appimage(1).s19
dll\ChuNeng_D7_SeednKey_V1.0.dll
Driver\FlashDriver.srec
Verification\7AABB0001AA.asc
Verification\DriverVerification.asc
```

其中 SeedKey DLL、Driver 和 Driver Verification 可在新 ARC331 资源目录找到同哈希副本；旧 APP S19 和 `7AABB0001AA.asc` 没有同哈希副本。旧 `chuneng_331_left_rear` 的 `chuneng_331`、`0x772/0x77A` 语义也已被新的 `chuneng_arc331`、右后 `0x72C/0x72D`、左后 `0x72E/0x72F` 替换。因此当前包可以证明新 ARC331 CBF/S19 流程已集成并实板通过，但不能宣称仍可直接复现老包的楚能 331/2944 刷写入口。

除该已确认兼容性边界外，共有的 CAN 驱动和 Qt 运行库均保持同哈希；其余项目的离线测试通过也不替代逐项目、逐设备、逐模式的真实 ECU 回归。

## 当前发布与验证

- 综合候选目录：`dist`；
- 主程序：`dist/uds_tool_qt.exe`；
- 最新干净交付包：`UDS_Tool_Release_20260819_7d74a5d.zip`；
- `dist` 包含运行库、CAN 驱动适配、Profile、项目资源、工具和文档；
- 当前构建命令：`scripts\build.ps1 -Config Release -DistPath dist`；
- 2026-08-19 当前候选完成 Release 构建和 CTest 1–7（7/7）回归；第 8 项 Qt 主窗口测试在新旧二进制上均停于 `QApplication` 启动检查点，尚未形成界面运行通过证据；
- 最新 Release 暂存目录：`build\release-stage-7d74a5d`；
- 最新 Release `uds_tool_qt.exe` SHA-256：`E1282F26027A25745ED7065F3D64618FF28FE41CF70F551DFFCBF4CEAD0EF887`；
- 当前 `master` 同时包含 `98f1cbd` 楚能 ARC331 诊断/ABT 支线和 `7d74a5d` 总线监听诊断 ID 过滤支线；
- 楚能 ARC331 默认使用独立 Driver/APP CBF 对；`resources\chuneng_d7_arc331_zip\S19` 同时提供从该 CBF 对提取的同源 S19、`*_Ver.asc` 和 `*_ABT.asc`，S19 模式在接入 CAN 前校验 ABT 地址、长度和 SHA-256；
- 当前 Release 对应的楚能 ARC331 CBF 与 S19/ASC 两种模式均已在左后雷达 `0x72E/0x72F` 实板完成 Driver、Driver ABT、APP、APP ABT、`0202=04`、`FF01=04` 和 `51 01` 完整闭环；右后雷达仍需独立台架确认；
- 该快照包含楚能 ARC331 专用流程的两处规范对齐修正（`chuneng_331_flow.cpp`）：预编程顺序改为物理 `10 03` → 物理 `31 01 02 03` → 功能 `10 83/85 82/28 83 03` → 物理 `10 02`；`2E F1 84` 指纹移到 `31 01 03 01` 激活 SBL 之后、`31 01 FF 00` 擦除之前（Q/CN A201-2025 5.4.5/附录 C）；修改前源码备份于 `validation\2026-08-19_flow_fix_backup`；
- 构建、离线测试、Fake ECU、Golden Trace、历史报告和真实 ECU 台架是不同证据层级；
- 某个项目或设备的 APP PASS 不自动证明其 FT、CAL、另一设备或相似项目通过。

## 文档入口

- `docs/README.md`：当前文档索引；
- `docs/ARCHITECTURE.md`：当前模块边界；
- `docs/VERSION_READ_CONFIGURATION.md`：版本读取 Profile 格式、解码器、项目统计和验证边界；
- `docs/*_FLOW_PARITY.md`：各项目当前服务流程和验收边界；
- `docs/SHIDAIXINAN_ARF232_PROJECT_INTEGRATION.md`：时代新安 Profile、资源和复用关系；
- `docs/KVASER_BENCH_CHECKLIST.md`：Kvaser 台架检查；
- `SOURCE_PACKAGE_README.md`：当前源码包组成和排除项。
