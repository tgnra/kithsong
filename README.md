# musicapp

A music library manager and player for Windows, Linux and Android. It autotags your
collection against MusicBrainz and keeps every device in sync over the local network,
with no server and no cloud.

Priorities:

- **Fast.** Quick launch, quick sync, and a UI that stays responsive with a large library.
- **Self-contained.** Codecs, TLS and the tag library are built into the app, so it relies
  on the OS as little as possible.
- **One brain.** A shared C core holds all the logic, so every platform behaves the same.

> **Status:** early scaffold. The core opens and migrates its database, reads and writes
> tags, and can reach MusicBrainz over HTTPS. Each frontend shows an empty window. See
> [docs/architecture.md](docs/architecture.md) for what's stubbed.

## Platforms

| Platform | UI | Toolchain |
|----------|----|-----------|
| Windows  | Win32 | [w64devkit](https://github.com/skeeto/w64devkit) (gcc), CMake, Ninja |
| Linux    | GTK4 + libadwaita | gcc/clang, CMake, Ninja |
| Android  | Jetpack Compose | Android Studio / Gradle, NDK |

## Features

### Library and autotagging

- **Import** scans a folder, matches it against MusicBrainz (and AcoustID fingerprints when
  the tags aren't enough), shows the best matches for you to confirm, then writes clean
  tags.
- The tagger is our own C code, not beets. That means no bundled Python, the same importer
  on every platform including Android, and tagging rules we control. See
  [docs/tagging-rules.md](docs/tagging-rules.md).
- The library is a single SQLite database with our own schema. Every artist, album, track
  and playlist has a stable id that is the same on all devices. See
  [docs/schema.md](docs/schema.md).

### Playlists

- One shared playlist structure, keyed by the stable track ids, so playlists survive sync
  and re-imports.

### LAN sync

- **Discovery:** each instance announces itself over mDNS. A background thread scans
  periodically and shows new devices for pairing.
- **Pairing:** a short code links two devices once. After that they reconnect and sync
  automatically whenever both are online.
- **No server:** any device can sync with any other. Edits are merged field by field using
  hybrid logical clocks, and playlist order merges cleanly even when it was changed on two
  devices.
- **Lossy copies:** a device can ask for a lossy format instead of the originals, e.g. a
  phone requesting Opus or MP3 rather than FLAC. The sender transcodes and caches the
  copies, and the copies are never mistaken for originals.

Details: [docs/sync-protocol.md](docs/sync-protocol.md).

## UI

**Desktop**

- A header bar with playback controls on the left, and **Import** and **Settings** on the
  right.
- A left sidebar with **Albums**, **Artists** and **Playlists**, plus a **Devices** section
  listing sync devices: online ones, and paired ones that are currently unreachable.
- The rest of the window is an album grid. Clicking an album opens its track list in a pane
  on the right.\

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
android/         Gradle project; Compose UI + JNI bridge to the core
third_party/     vendored single-file libraries; larger deps are fetched by CMake
docs/            design notes
```

Dependencies and their licenses: [LICENSES.md](LICENSES.md).

## Building

The first configure downloads TagLib, curl and Mbed TLS (pinned by SHA-256) into `build/_deps`.

Optional CMake settings:

- `-DMUSICAPP_CONTACT=<url or email>`: sent in the MusicBrainz User-Agent. MusicBrainz
  asks every client to include one.
- `-DMUSICAPP_ACOUSTID_KEY=<key>`: enables fingerprint lookups. Keep it out of git, for
  example in a `CMakeUserPresets.json`, which is already ignored.

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
build/windows-w64devkit/desktop/windows/musicapp.exe
```

### Linux

Needs GTK ≥ 4.12 and libadwaita ≥ 1.4 development packages, e.g.
`sudo apt install libgtk-4-dev libadwaita-1-dev ninja-build cmake`.

```sh
cmake --preset linux-gtk
cmake --build --preset linux-gtk
ctest --preset linux-gtk
build/linux-gtk/desktop/linux/musicapp
```

### Android

Open `android/` in Android Studio, or generate the Gradle wrapper once (`gradle wrapper`)
and then run:

```sh
cd android
./gradlew assembleDebug
```

The NDK build compiles the core from the repo root. It needs CMake ≥ 3.28 from the SDK
manager (`sdkmanager "cmake;3.30.5"`).
