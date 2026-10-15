# 2026-10-12 审查意见核实与修复

核实基线：`work/mochizuki-upstream-a9b13dd` 的 `0e4c906a`，外部意见针对较早的
`cc780a21`。以下编号对应外部报告，结论来自当前源码和本次专项验证。
外部报告里的修复方案不是产品约束；没有改主工作区、上游 pin、网络模块或用户配置。

| 编号 | 核实结论与本次处理 |
| --- | --- |
| 1 | 风险成立。有限 dispatch 只限制工作量，不能保证调度进度或帧预算。保留 `Config.h` 的默认 false，纠正文档和发布文案。AMD 机制测试成功/耗尽路径通过，但耗尽用例游戏 tail 达 243.880 / 257.522 ms，不能据此启用默认。 |
| 2 | Host 的 `uncertain` 确实把普通无网络失败变成永久占槽。失败也追加新的侧队列 tail Signal，与真实 game tail 共同证明完成；不直接把 `serial` 当作有效证明。未知 Signal 仍保留，停止无法产生新证据的 100 ms timer。runtime End 失败也停止无意义的重复回收。 |
| 3 | 成立。捕获采样采用 50 µs 指数退避到 5 ms，最多 2048 次且有 10 s deadline；等待实际 fence 值时 Sleep(1)，不再仅 SwitchToThread。 |
| 4 | 成立。发布正文直接维护在 release.yml（2026-10-15 调整）；空白、无版本标题或标题不匹配均失败。workflow 生成当前源码链接，Release 使用 body_path。 |
| 5 | 镜像链接存在。三语 README 与发布正文移除 gofile，指向上游提取方法，明确模型由用户按适用许可自行准备；没有把外部报告的法律判断当作已证事实。 |
| 6 | 成立。补入 lmxxf MIT；所有明确列出的宿主、后端、人物模型及依赖许可改为 staging 前必需项，复制失败中止；ZIP 独立门禁按 Licenses 的完整路径检查。 |
| 7 | 成立。根目录 LICENSE 复制既有产品 GPL-3.0 全文，NOTICE 说明来源链与组件许可；两者入包。没有替第三方重新授权。 |
| 8 | “没有哈希/源码对应记录”的判断不成立。当前是每架构 40 个、共 80 个模块；modules.json 已有源名、源哈希、binary SHA256、defines 与 opts，root/leaf SHA256SUMS 已由实际门禁校验。UPSTREAM.md 补上这些记录及 source assembly 入口；CI 未做 HIP 位级重建比对。 |
| 9 | 成立且基线比报告更严重：UploadColorPattern 的默认 0 会覆盖调用方先设置的 global，缩放测试实际未缩放。删除 global，改传 PatternOptions；HDR 期望值和 subrect padding 同样显式传参数。非 FP16 缩放及非有限 FP16 exponent 直接失败。 |
| 10 | 报告未证明关闭 event 本身导致失效句柄；原 WaitCapture 超时立即返回，也不会在同一调用里继续注册。没有按建议泄漏 event。捕获改为直接查 fence，DrainD3D12 在等待后复查实际 fence/设备移除值，避免把唤醒当作完成证明。 |
| 11 | 成立。InlineCallback 补 catch (...)；与 std 异常共用失败收尾，置 inlineEnqueueFinished 并在已报告游戏提交时完成 End。 |
| 12 | 成立。WaitInlineTurn 改 condition_variable_any，PopInlineEnqueue 在 runtime End 后 notify，保持 RecordingMutex 与原有提交顺序。 |
| 13 | 成立。保留整包当前 ABI 的严格 struct_size 检查，删除短结构体补默认值和 min memcpy 的死代码；不增加旧 ABI 兼容。 |
| 14 | FNV64 不认证写入者，风险边界成立。可写驱动缓存迁至 LOCALAPPDATA/OptScaler-NR/Mochizuki/aco-cache，不可用时禁用磁盘缓存；文档明确同用户进程仍能修改。包内固定 ACO records 仍作为构建输入。 |
| 15 | 成立。三个指出的 Check/Require 异常改为 0x%08X。 |
| 16 | 每个 recording 独立分配成立，但不是已证明的功能错误。已有 session pipeline/side queue 复用；本次修复可证明完成的失败回收。纹理/descriptor 池化暂缓，需先测 allocation/VRAM，并保证关闭列表的 replay 与跨队列寿命不被复用破坏。 |
| 17 | 当前文档和上传器已禁止覆盖不同字节的已发布附件。本次进一步在改 Release 正文前核对已存在 Release 的源码 SHA；来源未知或 tag 移位都拒绝。新 Release 固定 target_commitish，后续使用新版本如 alpha.2。 |

## 回收边界

Enqueue 失败可能发生在已提交部分 HIP/Vulkan 工作之后。Host side tail 只证明它自己
D3D12 命令和资源的完成，不能替 runtime 编造网络完成。`CollectRecording` 仍独立检查
runtime 的未确认提交与完成证书，必要时保留 job/session。失败的 runtime session 仍停用；
“容量可回收”不意味着忽略 runtime 错误继续推理。

游戏提交或 Signal 确实未知时仍保留对象并报告首错；不能为消除泄漏而释放仍可能在用的
GPU 资源。停止无意义的 timer 不替代完成证明；后续显式 Collect 仍可重试。

关于 event，核对了 [Microsoft SetEventOnCompletion 文档](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12fence-seteventoncompletion)
及 [Microsoft 的句柄引用说明](https://devblogs.microsoft.com/oldnewthing/20250918-00/?p=111608)。
本次保守处理的是 fence 完成判定与注册事件复用，而不是宣称已复现句柄 UAF。

## 本次验证

- `tests/host/run.cmd ci`：PASS。
- `tests/shader/run.cmd`：PASS（WARP；含恢复实际缩放后的 HDR 数值、stabilizer 和 post-SR）。
- `tests/lmxxf/run.cmd inline-discard`：PASS；12 次 Reset/Release、6 次真实 callback Enqueue 失败、双 fence 保留、turn notify、未知证明不永久重挂 timer。
- `nr_inline_recording.exe --hardware`：PASS，AMD vendor 1002 / device 7550；成功、不可变超时回退、replay、open query、增强布局恢复。
- `tools/build/build-mochizuki-runtime.cmd --reuse`：本次源码变更导致重编，BUILD_OK。
- `tests/mochizuki/run.cmd abi`：PASS，含 ACO CPU/源 pin 检查与当前 DLL ABI。
- `tools/build/build-release-local.cmd --fast`：BUILD_OK（host + person worker）。
- lmxxf_nr_gpu / nr_inline_runtime_gpu：新 PatternOptions 接口编译通过；未跑模型推理矩阵。
- 实际 packager 的两个专项：缺失许可前置拒绝、ZIP 精确携带原始 notices，PASS。
- release-notes 工作流正文的 3 项实际 PowerShell 测试：PASS。
- 发布来源与附件上传 9 项单元回归：PASS（mock API，未访问/修改线上 Release）。
- 模块 40/80 契约与真实 modules metadata / SHA256 门禁：PASS。
- `git diff --check`：PASS。

未执行：RDNA3/RDNA4 × HAGS 开关矩阵、实际游戏帧时间/长跑 VRAM、新版完整模型推理矩阵、
完整安装回归与 GitHub Actions 权威发布 CI。未出包、推送、移动 tag 或发布 Release。
资源池化和毫秒帧预算方案仍需独立性能证据；该修复不能作为 NrInline 默认开启或正式发版验收。
