# lmxxf 3a1be68 消费审阅

范围：已完成 pin `48a41fccb89300cd6636b16bc7b86010384c4cc1` 到锁定目标 `3a1be68c35dbac7d7edeb2073723e82076bb1a39`。逐项覆盖在 `third_party/lmxxf/upstream-review.json`。本文记录进入产品的结论与实际验证证据。

## 他实际打开的

0.42 发布说明和 `hip/build-modules.ps1`、`hip_reference_network.h` 的默认值一致：正式包默认仍是 MODE0、MP1、不打开已停止的实验。相对 `48a41fcc`，源码默认打开或已接入产品的能力包括：

- `HIP_VIT_INTER_BYTE` 默认 15，`HIP_C512_W16` 默认 2，`HIP_C512_FFN_W16` 默认 1，`HIP_C32_W16` 默认 1，`HIP_VIT_W16` 默认 7。
- `vit-stream` / `vit-stream-fast` 配方增加 `HIP_BYTE_F_ADD0 1`。
- 新增紧凑 QKV 注意力与 W16 重排（`c512-m32-deep`、`c512_qkv_attention_compact.inc`、`c512_qkv_attention_fused.inc`、`packed_weights.h`）。
- 桥接新增 `RequestNeverHistory` 和共享资源计数。共享池仍是未设置 `DLSS5_HIP_SHARED_POOL=0` 时保留 import。产品不调用 `RequestNeverHistory`：History 可以在会话中切换，不能因为某一帧关掉 History 就删掉 buffer。
- **Temporal Mode 2（低频时间稳定）与 Mode 3（单遍残差增强 + 强度 0..2）**：同步 `src/native_low_frequency_shader.h` 与 `src/native_temporal_mode.h`，在 `NativeTemporalHistory.h`（`LmxxfNativeTemporal::LowFrequency`）和 `LmxxfNrRuntime.cpp`（第 1684–1829 行 `ParseRuntimeTemporalMode`/`PrepareHistory`/`BeginHistory`）中基于调用方录制命令列表实现 Mode 2/3，并接入宿主 `Config`/`DlssNr_Menu`/`OptiScaler.ini`（宿主统一模式枚举 `LmxxfTemporalMode` 默认 `1` 即完整模型历史，兼容 `LmxxfModelHistory=true`；Mode 2/3 为显式 opt-in；`LmxxfTemporalEnhanceStrength` 默认 `1.0`；未带 `LMXXF_NR_TEMPORAL_MODEL_HISTORY` 标志时运行时直接走 `Off`）。Mode 0/1 时不创建或 dispatch 低频管线；Mode 2/3 要求 `MultiPass() == 1`，层数不为 1 时状态设为 `low-freq-requires-mp1` 并直接跳过历史（不会回退跑多层模型历史）。

双架构 80 个 `.hsaco` 模块（12 个 WSL LLVM 23.1.2 预编译模块 `exports/lmxxf-llvm23` + 68 个 COMGR 模块，覆盖 `gfx1200` 与 `gfx1201` 各 40 个）已从 `3a1be68c35dbac7d7edeb2073723e82076bb1a39` 完整重编并同步至 `third_party/lmxxf/modules` 与 `third_party/lmxxf/hip/SHA256SUMS`，所有新增 W16 / inter-byte / compact QKV 导出均已在运行时激活。

## 保持关闭

- 上游 addon 的 Mode 1 快速历史门控（`DLSS5_FAST_HISTORY`、`DLSS5_FAST_HISTORY_DEPTH_INVERTED`、`DLSS5_TEMPORAL_HISTORY_EXPERIMENT`、`DLSS5_TEMPORAL_MV_UNJITTERED`）与独立包装头 `src/native_low_frequency_temporal.h` / `src/native_temporal_experiment.h` 不接入；多层 History 继续使用产品自己的 `LmxxfNativeTemporal::History` 实现。
- 发布说明标明停止的项不打开：V 末 barrier、prefix NT、C32 NoHi、C512 少 New、tile-ready、graph 替换、原生 D3D12 网络、人物分割。
- `submit_pulse` 仍由 `LmxxfProductionOptions` 在环境覆盖之后写回 0。`experimental_temporal` 仍为 false。
- `Development/results` 与 `Development/HIP/experiments` 不在同步闭包里，也不进 `build-modules.ps1` 的源文件列表。

## 验证

- **同步与模块契约**：`tools/sync-lmxxf-upstream.ps1 -UpstreamRef 3a1be68c35dbac7d7edeb2073723e82076bb1a39 -SkipUpstreamFetch -LlvmPrebuiltDir exports/lmxxf-llvm23` 退出码 0；`UPSTREAM.md` 推进至 `3a1be68c35dbac7d7edeb2073723e82076bb1a39`，`sync-state.json` 状态为 `reviewed`（`modules_verified: b4a0745e34580da489a3d0a62ac6a723834321553a5f93800dc3d6c7df11aed5`）；`tools/release/check-module-contract.ps1` 通过（`40 per arch / 80 dual`）。
- **RX 9070 XT 固定帧 GPU Oracle（`lmxxf_nr_gpu.exe`）**：
  - 全部新增 W16 与 byte-stream 路径零回退命中：`C512_W16_SUMMARY mode=2 w16_blocks=32 stock_blocks=0 ffn_mode=1 ffn_w16_blocks=32 ffn_stock_blocks=0 c32_mode=1 c32_w16_calls=20 c32_stock_calls=0 vit_mode=7 vit_w16_calls=16 vit_stock_calls=0 vit_inter_byte=15 vit_inter_byte_calls=36`。
  - 固定输入整帧哈希（1920x1080，seed 1）：`--output-hash` = `f05d0177e0a82a9a`，`--auto-exposure` = `b849c8ff3d30e462`，`--auto-exposure --scale16` = `b849c8ff3d30e462`，`--r10g10b10a2` = `10d1507268da9a75`；生产默认 `DLSS5_FAST_NUMERIC=1` + `DLSS5_NETWORK_FREE_RES=1`（`--size 1920 1080`）输出哈希保持 `806dd30da2c516da` 不变。
- **Temporal Mode 2/3 与宿主契约回归**：`tests\lmxxf\run.cmd abi`（23/23 通过，含 Mode 2/3 的 `MultiPass() != 1 -> low-freq-requires-mp1` 门控校验）、`lmxxf_native_temporal.exe`（WARP 下 Mode 1 与 Mode 2/3 priming、低频稳定化、残差增强零强度保持与正强度生效均通过）、`tests/host/test_config_priority.py`（12/12 通过）。
