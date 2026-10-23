# 上游 (OptiScaler) 继承问题台账

记录随上游集成带入、已在下游修复或仍待处理的缺陷,供向上游提 issue 时取证据。每条含:上游位置(9df512d 快照)、下游修复提交或状态、复现证据。

## U1 FSR4 驱动签名 DLL 无条件补丁 → 内核反作弊击杀(已修)

- 上游:`fsr4/FSR4DriverProvider.cpp:144-145` 加载 amdxcffx64.dll 后无条件 `FSR4Presets::Hook`(代码页 Detours)+ `FSR4Models::Hook`(provider 虚表改写)。
- 危害:崩坏:星穹铁道(HoYoKProtect 内核反作弊)超分首帧交接后 ~85ms 进程被外部终止(异常码 0x00000000、无栈、无 dump);1.13.1-alpha 同 DLL 不补丁存活 9.5 分钟。
- 下游修复:`e9767600`——仅 `Fsr4Preset`/`Fsr4Model` 显式设置才挂钩;`SelectableModels` 只读 RTTI 枚举 + `IsModuleHooked`;菜单首次选择延迟补挂。
- 建议:上游对 driver-signed 模块默认零补丁,或至少提供开关。

## U2 XeLL 配对缺校验 → 旧 XeSS 栈游戏 XeFG 启动崩(已修)

- 上游:LOW_LATENCY_INPUTS 设计把全进程 XeLL 调用引流到最先加载的 libxell("The first one becomes the real XeLL"),`XeFG_Dx12::CreateSwapchainContext` 直接把该上下文喂给 XeFG,无版本校验,违反其自身注释 "XeFG and XeLL contexts must come from the same pair of libraries"。
- 危害:巫师 3 + MFG unlocker(游戏目录旧 libxell 1.2.1)× Opti libxess_fg 1.3.1 → XeFG 首次 present 崩溃;鸣潮(libxell 1.3.2)正常。
- 下游修复:`3182b248`——真实 libxell 不得旧于捆绑副本(`XeLLProxy::BundledVersion()` 读文件版本资源)才挂;不配对则 XeFG 无延迟加速运行。注:Intel 按 SDK 整套配对 fg/xell、版本号独立演化(官方 3.0.2 即 fg 1.3.4 + xell 1.3.2),下游校验以捆绑副本为基线,勿照搬"XeLL minor ≥ XeFG minor"。

## U3 Win10 Agility 自动升级误伤非 Agility 游戏(已修)

- 上游:`dllmain.cpp:1160-1168`——`CheckAgilitySDK` 对无 D3D12SDKVersion 导出的游戏(全部 DX11 游戏、未集成 Agility 的 DX12 游戏)返回 0,`0 < 615` 为真 → Win10 上自动开启 `FsrAgilitySDKUpgrade`。
- 危害:`RunAgilityUpgrade` 对 kernelbase `IsDeveloperModeEnabled` 做 Detours + `SetSDKVersion(615)` 强切 SDK:内核反作弊游戏直接杀进程;DX11 游戏被强拖 d3d12/Agility 初始化。仅 Win10(build<22000),Win11 不受影响。
- 下游修复:本提交——`gameAgilityVersion > 0 &&` 下限门禁。
- 建议:上游同款一行修复。

## U4 CustomOptional 带默认值时显式 false 存不住 → auto 回写(部分修复,同类键风险待上游处理)

- 上游:`Config.h` `value_for_config()`(WithDefault 分支)在"当前值==默认值"时返回 nullopt,SaveIni 把 nullopt 写成 `auto`;`FsrAgilitySDKUpgrade { false }`(上游 Config.h:484)。
- 危害:与 U3 组合成死循环——用户显式 `false` 保存后被回写 `auto`,下次启动 Win10 又强制 volatile true(volatile 值同样坍缩 nullopt,连 true 都存不下来),崩溃循环无法自救。
- 下游修复:本提交——`FsrAgilitySDKUpgrade` 改 NoDefault。
- 同类键审计结论(2026-10-22 复核,初版判断有误):自动调整类键均已使用 `SoftDefault`
  ("Change always gets saved",Config.h:15),显式值可持久化、quirk 翻转带 `!has_value()` 门且
  `set_volatile_value` 保留用户旧显式值——**无坑**:`DontUseNTShared { true }`、`DxgiSpoofing { true }`
  (FSR4-Int8 分支无视显式值是上游故意的运行时覆盖)、`CreateD3D12DeviceForLuma { false }`。
  `FsrNonLinearColorSpace` 是除 Agility 外唯一 WithDefault,但其自动置 true 是"PQ/SRGB 显式设置→推导",
  运行时本就覆盖显式值(设计如此),无用户可感知陷阱。
- 建议:上游为会被运行时自动调整的键一律使用 SoftDefault/NoDefault;`FsrAgilitySDKUpgrade` 是唯一的
  WithDefault 漏网(下游已改 NoDefault)。

