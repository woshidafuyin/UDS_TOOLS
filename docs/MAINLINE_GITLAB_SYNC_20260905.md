# 通用主线公司 GitLab 同步记录

日期：2026-09-05。用户明确选择先提交已完成主线，不等待 Perodua P02C 开发完成；奇瑞独立版不提交。

## 仓库与范围

- 本地：`D:/project/UDS_tools`，分支 `main`。
- 远程：`company`，`http://git.chuhang.tech/zhoufuyin/UDS_TOOLS.git`，只推送 `main`。
- 同步前已 fetch 公司主线；远程基线为 `512e63b`，本地已完成代码为 `ae42d27`。
- 保留全部22个项目Profile、17个Workflow及四个功能页，不裁剪厂商，不改为奇瑞专版。

## 纳入的已完成代码

| 提交 | 实际改动 | 验证 |
| --- | --- | --- |
| e0993f9 | 在最新E0Y基线上整合版本读取反馈：原始ECU响应与错误详情分离；未执行、取消、警告、失败分别展示；保留E0Y唤醒及UI回归 | 该提交已记录Release x86/x64构建、CTest 8/8和发布资源/SeedKey检查通过；详见BRANCH_CONSOLIDATION_20260904.md |
| ae42d27 | 外部同名资源保存到新的selected-XXXXXX子目录，保留原文件名；避免多个字段引用同一路径时互相覆盖；直接选择原受管文件则复用路径 | 本次从该提交提取实际源码与专项测试，在E盘独立目录重新MSVC/Qt编译运行，PASS |

本次新增文档提交更新README和CHANGE_LIST，准确区分主线已有能力、历史验证和独立分支交付，不更改刷写协议。

同名资源隔离专项覆盖：Driver/APP同名输入、重复导入、默认文件保护、历史选择内容保留、源文件保护、同路径复用与不存在的输入。测试文件为`tests/resource_file_collision_tests.cpp`；当前仍是单独MSVC/Qt专项，不冒充已经注册在8项CTest中。

## 未纳入范围

- 正在另一任务修改的Perodua P02C、AES-CMAC、Profile/Workflow/UDS/UI扩展及其测试、导入脚本和客户规范。
- `fix/kp31-canoe-parity-20260905`单独交付分支，以及`D:/project/Chery_UDS_tools`奇瑞裁剪工程。该专版的0x600/100ms唤醒、最大化等不在本次主线同步范围。
- 本地ZIP、dist副本、构建目录、ASC/BLF日志、OBJ、Python缓存、客户CANoe工程和台架资源。

以上材料继续保留在原位置。只暂存本次三个文档，不使用`git add .`，不推送其他分支、标签或GitHub。

## 本次验证与边界

- 新编译的同名资源隔离专项通过。
- README/CHANGE_LIST与`ae42d27`实际逻辑核对；主线项目及Workflow数量依据Git提交树检查，不使用含Perodua临时文件的工作目录数量。
- 本次不重新构建和发布整套dist，避免与另一任务并发构建冲突；此前8/8为e0993f9的已记录验证，不表述为本次全量重跑。
- 本次没有访问CAN或ECU；源码同步不等于新增台架验收。
- 推送前检查准确提交范围；推送后以公司远程main哈希与本地HEAD一致为成功条件，并单独报告工作区的未提交内容。

本次核对、初始工作区快照及新编译专项日志：`E:/project/UDS_tools_gitlab_sync_20260905`。
