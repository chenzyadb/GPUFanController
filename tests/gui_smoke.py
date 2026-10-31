"""Windows desktop integration check; runs only against project-built fake drivers."""
import ctypes as c
import configparser
from ctypes import wintypes as w
from pathlib import Path
import shutil
import os
import struct
import subprocess
import sys
import time
import threading

u = c.WinDLL('user32', use_last_error=True)
g = c.WinDLL('gdi32', use_last_error=True)
u.FindWindowW.argtypes = [w.LPCWSTR, w.LPCWSTR]
u.FindWindowW.restype = w.HWND
u.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.GetClassLongPtrW.argtypes = [w.HWND, c.c_int]
u.GetClassLongPtrW.restype = c.c_size_t
u.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
u.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM, w.UINT, w.UINT, c.POINTER(c.c_size_t)]
u.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u.IsWindowVisible.argtypes = [w.HWND]
u.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
u.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
u.ClientToScreen.argtypes = [w.HWND, c.POINTER(w.POINT)]
u.SetCursorPos.argtypes = [c.c_int, c.c_int]
u.SetForegroundWindow.argtypes = [w.HWND]
u.PrintWindow.argtypes = [w.HWND, w.HDC, w.UINT]
u.GetDC.argtypes = [w.HWND]
u.GetDC.restype = w.HDC
u.ReleaseDC.argtypes = [w.HWND, w.HDC]
g.CreateCompatibleDC.argtypes = [w.HDC]
g.CreateCompatibleDC.restype = w.HDC
g.CreateCompatibleBitmap.argtypes = [w.HDC, c.c_int, c.c_int]
g.CreateCompatibleBitmap.restype = w.HBITMAP
g.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
g.SelectObject.restype = w.HGDIOBJ
g.BitBlt.argtypes = [w.HDC, c.c_int, c.c_int, c.c_int, c.c_int, w.HDC, c.c_int, c.c_int, w.DWORD]
g.GetDIBits.argtypes = [w.HDC, w.HBITMAP, w.UINT, w.UINT, c.c_void_p, c.c_void_p, w.UINT]
g.DeleteObject.argtypes = [w.HGDIOBJ]
g.DeleteDC.argtypes = [w.HDC]
EnumProc = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u.EnumWindows.argtypes = [EnumProc, w.LPARAM]

def wait_for(predicate, label, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError(label)

def windows(pid, visible_only=True):
    result = []
    @EnumProc
    def callback(hwnd, unused):
        owner = w.DWORD()
        u.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid and (not visible_only or u.IsWindowVisible(hwnd)):
            result.append(hwnd)
        return True
    u.EnumWindows(callback, 0)
    return result

def main_window(pid):
    for hwnd in windows(pid):
        class_name = c.create_unicode_buffer(256)
        title = c.create_unicode_buffer(256)
        u.GetClassNameW(hwnd, class_name, len(class_name))
        u.GetWindowTextW(hwnd, title, len(title))
        if class_name.value == '#32770':
            details = []
            @EnumProc
            def child_text(child, unused):
                text = c.create_unicode_buffer(2048)
                u.GetWindowTextW(child, text, len(text))
                if text.value:
                    details.append(text.value)
                return True
            u.EnumChildWindows.argtypes = [w.HWND, EnumProc, w.LPARAM]
            u.EnumChildWindows(hwnd, child_text, 0)
            raise AssertionError('unexpected startup error dialog: ' + title.value + ' | ' + ' | '.join(details))
        if title.value == 'GPU 风扇控制':
            return hwnd
    return None

def screenshot(hwnd, path):
    rect = w.RECT()
    u.GetWindowRect(hwnd, c.byref(rect))
    width, height = rect.right - rect.left, rect.bottom - rect.top
    screen = u.GetDC(None)
    memory = g.CreateCompatibleDC(screen)
    bitmap = g.CreateCompatibleBitmap(screen, width, height)
    old = g.SelectObject(memory, bitmap)
    try:
        # Capture this window only; another app may be in front of it.
        assert u.PrintWindow(hwnd, memory, 2)
        g.SelectObject(memory, old)
        header = struct.pack('<IiiHHIIiiII', 40, width, -height, 1, 32, 0, width * height * 4, 0, 0, 0, 0)
        info = c.create_string_buffer(header)
        pixels = c.create_string_buffer(width * height * 4)
        assert g.GetDIBits(memory, bitmap, 0, height, pixels, info, 0)
        path.write_bytes(struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54) + header + pixels.raw)
    finally:
        g.DeleteObject(bitmap)
        g.DeleteDC(memory)
        u.ReleaseDC(None, screen)

