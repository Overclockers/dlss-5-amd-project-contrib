# lmxxf 0.44 hotfix 消费审阅（2026-10-18）

固定范围：已完成的 `d50cde4b4289e165773482795e4c79817a4da510` →
`00a7d047c289e5809cb911563cb8fa6cd3141799`，7提交、211条变化路径。
前一轮0.42→0.44的640条路径、80模块重建与Small接入见[原审阅](lmxxf-044-consumer-review.md)。
本轮按最终diff审阅，完成pin以 `third_party/lmxxf/UPSTREAM.md` 为准。

## bridge-network

`hip_d3d12_bridge.h` 在外部producer signal/wait前调用
`Network::PrepareForExternalInput(temporal)`，等冷上传和必要布局准备完成，
之后设置 `external_input_wait=true`，实际Enqueue结束才清除。
`hip_reference_network.h::Upload` 拒绝在该wait之后同步上传；
预备函数用独立scratch运行当前模型，缓存profile（模型/遍数、temporal、AE、predict、skin），
AE时也预备EXACT布局，保存并恢复用户adaptive/F8状态，最后清理预热history。
这修复了输入producer尚未提交时，冷上传等待同一stream造成互等的生产可达风险。
产品直接调用此bridge，采用该修复。

同一diff还加入inactive model weights/gain/post-row缓存及无同步 `SetModelMode`，
bridge Create默认1080 Small时预热两个模型，EnableNativePostHistory预读另一模型head。
产品只在创建时用显式 `integration.model_mode`，不调用热切setter；
`consumer-model-mode.patch` 新增 `allow_model_prewarm`，产品明确false，
同时控制双模型预热与备用history head加载。`allow_vit_hotkey`、`allow_input_poll` 仍false。
上游默认保持原样，其他消费者不受该产品权限影响。

菜单切换仍重建有独立所有权的录制链，不向在用网络热改权重。
补验发现旧Runtime每次Prepare都调用Require(Ready)的SetAdaptiveReuseAllowed，
在前帧执行期间交错录制下一帧时会失败。现在将any-temporal的adaptive许可纳入
`networkOptions`重建键，只在新bridge创建时设置，旧录制继续持有原策略与资源。
Off与LowFrequency2/3之间的策略切换同样触发重建。

第一遍输出专项曾触发D3D12消息1047（NR inline sample跨队列写引用）。
定位到 `NrInlineRecording::Op` 将未使用的abortWord槽也绑定侧队列sample；
普通指令现在将该槽绑定已有target，只有abortable op14仍绑定原abortChannel。
shader及实际访问不变，消除无关跨队列资源依赖。
修正后含debug层、提前录制、History/AE/第一遍输出的split/3-slot对照通过。

`first-pass-output.patch` 与 `inline-side-queue.patch` 仅重排patch上下文以匹配新源码；
第一遍sink与实际producer队列的语义保留。全部manifest补丁已在正常staging重放。
桥接头没有ABI/layout变化；本侧C ABI与96B history参数不变。

## addon-boundary

逐项覆盖以下五个路径：

- `src/LmxxfNrRuntime.cpp`：作者为OnimushaWotS加入硬编码Xbox可写镜像配置目录。
  本产品有自己的Runtime和INI所有权，不复制作者Runtime或路径。
- `src/native_game_oneshot.h`：Process后重新取DirectOutput，避免F9重建后返回旧指针。
  本产品不调用oneshot包装器，输出由录制链拥有。
- `src/native_game_frame.h`：FPS抑制持久化；同尺寸热切复用资源、更新history模型。
  本产品不实例化NativeGameFrame，菜单仍使用链重建。
- `src/native_addon_fast_history.h`：同尺寸切模型更新blend并reset。
  本侧NativeTemporalHistory已有逐链Small/Original blend与head，GPU往返已验证。
- `src/native_hot_flags.h`：只读路径上F9先更新内存、落盘尽力而为、HOT_RELOAD关闭仍保留内存值。
  该头因网络包含关系进入vendor；产品不调用NativeHotFlags轮询或热键入口，
  显式model_mode也避免读取其全局模型选择。保留源码不表示启用F9。

## experiments

七条路径分别审阅：`Development/HIP/benchmark_live_capture.cpp` 新增DirectOutput及history切换断言；
`Development/HIP/experiments/f9-hotfix-20261018/deferred_switch.cpp` 覆盖producer gate、
模型循环、head缓存、F8状态；`readonly_hotkey.cpp` 覆盖只读F9；
`run-gates.ps1`、`run-gold.ps1`、`run-history.ps1`、`run-runtime.ps1`
分别组织idle gate/金图ABBA/history/runtime及Xbox路径验证。
这些是调用现有生产接口的独立实验驱动，不生成产品kernel或默认配置。
其中producer等待契约影响本产品，新增本侧cold-producer测试；热setter/只读F9/Xbox实验
按addon-boundary排除。作者环境路径和GPU锁脚本不引入本产品。

