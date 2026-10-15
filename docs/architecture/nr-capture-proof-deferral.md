# GPU-armed 输入完成核验：暂缓修复与恢复研究

## 当前决定（2026-10-15）

用户明确决定先交付，撤回本轮新增的 CPU watch 完成核验。保留既有 GPU-armed 行为：
提交有界 watch 后即可 enqueue 网络；watch 未见 capture 时，GPU 选择原画，回收时请求
history reset。保持现有配置和用户偏好，不缩减槽数、不更改格式、不扩大等待预算。

这是**接受一个已知、尚未量化游戏触发频率的同步风险并暂缓修复**，不是证明原行为安全，
也不是宣称“有意设计所以没有 bug”。`NrInline` 仍默认关闭；split 不受此次回退影响。
保留曝光 recording 所有权、诊断资源寿命、恢复 completion proof 交给 runtime End、
PowerShell 5.1 native stderr 出包修复。本次不增加 RenoDX shader/vendor 补丁。

被撤回的完整修复和两阶段实验保存在 [研究补丁目录](../research/nr-capture-proof/README.md)。
它们是纯文本 `.patch`，不参与构建、打包或同步清单，没有运行时开关或自动应用入口。

## 风险与证据边界

危险条件是游戏已认领 gate、输入仍在写入，而 post-claim wait 先达到 cap 或收到 abort。
继续启动网络可能与该输入写入并发。若 gate 在游戏认领前关闭，拷贝被 predication 跳过，
则不是同一类并发写入问题。丢弃最终画面不能补上缺失的读写依赖。

旧路径在回收时才请求 history reset，不能保证提前入队的后续帧未消费污染后的历史。
成功 enqueue 也不再证明 predicated exposure measurement 已实际执行；曝光修复保留
recording/slot 的生命周期保证，但不能用它证明 GPU-armed miss 时测量内容有效。

已有故障注入可制造该窗口；正常游戏触发率、可见画质损害、崩溃频率尚未测定。
用户实际游戏中未遇到问题；此前 E33 观察的 claim cap/abort 为零。这些是实际使用证据，
不等于普遍的同步安全证明。现行 stuck-copy/inline-discard 测试验证回退原画、回收、
history reset 请求等行为，**不证明输入访问和后续历史安全**。

## 被撤回的修复

`NrInlineRecording::ArmCapture` 在 watch 后复制 seen/gate 到 readback、Signal、
在 RecordingMutex 外等待 fence，并只在 seen=1 时返回成功。owner 在 capture miss
时跳过 Enqueue，在释放有序 enqueue turn 前 reset history；NetworkQueued(false)
发布 bypass，同时保留游戏和 side 两端的完成证明。miss 与运行错误区分，避免较晚回收
再次重置已经恢复的后续好帧。测试包装实际 Enqueue，要求 stuck-copy 时调用次数为零。

该实现此前通过正常/超时/提前 Reset/跨队列重放/1440p temporal/迟到 copy/abort 等
真实 Mochizuki GPU 用例；默认 stuck-copy 中 watchdog 可能先于 cap，不能宣称两种终止
原因均被独立强制覆盖。对应安全修复没有完成游戏性能验收。

## 性能归因和无效方案

同机 RX 9070 XT，统一 Mochizuki runtime，1440p、History、FLOAT、MP1、ACO off、
GPU handoff、poll16、4槽、每段180测量帧＋12预热帧；强制验证全 compute 路径。
GPU运行与编译串行。以下有效性能段均180/180成功NR、0 timeout、0 admission skip。
NR/s 是合成夹具吞吐，**不是游戏 FPS**；单次候选只用于筛选，不作为最终验收。

