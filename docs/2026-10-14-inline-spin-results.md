# NR inline 测试结果汇总（2026-10-14，燕云 1440p，RX 9070 XT）

> 范围：只针对这台 RX 9070 XT、燕云当前测试场景。每个设置一般只有一段约 60 秒录制，没有交替复测；同包同设置的跨局 GPUBusy 波动可达 ~0.46 ms（21 vs 22）。下面的排名和因果结论都只代表**本轮观察**。

## 0. 结论（本轮收尾）
在燕云、RX 9070 XT 的当前测试场景中，armed＋poll16 已将 inline 与 split 的差距缩小到实际体验可接受的程度。本轮性能优化收尾；跨游戏、跨 GPU 的收益，以及剩余 Present 抖动原因尚未验证。

- 默认值（用户决定）：开 inline 时 NrGpuHandoff=true、NrSpinPollDelay=16。NrInline 本身仍默认关。
- 剩余慢帧：Present 间隔仍有 ~10% 超过 18.5 ms，但显示间隔基本没有；当前场景未感到明显影响，暂缓追查（见第 4、6 节）。

## 指标口径
- **Present 间隔**：PresentMon `MsBetweenPresents`（游戏提交节奏）。**显示间隔**：`MsBetweenDisplayChange`（画面节奏）。两者不能混用。
- p50 = 中位数；mean = 平均值；p95 线性插值。
- **FPS = 实际吞吐**（帧数 / 时长 ≈ 1000 / Present 间隔平均值）。旧版表格里的 FPS 是 1000/p50，偏高（如 09 段写 61.6，实际约 59.6）。
- 数据来源：GPT 审阅（`work/notes/2026-10-14-inline-spin-review.md`）对原始 CSV 的重算。

## 1. inline 差距来源（NrFrameDiag GPU 时间戳）
- 本轮观察：handoff（网络启动晚）是主要瓶颈之一；不能证明是唯一原因。
- handoff：split 0.32 ms / fixed100 1.35 ms / exp 2.28 ms；网络 core split 6.10 / fixed100 6.27 ms（core 本身也有差别）。
- event 模式（NrCaptureBackoff=2）：每帧只轮询 1 次，但没有更快（p50 18.56 vs fixed100 18.11 vs split 16.34 ms）。这不能排除 capture 发现延迟，只说明 event 这种改法本轮没带来收益。
- NrInlineReuse：本轮游戏内没有命中，未见收益（夹具高估了录制成本：游戏内 ~0.85 ms vs 夹具 ~3.5 ms）。

## 2. GPU-armed handoff（NrGpuHandoff）
- 游戏内 p50：split 16.93 / armed 17.04 / fixed100 18.43 / exp 21.06 ms；handoff 0.36 vs 0.35 ms。
- PresentMon 06/07（约 30 秒，早于 spinexp 包）：split p50 15.80 p95 16.8；armed p50 16.38 p95 19.7；GPUBusy 15.73 vs 16.36。armed 多出的 ~0.6 ms GPUBusy 可能与自旋占用有关，但 GPUBusy 差值本身不能单独证明来源（core 也有差别）。

## 3. 自旋开关实测（PresentMon，SDR）
| 段 | 设置 | 包 / 代码 | Present p50 | Present mean | FPS（吞吐） | Present p95 | 显示 p95 | GPUBusy p50 | Present >18.5 | 显示 >18.5 |
|---|---|---|---|---|---|---|---|---|---|---|
| 08 | split | -171625-140 / 3a159591 | 15.90 | 15.93 | 62.8 | 16.91 | 16.50 | 15.86 | 0.32% | 0.03% |
| 09 | armed 基线 | 同上 | 16.23 | 16.77 | 59.6 | 21.84 | 21.48 | 16.08 | 18.91% | 7.58% |
| 10 | armed + Game wait dispatches 256 | 同上 | 16.01 | 16.10 | 62.1 | 19.39 | 16.65 | 16.04 | 11.44% | 0.22% |
| 11 | armed + Spin poll delay 16 | 同上 | 15.95 | 15.99 | 62.5 | 19.25 | 16.56 | 15.95 | 9.61% | 0.21% |
| 12 | armed + Side watch start delay 4 | 同上 | 16.31 | – | – | 19.41 | – | 16.35 | – | – |
| 13 | armed + Side watch dispatches 256 | 同上 | 16.27 | – | – | 19.30 | – | 16.27 | – | – |
| 14 | split（新进程） | 同上 | 15.97 | 16.00 | 62.5 | 16.94 | 16.61 | 15.93 | 0.11% | 0.03% |
| 15 | armed + poll16 + wait256 | 同上 | 16.04 | 16.09 | 62.1 | 19.24 | 16.63 | 16.07 | 9.94% | 0.00% |

