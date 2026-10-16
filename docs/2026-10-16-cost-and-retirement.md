# 关闭态开销与分辨率回收

2026-10-16。对照 `work/notes/2026-10-15-disabled-feature-cost-plan.md` 和 `2026-10-15-resource-retirement-plan.md`。没有游戏帧时间，下面不写 FPS。

## 已改

| 项 | 结论 |
|---|---|
| 共享录制回收扫描 | `PollReleases` 对 lmxxf 和 Mochizuki 各走一次 `LmxxfRecording::Collect`。现改为每次 poll 只走一次。`Record`、`Submitted`、`ReleaseSession` 仍自己调用，关 NR 之后 poll 仍会扫。 |
| Mochizuki 闲置几何池 | 池里只留当前几何和上一个几何。更早的闲置副本在 checkout/recycle 时释放。在途录制不在池里，不在这里释放。分辨率在两个尺寸间来回时保留上一份，避免每次都重新分配。 |

策略测试：`tests/mochizuki/run.cmd pool-trim` 通过。`MochizukiNrRuntime.cpp` 以 `--skip-shaders` 编过，`exports/mochizuki-runtime/MochizukiNrRuntime.dll` 链接成功。宿主工程能编到 `AmdBridge.cpp` / `LmxxfBackend.cpp` / `MochizukiBackend.cpp`，但本工作树的 FidelityFX 与 XeSS 子模块没有检出，`xell.h` / `ffx_api.h` 缺失，`OptiScaler.dll` 没有生成。

## 已经有门控，未改行为

| 项 | 依据 |
|---|---|
| lmxxf `UpdateTiming` | 稳定关闭时在 `SetEnabled(0)` 之后的帧直接返回，不再 `GetTimings`。打开的那一次切换仍读一份快照。 |
| inline compute 标志 | 仍只在 inline 帧写入。切回 split 可能因 network key 重建，这是切换成本，不是 split 稳态在跑 inline kernel。 |
| FrameDiag | `Begin` 在关闭时返回。自动调优使用自己的 Present/时间戳，不要求打开逐帧日志。 |
| Daniel `PollRelease` | 只在 `releasePending` 时拿锁。本次不改 Daniel 的资源回收。 |
| 旧网络释放 | `ReleaseLater` 在后台等到对应 Vulkan `finished`，不占用渲染线程，也不等“再成功跑一帧”。设备丢失或等待失败时留给会话结束，这是原有隔离。 |

## 保留

| 项 | 原因 |
|---|---|
| Mochizuki 核心时间戳 | `timing_ok` 时每段录制写一对非等待 query，读回不带 `VK_QUERY_RESULT_WAIT_BIT`。菜单要显示这段 GPU 时间。拆掉需要改 vendor runtime 并打可重放补丁。没有开/关帧时间对照，按小额保留。 |
| `Info()` 每 500ms | `GetBuildProgress` 要看到编译结束。`GetInfo` 填菜单用的缓存。日志本身已有 10 秒门。 |
| GameWaitDiag 已安装钩子 | 关闭后钩子仍直通。卸钩有热切风险，计划允许保留。 |
| lmxxf 共享 import 池 | 默认开，是为了避开 HIP 在 destroy 之后仍占物理显存。本次不关、不按尺寸强行清空。 |
| 未 Reset 的可重放录制 | 仍按 generation/Reset/Release 退休。不把游戏还握着的闭合列表判成泄漏。 |

## 未测

1080/1440、3/4 槽、split/inline 的 CPU 次数和 GPU query 数没有采。上面“小额”只说明调用形态，不是测过的毫秒数。
