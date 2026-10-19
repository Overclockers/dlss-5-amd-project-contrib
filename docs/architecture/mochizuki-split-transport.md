# Mochizuki split 共享颜色输入

## 状态（2026-10-19）

共享颜色纹理已接入产品适配器。符合下述条件的 split 录制默认申请共享 texture，
初始化完成后采用；不支持、初始化未完成或预算不足时继续执行 buffer NR。
不新增菜单/INI 开关，不调整模型、精度、时域历史或合成语义。仅在载体 generation
变化及申请失败/退避时记录路线信息，不增加逐帧正常日志。

本轮只考虑 split。Inline 调研与修改搁置，lmxxf 不随本方案重构。共用层实际改动时
才复用相应非回归检查。原版指 Mochizuki 原版，不指本项目分支。

## 已定位的开销

相同 D3D12 设备/队列、2558×1449 RGBA16F、连续帧、单遍、完整模型比例与时域历史，
新旧模型及编译方式对齐后，产品桥接比原版约多 0.28–0.31 ms。原版 PE 桥接使用共享
D3D12 texture/Vulkan image；产品先 texture→共享 buffer→私有 image，输出再反向搬运。
核心同设备对照持平，改变原版核心录制时机没有复现主要差距。

阶段计时中，输入捕获约多 0.105 ms；Vulkan 核心前约 0.170 ms，包含颜色与 motion
两项上传，不能称作颜色单项。输出下载与写回差异量级约 0.04 ms。区间中位数不直接
相加当作整帧成本，未解释的剩余跨度不指定为某种驱动调度机制。

这不是游戏总 FPS 结论。举例原版 70 FPS 且新增耗时全在 GPU 关键路径：增加 0.29 ms
约下降 1.99%；下降 5% 则需约 0.752 ms。用户实际基准帧率、接法、NR 分辨率、
模型/编译/compose 配置与同场景帧时间仍需核对。

## 为什么简单跳过上传不能直接当收益上限

临时实验在一次合法 seed 后跳过颜色或 motion 上传，保留合法布局与完整 dispatch。
跳过颜色后继续消费上一帧输出，改变了后续核心输入；新模型中上传区间约缩短 0.138 ms，
核心区间却约增加 0.240 ms，整段约慢 0.092 ms。该结果只说明这个反事实受内容变化影响，
不能用于否定共享纹理，也不能称作可用 NR 优化。

固定 seed 的 image→image 恢复对照可保持静态输出一致，但新增复制本身又有成本。
因此最终以真正导入的共享纹理配对探针判断收益，没有把上述反事实直接外推到产品。

## 共享颜色纹理探针

RX 9070 XT，驱动 `32.0.31041.1004`，MSVC 14.44.35207/SDK 10.0.26100.0，Release。
未锁 GPU 频率；HAGS 未核实。vendored 核心 pin 为
`191b97b54175d50c7ec5992b54322dd3eb0ef8ac`，原版参考为
`ab00d2b23584de0f42ac91eca4d7d2d2fad11465`。不加载完整 OptiScaler 宿主/游戏。

每个比较在同一进程、同一 runtime 与相同 D3D12/Vulkan 设备中切换：

- buffer 基线：沿用产品原有颜色和 motion 上传。
- 颜色候选：D3D12 捕获到自有共享纹理，单 Vulkan 队列族、EXCLUSIVE，使用 EXTERNAL
  acquire/release；核心直接在导入 image 上读写。motion、输出下载与 D3D12 写回保持原路。
- 初始 UNDEFINED→GENERAL 在第一次 D3D12 捕获前完成。初始化用独立 fence 非阻塞查询，
  没有 `vkQueueWaitIdle`；每帧不再用 UNDEFINED 丢弃捕获内容。
- 四轮正反序；各段 ResetHistory 后以相同帧数推进，再预热和测量。每帧重新录制，
  计数核对完整网络提交及 dispatch。hash 在段末核对；变化输入检查另外逐帧核对。

