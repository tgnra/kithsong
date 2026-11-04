---
title: Introduction
description: What Kithsong is and what it does.
---

Kithsong is a local-only music library that syncs content over the local network. It can auto-tag imported music from MusicBrainz and scrobble to Last.fm. The Windows version is Win32 with w64devkit. The Linux version is a Flatpak that uses GTK4 and libadwaita. The Android and WearOS versions use Jetpack Compose. There are no versions for Apple platforms. If you'd like this, open an issue or contact me!

## Features

### Library

- Single library folder holding organised music, album art, and the database
- Import from folder, or from a zip file (downloads from Bandcamp!) and it'll be searched on MusicBrainz and cleaned up
- Originals are copied into the library folder
- Matches Kithsong is confident in are imported straight away, others may require user confirmation or editing
- Automatically searches for cover art if missing or low-quality (under 500px)
- Hides single track releases from the library and instead shows them under the artist, for a clean albums view

### Organisation

- Playlists - stored in the database
- Album Groups - these are playlists of albums
- Smart Playlists and Smart Album Groups - the above, but generated from rules - these can be nested and quite complex, or very simple.
  - Live preview for smart playlists as you edit
- Search - fuzzy and instant
- Recents, play counts. Synced across devices

### Sync

- mDNS device discovery. Pair with a 6 digit code
- No server required - libraries are merged field-by-field
- One device in your group is an "Archive" - it keeps the originals of your entire library so it's all in one place
- Sync lossy copies to save storage - Opus
  - Set a storage budget and Kithsong works out the best option automatically
  - Set automatically if there is low free space

### Scrobbling

- Scrobble to Last.fm
  - Or libre.fm or a custom API destination
- Records play counts in the database too for use in Smart Playlists

### Devices

- On desktop - Windows and Linux, each with native UI (GTK on Linux, sorry KDE fans, I like GNOME! Open an issue if the app needs a change on KDE)
- Android - modern Compose app, keeps the library in app storage for simplicity
- WearOS - simple smartwatch client - sync playlists to it for when you go running