"""Stellaris DLL 注入器（Win32 CreateRemoteThread）。

改自上游 Yidhar/stellaris-mcp 的 scripts/inject.py，只把默认 DLL 路径换成我们的构建产物。
用法：
    python inject.py [dll_path]
"""

import os
import sys
import ctypes
from ctypes import wintypes
import time

PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04
INFINITE = 0xFFFFFFFF
TH32CS_SNAPPROCESS = 0x00000002

# 我们的构建产物
DEFAULT_DLL = r"<构建目录>\src\stellaris-mcp-main\build-nmake\stellaris_bridge\stellaris_bridge.dll"
INSTALLED_DLL = os.path.join(os.environ.get("USERPROFILE", ""), "Documents",
                             "Paradox Interactive", "Stellaris", "plugins", "stellaris-mcp",
                             "stellaris_bridge.dll")


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * wintypes.MAX_PATH),
    ]


kernel32 = ctypes.windll.kernel32

kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.VirtualAllocEx.argtypes = [wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD, wintypes.DWORD]
kernel32.VirtualAllocEx.restype = wintypes.LPVOID
kernel32.WriteProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.LPCVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel32.WriteProcessMemory.restype = wintypes.BOOL
kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
kernel32.GetModuleHandleW.restype = wintypes.HMODULE
kernel32.GetProcAddress.argtypes = [wintypes.HMODULE, wintypes.LPCSTR]
kernel32.GetProcAddress.restype = wintypes.LPVOID
kernel32.CreateRemoteThread.argtypes = [wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t, wintypes.LPVOID, wintypes.LPVOID, wintypes.DWORD, wintypes.LPDWORD]
kernel32.CreateRemoteThread.restype = wintypes.HANDLE
kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.WaitForSingleObject.restype = wintypes.DWORD
kernel32.GetExitCodeThread.argtypes = [wintypes.HANDLE, wintypes.LPDWORD]
kernel32.GetExitCodeThread.restype = wintypes.BOOL
kernel32.VirtualFreeEx.argtypes = [wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t, wintypes.DWORD]
kernel32.VirtualFreeEx.restype = wintypes.BOOL
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL


def list_stellaris_pids() -> list:
    h_snapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if h_snapshot == -1:
        return []
    pe32 = PROCESSENTRY32W()
    pe32.dwSize = ctypes.sizeof(PROCESSENTRY32W)
    pids = []
    if kernel32.Process32FirstW(h_snapshot, ctypes.byref(pe32)):
        while True:
            if pe32.szExeFile.lower() == "stellaris.exe":
                pids.append(pe32.th32ProcessID)
            if not kernel32.Process32NextW(h_snapshot, ctypes.byref(pe32)):
                break
    kernel32.CloseHandle(h_snapshot)
    return pids


_user32 = ctypes.WinDLL("user32")
_WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def pids_with_game_window() -> set:
    found = set()

    def cb(hwnd, _):
        if _user32.IsWindowVisible(hwnd):
            rect = wintypes.RECT()
            _user32.GetClientRect(hwnd, ctypes.byref(rect))
            if rect.right - rect.left > 400 and rect.bottom - rect.top > 300:
                pid = wintypes.DWORD()
                _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
                found.add(pid.value)
        return True

    _user32.EnumWindows(_WNDENUMPROC(cb), 0)
    return found


def find_stellaris_pid():
    pids = list_stellaris_pids()
    if len(pids) <= 1:
        return pids[0] if pids else None
    windowed = [p for p in pids if p in pids_with_game_window()]
    if len(windowed) == 1:
        others = [p for p in pids if p != windowed[0]]
        print(f"[!] 忽略无窗口的 stellaris.exe {others}，使用 {windowed[0]}")
        return windowed[0]
    print(f"[-] 有 {len(pids)} 个 stellaris.exe {pids}，其中 {len(windowed)} 个有窗口；请先关掉多余实例")
    return None


def inject_dll(pid: int, dll_path: str) -> bool:
    abs_dll_path = os.path.abspath(dll_path)
    if not os.path.exists(abs_dll_path):
        print(f"[-] DLL 不存在：{abs_dll_path}")
        return False

    print(f"[*] 目标 PID: {pid}")
    print(f"[*] 注入 DLL: {abs_dll_path}")

    h_process = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not h_process:
        print(f"[-] OpenProcess 失败，错误码 {kernel32.GetLastError()}")
        return False

    try:
        dll_bytes = abs_dll_path.encode("utf-16le") + b"\x00\x00"
        p_remote_mem = kernel32.VirtualAllocEx(h_process, None, len(dll_bytes), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
        if not p_remote_mem:
            print(f"[-] VirtualAllocEx 失败，错误码 {kernel32.GetLastError()}")
            return False

        written = ctypes.c_size_t(0)
        ok = kernel32.WriteProcessMemory(h_process, p_remote_mem, dll_bytes, len(dll_bytes), ctypes.byref(written))
        if not ok:
            print(f"[-] WriteProcessMemory 失败，错误码 {kernel32.GetLastError()}")
            return False

        h_kernel32 = kernel32.GetModuleHandleW("kernel32.dll")
        pfn_load_library = kernel32.GetProcAddress(h_kernel32, b"LoadLibraryW")
        if not pfn_load_library:
            print("[-] 找不到 LoadLibraryW")
            return False

        thread_id = wintypes.DWORD(0)
        h_thread = kernel32.CreateRemoteThread(h_process, None, 0, pfn_load_library, p_remote_mem, 0, ctypes.byref(thread_id))
        if not h_thread:
            print(f"[-] CreateRemoteThread 失败，错误码 {kernel32.GetLastError()}")
            return False

        kernel32.WaitForSingleObject(h_thread, INFINITE)
        exit_code = wintypes.DWORD(0)
        kernel32.GetExitCodeThread(h_thread, ctypes.byref(exit_code))
        kernel32.CloseHandle(h_thread)
        kernel32.VirtualFreeEx(h_process, p_remote_mem, 0, MEM_RELEASE)

        if exit_code.value == 0:
            print("[-] 远程 LoadLibraryW 返回 NULL，注入失败")
            return False

        print(f"[+] 注入成功，模块句柄 0x{exit_code.value:X}")
        return True
    finally:
        kernel32.CloseHandle(h_process)


def main():
    dll_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_DLL
    pid = find_stellaris_pid()
    if not pid:
        print("[-] stellaris.exe 没在运行，请先启动游戏")
        sys.exit(1)

    if not inject_dll(pid, dll_path):
        sys.exit(1)

    print("[*] 等待命名管道初始化…")
    time.sleep(1)
    pipe_path = r"\\.\pipe\stellaris_mcp_bridge"
    print(f"[+] 管道 {'已就绪' if os.path.exists(pipe_path) else '可能还要等一下'}")
    print(f"[i] 安装位置：{INSTALLED_DLL}")


if __name__ == "__main__":
    main()
