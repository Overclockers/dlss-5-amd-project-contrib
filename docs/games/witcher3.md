# 巫师 3(The Witcher 3,D3D12)

用户向兼容说明 + 静态核对记录。代码里不按游戏名判断。

| 项 | 内容 |
| 当前状态 | 已实机验证（2026-10-23，巫师 3 Steam 版 1.13.2-alpha6，1080p，split + RenoDX）：vtable 残余风险未发生，NR 全程生效，见下节 |
| mochizuki 的问题(ab00d2b) | 进程里有东西给每个 command list 一份堆上的 vtable 副本。他的 Windows 桥按 vtable 认"列表类",续接列表(在列表的 device 上新建)拿到的是 runtime 原本的 vtable,被当成包装器,列表内路径关闭:NR 显示开启但每帧透传("the list's device makes lists of another class") |
| 我们的路线 | split/inline 都走 `CommandListProxy`(COM 代理对象,`dlssnr/submission/CommandListProxy.h`),不改 vtable、不按 vtable 认类 |

## 静态核对结论:按设计不受影响,不改代码

1. **代理识别靠 QI,不靠 vtable**:后端用 `QueryInterface(ILogicalCommandList / IRecordingResources)` 认代理
   (`MochizukiBackend.cpp` 约 :259);`CommandListProxy::Create/CreateClosed` 的"不重复包装"守卫也是 QI。
   vtable 指针只出现在诊断里(`ProxyDiagnostics::DescribeList` 打印 `vtable=… owner=…`)。
2. **续接列表直接按 COM 调**:续接/原生列表的方法都经对象自己的 vtable 调用,不存在"拿游戏列表类的原函数去调
   另一类对象"的情况——这正是 ab00d2b 修的那一类错误。
3. **挂钩是函数体 Detour,不是 vtable 改写**:`SubmissionHooks::Arm/ArmCreate` 对 `CreateCommandList(1)`、
   `ExecuteCommandLists` 的函数体做 Detour;`D3D12_Hooks.cpp` 的 list 早期钩子、`NativeWaitHooks` 的 Draw/Dispatch
   钩子也按函数地址(vtable 槽里取到的目标)装。逐对象 vtable 副本只要最终调到 runtime 的函数体,钩子照样生效;
   `NativeWaitHooks::EnsureTarget` 按目标地址去重,副本多不会多装。

## 仍可能出问题的地方与要抓的日志

唯一残余风险:那个"逐对象改 vtable"的东西若把 device 的 `CreateCommandList` 换成**不回到 runtime 函数体**的实现,
我们的 Detour 就收不到建表调用,列表不会被包成代理。表现不是 ab00d2b 那样的"另一类"拒绝,而是
**missing recording proxy**(原画面、NR 不生效或闪烁)。

请在巫师 3(D3D12,mochizuki 后端,split,默认 inline 关)抓一次:

- `OptiScaler.ini` 里 `[DlssNr] NrFlickerDiag=true`(或 Ins → 后端诊断 → NR flicker diagnostics),进游戏到能看到
  NR 的场景停 30–60 秒,正常退出。
- 发回:`OptiScaler*.log`、`mochizuki_nr.log*`、`NrFrameDiag.log*`、`OptiScaler.ini`。
- 看的行:
  - `mochizuki missing recording proxy: … coverage=… vtable=… owner=…`——出现即上述残余风险;`owner` 指明 vtable
    属于哪个模块(d3d12core.dll 以外的就是那个改 vtable 的模块)。
  - `mochizuki recording rejected: … reason=…`——split 边界被拒的原因(与 vtable 无关的普通拒绝)。
  - `NR coverage … reasons=[…] reject_causes=[…]`——各结果的占比。

若日志显示 missing proxy 且 owner 不是 d3d12core.dll,再针对该模块做修复(按日志定,不预先改)。

## 实机验证结果（2026-10-23）

配置：Steam 版，OptScaler-NR 1.13.2-alpha6，1080p，mochizuki 后端 split，RenoDX 开，
`NrFlickerDiag=true` 诊断运行约 6 分钟（9101 帧）。结果：

- `mochizuki missing recording proxy` 0 条、`recording rejected` 0 条、无任何 vtable/owner 行
  ——上节残余风险未发生，静态结论成立，本项结案。
- coverage 最终窗口 350/350 全录、`split_accepted=351 failed=0`，NR 经 split 全程生效。
- 同机后续 C/A/A2/B 变体验证（共享纹理准入开关）见
  [mochizuki-split-transport.md](../architecture/mochizuki-split-transport.md)。
