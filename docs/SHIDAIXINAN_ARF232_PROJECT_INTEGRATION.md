# 时代新安天王星、木星2代、庆铃接入说明

更新时间：2026-08-12

## 架构结论

三个项目不复制状态机，统一复用 `shidaixinan_hjzj_fmr` Workflow；客户/项目名称、默认文件和来源证据由三个独立 Profile 与资源目录管理：

```text
时代新安 ARF2.32 共用 Workflow
├─ 天王星 / FMR -> shidaixinan_tianwangxing_fmr.ini
├─ 木星2代 / FMR -> shidaixinan_muxing2_fmr.ini
└─ 庆铃 / FMR -> shidaixinan_qingling_fmr.ini
```

共用 Driver、SeedKey DLL 和 CRC 来源集中在 `resources/shidaixinan_arf232_common`。项目固件与证据分别保存在三个项目目录，避免状态机重复和不同客户固件混用。

## 当前可用边界

- 三个 Profile 已进入源码 Profile 目录，UI 可按“时代新安 -> 项目 -> FMR”选择。
- 独立候选发布目录 `dist-ui-latest_20260812_shidaixinan-three-projects` 已生成；
  8/8 CTest 和构建脚本内 SeedKey 已知向量检查通过。
- 可复现的文件数、哈希、测试结果和验收边界记录在
  `validation/2026-08-12_shidaixinan_three_projects_offline/README.md`。
- 三个 Profile 共用已实现的 CAN FD、APP/FT、S19 单段解析、CRC32、FMR.dll 预检和复位后重试能力。
- 默认 APP 文件为空；用户选中与当前 ECU 对应的 APP S19 之前不能开始刷写。这是故意的 fail-closed 设计，不使用现有 HJZJ APP 冒充三个项目的 APP。
- 公共盘中三份 PLS S19 已本地只读冻结，三者内容相同，均为 `0x000C0000 / 0x17C000`，但仍按项目文件名分别保留来源。
- PLS 文件描述 ECU 的来源状态/恢复场景，不作为 FT 的目标文件；FT 的目标仍是用户选择的正确 APP。

## 证据状态

| 项目 | 历史 APP | 历史 PLS/FT | 本次状态 |
| --- | --- | --- | --- |
| 天王星 | 日志有 `DownLoadOk`，对应 `SDTWXA/CHF0354N`；APP本体未冻结 | 主体完成后复位后 `10 03` 超时，旧工具记 `DownLoadError` | Profile/资源/离线契约接入；真机待验收 |
| 木星2代 | 日志有 `DownLoadOk`，对应 `SDMX2A/CHF0356N`；APP本体未冻结 | 主体完成后复位后 `10 03` 超时，旧工具记 `DownLoadError` | Profile/资源/离线契约接入；真机待验收 |
| 庆铃 | 日志有 `DownLoadOk`，对应 `SLAQLA/CHF0361N`；APP本体未冻结 | 主体完成后复位后 `10 03` 超时，旧工具记 `DownLoadError` | Profile/资源/离线契约接入；真机待验收 |

## 接入后仍必须完成

1. 分别取得并冻结三个项目实际使用的 APP S19，记录来源和 SHA-256。
2. 将 APP S19 的自动解析地址/长度、CRC32 与同版本成功日志逐项核对。
3. 分别运行 APP -> APP；保存当前二进制生成的报告和 ASC。
4. 分别在真实 PLS ECU 上运行 FT；验证复位后 readiness retry、APP上线、版本 DID 和清理结果。
5. 当前只能标记为“候选发布包离线验证通过”；真实 ECU PASS 仍须按项目和 APP/FT 入口分别记录，不能继承 HJZJ 的台架结论。

本次公盘仅只读，未修改共享源。
