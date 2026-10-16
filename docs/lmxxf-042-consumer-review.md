# lmxxf 3a1be68 消费审阅

范围：已完成 pin `48a41fccb89300cd6636b16bc7b86010384c4cc1` 到锁定目标 `3a1be68c35dbac7d7edeb2073723e82076bb1a39`。逐项覆盖在 `third_party/lmxxf/upstream-review.json`。本文只记会进产品的结论。

## 他实际打开的

0.42 发布说明和 `hip/build-modules.ps1`、`hip_reference_network.h` 的默认值一致：正式包仍是 MODE0、MP1、不打开已停止的实验。相对旧 pin，源码默认打开且带旧模块回退的是：

- `HIP_VIT_INTER_BYTE` 默认 15，`HIP_C512_W16` 默认 2，`HIP_C512_FFN_W16` 默认 1，`HIP_C32_W16` 默认 1，`HIP_VIT_W16` 默认 7。对应导出不存在时 `HasFn` 继续走原来的 kernel。
- `vit-stream` / `vit-stream-fast` 配方增加 `HIP_BYTE_F_ADD0 1`。
- 紧凑权重和 W16 重排只在新导出存在时选用。
- 桥接新增 `RequestNeverHistory` 和共享资源计数。共享池仍是未设置 `DLSS5_HIP_SHARED_POOL=0` 时保留 import。产品不调用 `RequestNeverHistory`：History 可以在会话中切换，不能因为某一帧关掉 History 就删掉 buffer。

这些源码已经在 vendor 树里。当前 40/架构模块仍是旧 pin 的二进制。没有 LLVM 23.1.2 预编译目录（`exports/lmxxf-llvm23` 不存在），这次没有重编模块，所以新布局还不会在游戏里生效。

## 保持关闭

- `DLSS5_TEMPORAL_MODE` 上游默认 0。Mode 2/3 是可选实验，不接入产品菜单，不分配、不 dispatch。已有多层 History 继续用产品自己的实现，不改成上游 addon 的 Mode 1。
- 发布说明标明停止的项不打开：V 末 barrier、prefix NT、C32 NoHi、C512 少 New、tile-ready、graph 替换、原生 D3D12 网络、人物分割。
- `submit_pulse` 仍由 `LmxxfProductionOptions` 在环境覆盖之后写回 0。`experimental_temporal` 仍为 false。
- `Development/results` 与 `Development/HIP/experiments` 不在同步闭包里，也不进 `build-modules.ps1` 的源文件列表。

## 验证

- 配方是否打开，以审阅报告里每个 kernel 的 `recipe_and_local_defines` / `implicit_recipe_candidate`，以及源码 `#define` 是否为字面 `1` 为准。
- 产品没有调用 `NativeHipNetwork` 或 `RequestNeverHistory`。
- 模块字节、固定帧 oracle、gfx1200 设备都没有在这次跑。下一步：备好 LLVM 23.1.2/COMGR 后重编 gfx1200 和 gfx1201，再跑模块契约和一份固定输入整帧对照。在那之前不能把新布局说成已经生效。