def click(hwnd, x, y):
    rect = w.RECT()
    u.GetClientRect(hwnd, c.byref(rect))
    point = w.POINT(round(x * rect.right / 720), round(y * rect.bottom / 640))
    position = (point.y << 16) | (point.x & 0xFFFF)
    u.ClientToScreen(hwnd, c.byref(point))
    u.SetCursorPos(point.x, point.y)
    # Deliver to the test window, never to another app obscuring its sliders.
    for message, buttons in ((0x200, 0), (0x201, 1), (0x202, 0)):
        result = c.c_size_t()
        assert u.SendMessageTimeoutW(hwnd, message, buttons, position, 2, 1000, c.byref(result))
    time.sleep(.1)

def settings_equal(path, target, stop):
    if not path.exists():
        return False
    data = path.read_bytes()
    text = data.decode('utf-16') if data.startswith(b'\xff\xfe') else data.decode('utf-8-sig')
    parser = configparser.ConfigParser()
    parser.read_string(text)
    return parser.getint('Temperature', 'Target') == target and parser.getint('Temperature', 'Stop') == stop

def profile_int(path, key):
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.GetPrivateProfileIntW.argtypes = [w.LPCWSTR, w.LPCWSTR, c.c_int, w.LPCWSTR]
    kernel.GetPrivateProfileIntW.restype = w.UINT
    return kernel.GetPrivateProfileIntW('Temperature', key, 999, str(path))

exe, nvml, nvapi = map(Path, sys.argv[1:])
assert not u.FindWindowW('GPUFanController.TrayHost', None), 'Close an existing app instance before this test.'
folder = exe.parent / 'gui-smoke'
folder.mkdir(exist_ok=True)
for source in [exe, nvml, nvapi]:
    shutil.copy2(source, folder / source.name)
