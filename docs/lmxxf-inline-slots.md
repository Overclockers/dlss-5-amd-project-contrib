# lmxxf inline 槽位与离线对比（2026-10-13）

> 测量修订：首轮夹具容量满时排空整批，会把槽数与提交节奏混在一起。
> 现已改为仅等待最老的一帧，腾出容量即提交下一帧，并补测 2560×1440。
> 首轮“不限帧 3 槽最佳、4 槽额外获益”的选型结论撤回。
> 本页保留修复前数据和因果诊断；当前结论以末尾“共享调度修复后的验证”为准。

## 配置与资源规则

`[DlssNr] LmxxfInlineSlots` 范围 1–5，默认 3。菜单仅在 lmxxf 后端勾选
`Experimental inline execution`（`NrInline`）时显示槽位选择；inline 仍默认关闭。
改槽数无需重启，下一次录制使用新上限。缩容不会取消在途帧；容量不足时保留原图。
关闭 inline 后先等待既有 inline 预约退还，再录制 split。

沿用本项目 Daniel 接入层的 `AmdSlots` 即时选择方式，但资源模型不同：该接入层的槽位不能直接套在
lmxxf 的共享 codec/HIP/history 缓冲上。这里保留一份模型和计算区，每个可重放录制独占
原始颜色、使用到的曝光/运动/深度快照，以及最终结果和可选首层结果。

游戏列表只捕获输入、发出捕获标记、有限等待并选择结果。COMPUTE side queue 顺序执行
history 控制更新 → codec/temporal 输入 → HIP → decode/history 输出 → 私有结果拷贝。
下一帧捕获可提前，网络仍串行执行，模型算法不变。side 链只等待共享计算完成凭证，
避免等游戏消费完成造成环形等待；录制自身仍同时保留 side 和实际 game tail 凭证。

可重放列表必须在失效且双侧完成后才能归还私有资源池，池上限随槽数变化；仅 GPU 完成
不代表录制失效。因此保留大量 closed command lists 的游戏可能持有超过槽数的物理缓冲。
重放重新申请 NR 准入；已完成录制遇到满槽时选择原图，保留整个游戏提交批次，
不启动网络或占用新槽。旁路仍等待 reset-side 和 game-tail 双完成，空槽恢复后可再次运行 NR。
同一录制尚未完成时不能重置其控制缓冲，仍拒绝重叠重放；未知提交/设备异常不猜测完成。实现见
`InlineFrame.h`、`LmxxfNrRuntime.cpp`、`LmxxfRecordingOwner.h`，流程见
[inline 架构](architecture/nr-inline-recording.md)。

整包 ABI 为 6：FrameInfo 为 168 字节，新增 `inline_slots`（0=split）。需要匹配宿主和
runtime，不提供旧 ABI 回退。上游 pin 仍为 `48a41fccb89300cd6636b16bc7b86010384c4cc1`，
网络选项、模块配方和模块字节未变。`inline-compute-recording.patch` 仅在 recording leases
及 side-queue 显式许可下允许 DIRECT/COMPUTE 类型切换，设备身份及活动执行顺序仍校验。
TemporalControl 的列表和复用池同时按实际队列类型区分。

## 三个后端的“槽”不是同一层

| 名称 | 谁实现 | 保存/限制什么 | split 是否使用 |
|---|---|---|---|
| Daniel 菜单 `AmdSlots` 1–5 | 本项目 `AmdPreSr.cpp::Slot` | 桥接颜色、曝光、描述符和未完成调用 | 本项目 Daniel 后端的 inline 接入，与 lmxxf 的 split 开关无关 |
| lmxxf `LmxxfInlineSlots` | 本项目 | inline 录制准入和私有捕获/结果 | 不使用这个准入上限；shared arena 仍须按完成凭证串行 |
| Mochizuki inline 4 | 本项目宿主与 runtime 包装层 | 私有录制及在途上限 | 不使用这个私有录制槽数 |
| Mochizuki `kSlots=3` | 本项目 Vulkan 包装层 | 命令缓冲/fence 的复用环 | 使用；它不代表三份网络并行推理 |

