# dsh-stellaris — 《群星》的 DSH 插件 + 本地桥

把《群星》（Stellaris 4.5.x）接进 DSH（DeepSeek Harness）的三块内容：

| 目录 | 是什么 |
|---|---|
| `dsh-stellaris/` | 插件：**187 个 `stellaris_*` 工具**，经命名管道与游戏内的桥通信 |
| `stellaris-bridge-patch/` | 桥：C++ DLL，挂在游戏进程里，**299 个 IPC 方法**（内存直读 + 调引擎自己的命令），附逆向笔记 |
| `stellaris-save/` | 存档侧的 Python 小工具（对账、注入、各类报告） |

## 先读哪份

- **[dsh-stellaris/docs/tools.md](dsh-stellaris/docs/tools.md)** — 187 个工具做什么、参数、状态（最常翻）
- **[dsh-stellaris/docs/build.md](dsh-stellaris/docs/build.md)** — 构建、注入、安装、自检
- **[dsh-stellaris/README.md](dsh-stellaris/README.md)** — 概念与术语、工具总表、未接清单
- **[stellaris-bridge-patch/README.md](stellaris-bridge-patch/README.md)** — 引擎内部：字段在哪、命令怎么造、试过什么不成立

## 依赖

- Stellaris 4.5.x（Windows）
- [Yidhar/stellaris-mcp](https://github.com/Yidhar/stellaris-mcp) 的 DLL 骨架（MIT）与 [stl](https://github.com/Yidhar/stellaris-Launcher) 启动器
- DSH（插件通过 `apply(ctx)` 往 `ctx.tools` 注册工具）
- Node ≥ 22；Python 3.10+（仅存档工具需要）

## 说明

- **非官方**，与 Paradox Interactive 无关；不含任何游戏文件、资源或存档。
- 桥只在本机进程内读内存、调游戏自己的命令接口，不联网、不改游戏文件。
- 工具表里的状态列：✅ 已在游戏里跑通，⚠ 桥里有、插件已接但没实测过——⚠ 的请先在备份档上试。

## 致谢 / 思路来源

- **[V2t-Nostalgia/OGAS](https://github.com/V2t-Nostalgia/OGAS)**（原 Imperial Auto Governor，GPL-3.0）：
  「不自己拼命令，而是调用游戏自己的生产者函数」这条思路来自它。只读过其公开文档与逆向记录，
  本项目**不含**它的代码——两边实现不同（这里是一部 C++ DLL + 本地 IPC，那边是 Python agent）
- **[Yidhar/stellaris-mcp](https://github.com/Yidhar/stellaris-mcp)**（MIT）：桥的骨架
- **[Yidhar/stellaris-Launcher](https://github.com/Yidhar/stellaris-Launcher)**：插件加载器 `stl`

## 许可

MIT，见 [LICENSE](LICENSE)；上游与第三方许可见 [NOTICE](NOTICE)。
