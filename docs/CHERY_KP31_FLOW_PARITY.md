# 奇瑞 KP31 三种正常刷写模式复刻

## 基准

- 公盘包：`Chery_KP31_Flash_V1.1_20260413.7z`
- 权威入口：`Capl\Flash.can::maintest()`
- C++ Flow/Profile：`chery_kp31`
- CAN：Classic CAN 500 kbit/s，TX `0x70D`，RX `0x78D`，FUNC `0x7DF`
- ISO-TP padding：`0x55`
- 安全访问：`27 11 / 27 12`，16 字节 Seed/Key，空 Variant
- 原工程没有 FT 或 PLS→APP 分支，通用工具也不提供 FT。

## 模式映射

| UI模式 | CAPL入口 | 下载内容 |
| --- | --- | --- |
| APP | `APP -> FileInit() -> 2 s -> Download()` | Driver + APP |
| CAL | `CAL -> TC_7()` | Driver + CAL |
| APP+CAL | `APPAndCAL -> TC_2()` | Driver + APP + CAL |

三种模式分别复刻，不能把 APP/CAL当成简单文件开关：

- APP 的前置步骤使用功能寻址 `10 83 / 85 82 / 28 81 03`，APP数据块固定为
  256字节，APP后省略`37`，校验例程为`DD02`；
- CAL/TC_7 使用物理寻址 `10 03`、`0203`、Driver/CAL `DD02`，最后执行`DD03`；
- APP+CAL/TC_2 使用物理寻址 `10 03`、`D003`、携带APP RSA的`D004`，
  Driver/APP/CAL分别以`D002`校验，最后执行`D005`；APP和CAL均发送`37`。

## 文件与布局

| 内容 | 地址/长度 | 当前默认文件 |
| --- | --- | --- |
| Driver | `0x08000000 / 0x400` | 留空，用户选择 |
| Driver RSA | 512字节 | 留空，用户选择 |
| APP | `0xC0080000 / 0xF5000` | 留空，用户选择 |
| APP RSA | 512字节 | 留空，用户选择 |
| CAL | `0xC0180000 / 0xC8` | 留空，用户选择 |
| CAL RSA | 512字节 | 留空，用户选择 |
| SeedKey DLL | x86 `GenerateKeyEx` | 已打包 |

压缩包自带`FlashDrv.s19`，但原始INI指向另一个正式Driver文件名，因此仅作来源
保留，不自动选中。原INI的CAL路径本身也是空白。

## 共同边界

- KP31基线没有ARS1.33使用的`0x600/0x25B/0x4B4`周期前置报文；
- `maintest()`中4秒TesterPresent首次启动被注释，C++不新增该报文；
- CAL/TC_7的`Update_PublicKey`是可选面板维护开关，默认关闭，不是第四种刷写模式；
- 异常`TC_11～TC_23`不是正常刷写模式，不接入通用工具正常模式下拉框。

## 验收边界

代码、Profile、资源打包和离线测试不能替代KP31实板首刷。三种模式必须分别用
配套Driver/APP/CAL/RSA完成ECU验证并冻结Trace、刷后版本、报告和全套哈希；尤其
要确认APP模式最后一块`36`后省略`37`的真实ECU行为。
