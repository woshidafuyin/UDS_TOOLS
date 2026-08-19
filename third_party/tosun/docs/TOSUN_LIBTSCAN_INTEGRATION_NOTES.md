# TOSUN libTSCAN 后续接入准备说明

## 1. 范围与证据优先级

本文只根据以下资料进行静态设计，不构成硬件通信验证：

1. `sdk/include/libTSCAN.h`（随当前 `lib.zip` 提供，API 声明的首要依据）；
2. `examples/cpp/TSCAN.API.Demo_C++`（调用顺序和结构体用法参考）；
3. `docs/libTSCAN_SDK_CPP_cn.pdf`（63 页，封面标注 Linux C++ V1.2）。

PDF 后半部分重复列出 Windows 风格 API，部分函数返回类型与当前头文件不一致。
例如 PDF 某处把初始化和释放描述为返回 `u32`，而当前 Windows 头文件声明为
`void`。未来实现必须以随实际 DLL 一起交付的 `libTSCAN.h` 和厂商版本说明为准，
不能仅按 PDF 抄写签名。

## 2. 固定架构边界

```text
现有刷写/在线探测 -> 现有 UDS -> 现有 ISO-TP -> ICanBus/ICanHardwareAdapter
                                                    |
                                                    +-> Vector（当前默认）
                                                    +-> TosunCanAdapter（未来：仅原始 CAN/CAN FD）
```

即使 `libTSCAN.h` 暴露了 `tsdiag_*` 诊断函数，未来适配也不得使用这些函数替代、
复制或旁路当前 ISO-TP、UDS 和刷写实现。`TosunCanAdapter` 只负责设备生命周期、
通道参数、原始帧发送和原始帧接收。

## 3. 初始化与释放 API

### 初始化

```cpp
void initialize_lib_tscan(bool AEnableFIFO,
                          bool AEnableErrorFrame,
                          bool AUseHWTime);
```

- 后续使用 FIFO 实现 `receive(timeout)` 时，`AEnableFIFO` 应为 `true`。
- 是否接收错误帧应成为适配器策略，不应直接照搬示例中的固定值。
- `AUseHWTime` 决定硬件时间戳策略；当前 `CanFrame` 没有时间戳字段，首版可不暴露，
  但必须记录选择。
- 只允许在适配器的库级生命周期开始时调用一次，并维护初始化引用/状态，避免多实例
  重复初始化或提前释放。

### 释放

```cpp
void finalize_lib_tscan(void);
```

必须在所有回调注销、FIFO 停止使用、设备断开之后调用。现有接口的 `release()` 是
`noexcept`，因此未来实现要在内部捕获并记录清理错误，不能让异常逃出析构/释放路径。

## 4. 设备扫描与连接 API

推荐的静态调用链：

1. `tscan_scan_devices(uint32_t* ADeviceCount)` 获取设备数量；
2. 对索引调用 `tscan_get_device_info(...)`，取得厂商、产品和序列号；
3. `tscan_get_can_channel_count(...)` 在连接后取得 CAN 通道数；
4. `tscan_connect(const char* ADeviceSerial, size_t* AHandle)` 用序列号连接指定设备；
5. 保存 `size_t` 设备句柄，并在关闭时调用 `tscan_disconnect_by_handle(...)`。

示例允许空序列号连接默认设备，并把返回码 `0` 和 `5` 都视为已连接。正式适配器不应
默认连接第一台设备；`CanDeviceInfo.id` 应使用稳定序列号，且需要向 TOSUN 确认返回码
`5` 的精确定义和重复连接时的句柄所有权。

## 5. CAN/CAN FD 通道配置 API

### 经典 CAN

```cpp
u32 tscan_config_can_by_baudrate(size_t handle,
                                 APP_CHANNEL channel,
                                 double rate_kbps,
                                 u32 termination_120_ohm);
```

### CAN FD

