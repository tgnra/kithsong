---
title: Your library
order: 2
description: How Kithsong organises your music, and how importing and tagging work.
---

## The library folder

The first time you run Kithsong, it asks where to keep your library. It suggests your
Music folder. Everything goes in that one folder:

- your music, organised as `Artist/Album (Year)/01 Title.flac`
- a `cover.jpg` for each album
- the library database, `library.db`

Kithsong's own settings, including where the library is, are kept separately: in
`%LOCALAPPDATA%\kithsong` on Windows and `~/.config/kithsong` on Linux.

## Importing music

Drop folders or files on the window, or use **Import**. Kithsong copies them into the
library. It never changes your originals.

Everything you import is matched against [MusicBrainz](https://musicbrainz.org) in one go:

- **Confident matches** are imported straight away, with cover art from the Cover Art
  Archive or iTunes.
- **Anything else** is listed for you to decide: pick a match, search again, keep the
  tags the files already have, or skip them.

If you're offline, Kithsong says so, and you can import without tagging.

## Singles

Singles don't become one-track albums. Each artist's singles are kept together in one
album, called "*Artist* Singles".
