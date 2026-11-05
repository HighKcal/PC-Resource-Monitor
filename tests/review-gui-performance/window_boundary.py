"""One boundary snapshot only. No text, UIA, input, screen capture or hooks.

Import and call boundary_snapshot before warmup and after the measured interval
while the process still lives. This module never polls windows in the interval.
WS_VISIBLE/IsIconic and foreground are instantaneous metadata. They cannot prove
continuous visibility, foreground ownership, or absence of an occluding window.
GetWindowRect may be DPI-virtualized in the caller; it is not a screenshot size.
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import time

k = C.WinDLL('kernel32', use_last_error=True)
u = C.WinDLL('user32', use_last_error=True)
k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
k.OpenProcess.restype = W.HANDLE
k.CloseHandle.argtypes = [W.HANDLE]
k.CloseHandle.restype = W.BOOL
k.GetProcessTimes.argtypes = [W.HANDLE] + [C.POINTER(W.FILETIME)] * 4
k.GetProcessTimes.restype = W.BOOL
k.QueryFullProcessImageNameW.argtypes = [W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD)]
k.QueryFullProcessImageNameW.restype = W.BOOL
ENUMPROC = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
u.EnumWindows.argtypes = [ENUMPROC, W.LPARAM]
u.EnumWindows.restype = W.BOOL
u.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
u.GetWindowThreadProcessId.restype = W.DWORD
u.GetWindowRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
u.GetWindowRect.restype = W.BOOL
u.GetClientRect.argtypes = [W.HWND, C.POINTER(W.RECT)]
u.GetClientRect.restype = W.BOOL
u.IsWindowVisible.argtypes = [W.HWND]
u.IsWindowVisible.restype = W.BOOL
u.IsIconic.argtypes = [W.HWND]
u.IsIconic.restype = W.BOOL
u.GetDpiForWindow.argtypes = [W.HWND]
u.GetDpiForWindow.restype = W.UINT
u.GetForegroundWindow.argtypes = []
u.GetForegroundWindow.restype = W.HWND


def _identity(handle):
    created, exited, kernel, user = (W.FILETIME() for _ in range(4))
    if not k.GetProcessTimes(handle, C.byref(created), C.byref(exited), C.byref(kernel), C.byref(user)):
        raise C.WinError(C.get_last_error())
    return (created.dwHighDateTime << 32) | created.dwLowDateTime


def boundary_snapshot(pid, creation_filetime_100ns, executable):
    """Verify PID+creation+image, enumerate once, and return only target metadata."""
    started = time.perf_counter()
    handle = k.OpenProcess(0x1000, False, int(pid))
    if not handle:
        raise C.WinError(C.get_last_error())
    try:
        creation = _identity(handle)
        if creation != int(creation_filetime_100ns):
            raise RuntimeError('PID creation identity differs from the measured process')
        size = W.DWORD(32768)
        image = C.create_unicode_buffer(size.value)
        if not k.QueryFullProcessImageNameW(handle, 0, image, C.byref(size)):
            raise C.WinError(C.get_last_error())
        expected = os.path.normcase(str(Path(executable).resolve(strict=True)))
        if os.path.normcase(str(Path(image.value).resolve(strict=True))) != expected:
            raise RuntimeError('PID executable differs from the measured executable')
        windows = []
        callback_errors = []

        @ENUMPROC
        def visit(hwnd, unused):
            try:
                owner = W.DWORD()
                tid = u.GetWindowThreadProcessId(hwnd, C.byref(owner))
                if owner.value == int(pid):
                    rect, client = W.RECT(), W.RECT()
                    outer_ok = bool(u.GetWindowRect(hwnd, C.byref(rect)))
                    client_ok = bool(u.GetClientRect(hwnd, C.byref(client)))
                    windows.append(dict(
                        hwnd=int(hwnd), thread_id=int(tid),
                        visible=bool(u.IsWindowVisible(hwnd)), iconic=bool(u.IsIconic(hwnd)),
                        dpi=int(u.GetDpiForWindow(hwnd)),
                        rect=[rect.left, rect.top, rect.right, rect.bottom] if outer_ok else None,
                        client=[client.left, client.top, client.right, client.bottom] if client_ok else None))
            except Exception as exc:
                callback_errors.append(str(exc))
            return True

        if not u.EnumWindows(visit, 0):
            raise C.WinError(C.get_last_error())
        if callback_errors:
            raise RuntimeError('; '.join(callback_errors))
        foreground = u.GetForegroundWindow()
        foreground_pid = W.DWORD()
        if foreground:
            u.GetWindowThreadProcessId(foreground, C.byref(foreground_pid))
        if _identity(handle) != creation:
            raise RuntimeError('Process identity changed during metadata snapshot')
        ended = time.perf_counter()
        return dict(
            utc=datetime.now(timezone.utc).isoformat(), pid=int(pid),
            creation_filetime_100ns=creation, executable=image.value,
            foreground_owned_by_target=bool(foreground and foreground_pid.value == int(pid)),
            top_windows=windows,
            visible_non_minimized_count=sum(w['visible'] and not w['iconic'] for w in windows),
            boundary_qpc_start=started, boundary_qpc_end=ended,
            boundary_call_wall_ms=(ended-started)*1000,
            qualification='Instantaneous boundary metadata; continuous unobscured visibility and foreground are unverified')
    finally:
        k.CloseHandle(handle)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pid', required=True, type=int)
    ap.add_argument('--creation', required=True, type=int)
    ap.add_argument('--exe', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    args = ap.parse_args()
    result = boundary_snapshot(args.pid, args.creation, args.exe)
    with args.out.open('x', encoding='utf-8') as out:
        json.dump(result, out, indent=2)


if __name__ == '__main__':
    main()
