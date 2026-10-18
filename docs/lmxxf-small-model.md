# lmxxf Small 模型

lmxxf 菜单的 **Network model** 可选 Original（71块）和 Small（310.9.16，31块）。
默认 Original；Small 是另一套训练权重，实际执行一次网络，画面可能改变。
切换从下一次网络重建生效，没有 F9 热键。Original 的叠层、预测和肤色偏好会保留。

Setup 找到本地 `dlssnr-small.bin` 时会自动转换为游戏目录下的
`native-game-tiled-assets/small`。需要 Python3.10+，转换只使用标准库。
没有该包时，可从用户自己提供的受支持 `nvngx_dlssnr.dll`（310.9.16）离线准备：

```powershell
python tools/install/lmxxf-model.py --dll C:/path/to/nvngx_dlssnr.dll --output C:/game/native-game-tiled-assets/small
```

解包安装目录中的工具位于顶层，命令改用 `python lmxxf-model.py ...`。
也可以用 `--pack C:/path/to/dlssnr-small.bin` 代替 `--dll`。
不加载执行 NVIDIA DLL，不下载或随包分发权重。只接受固定受支持版本，
93 项输出必须与固定上游模型哈希完全一致才会写入最终目录。

已有目标目录时需显式加 `--replace`；完整验证后才替换，旧目录保留为
`small.previous-*`。可用 `--verify C:/game/native-game-tiled-assets/small` 检查安装。
准备好后重启游戏，菜单选择 Small。INI 是 `[DlssNr] LmxxfNetworkModel=1`，0为Original。

Small 支持720/900/1080和自由尺寸、Off/Native history。
Temporal Mode2/3或不兼容的高级内核组合会使用Original，菜单显示原因。
缺失/损坏资产同样回退Original；修复模型后重启。
安装脚本不会替用户更改模型偏好。来源和验证边界见[消费审阅](lmxxf-044-consumer-review.md)。
