# 2026-09-04 本地开发分支收敛记录

## 目标

- 以公司 GitLab `company/main` 为唯一开发基线。
- 将最新奇瑞 E0Y 唤醒流程与本地版本读取反馈改动整合到同一条 `main` 开发线。
- 保留完整回退材料，不把无关工作区内容混入产品提交。

## 收敛前状态

- 主工作区：`D:\project\UDS_tools`，`main` 位于 `b821aa3`，落后 `company/main` 两个提交。
- E0Y 独立工作区：`D:\project\_codex_worktrees\uds_e0y_wakeup_fix`，分支 `codex/e0y-wakeup-fix` 位于 `512e63b`。
- 两个待纳入提交：
  - `70122f3 fix(chery): align E0Y CANoe wake-up flow`
  - `512e63b fix(chery): make E0Y wake-up response-driven`
- 主工作区另有尚未提交的版本读取反馈/UI 测试改动，以及不属于本任务的 `AGENTS.md`、`SOURCE_PACKAGE_README.md` 状态。

## 处理方式

1. 在任何分支或工作区变更之前创建完整 Git bundle、带二进制内容的 tracked-worktree patch 和 annotated rollback tag。
2. 将无关 tracked 改动与版本读取功能改动分别保存为两个 stash；现有未跟踪 ZIP、日志、构建目录和测试证据不删除。
3. 将本地 `main` 以 `--ff-only` 快进到 `company/main@512e63b`。
4. 在最新 E0Y 基线上应用版本读取改动；`tests/app_state_tests.cpp` 和 `tests/qt_main_window_tests.cpp` 采用三方合并，保留两侧测试。
5. 使用独立 BuildRoot 和候选 DistPath 进行 Release 构建与测试。
6. 测试通过后形成一个产品提交，更新正式 `dist`，再移除干净的临时 E0Y 工作区和本地临时分支。

## 回退材料

- 回退目录：`D:\project\_rollback\UDS_tools_branch_consolidation_20260904_182907`
- 回退标签：`rollback/pre-consolidation-20260904_182907`
- 完整 tracked patch：`tracked_worktree_full.patch`
- Patch SHA-256：`8BFC58C7336C0CBDB5D2345076AF6E0B6E052233DF578509475E6FB6690A1360`
- 全引用 Git bundle：`repository_all_refs.bundle`
- Bundle SHA-256：`4F9E8A6A5261BF7B27B46598DBDC599F315F5479F3D38F7AAF79BD5CBF4B0F55`
- 版本读取原始改动 stash：提交完成后仍保留，作为文件级回退依据。
- 无关 tracked 改动 stash：提交完成后仍保留，不纳入产品提交。

## 离线验证

- 构建脚本：`scripts\build.ps1`
- 配置：Release，x86 keygen broker + x64 主程序。
- 独立 BuildRoot：`build-branch-consolidation-20260904`
- CTest：`8/8 PASS`，0 failed。
- 发布资源与 Seed/Key 已知向量：Longma、Xizhong、Changan、Chuneng、Chery KP31/E0Y/T1EJ/T22、Shidaixinan、LP 全部 PASS。

## 证据边界

- 本记录证明源码整合、Release 构建、自动测试和发布资源检查通过。
- 本次没有重新连接 CAN 或实际 ECU，没有重新执行 E0Y 实车刷写。
- 真实 E0Y 刷写成功仍以对应执行日志、ASC/BLF Trace 和 HTML 报告为准。

## 恢复方法

- 恢复历史 Git 对象：可从 `repository_all_refs.bundle` clone/fetch。
- 恢复收敛前 tracked 工作区：在 `b821aa3` 基线上使用 `tracked_worktree_full.patch`。
- 仅恢复不相关 tracked 改动：应用对应 stash。
- 仅恢复版本读取原始改动：应用对应版本读取 stash。
- 回到收敛前代码提交：检出 `rollback/pre-consolidation-20260904_182907`。

执行恢复前应先检查当前工作区，禁止直接覆盖新的未提交内容。
