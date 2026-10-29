# Kithsong

> **kith** (n.): your own people and home ground; friends, neighbours, the familiar
> place you belong. From Old English *cȳþþ*, "native land, kinsfolk", the *kith* of
> "kith and kin".
>
> **Kithsong**: the songs of your own people and place. Your music, kept at home
> among your own devices.

A music library manager and player for Windows, Linux and Android. It autotags your
collection against MusicBrainz and keeps every device in sync over the local network,
with no server and no cloud.

Priorities:

- **Fast.** Quick launch, quick sync, and a UI that stays responsive with a large library.
- **Self-contained.** Codecs, TLS and the tag library are built into the app, so it relies
  on the OS as little as possible.
- **One brain.** A shared C core holds all the logic, so every platform behaves the same.

> **Status:** in development. Windows, Linux and Android import, autotag, browse, play and
> sync a library, including playlists, album groups, smart lists, search, scrobbling and
> merging two libraries when pairing. Windows is the most complete and tested; the Linux
> frontend has had the least testing, and Wear OS is a prototype (albums and a basic player,
> with sync still a mock-up). What's left, and which platform has what, is in
> [TODO.md](TODO.md); the core's modules are in [docs/architecture.md](docs/architecture.md).

## Platforms

| Platform | UI | Toolchain |
|----------|----|-----------|
| Windows  | Win32 | [w64devkit](https://github.com/skeeto/w64devkit) (gcc), CMake, Ninja |
| Linux    | GTK4 + libadwaita | gcc/clang, CMake, Ninja |
| Android  | Jetpack Compose | Android Studio / Gradle, NDK |
| Wear OS  | Compose for Wear OS | Android Studio / Gradle, NDK |

## Features

### Library and autotagging

- **One library folder.** You choose it on first run (default: your Music folder). It
  holds everything: the music, organised as `Artist/Album (Year)/01 Title.flac`, a
  `cover.jpg` per album, and the database as `library.db`. The app's own settings, i.e. where
  that folder is, live in `%LOCALAPPDATA%\kithsong` (`~/.config/kithsong` on Linux).
- **Import**: drop folders or `.zip` files (a Bandcamp download, say) on the window; on
  Android, pick a folder or files with the system picker, or share or open a zip with the app.
  Files are copied into the library; your originals are never modified. Everything is scanned and matched against MusicBrainz in one go.
  Confident matches are imported straight away, with cover art from the Cover Art Archive
  or iTunes. Anything below the confidence threshold is listed for you to review: pick a
  match, search again, keep the existing tags or skip.
- **Singles** don't become one-track albums. Each artist's singles live together in one
  album, "&lt;artist&gt; Singles".
- The tagger is our own C code, not beets. That means no bundled Python, the same importer
  on every platform including Android, and tagging rules we control. See
  [docs/tagging-rules.md](docs/tagging-rules.md).
- The library is a single SQLite database with our own schema. Every artist, album, track
  and playlist has a stable id that is the same on all devices. See
  [docs/schema.md](docs/schema.md).

### Playlists

- One shared playlist structure, keyed by the stable track ids, so playlists survive sync
  and re-imports.
- **Smart playlists and smart album groups** pick their tracks or albums by a rule, e.g. "jazz
  I haven't played in six months, least played first, 10 tracks", worked out afresh each time
  from play counts and this device's files. The rule syncs whole. On Windows they're made and
  edited with a rule editor (right-click the Playlists or Album groups page), with a live
  preview; elsewhere they can be opened, played and reshuffled so far.

### LAN sync

- **Discovery:** each instance announces itself over mDNS. A background thread scans
  periodically and shows new devices for pairing.
- **Pairing:** a short code links two devices once. If both already have music, the smaller
  library is merged into the larger one, once you agree on the device whose library is merged
  (it keeps a backup copy of its old library). No music moves until you've chosen what the
  joining device holds and pressed **Sync now**. After that they reconnect and sync
  automatically whenever both are online.
- **Archives:** the device whose music the group started from is its *archive*: it keeps the
  originals of everything. A device that joins a group says so once, naming the archive.
- **No server:** any device can sync with any other. Edits are merged field by field using
  hybrid logical clocks, and playlist order merges cleanly even when it was changed on two
  devices.
- **Remove from one device:** an album's or artist's menu has "Remove from this PC" (this
  computer, this phone). Unlike "Delete from library" (on albums and tracks; an artist can't
  be deleted), it takes the music off that device only, at once, and it stays in the library and on your other devices; "Sync to this PC"
  on the same menu brings it back. It's offered on devices that sync part of the library,
  not on an archive or outside a group.
- **Lossy copies:** a device can ask for a lossy format instead of the originals, e.g. a
  phone requesting Opus rather than FLAC. The sender transcodes and caches the
  copies, and the copies are never mistaken for originals.

Details: [docs/sync-protocol.md](docs/sync-protocol.md).

## UI

**Desktop**

- A header bar with playback controls on the left, and **Import** and **Settings** on the
  right.
- A left sidebar with **Albums**, **Artists**, **Playlists** and **Album groups**, a **Recents**
  section of the ten albums and playlists last played and artists and album groups last
  opened on any device, and a **Devices** section listing sync
  devices: online ones, and paired ones that are currently unreachable. It scrolls when it's
  taller than the window.
- The rest of the window is an album grid. Clicking an album opens its track list in a pane
  on the right. The Playlists and Album groups pages have a **+** in their header to make a
  new, empty one.
- An empty library suggests importing or, while the computer isn't paired yet, pairing it
  with another device to sync that device's music.

**Keyboard shortcuts** (Windows and Linux)

| Keys | Does |
| --- | --- |
| Space | Play or pause |
| Ctrl+Right / Ctrl+Left | Next / previous track |
| Shift+Right / Shift+Left | Forward / back 5 seconds |
| Ctrl+Up / Ctrl+Down, or + / - | Volume up / down |
| Ctrl+M | Mute or unmute |
| Ctrl+S | Shuffle on or off |
| Ctrl+F (on Windows also Ctrl+E or F3) | Search; on Linux, typing anywhere but a text box searches for it too |
| Ctrl+1, 2, 3, 4 | Albums, Artists, Playlists, Album groups |
| Ctrl+, | Settings |
| Ctrl+O | Import: choose folders to import |
| Ctrl+N | New playlist |
| Alt+Left or Backspace | Back, from an artist's page or an album group |
| F1 | About Kithsong |
| Esc | Close the track list |
| Enter | Play the selected track |
| Delete | Remove the selected track from the playlist; on the Playlists page, delete the selected playlist |
| Alt+Up / Alt+Down | Move the selected track up / down the playlist |
| F2 | Rename the playlist (in the track list, or the one selected on the Playlists page) |
| Menu key or Shift+F10 | The selected track's menu (with none selected, the playlist's) |