settings = folder / 'settings.ini'
previous_settings = settings.read_bytes() if settings.exists() else None
legacy_settings = Path(os.environ['LOCALAPPDATA']) / 'GPUFanController' / 'settings.ini'
legacy_bytes = legacy_settings.read_bytes() if legacy_settings.exists() else None
working_directory = folder / 'different-working-directory'
working_directory.mkdir(exist_ok=True)
original_cursor = w.POINT()
u.GetCursorPos(c.byref(original_cursor))
log = folder / 'commands.log'
log.unlink(missing_ok=True)
env = dict(os.environ, GPU_FAN_FIXTURE_LOG=str(log))
process = subprocess.Popen([str(folder / exe.name)], cwd=working_directory, env=env, creationflags=0x08000000)
host = None
try:
    host = wait_for(lambda: u.FindWindowW('GPUFanController.TrayHost', None), 'hidden tray host created')
    time.sleep(.5)
    assert process.poll() is None and not windows(process.pid), 'startup is tray-only, with no visible GUI or console'
    wait_for(lambda: settings.exists(), 'startup writes executable-local settings')
    assert not (working_directory / 'settings.ini').exists(), 'settings path is independent of working directory'
    assert (legacy_settings.read_bytes() if legacy_settings.exists() else None) == legacy_bytes, 'AppData settings remain untouched'
    probe = wait_for(lambda: log.read_text().splitlines() if log.exists() else None, 'startup control permission probe')
    assert probe[:4] == ['S 0 42', 'S 1 42', 'R 0 0', 'R 1 0'], 'startup verifies and restores every fan before PID control'
    # Real shell activation can send multiple notifications; users can also
    # double-click or launch another instance while OpenGL initializes.
    for notification in [0x202, 0x400, 0x203, 0x202, 0x400] * 4:
        u.PostMessageW(host, 0x8001, 0, notification)
    def sent_activation():
        for _ in range(30):
            result = c.c_size_t()
            u.SendMessageTimeoutW(host, 0x8003, 0, 0, 2, 250, c.byref(result))
            time.sleep(.005)
    activations = threading.Thread(target=sent_activation)
    activations.start()
    gui = wait_for(lambda: main_window(process.pid), 'tray click opens GUI')
    activations.join(timeout=3)
    assert not activations.is_alive(), 'activation callbacks remain responsive'
    time.sleep(1.5)
    assert process.poll() is None, 'render loop remains alive'
    assert windows(process.pid) == [gui], 'repeated activation creates exactly one GUI window'
    for kind, class_index in ((0, -34), (1, -14)):  # ICON_SMALL/GCLP_HICONSM, ICON_BIG/GCLP_HICON
        icon = c.c_size_t()
        assert u.SendMessageTimeoutW(gui, 0x7F, kind, 0, 2, 1000, c.byref(icon)), 'window answers WM_GETICON'
        assert icon.value and icon.value == u.GetClassLongPtrW(host, class_index), 'GUI and tray host share the embedded app icon'
    screenshot(gui, folder / 'gui.bmp')
    click(gui, 685, 440)
    click(gui, 31, 518)
    wait_for(lambda: settings_equal(settings, 100, 0), 'upper target / lower stop slider limits persist')
    assert profile_int(settings, 'Target') == 100 and profile_int(settings, 'Stop') == 0
    click(gui, 31, 440)
    click(gui, 685, 518)
    wait_for(lambda: settings_equal(settings, 50, 50), 'lower target / upper stop slider limits persist')
    u.PostMessageW(gui, 0x112, 0xF020, 0)  # SC_MINIMIZE
    wait_for(lambda: not u.IsWindowVisible(gui), 'minimize hides into tray')
    u.PostMessageW(host, 0x8001, 0, 0x400)
    wait_for(lambda: u.IsWindowVisible(gui), 'tray restores GUI')
    u.PostMessageW(gui, 0x10, 0, 0)  # WM_CLOSE
    wait_for(lambda: not u.IsWindowVisible(gui), 'close hides into tray')
    assert process.poll() is None, 'close keeps control process alive'
    second = subprocess.Popen([str(folder / exe.name)], cwd=folder, creationflags=0x08000000)
    assert second.wait(timeout=5) == 0, 'second instance exits without another PID loop'
    wait_for(lambda: u.IsWindowVisible(gui), 'second launch opens existing GUI')
    u.PostMessageW(host, 0x8004, 0, 0)
    assert process.wait(timeout=8) == 0, 'graceful tray exit'
    commands = log.read_text().splitlines()
    assert commands[-2:] == ['R 0 0', 'R 1 0'], 'exit restores both fans'
    writes = [line.split() for line in commands if line.startswith('S ')]
    assert writes and len(writes) % 2 == 0
    for first, second in zip(writes[::2], writes[1::2]):
        assert first[1] == '0' and second[1] == '1' and first[2] == second[2], 'all fan commands match'
    process = subprocess.Popen([str(folder / exe.name)], cwd=working_directory, env=env, creationflags=0x08000000)
    host = wait_for(lambda: u.FindWindowW('GPUFanController.TrayHost', None), 'restart with local settings')
    time.sleep(.5)
    assert settings_equal(settings, 50, 50), 'saved settings load across restart'
    u.PostMessageW(host, 0x8004, 0, 0)
    assert process.wait(timeout=8) == 0
    print('GUI checks passed: tray startup/click, sliders and persistence, hide/restore, single instance, synchronized fans, exit restoration.')
finally:
    if process.poll() is None:
        if host:
            u.PostMessageW(host, 0x8004, 0, 0)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.terminate()  # Only the isolated process with simulated drivers.
            process.wait(timeout=5)
    if previous_settings is not None:
        settings.write_bytes(previous_settings)
    else:
        settings.unlink(missing_ok=True)
    if working_directory.exists():
        working_directory.rmdir()
    u.SetCursorPos(original_cursor.x, original_cursor.y)
