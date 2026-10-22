# Linux/Proton vkd3d 直连 — MochizukiNrRuntime buffer 导入修复

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
