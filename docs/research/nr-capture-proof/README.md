# GPU-armed capture proof 研究存档

2026-10-15 用户决定停止研究并撤回 CPU 完成核验。风险、测量结果、失败尝试、后续方向与止步线见
[暂缓决定及研究记录](../../architecture/nr-capture-proof-deferral.md)。

这里只保存不会执行的文本补丁，不是产品源文件，没有构建、打包、同步或自动应用入口。
不得为了恢复研究直接在交付分支应用。先从包含本目录的提交建立独立研究工作树。

## 恢复顺序

基线是**本次回退并保留其他审阅修复后的源码**，不是裸的 `4e0e721a`。
`cpu-verification.patch` 恢复回退前七个文件（含菜单、翻译与 INI 说明）的完整 CPU 核验实现与严格的零 Enqueue 断言；
保留本轮曝光所有权、诊断寿命及恢复 End proof 修复。

```powershell
git apply --check docs/research/nr-capture-proof/cpu-verification.patch
git apply docs/research/nr-capture-proof/cpu-verification.patch
```

只有需要复现已失败的两阶段 watch 实验时，再应用第二份补丁：

```powershell
git apply --check docs/research/nr-capture-proof/two-stage-experiment.patch
git apply docs/research/nr-capture-proof/two-stage-experiment.patch
```

第二份补丁仅改 `NrInlineRecording.h`，依赖第一份补丁；它未恢复吞吐，不是推荐实现。
恢复后必须重新构建，按研究记录执行 inline-armed / inline-proof 及性能对照；
补丁可应用和编译通过不代表性能、安全性或游戏验收通过。
未来源码发生变化时，冲突需逐处重审，不要模糊应用或自动忽略失败。

## 来源与完整性

两份补丁由回退前源码快照及实际测试过的两阶段实验生成。已在独立临时目录依次严格应用，
核对恢复后的七文件与安全版快照、最终 header 与实验快照逐字一致（统一换行后）。

| 文件 | SHA-256 |
|---|---|
| cpu-verification.patch | `d5a1ce03b63742158055e2ddba9185e678531820c05bc1ecf12a1dfe0010dbbd` |
| two-stage-experiment.patch | `0617797f42d097c96fe97771f74e5988f3d9865ba59c23f935b98061d7b32c12` |

哈希按存档 LF 字节计算；若 checkout 自动转换换行，计算前先统一为 LF。
长期恢复只依赖本目录和仓库代码，不依赖本地调查目录、旧可执行文件或临时脚本。