## evidence-inventory

其余181条路径按来源分组，逐路径仍在upstream-review.json中：

| 路径组 | 数量 | 结论 |
| --- | ---: | --- |
| `Development/results/f9-hotfix-20261018/` | 124 | 实验原始输出及README，只作为上游证据 |
| `Development/results/game-install-20261018-f9fix/` | 6 | 作者游戏部署日志，不是本产品验收 |
| `Development/results/game-install-20261018-f9fix-r3/` | 6 | 作者后续部署日志，不是本产品验收 |
| `Development/releases/0.44/` | 45 | 包、哈希、归档、构建/部署/验证脚本和回执，不替代本产品打包 |

结果README明确区分首次游戏占用期间的诊断与后续strictidle-r2/r3验收。
只采用后者对producer gate、Gold稳定、F8恢复和history缓存的说明；
不把第一次占用测试当通过，不把作者包已交付等同本产品已发布。
release的 `package-044.ps1` 复用80模块，打入93 Small权重、作者host/runtime及配置层，
强制作者Small0.5默认，RE9保留PREDICT1/SKIN0并发出F9说明；
archive/deploy/verify/run-package脚本操作作者包、游戏目录及来源证明。
这些都没有生成新HIP源码、defines或模块配方。本侧不采用其默认/配置优先级/发布目录。

## docs-defaults

16条文档路径逐组核对：根 `0.42-更新与使用说明.md`、双语CHANGELOG、双语README、
`Development/DevHistory.md`、`Development/WorkingPlan.md`、双语scripts/CONFIGURATION、
六个scripts/package-notes文件及 `使用与参数配置说明.md`。
主要是F9修复说明、镜像下载链接、双语整理和永久参数指南。
作者指南仍写Small0.5默认、系统环境最高、F6/F8/F9及Xbox镜像目录；
本产品保持Original默认、菜单切模型，INI/menu优先，外部flag/env只填未设置键。
文档不新增生产profile文件或kernel选项，不将作者指南当作本产品用户说明。

## existing-switches

当前清单795项：211文件、310 runtime flags、252 kernel gates、13 runtime options、
7 compiler options、1 integration、1 external module bundle。
对照d50报告逐ID比较switch evidence，剔除blob/行号/实验与部署引用的位置变化后，
所有已有switch的定义、consumer列表、assignment/default与recipe语义证据相同；无新增switch ID。
85个因文件blob/实验引用变化而失效的flag保留各自原分类、决策与条件，补充本轮消费证据。
HOT_RELOAD/MULTI_PASS_HOTKEY/VIT_REUSE_HOTKEY及INPUT_POLL另行核对入口，明确不接入。
自适应许可的新链策略与预热权限见bridge-network，不从宏名推断是否启用。
HIP源码/片段、生成器、build recipes与模块defines在该范围没有变化。
因此沿用d50固定源码重建的80模块；仍检查bundle SHA、源码/配方指纹及target provenance。

## validation

本轮已完成Runtime MSVC构建、ABI23项、model CPU与配置优先级12项。
RX9070XT的model-gpu通过Small720/900/1080重复、档位/模型往返、历史及旧录制重放。
修正链策略后inline-gpu四组（正常、超时、ahead、ahead超时）全通过，
包括120帧资源有界、2/3/4/5帧提前录制、执行期间交错录制和watchdog重启。
`tests/lmxxf/run.cmd cold-producer` 六组实际GPU通过：Small、Original1/2/3（预测），
Original真三遍+skin、Small AE。首帧producer fence扣住直到Enqueue返回，
15秒救援均未触发；后续11次交替实际队列重放输出逐位一致，D3D debug无错误。
测试显式断言有效模型，Small使用1344×768工作几何，确认不是fallback。
第一遍专项用 `nr_slots_benchmark` 的verify-ahead/debug/first/history/auto、720、5帧，
分别比较split与3-slot；Original3和Small的最终/first哈希逐帧一致。
末帧Original3为 `f8491c37b5a3f11a` / `da5bb19e6a252fee`，
Small为 `05bd4fa699397e0d` / `160aee1c4befc7a3`。
模块contract40/80及8项配方/补丁重放/冲突/未变证据carry专项也通过。
最后源码的Runtime MSVC、Release宿主与person worker增量构建通过。
正常同步最终退出0，完成pin推进到00a7d047，status=reviewed、skipped_checks为空，
完整模块包与SHA/recipe验证通过；仍保留3条显式暂缓。
合回前再次查询官方main，仍为该固定目标。

## deferrals

原审阅的三条暂缓及可执行下一步原样保留。
Small Mode2/3仍回退Original，后续必须适配processing stride/model参数/录制资源，
通过解析WARP、GPU冷启动/切换/重放和连续帧验收。
仍未做游戏实玩、长时压力、gfx1200真机、本产品ABBA性能验收或发行认证。
本轮不发布、不推送；上游F9、双模型常驻及热切消费是产品选择排除，不列为待启用功能。
