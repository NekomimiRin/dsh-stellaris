"""编译 → 部署 → 热注入，一条龙。

顺序很重要：**先卸载模块**（否则 DLL 被游戏占用，链接器写不进去，报 LNK1104），
再编译，然后把产物复制到插件目录，最后从插件目录注入（这样构建产物不再被占用）。

用法：
    python rebuild_reload.py           # 全流程
    python rebuild_reload.py --no-build # 只做部署+注入（已经编译过）
"""

import os
import shutil
import subprocess
import sys
import time

import inject
import reload_dll

ROOT = r"D:\build\stl-build2\src\stellaris-mcp-main"
BUILD_DIR = os.path.join(ROOT, "build-nmake")
BUILT_DLL = os.path.join(BUILD_DIR, "stellaris_bridge", "stellaris_bridge.dll")
# 2026-11-23：构建树从 D:\Temp 搬到 D:\build（临时目录会被清理），
# cmake 用 VS Build Tools 自带的那份（D:\Temp 里下的 3.30.5 已随清理失效）。
CMAKE = (r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
         r"\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")
VCVARS = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
# 构建树在 %TEMP% 下，随时可能被清理，所以每次成功编译后把 org source 同步到这里。
# 这是**单向**的：构建树 → 快照，快照永远不是编辑目标。
SNAPSHOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "source")


def sync_snapshot() -> None:
    """把构建树的 src/include 覆盖到 source\\（robocopy /MIR，不排除新文件）。"""
    pairs = ((os.path.join(ROOT, "stellaris_bridge", "src"), os.path.join(SNAPSHOT, "src")),
             (os.path.join(ROOT, "stellaris_bridge", "include"), os.path.join(SNAPSHOT, "include")))
    for src, dst in pairs:
        if not os.path.isdir(src):
            print(f"[-] 快照源不存在：{src}")
            continue
        r = subprocess.run(["robocopy", src, dst, "/MIR", "/NJH", "/NJS", "/NDL", "/NP"],
                           capture_output=True, text=True, errors="ignore")
        # robocopy: 0–7 都是正常（1=有文件复制）
        if r.returncode > 7:
            print(f"[-] 同步失败 {src} -> {dst}（code {r.returncode}）")
            return
    print(f"[+] source\\ 已同步（{SNAPSHOT}）")


def build() -> bool:
    if not os.path.exists(CMAKE):
        print(f"[-] 找不到 cmake：{CMAKE}")
        return False
    cmd = f'"{VCVARS}" >nul && "{CMAKE}" --build "{BUILD_DIR}"'
    print(f"[*] 编译中…")
    r = subprocess.run(cmd, shell=True, capture_output=True, text=True, errors="ignore")
    tail = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        print("[-] 编译失败：")
        print("\n".join(tail.strip().splitlines()[-15:]))
        return False
    print("[+] 编译完成")
    return True


def main():
    do_build = "--no-build" not in sys.argv

    pid = inject.find_stellaris_pid()
    if not pid:
        print("[-] stellaris.exe 没在运行")
        sys.exit(1)
    print(f"[*] Stellaris PID: {pid}")

    # 1) **先编译**（2026-11-21 改序）：编译失败就直接退出，DLL 还挂在游戏里——
    #    以前是先卸载再编译，一遇 C1061 就把桥从运行中的游戏里拔掉了，游戏会在之后崩掉
    if do_build and not build():
        print("[-] 编译失败；桥仍挂在游戏里（未卸载），可以继续玩，改完再跑一次")
        sys.exit(1)

    # 1.5) 编译成功就把源码同步进 source\（构建树在 %TEMP%，不可靠）
    sync_snapshot()

    # 2) 卸载旧模块，释放 DLL 文件
    print("[*] 卸载当前模块（释放文件占用）…")
    reload_dll.eject_dll(pid, "stellaris_bridge.dll")
    time.sleep(0.8)

    if not os.path.exists(BUILT_DLL):
        print(f"[-] 没有构建产物：{BUILT_DLL}")
        sys.exit(1)

    # 3) 部署到插件目录
    shutil.copy2(BUILT_DLL, inject.INSTALLED_DLL)
    print(f"[+] 已复制到插件目录")

    # 4) 从插件目录注入（构建产物不再被占用）
    if not inject.inject_dll(pid, inject.INSTALLED_DLL):
        print("[-] 注入失败")
        sys.exit(1)

    time.sleep(1.0)
    pipe = r"\\.\pipe\stellaris_mcp_bridge"
    print(f"[+] 完成；管道 {'已就绪' if os.path.exists(pipe) else '稍后才出现'}")


if __name__ == "__main__":
    main()
