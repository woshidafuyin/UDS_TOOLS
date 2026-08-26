# 北汽 N61AB 运行资源来源

- 冻结来源：`客户项目测试工程/北汽/71_北汽N61AB_ARS1.31.7z`
- 流程依据：`BQN61ABPro/Flash(1)(2)/CAPL/Flash.can`
- 默认 APP：归档 `app/20240426` 下的 V1.2.00 `without_boot.s19`
- 默认 Driver：与 V1.2.00 APP 同目录的 `FlashDrv` S19
- SeedKey：归档 `CDD/SeednKey_bq(1).dll`

运行契约：Classic CAN 500 kbit/s，0x723 -> 0x72B，功能寻址
0x7DF；Driver 0x08000000/0x400；APP 0xC0080000/0xF5000；
SecurityAccess 0x11/0x12，4-byte Seed / 16-byte Key。

这些资料支持源码级流程复刻和离线预检，不等同于当前 ECU 台架 PASS。
