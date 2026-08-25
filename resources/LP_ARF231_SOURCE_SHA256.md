# 零跑 A12/B11 ARF2.31 默认资源来源

来源：`D:\project\通用自动化刷写工程需求资料_公盘冻结_20260824`。A12 与 B11 使用相同哈希的 `Flash_ARF2.31_V1.0.7z` 协议工程，但软件包、TMP 证书和 Profile 独立。

本机历史工具 `D:\project\30_工具与平台\S19TmpCutter\S19TmpTailExport.exe` 及其 README 用 S19 尾部特征在 TMP 中定位并导出 HEX-ASCII ASC。它的输出已作为兼容基准：A12/B11 均导出 1322 字节且与 TMP 尾部逐字节一致。但 B11 的尾部特征在 APP 填充区存在大量重复匹配，因此生产代码不依赖该未签名 EXE，也不使用“最后一次匹配”作为安全边界。

运行时直接结构化解析 `LEAP` TMP：验证 JSON `SignInfoLen`、单 APP 段、CRC32、地址、长度和完整 APP 数据，再提取尾部 1322 字节证书；证书前 32 字节必须等于 TMP 内 APP 的 SHA-256。TMP 可作为单一“APP 文件/升级包”输入，工具在内存中拆出 APP 与 Certificate；若用户选择 S19/SREC/BIN，则必须另选 ASC/TMP，使用 TMP 时还会把其内嵌 APP 与所选 APP 逐字节比较。由此既复用历史工具确认过的 ASC 载荷语义，又消除了模糊内容匹配和错配文件风险，且不生成容易失去来源关系的临时 ASC。

| 项目 | 相对路径 | 字节 | SHA-256 |
|---|---|---:|---|
| A12 | `resources/lp_arf231_a12/APP/ARF2.31CC3_LPA12A_APP_V3.01.07_without_boot.s19` | 3833910 | `35EA8462A09BF11693851972EDE225CA6132C0EC265DA7E80D4C6979308D5505` |
| A12 | `resources/lp_arf231_a12/dll/ARF2.31CC3_LPA12P_SeednKey_cdd.dll` | 777216 | `7F7A67365D7A54A9985C41B79C48F01C37A6595422E67B24DA54B2491A77D020` |
| A12 | `resources/lp_arf231_a12/Verification/LP-MRS050-BA_V3.01.07_R_20260608.tmp` | 1574528 | `49396263A9D8F45D806A1F8F73DE3AFF799A2F01ED3894201755193DA29ACCF5` |
| B11 | `resources/lp_arf231_b11/APP/ARF2.31CC3_LPB11F_APP_V2.10.16.s19` | 3833910 | `E98764819AED382714FC61F1663EC1A632C445362094A418B5C5E3F0E1858682` |
| B11 | `resources/lp_arf231_b11/dll/lingpao_SeednKey_cdd.dll` | 777216 | `7F7A67365D7A54A9985C41B79C48F01C37A6595422E67B24DA54B2491A77D020` |
| B11 | `resources/lp_arf231_b11/Verification/LP-MRS050-BA_V2.10.16_R_20240802.tmp` | 1574505 | `56F17AE41003E54374CE9ABA93EC9B44197012C667065B1E12115C1FC2CA019E` |
