# 奇瑞 ARS1.33 APPAndCAL（TC_2）流程对齐

## 基准与边界

- 基准工程：`D:\project\10_客户项目\qirui\ARS1.33_qirui\FlashChery_KP31_V1 .0_20260701_2`
- 权威入口：`Capl\Flash_ARS1.33.can::maintest()` 中 `APPAndCAL == 1` 分支调用的 `TC_2()`
- C++ 流程 ID：`chery_ars1_33`
- Profile：`profiles\chery_ars1_33.ini`
- CANoe 现场基准已经以 Driver、DriverData、APP、APPData、CAL、CALData 六个文件完成一次
  `APPAndCAL / TC_2` 正常刷写；该结果是复刻依据，不代表重新构建后的 C++ 程序已经完成台架验收。
- 当前只覆盖正常 APPAndCAL 流程，不包含校验数据破坏 Case，也不执行可选的 F600 公钥更新。

## 通信、前置报文与内存参数

| 项目 | 值 | 来源 |
| --- | --- | --- |
| 总线 | Classic CAN 500 kbit/s | CANoe cfg / CAPL 8 字节手工 ISO-TP |
| 从雷达物理请求 / 响应 | `0x6C4 / 0x6C5` | cfg DiagnosticsSettings、DBC `FT_PhyID/FT_ResID` |
| 主雷达物理请求 / 响应 | `0x71F / 0x79F` | 2026-08-12 用户指定；已接入 UI/Profile，待台架验证 |
| 功能请求 | `0x7DF` | CAPL `gFunId`、cfg DiagnosticsSettings |
| ISO-TP padding / FC | `0x55` / `BS=0, STmin=0` | CAPL `TxMsgSrever()`、`output_FC()` |
| FLD block0 | `0x00499000 / 0x10` | DBC、CAPL `FileInit()` |
| FLD | `0x0049C038 / 0x1EB8` | DBC、CAPL `FileInit()` |
| APP | `0x000C1000 / 0x7B000` | DBC、CAPL `FileInit()` |
| CAL | `0x000B0000 / 0x200` | 面板配置、CAL S19、CAPL `TC_2()` |
| 安全访问 | `27 11 / 27 12`，16 字节 Seed/Key | CAPL `server_27()`、CDD |
| 完整性数据 | FLD、APP、CAL 各 512 字节 RSA | CAPL `driverData()`、`fileData()`、`CALData()` |

刷写开始前必须在与诊断相同的 Vector 物理通道发送以下两条应用报文，并在整个流程中持续周期发送：

| CAN ID / 报文 | 信号与物理值 | 周期 | 完整 8 字节 payload |
| --- | --- | --- | --- |
| `0x25B` / `FLZCU_2` | `FLZCU_2_PowerMode = ON`，raw `2` | 20 ms | `00 00 02 00 00 00 00 00` |
| `0x4B4` / `VCU_1_G` | `VCU_PRNDGearAct = P`，raw `1` | 100 ms | `00 00 00 00 00 00 00 10` |

用户口述的“484 报文”应按截图和
`EEA5.1 SCIR Message list V3.60 Draft_202606051338_international.dbc` 修正为 **`0x4B4`**；
DBC 中没有 `0x484`。两条报文均为 11-bit Classic CAN、DLC 8，每个报文只有上述一个信号，
没有 CRC 或 Alive Counter。前置报文必须直接走原始 CAN 发送，不能经过 ISO-TP，也不能使用
诊断 padding `0x55` 填充未用字节。

Profile 中的 `channel=1` 是 C++ Vector 物理通道默认值，运行前必须按实际接线修改；它不是 ECU 固有属性。

## 正常 APPAndCAL 顺序

1. 先同步发送一帧 `0x25B` 和一帧 `0x4B4`，确认两帧已提交，再启动 20/100 ms 周期发送；
2. 等待 1 秒，物理寻址发送 `10 03` 进入扩展会话，执行 `31 01 02 03` 编程前置条件检查，
   再以物理寻址发送 `10 02` 进入编程会话；
3. `27 11` 获取 16 字节 Seed，调用 x86 `GenerateKeyEx`（等级 `0x11`、空 Variant），
   `27 12` 发送 16 字节 Key；