Mochizuki 的 4 来自本仓库提交 `d0ce9fd2`（2026-10-11），目的是允许 CPU 提前录制几帧，
当时通过了 2/3/4 ahead 的机制测试，没有同场景游戏性能证据，不能称为原作者给出的最优槽数。
Vulkan 的 3 项命令缓冲复用环早在包装层初次接入 `8cdffb1a` 就存在。
上游 `nr::Runtime` 的 timing/direct-source 等其它 ring 也不能与宿主帧槽混称。

本项目的 1–5 桥接槽只管理宿主资源；不代表后端内部网络的并行能力。

“本帧等待网络结果”与“只允许 CPU 录制一帧”不同。CPU 可提前录制 N+1/N+2，GPU 仍在
每一帧的对应位置等待该帧结果。保留 inline 又去掉全部私有槽，就必须在复用前施加背压；
在 Record 中等待尚未提交的列表可能死锁。split 已提供另一种选择：拆分提交边界、用
queue/fence 排序共享计算资源，不需要这个 inline 多槽选择器，但仍有录制、fence 和命令池生命周期。
原生在游戏可用的同一 GPU API/队列内执行网络也能减少跨 API 握手；这需要另一条网络执行路径，
不是把槽位开关删除就能实现。

## 5 槽回落能说明什么

多一个空闲槽不会额外运行一遍网络；真正变化的是允许多少帧提前提交、私有资源驻留量，
以及游戏等待 shader、side queue 和 CPU worker 之间的执行节奏。新的连续补帧实测仍可见
高槽数回落，说明首轮整批等待不是全部解释，但现有 timestamp 不能把原因定位到单个组件。

### 调度因素的受控对照（11:33 补充）

同一进程/设备/网络会话，2560×1440 History、MP1、180 帧/段，显式自由分辨率和快速数值，
正反顺序重复。诊断构建仅增加采样次数统计和两种独立控制：夹具最多放行两帧，或将
`LmxxfRecordingOwner::InlineCallback` 的生产逻辑从 `WaitCapture → WaitInlineTurn`
改为 `WaitInlineTurn → WaitCapture`。后者保留五槽准入及原夹具提交/回收方式。

| 条件 | 成功 NR 帧/秒 | 每 180 帧 capture 采样次数 |
|---|---:|---:|
| split | 57.02–57.26 | 0 |
| inline 2，修复前调度 | 52.71–53.00 | 354–355 |
| inline 5，修复前调度 | 39.62–40.28 | 2179–2228 |
| inline 5，夹具只放行 2 帧 | 52.62–52.70 | 354–355 |
| inline 5，按顺序开始 capture 轮询 | 50.71–50.89 | 360 |

这证明至少有一个真实产品调度因素：多个尚未轮到执行的 worker 提前在同一 side queue
提交 capture 采样、读回及等待，产生额外队列流量和 CPU 唤醒。只改变该调度顺序、保持
五槽上限和夹具其余行为，就恢复大部分吞吐。空闲的五槽配置本身没有表现出同样损耗。
不能再把现象全部解释成夹具误差，也不能把高槽数回落当成不可避免的模型成本。

游戏中若有多帧提前提交，会走相同的 `InlineCallback` / `WaitCapture` 路径，因此存在
实际游戏性能风险；具体幅度取决于游戏的录制/提交节奏，仍不能把离线跌幅直接当游戏 FPS 跌幅。
剩余差距、等待 shader 与其它同步成本尚未拆分。