（12/13 审阅没有重算，保留旧表的 p50/p95/GPUBusy；10–12 段录制中切到前台过几秒。）
- 本轮观察：poll16 的平均帧时间接近 split，显示 p95 明显好于 armed 基线；但 Present 间隔分布没有完全追平（p95 19.25 vs 16.9）。
- 本轮未显示明确收益：wait256、组合、两个 side watch 选项。差值多在跨局波动范围内，不代表在别的场景无效。
- start delay（12 段）录制时 ALU 延迟和 watch 之间没有依赖，延迟不一定生效；现已加 barrier（见第 8 节），12 段结论不成立，需要的话可重测。
- 离线：slices=8、delay256+slices8、delay1024 出现 200–680 ms 卡顿，不用。

## 4. 剩余 Present 慢帧（11 段，SDR）
- 显示间隔 p95 16.56 ms，与 split（16.50/16.61）接近；Present 间隔 >18.5 ms 约 9.6%，显示间隔 >18.5 ms 只有 0.21%。
- 慢帧时 CPUBusy 多 ~3.5 ms（19.1 vs 15.6），下一帧快 ~2.7 ms 补回；GPU 前面排着约两帧，未见 GPU 空泡。
- NR 各段在慢帧/正常帧相同，没有超时/bypass/history reset。
- armed 基线（09，无 poll16）是显示端也慢（显示 p95 21.48）。
- 原因未证实（推测与游戏线程等待 GPU fence 有关）。当前场景未感到明显影响，暂缓。

## 5. 版本记录（每段对应的包）
| 段 | 包 | 代码 | HDR | 备注 |
|---|---|---|---|---|
| 06/07 | spinexp 之前的包 | – | SDR | ~30 秒 |
| 08–13 | `-171625-140` | 3a159591 | SDR | 同一进程（PID 22912），每段约 60 秒 |
| 14–15 | `-171625-140` | 3a159591 | SDR | 新进程（PID 17584） |
| 16–17 | `-183446-263` | 5f5b61a7 | HDR | 开 NrFrameDiag + NrGameWaitDiag（PID 32560） |
| 18–19 | `-194448-681` | 2c10ad5e | HDR | 2c10ad5e 改了 runtime 时间戳和信息结构 |
| 20–22 | `-194448-681` | 2c10ad5e | SDR | 先录 21 再录 20；22 是另一进程重录 |
- 之前写的「代码 @5f5b61a7，之后仅文档提交」是错的：2c10ad5e 改了 runtime 和时间戳。
- 菜单（现状）：inline 开才显示 inline 组；GPU-armed handoff 及 spin 选项只对 Mochizuki 显示（lmxxf 不使用它们）；armed 关则 spin 选项变灰；armed 开则 Capture poll backoff 变灰（确实被绕过）；Reuse 不再变灰（armed 下仍使用资源池）。

## 6. 剩余 inline 慢帧：游戏线程等待（gw_*，16/17 段，HDR）
- 注意：16/17 是 HDR 段，绝对值不能和 08–15 比。
- 17 段：Present 间隔 >18.5 ms 约 16.4%，显示间隔 >18.5 ms 为 0%（显示 p95 17.21，16 段 split 16.97）。旧版把 16.4% 写成显示间隔，是错的；16.56 那个显示 p95 是 11 段的，不能用于 16/17。
- 17 段 Present 慢帧（gw_present_ms >18.5）：to_ecl 中位 12.84 vs 9.84 ms；fence 等待中位 5.08 vs 3.50 ms。这只说明慢帧和等待变长**相关**，不能确定原因。`to_ecl − wait` 不等于纯游戏 CPU 工作时间。
- Present 内耗时（旧字段 present_cpu_ms，现改名 `gw_present_wall_ms`）：inline 1.72 vs split 0.85 ms。这是 Present 包装函数的**墙钟时间**，包含阻塞、限帧和被抢占，不能写成「每帧多 0.9 ms CPU」。
- gw_* 的局限（5f5b61a7 版本）：只记一个 pending event（第二次注册会覆盖）；零超时探测后再阻塞等待会漏记；不覆盖 WaitForMultipleObjects；关了再开不清状态；Present1 没有采样；gw 区间与 NR 帧的对应没有校验。本次已修前四项中的大部分并给 Present1 补上采样（见第 8 节），16/17 的数据仍是旧版本采的。
- 结论：当前场景未感到明显影响，暂搁置。后续可做：(a) 记录游戏等待的 fence 值/放行时间，用新加的 `gw_begin_qpc/gw_end_qpc` 与 NR 帧对齐；(b) 拆分 Present 墙钟时间的组成。先在 SDR 下复核。

