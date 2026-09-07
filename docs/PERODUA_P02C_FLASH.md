# Perodua P02C CPD ARS1.33 刷写工程

日期：2026-09-05。原始资料：`D:\project\136_PeroduaP02C_ARS1.33_30200`。

## 两个入口

- 通用工具：`D:\project\UDS_tools\dist\CH_Diagnostic_Studio.exe`，选择 `Perodua / P02C / CPD ARS1.33`。
- 独立 CANoe 工程：`D:\project\136_PeroduaP02C_ARS1.33_30200\CANoe_Flash_P02C\Perodua_P02C_Flash.cfg`。

只提供正常 APP、CAL、APP+CAL 刷写，不添加异常案例、DOUT、车辆信号模拟、FT/PLS、公钥更新或自动重复擦除。
两份工程复用同一 Perodua Workflow/Flow。CANoe 的 CAPL 节点负责 CAN 收发，项目附带的原生 DLL 负责协议流程、公共 ISO-TP、文件解析和 AES-CMAC；不启动通用工具，不直接打开 Vector/ZLG/TOSUN 驱动。

## 规范对应与顺序

原文位于客户目录 `07_Specification\01_Customer`。以下页码为 PDF 页码。

|阶段|实现|资料依据|
|---|---|---|
|寻址|CPD 714/794，网关 701/781，功能 7DF|CES010 p6/p9；CPD 矩阵 Matrix A118:H123|
|传输|Classic CAN 500 kbit/s，8字节，FF填充；APP FC BS8/STmin20，Boot FC BS0/STmin0|CES004 填充条款；CES006 p14–15|
|识别|CPD物理 22 F191，要求读取到数据，不匹配固定件号/版本|CES012 p25刷前识别；CES005 F191定义|
|预编程|功能1083，等50ms；网关31010203；功能8582，等50ms；功能288103，等50ms|CES012 p23–26 Figure6/3.7.1.1|
|编程会话|CPD 1002|CES012 3.7.1.2|
|安全访问|2707，16字节seed；AES128-CMAC，2708发送16字节key；零seed跳过发送key|CES009 p6–9；OEM Key CPD Level4列|
|指纹|2E F107，3字节BCD日期+27字节ASCII，尾部空格|CES005 p13–14；CES012 p30|
|RAM Driver|每段340044地址长度、36分包、37、31010202+该段CRC；不调用Flash擦除|CES012 p25–26|
|APP/CAL|每个连续段先3101FF0044+该段地址长度，再34/36/37/0202；APP+CAL先APP后CAL|CES012 p26及FF00/0202/34条款|
|依赖|3101FF01，结果00|CES012 p36–37|
|复位|1101，等待2秒|CES012 p26|
|后处理|功能1083、288003、8581、1081，间隔50ms|CES012 3.7.1.3|
|会话保持|功能3E80，每2秒；退出/取消后停止|CES006 S3client时序|

请求默认P2=150ms，P2*=5000ms；公共UdsClient处理NRC78等待。读请求与TransferData等待响应超时最多重复2次；36重试保持完整报文和BSC不变。安全访问、擦除、校验例程、复位不自动重发。NRC及错误回显停止流程。
34返回块长包括SID和BSC，数据容量减2，尾块可缩短；每次34后BSC从01开始，FF后回00。当前公共ISO-TP支持最多4095字节UDS负载。例程必须包含正确RID和结果00，不能只凭71正响应判定成功。

## 文件处理约定

沿用公共S-record解析器：读取数据记录、地址和自然连续段，不限制S-record文件后缀，不加固定文件名、版本、SHA256或配套哈希匹配。`.bin`按原始数据读取，使用对应的配置起始地址。其他输入交给S-record解析器，不能把任意未支持容器当作BIN发送。

不做额外128MiB上限、重复窗口校验、未选模式的文件检查。保留文件读取、公共格式解析、非空数据和32位地址/长度编码所需的处理。

S-record不填充空洞，不强制配置整模块长度。Driver、APP、CAL的`*_length`通用字段不用于本流程；S-record的`*_start`也不覆盖文件内地址。BIN的`*_start`必须来自对应ECU实际布局。

CES012 p26要求后续段返回EraseMemory，本实现按段发送FF00，不重复擦除整个APP/CAL。实际交付的段范围必须与ECU逻辑块/擦除扇区规则相容；如果实际包需要把多个稀疏段合并为同一个擦除块，应根据正式FlashJob/布局定义调整下载计划，不按其他车型的扇区大小猜测或自动扩大范围。

## 尚需实际项目输入

1. 配套Driver、APP及需要时的CAL。当前备份目录未找到独立S19/BIN固件，工程默认不带虚构固件。
2. BIN的起始地址，以及实际固件段与ECU擦除块的对应关系。
3. `programming_tester_identity`：10字节维修站代码，后接最多17字节测试仪序列号（合计不超过27个可打印ASCII字符）。
4. `programming_crc_variant`：必须依据ECU实现选择`reflected`或`non_reflected`。
5. 网关0203所需的实际连接和前置条件。