以上为修复前的诊断对照；顺序轮询现已落地，两后端的实际验证见末节。
仍未覆盖真实设备丢失、所有游戏提交节奏及长时压力。诊断构建 SHA256 为
`34503b1b7343bcf321dc1ae0b4365bac862dae214e1f0d4be68cc8a5da92688a`，
runtime/宿主身份同下表。本地原始数据为 `exports/inline-slots/causal-run.log` 和
`exports/inline-slots/causal/results.json`。
debug layer 诊断中除已知原子握手报告外无其它 ERROR。
在相同冷启动状态下，split、原五槽、按序轮询五槽三条路径的 720p History + 自动曝光 +
首层输出/配置 MP3 各 9 帧，最终及首层 hash 全部一致。验证程序增加单一模式选择，SHA256
`f79d633965c8898ceb13123e38c216ba98d2a3578f6c894e10c1274e5d7235a1`。
没有使用不同运行时状态下的 hash 相互比较：跨段 ResetHistory 并不建立完全相同的自动曝光
起点，曾出现同会话各段 hash 逐渐稳定但不同于第一段，因此像素验证使用各自冷启动；性能 A/B
仍是同会话且未启用自动曝光。
System 4101 在 11:32–11:35 测量窗口无匹配事件。

已有 Daniel 0.6.0 shader 静态对比提供了明确待测差异：作者的 atomic 轮询热循环没有
`sync_uglobal`，支持 graphics wait 和 predicated slices；本项目 `NrInlineRecording.h`
在每次轮询里执行 `DeviceMemoryBarrier`，并固定录制 64 次 compute dispatch/UAV barrier。
这些同步/调度差异是下一步 A/B 对象，不是已证明的 5 槽回落根因。不能只删发布/获取屏障来追性能。
旧 Daniel 的同局 3/4/5 测量只证明当时那套路径及工作负载未测出损失，不能推导所有 inline 实现都单调增益。

## 正确性验证

- RX 9070 XT：split 与 inline 1–5 槽分别比较 9 帧逐帧 hash，颜色每帧变化且复用同一游戏纹理。
  spatial 单层，以及 History + 自动曝光 + 首层输出、配置 3 层，两组输出均完全一致。
  3 层沿用生产预测策略，日志实际网络调用为 2 次；不声称关闭预测的三次推理也已测。
  `--verify-ahead` 保留多帧在途，并在游戏消费位置快照每帧结果，避免读回被后帧覆盖。
- 原生 history fixture 的 reset、重放和层数切换通过；inline prime/active hash 为
  `9d13537405eedb55` / `9786b8906466da18`。
- 实际 HIP 的 2/3/4/5 帧提前捕获、stagger、拒绝超容量、成功与强制超时、跨队列重放、
  Reset-before-capture 和双侧回收通过。已有 Vulkan fixture 也通过。
- WARP 准入验证覆盖缩容不丢预约、扩到 5 槽、双完成、未提交丢弃及重放重新准入。
  热切换夹具循环 5/1/3/2/4/0，零等待下实际执行 split 6 帧、inline 超时 36 帧、
  容量旁路 18 帧，确认两条路径都执行，不能只把旁路算成切换成功。
- 开启 D3D12 debug layer 的一致性/热切换测试保留已知例外：两个具名的原子握手缓冲
  `NR inline atomic capture/control` 会报告跨队列 concurrent-access，调试层无法表达此 shader
  原子协议。5 槽一致性各报告 54 条、热切换 52 条；其它 ERROR 均导致失败，本次为 0。
  这不等于全路径 debug clean，也不构成所有驱动对该协议的保证。
- 最终 runtime ABI C/C++、23 个 runtime 检查、10 个配置/ACO 检查通过；宿主、lmxxf runtime
  和 Mochizuki ABI6 adapter 构建通过，Mochizuki 复用未改 shader（`--skip-shaders`）。
  post-SR 引导专项 WARP 验证已通过。未运行全发布套件。
- 从固定 pin 严格重放清单全部补丁，规范化 LF 后 bridge SHA256 为
  `c248bda8706f4c5e34a49e604c75d0a07ba8a7b7de07ed9c636588ba64f75457`，与 vendor 相同。

## 修订后的测量方法与产物

只测生产 split/inline 桥接回调，不测游戏 FPS，也不作为上游网络本体基准。
实际 runtime + CommandListProxy/SessionOwner，FP16 输入，无游戏渲染负载。
每帧新建夹具列表/query 等成本包含在 CPU 数据中。

