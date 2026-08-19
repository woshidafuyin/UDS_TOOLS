# 楚能 ARC331 刷写失败分析 + 流程修正记录（2026-08-19）

## 一、16:03 刷写失败根因（execution_20260819_160333_866.log + trace_20260819_160339_*.asc）

用户台架（左后雷达，TX 0x72E / RX 0x72F）在旧版 `dist\uds_tool_qt.exe`
（14:16 构建）上刷写失败：

- 旧 exe 内置的是零跑 LP 流程（`lp_arc_workflow`）：`22 F197/F150/F189`、
  `31 01 60 00` 1322 字节证书例程等；
- 用户 ECU 为楚能正式版（F197=`LeRR`，会话信息 `00 32 01 5E`，16 字节
  SeedKey，楚能 DLL 已通过 `27 11/27 12`）；
- 失败点：`31 01 60 00 CertificateDownload` → **NRC 0x31（请求超出范围）**。
  楚能 ECU 按官方规范 Q/CN A201-2025 不支持 LP 的 6000/6001 证书例程。

## 二、流程依据（三份证据）

1. 官方规范 **Q/CN A201-2025《BootloaderOnCAN(FD)刷新规范》**（规范目录
   `_analysis_text\BootloaderOnCAN(FD)刷新规范.txt`）：
   - 预编程：物理 `10 03` → 物理 `31 01 02 03`（0x04）→ 功能 `10 83`、
     `85 82`、`28 83 03`（抑制响应）→ 物理 `10 02`；
   - 编程：`27 11/27 12`（16 字节种子/密钥）→ Driver `34/36/37` →
     `31 01 02 02`（256 字节签名）→ `31 01 03 01` 激活 SBL →
     `2E F1 84` 指纹（9 字节，5.4.5 要求激活 SBL 后写入）→
     `31 01 FF 00` 擦除 → APP `34/36/37` → `31 01 02 02` →
     `31 01 FF 01` → `11 01` 复位；
   - 后编程：功能 `10 83`、`85 81`、`28 80 03`、`10 81`、`14 FF FF FF`；
   - **规范无 6000/6001 例程**。
2. 参考 CANoe 工程 `LP_ARC331\ARC`（Flash20230727.can）：零跑/宝腾 ARC331
   工程（0x772/0x77A、4 字节密钥、0x771、F198/F199、6000/6001、0202+CRC32），
   `Download_File.Ini` 指向 perodua Q01A 项目，不是楚能正式流程。
3. 实机 BLF（Logging2026-08-17/18，用 scripts\analyze_blf_summary.py 解析）：
   CANoe 在 LP 台架 ECU（F197=`LP-BSD0C0`、4 字节密钥）上完整刷写成功，
   含 6000/6001 与 0x771；与用户 ECU 行为不同，不能照搬。

## 三、本次修改（修改前已备份，备份文件即本目录）

| 文件 | 修改 |
|---|---|
| `src\flash\chuneng_331_protocol.hpp` | 新增 `kChuneng331ExtendedSessionRequest{0x10,0x03}` |
| `src\flash\chuneng_331_flow.cpp` | 预编程顺序对齐规范附录 C（物理 10 03 → 物理 31 01 02 03 → 功能 10 83/85 82/28 83 03 → 物理 10 02）；`2E F1 84` 指纹移到 `31 01 03 01` 激活 SBL 之后、`31 01 FF 00` 之前 |
| `tests\core_tests.cpp` | 补充 `kChuneng331ExtendedSessionRequest` 常量断言 |
| `docs\CHUNENG_331_FLOW_PARITY.md`、`README.md` | 同步流程描述与发布记录 |

## 四、构建与验证结果

- Release 构建成功；CTest **8/8 通过**（含 qt_main_window_tests 10,000 次随机操作回归）；
- 全部 SeedKey 已知向量 PASS（楚能 16 字节 `FFD1FC2E...`、长马、犀重、C857/B216、
  KP31、时代新安、LP-ARC/ARF）；
- 楚能双 CBF 输入哈希匹配：Driver `A3D4B9A5...`、APP `3EEF5C26...`；
- 退休 LP DLL（66272f124ced1_lingpao_SeednKey_cdd.dll）已从 chuneng 资源树删除；
- `dist\uds_tool_qt.exe` SHA-256：`AA9E6A27727243DB6E355B77AC2B99605285D6580AA3560A96D64DE792D8A770`。

## 五、仍需台架验证

1. 右后雷达 APP（0x72C/0x72D）；
2. 左后雷达 APP（0x72E/0x72F）；
3. 右后 FT（0x701/0x761）；
4. 左后 FT。
离线解析/构建/向量通过不能替代真实 ECU 台架验收；台架刷写时保留 ASC Trace、
执行日志与 HTML 报告。
