"""Renders kithsong.svg to the committed PNGs and kithsong.ico.

Run by hand after editing the SVG; nothing in the build calls it. Needs Microsoft Edge (or
Chrome, via $BROWSER) for the SVG rendering and nothing beyond the standard library.

    python packaging/icons/render.py
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
PNG_SIZES = [128, 256, 512]


def find_browser():
    candidates = [
        os.environ.get("BROWSER"),
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
        shutil.which("microsoft-edge"),
        shutil.which("google-chrome"),
        shutil.which("chromium"),
    ]
    for c in candidates:
        if c and Path(c).exists():
            return c
    sys.exit("No Edge or Chrome found; set BROWSER to one.")


def render(browser, svg, size, out, tmp):
    page = tmp / f"icon{size}.html"
    page.write_text(
        "<!doctype html><html><body style='margin:0;background:transparent'>"
        f"<img src='{svg.as_uri()}' width='{size}' height='{size}' style='display:block'>"
        "</body></html>",
        encoding="utf-8")
    subprocess.run(
        [browser, "--headless=new", "--disable-gpu", "--no-first-run", "--hide-scrollbars",
         "--force-device-scale-factor=1", "--default-background-color=00000000",
         f"--user-data-dir={tmp / 'profile'}", f"--window-size={size},{size}",
         f"--screenshot={out}", page.as_uri()],
        check=True, capture_output=True, timeout=60)
    if not out.exists():
        sys.exit(f"Rendering {size}px failed.")


def png_size(data):
    return struct.unpack(">II", data[16:24])


def write_ico(pngs, out):
    # PNG-compressed entries, which Windows reads from Vista on.
    header = struct.pack("<HHH", 0, 1, len(pngs))
    offset = len(header) + 16 * len(pngs)
    entries, blobs = b"", b""
    for data in pngs:
        w, h = png_size(data)
        entries += struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    out.write_bytes(header + entries + blobs)


def main():
    browser = find_browser()
    svg = HERE / "kithsong.svg"
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        rendered = {}
        for size in sorted(set(ICO_SIZES + PNG_SIZES)):
            out = tmp / f"{size}.png"
            render(browser, svg, size, out, tmp)
            data = out.read_bytes()
            if png_size(data) != (size, size):
                sys.exit(f"{size}px rendered as {png_size(data)}; the browser clamped the window.")
            rendered[size] = data
        for size in PNG_SIZES:
            (HERE / f"kithsong-{size}.png").write_bytes(rendered[size])
        write_ico([rendered[s] for s in ICO_SIZES], HERE / "kithsong.ico")
    print("Wrote", ", ".join(f"kithsong-{s}.png" for s in PNG_SIZES), "and kithsong.ico")


if __name__ == "__main__":
    main()