每个 profile 在一个进程、一个设备/网络会话内原地切换
`0→1→2→3→4→5→5→4→3→2→1→0`，每段 12 帧预热、180 帧计量、重置 History。
不限帧模式容量满时仅等最老的一帧，腾出容量即继续；所有模式均处理相同 180 帧。
60Hz 是 CPU 目标送帧节奏，inline 容量满走真实旁路，不额外强制五槽等待；
split 的夹具 pending 上限为五，必要时提交背压。计量段约 3 秒加尾部完成等待，
必须同时看成功帧数和墙钟吞吐，不能仅用覆盖率判定快慢。

表中为同会话正反两段的最小–最大范围，不是置信区间。计量关闭 debug/hash 读回，
有限等待参数 64 是工作预算，不是毫秒上限。`nr_per_s` 为实际成功 NR 帧/墙钟秒；
GPU P50 是游戏列表整段桥接和等待跨度，不是 HIP 核函数时间；VRAM 为 DXGI local
CurrentUsage 峰值，包含共享模型、纹理和缓存，正反池状态不同，不是每槽固定成本。
CSV/JSON 另存 GPU P95、CPU 均值和 observed completion；后者是 CPU 收集时观察的完成，
不能解释为显示延迟。

硬件 RX 9070 XT/gfx1201，驱动 `32.0.31041.1004`，HIP `70260201`。
HAGS 注册表未显式配置，实际状态未确认。不限帧复测窗口为 2026-10-13 11:16–11:21，
最终 paced 及 hash 验证窗口为 11:23–11:25（UTC+8）；System 4101 在 11:16–11:26 无匹配事件。
早期开发 DIRECT 控制列表提交到 COMPUTE 导致的 device removal 已修复，不使用该轮数据。

1280×720 / 1920×1080 复测保留首轮 runtime-only 配置（fast_numeric=0），仅比较本 profile 内槽数。
2560×1440 显式采用宿主默认 `DLSS5_NETWORK_FREE_RES=1`、`DLSS5_FIT_LARGE=1`、
`DLSS5_FAST_NUMERIC=1`、`DLSS5_SKIP_BLOCKS=none`、`DLSS5_MULTI_PASS=1`；
实际网络处理为 2560×1472（填充），不是缩至 1920×1080。不能把这些 profile 当不同分辨率的纯缩放曲线。
首次未配置自由分辨率的 1440 请求被 geometry contract 拒绝，未纳入结果。

构建：`tools/build/build-release-local.cmd --fast`、
`tools/build/build-lmxxf-runtime.cmd exports/inline-slots/runtime-compute`；
夹具 MSVC C++20 /O2，编译入口 `tests/lmxxf/run.cmd slots`。

| 产物 | SHA256 |
|---|---|
| 宿主 `OptiScaler.dll` | `bac9968cb16be6728c6d1ce1b950cafa6ae9621ff4829e8d45fe32476d7bd318` |
| `runtime-compute/LmxxfNrRuntime.dll` | `c39d4e5c8b3af9652dae8e6f7118be1d1099facfad40707bb4a79d296031db1d` |
| 不限帧连续补帧夹具 | `26af59ac9b6b66c970ce19f277e060a4d71cb77a18983d1964623c85f4bc19d7` |
| 最终 paced/hash 夹具 | `43962dab083c39dbf7e2c8370077f9071b54b4b6d670ab65952e962d69f41c1c` |

两版夹具的差别是最终版删除 inline pending 达到五时的额外等待，确保 paced 五槽仍按准入规则旁路。
修订后 spatial 与 History+auto exposure+首层/配置 MP3 的 split/1–5 槽 9 帧 hash 再次完全一致。
本地复测日志为 `exports/inline-slots/sliding/`，`results.json/csv` 有 60 段记录及起止时间。
首轮 `final/slots-results.*` 的整批排空数据保留为历史，不再用来推荐槽数。

## 连续补帧的不限帧结果

### 1280×720 spatial

