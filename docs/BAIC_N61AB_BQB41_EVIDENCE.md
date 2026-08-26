# 北汽 N61AB / BQB41 刷写实现依据

## N61AB

来源为 `客户项目测试工程/北汽/71_北汽N61AB_ARS1.31.7z`。归档包含
可读 `Flash.can`、Driver/APP S19 与标准 Vector SeedKey DLL，因此实现为可执行
Profile，并在 CAN 打开前执行文件窗口、长度和 SeedKey 已知答案预检。

## BQB41

来源为 `客户项目测试工程/北汽/Flash_BQB41_V1.1.7z`。主 CAPL 已加密，
但成功 BLF 固化了完整正常 Download 状态机、四组诊断 ID、CAN FD 帧格式、
0x00000000/0x400 Driver 和 0x00040000/0x80000 APP 布局，以及示例
`Seed 1BFE6E44 -> Key 52ACA070FE2639A97AF7DD5B57D88BAD`。

归档没有固件，且所带 `capldll*.dll` 是 CANoe CAPL 扩展接口，不是通用工具
可调用的 `GenerateKeyEx` ABI。本机 CANoe 验收资料中的
`SeednKey_北汽加特兰.dll` 对上述成功 BLF 已知向量给出完全一致的 16-byte Key，
因此作为 BQB41 默认算法资源，并在每次 CAN 访问前重复该已知答案测试。Driver/APP
仍由用户手动选择；不得把离线流程复刻描述为真实 ECU 刷写 PASS。

## 架构边界

两项目共用 `BaicRadarFlow`；项目差异仅由 `BaicRadarProjectSpec` 和 Profile
承载。零跑、长马等已有 Workflow 不被复用，避免把其他客户的地址、验签或恢复
入口假设带入北汽项目。

`0202` 使用归档 CAPL 的 CRC32 变体：初值 `0xFFFFFFFF`、反射多项式
`0xEDB88320`、无最终异或。不能替换成常见 ZIP CRC32 的展示值。

执行期间按 CANoe 周期发送功能寻址 `3E 80`：N61AB 使用 8-byte Classic CAN
和 `0x55` 填充，BQB41 使用 64-byte CAN FD+BRS 和 `0x00` 填充。

## 版本读取依据

版本读取 DID 来自研发公盘客户诊断规范，而不是从其他项目套用：

- N61AB：`\\njfilesrv\E研发\E000_项目管理\04_CUSTOMER_PROJECTS\71_BJEV_N61AB_ARS1.31_30093\07_Specification\01_Customer\02_诊断及刷写相关\01_诊断\N61AB-M19车型-生命体征检测系统（CPD）诊断规范 001-20231121.xlsm`
- BQB41 对应公盘项目名 B41V：`\\njfilesrv\E研发\E000_项目管理\04_CUSTOMER_PROJECTS\36_北汽_B41V_ARC2.61\07_Specification\01_Customer\02_诊断相关`

两项目在默认会话读取 `F187`、`F183`、`F18A`、`F191`、`F195` 作为必读版本/身份项。
其他规范中定义的系统 DID 作为可选项；ECU 不支持时只产生 WARN，不影响必读项结论。
`F183`、`F184` 以及日期/配置类二进制字段保留原始十六进制显示，避免将 BCD 或位域
误当成 ASCII。BQB41 四组物理请求/响应 ID 与 B41V 左后、右后、右前、左前规范逐一匹配。
