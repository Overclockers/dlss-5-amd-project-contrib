# 极限竞速 地平线 6（Forza Horizon 6）

用户向兼容说明。**当前补丁已通过专项 C++/WARP 机制验证，尚未完成固定工具链完整构建或游戏实测。**

| 项 | 内容 |
|---|---|
| 早期命令列表 | 游戏在 swapchain 创建之前就建 command list。lmxxf 默认的早期包装白名单包含 Unreal 引擎和 exe 名里带 `forza` 的进程（`c4690e5`）。`LmxxfEarlyExeWrap=true/false` 可以强制打开或关闭 |
| enhanced barrier | 默认仍拒绝。显式设置 `[DlssNr] LmxxfAllowEnhancedBarriers=true` 后，已观察到的完整增强输入经 COMMON 转换后录制并恢复；未受影响的 legacy 输入沿用调用方状态，无关 barrier 不再导致误拒绝。部分布局、aliasing 后未知以及 `SYNC_SPLIT` 仍拒绝 |
| 闪烁线索 | 用户日志中 lmxxf 的 recording ready 与 same-frame boundary unavailable 交替出现。补丁增加 Streamline native-object 查询及完整 NR/SR 调用中的代理保持；是否足以消除该游戏闪烁需要重测 |
| 现状 | 按下方修复计划编译并测试两个后端。OptiScaler 本体对 Forza Horizon 5 的 `dbghelp` 导出处理与本条无关 |

作者交接、日志证据、配置矩阵和二次修改顺序见 [修复计划](forza-horizon-6/修复计划.md)。