| 模式 | 成功 NR 帧/秒 | GPU P50（ms） | 峰值 local MiB | 成功帧/180 |
|---|---:|---:|---:|---:|
| split | 185.67–189.00 | 5.26–5.35 | 1163.7–1216.3 | 180 |
| inline 1 | 70.77–73.50 | 6.19–6.25 | 1184.2–1229.3 | 180 |
| inline 2 | 159.33–160.84 | 6.04–6.09 | 1207.6–1261.0 | 180 |
| inline 3 | 150.85–151.53 | 6.28–6.33 | 1230.9–1269.3 | 180 |
| inline 4 | 116.21–118.53 | 7.42–7.73 | 1254.3–1261.8 | 180 |
| inline 5 | 121.77–133.24 | 6.68–6.87 | 1277.6 | 180 |

### 1920×1080 History

| 模式 | 成功 NR 帧/秒 | GPU P50（ms） | 峰值 local MiB | 成功帧/180 |
|---|---:|---:|---:|---:|
| split | 91.95–92.55 | 10.68–10.75 | 1560.0–1640.0 | 180 |
| inline 1 | 51.81–53.12 | 11.57–11.64 | 1630.5–1694.6 | 180 |
| inline 2 | 82.63–83.14 | 11.88–11.92 | 1703.8–1751.9 | 180 |
| inline 3 | 76.79–78.56 | 11.93–12.03 | 1777.2–1809.2 | 180 |
| inline 4 | 57.11–58.39 | 17.84–18.17 | 1850.5–1866.5 | 180 |
| inline 5 | 61.86–62.56 | 17.40–17.57 | 1923.9 | 180 |

### 2560×1440 spatial

| 模式 | 成功 NR 帧/秒 | GPU P50（ms） | 峰值 local MiB | 成功帧/180 |
|---|---:|---:|---:|---:|
| split | 60.97–62.06 | 16.08–16.36 | 1774.8–1918.6 | 180 |
| inline 1 | 40.28–40.91 | 16.78–17.02 | 1859.1–1974.2 | 180 |
| inline 2 | 55.67–56.55 | 17.33–17.49 | 1946.2–2032.5 | 180 |
| inline 3 | 53.45–53.62 | 17.72 | 2033.3–2090.8 | 180 |
| inline 4 | 43.71–44.22 | 23.80–23.84 | 2120.4–2149.1 | 180 |
| inline 5 | 44.60–45.30 | 23.59–23.68 | 2207.5 | 180 |

### 2560×1440 History

| 模式 | 成功 NR 帧/秒 | GPU P50（ms） | 峰值 local MiB | 成功帧/180 |
|---|---:|---:|---:|---:|
| split | 56.77–57.08 | 17.37–17.44 | 2004.9–2148.7 | 180 |
| inline 1 | 38.87–39.14 | 18.25 | 2132.9–2247.9 | 180 |
| inline 2 | 52.53–52.64 | 18.70–18.71 | 2263.7–2350.0 | 180 |
| inline 3 | 51.22–51.26 | 18.81–18.85 | 2394.6–2452.1 | 180 |
| inline 4 | 39.33–39.38 | 25.47–25.82 | 2525.4–2554.2 | 180 |
| inline 5 | 39.66–40.68 | 25.31–25.67 | 2656.2 | 180 |

### 2560×1440 History，CPU 目标 60Hz

| 模式 | 成功 NR 帧/秒 | GPU P50（ms） | 峰值 local MiB | 成功帧/180 |
|---|---:|---:|---:|---:|
| split | 56.74–57.81 | 17.11–17.40 | 2004.9–2148.7 | 180 |
| inline 1 | 31.34–31.68 | 17.54–17.78 | 2132.9–2247.9 | 94–95 |
| inline 2 | 47.40–47.71 | 18.41–18.59 | 2263.7–2350.0 | 143–144 |
| inline 3 | 50.23–50.62 | 18.69–18.92 | 2394.6–2452.1 | 153–154 |
| inline 4 | 42.52–43.19 | 23.16–23.20 | 2525.4–2554.2 | 131–133 |
| inline 5 | 42.61–42.63 | 23.27–23.33 | 2656.2 | 132 |

## 修复前的选型记录