| ACO 组合 | 每段计时帧 | 整段节省中位数 ms | 四轮节省范围 ms | 计时内成功 NR |
|---|---:|---:|---:|---:|
| 新模型、RGBA16F motion、标记开 | 240 | 0.0786 | 0.0242–0.1078 | 1920/1920 |
| 原模型、RGBA16F motion、标记开 | 180 | 0.0936 | 0.0071–0.1257 | 1440/1440 |
| 新模型、RG16F motion、标记开 | 180 | 0.0996 | 0.0844–0.1060 | 1440/1440 |
| 新模型、RGBA16F motion、标记关 | 180 | 0.1578 | 0.0565–0.2584 | 1440/1440 |

每行在自身会话内配对；不能用不同行绝对值相减判定标记成本。计时内共 6240 次成功
执行，新模型每帧 71 次网络 dispatch，原模型 123 次；没有靠减少网络执行获得收益。
段末输出 hash 均与对应基线一致。RGBA16F 与 RG16F motion 分别建立配对，不混用基线。
RGBA16F motion 的颜色上传区间约减少 0.142 ms，捕获约减少 0.015–0.016 ms；总收益
仍需报告实际整段测量，不能把阶段节省相加代替它。

额外正确性检查：

- 新模型原生编译：两轮正反序、两个交替变化输入，16 个计时帧逐帧 hash 配对一致。
- 新模型 ACO、RG16F motion：同样检查 128 个计时帧；输出确实随输入变化。
- 两条路线关闭录制后反向切换请求，录制内容保持原路线；后续 Prepare 才采用新请求。
- 五次交替选择 texture/buffer 并注入核心录制前失败，image→buffer 或 buffer→buffer
  旁路输出均与当前捕获输入 hash 一致；故障期间零成功网络提交，五次旁路单独计数。
- 上述调试检查 D3D12 debug layer 错误数为零；没有启用 Vulkan validation，不能据此
  宣称全部 Vulkan VUID 或驱动组合已验收。

上述为接入前探针，收益低于最初约 0.15–0.20 ms 的预期，但实际路径存在可重复正收益。
该目标是工程估计，不是必须凑足的合入门槛。不能宣称回收全部 0.3 ms、提升游戏 FPS
或解决用户 5% 反馈。

## 产品准入与回滚

第一版只覆盖 RGBA16F、全尺寸单 mip/单层/单采样纹理、单遍、完整模型比例、
实际 motion 输入和普通 compose。canonical 转换、bucket/DRS、first-pass 导出、RenoDX、
无 motion 及 Inline 均使用现有 buffer 路线。只改变颜色载体；motion 与输出不变，
直接复用核心对 `colourImage` 的原地处理，不修改 vendored 核心或增加 `EngineFrame::target`。

`MZ_TEST_SHARED_INPUT=0` 在进程启动前设置，可靠选择原 buffer 路线；缺省或 `1` 仅请求
通过全部安全/能力/预算准入的共享路线。这是内部诊断入口，不属于用户配置优先级，
不提供产品热切换 ABI。临时同进程 A/B 夹具通过私有派生 setter 请求后续录制，已关闭
录制始终保持自己的路线、资源及 generation。

## 所有权、排序和故障处理

- `ColourInput` 单独拥有 D3D12 texture、Vulkan import/image、初始化 pool/fence，
  `FrameBuffers` 与 recording 直接持有它。其他资源的 `FrameBuffers::allocation` 不再
  强持旧颜色 buffer；Steal/Take/Use/Release、几何退休和设备丢失保留均携带颜色所有者。
  旧录制存在时允许暂时共存；仅以 weak cache 缓存两种载体，防止永久双持。
- 初始化在同一 Vulkan 队列族上使用 EXCLUSIVE + EXTERNAL 交接。独立 fence 非阻塞
  查询 Ready；申请提交前先登记强持有，即使用户未提交游戏录制就丢弃也不提前释放。
  可选初始化以 try-lock 避免等候网络构建；没有新增 render-thread queue/device idle。
  每帧从 GENERAL acquire，在下载后 release 回 GENERAL，不用 UNDEFINED 丢弃输入。
- `BeginRecordingExecution` 保留 previous consumer tail 的捕获前 GPU wait。
  录制 Collect 同时核对 D3D12 尾部和该录制**实际被接受的 Vulkan 完成值**；路线退休
  也持有旧载体直至 Vulkan 完成。不是用 later frame 或 `Follow` 推断当前帧完成。
  不确定/设备丢失沿用 quarantine；没有为了回收而强行释放导入资源。