4. `2E F184` 写 19 字节指纹：当前日期 BCD 三字节，后续 16 字节为 `FF`，随后等待 2 秒；
5. FLD block0：`34/36/37`，地址 `0x00499000`，长度 `0x10`；
6. FLD：`34/36/37`，地址 `0x0049C038`，长度 `0x1EB8`；
7. 等待 2 秒，发送 `31 01 DD 02 + 512-byte FLD RSA`，要求例程状态 `00`，再等待 2 秒；
8. `31 01 FF 00 44 + APP地址/长度` 擦除 APP，APP 执行 `34/36/37`；
9. 等待 2 秒，发送 `31 01 DD 02 + 512-byte APP RSA`，要求例程状态 `00`；
10. `31 01 FF 00 44 + CAL地址/长度` 擦除 CAL，CAL 执行 `34/36/37`；
11. 等待 2 秒，发送 `31 01 DD 02 + 512-byte CAL RSA`，要求例程状态 `00`，再等待 2 秒；
12. `31 01 FF 01` 检查编程依赖，要求例程状态 `00`；
13. `11 01` 硬复位，等待 2 秒，功能寻址发送 `14 FF FF FF` 并要求 `54`，再等待 2 秒；
14. 无论成功、失败还是用户停止，退出流程时都停止两条前置周期报文。

`TC_2()` 不发送 `3E 80` 周期 TesterPresent，也不发送 APP-only `Download()` 使用的功能寻址
`10 83 / 10 81`，C++ APPAndCAL 流程保持一致。CAPL 的校验破坏开关会改写 RSA 数据，
正常 C++ 流程不启用这些分支。

## 六个面板文件与打包资源

| 面板字段 | 文件 | SHA-256 |
| --- | --- | --- |
| Driver | `ARS1.33_702000275AA_S0000054588_FLD_020001.s19` | `9EE3D0F0D00429563CD7F179B8FD9408FDED90D92FC0CCDA2485C55AC2ECAA6A` |
| DriverData | `6a4e11d2ebc8c_CIR_S0000054588_020002_FLD1_MCU_UDS_20260622.rsa` | `0CAD07F075CC6793FCE24BC05790D8B60445CDCBD715E963C7C27542CEB61056` |
| APP | `ARS1.33C3A_AF2T3R_B1.0.00_APP_V02.00.02C_CHF0376N_without_boot.s19` | `E07B3785242F266227541F325AEB033E408E3CD52459BC914F25F296D9B0FB62` |
| APPData | `6a4e11f5384ca_CIR_S0000054589_020002_ASW1_MCU_UDS_20260706.rsa` | `BE4C6E4D0EF119ADF9CA65F79FACFC633AC1DEC05497CEBCFA735A688005219C` |
| CAL | `ARS133_Cail_20260605_S0000054590_02.00.01.S19` | `9D77482655256007541D725BC829142D6A211FB147817C28A2A7287C5930DDDB` |
| CALData | `6a4e11fa3ff68_CIR_S0000054590_020001_CAL1_MCU_UDS_20260622.rsa` | `67B5C8474AD848D6968205E74432AA8324B7CB86648F5C7043E4558454E0DD6E` |

CAL S19 与 CAL RSA 来自基准工程的 `shuaxiewenjian\Cail`。CAL S19 打包在
`resources\chery_ars1_33\CAL`，CAL RSA 与其余校验文件统一打包在
`resources\chery_ars1_33\Verification`。

`CHERY_E0Y_UPDATE23231115.dll` 已确认是 x86 DLL 并导出 `GenerateKeyEx`；离线 ABI 烟雾测试以
16 字节零 Seed、等级 `0x11`、空 Variant 成功返回 16 字节 Key。

## 验收状态

CANoe `Flash` Test Module 的 `APPAndCAL / TC_2` 已在参考工程完成一次 Passed 现场刷写，证明上述
文件组合和前置总线条件有效。C++ 侧的资源解析、协议编码和 Release 构建仍属于离线验收；必须对本次
重新构建的 EXE 在同一台架执行完整 APPAndCAL 刷写并冻结 EXE、Profile、资源、Trace 和报告哈希后，
才能宣称 C++ ARS1.33 流程通过实车/台架验收。
