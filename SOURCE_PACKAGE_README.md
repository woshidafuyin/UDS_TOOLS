# UDS Tool C++ 通用刷写工具源码包说明

更新时间：2026-08-19

本文件只说明 `D:\project\UDS_tools\UDS_tools` 当前源码、候选运行目录和验证边界。

## 当前基线

- 工程统一构建通用版 `uds_tool_qt.exe`，不提供客户独立版本构建入口；
- 当前综合候选运行目录：`dist`；
- 当前源码配置 19 个项目 Profile、16 个注册 Workflow ID；
- 19 个 Profile 均包含 `[version_check]`，合计 131 个读取项；零跑 A12EV 为占位项目，运行能力禁用；
- 当前工程根目录没有对应的最新源码 ZIP，因此不登记或虚构源码压缩包名称。

## 当前已接入项目

```text
楚能 ARC331
奇瑞 ARS1.33
奇瑞 KP31
奇瑞 E0Y
奇瑞 T22
奇瑞 T1EJ
长安 C857
长安 B216
长马 J90K / ARS1.31
犀重 RSMR
犀重 LSMR
时代新安 HJZJ FMR
时代新安 天王星 FMR
时代新安 木星2代 FMR
时代新安 庆铃 FMR
零跑 ARC
零跑 ARF631
零跑 A12EV（placeholder，运行能力禁用）
吉利 P416
```

时代新安天王星、木星2代、庆铃使用独立 Profile 和项目资源，复用 `shidaixinan_hjzj_fmr` Workflow 及共用 Driver/SeedKey 资源；三者默认 APP 为空，APP 和 FT 仍需按项目分别完成真实 ECU 验收。

## 构建与候选包

```powershell
scripts\build.ps1 -Config Release -DistPath dist
```

构建脚本完成 x86 SeedKey Broker、x64 主程序、CTest、Qt 运行库部署、安装和已知 SeedKey 向量检查。正式运行入口为：

```text
dist\uds_tool_qt.exe
```

2026-08-19 14:09 最近一次完整离线验证快照：

- Release 构建：PASS；
- CTest：8/8 PASS；
- Qt 主窗口默认 10,000 次随机操作回归：PASS；
- 长马、犀重、长安 C857/B216、楚能、奇瑞 KP31、时代新安、零跑 SeedKey 已知向量：PASS；
- `dist\uds_tool_qt.exe` SHA-256：`FF3479055F0EFA09D2809D554F12D3C4F7ACD4C85D32403F73E45E8B533FACEA`。

以上结果来自 14:09 完成的工作区构建输出，不把 2026-08-12 的时代新安专项验证材料合并为同一次验证证据。该验证结束后，`src/app/probe_service.cpp` 与 `src/flash/lingpao_radar_flow.cpp` 于 14:14 更新；两项最新源码尚未重新构建和执行完整 CTest，因此当前 `dist` 是最近一次已验证快照，不是这两项修改后的新候选包。

## 源码包应包含

```text
src
tests
profiles
resources
assets
third_party
scripts
docs
validation/2026-08-12_shidaixinan_three_projects_offline
CMakeLists.txt
.gitignore
README.md
SOURCE_PACKAGE_README.md
```

`validation` 中的日期目录只代表对应日期、对应范围的验证记录；不能用旧记录证明当前 EXE 已重新进行真实 ECU 执行。

## 源码包应排除

```text
build
所有 dist* 运行目录
backup_*
tmp
logs
历史 ZIP
历史 SHA-256 旁车文件
空的 packaging 目录
```

排除项是可重新生成的构建输出、独立运行包、临时文件、历史归档或不再使用的空目录，避免源码包递归嵌套和体积膨胀。

## 当前验证边界

- 未在本次文档更新中启动 CANoe；
- 未连接 CAN，也未发送 UDS；
- 未执行本轮真实 ECU 版本读取或刷写；
- Profile 已配置 DID 不等于诊断调查表已经确认含义；
- 已接入不等于已实板验收；项目、设备及 APP/FT/CAL 入口的验收结论必须分别记录，不能互相继承。

当前 Profile、Workflow、文件格式、在线探测、版本读取、刷写和发布目录的实现说明统一维护在根目录 `README.md`；版本读取配置契约见 `docs/VERSION_READ_CONFIGURATION.md`。
