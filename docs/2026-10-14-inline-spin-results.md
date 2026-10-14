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
- 分支 work/nr-spin-gpu-cost @5f5b61a7，包 dist\OptScaler-NR-1.12.0-alpha2-local-20261014-183446-263.zip。
- 菜单：inline 开才显示；armed 默认开；Spin poll delay 默认 16；armed 关则 spin 选项变灰；armed 开则 backoff/reuse 变灰。spin 选项只在 armed 开时生效。
- 新诊断 NrGameWaitDiag（菜单 Game thread wait timing，需 NrFrameDiag），字段 gw_*。待测：split vs armed+poll16，两段都开诊断。