```cpp
u32 tscan_config_canfd_by_baudrate(size_t handle,
                                   APP_CHANNEL channel,
                                   double arbitration_kbps,
                                   double data_kbps,
                                   TLIBCANFDControllerType controller_type,
                                   TLIBCANFDControllerMode controller_mode,
                                   u32 termination_120_ohm);
```

适配注意点：

- `APP_CHANNEL` 的 `CHN1` 数值为 0；现有 `CanChannelConfig.channel` 默认值为 1，
  必须做明确的 1-based 到 0-based 转换和范围检查。
- 现有比特率单位是 bit/s，libTSCAN 便捷 API 使用 kbit/s，必须检查整除和范围。
- CAN FD 正常模式应明确使用 `lfdtISOCAN`（或项目确需的控制器类型）和
  `lfdmNormal`，不能照搬示例中混用的 `lfdtCAN`。
- 当前 `CanChannelConfig` 没有 120 欧终端电阻、监听模式、ISO/non-ISO FD 等字段。
  在接口策略确认前不得硬编码示例中的 `1`。
- 资料中没有独立的 CAN `start/stop channel` API；配置后是否立即启用、以及如何
  可靠停用单通道，需要厂商确认和台架验证。

## 6. CAN/CAN FD 发送 API

经典 CAN 可选：

- `tscan_transmit_can_sync(handle, &frame, timeout_ms)`；
- `tscan_transmit_can_async(handle, &frame)`。

CAN FD 可选：

- `tscan_transmit_canfd_sync(handle, &frame, timeout_ms)`；
- `tscan_transmit_canfd_async(handle, &frame)`。

`CanFrame` 到厂商结构体的映射要求：

- `id` -> `FIdentifier`，并校验标准/扩展 ID 范围；
- `extended` -> `FProperties.bits.extframe`；
- `fd` 决定使用 `TLIBCAN` 还是 `TLIBCANFD`；
- `brs` -> `TLIBCANFD.FFDProperties.bits.BRS`；
- CAN FD 时还需设置 EDL/FDF 位；
- `data.size()` 必须转换为正确 DLC，尤其是 12/16/20/24/32/48/64 字节档位；
- 通道号写入 `FIdxChn`，方向位按发送帧设置。

现有 `send()` 没有超时参数。首版应在“同步发送并使用内部受控超时”与“异步发送后
只检查入队返回码”之间做明确选择，并用 ISO-TP 连续帧压力测试验证；不能仅凭示例决定。

## 7. CAN/CAN FD 接收 API

FIFO 路径与现有同步接口最匹配：

```cpp
u32 tsfifo_receive_can_msgs(size_t handle, TLIBCAN* frames,
                            s32* count, u8 channel, u8 rx_tx);
u32 tsfifo_receive_canfd_msgs(size_t handle, TLIBCANFD* frames,
                              s32* count, u8 channel, u8 rx_tx);
```

也可以使用 `tscan_register_event_can/canfd` 回调，但这会引入回调线程、对象生命周期、
注销竞态和队列同步。首版建议使用 FIFO：按 `receive(timeout)` 的截止时间轮询小批量
数据，短暂等待，超时返回 `std::nullopt`，不得把“无报文”当作厂商错误。

头文件中 `READ_TX_RX_DEF` 的枚举名称与注释存在矛盾，示例对 `TX_RX_MESSAGES` 的注释
也不完全一致。正式实现必须通过厂商说明或台架验证确认该参数，并用
`FProperties.bits.istx` 再过滤发送回显，防止 ISO-TP 把自身发送帧当作响应。

关闭前应停止接收循环；如使用回调，先注销回调；如使用 FIFO，可调用对应 clear API
清理缓存。接收结构映射回 `CanFrame` 时要还原 ID 类型、FD、BRS、DLC和数据长度。

## 8. 错误码获取方式

除初始化/释放外，多数 API 返回 `u32`：`0` 通常表示成功，其他值应保留原始码，并调用：

```cpp
u32 tscan_get_error_description(u32 code, char** description);
```

未来应把结果映射为：

