"""Build the site: content/home.md + docs/ + static/ + assets/ + site.toml -> dist/."""

import shutil
import time
import tomllib
from html import escape
from pathlib import Path

from . import content
from .images import ImageProcessor
from .markdown_ext import BuildError, SiteExtension, make_markdown
from .templates import Templates

DEFAULTS = {
    "title": "Kithsong",
    "description": "",
    "base_url": "http://localhost:8000",
    "language": "en",
    "github": "",
    "logo": "",
    "downloads": [],
}

# <img sizes> for each layout; they match the breakpoints in assets/style.css.
FEATURE_SIZES = "(min-width: 56rem) 30rem, (min-width: 40rem) 36rem, 100vw"
DOC_SIZES = "(min-width: 56rem) 44rem, 100vw"


def load_config(root, base_url=None):
    site = dict(DEFAULTS)
    path = Path(root) / "site.toml"
    if path.exists():
        site.update(tomllib.loads(path.read_text(encoding="utf-8")))
    if base_url:
        site["base_url"] = base_url
    site["base_url"] = site["base_url"].rstrip("/")
    for d in site["downloads"]:
        missing = {"platform", "name", "store", "href"} - d.keys()
        if missing:
            raise ValueError(f"site.toml: a [[downloads]] entry is missing {', '.join(sorted(missing))}")
    return site


def build(root=".", drafts=False, base_url=None, log=print):
    started = time.perf_counter()
    root = Path(root).resolve()
    site = load_config(root, base_url)
    dist = root / "dist"
    warnings = []

    def warn(msg):
        warnings.append(msg)
        log(f"warning: {msg}")

    home_path = root / "content" / "home.md"
    home, sections = content.load_home(home_path)
    docs = content.load_docs(root / "docs", drafts=drafts)
    if not docs:
        raise BuildError(f"{root / 'docs'} has no pages")

    # Empty dist/ rather than deleting it: on Windows a server or Explorer
    # window holding the folder open would make the delete fail.
    dist.mkdir(exist_ok=True)
    for child in dist.iterdir():
        shutil.rmtree(child) if child.is_dir() else child.unlink()
    images = ImageProcessor(dist / "img", root / ".cache" / "images")
    t = Templates(site, docs)

    home_ext = SiteExtension(images, root, FEATURE_SIZES, warn=warn)
    home_md = make_markdown(home_ext)
    features = []
    for s in sections:
        home_ext.begin(home_path)
        text = home_md.reset().convert(s.text)
        home_ext.begin(home_path)
        features.append((text, home_md.reset().convert(s.image)))
    write(dist, "/", t.home(home, features))

    doc_ext = SiteExtension(images, root, DOC_SIZES, warn=warn)
    doc_md = make_markdown(doc_ext)
    for doc in docs:
        doc_ext.begin(doc.source)
        doc.html = doc_md.reset().convert(doc.body)
        doc.description = doc.description or doc_ext.first_paragraph
    for i, doc in enumerate(docs):
        prev = docs[i - 1] if i else None
        nxt = docs[i + 1] if i + 1 < len(docs) else None
        write(dist, doc.url, t.doc(doc, prev, nxt))

    (dist / "404.html").write_text(t.not_found(), encoding="utf-8")
    (dist / "sitemap.xml").write_text(sitemap(site, ["/"] + [d.url for d in docs]),
                                      encoding="utf-8")
    (dist / "robots.txt").write_text(
        f"User-agent: *\nAllow: /\nSitemap: {site['base_url']}/sitemap.xml\n")
    static = root / "static"
    if static.is_dir():
        shutil.copytree(static, dist, dirs_exist_ok=True)

    n_images = len(list((dist / "img").glob("*"))) if (dist / "img").exists() else 0
    log(f"built the home page, {len(docs)} docs pages, {n_images} image files "
        f"in {time.perf_counter() - started:.2f}s -> {dist}")
    return warnings


def sitemap(site, urls):
    items = "".join(f"<url><loc>{escape(site['base_url'] + u)}</loc></url>" for u in urls)
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">{items}</urlset>\n')


def write(dist, url, html):
    path = dist / url.strip("/") / "index.html" if url.endswith("/") else dist / url.lstrip("/")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(html, encoding="utf-8")
