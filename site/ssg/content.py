"""Load the home page and the docs from Markdown files with simple front matter."""

import re
from dataclasses import dataclass, field
from pathlib import Path

# A line that is nothing but one Markdown image: a home page section's picture.
IMAGE_LINE = re.compile(r"^!\[[^\]]*\]\([^)]*\)[ \t]*$", re.M)


@dataclass
class Doc:
    source: Path
    slug: str
    title: str
    body: str
    order: int | None = None
    description: str = ""
    draft: bool = False
    meta: dict = field(default_factory=dict)
    html: str = ""

    @property
    def url(self):
        return "/docs/" if self.slug == "index" else f"/docs/{self.slug}/"


@dataclass
class Section:
    """One home page section: Markdown text (starting with its ## heading) and its image."""
    text: str
    image: str


def parse_front_matter(text):
    """Split `---` delimited `key: value` front matter from the body.

    Values are unquoted; true/false become bools and plain integers become ints.
    """
    text = text.lstrip("﻿")
    if not text.startswith("---"):
        return {}, text
    lines = text.splitlines(keepends=True)
    meta = {}
    for i, line in enumerate(lines[1:], start=1):
        if line.strip() == "---":
            return meta, "".join(lines[i + 1:])
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        key, sep, value = line.partition(":")
        if not sep:
            raise ValueError(f"bad front matter line: {line.strip()!r}")
        meta[key.strip().lower()] = _coerce(value.strip())
    raise ValueError("front matter is not closed with ---")


def _coerce(value):
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    if value.lower() in ("true", "false"):
        return value.lower() == "true"
    if re.fullmatch(r"-?\d+", value):
        return int(value)
    return value


def slugify(text):
    text = re.sub(r"[^\w\s-]", "", text.lower()).strip()
    return re.sub(r"[\s_-]+", "-", text) or "untitled"


def load_home(path):
    """Return (front matter, sections). Each `## ` heading starts a section, and each
    section needs one line holding just an image, which is shown beside its text."""
    path = Path(path)
    meta, body = parse_front_matter(path.read_text(encoding="utf-8"))
    parts = re.split(r"^(?=## )", body, flags=re.M)
    if parts[0].strip():
        raise ValueError(f"{path}: text before the first ## heading isn't shown anywhere")
    sections = []
    for part in parts[1:]:
        heading = part.splitlines()[0][3:].strip()
        images = list(IMAGE_LINE.finditer(part))
        if len(images) != 1:
            raise ValueError(f"{path}: section {heading!r} needs exactly one line with just an "
                             f"image on it, found {len(images)}")
        m = images[0]
        sections.append(Section(text=(part[:m.start()] + part[m.end():]).strip(),
                                image=m.group(0).strip()))
    return meta, sections


def load_doc(path):
    meta, body = parse_front_matter(Path(path).read_text(encoding="utf-8"))
    stem = Path(path).stem
    order = meta.get("order")
    return Doc(
        source=Path(path),
        slug=meta.get("slug") or ("index" if stem == "index" else slugify(stem)),
        title=str(meta.get("title") or stem.replace("-", " ").title()),
        body=body,
        order=order if isinstance(order, int) else None,
        description=str(meta.get("description", "")),
        draft=bool(meta.get("draft", False)),
        meta=meta,
    )


def load_docs(docs_dir, drafts=False):
    """Docs in sidebar order: index.md first, then by `order`, then by title."""
    docs = [load_doc(p) for p in sorted(Path(docs_dir).glob("*.md"))]
    if not drafts:
        docs = [d for d in docs if not d.draft]
    docs.sort(key=lambda d: (d.slug != "index", d.order is None, d.order or 0, d.title.lower()))
    seen = {}
    for doc in docs:
        if doc.url in seen:
            raise ValueError(f"{doc.source} and {seen[doc.url]} both map to {doc.url}")
        seen[doc.url] = doc.source
    if docs and docs[0].slug != "index":
        raise ValueError(f"{docs_dir} needs an index.md: it's the page at /docs/")
    return docs