NR 实际输入 2560×1440、只比较本次离线吞吐：split 最快；必须用 inline 时，
不限帧优先 2 槽，3 槽接近；目标 60Hz 送帧时 3 槽的成功 NR 帧/秒比 2 槽高。
4/5 在这里均不占优。默认 3 保留为覆盖提前录制的策略，不再称它是离线不限帧最优点。
游戏内实际最优值仍需同场景确认，尤其是帧率限制、CPU 提前录制和长时资源保留不同的游戏。

2K 显示输出不代表 NR 输入为 2560×1440：先 NR 后超分时通常按内部渲染分辨率处理，
先 SR 后 NR 才可能处理完整输出。选择时应看 NR 实际输入尺寸。

## 复现与未覆盖范围

设置权重与当前 ABI6 runtime 的绝对路径，运行
`tests\lmxxf\run.cmd slots exports\slots-recheck` 可编译夹具并执行既有 hash/热切换/性能检查。
模块从当前 checkout 的 `third_party/lmxxf/modules` 加载。补测 1440：

```bat
set "LMXXF_WEIGHTS_DIR=C:\absolute\native-game-tiled-assets"
set "DLSS5_NETWORK_FREE_RES=1"
set "DLSS5_FIT_LARGE=1"
set "DLSS5_FAST_NUMERIC=1"
set "DLSS5_SKIP_BLOCKS=none"
set "DLSS5_MULTI_PASS=1"
exports\slots-recheck\nr_slots_benchmark.exe C:\absolute\LmxxfNrRuntime.dll third_party\lmxxf\modules 0 --sweep --frames=180 --height=1440 --history
```

去掉 `--history` 为 spatial；加 `--interval=16.667` 为目标 60Hz。
夹具当前明确选择 RX 9070 XT，其它 GPU 需调整选择后重新建立证据。

待验证：游戏内同站位切换的 FPS/显示节奏、长时显存和 retained-list 压力、其它 GPU/驱动、
变化的外部曝光/运动/深度、更多模型/画幅/数值选项和全功能组合。
5 槽已通过调度顺序 A/B 确认提前 capture 轮询的真实开销；剩余差距尚未通过 profiler 拆分。
离线通过不等于游戏验收或整包发布认证。

## 共享调度修复后的验证（2026-10-13）

生产 `InlineCallback` 已改为先取得 enqueue turn，再轮询 capture。Mochizuki 和 lmxxf
共用该路径，因此两者都修复；不改变私有输入/输出所有权、网络计算串行规则和双侧回收证明。
Mochizuki 仍为 4 项 inline 准入及独立 3 项 Vulkan 命令环；没有引入新的槽位策略。

最终宿主 SHA256：`b08b8b89bc3e32d8ad515aa94be819aff637732a4d29b6a37462da9658a9bf29`；
生产头文件编译的 benchmark SHA256：`ad1bdea945b937260a00227a0ae4490c9d7f080d93fd78280afe58e5e3eca462`。
lmxxf runtime 仍为上文的 `c39d4e5c…`。硬件、驱动及 1440 配置同前；每段预热 12 帧，
1440 每段计时 180 帧，正反各一次，同进程/设备/网络；不启用 debug/readback。
720/1080 使用 90 帧/段。计时与 GPU 测试串行运行；1440 paced 最终轮也避开编译/模型解包。

| 模式 | 1440 History 不限帧成功 NR/s | 1440 History 目标 60Hz 成功 NR/s | 目标 60Hz 成功帧/180 |
|---|---:|---:|---:|
| split | 56.96–57.87 | 56.86–57.40 | 180–180 |
| inline 1 | 38.87–39.23 | 31.58–31.66 | 95–95 |
| inline 2 | 52.74–52.90 | 47.48–48.40 | 143–146 |
| inline 3 | 50.48–50.49 | 50.32–51.25 | 153–156 |
| inline 4 | 50.20–50.39 | 50.02–50.17 | 153–154 |
| inline 5 | 50.31–50.41 | 50.27–50.35 | 155–155 |

