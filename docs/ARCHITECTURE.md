# 当前架构

更新日期：2026-08-19

本文只记录当前通用版 `CH_Diagnostic_Studio.exe` 的模块边界、依赖方向和运行期协作关系。

## 总体结构

```text
Qt MainWindow
  ├─ 刷写作业页
  ├─ VersionConfirmationPage
  └─ BusMonitorPage
        │
ControllerBridge
  ├─ ProbeController -> ProbeService
  ├─ FlashController -> FlashWorkflow registry
  └─ VersionCheckController -> VersionCheckService
        │
OperationState
  └─ 探测、刷写、版本读取互斥
        │
ICanBusProvider
  ├─ SharedCanBusProvider
  └─ TracingCanBusProvider / TracingCanBus
        │
CanBusSession
  └─ ICanHardwareAdapter
       ├─ VectorCanAdapter -> VectorXlBus
       ├─ TosunCanAdapter -> libTSCAN.dll
       ├─ ZlgCanAdapter -> zlgcan.dll
       ├─ KvaserCanAdapter -> canlib32.dll
       └─ OtherCanAdapter -> Unsupported/NotImplemented

CH_Diagnostic_Studio.exe (x64，CMake 内部目标：uds_tool_qt)
  └─ keygen_broker.exe (x86)
       └─ OEM GenerateKeyEx DLL (x86)
```

## 依赖方向

- Qt UI 负责选择、输入、显示和发起操作，不实现 UDS 服务序列；
- `ControllerBridge` 将 Qt 信号转换为应用层请求，并把工作线程回调安全地转回 UI；
- `ProbeController`、`FlashController`、`VersionCheckController` 共享 `OperationState`，任何时刻只允许一个主动硬件操作；
- `ProbeService`、`VersionCheckService` 和具体 `FlashWorkflow` 通过 `ICanBusProvider` 获取总线，不直接依赖某个硬件 SDK；
- UDS 与 ISO-TP 只依赖 `ICanBus`，不认识 Vector、TOSUN、ZLG 或 Kvaser；
- CAN 厂商选择只出现在适配器工厂和应用组合根，业务流程不直接包含厂商 API；
- 项目配置是数据，刷写服务顺序由 Workflow 管理；多个项目可以复用状态机，但仍保持独立 Profile、资源和验收结论。

## Profile 与项目复用

Profile 集中保存：

- 厂商、项目、设备、入口和能力开关；
- CAN/CAN FD 参数、通道、Tx/Rx/功能 ID；
- `target_N_*` 多设备端点；
- Driver、APP、CAL、校验文件和 SeedKey DLL；
- 固定地址窗口、长度、填充值和安全等级；
- `[version_check]` 版本读取计划。

同一项目的多 ECU 或多雷达差异优先由 `target_N_*` 描述。选择目标时，UI 整体切换设备名和默认端点；版本读取页同步使用当前 Profile、目标、通道和界面 Tx/Rx ID。通用版允许手工修改 Tx/Rx ID，切换项目或设备会恢复 Profile 默认值。

顺序相同的项目复用共享状态机，项目类型只提供 Workflow ID、协议参数或显示标签；服务顺序不同时才新增独立 Workflow。复用状态机不代表资源、身份、诊断调查表或台架结论可以继承。

## 一键版本读取

版本读取由以下模块协作：

```text
Profile [version_check]
  -> load_version_check_plan()
    -> VersionCheckController
      -> VersionCheckService
        -> UdsClient / IsoTpSession
          -> decode_version_value()
            -> VersionConfirmationPage
```

- `load_version_check_plan()` 从当前 Profile 读取会话、前置条件、请求、响应前缀、解码器、期望值和必读属性；
- `[version_check.target.<target_id>]` 只覆盖目标相关的 `expected`，共用请求列表不复制；
- `VersionCheckService` 打开当前 CAN 上下文、启动项目所需前置发送器、逐项请求并保留完整原始响应；
- 当前前置条件包括 `ars131_0x400`、`chuneng_520` 和 `xizhong_nm`；
- 解码器负责把正响应前缀之后的 payload 转为项目值；布局不合法、非 ASCII 字节、未知解码器均明确失败；
- 必读项决定整体结果，选读项失败不伪装成全部成功；
- 版本读取可以生成 ASC 原始通信记录和 HTML 报告；配置存在、离线测试通过和真实 ECU 返回正确是不同证据层级。

详细配置契约见 `VERSION_READ_CONFIGURATION.md`。

## 主动操作与总线监听

`OperationState` 只管理在线探测、刷写和版本读取三个主动操作，状态为 `idle/running/stopping`。它位于应用层，不依赖 Qt，因此控制器测试可以离线验证互斥和停止行为。

`BusMonitorPage` 是被动接收页面：

- 跟随当前硬件后端、通道和波特率上下文；
- 使用共享 Provider 复用满足相同硬件参数的底层通道；
- 只接收、筛选、显示和导出 ASC，不发送报文；
- 主动操作运行时由 UI 协调监听状态，不能把监听结果当作 UDS 请求成功证据。

`TracingCanBusProvider`/`TracingCanBus` 包装现有总线并记录原始帧，不改变 ISO-TP、UDS 或项目 Workflow 的控制流。

## CAN 硬件接口

`ICanHardwareAdapter` 统一提供初始化/释放、设备枚举、设备打开/关闭、CAN/CAN FD 通道配置、通道启动/停止、报文收发、状态和错误查询。`CanBusSession` 将该生命周期适配为 `ICanBus::open/close/send/receive`。

当前默认厂商是 Vector。Qt 菜单“设备 -> CAN硬件后端”提供 Vector、ZLG/ZCANPRO、TOSUN/TSMaster、Kvaser；选择通过 `QSettings` 持久保存。每个后端使用独立的 `hardware/channel/<vendor>` 通道键，切换后端先保存旧通道，再恢复新后端通道。

- Vector：`VectorCanAdapter` 包装兼容实现 `VectorXlBus`；
- TOSUN：动态加载 `libTSCAN.dll`；
- ZLG：动态加载 `zlgcan.dll`，首个实机目标为 USBCANFD-200U（设备类型 41）；
- Kvaser：动态加载系统 `canlib32.dll`，枚举时区分物理与 Virtual CAN，并保留全局索引、板载通道、序列号和 EAN；
- Other：明确返回 `Unsupported/NotImplemented`。

构建开关为 `ENABLE_VECTOR`、`ENABLE_ZLG`、`ENABLE_TOSUN`、`ENABLE_KVASER`，四者默认开启。缺少运行时 DLL 或设备时返回带厂商上下文的错误，不伪造通信成功。

## 文件解析与访问 CAN 的边界

S19/SREC、ASC/HEX、VBF、CBF 的解析位于核心层。刷写前先完成文件类型、地址、长度、校验和项目窗口约束，再打开 CAN。

楚能 CBF 1.0 预检包括 Header、容器类型、数据格式、段地址/长度、段 CRC16、整体 CRC32、ABT Header、ABT Hash、主数据 SHA-256 和固定刷写窗口。离线解析或重算通过只证明输入满足软件契约，不证明 ECU 接受。

## 测试与证据边界

当前 CTest 覆盖核心解析、Workflow/Profile、CAN 适配、应用状态、探测桥接和 Qt 主窗口。Fake Bus、已知 SeedKey 向量、Golden Trace、构建成功、候选包生成、真实 CAN 执行和台架 PASS 必须分别陈述；任何一个层级都不能替代其他层级。
