# lmxxf 0.44 消费审阅

范围固定为 `3a1be68c35dbac7d7edeb2073723e82076bb1a39` →
`d50cde4b4289e165773482795e4c79817a4da510`（16 个提交）。
逐项路径、开关、模块配方与内容指纹在 `third_party/lmxxf/upstream-review.json`。
本次同步接入 0.43 的两项优化及 0.44 的 Small 网络；默认继续使用 Original。
用户要求模型只从菜单切换，不接入 F9。

## 覆盖范围

最终 diff 共 640 个路径。按实际消费者审查，不按 commit 重复审批：

| 分组 | 路径数 | 结论 |
|---|---:|---|
| 网络、bridge、几何、shader、runtime、benchmark、HIP/checksum | 24 | 生产闭包同步；addon/runtime 参考实现按下面的消费者边界处理 |
| `Development/HIP/experiments/five-bitexact-20261017/` | 12 | 深查候选的最终采用/停止情况；实验脚本、补丁不进入生产配方 |
| `Development/HIP/experiments/small-model/` | 14 | 深查转换、几何、history、oracle 域和热切资源；诊断抓取/注入工具不接入 |
| `Development/tools/package-043.ps1`、`package-044.ps1` | 2 | 检查隐含默认值、Small 文件和模块来源；不执行作者打包部署脚本 |
| `scripts/` 配置、三种部署模板、六份包说明 | 11 | 检查新默认与热键；产品 INI/menu 继续优先，不复制模板覆盖偏好 |
| `Development/releases/0.43/`、`0.44/` | 43 | 发布元数据/校验/脚本证据；不把作者发布测试当本产品测试 |
| `Development/results/` | 527 | 逐路径登记、按下面的实验结果归组；数据/日志/安装回滚不进入产品 |
| 变更记录、README、DevHistory、WorkingPlan、failed-experiments | 7 | 交叉核对范围和停止项，文档不作为执行路径 |

527 个 results 路径包括：Small 第一阶段 235、完整几何阶段 201、310.9.16 分析 49、
five-final 16、两次游戏部署各 7、Upbody 4，以及 decoder/down/pool32/gather 各 2。
路径清单保留每个文件的单独判断和指纹；无关联的原始 CSV、图像 SHA、日志不逐行重复长篇分析。

## decoder-down

完整链路是 `hip_reference_network.h` 的 `DecoderF8W16Mode` / `DsF8W16Mode` →
`Down` / `Up` / block47 byte output → `mh_fast` / `deep_fast` →
`multihead_fast_padded.hip` / `deep_fast.hip` 的 `_f8w16` 导出。

- `NR_DEC_F8_W16` 优先于 `DLSS5_HIP_DEC_F8_W16`，未设置时模式 3。
  bit0 接 block47→Up48 字节边，bit1 用 FP8 W16 权重；按 `HasFn` 成对保护。
- `NR_DS_F8_W16` 未设置默认开。grouped Down 的 `_hin` / `_out8` 等组合只在相应导出存在时使用。
- 新权重键 `@decoder-f8w16`、`@ds-frag-f8w16` 与旧 half/fp8 权重分开，
  避免同一缓存地址被另一种布局解释。`FragmentPackedMatrixW16` 在上传前重排。
- decoder 保持 K16 累加/分段求和和 wide epilogue；Down 改为字节 LDS，
  仍是每 K32 两次 K16 WMMA、原有 half(acc+sum)、pool 和 padding 次序。
  新 variant 没有用宽松误差阈值替代逐位比较。
- 受控模块配方及既有 `HIP_FFN_LINE_STORES=1` 覆盖不变；
  `swin_persistent_network.h` 只修正行尾注释，不引入新 SP 调度策略。

作者 `Development/results/five-final-20261017/README.md` 的联合 ABBA 为
1080 −20.846us、900 −34.487us；不是历史各项收益相加，也不是本产品实测加速。
本地 RX 9070 XT 使用相同新模块，把上述两个开关同时设 0 / 默认 3：
720 输出 `5d99a5bca4849fb3`，900 `27192048f9a3975e`，1080 `806dd30da2c516da`，
两组逐位一致（固定合成输入、FAST1、Original MP1、无历史）。

## small

Small 是 310.9.16 重新训练的 31 块模型，不能用 Original 的跳块列表或权重代替。
`hip_reference_network.h` 保留八个 ViT，按 `small_model_assets.h` 的兼容角色使用
`0,1,4,5,8,9,14,15,22,23,30,31..38,39,40,47,48,55,56,61,62,65,66,69,70`。
encoder 各对 shift 0/3，decoder 各对 1/2；深层按 round4(ceil(n/2)) 生成几何。
Small 禁用 Original 专用 SP/Pulse/PDL 路径；不启用失败的 Up56/62/66 FP8 候选。
自适应 ViT gain 使用 Small 的八个 ViT 权重；旧 gain 文件捷径不能用于 Small。

