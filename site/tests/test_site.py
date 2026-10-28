import re
import shutil
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from ssg.build import build
from ssg.markdown_ext import BuildError

SITE_TOML = """title = "Test"
base_url = "https://example.org"
github = "https://github.com/example/test"

[[downloads]]
platform = "windows"
name = "Windows"
store = "Microsoft Store"
href = "#"

[[downloads]]
platform = "android"
name = "Android"
store = "Google Play"
href = "#"
"""

HOME = """---
tagline: A test tagline.
---

## First section

![A wide picture](images/wide.png)

Some text.

## Second section

Text before the picture.

![A tall picture](images/tall.png)
"""


class SiteTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        (self.root / "content" / "images").mkdir(parents=True)
        (self.root / "docs").mkdir()
        Image.new("RGB", (300, 200), (200, 100, 50)).save(self.root / "content/images/wide.png")
        Image.new("RGB", (100, 300), (50, 100, 200)).save(self.root / "content/images/tall.png")
        self.write("site.toml", SITE_TOML)
        self.write("content/home.md", HOME)
        self.write("docs/index.md", "---\ntitle: Intro\n---\nWelcome.\n")
        self.write("docs/zebra.md", "---\ntitle: Zebra\norder: 1\n---\nFirst after the intro.\n")
        self.write("docs/apple.md", "---\ntitle: Apple\n---\nNo order, so after the ordered ones.\n")
        self.write("docs/hidden.md", "---\ntitle: Hidden\ndraft: true\n---\nDraft.\n")

    def write(self, rel, text):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def build(self):
        build(self.root, log=lambda msg: None)
        return self.root / "dist"

    def read(self, rel):
        return (self.root / "dist" / rel).read_text(encoding="utf-8")

    def test_home(self):
        self.build()
        html = self.read("index.html")
        self.assertIn('<h1 id="site-name" class="logo">Test</h1>', html)
        self.assertIn('<p class="tagline">A test tagline.</p>', html)
        self.assertIn('<a href="/docs/">Docs</a>', html)
        self.assertIn('<a href="https://github.com/example/test">GitHub</a>', html)
        # Downloads: one link per platform, in site.toml's order.
        platforms = re.findall(r'data-platform="(\w+)"', html)
        self.assertEqual(platforms, ["windows", "android"])
        self.assertIn("Available for Windows and Android.", html)
        # Sections in order, each labelled by its heading, with its picture beside the text.
        sections = re.findall(r'<section class="feature" aria-labelledby="([^"]+)">', html)
        self.assertEqual(sections, ["first-section", "second-section"])
        self.assertIn('style="--ar: 300 / 200"', html)
        self.assertIn('style="--ar: 100 / 300"', html)
        second = html[html.index('aria-labelledby="second-section"'):]
        text = second[second.index('class="feature-text"'):second.index('class="feature-media"')]
        self.assertIn("Text before the picture.", text)
        self.assertNotIn("<img", text)
        # Page scripts: images (lightbox) and the platform pick.
        self.assertIn("a.zoom", html)
        self.assertIn('querySelector(".downloads")', html)

    def test_docs_sidebar_order_and_current_page(self):
        dist = self.build()
        self.assertFalse((dist / "docs/hidden").exists())
        html = self.read("docs/zebra/index.html")
        sidebar = html[html.index('<nav class="sidebar"'):html.index("</nav>")]
        self.assertLess(sidebar.index('href="/"'), sidebar.index('href="/docs/"'))
        self.assertEqual(re.findall(r'<li><a href="([^"]+)"', sidebar),
                         ["/docs/", "/docs/zebra/", "/docs/apple/"])
        self.assertIn('<a href="/docs/zebra/" aria-current="page">Zebra</a>', sidebar)
        self.assertEqual(sidebar.count("aria-current"), 1)
        # The transitions' order comes from the sidebar.
        self.assertIn('var DOCS=["/docs/", "/docs/zebra/", "/docs/apple/"];', html)
        self.assertIn('rel="prev"', html)
        self.assertIn('rel="next"', html)
        self.assertNotIn('class="top"', html)

    def test_sitemap_lists_home_and_docs(self):
        self.build()
        sitemap = self.read("sitemap.xml")
        for url in ("/", "/docs/", "/docs/zebra/", "/docs/apple/"):
            self.assertIn(f"<loc>https://example.org{url}</loc>", sitemap)

    def test_section_without_a_picture_fails(self):
        self.write("content/home.md", "## Lonely\n\nNo picture here.\n")
        with self.assertRaisesRegex(ValueError, "Lonely"):
            self.build()

    def test_text_before_the_first_section_fails(self):
        self.write("content/home.md", "Lost words.\n\n" + HOME.split("---\n", 2)[2])
        with self.assertRaisesRegex(ValueError, "before the first"):
            self.build()

    def test_picture_without_alt_text_fails(self):
        self.write("content/home.md", "## Section\n\n![](images/wide.png)\n")
        with self.assertRaisesRegex(BuildError, "alt text"):
            self.build()

    def test_docs_need_an_index(self):
        (self.root / "docs/index.md").unlink()
        with self.assertRaisesRegex(ValueError, "index.md"):
            self.build()

    def test_download_needs_every_field(self):
        self.write("site.toml", SITE_TOML + '\n[[downloads]]\nplatform = "linux"\n')
        with self.assertRaisesRegex(ValueError, "missing"):
            self.build()


if __name__ == "__main__":
    unittest.main()
