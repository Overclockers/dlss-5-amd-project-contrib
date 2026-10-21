# Mochizuki 0.0.7 剩余能力统一评估

日期：2026-10-19。产品基线为 `7e6f276a`，核心接入提交为 `471d26c6`；
原版固定为 `cc27501dc116bdcfbb43612e497c5ca12497574a`。
本次一起评估全部剩余项，没有启用新的产品路径，也不推进 Inline 调研。

## 结论

| 剩余项 | 取舍 | 实际用途与限制 |
|---|---|---|
| 同模型兼容尺寸 GraphCore、权重复用 | 2026-10-21 接入受限后台 `share_for` | 仅同 Session/device 的 split、除几何外精确语义匹配；格式仍须相同，核心核对 working extent 和原 core 源高度上限。减少重建和重叠内存，不改固定尺寸网络算术。 |
| plan-only + with_slot_of、预留容量 | 保持关闭 | 不同 plan 的 retarget 在录制时先修改 loaded 标记，未提交/失败/重放需要事务式提交证明；容量加大不能补上该证明。当前不预留更大容量。 |
| 跨模型 activation arena 共享 | 保持关闭 | 不同模型/plan 的 arena 重定位及队列失败语义尚未完整证明；本次不建立模型缓存或扩大共享范围。 |
| deferred 构造 + 双区域 image heap | 不直接接入原版策略 | 第三个 runtime 会让第一份 runtime stale；与可长期保留、以后再回放的本地 recording 冲突。若后续有图像分配瓶颈，使用完成证明驱动的空闲池，而非固定两区域强制轮换。 |
| park / resident / RAM offload | 当前产品不增加这套缓存及菜单 | 我们退休旧网，原版保留多个闲置网络。没有闲置缓存就没有需要卸载的对象；新增缓存会先增加占用及状态复杂度。关闭 NR 的现有释放行为仍应另行遵循本地所有权。 |
| 全局 SharedPipeline registry | 保持产品关闭 | 路径、device、bindings、push size、robustness 的 key 不表达全部编译来源；命中还绕过实际 import/capture。现有 ACO 用本次 imported 计数判断成功，不能只改开关。非 deferred 的 GraphCore 复用不要求开启该全局缓存。 |
| motion point | 保留当前产品采样；若追原版语义，须成套修改输入链 | 原版 NGX 选择 point、ReShade 保持 bilinear；我们有共享直读、buffer 上传、LINEAR rescale、bucket padding、多 pass 等路线。后级 point 无法撤销前级插值。它是画质语义选择，不列入已证实性能收益。 |
| resize history / carry_history | 保持尺寸变化时重置 | 原版也是 opt-in。当前 history 连续性包含 network、尺寸、队列、帧号及回放重置；不能因共享内存就取消重置。跨尺寸保留历史需要运动画质对照，且核心仅支持单 pass。 |

2026-10-21 更新仅接入上表受限同模型 `share_for`；其他建议不是已实现承诺。保留现有 buckets/settle、预算保守估算、历史重置与 recording 完成/退休证明；没有新增用户开关。

## 2026-10-21 受限接入与证据

后台 builder 暂持当前或 handed-off runtime 的一个引用；仅同 Session/device、split、模型/实际 INT4 无回退且除 width/height/renoWidth/renoHeight 外完整 NetworkKey 相同才考虑 donor。实际 ACO 来源必须与本次构造臂一致，仍创建私有 pipelines 并要求本次真正 imported；ACO 失败仍丢弃整图后独立 native 重建。未证明完成的 old donor 被清除。核心 `share_for` 最终核对参数、相同 working extent 与原 core 的 source-height 上限，拒绝时真实完整构造，不进行 render-thread 降图/上传或新增等待。

可复用例：Small/Original 1920x1080→1918x1078；Small 1920x1080→1920x1056 的 working extent 不同则拒绝，独立较矮 donor→更高 source 也拒绝。先从高 source 分享到较矮后，返回原高 source 仍在同 core 上限内。大幅 DRS 变化通常不命中。现有 bucket/canonical/pass-capacity 已避免的重建不重复计算收益。

RX 9070 XT / 32.0.32015.7029 离线、匹配观察器、Small 1920x1080→1918x1078：native/ACO 各两轮 AB/BA/A/A 共24进程，全部17帧输出字节和 reset epoch 相同；每臂前恢复并记录同一 pipeline.cache 的大小/hash。native 构造中位1.995→0.31秒、ACO 2.05→0.375秒；配对节省分别1.60–1.75/1.67–1.70秒，A/A漂移至0.07秒。采样进程显存峰值分别减少520.375/521.375MiB。只证明兼容重建的构造/进程峰值效应，不证明完整安装延迟、游戏FPS或任意DRS收益；驱动内部缓存未重置。

Small/Original×native/ACO、兼容/拒绝尺寸、RenoDX、保留旧录制跨队列重放及未提交取消有针对性字节/epoch对照。候选构造丢弃、已完成ACO导入候选失败后整图native回退、模拟分配拒绝、Abandon及未证明旧完成分支有窄范围故障证据；不冒充真实GPU挂起或部分分配耗尽。12个同进程runtime交替重建、旧录制保留、其余收集后，25帧对照一致，candidate在第1–11次收集后的进程显存固定399212544字节（baseline544571392）。不据此认证无限期无泄漏。

Patch28只让 `share_pipelines=false` 的私有 Kernel 不再被共享 arena 额外永久 pin；每个 Kernel 仍强持 SharedPipeline，保留 recording 的原 runtime/pipelines/images 不变。registry/deferred pin 路径不变；不使用双区 image heap、carry_history 或不同模型 activation sharing。INT4/multipass新共享路径、Vulkan validation、其他设备/驱动、真实GPU丢失和长期游戏未认证；模型数学、精度、采样及250ms历史保护不变。

干净产品闭包另完成 `tools/mochizuki-sync/check.py` 固定pin重放（578原文件、9新增文件、26补丁）、原构建入口 `--skip-shaders` 完整core/adapter编译，以及实际产品DLL Small native/ACO各17帧几何替换/旧录制重放对照：全部输出字节相同，原core日志确认真正graph shared，ACO本次31/30条导入，D3D12 debug错误零。既有runtime夹具 `--split-retain` 返回 `SPLIT_RETAIN_OK`。干净DLL不含调查epoch/pointer日志或故障注入；上述构造收益仍来自独立受控调查，不以干净冒烟替代性能测量。



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
