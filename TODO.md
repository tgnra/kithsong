# Todo

## Playlists

- Linux - playlist cover art (a placeholder for now)
- Windows - playlist cover art (a placeholder for now)
- Linux - keyboard access to the track menu (Menu key, Shift+F10)

## Album Groups

- Add UI to show Album Groups
- Add UI to browse an Album Group - the grid

## Smart Playlists/Groups

- UI - rule editor (build it from mc_smart_field_get()), list smart lists alongside playlists and album groups, reshuffle button
- Desktop playback - call mc_library_record_play() when a track counts as played. Android already does: half the track or 4 minutes, whichever comes first
- Core - limit by total duration ("an hour of music")

## Artists View

- Core - an artist API; Android derives artists from the album list
- List of Artists - toggle for album artist/ artist?
- Linux and Windows - Artists shows the album grid for now

## Import

- Core - detect being offline, or an option to skip MusicBrainz, so offline import isn't slow and every item doesn't need a decision
- Core - skip files already in the library; importing them again copies them as "(2)" files
- Core - AcoustID fingerprints (Chromaprint) for untagged files
- Core - cover art for the singles album

## Sync

- Core - test PREPARE_REQ end to end (needs three devices), and a merged album going through the file phase
- Core - albums and artists without MusicBrainz ids never merge; a playlist or album group that held both copies lists the survivor twice
- Core - changelog compaction must keep the rows that decide merges and archive demotions
- Linux - the sync UI: show the pairing code, a device page with profiles, Sync now, Leave Group (Windows has them)
- Windows - the device page's progress shows rows only; show files and bytes (files_done, files_total in mc_sync_progress)
- Windows and Linux - make a device an archive (mc_sync_set_archive) and show the size estimate (mc_sync_estimate_profile) in the profile editor
- Frontends - run mc_sync_verify(deep) weekly, and show the unknown files an archive reports
- Android - switch recents to the core's mc_library_record_opened()/mc_library_list_recents(); artists need core uids rather than names
- Test phone to desktop sync now that the desktop can pair, including file transfer

## Other

- Last.fm scrobbler
- Playback - AAC/M4A - pick a decoder: the free ones are GPL or awkwardly licensed, and the OS codec breaks "no OS codecs"
- Volume control (in the core, so Android can also duck itself on Android 7)
- Spacebar to play/pause
- Recents list above devices
- Linux and Windows - Move Library (the button is disabled)

## Wear OS

- Playback - a foreground MediaSessionService, so the system doesn't kill playback in the background and the watch's media controls work
- Playback - route to Bluetooth headphones and prompt to connect them, rather than the watch speaker
- Playback - progress and seeking on the player screen
- Playback - a way back to the player from the home screen
- Artists view
- Playlists
- Sync - drive the sync screen from the core instead of SampleSync
- Sync - request a Wi-Fi network while syncing; Wear OS keeps Wi-Fi off while the phone is connected over Bluetooth
- Sync - sync in the background, not only while the sync screen is open
- Settings

## Android

The phone app spec is done; see ANDROID_PROGRESS.md for the phases.

- Playback - resume after the process dies (e.g. Bluetooth play with the app not running), and save the queue across restarts
- Import - a screen to review items one at a time
- Import - files MediaStore hasn't indexed don't appear in the folder sheet
- Import - the "name: error" format for failed items is still hard-coded
- Upgrade Kotlin to 2.4, then Coil and MaterialKolor
- Test - mDNS discovery and sync over real Wi-Fi, API 24-32 devices, audio focus during calls, plays landing in the core's database
- Test - the themed launcher icon, and the import and sync notification icons
- Search

## Release

- Windows - installer, MSIX/Store package, code signing
- Linux - Flathub (every dependency in third_party/CMakeLists.txt as a Flatpak source), architectures other than x86_64
- Android - enable R8
- Android - third-party license texts in the apps

## Docs

- architecture.md - the playlist module is listed as a stub
- README - the status note and the Mobile UI section are out of date
