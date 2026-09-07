# 跨项目刷写输入闭环检查与修复

检查对象：D:/project/UDS_tools 的 23 份正式 Profile、18 个注册 Workflow，以及 Qt 文件选择、资源保存、参数持久化、模式切换、ControllerBridge/FlashRequest/FlashJob 链路。

本次针对“后端需要输入，但正常界面无法提供或传递”的缺陷。结论来自源码检查、Release 构建、离线单元测试和 Qt 回归；没有操作 CANoe、连接总线或执行 ECU 刷写。项目的实际固件、密钥和台架验收仍按原项目条件执行。

## 已修复问题

|问题|范围|修复|
|---|---|---|
|CRC 与测试仪身份是必需参数，但没有界面入口|Perodua P02C|新增项目参数对话框，保存后立即用于新任务，重启恢复；取消不写入|
|默认路径为空时，文件选择仍被资源替换函数拒绝|奇瑞 KP31、北汽 BQB41、犀重 LSMR、时代新安木星2代/庆铃/天王星|共享导入层生成 resources/项目ID/原文件名目标，创建缺失目录；同名不同来源保留独立副本|
|BIN 未确认地址与合法地址 0 无法区分|P02C|BIN 必须明确填写该角色地址；0 可以明确保存，溢出拒绝；S19 使用文件内分段地址与长度|
|APP Hash 是否必需依赖默认路径是否非空|犀重 RSMR/LSMR，实际缺口为 LSMR|按流程要求检查 Hash，缺失在界面明确提示|
|允许选择后端不接受的 Driver，项目切换可能恢复旧路径|零跑 ARF|禁用 Driver 输入和浏览，选择 ARF 时清空界面 Driver；不删除文件|
|允许混合 Driver CBF 与 APP S19 或反向组合|楚能 ARC331|前置提示必须同为 CBF 或同为 S-record 并配套校验文件|
|必需 ESS 文件错误提示仍叫 CAL|吉利 P416/P417/P611|提示使用 ESS，保留原有内部文件槽位兼容|
|报告快照仍按 DLL 记录安全算法输入|P02C|AES-CMAC 任务记录 OEM Key 文件路径，不记录密钥内容|

## 全项目检查表

所有项目都复用现有模式/文件选择、任务控制器和报告通道。固定协议常量及已有确认的下载窗口仍由 Profile 与 Workflow 约束；它们不属于需要用户每次手填的参数。空地址也不一律判为缺陷：S-record/VBF 自动解析和固定窗口两类流程分开判断。

|Profile 项目|注册流程|输入及模式核对结果|
|---|---|---|
|奇瑞 ARS1.33|chery_ars1_33|Driver、按模式 APP/CAL、配套 RSA、SeedKey 均有入口；固定双 Driver 窗口有配置|
|奇瑞 E0Y|chery_e0y|APP/CAL/APP+CAL、RSA、SeedKey 及原公钥更新入口保留|
|奇瑞 T1EJ|chery_t1ej|APP/CAL/APP+CAL 与对应 RSA 输入一致|
|奇瑞 T22|chery_t22|CAL 模式额外要求 APP RSA 的现有规则保留|
|奇瑞 KP31|chery_kp31|修复所有默认固件/校验路径为空时的导入；三种模式仅要求实际使用角色|
|北汽 N61AB|baic_n61ab|Driver/APP S19、DLL 入口齐全；已确认窗口由流程约束|
|北汽 BQB41|baic_bqb41|Driver/APP 空默认路径可导入；正常 APP 模式不暴露未支持的 CAL/FT|
|长安 C857|changan_c857|主/从目标、APP/FT、目标 DLL 及资源保持原有隔离|
|长安 B216|lingyao_b216|与 C857 共用实现但独立 Profile/资源，保持 APP/FT|
|长马 J90K|longma_ars1_31|主/从、APP/FT、Driver/APP/DLL 入口齐全|
|零跑 ARC|lp_arc|Driver/APP S-record、可选证书、SeedKey、APP/FT 规则保留|
|零跑 ARF|lp_arf|APP TMP 或现有支持的分离输入；不下载 Driver，已禁用误选|
|楚能 ARC331|chuneng_arc331|CBF 配对或 S-record+校验；新增混用前置拦截，原格式内容验证保留|
|吉利 P416|geely_p416|SBL/ESS/APP VBF、APP/PLS；内置安全算法不要求外置 DLL|
|吉利 P417|geely_p416|独立资源复用 P416 流程，ESS 提示同步修复|
|吉利 P611|geely_p416|独立资源复用 P416 流程，ESS 提示同步修复|
|时代新安 HJZJ|shidaixinan_hjzj_fmr|Driver/APP S19 自动取地址长度、DLL、APP/FT|
|时代新安 木星2代|shidaixinan_hjzj_fmr|修复 APP 空默认路径导入，复用同一流程|
|时代新安 庆铃|shidaixinan_hjzj_fmr|修复 APP 空默认路径导入，复用同一流程|
|时代新安 天王星|shidaixinan_hjzj_fmr|修复 APP 空默认路径导入，复用同一流程|
|犀重 HQ001 RSMR|xizhong_rsmr|Driver/APP/Hash/DLL，Hash 必需规则显式化|
|犀重 HQ001 LSMR|xizhong_lsmr|修复 Driver/APP/Hash 导入和必需 Hash 检查|
|Perodua P02C|perodua_p02c|Driver、APP/CAL、OEM Key 已有文件入口；本次补齐 CRC、身份与 BIN 地址的编辑/保存/传参闭环|

