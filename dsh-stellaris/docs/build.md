# 构建与安装

## 0. 前提

- Windows 10/11 x64，**Stellaris 4.5.x**（桥是在 4.5.4 上做的）
- Visual Studio 2022 Build Tools（MSVC v143）。没有 CMake 也能用 `cl` 直编，见桥端 README「重建」一节
- **Node ≥ 22**：跑插件与自检脚本
- Python 3.10+（可选）：`stellaris-save/` 里的存档报告与注入脚本

## 1. 上游两个仓库

| 仓库 | 作用 |
|---|---|
| [Yidhar/stellaris-mcp](https://github.com/Yidhar/stellaris-mcp)（MIT） | 桥的骨架：IPC / 命令构造 / 主线程队列。本项目 `stellaris-bridge-patch/` 是它的修改版 |
| [Yidhar/stellaris-Launcher](https://github.com/Yidhar/stellaris-Launcher)（`stl`） | 插件加载器；**只有用它启动游戏，DLL 插件才会加载** |

## 2. 编桥

1. 取上游源码，按它的 CMake 构建得到 `stellaris_bridge.dll`；
   `stellaris-bridge-patch/source/` 是与构建树同步的副本，直接替换上游 `source/` 里的同名文件即可
2. 没有 CMake 时用 `cl` 直编（依赖 minhook 与 nlohmann/json，命令见桥端 README「重建」）
3. 产物放到 `plugins/stellaris-mcp/stellaris_bridge.dll`，**先备份官方 0.7.0 那份**，随时可回滚

> 换游戏版本（如 4.5.5）要先重生成 SDK 头文件：`tools/sdk_dumper/dump.py`，
> 再读桥端 README「参考仓库」——偏移跟着版本变。

## 3. 注入 / 换 DLL

- 正常玩法：用 `stl` 启动游戏，插件自动加载
- 开发时热替换：`stellaris-bridge-patch/rebuild_reload.py`（编译 + 卸载 + 注入；
  `--no-build` 只部署已编好的 DLL）。**游戏内热替换遇线程忙会静默崩溃**，稳妥做法是退出游戏重开

## 4. 装插件

```
node dsh-stellaris/scripts/install.mjs --profile <desktop|web>
node dsh-stellaris/scripts/install.mjs --status --profile web
node dsh-stellaris/scripts/install.mjs --remove --profile web
```

装完**完全退出 DSH 再打开**。

## 5. 自检

```
node dsh-stellaris/scripts/plugin.test.mjs     # 工具数、README 表、⚠ 集合、结构、重名、数字一致
node dsh-stellaris/scripts/toc.mjs --check     # 文档目录
node dsh-stellaris/scripts/toc-bridge.mjs --check
node dsh-stellaris/scripts/doc-audit.mjs       # 覆盖、坏表、旧名字、反引号、围栏
```

## 6. 要在游戏里自己挂的两处

桥里一共四处 hook，其中两处（星海共同体、附庸协议）需要「存一次档」才会把对象截下来。
第一次装好后在游戏里存一次档即可；重注入 DLL 之后要重新挂。细节见桥端 README「四处 hook」。
