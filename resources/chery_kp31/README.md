# 奇瑞 KP31 刷写资源

来源：

`\\njdatasrv\测试部公共盘\01_软件测试组\02_项目管理\客户项目测试工程\奇瑞\KP31\Chery_KP31_Flash_V1.1_20260413.7z`

接入基线为压缩包内 `Capl\Flash.can::maintest()` 的三个正常分支：
`APP -> FileInit/Download`、`CAL -> TC_7`、`APPAndCAL -> TC_2`。诊断端点为
`0x70D -> 0x78D`，功能寻址为 `0x7DF`，Classic CAN 500 kbit/s。原工程
没有FT或PLS→APP分支。

压缩包没有包含可用于完整三模式发布的正式Driver、APP、CAL及三份RSA；INI中的
APP/Driver指向原开发机`D:\03_Chery\...`，CAL路径为空。因此Profile故意将
六个输入全部留空，使用者必须从界面选择当前版本的正式文件。流程会分别校验：

- Driver S19 必须完整覆盖 `0x08000000 / 0x400`；
- APP S19 必须完整覆盖 `0xC0080000 / 0xF5000`；
- CAL S19 必须完整覆盖 `0xC0180000 / 0xC8`；
- Driver、APP和CAL RSA必须各解析为512字节。

`Driver\FlashDrv.s19` 是 V1.1 压缩包自带的历史文件，但与 INI 指向的
`ARS1.31C3A_FlashDrv.s19` 名称不一致，故只作来源保留，未设为默认刷写文件。

`dll\CHERY_E0Y_UPDATE23231115.dll` 是压缩包内的 x86 `GenerateKeyEx` DLL；
其 SHA-256 与现有 ARS1.33 同名 DLL 一致，但两个项目仍保持独立资源目录。

`Reference` 保存本次复刻所依据的 CAPL、CANoe 配置和原始 INI，不参与运行时
刷写。CDD 只作诊断定义参考。

当前已接入APP、CAL、APP+CAL三种正常流程；异常Case不在通用工具正常模式范围。
CAPL中CAL/TC_7可选的`2E 6F00`公钥更新默认未启用，当前工具也不暴露该维护开关。
只有APP/Download分支在APP下载后省略`37 RequestTransferExit`；TC_2的APP和CAL、
TC_7的CAL均按原CAPL发送`37`。三种模式仍需分别实板验收。
