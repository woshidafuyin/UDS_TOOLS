# 北汽 BQB41 运行资源来源

- 流程、端点、布局与安全访问已知向量：
  `客户项目测试工程/北汽/Flash_BQB41_V1.1.7z` 中成功 BLF。
- 默认 SeedKey：
  `D:/project/CANoe刷写验收工程/06_零跑旧角雷达_CANoe/工程本体/CDD/SeednKey_北汽加特兰.dll`。
- DLL SHA-256：
  `C619670F5198993FF62EBF6212A55D0882DF4333CD7B7049104B486E1545169D`。
- 兼容性验证：该 DLL 对成功 BLF 的 Seed `1BFE6E44` 计算结果为
  `52ACA070FE2639A97AF7DD5B57D88BAD`，逐字节一致。

BQB41 归档未附 Driver/APP 固件，运行时必须手动选择与目标硬件匹配的 S19。
已知答案验证和离线流程复刻不等同于当前 ECU 台架 PASS。
