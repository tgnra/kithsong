"""Generate image variants for every connection tier, cached by content hash.

Tiers (see README):
  placeholder  tiny dithered PNG, inlined as a data URI
  dither       8-colour ordered-dithered lossless WebP (save-data / 2G)
  lite         low-quality WebP (3G)
  full         AVIF, then WebP, then a JPEG fallback (fast / unknown)
  original     source size (capped) for the lightbox
"""

import base64
import hashlib
import io
import json
import shutil
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageChops, ImageOps

# Bump when encoder settings change so cached variants are regenerated.
VERSION = 5

PLACEHOLDER_WIDTH = 24
PLACEHOLDER_COLOURS = 8
DITHER_WIDTHS = (320, 480)
DITHER_COLOURS = 8
# Ordered (Bayer) dithering repeats a fixed pattern, so it compresses ~4x
# better than Floyd-Steinberg's error diffusion. Strength is +/- per channel.
DITHER_STRENGTH = 32
LITE_WIDTHS = (400, 800)
LITE_QUALITY = 45
FULL_WIDTHS = (480, 800, 1280)
FULL_JPEG_WIDTH = 800
AVIF_QUALITY = 50
WEBP_QUALITY = 75
JPEG_QUALITY = 80
ORIGINAL_MAX = 2560


@dataclass
class ImageInfo:
    width: int
    height: int
    placeholder: str  # data URI
    dither: list  # [(url, width)]
    lite: list
    full_avif: list
    full_webp: list
    full_jpeg: list
    original: dict  # {"avif": url, "webp": url, "jpeg": url, "width": w, "height": h}

    @staticmethod
    def srcset(variants):
        return ", ".join(f"{url} {w}w" for url, w in variants)

    @property
    def fallback(self):
        """The universally supported image: used for noscript and feeds."""
        return self.full_jpeg[-1][0]


class ImageProcessor:
    def __init__(self, out_dir, cache_dir, url_prefix="/img/"):
        self.out_dir = Path(out_dir)
        self.cache_dir = Path(cache_dir)
        self.url_prefix = url_prefix
        self._memo = {}

    def process(self, path):
        path = Path(path).resolve()
        if path not in self._memo:
            self._memo[path] = self._process(path)
        return self._memo[path]

    def _process(self, path):
        data = path.read_bytes()
        key = hashlib.sha256(data + repr(_settings()).encode()).hexdigest()[:16]
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        manifest_path = self.cache_dir / f"{key}.json"
        if manifest_path.exists():
            manifest = json.loads(manifest_path.read_text())
        else:
            manifest = self._generate(data, key)
            manifest_path.write_text(json.dumps(manifest))
        self.out_dir.mkdir(parents=True, exist_ok=True)
        for name in manifest["files"]:
            dest = self.out_dir / name
            if not dest.exists():
                shutil.copyfile(self.cache_dir / name, dest)
        return self._info(manifest)

    def _info(self, m):
        def urls(entries):
            return [(self.url_prefix + name, w) for name, w in entries]

        o = m["original"]
        return ImageInfo(
            width=m["width"],
            height=m["height"],
            placeholder=m["placeholder"],
            dither=urls(m["dither"]),
            lite=urls(m["lite"]),
            full_avif=urls(m["full_avif"]),
            full_webp=urls(m["full_webp"]),
            full_jpeg=urls(m["full_jpeg"]),
            original={
                "avif": self.url_prefix + o["avif"],
                "webp": self.url_prefix + o["webp"],
                "jpeg": self.url_prefix + o["jpeg"],
                "width": o["width"],
                "height": o["height"],
            },
        )

    def _generate(self, data, key):
        img = _flatten(ImageOps.exif_transpose(Image.open(io.BytesIO(data))))
        w, h = img.size
        files = []

        def save(im, name, fmt, **opts):
            im.save(self.cache_dir / name, fmt, **opts)
            files.append(name)
            return name

        def series(widths, suffix, fn):
            out = []
            for tw in _widths(widths, w):
                out.append((fn(_resize(img, tw), f"{key}-{suffix}{tw}"), tw))
            return out

        dither = series(DITHER_WIDTHS, "d", lambda im, n: save(
            _ordered_dither(im, DITHER_COLOURS), n + ".webp", "WEBP", lossless=True, method=6))
        lite = series(LITE_WIDTHS, "l", lambda im, n: save(
            im, n + ".webp", "WEBP", quality=LITE_QUALITY, method=6))
        full_avif = series(FULL_WIDTHS, "f", lambda im, n: save(
            im, n + ".avif", "AVIF", quality=AVIF_QUALITY))
        full_webp = series(FULL_WIDTHS, "f", lambda im, n: save(
            im, n + ".webp", "WEBP", quality=WEBP_QUALITY, method=6))
        full_jpeg = series((FULL_JPEG_WIDTH,), "f", lambda im, n: save(
            im, n + ".jpg", "JPEG", quality=JPEG_QUALITY, optimize=True, progressive=True))

        ow = min(w, ORIGINAL_MAX)
        orig = _resize(img, ow)
        original = {
            "avif": save(orig, f"{key}-o.avif", "AVIF", quality=AVIF_QUALITY + 10),
            "webp": save(orig, f"{key}-o.webp", "WEBP", quality=WEBP_QUALITY + 10, method=6),
            "jpeg": save(orig, f"{key}-o.jpg", "JPEG", quality=JPEG_QUALITY + 5,
                         optimize=True, progressive=True),
            "width": orig.width,
            "height": orig.height,
        }

        return {
            "width": w,
            "height": h,
            "placeholder": _placeholder(img),
            "dither": dither,
            "lite": lite,
            "full_avif": full_avif,
            "full_webp": full_webp,
            "full_jpeg": full_jpeg,
            "original": original,
            "files": files,
        }


