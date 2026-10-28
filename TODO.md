# Todo

## Playlists
- Android - smart playlists' art (still their first track's cover; mc_playlist_art() only knows manual playlists)
- Linux - keyboard access to the track menu (Menu key, Shift+F10)
- Playlists with nothing to play on this device are hidden (MC_LIST_PLAYABLE) on the Playlists page of every platform and in Android's Recents; Add to playlist and the sync profile pickers still list them all. Left to do:
  - Windows and Linux - when they show smart playlists, pass MC_LIST_PLAYABLE too (the sidebar's Recents already do)
  - Windows - "Add to playlist → New playlist…" with a track that has no file here makes a playlist that's hidden at once; say so, or grey the menu for such tracks
  - Linux - a playlist made in the Add to playlist dialog and left empty is only reachable from that dialog until a track is added
  - Linux - check the Playlists page under Xvfb once the -std=c11 core fix (68d61bf) and track_list_item_at() fix (3eaa789) are on master
  - Removing a playlist's last playable track from its track list makes the playlist vanish from the grid with the track list closing; consider keeping the open one shown until it's left

## Keyboard Shortcuts

Existing: Space (play/pause), media keys (WM_APPCOMMAND and SMTC on Windows, MPRIS on Linux), Esc closes the track pane, arrows/Home/End on a focused seek bar (5 s), Alt+letter mnemonics in the import page and dialogs (so no bare Alt+letter shortcuts). Windows' track list also has Enter, Delete, F2, Alt+Up/Down and the Menu key.

- Windows and Linux - Ctrl+Right/Ctrl+Left next/previous track (not Alt+Left/Right, which is Back)
- Windows and Linux - Shift+Right/Shift+Left seek 5 s anywhere but text fields, as the seek bar's arrows do
- Windows and Linux - Ctrl+Up/Ctrl+Down volume and Ctrl+M mute, once volume exists
- Windows and Linux - Ctrl+F focus search (both have a Search page now); Windows also Ctrl+E/F3 as in Explorer, Linux also type to search (GNOME)
- Windows and Linux - Ctrl+1/2/3 switch to Albums/Artists/Playlists (sidebar order)
- Windows and Linux - Alt+Left and the mouse's back button go back (album -> grid, device page -> library); Esc keeps closing the pane
- Windows and Linux - Ctrl+O import, Ctrl+N new playlist
- Windows and Linux - Delete on a selected sidebar playlist asks to delete it; F2 renames it
- Linux - Delete, F2, Enter and Alt+Up/Down in the track list, as Windows has
- Linux - Ctrl+, settings, Ctrl+? shortcuts dialog (AdwShortcutsDialog), F1 help/about, Ctrl+W close window, Ctrl+Q quit (GNOME HIG)
- Windows - Ctrl+, settings too (no menu bar, so no Alt+T,O); F1 about; Alt+F4 only to quit
- Windows and Linux - show shortcuts in button tooltips (e.g. "Next (Ctrl+Right)")
- Windows and Linux - test media keys while the window is in the background: SMTC on Windows, MPRIS on GNOME and KDE (the desktop forwards the keys)

## Album Groups

- Windows - done: the Album groups page (2x2 covers from mc_album_group_art()), a group's albums with Back, Add to album group on an album's right-click menu, Remove from album group inside one, Rename and Delete; opening one is recorded in Recents, and search results open them
- Linux and Android - the same UI (the core API is mc_album_group_*() in playlist.h, mc_library_list_album_group_albums())
- Windows - reorder albums in a group (core has no mc_album_group_move() yet), and drag albums onto a group
- Windows - play a whole group (queue its albums in order)
- Windows - the group's header has no tooltips or screen reader names for Back and More, like the rest of the custom-drawn grid

## Smart Playlists/Groups

- UI - rule editor (build it from mc_smart_field_get()), list smart lists alongside playlists and album groups, reshuffle button
- Core - limit by total duration ("an hour of music")

## Artists View

- Core - an artist API; Android derives artists from the album list
- List of Artists - toggle for album artist/ artist?
- Linux and Windows - Artists shows the album grid for now

## Import

- Core - the offline check (mc_musicbrainz_reachable) bounds the connect but not the name lookup, so a network with broken DNS can stall each check; it runs before matching and every 30 s while offline
- Core - duplicates: a swap keeps the library's album and track metadata and files the new file under the old name; the new file's tags come from its own match, so a release-group or by-name swap can leave tags naming another release than the library row
- Core - duplicates: a byte-identical file is only recognised against files that were imported as-is or synced (matched imports are re-tagged in the library, so their bytes differ from the source)
- Linux - check the import outcomes ("Replaced with higher quality", "Already in library", "Added missing tracks") in the running app; only compiled so far
- Core - AcoustID fingerprints (Chromaprint) for untagged files
- Core - cover art for the singles album

## Sync

- Core - test PREPARE_REQ end to end (needs three devices), and a merged album going through the file phase
- Core - albums and artists without MusicBrainz ids never merge; a playlist or album group that held both copies lists the survivor twice
- Core - changelog compaction must keep the rows that decide merges and archive demotions
- Windows - make a device an archive (mc_sync_set_archive) in the profile editor (Linux and Android have it: a switch that asks first, with the size as originals)
- Linux - a pairing request while the window is in the background goes unnoticed (Windows flashes the taskbar button); send a GNotification
- Linux - with no sync.db yet (first run), sync can fail to start with "sync.db open failed: not an error" and Devices says "Sync is off (database error)"; it starts on the next run (seen twice in Docker under Xvfb)
- Linux - sync UI untested: pairing with Windows or Android, the over-budget estimate, a watch's icon, switching library while a device page is open, and a real LAN (only two containers on one Docker network so far)
- Frontends - run mc_sync_verify(deep) weekly, and show the unknown files an archive reports
- Core - move removed untracked files to the Recycle Bin/trash instead of deleting them permanently (mc_trash() in util/platform.h does it; import's swaps use it)
- Core - a stale copy replaced after another device swapped in a better file (tracks.file_rev) arrives as "<name> (2).<ext>" when the old copy had the same name, and keeps that name
- Android - switch recents to the core's mc_library_record_opened()/mc_library_list_recents() (with MC_LIST_PLAYABLE, so playlists with nothing here stay hidden); artists need core uids rather than names
- Android - profile editor done (see ANDROID_PROGRESS.md); untested: the over-budget message, renaming a member, a group renaming the phone, dark theme, and sync on a real device over Wi-Fi

## Recents

- Linux and Windows - artists and smart playlists (and on Linux album groups) are left out of the sidebar's Recents (and opening them isn't recorded) until there are pages to open them on
- Linux and Windows - remove one from Recents, or clear them (mc_library_remove_recent(), mc_library_clear_recents()), e.g. from a context menu
- Linux and Windows - Recents show the page's icon, not the cover art (no sidebar row has art yet)
- Linux and Windows - untested: a recent arriving or going with a real sync between two devices (the refresh after sync is the same one an import uses; a removal was tried by changing the database under the app and reloading)
- Windows - the sidebar is custom-drawn now (it was a list box), so screen readers don't see its rows, like the album grid and track list; give them a UI Automation provider

## Search

- Windows and Linux - artist and smart playlist results (and on Linux album group results) don't open until there are pages for them
- Windows and Linux - Home in the sidebar is greyed out until there's a home page
- Windows - the search page is custom-drawn, so screen readers don't see its results (like the album grid and track list)
- Windows - Up and Down move through the album tiles one at a time, not by row
- Linux - only compiled and launched so far; check it in the running app

## Other


- Scrobbling - UI on every platform: sign in (mc_scrobbler_auth_begin() opens the browser, then poll mc_scrobbler_auth_finish()), sign out, choose the service (Last.fm, Libre.fm or another URL), show pending plays and errors
- Scrobbling - Android: a JNI bridge for mc_scrobbler, then pass on plays (CoreMediaPlayer's onPlayed) and now playing
- Scrobbling - cancel a call in progress on mc_scrobbler_free(), so quitting while the service is slow doesn't wait for curl's timeout (up to 30 seconds)
- Playback - AAC/M4A - pick a decoder: the free ones are GPL or awkwardly licensed, and the OS codec breaks "no OS codecs"
- Volume control (in the core, so Android can also duck itself on Android 7)
- Move Library - the core can't be cancelled part-way (the progress window has no Cancel); copying reports progress per file, so one huge file shows no movement while it copies
- Move Library - Windows: read-only files copy with their attribute, so undoing a failed copy, or removing the old files, can't delete them (they're counted as left behind); clear the attribute first
- Move Library - Windows: a desktop.ini at the top of the library stays in the old folder (it's the folder's own); Linux's .directory isn't treated the same way
- Move Library - Windows: no OneDrive warning for the new folder, as Open another library gives
- Move Library - Linux: close-request is refused while moving, but untested under a real window manager (tested under Xvfb with none)
- Move Library - Android has no Move library (its library lives in app storage)

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
- Import - removed from Android for now (music arrives by sync); bring it back: the UI (header button, folder sheet, progress card), the READ_MEDIA_AUDIO/READ_EXTERNAL_STORAGE permissions and their request flow, and the foreground ImportService (see the Phase 5 commit), plus the offline banner the desktops have (MC_IMPORT_EV_OFFLINE, mc_import_skip_tagging)
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
