# Linux/Proton vkd3d 直连 — MochizukiNrRuntime buffer 导入修复

> **状态(2026-10-22,feat/nr-bridge-opt):源码完成、待实机。** 实现与下文"修复方向"的差别、以及验收缺口,
> 见本文末尾"实现(源码完成、待实机)"一节与 docs/decisions.md 同日条目。未在 Proton 实机跑过,不声称通过。

## 问题

Dying Light: The Beast(GE-Proton,vkd3d-proton)开 NR 后:
- `candidate frame buffers failed at 1920x1080: import of a D3D12 buffer failed, VkResult -1`
- 网络构建成功(24 pipelines),但 frame buffer 共享失败
- 每秒 ~128 次重试,熔断器(已合入 main)在 50 次后止血
- **mochizuki 独立版 0.0.7 在 Linux 正常** — 这是我们的集成层问题,不是 Proton 限制

## 根因

`MochizukiNrRuntime.cpp` 的 `SharedBuffer::Create`(~:1130-1200):

```
D3D12_HEAP_FLAG_SHARED 的 buffer → CreateSharedHandle → NT handle
→ vkCreateBuffer(VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT)
→ vkGetMemoryWin32HandlePropertiesKHR
→ VkImportMemoryWin32HandleInfoKHR + vkAllocateMemory  ← VkResult -1 (此处失败)
```

winevulkan 不支持 `VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT` 的 Win32 句柄导入 — 底层 Linux 驱动(RADV)没有 Windows 式 D3D12↔Vulkan 驱动级互操作。

## 修复方向

在 Proton 下,D3D12 本来就是 Vulkan(vkd3d-proton):
- 游戏 `ID3D12Device` → vkd3d 内部有 `VkDevice`/`VkPhysicalDevice`/`VkQueue`
- 不需要自建 Vulkan 设备 + 跨 API 共享 — 直接用 vkd3d 的设备
- buffer 也不需要 D3D12→Vulkan 导入 — 同一设备上操作

### 具体步骤

1. **检测 Proton**: 环境变量 `WINEPREFIX` 或 `wine_get_version` (ntdll)
2. **获取 vkd3d 的 Vulkan 对象**:
   - vkd3d-proton 导出 `ID3D12DeviceExt`(非标准接口)或可通过 `vkGetInstanceProcAddr` 拿到底层实例
   - 参考 `nrvk.hpp:101` 已有的 `queue_lock/queue_unlock`(为 vkd3d 预留)和 `HostDevice` 结构
3. **Proton 分支的 buffer 路径**:
   - `SharedBuffer::Create` 在 Proton 下跳过 `CreateSharedHandle` + Vulkan 导入
   - 改用 vkd3d 的 `VK_KHR_external_memory_fd`(opaque FD)路径,或直接在 D3D12 buffer 对应的 native VkBuffer 上操作
4. **Windows 路径零改动**(用 `#ifdef` 或运行时检测门控)

## 关键文件

- `OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrRuntime.cpp` — 主要改动(SharedBuffer, Vulkan 设备创建, buffer 路径)
- `third_party/mochizuki/windows/src/core/nrvk.hpp` — 参考(Vulkan 上下文,已有 vkd3d queue 借用预留)
- `third_party/mochizuki/windows/src/core/nr_runtime.cpp` — 参考(上游 runtime,独立版 Linux 路径)

## 验证

- Windows: 构建通过 + 现有游戏(燕云/巫师 3)NR 功能不变
- Linux: Dying Light: The Beast 用户实测,日志不再出现 `import of a D3D12 buffer failed`,NR 正常工作

## 实现(源码完成、待实机)

按 Linux 路线的 `linux/src/pe/nr_pe_interop.cpp`(mochizuki0323/DLSSNR-AMD)做,替代 bd6d388a 里 gate off 的
`AdoptVkd3dDevice`:

- **取设备**:`ID3D12Device` QI `ID3D12DXVKInteropDevice`(uuid 39da4e09-…,声明在
  `mochizuki_runtime/MochizukiVkd3dInterop.h`)→ `GetVulkanHandles`。只在 Wine 下(ntdll `wine_get_version`)尝试;
  `MZ_PROTON_DIRECT=0` 关闭。instance/physical/device 是借用的,`Destroy` 不销毁,只 Release 接口与自建队列。
- **入口点(对应 c71b0f4)**:vkd3d-proton 的句柄属于 winevulkan.dll;本模块经 vulkan-1.dll(延迟加载)调用。
  Proton 自带的 vulkan-1.dll 只是转发,`GetProcAddress` 两边相同;不同(nativevulkanloader,Khronos loader)时
  **不走直连**,回落自建设备路径(熔断器兜底)。Linux 路线是整体改绑 winevulkan.dll,这里只做拒绝。
- **buffer**:Proton 下的帧 buffer 建普通 committed buffer,`GetVulkanResourceInfo` 取 vkd3d 的 VkBuffer 与
  offset;所有 copy 区域加 offset;不 vkDestroyBuffer(borrowed)。passthrough 录制缓存同时按 buffer+offset 判定。
