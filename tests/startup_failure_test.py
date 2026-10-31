"""Verify denied fan-control access fails before the main GUI opens."""
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import os
import shutil
import subprocess
import sys
import time

u = c.WinDLL('user32', use_last_error=True)
u.FindWindowW.argtypes = [w.LPCWSTR, w.LPCWSTR]
u.FindWindowW.restype = w.HWND
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.IsWindowVisible.argtypes = [w.HWND]
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.GetDlgItem.argtypes = [w.HWND, c.c_int]
u.GetDlgItem.restype = w.HWND
Callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [Callback, w.LPARAM]
u.EnumChildWindows.argtypes = [w.HWND, Callback, w.LPARAM]

exe, nvml, nvapi = map(Path, sys.argv[1:4])
scenario = int(sys.argv[4])
assert not u.FindWindowW('GPUFanController.TrayHost', None), 'Close an existing app before testing.'
folder = exe.parent / ('permission-denied-' + str(scenario))
folder.mkdir(exist_ok=True)
for source in (exe, nvml, nvapi):
    shutil.copy2(source, folder / source.name)
log = folder / 'commands.log'
log.unlink(missing_ok=True)
env = dict(os.environ, GPU_FAN_FIXTURE_SCENARIO=str(scenario), GPU_FAN_FIXTURE_LOG=str(log))
process = subprocess.Popen([str(folder / exe.name)], cwd=folder, env=env, creationflags=0x08000000)
dialog = None
buttons = []

def visible_windows():
    result = []
    @Callback
    def visit(hwnd, unused):
        pid = w.DWORD()
        u.GetWindowThreadProcessId(hwnd, c.byref(pid))
        if pid.value == process.pid and u.IsWindowVisible(hwnd):
            name = c.create_unicode_buffer(128)
            u.GetClassNameW(hwnd, name, len(name))
            result.append((hwnd, name.value))
        return True
    u.EnumWindows(visit, 0)
    return result

try:
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline and dialog is None:
        windows = visible_windows()
        assert not any(name == 'GLFW30' for _, name in windows), 'main GUI must not open when permission is denied'
        dialog = next((hwnd for hwnd, name in windows if name == '#32770'), None)
        time.sleep(.05)
    assert dialog, 'startup must report denied access in a dialog'
    assert len(visible_windows()) == 1, 'only the error dialog is visible'
    messages = []
    @Callback
    def child(hwnd, unused):
        value = c.create_unicode_buffer(2048)
        u.GetWindowTextW(hwnd, value, len(value))
        messages.append(value.value)
        cls = c.create_unicode_buffer(128)
        u.GetClassNameW(hwnd, cls, len(cls))
        if cls.value == 'Button':
            buttons.append(hwnd)
        return True
    u.EnumChildWindows(dialog, child, 0)
    assert '风扇控制权限' in '\n'.join(messages), messages
    assert not (folder / 'settings.ini').exists(), 'failed startup does not save or overwrite settings'
    before = log.read_text()
    time.sleep(.3)
    assert log.read_text() == before, 'control worker is already stopped while error dialog is visible'
    if scenario == 10:
        assert not any(line.startswith('S ') for line in before.splitlines()), 'denied writes never start PID'
    else:
        assert 'S 0 42' in before and 'R 0 0' in before, 'successful first fan is restored after second fan denies permission'
    button = u.GetDlgItem(dialog, 1) or (buttons[0] if buttons else None)
    assert button, 'startup error dialog has an OK button'
    u.PostMessageW(button, 0xF5, 0, 0)  # BM_CLICK
    time.sleep(.3)
    assert process.wait(timeout=5) == 1, 'acknowledging startup failure exits with nonzero status'
    assert not u.FindWindowW('GPUFanController.TrayHost', None), 'no background instance remains'
    print('Startup permission failure passed:', scenario)
finally:
    if process.poll() is None:
        if dialog:
            if buttons:
                u.PostMessageW(buttons[0], 0xF5, 0, 0)
            else:
                u.PostMessageW(dialog, 0x10, 0, 0)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.terminate()  # This process uses only isolated simulated driver DLLs.
            process.wait(timeout=5)
