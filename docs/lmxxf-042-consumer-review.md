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

- **同步与模块契约**：`tools/sync-lmxxf-upstream.ps1 -UpstreamRef 3a1be68c35dbac7d7edeb2073723e82076bb1a39 -SkipUpstreamFetch -LlvmPrebuiltDir exports/lmxxf-llvm23` 退出码 0；`UPSTREAM.md` 推进至 `3a1be68c35dbac7d7edeb2073723e82076bb1a39`，`sync-state.json` 状态为 `reviewed`（`modules_verified: 36f14970290b48831b1ef347e76b453719ad5cd270180429400d883b4812dde8`）；`tools/release/check-module-contract.ps1` 通过（`40 per arch / 80 dual`）。
- **RX 9070 XT 固定帧 GPU Oracle（`lmxxf_nr_gpu.exe`）**：
  - 全部新增 W16 与 byte-stream 路径零回退命中：`C512_W16_SUMMARY mode=2 w16_blocks=32 stock_blocks=0 ffn_mode=1 ffn_w16_blocks=32 ffn_stock_blocks=0 c32_mode=1 c32_w16_calls=20 c32_stock_calls=0 vit_mode=7 vit_w16_calls=16 vit_stock_calls=0 vit_inter_byte=15 vit_inter_byte_calls=36`。
  - 固定输入整帧哈希（1920x1080，seed 1）：`--output-hash` = `f05d0177e0a82a9a`，`--auto-exposure` = `b849c8ff3d30e462`，`--auto-exposure --scale16` = `b849c8ff3d30e462`，`--r10g10b10a2` = `10d1507268da9a75`；生产默认 `DLSS5_FAST_NUMERIC=1` + `DLSS5_NETWORK_FREE_RES=1`（`--size 1920 1080`）输出哈希保持 `806dd30da2c516da` 不变。
- **Temporal Mode 2/3 与宿主契约回归**：`tests\lmxxf\run.cmd abi`（23/23 通过，含 Mode 2/3 的 `MultiPass() != 1 -> low-freq-requires-mp1` 门控校验）、`lmxxf_native_temporal.exe`（WARP 下 Mode 1 与 Mode 2/3 priming、低频稳定化、残差增强零强度保持与正强度生效均通过）、`tests/host/test_config_priority.py`（12/12 通过）。

## 发布前来源复核

2026-10-16 准备 1.12.2 时，发现先前 `sync-state.json` 的配方/模块指纹不能复现。重新从固定上游 3a1be68c 生成配方并构建双架构 80 个模块：68 个 COMGR 编译，12 个 LLVM 23.1.2 预编译对象按生成源码、defines 和编译命令核验。重建对象的 SHA256 与此前提交的 80 个二进制逐个一致；元数据哈希字段大小写及清单排序存在文本差异，不能据此认定算子内容变更。

为避免 Git 的 LF/CRLF、UTF-8 BOM 转换再次使来源记录失效，配方源码/头/defines 与模块清单的验证指纹改为规范化 UTF-8/LF；实际 HSACO 校验以及 generated-source 来源哈希保持原始字节语义。新增测试在 Windows PowerShell 5.1 与 PowerShell 7 下确认文本换行/BOM 不改变指纹，而实际源代码或二进制内容变化仍会被检测。该修改不是手填新指纹：由正常同步流程重新验证构建与审阅后写回状态。

首次同步重建成功，但运行时编译因未初始化标准 MSVC/SDK 环境而缺少 wrl/client.h，中止并保留 pending。后续使用 tests/_lib/msvc-env.cmd 初始化环境，只复用这次实际重建的模块继续验证，不使用跳过编译或陈旧模块豁免。游戏/长时间运行等现有暂缓项保持原样。

发布测试层已接入 native temporal WARP 的 Mode 2/3 回归；该专项及完整 host ci 通过。菜单新增中文提示改用现有字体支持的措辞，完整菜单布局/字形渲染回归通过。

后续标准环境同步退出码 0，运行时重新编译成功，状态恢复 reviewed；实际重新计算的 recipes_verified 与 modules_verified 均匹配记录，未使用跳过项或陈旧模块豁免。仍保留 3 项既有显式暂缓，不将本次验证描述为全游戏/长时间验收。