产品 `LmxxfModelOptions.h` 明确解析 `[DlssNr] LmxxfNetworkModel=0|1`，
通过 `ConfigKeys.h` 的 `DLSS5_NETWORK_MODEL` alias 跨 CRT 传递。
Config/menu 胜过 flags/外部环境，默认 Original；Original 的 1..3 遍、预测和肤色偏好保留。
直接调用 runtime 且没有模型 alias 时兼容显式 `DLSS5_MULTI_PASS=0.5`，
未设置不自动采用上游的 Small 默认。Small 实际只跑一次。

新 `consumer-model-mode.patch` 为 `IntegrationOptions` 增加显式 `model_mode`，
Network 构造时优先使用消费者的选择；不改 addon 的默认语义。
模型变化通过产品既有 bridge/recording chain 重建机制，旧录制继续持有原链和权重。
不调用 `SetModelMode` 热切，不调用 `NativePollRequestedModelMode` / addon `SyncModelMode`；
`allow_vit_hotkey=false`、`allow_input_poll=false` 保持。F9 不进入产品控制路径。

Small 要求 wave-owned、C512 M32、ViT projection N64 等兼容组合，
不允许 graph、实验 temporal、feature tap、base skip。
权重缺失/损坏、选项不兼容或 Temporal Mode 2/3 时按原 Original 几何回退，
状态和菜单给出具体原因。LowFrequency 对 Small 的适配暂缓，不能把回退说成 Small 已支持。

## geometry-history

`native_network_geometry.h` / `native_game_rgb_input.h` / `native_game_codec.h` /
`native_rgb_texture.h` 及对应三份 shader 传递 processing width。
720 Small valid1280×720、work1344×768；900 valid1600×900、work1600×1024；
1080 valid1920×1080、work1920×1152。输入额外列反射、RGB/输出使用各自 work stride。
free1001×777 的 work1024×832，不能把 valid width 直接作为 history buffer 步长。

产品 `NativeTemporalHistory.h` 的 96B Parameters ABI 不变，reserved.z 带 processing width，
reserved.y 带按模型确定的 blend，分配/Matches/dispatch/所有索引统一为 work stride。
Small 用独立 `post70-history-head.f16` 和 half0x3A23（0.76708984375）；
Original 用既有行和 half0x39EB（0.73974609375）。logit offset 按 workW×workH×3。
模型/尺寸重建后 priming；已经录制的 job 捕获原参数与原 chain，不从新模型借用 history。

`src/native_fast_history.h` / `native_addon_fast_history.h` / `native_game_frame.h` /
`native_temporal_experiment.h` 是作者 addon 的消费者：仅用于核对相同尺寸/常量/资源前提，
不替换产品自己的 history、motion/depth 门控、队列和录制生命周期。
上游独立 `src/LmxxfNrRuntime.cpp` 的热配置重建也不替换产品 C ABI wrapper。

## assets-install

复用 Mochizuki 已验证的离线提取器和 Small pack，网络与通道布局仍走 lmxxf。
`tools/install/lmxxf-model.py` 只接受完整 pack SHA
`7e966e2639dbec6596aa0bac3b8761137d2c40ca8cb4aca30df7ca10043c6e33`，
或由既有提取器从 DLL SHA
`2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f` 离线提取。
不加载执行 NVIDIA DLL，不分发模型系数。

转换重新建立 lmxxf channel basis、W1/W2/skip、QKV FP8、位置 bias、ViT bit scatter、
首尾投影与 history row；93 项原始字节和 logical_f32 SHA 都匹配目标 pin 的
`Development/results/small-model-20261018/model-manifest-small.json`。
生成的 C++ allowlist 只含名称/大小/哈希。

安装先在同一父目录完成转换、逐文件验证，再切换目录。
`--replace` 保留 `small.previous-*`，发布失败恢复原目录；拒绝链接路径。
验证还拒绝额外 f32 覆盖已验证 f16 的优先级问题。Runtime 首次请求 Small 验证固定
93 项及 bridge 所需 manifest 身份；失败从 Original 几何开始，避免 bridge 内部回退后尺寸错配。
此结果缓存到 session，安装/修复模型后重启游戏。

Setup 发现本地 Mochizuki Small pack 后准备 `native-game-tiled-assets/small`，
转换失败显示原因并保留 Original。lmxxf 单独安装也可用工具的 `--dll` / `--pack` 手动准备。
包仅增加转换器、manifest、PowerShell helper；INI 默认0，不改已有模型/叠层偏好。

## experiments-deployment

深查 five-bitexact 的候选补丁、最终配方和结果：

| 候选 | 生产关联和决定 |
|---|---|
| block47 byte / decoder W16 | 已进入网络和 deep_fast；接入，见 decoder-down |
| grouped Down byte LDS / W16 | 已进入网络和 multihead_fast_padded；接入 |
| producer gather folding | 作者三轮均无均值改善；生产开关仍关，不应用候选补丁 |
| C32 pool 四列合并 | 均值回退；保留原四 wave，不应用候选补丁 |
| fused Up56/62/66 FP8 | 部分路径回退且完整 Up66 Gold 失败；不接入，不归咎于全部 FP8 算术 |