## 7. HDR vs SDR（18–22）
- 2c10ad5e 在 NrFrameDiag 增加 `gpu_input_encode_ms`（没跑记 -1）和 `gpu_transfer_ms`（都在 `gpu_core_ms` 内；可能落后几帧，只看中位）。
- PresentMon（p50 / GPUBusy）：18 HDR NR 关 10.45 / 10.18；19 HDR split 16.58 / 16.55；20 SDR NR 关 10.33 / 10.22；21 SDR split 16.64 / 16.59；22 SDR split 重录 16.17 / 16.13。
- 本轮观察：游戏自身 HDR 成本 ≈ 0（GPUBusy 10.18 vs 10.22）；帧级 NR 增量两边都约 +6.37 ms。
- NrFrameDiag split 中位 HDR vs SDR：gpu_core 6.214 vs 6.030（+0.184）；其中 input encode 0.049、transfer 0.151 vs 0.070（+0.081），余约 0.054 ms 来源未单独验证（可能是 16 位输入流量）。capture 0.051 vs 0.032；handoff 0.336 vs 0.287。
- HDR 在 NR 内多约 0.18 ms，属于预期的额外工作，不是 bug；具体可优化空间没有验证，现阶段不追。
- 同包 split GPUBusy 在 16.13–16.59 之间波动（14 段 15.93）。跨局波动说明「新包拖慢 split」证据不足，但也不能反过来证明一定不是代码。
- HDR 下 Input encoding=Encoded 更快，但相当于把 HDR 当 SDR 喂给网络，不建议。
- 日志归档：`Desktop\nr-pm\hdr-18-19`、`sdr-20-21`。

## 8. 审阅后的安全修复（本次提交）
1. **capture 迟到 / 没出现（P1）**：armed 时，游戏在拷贝 NR 输入前先在 GPU 上「认领」一个输入闸门（capture 缓冲 +4：0 开、1 游戏已认领、2 侧队列已关），输入拷贝用 D3D12 predication 挂在认领结果上。侧队列 watch 用完预算后先关闸门：游戏还没认领，就跳过这帧的输入拷贝，网络只读到旧内容，且输出被丢弃（原画面 + history reset），不会和游戏同时读写输入；游戏已认领，则侧队列再有界地等它的 capture（16 段），然后才放行网络。seen 现在要求「capture 已到 且 闸门为 1」。无 CPU 同步等待，也不退回 split。剩余风险：游戏认领后拷贝极慢、超过额外等待预算时仍可能竞争（理论上，未观察到）。
   - 夹具：`nr_inline_runtime_gpu --gpu-handoff --poll-delay=16 --late-capture=64` 在游戏队列认领前插入长 ALU 延迟，要求闸门=2、seen=0、输出为原画面、按超时回收（history reset）、不挂起。`tests\mochizuki\run.cmd inline-armed` 跑 armed+poll16 的默认组合（普通 / timeout / ahead / large temporal）和这个故障用例；`inline` 也包含它们。
2. **诊断钩子安装（P1）**：NrGameWaitDiag 安装时枚举本进程所有其他线程并逐个 DetourUpdateThread；只有 commit 成功才标记已安装；Attach/Commit 失败会 Abort、记一次日志，之后最多重试 3 次。
3. **gw_* 采样**：最多 8 个 pending event；零超时探测不计入、也不清掉 pending；只有等待返回 signalled 才移除；关了再开清空状态；新增 `gw_begin_qpc/gw_end_qpc`；`gw_present_cpu_ms` 改名 `gw_present_wall_ms`；Present1 也采样。WaitForMultipleObjects 仍不覆盖。
4. **菜单**：见第 5 节。
5. **Side watch start delay**：ALU 延迟之后加 UAV barrier，watch 确实在延迟完成后才开始。
6. 诊断关闭时的开销：没有新增同步等待、不写新增的阶段时间戳；但配置/原子检查一直在，开过一次后钩子保持安装（直通），query pool 也已扩大。所以准确说法是「无新增同步等待」，不是「零开销」。