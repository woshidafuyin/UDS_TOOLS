# ZLG / ZCAN SDK

## 来源与目标硬件

- 官方产品页：<https://www.zlg.cn/index.php/can/can/product/id/223.html>
- 官方资料页：<https://www.zlg.cn/can/down/down/id/223.html>
- 官方开发库：<https://www.zlg.cn/data/upload/software/Can/CAN_lib.zip>
- 下载日期：2026-07-28
- 首个实机目标：`USBCANFD-200U`，ZCAN 设备类型号 `41`，2 路 CAN FD。

原始 ZIP 永久保留在 `packages/CAN_lib.zip`，解压原样保留在 `sdk/`。文件哈希见
`SOURCE_MANIFEST.sha256`。

## 项目接入

`ZlgCanAdapter` 通过运行时动态加载 `zlgcan.dll`，不静态链接 import library。
默认加载顺序：

1. 环境变量 `UDS_ZLG_DRIVER_DIR` 指向的目录；
2. 可执行文件旁的 `drivers/zlg/zlgcan.dll`；
3. 可执行文件同目录的 `zlgcan.dll`。

CMake x64 构建会把 `sdk/zlgcan_x64/` 原目录复制到
`<exe>/drivers/zlg/`，保留 `kerneldlls/` 与设备属性文件的官方相对布局。
x86 构建使用 `sdk/zlgcan_x86/`。

当前实现支持：

- `ZCAN_OpenDevice` / `ZCAN_GetDeviceInf` 设备发现与连接；
- ISO CAN FD；
- 常用仲裁域、数据域波特率；
- Classic CAN 与 CAN FD/BRS 原始帧收发；
- 1-based 项目通道到 0-based ZCAN 通道的转换；
- `zlg:<device-type>:<index>` 设备标识；空标识默认
  `USBCANFD-200U(type=41,index=0)`。

安全默认值：

- 内置 120Ω 终端电阻不使能；
- 正常发送模式，不启用周期发送或队列发送；
- 发送超时 100 ms；
- 项目现有 ISO-TP、UDS、探测和刷写逻辑不迁移到厂商高层协议 API。

## 许可证

官方 ZIP 内的 `zlgcan License.txt` 允许源代码和二进制形式再发布，但必须保留版权
声明、条件和免责声明，且不得未经许可使用 ZLG 名称为衍生产品背书。构建输出复制运行
库时同时复制该许可证。
