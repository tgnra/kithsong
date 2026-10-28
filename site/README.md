# Kithsong website

The site at https://kithso.ng: a home page and the user docs, built by a small static site
generator in `ssg/`. It's adapted from the blog's generator. Two dependencies: Pillow and
Markdown.

```sh
cd site
python -m venv .venv
.venv/Scripts/python -m pip install -r requirements.txt   # .venv/bin/python on macOS/Linux
.venv/Scripts/python -m ssg serve        # http://localhost:8000, rebuilds on change
.venv/Scripts/python -m ssg build        # -> dist/
.venv/Scripts/python -m unittest
```

## Layout

| Path | What it is |
|---|---|
| `site.toml` | Title, description, GitHub link, logo, and the download links |
| `content/home.md` | The home page's tagline and sections |
| `docs/*.md` | One docs page each, at `/docs/<name>/`. `index.md` is `/docs/` |
| `static/` | Copied to the site root as-is (favicon, …) |
| `assets/` | CSS and JS, inlined into every page |

## Home page

`content/home.md` front matter sets the page `title`, `description` and the `tagline` under
the logo. Each `## ` heading starts a section. Every section needs one line holding only an
image, which is shown beside the text. The sides alternate, and on narrow screens the image
comes first.

```
## Sync without the cloud

![Two computers paired for sync, on Linux](images/sync.png)

Your devices find each other on your local network. …
```

The logo is the site title as text. Set `logo` in `site.toml` to an image (e.g. put
`logo.svg` in `static/` and set `logo = "/logo.svg"`) to use that instead.

### Download links

`[[downloads]]` entries in `site.toml` become the download links, in order. Each has a
`platform` (`windows`, `linux` or `android`), a `name`, a `store` and an `href`. The hrefs
are `#` until the store listings exist.

`assets/download.js` reads the browser's platform and makes the matching link the large
main button, with the others below it. Without JS, or on another platform (macOS, iOS),
all of them show at the same size.

## Docs

Each `docs/*.md` file is a page in the sidebar. The sidebar starts with a link home, and
there's no header. Front matter:

```
---
title: Syncing devices
order: 3                 # sidebar position; pages without one follow, by title
description: For search results and link previews (defaults to the first paragraph)
draft: true              # left out unless built with --drafts
---
```

`index.md` is always first.

## Images

Images work as in the blog: `![Alt text](path.png "Optional caption")`, with the path
relative to the Markdown file, or starting with `/` for one relative to `site/`. A missing
alt text fails the build; write `![""](x.png)` for a decorative image. Each image gets a
tiny inline placeholder, connection-dependent quality tiers (dithered, lite, full
AVIF/WebP/JPEG), lazy loading and a lightbox. Variants are cached in `.cache/images/`.

## Page transitions

In browsers with cross-document view transitions (Chrome and Edge), the page slides left
from home to the docs and right on the way back, and between docs pages the content
slides up or down according to the sidebar order. `assets/head.js` picks the direction;
the animations are at the end of `assets/style.css`. With `prefers-reduced-motion` set,
there are none.

## Deploying

`.github/workflows/site.yml` (at the repository root) runs on pushes to `master` that
change `site/`, or when started by hand. It runs the tests, builds with the Pages URL as
`--base-url`, and deploys `dist/` to GitHub Pages.

One-time setup:

1. In the repository, **Settings → Pages → Source**: choose **GitHub Actions**.
2. Under **Custom domain**, enter `kithso.ng`, then tick **Enforce HTTPS** once the
   certificate is issued. A CNAME file isn't needed: Pages ignores it for Actions deploys.
3. At the domain's registrar, point `kithso.ng` at GitHub Pages: `A` records to
   `185.199.108.153`, `185.199.109.153`, `185.199.110.153` and `185.199.111.153`, `AAAA`
   records to `2606:50c0:8000::153` to `2606:50c0:8003::153`, and `www` as a `CNAME` to
   `tgnra.github.io`.
4. Verify the domain under your account's **Settings → Pages**, so no other repository
   can claim it.

The site uses root-relative URLs, so it must be served from the domain root. Until the
custom domain is set, Pages would serve it under `/kithsong/`, and the workflow fails
with an explanation rather than deploy a broken site.