- **fence**:toVk/fromVk 仍是 D3D12 共享 fence 导入为 timeline semaphore(Proton 日志已证明这一步通过,失败的
  只是内存导入)。
- **队列**:每帧提交(Submit / SubmitPhases / passthrough)先 `GetVulkanQueueInfo(producer)` 校验队列族与帧的
  命令缓冲同族,再 `LockCommandQueue(producer)`——它先排空该 D3D12 队列已提交的工作(含 toVk 的 Signal),
  再持锁提交到同一个 VkQueue,`UnlockCommandQueue` 后 vkd3d 才提交 Wait(fromVk) 与后半段。网络构建走自建的
  DIRECT D3D12 队列对应的 vkd3d 队列,`HostDevice::queue_lock/unlock` 包住每次 vkQueueSubmit。无独立 compute 队列。
- **共享纹理**:直连下关闭(`sharedInputRequest/sharedMotionRequest=0`,`PlanShared` 也拒绝)——它是
  D3D12_RESOURCE 导入,winevulkan 做不到;直连下 buffer 已是游戏同设备内存。
- **收尾**:直连下不 `vkDeviceWaitIdle`(那会等游戏),改为限时等 fromVk 到最后的 finished 值。
- **Windows**:所有分支都以 `vk.adoptedVkd3d` 为门,它只在 Wine 下可能为真;Windows 上 offset 恒为 0、
  borrowed 恒为 false、提交目标仍是 `vk.queue`。
- **未采用**:`ID3D12GraphicsCommandListExt` 的列表内录制——我们的 split 结构是 D3D12 前半 / Vulkan 网络 /
  D3D12 后半三段提交,不在游戏的 command list 里录 Vulkan 命令。

### 验收缺口(待实机)

1. **未在 Proton 实机运行**:需 Linux 用户(DL:The Beast 等)抓 OptiScaler.log,看
   `Proton direct path: vkd3d-proton's device …` 一行与是否仍出现 `import of a D3D12 buffer failed`。
2. **vkd3d-proton 不启用 VK_KHR_workgroup_memory_explicit_layout**(其 vkCreateDevice 扩展表里没有):
   与 Linux 路线一样只查物理设备支持,依赖 RADV 不校验;日志会列出 vkd3d 未启用的扩展,便于区分失败原因。
3. **nativevulkanloader 游戏**:直连被拒绝,回到原状态(自建设备 + 熔断器),不是支持。
4. **producer 队列族与帧命令缓冲族不同**(例如游戏从 COMPUTE 队列提交 NR):该帧提交失败,走既有丢帧/熔断。
5. **LockCommandQueue 会排空游戏队列**:每帧一次额外同步点,性能代价未测。

## 游戏内同场景 split A/B 流程(共享纹理 vs buffer)

共享纹理路线的开关是进程启动时读的环境变量,不能在同一会话内原地切换,所以这是**跨会话**比较,
违反 [docs/measurement.md](docs/measurement.md) "只比同一会话"一条;下面靠交替多次启动和固定站位压低偏差,
结论只能写"在此流程下的差异",不写绝对收益。

1. 准备:同一 Release 构建(记 `OptiScaler.dll` SHA256)、split 模式、同一画质/渲染分辨率/NR 设置;
   `OptiScaler.ini` 设 `NrTimingEnabled=true`、`NrTimingLog=true`,其余诊断保持关闭。关帧生成,
   否则 PresentMon 加 `--track_frame_type`。
2. 变体(每次都从关闭游戏开始):
   - A = 默认(不设变量)。
   - B = `MZ_TEST_SHARED_INPUT=0`(颜色与 motion 都回 buffer)。
   - 可选 C = `MZ_TEST_SHARED_EXTENDED=0`(只测 2026-10-22 新增准入的影响);`MZ_TEST_SHARED_MOTION=0` 单独看 motion。
   - 设变量:从 PowerShell `$env:MZ_TEST_SHARED_INPUT='0'; & '<游戏exe>'`;必须经 Steam 启动时,启动选项写
     `cmd /c "set MZ_TEST_SHARED_INPUT=0&& %command%"`。测 A 前清掉该变量/启动选项。
3. 场景:同一存档点读档,原地不动、镜头不动(或同一段过场),读档后等 30 s 稳定再采。
4. 采集:每次 PresentMon 60 s;按 A B A B(至少各 2 次)交替,不要连续跑同一变体。
5. 每次先核对日志确实走了预期路线:`[mochizuki] split colour input: shared texture` / `buffer`
   与 `split motion input: ...`;A 若出现 buffer(准入未命中、预算或退避)该次作废并记下原因。
6. 读数:`MsBetweenPresents` 与 `MsBetweenDisplayChange` 的中位与 P95、NR timing 日志的阶段时间;
   每次单独列,再看 A/B 是否在各自的启动间波动之外。窗口内有 TDR 的按 measurement.md 对事件日志处理。
7. 画面:同站位各截一张,确认没有错位、黑边或 bucket 边缘异常(DRS 游戏重点看右/下边缘)。
