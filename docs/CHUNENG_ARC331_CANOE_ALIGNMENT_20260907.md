# ARC331 与指定 CANoe 工程的五项对齐（2026-09-07）

参考工程：D:/project/jialinggeide_chuneng331/Flash2944_CN_ARC_V1.2(2)/Flash2944_CN_ARC_V1.2/Flash2944_CN_ARC_V1.2/CN2944LC_Flash.cfg。
参考 CAPL：CAPL/Flash20230727.can，SHA256 75281E9A8E3347C7D93A960B8841CCA8F623E256DFCE90EDFE37565FE439B76B。

## 修改范围

用户明确要求将比较表中五处差异按该工程对齐：

1. APP 模式先功能寻址 7DF:10 01，等待 2 秒，再功能寻址 7DF:10 03，然后进行原来的物理 0203 检查。
2. CBF Driver RequestDownload 使用经过完整校验的 driver.main.address（0x10280000），报告同步显示实际地址。S-record 路径不属于本次 CBF 对齐，保留原地址规则。
3. APP 物理 10 02 成功后等待至少 2 秒再请求安全访问。两处等待可取消。
4. 复位后功能寻址 10 03 并核验 50 03；异常仍加入收尾失败列表。
5. 先恢复通信 28 80 03，再恢复 DTC 85 81，然后 10 81 和清 DTC。

FF01 失败后仍继续收尾，最终保留 FAIL 和失败摘要。没有复制 CANoe 对抑制正响应的 28/85 命令仍要求正响应的判定。仅承诺上述五项对齐，不代表所有入口、异常策略和 ISO-TP 时序逐字节一致。

## 架构与回归

修改集中在 Chuneng331Flow 和 CBF 装载 workflow，无新增依赖、配置格式或 UI 接口。CBF 地址仍通过既有布局和完整性校验，不采用未校验文件头。现有收尾测试增加准确 SID 顺序断言及 50 03 仿真响应；取消、断连、累计失败和超时测试保留。

2026-09-07 的上一份 CHUNENG_ARC331_COMPLETION_20260907.md 是五项对齐前的实测历史，地址 0 / 10 83 / 先85后28等内容仅适用于其记录的旧版本。

## 当前验证和剩余差异

Release 构建通过；CTest 9/9 PASS，104.97 秒。正式 EXE SHA256：71C71CE3B2C40A695FCD68E7C512C5361E4DB1BFA949D0D3442DC0CED021F7DE。

正式 GUI PID 23064：通过窗口选择 APP、点击能否刷写，16:23:48 探测成功。执行日志显示 GUI 于16:23:53启动刷写；自动化后续尝试点击开始按钮时按钮不可用，断言退出，未触发重复刷写；不将启动归因于该次失败的自动点击。

真实 GUI 日志确认：16:23:54.710功能10 01；随后功能10 03；16:23:56.982物理10 02；16:23:58.988请求27 11；16:23:59.695 Driver请求下载为34 00 44 10 28 00 00 00 00 40 00。16:25:07收到APP0202/04、FF01/05和复位51 01；16:25:13.765功能10 03并收到50 03，随后28 80 03、85 81、10 81、14 FF FF FF。16:25:16最终界面100%且FAIL，汇总FF01/05和ClearDTC超时。该结果验证本次改动生效，不是固件刷写PASS。

报告：dist/logs/reports/report_20260907_162516_064.html。
执行日志：dist/logs/execution/execution_20260907_162214_705.log。
原始ASC/BLF：dist/logs/traces/flash/trace_20260907_162353_chuneng_331_left_rear_left_rear_app.*。
界面截图和CTest记录：validation/2026-09-07_chuneng_canoe_alignment/。

仍未全量对齐：

- 非收尾阶段的失败传播：工具通常立即终止；CANoe若干服务只记FAIL，主流程仍继续。
- 0203的05/NRC31在工具中作为WARN继续，CANoe测试判定不同。
- CANoe APP前置功能10 03使用不期待响应判定；工具核验50 03，避免合法正响应被判错。
- CANoe对抑制正响应的28/85仍等待正响应；工具发送后继续。工具当前也没有逐项收集这些抑制响应请求的负响应，这是可独立改进的观察逻辑。
- CANoe JudgeResponse首次等待50ms、Pending等待_P2Server（初始化2000ms）；工具默认P2=2000ms、P2*=5000ms。连续Pending整体上限尚未在此次建立。
- CANoe根据74回报块长度对齐到0x100有效载荷边界；工具要求至少0x802，并固定使用0x800有效载荷。这次ECU返回0x802，实际块长度相同。
- CANoe test_online定时器S3server=4000ms；工具TesterPresent=2000ms，启停阶段也未逐项对齐。
- FT/BOOT入口、S-record、异常测试Case和边界响应处理未包含在这次APP+CBF五项验收中。

本次没有新增依赖或跨层调用；未更改CANoe工程。工作区原有validation删除与其他未跟踪项均不纳入提交。
