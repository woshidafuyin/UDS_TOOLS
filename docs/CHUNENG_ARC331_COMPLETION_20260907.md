# 楚能 ARC331 失败后完成收尾：2026-09-07

## 行为与范围

用户要求与 CANoe 一样：最终依赖性检查失败后继续复位和诊断恢复，流程到结尾仍显示 FAIL，列出失败步骤、响应及简单原因。

仅在 APP CheckMemory 0202 已通过后启用收尾策略。DependencyCheck、ECUReset、ClearDTC 的最终 NRC、例程失败或响应超时被累计；收尾全部尝试后输出 100% / WARNING，并通过原控制器保留总体 FAIL。正常流程仍 PASS。取消和适配器/传输异常仍中止，不宣称完成。前面的安全解锁、擦除、传输、APP 校验门限不变。

复位后功能寻址的 10 83、85 81、28 80 03、10 81 沿用原流程的抑制正响应发送方式，只能确认发送，不将无响应记为已确认通过。

流程策略在 Chuneng331Flow；workflow 将 PASS/FAIL/WARN 写入报告；Qt 在完成但失败时显示 warning 和最终红色失败。无新增库或反向依赖。UI 沿用现有文字结果接口识别完成提示，后续若统一结果模型可改为结构化完成状态；本次未扩展所有厂商接口。

既有 c857_bench_validation 增加可选 --profile / --target，保持原默认值。实测复用正式 FlashController/flow，并增加目标、资格和 ASC/BLF 证据；未另外实现刷写协议。

## 构建与离线验证

- Windows MSVC x64 Release，build/nmake-x64：构建通过。
- CTest 9/9 PASS，97.47 秒。记录：validation/2026-09-07_chuneng_completion/CTest.log。
- 新增 6 个收尾场景：全部通过、FF01 失败、多个 NRC 失败、取消、设备断连、FF01 超时。
- Qt 回归确认完成后的 warning 与最终 FAIL 同时保留。
- 正式 dist/CH_Diagnostic_Studio.exe 已更新；旧程序保存在 build/CH_Diagnostic_Studio.before_chuneng_completion.exe。未清空 dist 或历史日志。
- 正式 EXE SHA256：9A374348799F49A5A9015904C52B98C57E02B71B4468B27E7FB17636A81BF520。

## 真实 ECU 验证

用户明确授权真实测试。2026-09-07 16:03:10–16:04:29，仅一次 APP 刷写，左后雷达，Vector XL CH2，500k/2M，TX 0x72E / RX 0x72F / functional 0x7DF。保持台架供电，未操作 CANoe 或更改 CBF。

输入来自正式 dist/resources/chuneng_d7_arc331_zip：

| 文件 | SHA256 |
|---|---|
| CBF/Driver/driver_712345678AB.cbf | A3D4B9A5323FDA405400712FA5E46D8CD0CB1D9AD218AEBCC35008716FB933C3 |
| CBF/APP/7052A5023002AB.cbf | 3EEF5C26084570BD9B1E7C5430025A2A5ED307AEE955793BCC35CC05C8278205 |

运行命令（会实际刷写，本文仅记录已执行命令）：

```powershell
./build/nmake-x64/Release/c857_bench_validation.exe --flash --profile chuneng_331_left_rear --target left_rear --dist D:/project/UDS_tools/dist --log D:/project/UDS_tools/validation/2026-09-07_chuneng_completion/real_flash.log
```

| 步骤 | 实际结果 |
|---|---|
| Driver 校验 / SBL 激活 | 71 01 02 02 04 / 71 01 03 01 04 |
| APP 传输与校验 | 768 块全部应答；71 01 02 02 04 |
| DependencyCheck | 16:04:20.817，71 01 FF 01 05，FAIL：依赖性/兼容性例程执行失败 |
| ECUReset | 16:04:20.819，11 01 → 51 01，PASS |
| 恢复扩展会话、DTC、通信、默认会话 | 16:04:26.849–27.101，均已发送；抑制正响应 |
| ClearDTC | 16:04:27.152 发送 14 FF FF FF，16:04:29.153 最终响应超时，FAIL |
| 最终 | 100%，WARNING 同时列出 DependencyCheck 和 ClearDTC；总体 FAIL，控制器正常退出 |

ASC 相对时间独立确认：70.100963 收到 FF01/05；70.102962 收到 51 01；76.130686/76.181381/76.281691/76.382387/76.432703 秒依次发出 10 83/85 81/28 80 03/10 81/14 FF FF FF。实测与本次行为要求相符。

## 复位后状态与证据边界

随后使用 version_check_bench_validation 读取同一目标：10 01 → 50 01，5 项读取成功 4 项。

- F180：7052A5023003AB。
- F195：1.00.00。
- F189：7052A5023002AA，与本次 CBF 的 AB 标识不一致。
- F193：1.00.00。
- F187：7F 22 31，请求超出范围。

这是诊断仍可响应的证据；不能证明 AB APP 已运行或雷达功能验收通过。FF01 的 05 是例程返回的失败状态，不是 7F NRC 拒绝；其内部具体根因无法仅凭这 5 字节确定。ClearDTC 超时也不能断言为 ECU 明确拒绝。没有为了获得 PASS 而屏蔽这些失败。

## 证据路径与校验值

固定报告：D:/project/UDS_tools/dist/logs/reports/report_20260907_160429_226.html。

完整本地证据：D:/project/UDS_tools/validation/2026-09-07_chuneng_completion/，含 probe.log、real_flash.log、real_flash.asc、real_flash.blf、固定 HTML 副本、post_reset_versions.log、CTest.log、evidence_sha256.json。大报文文件按既有规则保留本地，不加入 Git。

| 证据 | SHA256 |
|---|---|
| real_flash.asc | 24DDA529DAD8177729CBA9D38FAFA7B9A4A91882A8995B2A85A9FAD8CE37C6A9 |
| real_flash.blf | C031A0D4250C5651665F3272FEE2E273B8EB5AA1B84B65F7494172C8DD92F4BE |
| report_20260907_160429_226.html | 627C0E6846CF56E9A08D216B596C6CA885E8279EE9A80BD5D629C0DACF7BC9F5 |

原工作区 validation/2026-08-19_flow_fix_backup 和 validation/2026-08-24_chuneng_arc331_left_rear_app_pass 下的既有删除项不属于本次变更，未提交。未推送远端。