def _settings():
    return {k: v for k, v in globals().items() if k.isupper()}


def _flatten(img):
    """Composite transparency onto white; every tier is then plain RGB."""
    if img.mode.startswith("I"):
        # 16/32-bit integer images: convert("RGB") would clip to white.
        img = img.convert("I")
        top = img.getextrema()[1]
        img = img.point(lambda v: v * 255 / top if top > 255 else v).convert("L")
    if img.mode in ("RGBA", "LA") or (img.mode == "P" and "transparency" in img.info):
        img = img.convert("RGBA")
        bg = Image.new("RGB", img.size, (255, 255, 255))
        bg.paste(img, mask=img.getchannel("A"))
        return bg
    return img.convert("RGB")


def _widths(targets, source_width):
    """Target widths no larger than the source, always including at least one."""
    out = [t for t in targets if t < source_width]
    top = min(source_width, max(targets))
    if top not in out:
        out.append(top)
    return out


def _resize(img, width):
    if width >= img.width:
        return img
    height = max(1, round(img.height * width / img.width))
    return img.resize((width, height), Image.Resampling.LANCZOS)


def _dither(img, colours):
    # quantize() only dithers when mapping onto an existing palette, so pick
    # the palette first, then map onto it with Floyd-Steinberg.
    palette = img.quantize(colours, method=Image.Quantize.MEDIANCUT)
    return img.quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG)


BAYER8 = (
    0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26,
    12, 44, 4, 36, 14, 46, 6, 38, 60, 28, 52, 20, 62, 30, 54, 22,
    3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
    15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21,
)


def _ordered_dither(img, colours):
    palette = img.quantize(colours, method=Image.Quantize.MEDIANCUT)
    tile = Image.new("L", (8, 8))
    tile.putdata([v * 2 * DITHER_STRENGTH // 64 for v in BAYER8])
    pattern = Image.new("L", img.size)
    for y in range(0, img.height, 8):
        for x in range(0, img.width, 8):
            pattern.paste(tile, (x, y))
    # Shift every pixel by its threshold, then snap to the nearest palette colour.
    shifted = ImageChops.add(img, Image.merge("RGB", [pattern] * 3), offset=-DITHER_STRENGTH)
    return shifted.quantize(palette=palette, dither=Image.Dither.NONE)


def _placeholder(img):
    small = _dither(_resize(img, PLACEHOLDER_WIDTH), PLACEHOLDER_COLOURS)
    buf = io.BytesIO()
    small.save(buf, "PNG", optimize=True)
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()
