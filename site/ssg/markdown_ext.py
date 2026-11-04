"""Python-Markdown extension: responsive images, heading levels/ids, a11y tweaks."""

import re
import xml.etree.ElementTree as etree
from pathlib import Path

from markdown import Markdown
from markdown.extensions import Extension
from markdown.postprocessors import Postprocessor
from markdown.treeprocessors import Treeprocessor

from .content import slugify

# Markdown alt text that means "intentionally decorative": ![""](x.jpg)
DECORATIVE_ALT = '""'


class BuildError(Exception):
    pass


def figure(media, caption=""):
    """Element for a standalone image: <figure> with optional caption."""
    fig = etree.Element("figure", {"class": "img"})
    fig.append(media)
    if caption:
        etree.SubElement(fig, "figcaption").text = caption
    return fig


def image(info, alt, sizes, eager=False):
    """Element for one processed image: a JS-upgraded <picture> (placeholder src, data-*
    srcsets per tier) plus a <noscript> fallback, wrapped in a lightbox link unless the
    image is decorative."""
    size = {"width": str(info.width), "height": str(info.height)}
    js = etree.Element("picture", {"class": "img-js"})
    etree.SubElement(js, "source", {"type": "image/avif", "data-srcset": info.srcset(info.full_avif)})
    etree.SubElement(js, "source", {"type": "image/webp", "data-srcset": info.srcset(info.full_webp)})
    attrs = {
        "src": info.placeholder,
        "alt": alt,
        **size,
        "sizes": sizes,
        "decoding": "async",
        "data-full": info.srcset(info.full_jpeg),
        "data-lite": info.srcset(info.lite),
        "data-dither": info.srcset(info.dither),
    }
    if eager:
        attrs["data-eager"] = ""
    etree.SubElement(js, "img", attrs)

    noscript = etree.Element("noscript")
    pic = etree.SubElement(noscript, "picture")
    etree.SubElement(pic, "source", {"type": "image/avif", "srcset": info.srcset(info.full_avif)})
    etree.SubElement(pic, "source", {"type": "image/webp", "srcset": info.srcset(info.full_webp)})
    loading = {"fetchpriority": "high"} if eager else {"loading": "lazy"}
    etree.SubElement(pic, "img", {
        "src": info.fallback,
        "srcset": info.srcset(info.full_jpeg),
        "sizes": sizes,
        "alt": alt,
        **size,
        "decoding": "async",
        **loading,
    })

    if not alt:
        wrap = etree.Element("span", {"class": "img-wrap"})
    else:
        o = info.original
        wrap = etree.Element("a", {
            "class": "zoom",
            "href": o["jpeg"],
            "data-avif": o["avif"],
            "data-webp": o["webp"],
            "data-width": str(o["width"]),
            "data-height": str(o["height"]),
        })
    wrap.append(js)
    wrap.append(noscript)
    if alt:
        etree.SubElement(wrap, "span", {"class": "vh"}).text = " (open full size)"
    return wrap


def themed_image(light, dark, alt, sizes, eager=False):
    """Element for a light and dark pair: both images, with style.css hiding the one that
    doesn't match the theme. img.js only loads an image once it's displayed, so a visitor
    who never changes theme downloads one of the two."""
    wrap = etree.Element("span", {"class": "img-themed"})
    for theme, info in (("light", light), ("dark", dark)):
        etree.SubElement(wrap, "span", {"class": f"theme-{theme}"}).append(
            image(info, alt, sizes, eager))
    return wrap


class SiteExtension(Extension):
    """Configured per build; `begin()` must be called before each document.

    `sizes` is the <img sizes> for the layout the images appear in.
    """

    def __init__(self, images, root, sizes, warn=print):
        super().__init__()
        self.images = images
        self.root = Path(root)
        self.sizes = sizes
        self.warn = warn
        self.begin(None)

    def begin(self, source, eager_first=False):
        self.source = source
        self.eager_first = eager_first
        self.first_paragraph = ""

    def resolve(self, src, source=None):
        """Image paths: /x is relative to site/, otherwise to the Markdown file."""
        source = source or self.source
        if src.startswith("/"):
            path = self.root / src.lstrip("/")
        else:
            path = (source.parent if source else self.root) / src
        if not path.is_file():
            raise BuildError(f"{source}: image not found: {src}")
        return path

    def extendMarkdown(self, md):
        # After the inline processor (priority 20) has created <img> elements.
        md.treeprocessors.register(SiteTreeprocessor(md, self), "site", 15)
        md.postprocessors.register(PrePostprocessor(md), "site-pre", 5)


