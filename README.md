# Kithsong

> **kith** (n.): your own people and home ground; friends, neighbours, the familiar
> place you belong. From Old English *cȳþþ*, "native land, kinsfolk", the *kith* of
> "kith and kin".

Kithsong is a local-only music library that syncs content over the local network. It can auto-tag imported music from MusicBrainz and scrobble to Last.fm. The Windows version is Win32 with `w64devkit`. The Linux version is a Flatpak that uses GTK4 and `libadwaita`. The Android and WearOS versions use Jetpack Compose. There are no versions for Apple platforms. If you'd like this, open an issue or contact me!

## Screenshots

TODO

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

## Quick start

### Windows

Windows 10 version **1903** or newer is required to run this application.

The recommended way to install Kithsong on Windows is using the [Microsoft Store](https://isitchristmas.com). This will give you automatic updates and the firewall is configured automatically. If you don't want to use the Microsoft Store app, you can install with Winget using the following command:

```powershell
winget install TODO --source msstore
```

Alternatively, there are portable zips with a .exe built for each release, available [here on Github]()

### Linux

The recommended way to install Kithsong is using a Flatpak. See installation instructions for Flatpak for your distro [at Flathub's website](https://flathub.org/en/setup)

Open the [Flathub store page]() and click download or run this command:

```bash
flatpak install ng.kithso.app
```

Alternatively, there is a tarball built for each release, available [here on Github]()

### Android

TODO

## Contributing

TODO
