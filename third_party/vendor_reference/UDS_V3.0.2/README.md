# UDS_V3.0.2 厂商参考快照

来源：`D:\project\UDS_tools\UDS_V3.0.2`

该目录仅用于后续多品牌 CAN 硬件适配研究，不属于当前 UDS_TOOLS
运行时。原始 68 个文件已按原目录结构复制，文件大小、SHA-256 和 PE
位数记录在 `SOURCE_MANIFEST.sha256` 中。

约束：

- 当前构建、安装和打包流程不得链接、加载或复制本目录内容到 `dist`。
- 当前唯一可用实现仍是系统 Vector Driver 提供的 x64 `vxlapi64.dll`。
- 本目录中的 x86 `vxlapi.dll` 不能替代当前 Vector 运行时。
- `TSMaster.dll` 和 `Interop.TSMasterAPI.dll` 仅作历史参考。后续 TOSUN
  适配必须使用厂商官方 `libTSCAN` 直接访问硬件，不依赖 TSMaster
  上位机进程。
- ZLG 后续根据确定的设备型号和官方 SDK 单独实现。
- 许可证文件、算法 DLL、EXE 和 Qt 运行库均按原样留存，但本项目不对其
  可用性、授权状态或再分发权作保证。

正式接入任何厂商前，应先确认 SDK 版本、位数、许可证和再分发条款，再在
`src/drivers/can/<vendor>` 与 `third_party/<vendor>` 中独立实现和管理。
