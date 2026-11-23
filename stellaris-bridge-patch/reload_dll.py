"""热替换 stellaris_bridge.dll：不必重启游戏。

改自上游 Yidhar/stellaris-mcp 的 scripts/reload_dll.py。
流程：卸载旧模块（FreeLibrary 多次直到引用计数归零）→ 注入新构建。

用法：
    python reload_dll.py                 # 构建产物 → 正在运行的游戏
    python reload_dll.py --install       # 构建产物 → 也复制到插件目录
    python reload_dll.py other.dll       # 指定 DLL
"""

import os
import sys
import ctypes
from ctypes import wintypes
import shutil
import time

import inject

PROCESS_ALL_ACCESS = 0x1F0FFF
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
INFINITE = 0xFFFFFFFF

kernel32 = ctypes.windll.kernel32


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("th32ModuleID", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("GlblcntUsage", wintypes.DWORD),
        ("ProccntUsage", wintypes.DWORD),
        ("modBaseAddr", ctypes.c_void_p),
        ("modBaseSize", wintypes.DWORD),
        ("hModule", ctypes.c_void_p),
        ("szModule", wintypes.WCHAR * 256),
        ("szExePath", wintypes.WCHAR * 260),
    ]


def find_module(pid, module_name):
    h_snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if h_snap == -1:
        return None
    me = MODULEENTRY32W()
    me.dwSize = ctypes.sizeof(MODULEENTRY32W)
    found = None
    if kernel32.Module32FirstW(h_snap, ctypes.byref(me)):
        while True:
            if me.szModule.lower() == module_name.lower():
                found = me.hModule
                break
            if not kernel32.Module32NextW(h_snap, ctypes.byref(me)):
                break
    kernel32.CloseHandle(h_snap)
    return found


def eject_dll(pid, module_name="stellaris_bridge.dll"):
    h_k32 = kernel32.GetModuleHandleW("kernel32.dll")
    pfn_free_library = kernel32.GetProcAddress(h_k32, b"FreeLibrary")

    count = 0
    while True:
        h_mod = find_module(pid, module_name)
        if not h_mod:
            if count == 0:
                print(f"[*] {module_name} 当前没被加载（PID {pid}）")
            else:
                print(f"[+] 卸载完成，共调用 FreeLibrary {count} 次")
            return True

        count += 1
        print(f"[*] 找到模块 0x{h_mod:X}（第 {count} 次），卸载…")
        h_proc = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
        if not h_proc:
            print(f"[-] 打开进程失败：{kernel32.GetLastError()}")
            return False

        h_thread = kernel32.CreateRemoteThread(h_proc, None, 0, pfn_free_library, ctypes.c_void_p(h_mod), 0, None)
        if not h_thread:
            print(f"[-] 创建远程线程失败：{kernel32.GetLastError()}")
            kernel32.CloseHandle(h_proc)
            return False

        kernel32.WaitForSingleObject(h_thread, INFINITE)
        kernel32.CloseHandle(h_thread)
        kernel32.CloseHandle(h_proc)
        time.sleep(0.1)

        if count > 25:
            print("[-] 25 次仍未卸载干净")
            return False


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    install = '--install' in sys.argv

    pid = inject.find_stellaris_pid()
    if not pid:
        print("[-] stellaris.exe 没在运行")
        sys.exit(1)

    dll_path = os.path.abspath(args[0]) if args else inject.DEFAULT_DLL
    print(f"[*] Stellaris PID: {pid}")
    print(f"[*] 新 DLL: {dll_path}")

    if install:
        shutil.copy2(dll_path, inject.INSTALLED_DLL)
        print(f"[+] 已复制到插件目录：{inject.INSTALLED_DLL}")

    eject_dll(pid, "stellaris_bridge.dll")
    time.sleep(1.0)

    if not inject.inject_dll(pid, dll_path):
        print("[-] 注入失败")
        sys.exit(1)

    time.sleep(1.0)
    pipe = r"\\.\pipe\stellaris_mcp_bridge"
    print(f"[+] 完成；管道 {'已就绪' if os.path.exists(pipe) else '稍后才出现'}")


if __name__ == "__main__":
    main()