| 对照/方案 | NR/s | GPU P95 ms | 结论 |
|---|---:|---:|---|
| 修前，正/反序 | 64.486 / 64.697 | 15.919 / 16.168 | 基准 |
| CPU核验安全版，正/反序 | 55.363 / 57.368 | 28.303 / 25.559 | 回退可重复 |
| 两阶段watch，两次 | 56.266 / 56.434 | 25.697 / 25.522 | 未恢复，不采用 |
| 独立capture队列 | 58.877 | 17.860 | P95改善，吞吐仍不足；不采用 |
| side queue HIGH priority | 57.244 | 25.111 | 无有效恢复，不采用 |
| CPU忙轮询完成fence | 59.620 | 17.822 | 未恢复且占CPU，不采用 |
| game wait切256段，总迭代数不变 | 59.126 | 22.916 | 未恢复，不改产品设置 |
| 修前，附runtime GPU计时 | 64.855 | 16.182 | network/core平均14.113/12.932ms |
| 安全版，附runtime GPU计时 | 58.483 | 19.308 | network/core平均15.396/14.259ms |
| 保留其他修复，只移除CPU核验的反事实 | 63.764 | 16.268 | network/core平均14.266/13.064ms；不可交付的归因实验 |

此前40段1080/1440正反序 sweep 也显示1440p三/四槽新增约8–12%回退，1080p没有同样结论。
本轮实际回退后的复测及源码验证另见文末；不得用上述反事实结果代替最终源码实测。

CPU插桩180帧：capture等待平均17.014ms（包含前序网络与排队），锁等待平均0.188ms、
P95 0.001ms，仅两帧超过2ms；Enqueue平均0.263ms。GPU插桩：watch自身平均0.139ms、
P95 0.161ms；完成到CPU读取平均0.090ms、P95 0.136ms。输入核验同步点是主要回退来源，
但256段early-out、普通锁竞争、event唤醒本身都不足以解释损失。

证据支持提交时序变化影响GPU执行/竞争：网络核心GPU计时本身变长。
**尚未证明具体WDDM量子、硬件抢占或缓存机制，不应写成确定的驱动归因。**
曾逐帧调用GetClockCalibration的探针自身引入数百毫秒阻塞和timeout，已废弃；有效GPU
探针只在首个预热帧校准，测量段不再调用。不要重新引入这种测量干扰。

两阶段实验的具体做法：首阶段保留原watch并关闭gate，snapshot＋fence后，seen=1或
gate=2直接结束；只有gate=1且unseen才复用已退休allocator，执行原256段post-claim
wait。两阶段保持同一generation和watchdog截止时间；fast path不采样虚假的wait速率，
但保留adaptive clean统计。它已实测无效，不再作为尚未尝试的下一方案。

## 值得继续研究的方向

1. **提交时序剖析。** 对齐game capture、game spin、D3D→Vulkan semaphore、网络首尾
   与side completion，区别GPU空闲、被抢占、并发竞争及网络执行本身变化。优先使用低干扰
   时间戳/可用GPU轨迹；单独核验插桩是否改变吞吐。不再猜测event或锁为主因。
2. **安全预提交机制的可行性。** 能否提前排队，但在输入未完成时真正跳过整笔网络的所有
   输入访问与历史写入，并正常完成fence/回收？现有跨API接口没有现成、已验证的实现。
   需要审查GPU条件执行/间接调度等能力及runtime契约，不能把普通semaphore等待当成可取消
   提交，也不能靠永不signal挂住队列、替换pending命令缓冲或只丢最终画面冒充解决。
3. **完成证明的来源。** 若考虑从真实输入边界产生fence，先确认command-list/query约束；
   不能悄悄把inline改成split再宣称保留inline性能。CPU读取GPU写入标志也必须有成立的
   可见性和寿命保证，不能省略同步仅靠volatile或“通常能读到”。
4. **独立的游戏自旋调度研究。** 已有 E33/燕云观察提出按GPU计时自动选择game wait档位，
   避免逐游戏手调；尚未实施，见[实测补充](../2026-10-14-inline-spin-results.md)。
   这是独立的调度方向，不是capture proof的替代。必须保持总预算、配置优先级和inline范围，
   先验证时间戳测量无干扰；E33的claim cap/abort为零且Adaptive wait budget无可见收益，
   不要重复把已试过的adaptive post-claim预算当成新的game wait方案。

