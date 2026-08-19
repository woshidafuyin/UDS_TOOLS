# 一键版本读取配置与维护说明

更新日期：2026-08-19

本文记录当前通用版的一键版本读取配置契约、项目覆盖范围、扩展方式和证据边界。DID 名称以各 Profile 当前配置为准；名称存在不代表已经取得诊断调查表或完成真实 ECU 验证。

## 当前覆盖

- 19 个 Profile 均包含 `[version_check]`；
- 合计 131 个读取项；
- 18 个非占位 Profile 可进入版本读取流程；
- 零跑 A12EV 配置了 10 项，但 `placeholder=true`，在线探测、版本读取和刷写均被禁止；
- UI 自动跟随刷写作业页的厂商、项目、设备、CAN 后端、通道和当前 Tx/Rx ID；
- 多目标 Profile 共用读取计划，设备切换只改变目标、端点和可选的期望值覆盖。

| Profile | 项目 | 读取项 | 前置条件 |
|---|---|---:|---|
| `chuneng_331_left_rear.ini` | 楚能 ARC331 | 5 | `chuneng_520` |
| `chery_ars1_33.ini` | 奇瑞 ARS1.33 | 7 | 无 |
| `chery_kp31.ini` | 奇瑞 KP31 | 7 | 无 |
| `chery_e0y.ini` | 奇瑞 E0Y | 7 | 无 |
| `chery_t22.ini` | 奇瑞 T22 | 7 | 无 |
| `chery_t1ej.ini` | 奇瑞 T1EJ | 7 | 无 |
| `changan_c857.ini` | 长安 C857 | 3 | `ars131_0x400` |
| `lingyao_b216.ini` | 长安 B216 | 3 | `ars131_0x400` |
| `longma_ars1_31.ini` | 长马 J90K / ARS1.31 | 7 | `ars131_0x400` |
| `xizhong_rsmr.ini` | 犀重 RSMR | 3 | `xizhong_nm` |
| `xizhong_lsmr.ini` | 犀重 LSMR | 3 | `xizhong_nm` |
| `shidaixinan_hjzj_fmr.ini` | 时代新安 HJZJ FMR | 6 | 无 |
| `shidaixinan_tianwangxing_fmr.ini` | 时代新安 天王星 FMR | 9 | 无 |
| `shidaixinan_muxing2_fmr.ini` | 时代新安 木星2代 FMR | 9 | 无 |
| `shidaixinan_qingling_fmr.ini` | 时代新安 庆铃 FMR | 9 | 无 |
| `lp_arc.ini` | 零跑 ARC | 8 | 无 |
| `lp_arf.ini` | 零跑 ARF631 | 8 | 无 |
| `lp_a12ev.ini` | 零跑 A12EV | 10 | 无；占位项目禁止运行 |
| `geely_p416.ini` | 吉利 P416 | 13 | 无 |

## 配置格式

版本读取配置与刷写 Profile 放在同一个 UTF-8 INI 文件中：

```ini
[version_check]
session=0x01
precondition=chuneng_520
item_count=2

item_0_name=ECU零件号
item_0_request=22 F1 87
item_0_response_prefix=62 F1 87
item_0_decoder=ascii_trim
item_0_expected=
item_0_required=true

item_1_name=软件版本号
item_1_request=22 F1 89
item_1_decoder=ascii_trim
item_1_required=true
```

字段含义：

| 字段 | 必需 | 当前行为 |
|---|---|---|
| `session` | 否 | 读取前进入的诊断会话，默认 `0x01` |
| `precondition` | 否 | 项目前置发送器名称；为空表示不启动周期前置报文 |
| `item_count` | 是 | 从 `item_0` 开始连续读取的项目数 |
| `item_N_name` | 是 | UI 中显示的 DID 含义 |
| `item_N_request` | 是 | 完整 UDS 请求字节，例如 `22 F1 87` |
| `item_N_response_prefix` | 否 | 正响应前缀；省略时根据请求自动推导 |
| `item_N_decoder` | 否 | payload 解码器，默认 `ascii_trim` |
| `item_N_expected` | 否 | 非空时比较解码值；为空时只读不比较 |
| `item_N_required` | 否 | 是否影响整体成功，默认 `true` |

自动正响应前缀规则：

- `22 xx xx` -> `62 xx xx`；
- `23 ...` -> `63`；
- 其他请求 -> 请求 SID 加 `0x40`。

请求、名称或最终正响应前缀为空时，配置加载失败。请求字节超过 `0xFF`、布尔值非法或 INI 不是有效 UTF-8 时也明确报错。

## 目标覆盖

多设备项目可以在目标段中只覆盖期望值：

```ini
[version_check.target.right_rear]
item_0_expected=RIGHT_PART_NUMBER

[version_check.target.left_rear]
item_0_expected=LEFT_PART_NUMBER
```

