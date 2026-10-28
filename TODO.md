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
- Core - limit by total duration ("an hour of music")

## Artists View

- Core - an artist API; Android derives artists from the album list
- List of Artists - toggle for album artist/ artist?
- Linux and Windows - Artists shows the album grid for now

## Import

- Core - detect being offline, or an option to skip MusicBrainz, so offline import isn't slow and every item doesn't need a decision
- Core - duplicates: a swap keeps the library's album and track metadata and files the new file under the old name; the new file's tags come from its own match, so a release-group or by-name swap can leave tags naming another release than the library row
- Core - duplicates: a byte-identical file is only recognised against files that were imported as-is or synced (matched imports are re-tagged in the library, so their bytes differ from the source)
- Linux - check the import outcomes ("Replaced with higher quality", "Already in library", "Added missing tracks") in the running app; only compiled so far
- Core - AcoustID fingerprints (Chromaprint) for untagged files
- Core - cover art for the singles album

## Sync

- Core - test PREPARE_REQ end to end (needs three devices), and a merged album going through the file phase
- Core - albums and artists without MusicBrainz ids never merge; a playlist or album group that held both copies lists the survivor twice
- Core - changelog compaction must keep the rows that decide merges and archive demotions
- Linux - the sync UI: show the pairing code, a device page with profiles, Sync now, Leave Group (Windows has them)
- Windows and Linux - make a device an archive (mc_sync_set_archive) in the profile editor (Windows already shows the size estimate; Linux's device page needs it too)
- Frontends - run mc_sync_verify(deep) weekly, and show the unknown files an archive reports
- Core - move removed untracked files to the Recycle Bin/trash instead of deleting them permanently (mc_trash() in util/platform.h does it; import's swaps use it)
- Core - a stale copy replaced after another device swapped in a better file (tracks.file_rev) arrives as "<name> (2).<ext>" when the old copy had the same name, and keeps that name
- Android - switch recents to the core's mc_library_record_opened()/mc_library_list_recents(); artists need core uids rather than names
- Android - profile editor done (see ANDROID_PROGRESS.md); untested: the over-budget message, renaming a member, a group renaming the phone, dark theme, and sync on a real device over Wi-Fi

## Other

- Scrobbling - UI on every platform: sign in (mc_scrobbler_auth_begin() opens the browser, then poll mc_scrobbler_auth_finish()), sign out, choose the service (Last.fm, Libre.fm or another URL), show pending plays and errors
- Scrobbling - Android: a JNI bridge for mc_scrobbler, then pass on plays (CoreMediaPlayer's onPlayed) and now playing
- Scrobbling - cancel a call in progress on mc_scrobbler_free(), so quitting while the service is slow doesn't wait for curl's timeout (up to 30 seconds)
- Playback - AAC/M4A - pick a decoder: the free ones are GPL or awkwardly licensed, and the OS codec breaks "no OS codecs"
- Volume control (in the core, so Android can also duck itself on Android 7)
- Spacebar to play/pause
- Recents list above devices
- Linux and Windows - Move Library (the button is disabled)

## Wear OS

- Playback - count plays and scrobble them: CorePlayer's Played event, as the phone does
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
- Import - removed from Android for now (music arrives by sync); bring it back: the UI (header button, folder sheet, progress card), the READ_MEDIA_AUDIO/READ_EXTERNAL_STORAGE permissions and their request flow, and the foreground ImportService (see the Phase 5 commit)
  - It needs a screen to review items one at a time
  - Files MediaStore hasn't indexed didn't appear in the folder sheet
  - The "name: error" format for failed items was hard-coded
- Upgrade Kotlin to 2.4, then Coil and MaterialKolor
- Test - mDNS discovery and sync over real Wi-Fi, API 24-32 devices, audio focus during calls, plays landing in the core's database
- Test - the themed launcher icon, and the sync notification icon
- Search

## Release

- Windows - installer, MSIX/Store package, code signing
- Linux - Flathub (every dependency in third_party/CMakeLists.txt as a Flatpak source), architectures other than x86_64
- Android - enable R8
- Android - third-party license texts in the apps

## Docs

- architecture.md - the playlist module is listed as a stub
- README - the status note and the Mobile UI section are out of date