以上均是研究方向，不是已证明可落地的下一补丁。不自动增加vendor/kernel补丁，
不放宽cap/abort、不用减少槽数、UNORM8、降低成功NR覆盖率或覆盖用户配置掩盖代价。

## 恢复研究的止步线

用户原先讨论的窗口是90分钟，最多2小时；随后明确选择**现在停止研究，记录并回退**。
没有预约任务、自动重启或后台监控。未来重新启动时可沿用以下边界：

- 前30分钟审查可行性；若必须广泛修改网络shader、大量vendor补丁或整体重构runtime，止步。
- 再60分钟做临时最小实验；正常性能与迟到输入/cap/abort安全性同时验证。
- 仅已有明确收益时最多追加30分钟交替复测；不以追加时间继续盲试。
- 继续正式实现的门槛：1440p三/四槽与修前吞吐差距不超过3%，交替复测成立，P95无明显恶化；
  同时保持槽数、格式、等待预算和成功NR覆盖率，失败路径不访问未完成输入，历史与回收有证明。
- 正式实现、完整回归、游戏/其他驱动验收另计，不把研究通过当作发布通过。

## 复现与验证入口

`tests/mochizuki/run.cmd inline-armed`（正常/timeout/ahead/1440p temporal/late/stuck/abort），
`tests/mochizuki/run.cmd inline-proof`（真实runtime恢复End证书），
`tests/lmxxf/run.cmd inline-discard`、`diag-lifetime`，
`tests/mochizuki/run.cmd exposure-recording`。HIP共用路径另用
`tests/lmxxf/run.cmd inline-gpu`，必须指定匹配源码/ABI的runtime和模型。

性能夹具 `tests/lmxxf/nr_slots_benchmark.cpp` 使用相同runtime/assets，设置
`MZ_TEST_INLINE_CHAIN=1`，参数为 `4 --mochizuki --history --gpu-handoff --poll-delay=16
--height=1440 --frames=180`。三槽改为3；split改为0；`--sweep`跑同进程正反序。
必须保留所有成功/超时/跳帧计数，报告P95，并交替跑修前/候选，不能只比较不同游戏场景。

## 回退后的最终验证（2026-10-15）

实际回退源码重新编译，按上文相同参数交替运行，先三槽 before/rollback，再四槽
before/rollback，然后逆序 rollback/before 四槽与三槽。每段180测量帧＋12预热：

| 槽数 | 修前 NR/s（正/反） | 回退版 NR/s（正/反） | 修前 GPU P95 ms | 回退版 GPU P95 ms |
|---|---|---|---|---|
| 3 | 64.429 / 64.905 | 64.471 / 64.629 | 16.036 / 15.399 | 16.166 / 15.901 |
| 4 | 64.426 / 64.443 | 64.687 / 64.516 | 16.338 / 16.220 | 16.136 / 16.065 |

全部8段 applied=180、timeout=0、skipped=0。三槽平均吞吐差约−0.18%，四槽约+0.26%；
此前新增约8–12%损失不再出现，未见此前25ms以上的GPU P95。该结果只验证本机合成
夹具的回退效果，不代表游戏FPS或长期安全验收。Mochizuki runtime SHA-256 保持
`db0fa9138fe9c8f201ff75f6a39109e8d7a6295e911e8faaf4691dc32e3044b3`。

通过的定向验证：Release x64宿主和lmxxf runtime编译；recording lifecycle、inline-discard、
exposure-recording、diag-lifetime、144种菜单布局；Mochizuki inline-armed 8组及inline-proof；
新编译HIP inline-gpu；PS5 native stderr出包脚本6项测试；模块契约40/arch、80双架构及host ci。
两份封存补丁在独立目录严格顺序应用并与保存的源码逐字核对（统一换行）。
上游审计611项通过，只刷新本地integration证据及snapshot，保留原3项暂缓，未改vendor。

未重跑ACO、实际游戏、其他驱动、长时间压力、完整Actions；没有新建交付包、推送、tag或发布。
失败路径测试仍只证明回退/回收，不证明已暂缓的输入与历史安全。完整Actions和游戏验收
不能由本文替代。