## 组织方式与兼容性

- `project_flash_settings` 负责项目参数读取、保存、校验和应用；Qt 对话框负责编辑，ControllerBridge 在任务开始前再次检查并填入 FlashProfile 副本，刷写流程不依赖 Qt 设置。
- `resource_file_store` 负责生成导入目标、写入资源与同名保护；窗口复用这一层，没有为六个受影响项目复制独立导入函数。
- P02C 设置按当前 Windows 用户及项目 ID 保存。未保存时以 INI 为默认，保存后以界面值为准；不与其他项目共享身份或地址。
- 既有 Profile 格式、Flow/Workflow 协议实现、目标资源选择及文件相对路径持久化保持兼容。没有把未知 CRC、身份或固件填成虚构默认值。
- 当前各流程输入要求仍在 Workflow 和 UI 两处表达；本次用全配置/模式回归约束差异。未来新增项目需要同步声明输入条件，统一能力描述是后续可维护性改进点。

## 回归证据

2026-09-07 Release 构建通过；完整 CTest **9/9 通过、0 失败**，耗时 70.28 秒。全配置完整输入检查实际覆盖 **53 个 Profile/模式组合**。使用 Windows 字体目录重新进行 Qt 离屏渲染，参数窗口文字和控件显示正常，该次 Qt 回归也通过。

- `qt_main_window_tests`：全 23 份配置空默认路径资源导入、原文件名保留、同名避让；所有活动 Profile 支持模式的完整输入检查；22 个界面项目缺模式/缺 APP 拦截。
- P02C 对话框：保存立即生效、取消保留、项目切换隔离、S19 不要求 BIN 地址、BIN 未确认地址拦截、显式 0、十进制前导 0 和 32 位溢出检查。
- `qt_probe_bridge_tests`：缺参数不进入 Workflow；三个模式把实际保存的 CRC、身份、Driver 地址和 OEM Key 路径传到 Fake Workflow 收到的 FlashJob。
- 新增 LSMR 缺 Hash、楚能混用格式、ARF 禁用 Driver 回归；原有目标切换、文件持久化及协议模拟测试继续运行。
- 完整输入的 UI 测试使用离线存在性夹具，只证明输入通路；文件内容由后端原有格式/签名/窗口检查验证，不能把 UI 测试当作真实 ECU 刷写证明。

构建和 CTest 原始记录位于 `build/perodua-p02c/ui-audit-build.log` 与 `build/perodua-p02c/Testing/Temporary/LastTest.log`。正式程序验证通过后更新到 `D:/project/UDS_tools/dist`，保留现有资源、日志和用户设置。
