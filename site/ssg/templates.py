"""HTML templates as plain functions. All interpolated text goes through esc()."""

import json
import re
from html import escape
from pathlib import Path

ASSETS = Path(__file__).resolve().parent.parent / "assets"


def esc(s):
    return escape(str(s), quote=True)


def _minify_css(css):
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    css = re.sub(r"\s+", " ", css)
    return re.sub(r"\s*([{};,>])\s*", r"\1", css).strip()


def _minify_js(js):
    # Only strips whole-line comments and indentation; safe for our own scripts.
    lines = (line.strip() for line in js.splitlines())
    return "\n".join(line for line in lines if line and not line.startswith("//"))


class Templates:
    def __init__(self, site, docs, assets_dir=ASSETS):
        self.site = site
        self.docs = docs

        def asset(name):
            text = (assets_dir / name).read_text(encoding="utf-8")
            return _minify_css(text) if name.endswith(".css") else _minify_js(text)

        self.css = asset("style.css")
        # head.js orders the docs transitions by the sidebar's order.
        self.head_js = f"var DOCS={json.dumps([d.url for d in docs])};\n" + asset("head.js")
        self.img_js = asset("img.js")
        self.theme_js = asset("theme.js")
        self.download_js = asset("download.js")

    # -- layout ---------------------------------------------------------------

    def layout(self, *, title, body, path, description="", has_images=False, noindex=False,
               scripts=""):
        site = self.site
        canonical = site["base_url"] + path
        description = description or site["description"]
        robots = '<meta name="robots" content="noindex">' if noindex else ""
        if has_images:
            scripts = f"<script>{self.img_js}</script>{scripts}"
        return _tidy(f"""<!doctype html>
<html lang="{esc(site['language'])}" class="no-js">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{esc(title)}</title>
<meta name="description" content="{esc(description)}">
<meta name="color-scheme" content="light dark">
<link rel="canonical" href="{esc(canonical)}">
<meta property="og:type" content="website">
<meta property="og:site_name" content="{esc(site['title'])}">
<meta property="og:title" content="{esc(title)}">
<meta property="og:description" content="{esc(description)}">
<meta property="og:url" content="{esc(canonical)}">
<link rel="icon" href="/favicon.svg" type="image/svg+xml">
{robots}
<script>{self.head_js}</script>
<style>{self.css}</style>
</head>
<body>
<a class="skip" href="#main">Skip to content</a>
{body}
<script>{self.theme_js}</script>
{scripts}
</body>
</html>
""")

    def top(self):
        """The home page's links, top right."""
        items = ['<li><a href="/docs/">Docs</a></li>']
        if self.site["github"]:
            items.append(f'<li><a href="{esc(self.site["github"])}">GitHub</a></li>')
        items.append(f"<li hidden>{THEME_BUTTON}</li>")
        return f'<header class="top"><nav aria-label="Site"><ul>{"".join(items)}</ul></nav></header>'

    def footer(self):
        links = ['<a href="/docs/">Docs</a>']
        if self.site["github"]:
            links.append(f'<a href="{esc(self.site["github"])}">GitHub</a>')
        return (f'<footer class="foot"><p>{esc(self.site["title"])} · {" · ".join(links)}</p>'
                f'</footer>')

    # -- home -----------------------------------------------------------------

    def home(self, meta, features):
        site = self.site
        if site["logo"]:
            logo = f'<img class="logo-img" src="{esc(site["logo"])}" alt="{esc(site["title"])}">'
        else:
            logo = esc(site["title"])
        tagline = meta.get("tagline", "")
        tagline = f'<p class="tagline">{esc(tagline)}</p>' if tagline else ""
        sections = []
        for text, media in features:
            m = re.search(r'<h2 id="([^"]+)"', text)
            label = f' aria-labelledby="{m.group(1)}"' if m else ""
            img = re.search(r"<img [^>]*>", media)
            w = img and re.search(r'\swidth="(\d+)"', img.group(0))
            h = img and re.search(r'\sheight="(\d+)"', img.group(0))
            ratio = f' style="--ar: {w.group(1)} / {h.group(1)}"' if w and h else ""
            sections.append(f"""<section class="feature"{label}>
<div class="feature-text">{text}</div>
<div class="feature-media"{ratio}>{media}</div>
</section>""")
        body = f"""{self.top()}
<main id="main" tabindex="-1">
<section class="hero" aria-labelledby="site-name">
<h1 id="site-name" class="logo">{logo}</h1>
{tagline}
{self.downloads()}
</section>
<div class="features">
{"".join(sections)}
</div>
</main>
{self.footer()}"""
        title = meta.get("title") or site["title"]
        return self.layout(title=title, body=body, path="/",
                           description=meta.get("description", ""),
                           has_images=_has_images(body),
                           scripts=f"<script>{self.download_js}</script>")

    def downloads(self):
        """One link per platform. download.js puts the visitor's own platform first."""
        items = self.site["downloads"]
        if not items:
            return ""
        links = "".join(
            f'<li><a class="dl" href="{esc(d["href"])}" data-platform="{esc(d["platform"])}">'
            f'<span class="dl-name">{esc(d["name"])}</span><span class="vh">, </span>'
            f'<span class="dl-store">{esc(d["store"])}</span></a></li>' for d in items)
        other = ""
        if self.site["github"]:
            releases = self.site["github"].rstrip("/") + "/releases"
            other = f'\n<p class="other-downloads"><a href="{esc(releases)}">Other downloads</a></p>'
        return f"""<section class="get" aria-labelledby="get">
<h2 id="get" class="vh">Download</h2>
<ul class="downloads" role="list">{links}</ul>{other}
</section>"""

    # -- docs -----------------------------------------------------------------

    def doc(self, doc, prev, nxt):
        body = f"""<div class="docs">
{self.sidebar(doc.url)}
<main id="main" class="doc" tabindex="-1">
<article>
<h1>{esc(doc.title)}</h1>
{doc.html}
</article>
{self.pager(prev, nxt)}
</main>
</div>"""
        return self.layout(title=f"{doc.title} – {self.site['title']} docs", body=body,
                           path=doc.url, description=doc.description,
                           has_images=_has_images(body))

    def sidebar(self, path):
        items = []
        for d in self.docs:
            current = ' aria-current="page"' if d.url == path else ""
            items.append(f'<li><a href="{esc(d.url)}"{current}>{esc(d.title)}</a></li>')
        items = "".join(items)
        return f"""<nav class="sidebar" aria-label="Documentation">
<a class="home-link" href="/"><span aria-hidden="true">←</span> Home</a>
<ul role="list">{items}</ul>
<div class="sidebar-foot" hidden>{THEME_BUTTON}</div>
</nav>"""

    def pager(self, prev, nxt):
        items = []
        if prev:
            items.append(f'<li class="prev"><a href="{esc(prev.url)}" rel="prev">'
                         f'<span class="pager-dir">Previous<span class="vh">:</span></span> '
                         f'{esc(prev.title)}</a></li>')
        if nxt:
            items.append(f'<li class="next"><a href="{esc(nxt.url)}" rel="next">'
                         f'<span class="pager-dir">Next<span class="vh">:</span></span> '
                         f'{esc(nxt.title)}</a></li>')
        if not items:
            return ""
        return f'<nav class="pager" aria-label="More docs"><ul role="list">{"".join(items)}</ul></nav>'

    # -- other ----------------------------------------------------------------

    def not_found(self):
        body = f"""{self.top()}
<main id="main" class="lost" tabindex="-1">
<h1>Page not found</h1>
<p>There’s nothing at this address. Try the <a href="/">home page</a> or the
<a href="/docs/">docs</a>.</p>
</main>
{self.footer()}"""
        return self.layout(title=f"Page not found – {self.site['title']}", body=body,
                           path="/404.html", noindex=True)


# Half-filled circle: reads as "contrast" whichever theme is active. Revealed by
# theme.js; without JS the system setting applies.
THEME_BUTTON = (
    '<button type="button" class="theme-toggle" aria-pressed="false" title="Dark theme">'
    '<svg aria-hidden="true" focusable="false" width="20" height="20" viewBox="0 0 20 20">'
    '<circle cx="10" cy="10" r="8" fill="none" stroke="currentColor" stroke-width="2"/>'
    '<path d="M10 2a8 8 0 0 1 0 16z" fill="currentColor"/></svg>'
    '<span class="vh">Dark theme</span></button>')


def _has_images(html):
    return "data-full=" in html


def _tidy(html):
    """Drop blank lines and indentation outside <pre>, which is whitespace-significant."""
    parts = re.split(r"(<pre[\s>].*?</pre>)", html, flags=re.S)
    for i in range(0, len(parts), 2):
        parts[i] = re.sub(r"\n\s*", "\n", parts[i])
    return "".join(parts)