Small experiments 的 capture/oracle 只用于区分 NV 内部 Post 和最终 API 合成域，
正式消费者不注入 NGX、不抓游戏 GPU 参数。作者 900/720 的约47dB 是 HIP 与内部 Post 对账，
不是逐位相同或本侧画质评分。上游第一阶段缺全档/History 的记录已由 small-complete 补齐，
不能只读早期说明就排除最终生产分支。

`package-044.ps1` 比 043 加入93 Small 文件、新 head/geometry、默认0.5和 F9/HOT；
80 模块按作者已验 SHA 复用。三个 flags 模板都改0.5；RE9额外打开 HOT/F9/F8。
这些是作者发行/部署选择，本侧不运行这些生成器，不引入热键或更改用户配置。
两次 game-install 的 manifest/rollback 是作者机器的文件操作证据，没有本侧消费者。
release 下元数据、来源 archive 检查及日志用于核实范围，不安装其中二进制。

## existing-switches

把旧 pin 和目标的扫描证据对比：除新增 `small_geometry`、`DLSS5_HIP_DEC_F8_W16`，
Options、kernel 宏配方/默认值和本地 assignments 均无新差异。
已有 flag 的 profile 值差异只有 MULTI_PASS，以及 RE9 增加 HOT/F9/F8；均已在上面审查。
`NR_DEC_F8_W16`、`NR_DS_F8_W16` 不在 DLSS5 文本扫描范围，但已显式审阅。
部分旧项因同一文件 blob 改变或新增实验引用而失效：逐项保留原分类、是否启用及暂缓条件，
复核最终 diff 后补充本次证据，不把它们统一改为已启用。
既有 submit_pulse=0、graph/诊断/消融/实验路径保持原消费者限制。

## validation

实际在 Windows / RX9070XT gfx1201 完成：

- 固定目标源码 archive，Linux LLVM23.1.2 RowOpts / `-real-true16` 生成12模块，
  其余68模块由重新编译的 `rtc_compile.cpp` / COMGR 生成；两架构各40。
  modules.json、生成源码、defines、LLVM provenance 经正常模块包装器校验；
  逐个 ELF machine=AMDGPU、gfx1200 flags0x48、gfx1201 flags0x4e。
- Runtime MSVC 构建、OptiScaler Release 宿主及 person worker 构建通过。
  `check-module-contract.ps1` 通过；`tests/lmxxf/run.cmd abi` 的 C/C++ ABI 与23项检查通过。
- `tests/lmxxf/run.cmd model`：选择、fallback、独立Original遍数、alias优先、几何，
  以及坏包/损坏/转换失败/发布失败不破坏旧资产通过。
  设置 `LMXXF_SMALL_PACK` 实际转换93项 golden；`--dll` 路径也实际转换完成。
- `tests/install/test_lmxxf_small.py` 在源码及含空格的解包布局执行真实 PS5.1 helper，
  坏包保留旧文件；`tests/host/test_config_priority.py` 在 MSVC 环境12项全过。
  菜单本地化3项及生成 allowlist 新鲜度检查通过，中文菜单字体已重建。
  `tests/host/menu-localization.cmd` 的144组语言/字体/宽度/缩放布局、字形和WARP检查通过；
  同步工具8项配方/补丁重放/冲突/审阅证据回归通过。
- `tests/lmxxf/run.cmd native-temporal` 的 WARP history/Mode2/3、4K/ultrawide、
  Small X反射/步长/blend解析检查通过；新case是valid65、processing72。
- `tests/lmxxf/run.cmd model-gpu`：Small720/900/1080、各重复两次、跨模型/档位切换，
  Original和Small往返逐位恢复。Small720 `f10b86dcca88ded8`、1080 `47faf3586a6c5604`。
  Small独立/inline history的active/priming/reset/missing-guides/queue/discard/replay通过，
  切到Original后旧Small录制仍能重放，D3D debug无错误。
  移走manifest或增加未验证f32同名文件的独立进程验证均回退Original，720哈希恢复为
  `5d99a5bca4849fb3`，测试后恢复原资产。
- Small自由尺寸1001×777，在AE0/1下完成并通过有限性，均输出 `f219054ed9855154`；
  两帧不足以宣称自适应长期收益。Original W16开关配对结果见 decoder-down。

正常同步最终退出0，`UPSTREAM.md`推进至目标d50cde4，状态reviewed、skipped_checks为空；
模块包/校验和与Runtime构建均由同步流程再次检查通过，仍保留3项显式暂缓。

未做游戏实玩、长时VRAM/队列压力、所有尺寸/模式组合、gfx1200真机或本产品ABBA性能验收。
未重新跑全部ci，未准备发行包，未发布。同步完成表示来源/接入已审查，不能称全功能或发布就绪。

## deferrals

保留既有 integration、submit_pulse option/flag 三项暂缓及原可执行验收条件。
追加 Small Mode2/3 的明确后续：适配LowFrequency的processing stride、模型参数和录制资源，
完成WARP解析、GPU冷启动/切换/重放及真实连续帧验收后才允许Small运行这些模式。
当前自动回退Original。gfx1200需在对应硬件验证Small各档和History；
游戏/长时/性能验收需使用完整匹配包，不能从本地合成输入吞吐推导FPS。
