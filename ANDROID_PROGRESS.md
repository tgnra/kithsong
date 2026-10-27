# Android phone app progress

Work on the phone UI is done by agents in sequence, one phase at a time. The spec lives in
the Android section of `TODO.md`. This file records what each phase did and the decisions made.

## Decisions before work started

- Build on the existing `android/` Gradle project (`:native`, `:app`, `:wear`) rather than
  creating a new hello world app.
- Tapping the mini player opens a full player with large art, a seekbar, play/pause and
  seeking. Swipe down to dismiss.
- Albums, artists and playlists open a track list screen. Swipe right anywhere to dismiss.
- Search is deferred.
- Albums are a grid; Recents and Playlists are lists. The main screen is one lazy list, with
  album grid rows inside it rather than nested grids, and album art is preloaded ahead of the
  scroll position.
- Edge to edge. The header clears the status bar at rest and art scrolls under it. The mini
  player is a floating rounded rectangle that clears the gesture navigation bar.
- Background playback through a media session.
- Sync writes to the app's own storage, never the SD card.
- Sections with no content have no heading; with no content at all, show an icon and a message.
- Predictive back, accessibility labels and colours taken from the album art are all in scope.
- Colours from album art come in the playback phase, when real art and a current track exist.

## Phases

1. Build system and emulator check, current Compose and Material 3, lowest feasible minSdk
2. Mockup UI with fake data
3. Real library data and album art loading
4. Playback through a media session, colours from album art
5. Import, permissions and settings
6. Sync into app storage
7. Polish left over from earlier phases: predictive back on track list screens, landscape,
   launch flash in light mode, strings into resources

## Phase 1: build system and emulator

The phone app builds on Windows and runs on the `Pixel_10` emulator (API 37); the native core
loads and reports its version.

Changes:

- `app/build.gradle.kts`: compose-bom 2026.09.00 and activity-compose 1.13.0. compileSdk 37,
  because Compose 1.12 requires it; targetSdk stays 36. minSdk 24.
- `wear/build.gradle.kts`: the same compose-bom and activity-compose versions.
- `native/build.gradle.kts`: minSdk 24, and `-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON` so the
  library is 16 KB page aligned and Android 15+ doesn't warn about it.
- `Theme.kt` already used dynamic colour on Android 12+ and followed the system dark/light
  setting, so it was left alone.

Decisions:

- **minSdk 24 (Android 7.0).** Current Compose and activity need 23. The core sets the floor:
  `core/src/discovery/discovery.c` uses `getifaddrs`, which exists from API 24. AAudio is not a
  constraint because miniaudio loads it at runtime and falls back to OpenSL ES.
- `android/scripts/android.sh` is Linux-only, so on Windows use `gradlew.bat` directly.

Setup on this machine: cmdline-tools and CMake 3.30.5 were installed into the SDK; Gradle
fetched NDK 27.2.12479018. There is no `local.properties`, so `ANDROID_HOME` must be set:

```
$env:JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"; $env:ANDROID_HOME="$env:LOCALAPPDATA\Android\Sdk"
cd android; .\gradlew.bat :app:assembleDebug
& "$env:ANDROID_HOME\emulator\emulator.exe" -avd Pixel_10
adb install -r app\build\outputs\apk\debug\app-debug.apk
adb shell am start -W -n app.musicapp/.MainActivity
```

Open issues:

- The 16 KB warning dialog persisted after `install -r` until the app was uninstalled once.
- `:wear:assembleDebug` was built with the new dependencies, but not since the page size change.

## Phase 2: mockup UI with fake data

The whole spec except playback, import, settings and sync runs on fake data and was checked on
the emulator in light and dark mode: the main screen at rest and scrolled, the heading jump
list, track list screens with swipe right, the full player with seeking and swipe down,
predictive back on the full player, and the empty state (launch with `--ez fake_empty true`).

Files, under `android/app/src/main/kotlin/app/musicapp/`:

- New: `MusicApp.kt`, `data/Library.kt`, `data/FakeLibrary.kt`, `playback/Playback.kt`,
  `ui/App.kt`, `ui/MainScreen.kt`, `ui/DetailScreen.kt`, `ui/Player.kt`, `ui/Dismissible.kt`,
  `ui/Seekbar.kt`, `ui/Icons.kt`, `ui/art/Art.kt`
- Changed: `MainActivity.kt`, `AndroidManifest.xml` (registers `MusicApp`), `app/build.gradle.kts`
  (adds Coil)

Decisions:

- The UI reads a `LibrarySource` and a `Playback` interface; later phases swap in real ones.
- Navigation is a small saveable back stack, not Navigation Compose. Each layer owns its slide
  in, drag to dismiss and predictive back; Navigation Compose's transitions would fight the
  custom drag gestures.