- 未找到/无法连接设备 -> `DeviceNotFound` 或 `DriverMissing`；
- 参数、通道、比特率无效 -> `InvalidConfiguration`；
- 其他厂商返回码 -> `VendorError`，消息中保留十进制/十六进制原始码及说明。

说明字符串的所有权、编码和生命周期未在当前头文件中充分说明，禁止由调用方随意释放；
接入前必须向厂商确认并复制到自有 `std::string`。

## 9. 关闭设备流程

建议的逆序关闭流程：

1. 阻止新的 `send/receive` 调用；
2. 停止接收线程或轮询；
3. 如使用回调，注销 CAN/CAN FD 回调并等待在途回调退出；
4. 清理本适配器缓存/FIFO；
5. `tscan_disconnect_by_handle(handle)`；
6. 清空句柄和通道状态；
7. 最后一个库使用者执行 `finalize_lib_tscan()`；
8. 只有在未来选定显式动态加载方案时，才在所有函数调用结束后卸载 DLL。

任何清理失败都要记录到 `last_error()`，但 `close_device()`、`stop_channel()` 和
`release()` 的 `noexcept` 路径不能抛出。

## 10. 与 ICanHardwareAdapter 的对应关系

| 现有接口 | libTSCAN 候选 API/动作 | 待确认点 |
| --- | --- | --- |
| `initialize()` | 未来加载/解析 ABI 后调用 `initialize_lib_tscan` | DLL查找策略、单例/引用计数、版本检查 |
| `release()` | 逆序清理后 `finalize_lib_tscan` | 多实例和异常安全 |
| `enumerate_devices()` | `tscan_scan_devices` + `tscan_get_device_info` | 是否必须先初始化；字符串所有权 |
| `open_device(id)` | `tscan_connect(serial, &handle)` + `tscan_get_can_channel_count` | 返回码5、重复连接、热插拔 |
| `close_device()` | `tscan_disconnect_by_handle` | 在途接收/回调的停止顺序 |
| `configure_channel()` | `tscan_config_can_by_baudrate` 或 `tscan_config_canfd_by_baudrate` | 单位、通道基准、终端电阻、ISO FD |
| `start_channel()` | 本地状态转换；当前资料无明确独立启动 API | 需厂商确认配置后即启用的语义 |
| `stop_channel()` | 停止本地I/O；当前资料无明确独立停用 API | 是否只能断开设备来保证停用 |
| `send()` | `tscan_transmit_can*_sync/async` | 超时策略、DLC、回显 |
| `receive(timeout)` | `tsfifo_receive_can*_msgs` 轮询，或回调入队 | rx/tx参数、线程安全、取消和超时 |
| `status()` | 适配器自维护状态 + 最近厂商返回码 | libTSCAN是否提供可靠在线状态 API |
| `last_error()` | `tscan_get_error_description` 后复制文本 | 字符串所有权和编码 |

## 11. 后续实现 TosunCanAdapter 前仍需完成

1. 取得与实际 TOSUN 硬件型号、固件和 Windows 版本匹配的 SDK 版本说明/变更记录；
2. 取得可保存的许可证/EULA或书面集成与再分发许可，明确 DLL 能否随产品发布；
3. 向厂商确认 DLL 依赖项、部署目录、驱动要求、错误说明字符串所有权和线程安全；
4. 确认 `start/stop` 语义、终端电阻策略、通道编号、热插拔和多设备行为；
5. 解决 `READ_TX_RX_DEF` 枚举/注释矛盾，并验证发送回显过滤；
6. 设计 DLL 缺失、位数错误、导出缺失和版本不兼容时的可诊断失败路径；
7. 增加不需要 DLL 的结构/DLC/错误映射单元测试；
8. 经单独批准后，再做 DLL 加载验证、虚拟/实物设备冒烟测试和 CAN/CAN FD 收发台架测试；
9. 通过原始帧层验证后，复用现有 ISO-TP、UDS 和刷写测试，不引入 libTSCAN 诊断模块。

本阶段没有修改任何运行时代码、CMake、DLL搜索路径或 `dist` 内容。