class PrePostprocessor(Postprocessor):
    """Fenced code is stashed as raw HTML, so <pre> is only reachable here.
    Scrollable regions must be keyboard reachable."""

    def run(self, text):
        return re.sub(r"<pre(?=[\s>])(?![^>]*tabindex)", '<pre tabindex="0"', text)


class SiteTreeprocessor(Treeprocessor):
    def __init__(self, md, ext):
        super().__init__(md)
        self.ext = ext

    def run(self, root):
        parents = {child: parent for parent in root.iter() for child in parent}
        self.headings(root)
        self.images(root, parents)
        self.blocks(root, parents)
        self.links(root)
        for el in root:
            if el.tag == "p":
                self.ext.first_paragraph = "".join(el.itertext()).strip()
                break

    def headings(self, root):
        hs = [el for el in root.iter() if el.tag in ("h1", "h2", "h3", "h4", "h5", "h6")]
        if not hs:
            return
        # The page title is the only <h1>; content headings start at <h2>.
        shift = 2 - min(int(h.tag[1]) for h in hs)
        seen, prev = set(), 1
        for h in hs:
            level = min(6, int(h.tag[1]) + shift)
            h.tag = f"h{level}"
            if level > prev + 1:
                self.ext.warn(f"{self.ext.source}: heading level skips from h{prev} to h{level}")
            prev = level
            if "id" not in h.attrib:
                base = slugify("".join(h.itertext())) or "section"
                hid, n = base, 2
                while hid in seen:
                    hid, n = f"{base}-{n}", n + 1
                h.set("id", hid)
            seen.add(h.get("id"))

    def images(self, root, parents):
        ext = self.ext
        for img in [el for el in root.iter("img")]:
            src, alt = img.get("src", ""), img.get("alt", "")
            if alt == DECORATIVE_ALT:
                alt = ""
            elif not alt.strip():
                raise BuildError(
                    f'{ext.source}: image {src} has no alt text (use ![""]({src}) if decorative)')
            img.set("alt", alt)
            if "://" in src or src.startswith("data:"):
                img.set("loading", "lazy")
                img.set("decoding", "async")
                continue

            eager, ext.eager_first = ext.eager_first, False
            media = self.media(ext.resolve(src), alt, eager)
            parent = parents[img]
            standalone = (parent.tag == "p" and len(parent) == 1
                          and not (parent.text or "").strip() and not (img.tail or "").strip())
            if standalone:
                new = figure(media, img.get("title", ""))
                new.tail = parent.tail
                _replace(parents[parent], parent, new)
            else:
                media.tail = img.tail
                _replace(parent, img, media)

    def media(self, path, alt, eager):
        """x-light.png and x-dark.png are one image that follows the theme, whichever of
        the two the Markdown names."""
        ext = self.ext
        m = re.fullmatch(r"(.+)-(light|dark)", path.stem)
        partner = m and path.with_stem(f"{m[1]}-{'dark' if m[2] == 'light' else 'light'}")
        if not m or not partner.is_file():
            if m:
                ext.warn(f"{ext.source}: {path.name} has no {partner.name}, so it shows in both themes")
            return image(ext.images.process(path), alt, ext.sizes, eager)
        light, dark = (path, partner) if m[2] == "light" else (partner, path)
        return themed_image(ext.images.process(light), ext.images.process(dark), alt, ext.sizes, eager)

    def blocks(self, root, parents):
        for pre in root.iter("pre"):
            pre.set("tabindex", "0")
        for table in [el for el in root.iter("table")]:
            wrap = etree.Element("div", {"class": "table-wrap", "tabindex": "0",
                                         "role": "region", "aria-label": "Table"})
            _replace(parents[table], table, wrap)
            wrap.append(table)

    def links(self, root):
        for a in root.iter("a"):
            text = "".join(a.itertext()).strip()
            if not text and not any(img.get("alt") for img in a.iter("img")):
                self.ext.warn(f"{self.ext.source}: link to {a.get('href')} has no text")


def _replace(parent, old, new):
    i = list(parent).index(old)
    parent.remove(old)
    parent.insert(i, new)


def make_markdown(ext):
    return Markdown(extensions=["fenced_code", "tables", "footnotes", ext], output_format="html")