- `Submitted` 在可分配/抛异常的诊断记账前记录完成凭证。split 已被接受的提交若记账
  失败，则禁止重试和继续执行；核心开始推进 CPU 历史后的录制错误也禁止改走旁路
  重录同一帧。只有安全的核心录制前失败才允许 image→buffer 或原 buffer→buffer 旁路。
- 旁路缓存检查颜色 generation、输出 buffer 和 extent；换路线使缓存失效，复用命令
  前仍检查其完成 fence。texture 旁路包括 EXTERNAL acquire/release；入口不再误要求
  快路不存在的 `input.buffer`。
- 不支持能力缓存为不可用，可恢复导入/共享句柄错误使用 `kInvalidRetryMs` 退避，
  预算拒绝保持 10 秒，避免逐帧申请或路线抖动。可选候选失败保留可执行的 buffer NR，
  不调用整体预算拒绝的 `ReleaseNetwork`。device loss 继续按丢失处理。
- FrameBytes 按实际颜色 allocation 替换颜色项，候选另查实际进程用量、增量分配和
  预算余量。现有 buffer 的 16 MiB 驱动规避保持原样，不套给 texture。2558×1449 本机
  buffer 颜色载体为 59,834,368 字节、shared 为 30,146,560；旧录制收集后可释放差额
  29,687,808 字节（约 28.31 MiB），而非永久叠加一张共享纹理。

实现入口为适配器的 `MochizukiColourInput.inl`、`MochizukiColourRouting.inl`、
`MochizukiNrRuntime.cpp` 及 `MochizukiRecording.inl`。宿主 ABI、lmxxf、Inline 算法不变。

## 产品验证范围

复用现有 runtime ABI、split retained recording、GPU 格式/几何/生命周期检查和宿主
early-unity/inline-discard 所有权检查；新增故障控制和 A/B 只在临时夹具中使用，
没有新增测试项目。检查包括变化输入逐帧 hash、保留两种路线录制、跨两条 D3D12 队列
重放、几何改变、未提交丢弃、消费者尾部未完成、旧 buffer 释放和共享句柄拒绝。

两个独立 GPU gate 还检查了初始化未完成即丢弃后 Destroy 等待独立初始化凭证，以及
Vulkan 已接受但记账抛异常时 D3D12 尾部已完成仍拒绝 Collect，Vulkan 完成后才允许回收。
预算注入确认连续 buffer NR、原网络保留及预算恢复后重新准入。D3D12 debug layer
未报错误；没有启用 Vulkan validation，也没有覆盖真实硬件设备丢失与全部驱动组合。

接入后的离线 A/B（同机、同设备、四轮正反序）：

| 工作负载 | 配对节省中位数 ms | 计时内成功 NR |
|---|---:|---:|
| 新模型 ACO、RG16F motion、阶段标记开 | 0.0939 | 1920/1920 |
| 原模型 ACO、RGBA16F motion、阶段标记开 | 0.1126 | 1440/1440 |
| 新模型 ACO、RG16F motion、阶段标记关 | 0.0978 | 1920/1920 |
| 最终版本完整宿主、新模型、正常日志、NR+FSR2.1.2 | 0.0836 | 1920/1920 |

前三行在最终补充 Vulkan 回收凭证前测量，输出 hash 对应相同；补充后重跑变化输入、
故障、生命周期、现有 GPU/ABI 和完整宿主检查。最后一行加载实际 Release 宿主与带私有
计数入口的同源码 runtime，NR 2558×1449、SR 输出 3840×2160，每段预热 40、计时 240 帧。
四轮配对差为 0.0679、0.0903、0.0770、0.1130 ms；无减少网络执行。此前一次宿主探索
会话为 0.1732 ms；未锁频且会话有波动，不能只选较大收益，也不能把两次差值归因于
回收凭证。当前可确认的是本机受限 split 有正收益，精确游戏收益仍需实测。

motion 后续独立评估，输出最后评估或不做。仍需真实游戏、Present/FG/FSR4、长时间运行
及其他 GPU/驱动验证；不能将离线 NR+SR 耗时直接称为游戏 FPS 改善。