当前解析器只从目标段覆盖 `item_N_expected`；请求、名称、解码器和必读属性仍来自公共 `[version_check]`。这样可以避免同一项目的多个设备复制整套 DID 清单。

## 当前前置条件

| 名称 | 当前用途 | 行为边界 |
|---|---|---|
| 空 | 普通项目 | 不启动额外周期发送器 |
| `ars131_0x400` | 长马及相关时代新安项目 | 读取期间维持项目 `0x400` 周期报文 |
| `chuneng_520` | 楚能 ARC331 | 读取期间维持标准 CAN `0x520` 唤醒报文 |
| `xizhong_nm` | 犀重 RSMR/LSMR | 按当前 Profile/目标维持项目 NM |

前置发送器异常会终止读取并报告错误。新增前置条件需要在应用层集中实现和测试，不能把周期报文发送逻辑写入 Qt 页面。

## 当前解码器

| 解码器 | 输入契约 | 输出 |
|---|---|---|
| `ascii_trim` | 可打印 ASCII；忽略 `00/FF` 填充并去除首尾空白 | 完整 ASCII 文本 |
| `hex` | 任意 payload | 完整十六进制字符串 |
| `xizhong_f180` | 13 字节标识 + 2 字节版本 | `标识 Vxx.xx` |
| `xizhong_f189` | 计数 + 多个 15 字节软件记录 | PBL/SBL/APP 分项完整显示 |
| `counted_ascii_24` | `01` + 24 字节 ASCII | 单模块 Boot 标识 |
| `bcd_ascii_part_7` | 4 字节 BCD + 3 字节 ASCII | 零件号 |
| `bcd_ascii_part_8` | 5 字节 BCD + 3 字节 ASCII | 零件号 |
| `counted_bcd_ascii_part_7` | 计数 + 多个 7 字节记录 | 模块数和逐模块零件号 |
| `counted_bcd_ascii_part_8` | 计数 + 多个 8 字节记录 | 模块数和逐模块零件号 |

结构长度、计数、BCD 数字或 ASCII 合法性不满足契约时，解码失败；工具不会截短后伪装成成功。新增解码布局应集中加入 `version_value_decoder.cpp` 并补离线测试，不在 Profile 中嵌入脚本。

## 界面与完整结果

版本读取页在开始前显示计划表，列出状态、请求、DID、含义、必读属性和读取值。运行时逐项更新，并在原始通信区域显示完整 UDS 收发；ASC Trace 保留原始 CAN 帧。

“结果完整”包含两层：

1. 解码值不因表格列宽而被逻辑截断，结构型 DID 展开所有记录；
2. 原始正响应完整保留，便于在解码规则或含义有争议时重新核对。

表格视觉上出现省略号不代表数据被截断；应通过完整单元格内容、原始通信、ASC 和报告核对。

## 新增或修改项目

1. 从项目诊断调查表、CDD、CANoe 工程或已确认 Trace 提取 DID、会话、返回布局和前置条件；
2. 在对应 Profile 增加或修改 `[version_check]`；
3. 优先复用已有解码器和前置条件；只有返回布局或唤醒机制确实不同才扩展应用层；
4. 为请求字节、名称、解码器、必读属性和目标覆盖增加离线断言；
5. 验证刷写作业选择与版本读取页的厂商、项目、设备、通道和端点同步；
6. 用 Fake Bus 验证完整多帧响应、NRC、超时、停止和必读/选读判定；
7. 在真实 ECU 上保存 EXE 哈希、Profile、诊断调查表版本、ASC、报告和实际读取结果；
8. 只有真实返回与资料一致后，才记录为对应项目/设备的台架 PASS。

## DID 含义与证据等级

维护 DID 时应记录来源，优先级通常为：

1. 当前项目正式诊断调查表或正式诊断规范；
2. 与当前 ECU/软件版本匹配的 CDD/ODX；
3. 当前项目 CANoe/CAPL 中有明确名称和解析布局的实现；
4. 真实 ECU Trace 与已知软件包/零件号的交叉核对；
5. 仅凭通用 UDS 习惯或其他项目同名 DID 的推测。

第 5 类只能标记为待确认，不能直接写成项目事实。同一个 DID 在不同 OEM、ECU 或软件版本中可能具有不同含义和布局，不能跨项目继承。

## 验证边界

- Profile 配置存在：证明软件有读取计划；
- 配置解析测试通过：证明字段和请求符合软件契约；
- Fake Bus 通过：证明请求、响应和解码控制流满足测试输入；
- 正式包包含 Profile：证明候选包集成；
- 真实 ECU 有响应：证明当前设备在当前条件下返回数据；
- 调查表、原始响应和解码值一致：才能确认 DID 含义与布局；
- 一个项目或设备 PASS 不自动覆盖另一项目、另一设备或另一入口。
