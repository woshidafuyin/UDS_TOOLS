# Kvaser 实机接入与验收清单

适用工程：`UDSD_7_28`  
默认示例目标：楚能 331  
默认参数：Kvaser 逻辑 CH1，500 kbit/s / 2 Mbit/s

## 已完成的软件准备

- Kvaser 通过系统 `canlib32.dll` 动态加载，不把系统驱动 DLL 复制进发布包。
- CANlib 全局目录中的物理通道排在 Virtual CAN 之前；Kvaser UI CH1 表示
  “当前首个物理 Kvaser 通道”，不是固定的 CANlib 全局索引 0。
- 枚举输出包含 `kind`、CANlib `api` 索引、设备板载通道、序列号和 EAN。
- Classic CAN、ISO CAN FD、BRS、标准/扩展帧、同步发送和限时接收已实现。
- `List`、`Passive` 和显式授权的 `Active` 三级预检已经放入正式包。

## 明天接线前

1. 先关闭正在占用 Kvaser 的 CANking、Kvaser Device Guide 总线窗口或其他应用。
2. 连接 Kvaser USB 后，在设备管理器的 `CanDevices` 类下确认实际硬件正常。
3. 用 Kvaser Device Guide 确认设备型号、序列号、固件版本和物理通道数。
4. 目标板断电时核对 CANH、CANL、GND；按台架拓扑检查终端电阻。
5. 只保留一个明确的目标板和一套 CAN 硬件，避免 Vector/ZLG/TOSUN 同时驱动
   同一总线。

Kvaser 官方建议先安装 Windows Driver，再连接硬件；正常设备应出现在设备管理器
并能在 Kvaser Device Guide 中读取固件版本：
<https://kvaser.com/canlib-webhelp/page_installing.htm>

## Stage 0：只枚举，不打开总线

在正式包根目录运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\kvaser_preflight.ps1 `
  -Mode List -RequirePhysical
```

通过条件：

- `CANLIB_DLL` 存在；
- 至少出现一项 `kind=PHYSICAL`；
- `PHYSICAL_KVASER=YES`；
- 型号、序列号、`device_ch` 与实物一致；
- 物理通道列在所有 `kind=VIRTUAL` 通道之前。

## Stage 1：被动监听，不发送报文

楚能 331 示例。监听通道直接读取 Profile 的 `channel`，不由脚本覆盖：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\kvaser_preflight.ps1 `
  -Mode Passive `
  -Profile .\profiles\chuneng_331_left_rear.ini `
  -PassiveMs 5000
```

`PASSIVE_RX_COUNT=0` 只能说明监听期间没有观察到报文，不能据此判断驱动失败或
ECU 离线。若目标总线本应有周期报文，应先检查供电、物理通道、CANH/CANL/GND、
终端和位时序。

## Stage 2：受控诊断探测

确认 Stage 0/1 无异常后运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\kvaser_preflight.ps1 `
  -Mode Active `
  -Profile .\profiles\chuneng_331_left_rear.ini `
  -AllowDiagnosticTransmit
```

该模式只执行 Profile 定义的在线探测请求。楚能 331 当前探测范围为默认会话和
扩展会话确认；不会执行安全访问、擦除、下载、刷写或 ECU 复位。脚本会保存 ASC
并输出 SHA-256。

## 实机 PASS 条件

- 设备管理器、CANlib 和 Kvaser Device Guide 三处设备身份一致；
- 工具枚举为 `kind=PHYSICAL`，序列号和物理通道可追溯；
- 指定通道可以打开且不进入 bus-off/error-passive；
- 发送返回成功，并收到与 Profile ID、寻址格式和 UDS 服务匹配的响应；
- ASC、程序哈希、驱动版本、设备序列号和接线通道被记录；
- 只有上述条件成立后，才可标记“Kvaser 基础诊断收发 PASS”。

基础诊断 PASS 仍不等于完整刷写验收。完整刷写需要单独的固件、供电恢复、失败注入
和刷写后版本/完整性证据。
