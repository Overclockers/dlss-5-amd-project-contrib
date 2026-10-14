# NR inline 测试结果汇总（2026-10-14，Yanyun 1440p，RX 9070 XT）

## 1. inline 差距来源（NrFrameDiag GPU 时间戳）
- 差距在 NR 网络启动晚（handoff），不在网络本身。
- handoff：split 0.32 ms / fixed100 1.35 ms / exp 2.28 ms；网络 core split 6.10 / fixed100 6.27 ms。
- event 模式（NrCaptureBackoff=2）：只轮询 1 次/帧但不更快（18.56 vs fixed100 18.11 vs split 16.34 ms），排除 capture 发现慢。
- NrInlineReuse 游戏内从不命中，无效（夹具高估了录制成本，游戏内 ~0.85 ms vs 夹具 ~3.5 ms）。

## 2. GPU-armed handoff（NrGpuHandoff）
- 游戏内 p50：split 16.93 / armed 17.04 / fixed100 18.43 / exp 21.06 ms；handoff 0.36 vs 0.35 ms。
- PresentMon 06/07：split 15.80 ms 63.3 FPS p95 16.8；armed 16.38 ms 61.1 FPS p95 19.7；GPUBusy 15.73 vs 16.36（armed 多 ~0.6 ms GPU，来自自旋占用）。

## 3. 自旋开关实测（spinexp 包 -171625-140，PresentMon）
| 段 | 设置 | p50 | FPS | p95 | GPUBusy | >50ms |
|---|---|---|---|---|---|---|
| 08 | split | 15.90 | 62.9 | 16.92 | 15.86 | 1 |
| 09 | armed 基线 | 16.24 | 61.6 | 21.86 | 16.08 | 4 |
| 10 | Game wait dispatches 256 | 16.01 | 62.5 | 19.41 | 16.04 | 1 |
| 11 | Spin poll delay 16 | 15.95 | 62.7 | 19.26 | 15.95 | 0 |
| 12 | Side watch start delay 4 | 16.31 | 61.3 | 19.41 | 16.35 | 0 |
| 13 | Side watch dispatches 256 | 16.27 | 61.5 | 19.30 | 16.27 | 0 |
| 14 | split | 15.97 | 62.6 | 16.95 | 15.93 | 0 |
| 15 | poll16 + wait256 | 16.04 | 62.3 | 19.25 | 16.07 | 0 |
- 结论：Spin poll delay 16 最好，平均帧已追平 split；组合无额外收益；side watch 两项无效。
- 离线：slices=8、delay256+slices8、delay1024 出现 200–680 ms 卡顿，不用。

## 4. 剩余 p95 慢帧
- poll16 下显示间隔 p95 16.56 ms ≈ split，画面平稳；慢帧是 CPUBusy +~3.5 ms（19.1 vs 15.6），下一帧快 ~2.7 ms 补回，GPU 无空泡。
- NR 各段慢帧/正常帧相同，无超时/bypass/history reset。
- 慢帧率：split 0.1–0.3%，inline ~10%（约每 7 帧一次）。armed 基线（无 poll16）是真慢（显示 p95 21.5）。
- 推测：游戏线程等 GPU fence 被自旋带抖，未证实。

## 5. 当前状态
- 分支 work/nr-spin-gpu-cost（代码 @5f5b61a7，之后仅文档提交），包 dist\OptScaler-NR-1.12.0-alpha2-local-20261014-183446-263.zip。
- 菜单：inline 开才显示；armed 默认开；Spin poll delay 默认 16；armed 关则 spin 选项变灰；armed 开则 backoff/reuse 变灰。spin 选项只在 armed 开时生效。
- 新诊断 NrGameWaitDiag（菜单 Game thread wait timing，需 NrFrameDiag），字段 gw_*。已测 16（split）/17（armed+poll16），结果见第 6 节。

## 6. 剩余 inline 慢帧：游戏线程等待（gw_*，16/17 段）
- 注意：gw_* 数字来自 HDR 下的 16/17 段（IsHdr true，DXGI 10），保留不改；绝对值不能直接与 08–15 比，同局 split/inline 相对差有效。
- inline 慢帧（显示间隔 >18.5 ms，约 16%）to_ecl 多 ~3.0 ms = fence 等待 +1.6 ms（5.08 vs 3.50）+ 等待后游戏 CPU +1.65 ms。
- present_cpu：inline 每帧 1.72 vs split 0.85 ms（+0.9 ms CPU，每帧都有）。
- 慢帧与正常帧的 NR GPU 各段相同；显示 p95 16.56 vs split ~16.6 ms，画面看不出；下一帧补回。
- 结论：影响小，暂搁置。后续可做：(a) 记录游戏等待的 fence 值/放行时间，与 qpc_game_wait_exit 对齐；(b) 拆分 Present +0.9 ms。先在 SDR 下复核。

## 7. HDR vs SDR（18–22，纠正此前「HDR 让 split 慢 ~0.5 ms」）
- 包 2c10ad5e 在 NrFrameDiag 增加 `gpu_input_encode_ms`（未跑记 -1）和 `gpu_transfer_ms`（两者都在 `gpu_core_ms` 内；可能落后几帧，只看中位）。
- PresentMon：
  - 18 HDR NR 关：p50 10.45 / GPUBusy 10.18
  - 19 HDR split：16.58 / 16.55
  - 20 SDR NR 关：10.33 / 10.22（同局先录 21 再录 20）
  - 21 SDR split：16.64 / 16.59
  - 22 SDR split 重录：16.17 / 16.13（61.8 FPS）
- 游戏自身 HDR 成本 ≈ 0（GPUBusy 10.18 vs 10.22）；帧级 NR HDR 成本 ≈ 0.00（两边都 +6.37 ms）。
- NrFrameDiag split 中位 HDR vs SDR：gpu_core 6.214 vs 6.030（+0.184）= input encode 0.049 + transfer +0.081（0.151 vs 0.070）+ 余约 0.054（16 位网络本身）；capture 0.051 vs 0.032；handoff 0.336 vs 0.287；writeback 0.023 vs 0.014；capture_to_resume 6.549 vs 6.321。Kenny overlay NR 时间 SDR 下约 6.0x ms，与此一致。
- 结论：HDR 在 NR 内多约 0.18 ms，是预期工作（编码、transfer 混回、16 位流量），不是 bug；可优化空间约 0.05–0.1 ms，现阶段不值得。同包 split GPUBusy 各次在 16.13–16.59（对照段 14 的 15.93），此前「新包拖慢 split」是 NR 以外的局间波动，不是代码。HDR 下 Input encoding=Encoded 更快，但把 HDR 当 SDR 喂网络，不建议。
- 日志归档：`Desktop\nr-pm\hdr-18-19`、`sdr-20-21`。