- The album grid is rows of N albums, each row one item in the single main list. N comes from
  the screen width with a 150dp minimum cell.
- Art loads through Coil 3.4.0 behind an `ArtSource` interface; the fake source draws coloured
  letter placeholders. Art is prefetched six items ahead of and three behind the visible range.
  Coil 3.5+ needs Kotlin 2.4, and the project is on 2.2.21.
- Added beyond the spec, for accessibility: a back arrow on track list screens and a collapse
  button on the full player.

Open issues:

- Predictive back checked on the full player only, not on track list screens.
- Landscape untested.
- The manifest theme is a dark platform theme, so light mode may flash dark at launch.
- The fake playback uses `runBlocking` at startup.
- Strings are hard-coded rather than in resources.

## Phase 3: real library data and album art

The app reads the real library from the core and shows real art. Tested with 321 albums and
2,552 tracks; with an empty cache, art was already drawn in screenshots taken mid-fling. The
core's C code is unchanged and the wear app still compiles.

Files, under `android/`:

- Bridge: `native/src/main/cpp/jni_bridge.c`, `CoreLibrary.kt`, `NativeCore.kt` (now calls
  `mc_init`), new `CoreImport.kt`. Covers playlists, smart playlists, track lists and import.
- App: new `data/CoreLibrarySource.kt`; changed `Library.kt`, `FakeLibrary.kt`, `ui/art/Art.kt`,
  `MusicApp.kt`, `MainActivity.kt`, `App.kt`, `MainScreen.kt`, `DetailScreen.kt`.
- Debug builds only: `app/src/debug/AndroidManifest.xml` and `DebugImportReceiver.kt`.

Decisions:

- The library lives in `noBackupFilesDir/library`. Music is too big for Auto Backup, and a
  restored database would carry this phone's sync id onto another device.
- No paging: the core returns every album in one query (72 ms for 321 albums).
- Core calls run on one background thread. The list stays unset until the first read, so the
  empty state doesn't flash at launch.
- The core has no artist or recents API. Artists are derived from the album list and Recents
  are stored by the app for now. Smart album groups aren't shown.
- Covers are downsampled to cell size while decoding and cached in memory and as thumbnails on
  disk. Embedded art is used when there is no cover file.
- Fake data: launch with `--ez fake true`, or `--ez fake_empty true` for the empty state.
- Kotlin and Coil were not upgraded.

Loading test data (debug builds):