CRC不是可随意省略的文件审核，而是客户规定的ECU CheckMemory步骤。CES009 p14规定多项式04C11DB7、初值FFFFFFFF、最终取反，但未明确RefIn/RefOut，因此不替客户默认确认反射方式。配置留空时在CAN访问前报告缺少算法参数。支持的两个实现分别是reflected（检查值CBF43926）和non_reflected（检查值FC891918），检查串均为ASCII 123456789。

0203存在文字/示例差异：实现接受完整RID回显后无状态列表，或四个00；其他布局停止。这是对CES012 p31–32的兼容解释，仍需实际网关响应验证。

刷后不附加强制F186/F107逐字节回读，也不匹配未知的目标软件版本；版本读取可在通用工具已有页面单独执行。结果PASS代表该次协议链完成，不自动证明目标软件版本和所有应用功能正确。

## OEM Key

两份工程默认使用当前Windows用户的受保护密钥文件：
`%LOCALAPPDATA%\ChuHang\DiagnosticStudio\keys\perodua_p02c_level4.key`。

本机此前已从用户提供的OEM文档导入CPD Level4。OEM密钥不写入源码、INI或交付包；AES-CMAC由Windows CNG执行。更换电脑/Windows用户后运行随工程提供的`import_perodua_oem_key.ps1`重新导入。CANoe面板中的OEM Key路径可以留空，使用本机默认值。

## CANoe 使用

本机验证版本为 CANoe 12.0.221 SP6 x64，配置使用32位执行环境；工程同时附带Exec32/Exec64 DLL，由CAPL按执行环境选取。项目默认Simulated Bus，CAN 1 已配置500 kbit/s。实际刷写时按实际连接切换Real Bus并核对通道映射；面板CAN字段是CANoe逻辑通道，默认使用CAN 1。

1. 打开CFG，打开P02C面板，选择Driver、APP/CAL文件。
2. 填入测试仪身份，选择APP/CAL/APP+CAL、已确认的CRC方式；BIN地址在面板中按十进制填写。
3. 启动Measurement只是准备环境，不自动刷写。
4. 点击面板Start flash执行一次；也可以在Test Setup中启动P02C Test Module，由模块读取面板当前模式执行一次并生成测试报告。不要同时启动两份入口。
5. Stop中止后续步骤并停止TesterPresent；停止Measurement也会回收后台工作线程。Test Module运行时发送内部心跳，模块被中止后约1秒内取消后台刷写；此心跳不发送CAN报文。失败不自动复位或再次擦除。

CANoe Write/Trace提供步骤与报文，Test Module报告记录本次结果。独立工程附带所需DLL，不依赖通用工具EXE运行。

## 验证与架构

- 通用工具Release构建完成；9/9 CTest通过，包含Perodua、公共协议、适配器边界、应用状态和Qt回归。
- Perodua专用测试覆盖AES-CMAC RFC4493向量、两种CRC、顺序、段擦除/传输、BSC回绕、错误结果、NRC、重试上限、取消和三模式真实UdsClient/ISO-TP模拟运行。
- CANoe DLL独立ABI测试覆盖三模式完整CAN/ISO-TP、缺少CRC参数时无CAN输出、停止及发送队列清理。
- CANoe原生打开CFG、加载面板和Test Module成功；两个CAPL生成CBF，编译无错误，刷写节点仍有7条编译警告，未将其表述为零警告构建。
- Simulated Bus启动日志确认CAN 1为500000 BPS，模式/CRC下拉框各3项；面板Start与Test Module均执行到参数检查，缺少CRC时停止。Test Module生成HTML/XML预期FAIL报告，保存在Validation中，不能作为ECU刷写结果。
- 尚无真实CPD/网关台架刷写结果。

UI只处理项目选择和文件路径，Workflow负责输入/通信接入，Flow负责规范步骤；公共ISO-TP和UDS保持复用。新增CAPL桥接层仅把CANoe报文队列接入ICanBus。密钥保护使用Windows crypt32，AES使用已有bcrypt，不添加第三方加密运行时。CANoe桥接为单个刷写节点设计，同一DLL进程内不支持多个并发刷写节点。

通用工具选择 Perodua → P02C 后，点击“项目刷写参数…”，填写已确认的 CRC 方式和测试仪身份。只有所选文件是 BIN 时才需填写该角色的起始地址，支持十进制和 0x 十六进制（已确认的 0 地址需明确填写 0）。S19 自动解析各段地址和长度，不要求填写 BIN 地址或整模块长度。

点击保存后立即生效，下次启动自动恢复；取消不改动已保存参数。参数按项目 ID 保存到当前 Windows 用户的工具设置中，未保存过时读取 INI 默认值；保存过后以界面设置为准。无需修改 INI 或重启。OEM Key 仍通过文件选择入口使用当前用户的受保护密钥文件，不放入参数设置。CANoe对应参数由面板填写。源码中的项目默认参数保持未确认状态，不提供虚构的正式Tester ID或CRC结论。

CANoe工程运行所需文件均在工程目录中。修改CAPL后可直接在CANoe重新编译；修改C++协议引擎时，在UDS_tools仓库以MSVC分别构建x86/x64的`perodua_capl`目标，设置`UDS_CAPL_INCLUDE_DIR`为Vector安装示例的`Programming/CAPLdll/Includes`，再更新相应Exec目录。工程Source文件夹保留本项目C++源文件快照，公共依赖以UDS_tools仓库为准。
