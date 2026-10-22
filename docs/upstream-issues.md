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
- **同类未修键**(均为 `CustomOptional<bool> { false }` + 运行时 `set_volatile_value(true)` 自动开启,同样存在"显式 false 被 auto 回写后又被自动开启"陷阱,暂无现场报告,先记录):`FsrNonLinearColorSpace`(Config.cpp:334)、`DontUseNTShared`(dllmain:1352/1381)、`CreateD3D12DeviceForLuma`(dllmain:1361/1390)、`DxgiSpoofing`(dllmain:1506)。
- 建议:上游在 SaveIni 层为"被运行时自动开启过"的键持久化显式值,或这些键改 NoDefault。

## U5 XeFG passthrough 输入的 XeLL 配对(未修,待上游讨论)

- 上游:LL 构建下 `XeFGInputs::Hook` 把游戏旧 libxess_fg 的导出重定向到 Opti 副本,而游戏的 XeLL 上下文仍落在最先加载的旧 libxell;非 LL 构建是整对交换(正确),LL 构建不是。
- 状态:无现场报告(FGInput=XeFG+FGOutput=XeFG 组合);正确修法需 XeLL 输入在 passthrough 重定向时改用 Opti 配对,涉及引流设计,留待与上游讨论。

## 非上游问题的说明

菜单虚基下转崩溃(bdd11179)与 `_ffxFGIndex` 越界是**我们集成时引入**(上游无此代码/上游本有上界保护),不属于上游问题,已在 decisions.md 记录。
