"""Pulse 构建脚本：生成图标 -> 编译资源 -> 编译链接 -> 输出 dist/
用法：python build.py
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BUILD = ROOT / "build"
DIST = ROOT / "dist"
SRC = ROOT / "src"

# 本机没有独立安装的编译器，直接复用 bilidownload 里那份 MinGW
TOOLCHAIN_CANDIDATES = [
    ROOT / "toolchain" / "mingw64" / "bin",
    Path("D:/WorkSpace/bilidownload/toolchain/mingw64/bin"),
]


def find_toolchain() -> Path:
    for c in TOOLCHAIN_CANDIDATES:
        if (c / "g++.exe").exists():
            return c
    raise SystemExit("找不到编译器。把 MinGW 解压到 toolchain/mingw64/，或改 TOOLCHAIN_CANDIDATES。")


def make_icon() -> None:
    """深色圆角方块 + 三根频谱条"""
    from PIL import Image, ImageDraw

    size = 256
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([8, 8, size - 8, size - 8], radius=56, fill=(20, 22, 28, 255))
    bars = [(84, 118, 190), (120, 74, 198), (156, 96, 168)]
    for i, (x, y0, y1) in enumerate(bars):
        col = (255, 94, 122, 255) if i == 0 else ((255, 138, 107, 255) if i == 1 else (255, 184, 107, 255))
        d.rounded_rectangle([x, y0, x + 22, y1], radius=11, fill=col)

    BUILD.mkdir(exist_ok=True)
    img.save(BUILD / "app.ico",
             sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])


def run(cmd: list[str], what: str) -> None:
    print(" ", what)
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                       errors="replace", cwd=ROOT)
    if p.returncode != 0:
        print(p.stdout)
        print(p.stderr)
        raise SystemExit(f"{what} 失败")
    if p.stderr.strip():
        print(p.stderr[:2000])


def build() -> None:
    tc = find_toolchain()
    gxx = tc / "g++.exe"
    windres = tc / "windres.exe"

    print("[1/3] 生成图标")
    make_icon()

    print("[2/3] 编译资源")
    run([str(windres), "-I", str(SRC), str(SRC / "resource.rc"), "-o", str(BUILD / "resource.o")],
        "windres")

    print("[3/3] 编译链接")
    cmd = [
        str(gxx), "-std=c++17", "-O2", "-municode", "-mwindows",
        "-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN",
        "-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00",
        "-I", str(SRC),
        str(SRC / "main.cpp"), str(BUILD / "resource.o"),
        "-o", str(BUILD / "Pulse.exe"),
        "-ld2d1", "-ldwrite", "-lwindowscodecs", "-ldwmapi",
        "-lmfplat", "-lmfreadwrite", "-lmfuuid",
        "-lole32", "-loleaut32", "-luuid", "-lshlwapi", "-lshell32",
        "-luser32", "-lgdi32", "-ladvapi32",
        "-static", "-static-libgcc", "-static-libstdc++", "-s",
    ]
    run(cmd, "g++")

    DIST.mkdir(exist_ok=True)
    exe = DIST / "Pulse.exe"
    if exe.exists():
        # 旧版正开着时不能直接覆盖（WinError 32）；Windows 允许把运行中的 exe
        # 改名挪开，旧进程继续跑不受影响。不要 unlink —— 某些环境会把删除代理成
        # 「移到回收站」，对运行中的文件必然失败。挪到 build/ 下，不会进仓库。
        exe.replace(BUILD / "Pulse.prev.exe")
    shutil.copy2(BUILD / "Pulse.exe", exe)
    print(f"\n完成：{exe}  ({exe.stat().st_size / 1048576:.2f} MB)")


if __name__ == "__main__":
    build()