On Linux, Ctrl+? lists the shortcuts, and Ctrl+W or Ctrl+Q closes the window, which quits:
playback stops with it, as nothing keeps playing in the background. On Windows Alt+F4 quits
(there's no Ctrl+Q or Ctrl+W, as is usual there), and on the Playlists and Album groups
pages Delete and F2 work on smart playlists and album groups too. The playback buttons' and
other buttons' tooltips show their shortcuts.

In a text box (e.g. search) the arrow keys, + and - keep their usual meaning. The volume is
the app's own and is remembered; there's no volume control on screen yet, so the level shows
briefly in place of the track's name.

**Android**

- A Compose app that follows the system light/dark theme, with colours taken from the album
  art on screen.
- One scrolling main screen: a header with **Import**, **Search**, **Sync** and **Settings**, then
  **Recents**, **Playlists**, **Album groups**, an album grid and **Artists**. Tapping or
  holding a section's heading jumps to another section, and tapping an item opens its track
  list over the main screen; screens slide away with a swipe or the back gesture.
- A mini player above the navigation bar opens the full player. Playback runs in the
  background, with Android's media controls and notification.
- Holding an album, artist, playlist or album group opens its menu (Play, Shuffle, Add to
  playlist, Remove from this phone, Delete from library and so on), as do the menu buttons on
  an album's page and the player; a playlist's tracks and an album group's albums reorder by
  dragging their handles.
- **Sync** lists this phone and the other devices, pairs new ones, and opens each device's
  page to choose what it syncs. **Settings** has scrobbling, storage, and the open source
  licences.
- An empty library suggests importing or, until the phone is paired, pairing it with a
  computer; once it's in a group, choosing what to sync to it.

**Wear OS**

- A prototype: a menu of **Albums**, **Sync** and **Settings** (the open source licences so
  far), the albums on the watch, and a basic player. Artists and playlists aren't built yet,
  and sync is a mock-up.

## Architecture

All behaviour lives in `libmusiccore`, a C11 library. The frontends stay thin and only
render state and forward user actions.

```
core/            libmusiccore: library, tagger, playback, sync, discovery, transcode
  include/       public API (musiccore/*.h)
  src/           implementation, one directory per module
  tests/         CTest unit tests
desktop/windows  Win32 frontend
desktop/linux    GTK4 + libadwaita frontend
android/         Gradle project
  native/        core built with the NDK + JNI bridge, shared by both apps
  app/           phone app (Compose)
  wear/          Wear OS app (Compose for Wear OS)
third_party/     CMake glue for dependencies; all are fetched at configure time, none committed
docs/            design notes
```

Dependencies and their licenses: [LICENSES.md](LICENSES.md).

## Building

The first configure downloads TagLib, curl and Mbed TLS (pinned by SHA-256) into `build/_deps`.

Optional CMake settings:

- `-DKITHSONG_CONTACT=<url or email>`: sent in the MusicBrainz User-Agent. MusicBrainz
  asks every client to include one.
- `-DKITHSONG_LASTFM_KEY=<key>` and `-DKITHSONG_LASTFM_SECRET=<secret>`: a Last.fm API
  account (from https://www.last.fm/api/account/create), for scrobbling to Last.fm. Keep
  them out of git too. Or set the environment variables of the same names, or paste the
  account page into `last.fm.api` at the top of the repo (gitignored). Without them the
  app builds fine and Settings says it can't scrobble.

How to get each of these, where to put them on every platform and how to test them:
[docs/api-keys.md](docs/api-keys.md).


### Windows

Install CMake, Ninja and w64devkit, and put w64devkit's `bin` on `PATH`, or set the
`W64DEVKIT` environment variable to its root folder.

From VS Code:

| | |
|---|---|
| `Ctrl+Shift+B` | build (`windows: build`) |
| `Tasks: Run Task` → `windows: run` | build and launch |
| `Tasks: Run Task` → `windows: test` | run unit tests |
| `F5` | build and debug with gdb |

From a terminal:

```sh
cmake --preset windows-w64devkit
cmake --build --preset windows-w64devkit
ctest --preset windows-w64devkit
build/windows-w64devkit/desktop/windows/kithsong.exe
```

### Linux

Needs GTK ≥ 4.12 and libadwaita ≥ 1.4 development packages, e.g.
`sudo apt install libgtk-4-dev libadwaita-1-dev ninja-build cmake`.

```sh
cmake --preset linux-gtk
cmake --build --preset linux-gtk
ctest --preset linux-gtk
build/linux-gtk/desktop/linux/kithsong
```

With Nix (flakes enabled), `flake.nix` provides a dev shell with CMake, Ninja, GTK4,
libadwaita and gdb. It also sets up what an installed app would get from its wrapper: the
audio libraries miniaudio loads at runtime (ALSA, PulseAudio, PipeWire), GSettings schemas
and icons. Run the commands above through it, or enter it with `nix develop`:

```sh
nix develop -c cmake --preset linux-gtk
nix develop -c cmake --build --preset linux-gtk
nix develop -c ctest --preset linux-gtk
nix develop -c build/linux-gtk/desktop/linux/kithsong
```

The VS Code tasks (`linux: build`, `linux: run`, `linux: test`) and the
`Kithsong (Linux, gdb)` debug configuration run inside the dev shell. `Ctrl+Shift+B` runs
the Windows build, so on Linux start `linux: build` from `Tasks: Run Task`.

### Android

Open `android/` in Android Studio, or build with the Gradle wrapper through
`android/scripts/android.sh`, which takes `JAVA_HOME` (and, on NixOS, the emulator's
libraries) from Android Studio when it is not already set:

```sh
android/scripts/android.sh gradle :app:assembleDebug    # phone
android/scripts/android.sh gradle :wear:assembleDebug   # watch
```

The NDK build compiles the core from the repo root. It needs CMake ≥ 3.28 from the SDK
manager (`sdkmanager "cmake;3.30.5"`), or unzip
https://dl.google.com/android/repository/cmake-3.30.5-linux.zip into
`$ANDROID_HOME/cmake/3.30.5`. The watch app also ships `armeabi-v7a`, because many Wear OS
watches run a 32-bit userspace.

To run the watch app, create a Wear OS emulator named `Wear_OS_Small_Round` in Android
Studio's Device Manager and run the `wear: run` task in VS Code. It boots the emulator unless
it is already running, then installs and starts the debug build.

### Release builds

`scripts/release-*` build the packages: a portable zip on Windows, a Flatpak bundle on
Linux, and signed APKs and AABs for Android and Wear OS. Pushing a `v<version>` tag runs them
on GitHub Actions and creates a draft release. See [docs/RELEASING.md](docs/RELEASING.md).
