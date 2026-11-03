"""Renders kithsong.svg to the committed PNGs and kithsong.ico, and the wordmark (light and
dark) to the PNGs the Windows app embeds.

Run by hand after generate.sh; nothing in the build calls it. Renders with resvg if it's on
PATH (e.g. nix shell nixpkgs#resvg), else Microsoft Edge (or Chrome, via $BROWSER); needs
nothing beyond the standard library.

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
# The wordmark's PNGs: three times its 352 x 100 viewBox, enough for the Windows first run's
# logo up to 300% scaling.
WORDMARK_SIZE = (1056, 300)


def find_browser():
    candidates = [
        shutil.which("resvg"),
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
    """size is a width or (width, height)."""
    w, h = size if isinstance(size, tuple) else (size, size)
    if Path(browser).name.startswith("resvg"):
        subprocess.run([browser, "-w", str(w), "-h", str(h), str(svg), str(out)],
                       check=True, capture_output=True, timeout=60)
        return
    page = tmp / f"{svg.stem}{w}x{h}.html"
    page.write_text(
        "<!doctype html><html><body style='margin:0;background:transparent'>"
        f"<img src='{svg.as_uri()}' width='{w}' height='{h}' style='display:block'>"
        "</body></html>",
        encoding="utf-8")
    subprocess.run(
        [browser, "--headless=new", "--disable-gpu", "--no-first-run", "--hide-scrollbars",
         "--force-device-scale-factor=1", "--default-background-color=00000000",
         f"--user-data-dir={tmp / 'profile'}", f"--window-size={w},{h}",
         f"--screenshot={out}", page.as_uri()],
        check=True, capture_output=True, timeout=60)
    if not out.exists():
        sys.exit(f"Rendering {w}x{h} failed.")
    if png_size(out.read_bytes()) != (w, h):
        sys.exit(f"{w}x{h} rendered as {png_size(out.read_bytes())}; the browser clamped the window.")


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
            rendered[size] = out.read_bytes()
        for size in PNG_SIZES:
            (HERE / f"kithsong-{size}.png").write_bytes(rendered[size])
        write_ico([rendered[s] for s in ICO_SIZES], HERE / "kithsong.ico")
        for name in ("kithsong-wordmark", "kithsong-wordmark-dark"):
            render(browser, HERE / f"{name}.svg", WORDMARK_SIZE, HERE / f"{name}.png", tmp)
    print("Wrote", ", ".join(f"kithsong-{s}.png" for s in PNG_SIZES),
          "kithsong.ico, kithsong-wordmark.png and kithsong-wordmark-dark.png")


if __name__ == "__main__":
    main()
