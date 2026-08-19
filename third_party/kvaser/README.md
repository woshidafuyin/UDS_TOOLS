# Kvaser CANlib SDK boundary

## 来源

- 官方文档：<https://kvaser.com/canlib-webhelp/>
- 官方 SDK 页面：<https://kvaser.com/developer/canlib-sdk/>
- 本机 SDK：`C:\Program Files (x86)\Kvaser\Canlib`
- 本机 SDK 版本：`5_46_843`
- 导入日期：2026-07-28

`sdk/include/` 仅保存构建 `KvaserCanAdapter` 所需的官方 CANlib 头文件，文件哈希见
`SOURCE_MANIFEST.sha256`。

## 运行时

项目通过运行时动态加载 `canlib32.dll` 使用 CANlib。Kvaser 在 Windows 上对 x86
和 x64 API 都使用该 DLL 名称；x64 程序加载 `System32` 中的 x64 版本。加载顺序：

1. `UDS_KVASER_DRIVER_DIR`；
2. `<exe>/drivers/kvaser/`；
3. 可执行文件同目录；
4. `C:\Windows\System32\canlib32.dll`。

不把系统驱动 DLL 复制进发布包。目标电脑应先安装官方 Kvaser Windows Driver；
SDK 本身只用于编译。

## 适配器边界

- CANlib 全局通道会混合物理与 Virtual CAN。适配器依据
  `canCHANNELDATA_CARD_TYPE` / `canCHANNELDATA_CHANNEL_CAP` 分类，保持各组原始
  顺序并将物理通道排在虚拟通道之前；
- UI 的 1-based `Channel N` 映射到上述逻辑目录；枚举日志同时输出 CANlib 全局
  索引、设备板载通道、序列号和 EAN，避免插入实物后误选 Virtual CH1；
- 支持 Classic CAN、ISO CAN FD、扩展帧、BRS、同步发送与限时接收；
- 终端电阻不由本项目自动修改；
- 虚拟 CAN 通道允许用于离线开发，但不能视为物理硬件验收。
