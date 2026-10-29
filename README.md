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

> **Status:** early scaffold. The core opens and migrates its database, and the importer
> tags folders against MusicBrainz and fetches cover art. The Windows frontend has the import
> screen, the album grid and gapless playback, with media keys, taskbar buttons and Windows'
> media controls; otherwise the frontends show an empty window. See
> [docs/architecture.md](docs/architecture.md) for what's stubbed.

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
  match, search again, keep the existing tags or skip. AcoustID fingerprints for untagged
  files are planned.
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
- **Pairing:** a short code links two devices once. The device that joins must have an empty
  library (on Windows and Linux the pairing dialog offers to create a new one if it hasn't),
  and no music moves until you've chosen what it holds and pressed **Sync now**. After that
  they reconnect and sync automatically whenever both are online.
- **Archives:** the device whose music the group started from is its *archive*: it keeps the
  originals of everything. A device that joins a group says so once, naming the archive.
- **No server:** any device can sync with any other. Edits are merged field by field using
  hybrid logical clocks, and playlist order merges cleanly even when it was changed on two
  devices.
- **Remove from one device:** an album's or artist's menu has "Remove from this PC" (this
  computer, this phone) next to "Delete from library". It takes the music off that device
  only, at once, and it stays in the library and on your other devices; "Sync to this PC"
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
  with another device to sync that device's music (pairing only joins an empty library).

**Keyboard shortcuts** (Windows and Linux)

| Keys | Does |
| --- | --- |
| Space | Play or pause |
| Ctrl+Right / Ctrl+Left | Next / previous track |
| Shift+Right / Shift+Left | Forward / back 5 seconds |
| Ctrl+Up / Ctrl+Down, or + / - | Volume up / down |
| Ctrl+M | Mute or unmute |
| Ctrl+S | Shuffle on or off |
| Ctrl+F | Search |
| Ctrl+1, 2, 3, 4 | Albums, Artists, Playlists, Album groups |
| Ctrl+, | Settings |
| Alt+Left or Backspace | Back, from an artist's page or an album group |
| Esc | Close the track list |
| Enter | Play the selected track |
| Delete | Remove the selected track from the playlist |
| Alt+Up / Alt+Down | Move the selected track up / down the playlist |
| F2 | Rename the playlist |
| Menu key or Shift+F10 | The selected track's menu (with none selected, the playlist's) |

In a text box (e.g. search) the arrow keys, + and - keep their usual meaning. The volume is
the app's own and is remembered; there's no volume control on screen yet, so the level shows
briefly in place of the track's name.

**Mobile**

- A Compose app that follows the system light/dark theme. The rest of the design is still
  open.

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
- `-DKITHSONG_ACOUSTID_KEY=<key>`: enables fingerprint lookups. Keep it out of git, for
  example in a `CMakeUserPresets.json`, which is already ignored.
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