不限帧 4/5 槽从约 39–41 恢复至约 50，与 3 槽接近，说明旧回落有真实产品调度原因。
2 槽仍约 52.7–52.9，split 约 57.0–57.9；五槽相对 split 仍慢约 12%。
目标 60Hz 时 3–5 槽整体接近，均比 2 槽成功覆盖更多 NR 帧；没有证据支持默认升到 4/5。
默认 3 保留，实际输入 1440 的不限帧离线吞吐优先 2；游戏中需同场景验证。
剩余差距尚未拆分，不能宣称槽位完全无开销，也不能将此表换算为游戏 FPS。

验证命令及结果：

- `tests\lmxxf\run.cmd inline-discard`：12 次 Reset/Release、6 次真实 callback enqueue 失败、双侧完成、重放准入、缩容/扩容、未知提交保留通过；新增取消排队 worker 在无 capture 时迅速报告 `inline order`，不运行网络。
- `tests\lmxxf\run.cmd inline-gpu`：真实 HIP 成功/强制超时、2–5 ahead、交错提交、跨队列重放和 Reset 通过。
- `tests\lmxxf\run.cmd slots`：真实 history/reset/replay，split 与 1–5 槽各自冷启动的 spatial 和 History+自动曝光+首层/配置 MP3 九帧 hash 完全一致；热切换以及 720/1080 的 36 段计时通过。
- `tests\mochizuki\run.cmd inline`、`aco-inline`：原生/ACO 的成功、超时、2–4 ahead、重放、Reset、2560×1440 时域及三层（369 dispatch）通过；FLOAT 分段和 UNORM8 全 compute/三层回退均覆盖。
- `tests\mochizuki\run.cmd phase-failure`：第 2/3 个分段、第 1 个全 compute 提交失败保留资源；拒绝 compute 录制使该帧失败。
- `tools\build\build-release-local.cmd --fast`：宿主重建通过。

Debug 对比仍仅豁免已知、具名的原子握手并发访问报告；其它 ERROR 不允许。
本次测试窗口 System 4101 无记录。没有测得 Mochizuki 的性能提升百分比，也没有游戏、
真实设备丢失、长时间 retained-list 压力及其它硬件/驱动验收。inline 保持默认关闭。
本地日志 `exports/inline-slots/ordered-*.log`、`ordered-slots/` 和 `ordered-1440-results.json`
供本机复核；表中命令可从当前源码重新生成验证。

## 满槽重放旁路修复（2026-10-13）

审查发现：已完成并回收准入的 closed recording 遇到其它预约占满槽位时，
`BeforeExecute` 的 `E_PENDING` 会使 unsplit 提交钩子跳过整个原始 batch，连同普通游戏列表。
lmxxf 和 Mochizuki 共用该路径。现改为仅旁路该次 NR：control=2 让等待立即退出，
decision 仅在 control=1 时选择网络结果；final/first-pass 均选择当前原图。
不调用 runtime Begin/Enqueue/End，不增加槽位占用；实际提交后重置时域状态。
旁路私有资源仍保留到 reset-side 和 game-tail 完成，未知尾信号保持钉住。
同一录制尚未完成的重叠重放仍拒绝，释放容量后正常 NR 重放可恢复。

本次针对性验证全部通过：

- `tests\lmxxf\run.cmd inline-discard`：真实提交钩子保留混合 batch；无 runtime 调用；旁路重叠拒绝、未提交取消、Reset 前后保留/回收、满槽恢复、未知尾信号。
- `tests\lmxxf\run.cmd inline`：当前原图和首层像素、FP32 大范围、立即退出等待、enhanced layout 恢复。
- `tests\lmxxf\run.cmd inline-gpu`：真实 HIP 满槽/降槽旁路、释放后恢复并与 split hash 一致；既有成功/超时、2–5 ahead、交错、跨队列和 Reset 回归。
- `tests\mochizuki\run.cmd inline`、`aco-inline`：原生及 ACO 同样覆盖旁路/恢复；FLOAT 分段、UNORM8 全 compute、1440p 时域和三层均通过。
- `tools\build\build-release-local.cmd --fast`：宿主构建通过。

本次未重跑性能 sweep，也没有新增游戏 FPS 结论；正常准入路径及槽位默认值不变。
游戏实测、长时间 retained-list 压力和其它驱动仍待验收。
