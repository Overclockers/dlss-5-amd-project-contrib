# 发布相关测试入口

发版流程、打包命令、模块消费者清单与 Actions 故障处理统一见
[docs/release.md](../docs/release.md)。本页只说明测试怎样运行和覆盖什么。
工作目录为仓库根目录。

## 统一入口与范围

```cmd
tests\run-all.cmd --tier release
```

| tier | 覆盖 | 运行条件 |
|---|---|---|
| `release` | 当前 DLL ABI/Shader、关键 host/WARP 冒烟、完整包安装/重装/卸载及配置保留；日常 tag 发包默认 | Windows/MSVC/WARP |
| `ci` | host/config、shader、mochizuki ABI、lmxxf ABI/C 冒烟/WARP、安装卸载、模块打包、上传回归、同步工具及 Git 二进制往返（本周同输入成功记录可复用） | 无独立 GPU 要求；Windows/MSVC/WARP |
| `device` | host graphics 与实际 D3D12 列表/proxy 测试 | D3D12 硬件 |
| `gpu` | mochizuki Vulkan 输出/重放/跨队列/分辨率/取消（自备模型）、lmxxf 实际 HIP、格式/曝光、输出黄金哈希、录制租约、bridge 与故障回归 | AMD GPU，`LMXXF_ASSETS` 指向有效权重目录 |
| `all` | 上述全部 | 满足全部依赖 |

可组合 `--tier ci,device`，可用 `--out exports/test-run` 指定结果目录。
`tools/build/build-release-local.cmd` 已跑 `ci,device`，无需在完整构建前再重复运行。

成功的 release 或完整 ci 都会生成 `runtime-ci.sha256` 并确认测试期间 DLL 未改变。
失败或 `--skip-sync` 不生成凭证；单跑 ABI 也不生成发布检查凭证。
`LMXXF_TEST_RUNTIME` 可以指定待测 DLL，发布验证必须核对它的真实路径与哈希。
不要把分项通过或 `--fast` 编译当成完整发版验证。

日常 release 的具体保留/移出范围见 [发布检查表](../docs/release.md#减少重复验证2026-10-05)。完整 `ci` 的范围保留。测试报告会明确标注 release scope，不声称已跑全量。

## 何时必须跑

- 日常修改先审最终 diff，按实际风险运行受影响领域的专项；不因目录或文件名触发全套。
- tag 发包运行 `--tier release`；完整 ci 通过 Actions 手动入口或本地按需运行，不是每次出包前置。加适用的 device/GPU 验证。
- 增删模块：先核对模块契约及消费者，运行相关专项；必要时选择完整 ci。
- 仅文档/注释变化：可跳过编译和运行回归，检查命令、路径及链接。
- 游戏、显卡不可用时明确记 SKIP；不把模拟、WARP 或别的型号测试等同该游戏/硬件实测。

## 分项定位失败

每个领域的 `run.cmd` 是该领域入口。修复后先重跑受影响专项；最终发版仍需发布检查凭证绑定实际 runtime。不要删除断言或跳过失败的领域。

```cmd
tests\host\run.cmd ci
tests\shader\run.cmd
tests\lmxxf\run.cmd abi
tests\lmxxf\run.cmd warp
tests\lmxxf\run.cmd split-state
tests\lmxxf\run.cmd device
tests\lmxxf\run.cmd early-unity
tests\lmxxf\run.cmd hip-passthrough
tests\lmxxf\run.cmd gpu
tests\install\run.cmd
tests\sync\run.cmd
tests\sync\run.cmd --force
tests\mochizuki\run.cmd abi
tests\mochizuki\run.cmd gpu
tests\mochizuki\run.cmd shader-tail
```

ABI 入口包含 C++、C 和 Python runtime 校验，覆盖当前 ABI v3 函数表边界、旧 ABI 拒绝与模块状态。
`split-state` 是 `warp` 内的聚焦入口：验证前一列表建立的 legacy 输入在无关 GLOBAL/纹理
enhanced barrier 后仍被接受、partial guide 拒绝的原子性、显式关闭策略，以及增强输入经
COMMON 转换、真实切分、恢复和像素读回。需要支持 enhanced barriers 的 WARP；尝试启用
D3D12 debug layer，缺少 Graphics Tools 时单独报告跳过该层检查。`warp` 的包装器用例还
覆盖只暴露 Streamline native IID 的一层包装、引用平衡和拆分后的提交/读回顺序。
`early-unity` 是 `device` 内的独立定位入口：真实 UnityPlayer/OtherEngine 测试 DLL
验证早期调用来源过滤、显式开关和自动范围，以及保留列表的拆分/执行/读回/Reset。
已运行完整 `device` 时不重复运行它；不需要 HIP/Vulkan 模型。
host CI 的 `nr_multiplier` 验证 NR 专用 XeFG 倍率、基础配置保留、能力限制和失败重试；
`nr_activity_recording` 验证完整提交后才激活，诊断/回退不激活，旧会话回调不能激活新会话。
这些 CPU 测试不替代游戏中的 XeSS 倍率切换、画面和帧节奏实测。
同步入口使用临时仓库和测试模块，验证补丁、完整性、失败退出与字节往返；
它不等于已经完成真实上游集成审阅。后者仍按
[同步说明](../tools/lmxxf-sync/README.md) 执行。

GPU tier 的 EXACT/AE/R10 黄金哈希用于固定输入回归，不证明游戏帧率或整个游戏矩阵。
Mochizuki `shader-tail` 额外运行 1280×720、1129×635、640×360 的 ViT 部分 key chunk，
检查有限、非黑屏且有实际网络修正的输出，重复执行录制并导出 R11G11B10 像素。
它验证运行健康，不能代替独立数值正确性对照或 Windows 32.0.32015 驱动实测。
需要性能结论时按 [测量规则](../docs/measurement.md) 记录实际配置与产物身份。

## 工具回归复用与构建缓存

同步工具和本地试包启动器的完整回归允许本周、同内容、同环境复用。
由 `tools/lmxxf-sync/test-cache.py` 绑定所有测试输入，详细范围和失效规则见
[发版：减少重复验证](../docs/release.md#减少重复验证2026-10-05)。
`tests/sync/run-uncached.cmd` 是内部入口；日常用 `run.cmd`，审计时用 `run.cmd --force`。
缓存正确性回归也在该完整套件中。强制运行失败会清除旧成功记录。

Mochizuki构建缓存的来源/产物/工具链失效回归由 `tests/mochizuki/run.cmd abi` 执行。
恢复构建缓存之后ABI依旧每次执行。原有运行时CI凭证的失败、替换DLL、skip-sync
拒绝测试保留在install套件，不随工具缓存跳过。
