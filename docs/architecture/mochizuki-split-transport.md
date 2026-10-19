# Mochizuki split 数据传输：离线验证与接入边界

## 状态（2026-10-19）

基线为 `701fc468`。共享颜色纹理已在临时派生 runtime 中完成受限 split 的性能、
变化输入、录制路线冻结和失败旁路验证，**尚未接入产品**。产品仍使用共享 buffer；
不新增菜单/INI 开关，不调整正常日志、模型、精度或合成语义。

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

收益低于最初约 0.15–0.20 ms 的预期，但实际路径存在可重复正收益，值得继续最小接入。
该目标是工程估计，不是必须凑足的合入门槛。不能宣称回收全部 0.3 ms、提升游戏 FPS
或解决用户 5% 反馈。

## 产品接入前必须补齐

探针仅覆盖单会话、固定几何、逐帧完成后复用资源。它仍保留旧载体；导入颜色 allocation
为 30,146,560 字节，不能把该分配永久叠加到产品而不计预算。

1. **所有者和初始化状态。** 共享 texture/import/view、初始化 pool/command/fence 进入
   `FrameBuffers::allocation` 所有者，贯穿 Steal/Take/Use/Release、retired/quarantine。
   `Unallocated → Initializing → Ready`；仅 Ready 可绑定新录制。没有游戏提交也要等
   初始化完成才能回收。device loss/不确定完成进入保留状态，不能以单侧完成释放。
2. **录制固定路线。** route 与源/目标资源 generation、格式、extent、usage、family/sharing
   契约固定在 recording。开关只影响下一份录制；不重建网络或额外重置历史。保留旧录制
   和在途资源，不能用 Session 可替换裸指针代替所有者。
3. **回退缓存。** `RecordFallback` 对 texture 录制 image→buffer acquire/copy/release；
   route、资源身份或 footprint 变化使缓存失效，等待旧命令完成后再录。当前
   `BoundRecording::TakeBuffers` 已清除 `fallbackCurrent`，但不将这种附带行为作为
   新路线唯一的缓存契约。`Enqueue` 的 `input.buffer` 前置检查也要按路线修改。
4. **错误状态。** capability 不支持与可恢复 handle/预算失败分别缓存；沿用
   `kInvalidRetryMs` 的退避思路和预算滞回，不每帧重试。16 MiB 是现有 buffer 的驱动
   规避，不能机械套给 texture。device loss/不确定提交不能降级成普通能力失败。
5. **独立可选预算。** 按实际 texture allocation、保留缓冲和在途份数重算 FrameBytes。
   当前 `MakeGeometry` 的预算拒绝可能 `ReleaseNetwork`；可选快路失败不能复用这条
   整体拒绝策略，必须保住原可执行的 buffer 路线。新旧资源暂时共存须有峰值预算，
   旧录制释放后不再永久双持颜色载体。
6. **捕获前排序。** 当前 recording ABI 中 `LogicalList::Execute` 在 producer 提交前调用
   observer，`BeginRecordingExecution` 对 previous consumer tail 排 GPU wait。保留这条
   顺序，不因 `Follow` 在 producer 提交后再等待就宣称所有入口都缺捕获前依赖；
   也不能把 Follow 单独当成证明。跨队列/重放仍须专项验证。
7. **失败时点。** 本次故障注入仅覆盖核心录制前。核心 CPU 状态已推进、部分实际提交、
   未确认消费者尾部、初始化未完成即丢弃、几何变化、Reset/Release 与设备丢失均待验收。
   不允许捕获异常后重录网络并重复推进同一帧历史。

仅在上述条件、受影响现有检查和完整宿主验证通过后，才决定改 split 默认路径。
内部 A/B/回滚控制不进菜单；强制请求快路也必须服从能力和安全准入。motion 按独立
收益随后评估，输出最后评估或不做；Inline 不在扩展列表。
