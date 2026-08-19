# 时代新安 HJZJ_FMR 资源

运行资源来自 `D:\project\shidaixinan`，用于时代新安 FMR 主雷达
“客户刷客户”APP 正常刷写与“PLS 刷回 APP”FT 恢复刷写。

- `Driver`、`APP`：工具运行时解析 S1/S2/S3 数据记录，自动取得起始地址与长度。
- `dll\FMR.dll`：32 位 Vector `GenerateKeyEx` 安全算法，由 x86 `keygen_broker.exe` 调用。
- `integrity\HJZJ_CRC32.dll`：原工具来源留档。新工具使用等价的标准 CRC-32/ISO-HDLC 实现，已由 Driver/App 两个成功报文 CRC 交叉验证。
- `config\HJZJ_FMR.ini`：APP 正常刷写原流程留档。
- `config\HJZJ_FMR_PL.ini`：PLS 状态下从物理 `10 02` 开始的恢复主体及刷后清理原流程留档。
- `CDD`：原诊断定义来源留档。

FT 入口不直接取自 INI：依据完整 CANoe ASC，工具先执行功能寻址
`0x7DF -> 0x761` 的 `10 03`，再发送不等待正响应的功能 `10 02`，并在
4 秒窗口内切换到物理 `0x7A4 -> 0x7AC` 的 `10 02`。进入编程会话后复用
Driver + APP 主体；本模式不选择或下载 PLS 文件。

当前 FT 状态为代码实现及离线协议/UI 回归通过，尚未完成通用工具的真实 PLS
板端首刷验收。

文件大小与 SHA-256 见 `SOURCE_MANIFEST.sha256`。
