"""开发用：启动 Pulse、点几下、截图。仅依赖标准库 + Pillow。"""
import ctypes, ctypes.wintypes as wt, subprocess, sys, time, os
from PIL import ImageGrab

user32 = ctypes.windll.user32
user32.SetProcessDPIAware()

EXE = r"D:\WorkSpace\musicdown\dist\Pulse.exe"
OUT = r"D:\WorkSpace\musicdown\build\shots"


def find_window(title, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        found = []

        @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        def cb(hwnd, _):
            n = user32.GetWindowTextLengthW(hwnd)
            if n:
                buf = ctypes.create_unicode_buffer(n + 1)
                user32.GetWindowTextW(hwnd, buf, n + 1)
                if title in buf.value and user32.IsWindowVisible(hwnd):
                    found.append(hwnd)
            return True

        user32.EnumWindows(cb, 0)
        if found:
            return found[0]
        time.sleep(0.2)
    return None


def client_origin(hwnd):
    p = wt.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(p))
    return p.x, p.y


def click(hwnd, lx, ly, scale):
    ox, oy = client_origin(hwnd)
    x, y = int(ox + lx * scale), int(oy + ly * scale)
    user32.SetCursorPos(x, y)
    time.sleep(0.12)
    user32.mouse_event(0x0002, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(0x0004, 0, 0, 0, 0)
    time.sleep(0.35)


def shot(hwnd, name):
    rc = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rc))
    img = ImageGrab.grab(bbox=(rc.left, rc.top, rc.right, rc.bottom), all_screens=True)
    os.makedirs(OUT, exist_ok=True)
    p = os.path.join(OUT, name + ".png")
    img.save(p)
    img.resize((img.width // 2, img.height // 2)).save(os.path.join(OUT, name + "-s.png"))
    print("SHOT", p, img.size)


def main():
    subprocess.Popen([EXE], cwd=os.path.dirname(EXE))
    hwnd = find_window("Pulse")
    if not hwnd:
        print("window not found")
        return
    time.sleep(2.0)
    # 可能是上次留下的最大化状态，先恢复成固定尺寸，否则坐标全不对
    user32.ShowWindow(hwnd, 9)
    user32.SetWindowPos(hwnd, 0, 120, 80, int(940 * 1.972), int(640 * 1.972), 0x0004)
    time.sleep(0.4)
    # 不抢到前台的话，点击会落到别的窗口上
    user32.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0040)
    user32.SetForegroundWindow(hwnd)
    user32.BringWindowToTop(hwnd)
    user32.SetWindowPos(hwnd, -2, 0, 0, 0, 0, 0x0001 | 0x0002)
    time.sleep(0.6)

    rc = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rc))
    # 缩放比必须按 DPI 算：940 是含边框的窗口宽度，客户区逻辑宽度只有 ~913
    scale = user32.GetDpiForWindow(hwnd) / 96.0
    print("client", rc.right, rc.bottom, "scale", round(scale, 3))

    shot(hwnd, "n1-idle")
    click(hwnd, 220, 355, scale)          # 第一行
    time.sleep(2.2)
    shot(hwnd, "n2-playing")
    click(hwnd, 880, 217, scale)          # 迷你按钮
    time.sleep(1.2)
    shot(hwnd, "n3-mini")
    click(hwnd, 325, 15, scale)           # 迷你窗里的还原
    time.sleep(1.0)
    shot(hwnd, "n4-back")


if __name__ == "__main__":
    main()
