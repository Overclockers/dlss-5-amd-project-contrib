# Mochizuki 0.0.7 剩余能力统一评估

日期：2026-10-19。产品基线为 `7e6f276a`，核心接入提交为 `471d26c6`；
原版固定为 `cc27501dc116bdcfbb43612e497c5ca12497574a`。
本次一起评估全部剩余项，没有启用新的产品路径，也不推进 Inline 调研。

## 结论

| 剩余项 | 取舍 | 实际用途与限制 |
|---|---|---|
| 同模型跨尺寸/格式 GraphCore、权重复用 | 建议接入受限后台复用 | 减少兼容尺寸变化时的重复降图、打包、权重上传和新旧网重叠内存；不是降低固定尺寸每帧网络算术成本。保留现有 buckets/settle、完成证明和失败继续旧网。 |
| plan-only + with_slot_of、预留容量 | 作为同一复用方案的扩展，不预设大容量 | 既有 slot 足够时可装入另一份同模型 plan；未缓存 plan 仍有 CPU 降图成本，容量增加会常驻额外显存，不能把按需方案变成一律预留。 |
| 跨模型 activation arena 共享 | 建议纳入同一实现方案，但限同 Session/device、受证明的 split 提交顺序 | 主要节省切换模型期间新旧网重叠的 activation 占用；不共享不同模型的权重。无需为了获得该收益额外建立永久多模型缓存。 |
| deferred 构造 + 双区域 image heap | 不直接接入原版策略 | 第三个 runtime 会让第一份 runtime stale；与可长期保留、以后再回放的本地 recording 冲突。若后续有图像分配瓶颈，使用完成证明驱动的空闲池，而非固定两区域强制轮换。 |
| park / resident / RAM offload | 当前产品不增加这套缓存及菜单 | 我们退休旧网，原版保留多个闲置网络。没有闲置缓存就没有需要卸载的对象；新增缓存会先增加占用及状态复杂度。关闭 NR 的现有释放行为仍应另行遵循本地所有权。 |
| 全局 SharedPipeline registry | 保持产品关闭 | 路径、device、bindings、push size、robustness 的 key 不表达全部编译来源；命中还绕过实际 import/capture。现有 ACO 用本次 imported 计数判断成功，不能只改开关。非 deferred 的 GraphCore 复用不要求开启该全局缓存。 |
| motion point | 保留当前产品采样；若追原版语义，须成套修改输入链 | 原版 NGX 选择 point、ReShade 保持 bilinear；我们有共享直读、buffer 上传、LINEAR rescale、bucket padding、多 pass 等路线。后级 point 无法撤销前级插值。它是画质语义选择，不列入已证实性能收益。 |
| resize history / carry_history | 保持尺寸变化时重置 | 原版也是 opt-in。当前 history 连续性包含 network、尺寸、队列、帧号及回放重置；不能因共享内存就取消重置。跨尺寸保留历史需要运动画质对照，且核心仅支持单 pass。 |

以上是本次评估的取舍，不表示建议项已实现。基础版本可继续游戏验收；后续修改可以按同一整体方案实现和验收，无需再逐项请求评估。

## 本地离线证据

RX 9070 XT，Windows 驱动 `32.0.31041.1004`。临时 Vulkan 探针链接最终 0.0.7
产品 core 对象，采用普通 native 编译、线性 RGBA16F 输入、单 pass、无 temporal history。
每次提交等待完成后才进行下一次操作；变化输入逐次比较输出哈希，没有以旁路冒充网络运行。
没有游戏/Present、ACO/INT4 共享矩阵、异步重叠、取消、设备丢失或长期压力验收。

- `256x256 → 256x255`：复用与原 runtime 的 `network_id` 相同，3 组输入输出与独立构建一致。
  一次暖缓存探测中，独立构建约 2649 ms，共享 graph/权重约 53 ms；
  保留产品的 private pipelines 时约 221 ms，3 组输出仍相同。
  这些是构造耗时样本，缓存/配置不构成严格配对产品基准，不能承诺游戏缩短同等时间。
  初次冷构建约 79.6 s，单独记录，不与暖构建混算。
- 另一份 `128x128` plan 可装入既有 slot，`network_id` 相同，3 组输出与独立构建一致。
  不把源尺寸不同直接称为 working extent 不同；是否适配由 plan/slot 的实际容量决定。
  512x512 的新 plan（工作范围 576x512）在旧 slot 放不下时被拒绝。
