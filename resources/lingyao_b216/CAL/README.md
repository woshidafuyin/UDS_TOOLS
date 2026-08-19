# CAL 刷写资源

本目录用于铃耀 B216 ARS1.31 的 CANoe `Protocol=CAL` 与
`Protocol=APP+CAL` 复刻流程：

- 主雷达 ICRF：`ICRF_002_003.s19`
- 从雷达 ICRR：`ICRR_001_003.s19`
- 下载窗口：`0xC0180000 / 0x270`

CAL 模式执行 `Driver + CAL`；APP+CAL 模式执行
`Driver + APP + CAL`。两段内容分别执行擦除、下载、TransferExit CRC
校验和 `31 01 FF01` 依赖检查。