## U5 XeFG passthrough 输入的 XeLL 配对(未修,待上游讨论)

- 上游:LL 构建下 `XeFGInputs::Hook` 把游戏旧 libxess_fg 的导出重定向到 Opti 副本,而游戏的 XeLL 上下文仍落在最先加载的旧 libxell;非 LL 构建是整对交换(正确),LL 构建不是。
- 状态:无现场报告(FGInput=XeFG+FGOutput=XeFG 组合);正确修法需 XeLL 输入在 passthrough 重定向时改用 Opti 配对,涉及引流设计,留待与上游讨论。


## U6 DxgiSpoofing 自动改写的观感问题(下游已加提示日志,建议上游跟进)

用户反馈"spoofing 选项被自动改掉"。全部 7 处运行时改写点核验(2026-10-22):

| 触发 | 方向 | 门控 |
|---|---|---|
| OptiPatcher 打补丁成功(dllmain:301/361) | 关 | `!has_value()` |
| 检测到 Luma / Sekiro TSR(1349/1378) | 关 | `!has_value()` |
| 游戏不用 DLSS、伪装无服务对象(1783) | 关 | `!has_value()` |
| nvngx 替换缺失且无 nvngx.dll(1858) | 关 | `!has_value()`(外层整块) + 已有日志 |
| **FSR4-Int8 GPU**(1510) | **强制开** | **无门,覆盖显式值** |

结论:除 Int8 外全部只在用户未显式设置(auto)时调整——设计行为;DxgiSpoofing 为
SoftDefault,显式值持久化并在此后启动被尊重。唯一无门的是 FSR4-Int8(Int8 跑 FFX
必须伪装,上游故意),但**零提示**地覆盖显式 false,用户观感即"设置被吞"。下游已在
该处加日志("forces DxgiSpoofing on ... your explicit ini value is kept")。建议上游:
无门强制处补同款提示,或评估 Int8 也改为带门+醒目警告。判别口径(供支持):ini 值为
auto=从未显式设置(门控自动调整,显式设一次即固定);ini 显式值仍在而行为相反=Int8
强制(看日志);两者皆非=新问题,收日志。

## 上游同步审计(2026-10-23,9df512d → 990ff98,19 commits)

上游快照落后 19 个提交,已全量比对。结论:**我们修复的 5 个问题上游全部未修**(still-broken),
上游有 3 个新修复我们缺(we-lack)。无一方方案需替换——上游的新代码不与我们的门控冲突,
我们的 5 个修复保持不动。

### A. 我们已修、上游仍未修(提 issue 的候选,附下游证据)

| # | 问题 | 上游位置(990ff98) | 下游修复 |
|---|---|---|---|
| U1 | 驱动 DLL 无条件钩子 → AC 击杀 | fsr4/FSR4DriverProvider.cpp:181-182 | e9767600(Fsr4Preset/Fsr4Model has_value 门) |
| U1b | SDK DLL 无条件钩子(同 AC 面 + ZZZ 卡顿源) | proxies/FfxApi_Proxy.h:285-286,433-434 | 85df65da(同款门控,双 DLL 四处) |
| U2 | XeLL 配对无版本校验 → 旧栈崩 | framegen/xefg/XeFG_Dx12.cpp:110-112 | 3182b248(BundledVersion 比对) |
| U3 | Win10 Agility 0<615 陷阱 | dllmain.cpp:1160-1167(无 >0 下限) | 2138e127(gameAgilityVersion > 0 门) |
| U4 | FsrAgilitySDKUpgrade WithDefault{false} 显式值存不住 | Config.h:485 | 2138e127(改 NoDefault) |

注:上游 d8aebe4 "Improve auto model selection" 给 hkIsSupported 加了 DetectedModel() 回退,
**不是** ZZZ 卡顿修复——它仍每次调用 o_IsSupported、无缓存、钩子仍无条件安装;只改了
Auto 时的模型选择策略(INT8 检测到就当已选)。与我们的门控不冲突,也不构成替换方案。

### B. 上游新修复、我们缺(移植候选)

| # | 上游提交 | 内容 | 移植判断 |
|---|---|---|---|
| P1 | d06f31e 菜单 >8 缓冲区崩溃 | 数组 8→16(DXGI 上限),blur 描述符 20→36,拒绝超大链,越界帧跳过 | **已移植**(a7537612):NUM_BACK_BUFFERS 8→16 + MaxBackBuffers 8→16;下游越界守卫原已存在 |
| P2 | fa44a4a Vulkan RDR2 Reflex/FSRFG 崩溃 | KHR/EXT 别名解析设备函数 + vkSignalSemaphore[KHR] 时间线信号 + vkAcquireNextImage2KHR FG 路由 | **已移植**(a7537612):5 文件全量;SignalSemaphore 按设备解析 core/KHR,GetDeviceProcAddr 带 KHR/EXT 回退 |
| P3 | 33e434e DX12 FSR4 交换链升级 | FrameGeneration 选项链加 FrameGenerationSwapchain next 项 | **已移植**(a7537612) |
| P4 | d8aebe4 FSR4 Auto 模型选择改进 | DetectedModel() 回退 + FP8 抑制 | **策略层已移植**(a7537612,见下);IdentifyGpu.cpp 检测精化跳过(与 65ee5d6 重叠,待同步窗口) |