- 无余量时，Original arena 为 25,808,384 字节，Small 为 26,027,776 字节；
  向 Small 提供前者时确实退回独立分配。此项没有算作共享验证成功。
  原版 `act_slack=1.06` 后，Original 提供 27,356,928 字节，Small 真正采用相同 `act_id`；
  交替执行两模型的 3 组变化输入均与各自独立结果一致。
  相对两份无余量 activation 合计，理论少分配 24,479,232 字节，约 23.35 MiB；
  这是本尺寸的核心分配计算，不是整个游戏显存实测，权重、图像、帧载体不计入该节省。
- 双区域 heap 的 region 为 4,456,448 字节，共 8,912,896 字节。
  建第三个 deferred runtime 后，第一个 `stale()==true`，第二、三个仍有效；
  有效第三个的输出与独立路线一致。保留 C++ 引用并不能阻止底层区域被复用。
- 无 runtime/在途工作、只剩 core 时，offload/reload 后输出一致；
  下载到 RAM 的权重为 152,958,160 字节。多次观察下载约 651–800 ms，
  恢复上传约 77–89 ms，之后重建 runtime 约 44–65 ms；未计完整游戏恢复首帧。
  它能工作，但既不是热路径优化，也不等于当前产品需要增设闲置模型缓存。

## 建议实现的统一边界

1. 保留 split 的 Session、录制和退休框架，只在后台构造入口选择可复用 core/arena。
   同模型匹配除尺寸外仍核对模型/INT4、ACO、scale、capacity、preprocess、线性解释、
   native compose/first-pass、转换路径等实际配置。格式变化重建自己的图像与 descriptors，
   不能因 core 可复用而扩大 Plan A/共享输入的格式准入。
2. 使用非 deferred 构造，保留 `share_pipelines=false`。不在渲染线程调用完整构造、
   working-extent 搜索、plan-only 降图、上传或 offload；本地探针的 53/221 ms 也不适合塞进一帧。
3. 预算按真正新增的唯一所有者计费：共享网络/activation 不重复收费，新增 per-runtime 图像、
   载体和 descriptor 仍收费；复用失配或失败回到既有完整构建/退避路径。
   不一律增加容量，不用两份永久载体来换取临时切换收益。
4. 跨模型 sharing 只在同一 device 的执行序列中选择足够大的 arena，优先现有空余容量；
   余量必须进入预算。核心 `retarget` 会清零 arena，可能重载 tail/noise，
   这是模型交替的额外工作，尚无游戏净耗时收益数据。后台构建不触碰仍在使用的 arena，
   也不得复用到别的 Session/device 或未排序的队列。
5. 已关闭、未提交、在途、回放的 recording 保留原 runtime 及各自图像；
   不引入 stale heap。split 在实际 enqueue 时录制 Vulkan 工作，为共享顺序提供可用接点，
   但仍需覆盖取消、提交失败和跨 D3D12 队列等待，不能仅依赖 CPU 调用顺序。
6. ACO/INT4 复用必须验证本次 import、有效模型和整图 native fallback；
   descriptor/管线私有与网络内存共享是不同层次。原版全局缓存需要编译来源 key、
   命中时的来源证明/统计和 capture 语义完整设计，当前收益不依赖先开放它。
7. 保留内部 A/B/回滚与精确执行计数，复用现有 split、保留录制、预算/OOM、
   扩容失败、模型切换夹具；临时探针用于补充，不新增 tracked 测试项目。
   变化输入、非零 motion、temporal、多 pass、RenoDX、native/ACO/INT4 和回放输出是产品验收条件。
   稳态 NR 时间、切换等待、实际执行率、峰值/稳态显存分别比较，不再以构造速度替代 FPS。

## 源码依据

- 核心 API 与约束：`third_party/mochizuki/windows/src/core/nr_runtime.hpp` 中的
  `RuntimeConfig::share/deferred/capacity/act_arena`、`share_for/with_slot_of`、offload 和 history 注释。
- 实现：同目录 `nr_runtime.cpp` 的共享配置校验、heap generation、offload/reload、`stale`；
  `nr_graph.cpp` 的 `build_shared`、`retarget`、activation 容量校验；
  `nrvk.hpp` 的 `ImageHeap` 和 `Kernel::create` registry key。
- 原版宿主策略以目标 SHA 的 `windows/src/pe/nr_pe_session.cpp` 为准：
  `residency/park/start_offload/act_for/inframe`。此文件不在产品编译闭包中。
- 产品：`MochizukiNrRuntime.cpp` 的 `EnsureNetwork/Build/Serves/RetireNetwork`、
  `MochizukiRecording.inl` 的 `BeginRecordingExecution/EnqueueHip/Collect`；
  `Build` 的 ACO 成功检查依赖 `nr::binary::mode().imported`。
