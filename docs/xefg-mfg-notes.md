# XeFG 多帧生成（MFG）宿主兼容与性能说明

本文记录外部 XeFG 多帧生成插件（如 `MFGUnlocker.asi`）在 OptiScaler(NR) 宿主下的兼容机制、`HighResMV` 自动检测行为，以及 AMD 显卡在 4K 输出下的性能开销构成。

## 1. 宿主兼容契约（三处关键点）

外部 `MFGUnlocker.asi`（通过 `OptiScaler\plugins\` 加载）识别到 OptiScaler 宿主后，会在首次创建 XeFG 上下文（`xefgSwapChainD3D12CreateContext`）时解锁已加载的 `libxess_fg.dll` / `libxell.dll` 帧数上限（使 `GetProperties().maxSupportedInterpolations` 返回 `5`，即最高 6X），随后将实时倍率控制权交回宿主（`xefgSwapChainSetNumInterpolatedFrames`）。

宿主侧需保持以下三项兼容守卫（见 [Xell_Hooks.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/hooks/Xell_Hooks.cpp)、[XeFG_Dx12.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/framegen/xefg/XeFG_Dx12.cpp)、[Kernel_Hooks.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/hooks/Kernel_Hooks.cpp)）：

1. **不对 `libxell.dll` 导出表做纯日志 Detour（`#ifndef LOW_LATENCY_INPUTS`）**
   - 当未启用 `LOW_LATENCY_INPUTS` 时，`XellHooks::Hook()` 直接返回，不再将 `libxell.dll` 的 `xellD3D12CreateContext` / `xellDestroyContext` 等 8 个导出 Detour 到 `OptiScaler.dll`。
   - 否则插件扫描 `libxell.dll` 导出时会因入口跳入宿主模块而置位 `hostLatency = true`，在 `ApplyHostUnlock` 中触发 `host_xell_redirected` 熔断并跳过帧数解锁。
2. **`CreateSwapchainContext` 前触发公开 `GetProcAddress` 同步解析**
   - 宿主启动时 `CheckMemoryForProxies()`（内含 `XeFGProxy::InitXeFG()`）早于 `LoadAsiPlugins()` 执行，且使用 `KernelBaseProxy::GetProcAddress_()` 缓存函数指针，绕过了插件挂在 `kernel32!GetProcAddress` 上的 `FirstCreate` 同步初始化钩子。
   - 在 `XeFG_Dx12::CreateSwapchainContext` 调用 `D3D12CreateContext` 前显式调用一次 `GetProcAddress(XeFGProxy::Module(), "xefgSwapChainD3D12CreateContext")`，确保插件在当前线程同步完成模块发现与钩子安装，避免与后台 `PatchWorker` 线程竞态。
3. **`hk_K32_GetModuleHandleExA/W` 过滤 `GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS`**
   - 当 `dwFlags` 含 `GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS` 时，`lpModuleName` 是内存地址而非模块名字符串，直接透传给原函数早退。

## 2. `HighResMV` 的自动检测与落盘机制

`OptiScaler.ini` 中的 `[XeFG] HighResMV` 默认是 `auto`（`false`），但宿主会在运行期根据当前选用的 `FGInput` 自动检测并调用 `Config::SaveXeFG()` 写回 INI：

- **`FGInput = dlssg`（Streamline 输入）**：在 [Streamline_Inputs_Dx12.cpp](../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/inputs/FG/Streamline_Inputs_Dx12.cpp) 中，若游戏向 Streamline 提交的 `data.motionVectorsDilated == sl::Boolean::eTrue`（即引擎已将运动矢量膨胀到显示分辨率），宿主会自动将 `FGXeFGHighResMV` 设为 `true` 并保存到 `OptiScaler.ini`。即使同一游戏给 Upscaler（`IFeature`）传的是渲染分辨率 `LowResMV: true`，给 DLSS-G 传的也可以是膨胀后的 `HighResMV`。
- **`FGInput = fsrfg` / `upscaler`**：分别根据 `FG_Flags::DisplayResolutionMVs` 或 `!_initFlags.LowResMV` 自动同步并调用 `SaveXeFG()`。
- **为什么落盘到 INI**：`XeFG_Dx12::CreateSwapchain` 在启动早期创建交换链时就必须确定 `XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV`，早于游戏进入场景首次提交帧常量的时机；落盘后下次启动即可在创建交换链时直接使用匹配的标志。

## 3. 4K 多帧生成的性能边界

用户报告 RX 9070 XT 在 1080p 渲染、4K 输出时，不开 FG 的基础帧为 60+ fps，XeFG 2X 约 48 fps，4X 约 30 fps；同场景 pre-SR Mochizuki NR 的菜单计时约 6 ms。这是用户观察，尚缺对应构建 SHA、完整配置和同会话 GPU 逐段计时，不作为可复现的性能基线。

NR 在渲染分辨率处理，插帧需要处理输出分辨率；4K 的像素数为 1080p 的四倍，多帧生成还会增加 GPU 工作。以上可解释开销可能增加，但不能直接推出耗时按像素数或插入帧数线性累加。

AMD 上所选 XeFG/外部插件实际使用的内核、是否调用矩阵指令、光流与生成阶段各占多少时间，均须用匹配版本的模块和 GPU 捕获确认。不能仅凭非 NVIDIA 路径标志，断言整个管线只能使用 DP4a、无法使用 WMMA，或把基础帧下降全部归因于某一条指令路径。

后续对比应固定宿主/插件/DLL 版本、场景、输出分辨率、HighResMV、同步/限帧与 NR 设置，记录基础帧和显示帧、GPU 阶段耗时与画质。未完成这些测量前，不宣称该开销必然正常，也不承诺另一 FG 后端一定更快。
