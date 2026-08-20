# LP-A12EV source archive provenance

- Source project: `D:\project\CANoe刷写验收工程\LingPao_A12EV_Flash_V1.0_20241202`
- Source package: `\\njdatasrv\测试部公共盘\01_软件测试组\02_项目管理\客户项目测试工程\零跑\LingPao_A12EV_Flash_V1.0_20241202.7z`
- Copied files: `FlashDriver.srec`, `CDD\lingpao_SeednKey_cdd.dll`, and `CAPL\Flash20251103.can` as protocol reference.
- Driver SHA-256: `025AB1FAAD2FB053FC1198D22DED08AA8CF3154DDA44FF87882973BCA5BEB708`
- SeedKey DLL SHA-256: `7F7A67365D7A54A9985C41B79C48F01C37A6595422E67B24DA54B2491A77D020`
- The source project references its APP S19 and ASC certificate through external absolute paths. They were not present in the package.
- The runtime project entry has been merged into `profiles\lp_arc.ini`. This directory remains source-only provenance and is not copied into generated runtime packages; the merged ARC entry uses the reviewed `resources\lp_arc` preset Driver, APP, ASC and SeedKey DLL.
