# TOSUN / TSMaster libTSCAN SDK

## 官方来源

- 统一来源页：<https://www.tosunai.com/downloads/tsmaster-api/>
- `packages/TSCAN.API.Demo_C++.zip`：
  <https://www.tosunai.com/download/tscan-api-demo_c/>
- `packages/lib.zip`：<https://www.tosunai.com/download/lib/>
- `docs/libTSCAN_SDK_CPP_cn.pdf`：
  <https://www.tosunai.com/download/libtscan-sdk-cpp-cn-pdf/>
- 下载日期：2026-07-23

2026-07-28 实机接入时，下载包中的 x64 `libTSCAN.dll` 在本机设备扫描阶段触发
访问异常。`sdk/bin/x64/` 的两个运行 DLL 已换成这台电脑官方 TSMaster 安装目录
`C:\Program Files (x86)\TOSUN\TSMaster\bin64` 中相互匹配的版本；原下载 ZIP
仍保留在 `packages/`，两套来源没有混淆。当前部署 DLL 的 SHA-256 见清单。

原始 ZIP 永久保留在 `packages/`，规范化 SDK 入口在 `sdk/`，官方示例原样保留在
`examples/cpp/`。文件大小和 SHA-256 见 `SOURCE_MANIFEST.sha256`。

## 项目接入

`TosunCanAdapter` 通过进程级运行时动态加载 `libTSCAN.dll`，不依赖 TSMaster 进程，也不把
现有 ISO-TP/UDS/刷写流程迁移到同星高层诊断 API。默认加载顺序：

1. 环境变量 `UDS_TOSUN_DRIVER_DIR` 指向的目录；
2. 可执行文件旁的 `drivers/tosun/libTSCAN.dll`；
3. 可执行文件同目录的 `libTSCAN.dll`；
4. 官方 TSMaster 默认安装目录的 `bin64/libTSCAN.dll`。

`libTSCAN` 的初始化、释放是进程级生命周期，适配器不会在每次在线探测、版本读取或
刷写之间反复 `finalize`/卸载厂商库；每次操作仍独立连接和断开设备。

x64 本地构建将 `sdk/bin/x64/` 复制到 `<exe>/drivers/tosun/`。当前实现支持：

- 设备扫描、序列号连接、CAN 通道数读取；
- Classic CAN 与 ISO CAN FD（仲裁域/数据域波特率）配置；
- Classic CAN 与 CAN FD/BRS 原始帧收发；
- 1-based 项目通道到 0-based TSCAN 通道的转换。

安全默认值：

- 内置 120Ω 终端电阻不使能；
- 普通同步发送，100 ms 超时；
- 不启用周期发送、总线控制或厂商 UDS/刷写 API。

## 位数

- `sdk/bin/x64/libTSCAN.dll`、`libTSH.dll`：PE32+ x64；
- `sdk/lib/windows/x86/`：PE32 x86；
- Qt 主程序为 x64，只部署 x64 运行库。

## 许可证边界

官方 ZIP 未附带独立 LICENSE/EULA 文件，下载页包含资料使用与分发条件。本仓库中的
SDK 和构建输出仅用于内部开发与台架验证。对外发布或产品交付前，必须重新核对并保存
当时适用的同星书面许可；不得把本目录中的二进制直接视为可自由对外再分发组件。