### P4 移植说明(2026-10-23,与门控并存)

d8aebe4 三部分,移植两部分、跳过一部分:

1. **FSR4Models.cpp `DetectedModel()` + `hkIsSupported` 策略**——已移植。INT8/INT8_2/INT8Lite 检测值
   映射为模型;selected 未设置时回退检测值(门控下不可达,保留与上游行为一致);FP8 抑制分支(显式
   选择跑不动时不让 FFX 落到 FP8);device==nullptr 跳过 lastReportedSupport。
2. **FSR4DriverProviderDx12.cpp FP8 仿真门**——已移植。仿真 FP8 wave matrix 只对 FG 模块和显式 FP8
   超分开放(INT8 GPU 上 FFX 默认选 FP8 质量差/慢)。
3. **IdentifyGpu.cpp 140 行**——跳过。与 65ee5d6 重叠,无 GPU 实测不整体搬;现有检测喂同一枚举,
   策略层已完整生效。

卡顿不受影响:门控不变,Auto(未显式选模型)依然零挂钩。
### C. 未移植提交(19 中剩 7,2026-10-24 二批移植后)

勘误(2026-10-24):初版把 b686ef4/824a7b7/62887bf(Vulkan FSR4 支持、FG 隐藏、VK 管线 workaround)
误列为待同步——经 `git merge-base --is-ancestor` 核实,它们是 9df512d 的祖先,早已随快照集成。
策略修正:上游已修 = 上游用户报过真实 bug,同 bug 在我们树里;不再"等我们的用户再报一遍"。

**已二批移植(P5-P8,小冲突直进)**:

| # | 提交 | 内容 |
|---|---|---|
| P5 | d1a146a | AL2→AL2 转换 × FSRFG:UsingDifferentInput 容忍 + mark_present_start 的 AL2+FSRFG 帧标记 |
| P6 | 0778a10 | FSRFG Vulkan vsync:vkWaitForPresent[KHR/2KHR] 挂钩,FG 持有交换链时立即返回成功(FG 自己节奏控制) |
| P7 | 223cf6b | Enshrouded MV:新 velocity_convert 计算着色器(R16G16_SNORM/UNORM → R16G16_SFLOAT)+ FFXFeature_Vk 接线 + shaderStorageImageExtendedFormats 特性启用 |
| P8 | 14434bf | HZD 重制版启动崩溃:版本枚举 outputCount 空指针防护 + 非零容量时才写数组 |
| (P4 内含) | 42d6162 | device==nullptr 跳过 lastReportedSupport(已随 P4 移植) |

**剩 7 个待评估(冲突面中大)**:

| # | 提交 | 冲突点 |
|---|---|---|
| E1 | e0a4859 Amdxc64_Hooks 重构 | 我们该文件 420 行差异(Proton QI 守卫 334c9ebf),需人工对账 |
| E2 | c220ad9 UI 重排(会话计时器/信息盒) | menu_common.cpp 我们四区布局大改 |
| E3 | 03e6ef0/c471428/c2529ed/a8d0f0a FPS overlay 群组 | 新配置键须走我们 CfgKey 注册体系;overlay 布局分叉 |
| E4 | 65ee5d6 GPU 检测大修 | 与 P4 跳过的 IdentifyGpu 部分配套,需 GPU 实测 |
| E5 | a79c2f3 移除非 LL 路径 | 结构性,与我们 LL fork 配置强耦合 |
| E6 | 5dfedd6 fakenvapi 简化 | 结构性 |
| — | 990ff98 merge,无内容 |

### D. 行动项

1. **提 issue(高优先)**:U1/U1b(AC 击杀,有星铁现场)、U3+U4(Win10 Agility 死循环)——上游修复成本低
2. **提 issue(中优先)**:U2(XeLL 配对)、U6(DxgiSpoofing 无提示覆盖)
3. ~~移植 P1-P3~~ 已完成(a7537612)
4. **下次同步窗口**:评估 E1-E6(见 C 节);E3 overlay 群组需按 CfgKey 体系适配,非直拷

## 非上游问题的说明

菜单虚基下转崩溃(bdd11179)与 `_ffxFGIndex` 越界是**我们集成时引入**(上游无此代码/上游本有上界保护),不属于上游问题,已在 decisions.md 记录。