1. Launch the app and send `adb shell am broadcast -n app.musicapp/.DebugImportReceiver` once,
   so the app creates its own `import` folder (folders created by adb aren't readable by it).
2. `adb push` music into `/sdcard/Android/data/app.musicapp/files/import/`.
3. Send the broadcast again. Every item is looked up on MusicBrainz first; with the network
   off, 332 items still took about 6 minutes.

The generated test library came from a throwaway script in the session scratchpad, not the repo.

Not verified on the device: playlists and smart playlists (no way to create one yet), and a
fix for artists that showed a letter placeholder despite having covers.

Open issues:

- The core doesn't record play counts from Android yet; `Track.path` is ready for the player.
- `CoreImport` events can arrive before the UI is ready, so the import UI must hand them over to
  another thread.
- Recents should move into the core so they can sync.
- Import is slow even offline because of the MusicBrainz lookups.

## Phase 4: playback and colours from album art

Real audio plays through the core's player inside a Media3 media session, with notification
and lock screen controls. Media key events, seeking, playback with the screen off and after the
activity finished were all checked on the emulator, and `dumpsys audio` showed the app's stream
playing. The full and mini players take their colours from the album art in light and dark mode.

Files, under `android/`:

- New in `app/.../playback/`: `CoreMediaPlayer.kt`, `PlaybackService.kt`, `SessionPlayback.kt`,
  `MediaItems.kt`, `ArtBitmapLoader.kt`; plus `ui/theme/ArtTheme.kt`.
- Changed: `app/build.gradle.kts`, `AndroidManifest.xml`, `MusicApp.kt`, `MainActivity.kt`,
  `Playback.kt`, `Library.kt`, `FakeLibrary.kt`, `CoreLibrarySource.kt`, `App.kt`, `Player.kt`,
  `Icons.kt`, `ui/art/Art.kt`.
- Bridge: `jni_bridge.c` and `CoreLibrary.kt` gain calls to record plays and preview smart albums.

Decisions:

- Only `media3-session` 1.11.1 is added, not ExoPlayer, since the core decodes and plays.
  `CoreMediaPlayer` adapts the core player for Media3 and handles audio focus, pausing when
  headphones are unplugged, and a wake lock while playing.
- The core has no volume control, so the app doesn't duck itself. Android 8+ ducks it
  automatically; Android 7 doesn't duck.
- The notification permission is requested once, on first play (Android 13+). Playback controls
  still show without it.
- Previous and next are in the full player and the notification because the core's queue made
  them trivial. The mini player stays per spec.
- Android records plays in the core (desktop doesn't yet). A play counts after half the track or
  4 minutes, whichever comes first.
- Recently played albums now come from the core, so plays on other devices will appear after
  sync. Playlist and artist recents stay in the app's storage, with timestamps, and are merged in.
- Colours use MaterialKolor 4.1.1 with Material's Content style; 5.x needs Kotlin 2.4. They apply
  to the full and mini player and animate on track change. The rest of the app keeps system colours.

Not verified: that plays were written to the core's database (no errors, but not queried);
ducking on Android 7; focus loss during a call; the colour animation itself.

Open issues:

- Playback can't resume after the process dies (e.g. Bluetooth play with the app not running),
  and the queue isn't saved across restarts.
- Import must call `refresh()` when it finishes.
- Synced plays only appear in Recents on the next refresh; artist and playlist recents don't sync.

## Phase 5: import, permissions and settings

Import, its permission flow and a settings screen work on the emulator. No core C code changed.

Files, under `android/`:

- New in `app/.../`: `AppSettings.kt`, `importer/DeviceMusic.kt`, `importer/Importer.kt`,
  `importer/ImportService.kt`, `ui/ImportUi.kt`, `ui/SettingsScreen.kt`
- Changed: `AndroidManifest.xml`, `MusicApp.kt`, `MainActivity.kt`, `ui/App.kt`,
  `ui/MainScreen.kt`, debug `DebugImportReceiver.kt`
- Bridge: `jni_bridge.c` and `CoreImport.kt` gain `itemName` and `itemError`

Decisions:

- The core needs real file paths and copies files into the library (in app storage, never the SD
  card), as on desktop. So music is found through MediaStore with READ_MEDIA_AUDIO (API 33+) or
  READ_EXTERNAL_STORAGE (API 32 and below, with legacy storage on API 29), not a folder picker,
  which would mean copying every file twice.
- The import sheet lists top-level music folders per storage volume. Folders imported before
  start unticked, because the core doesn't skip files it already has. It warns when offline.
- Permissions: ask, show a rationale when Android suggests one, and after a permanent denial
  offer a link to the app's system settings.
- Import runs in a foreground service (dataSync type) with a wake lock rather than WorkManager,
  because the core's import session lives in the process and can't be resumed.
- Progress shows in a card under the header, with Cancel, "Import as tagged" or "Skip" for items
  with no confident match, and a summary of failures at the end. The notification has the same
  actions and posts a summary if the app is in the background.
- The library refreshes at most every 3 seconds while albums arrive, and once at the end.
- Settings: download missing covers (the core's `fetch_art`), music and notification permission
  status, library size, art cache size with Clear, app and core versions. Notifications are
  requested once, on first play or first import.
- The debug import receiver stays, debug-only, and now goes through the same importer.

Verified: revoke, request, rationale, permanent denial and the link to app settings; the folder
sheet; import online, in the background and finished from the notification; review of items
offline in dark mode; an untagged WAV imported; cancel stops the service; settings in light and
dark mode, surviving rotation.

Open issues:

- **Import is slow offline.** Each item makes one or two MusicBrainz requests. Each waits for the
  shared 1.1 s rate limit even when it fails at once, and a failed connection can take 10 s to
  give up. The core never notices it is offline and has no option to turn lookups off, so every
  item then needs a decision. Fix in the core: an option to skip lookups, or stop after the
  first network error.
- **Duplicates.** Importing the same tracks again copies them again as "(2)" files. The
  emulator's test library now has 5 albums with doubled tracks.
- Test files remain in the emulator's `/sdcard/Music` and `/sdcard/Download`.
- Files MediaStore hasn't indexed don't appear in the sheet.
- No screen to review items one by one; the notification icon is a platform one.

## Phase 6: sync

The phone syncs end to end, but was tested against a scratch C peer rather than the desktop
app, because the Windows desktop's Add Device only logs a message so far. No core C changed.

How sync works in the core: there is no server, and any two devices in a group sync with each
other. Devices find each other over mDNS, or by address when discovery doesn't reach. They pair
with a 6-digit code that one device shows and the other types in; a device can only join with
an empty library. Group data and the whole library's metadata sync, including playlists and
plays, merged with hybrid logical clocks. **The core doesn't transfer audio files yet**, so the
rule that synced files go into app storage holds only because the library already lives there.

Files, under `android/`:

- Bridge: `native/.../jni_bridge.c` and a new `CoreSync.kt`
- New: `app/.../sync/Syncer.kt`, `SyncService.kt`, `SyncWorker.kt`, and `ui/DevicesUi.kt`
- Changed: `MusicApp.kt`, `MainActivity.kt`, `CoreLibrarySource.kt`, `Importer.kt`, `App.kt`,
  `SettingsScreen.kt`, `Icons.kt`, `AndroidManifest.xml`, `app/build.gradle.kts` (WorkManager 2.11.1)

Decisions:

- Sync lives in a Devices section at the top of Settings, not a header icon: pairing is rare,
  syncing is automatic, and it mirrors the desktop's devices list. The Add Device dialog uses
  the desktop's wording and fields. The pairing code dialog can appear over any screen, since
  the other device can start pairing.
- Sync runs while the app is on screen, with a `dataSync` foreground service during a session
  or pairing. In the background, WorkManager syncs about hourly on Wi-Fi.
- A multicast lock is held while syncing. `NEARBY_WIFI_DEVICES` isn't needed because there is no
  Wi-Fi scanning, Wi-Fi Direct or Aware.
- `sync.db` is in no-backup storage.
- The phone lists only albums and tracks that have a file on the phone.

Verified on the emulator (with `adb forward` and 10.0.2.2): pairing in both directions,
including a wrong code; library metadata reaching the peers and a second peer syncing through
the group; a play and a playlist made on the peer appearing in Recents and Playlists without a
restart; the background job; removing a device and leaving the group.

Not tested: mDNS discovery (it doesn't cross the emulator's network), real Wi-Fi, API 24-32,
file transfer, and syncing with the desktop app.

Open issues:

- Android sometimes runs the background worker while it blocks the app's network, and the
  core's sessions then hang instead of timing out. The worker retries later; the hang needs a
  fix in the core.
- The same album imported on two devices: how to merge it is an open question in the protocol
  doc.
- The emulator's library now has a synced 4-track playlist and a few extra plays.

## Phase 7: polish

`:app:assembleDebug`, `:app:lintDebug` (0 errors) and `:wear:assembleDebug` all pass, and both
apps' native libraries pass the 16 KB alignment check. No core C changed.

- **Predictive back** already worked on album, artist, playlist and settings screens; checked
  mid-gesture, cancelled and committed. No code change.
- **Launch flash:** `core-splashscreen` 1.2.0 with `installSplashScreen()`, and light and night
  themes in `res/` (dynamic neutrals on v31, `system_surface_*` on v34). The splash and app
  backgrounds match exactly in both modes.
- **Landscape:** in short windows, track list and settings headings sit beside the back button
  (`isShortWindow()` in `App.kt`). The album grid's column count leaves out a side cutout. The
  full player already put art beside the controls. Cutouts on both sides checked; open screens
  and the player survive rotation.
- **Strings:** all user-visible text and content descriptions are in `res/values/strings.xml`,
  with plurals for counts. `TrackList` and `Recent` now carry structured fields instead of a
  ready-made subtitle, to keep text out of the data layer. Only the "name: error" format for
  failed import items is still hard-coded.
- **Icons:** a monochrome `ic_notification` for playback, import and sync, and an adaptive
  launcher icon with a monochrome layer (vector fallback for API 24-25).
- **Lint:** warnings went from 19 to 13. Left on purpose: the playback wake lock has no timeout,
  the media service must be exported, plus version-pin and UseKtx warnings.

Not checked: the themed launcher icon on screen, and the import and sync notification icons.

## Follow-ups

Core:

- Import lookups: detect being offline, or offer an option to skip MusicBrainz, so offline
  import isn't slow and every item doesn't need a decision.
- Importing the same files twice copies them again; skip files already in the library.
- Sync doesn't transfer audio files yet.
- Sync sessions hang instead of timing out when Android blocks the network.
- Merging the same album imported on two devices is an open question in the protocol doc.
- No artist or recents API; artists are derived from albums and some recents live in the app.
- No volume control, so the app can't duck itself on Android 7.
- Desktop doesn't record plays yet; Android does.

Desktop:

- The Windows Add Device only logs a message, so phone-to-desktop sync is untested.

Android:

- Resume playback after process death (e.g. Bluetooth play) and save the queue across restarts.
- Not yet tested: mDNS discovery and sync over real Wi-Fi, API 24-32 devices, audio focus during
  calls, and that plays land in the core's database.
- Coil and MaterialKolor are held back until Kotlin moves to 2.4.
- A screen for reviewing import items one at a time; files MediaStore hasn't indexed are missed.
- Search.

The emulator's test library has 5 albums with doubled tracks, a synced playlist and extra plays;
test files remain in its `/sdcard/Music` and `/sdcard/Download`.
