# Todo

## Broad strokes

Big pieces that haven't been started, or that only one platform has.

- **Smart playlists UI** - the core is done (rules, preview, reshuffle); no platform can create, edit or open one. Needs a rule editor (built from mc_smart_field_get()), a page to open one on, and listing alongside playlists and album groups
- **Album groups on Linux and Android** - only Windows has the UI
- **Artist pages on Linux and Android** - Linux shows the album grid under Artists; Android derives artists from the album list instead of the core's artist API
- **Playlist editing on Android** - the JNI bridge only lists playlists; no create, rename, delete, add, reorder or remove
- **Search on Android** - the desktops have a Search page; Android has none
- **Import on Android** - removed for now (music arrives by sync); see Android below
- **Scrobbling UI** - the core can scrobble, but no platform can sign in, and Android has no JNI bridge for it
- **Volume control** - nowhere yet; belongs in the core so Android can duck itself on Android 7
- **Home page** - Home in the desktop sidebars is greyed out until it exists
- **Keyboard shortcuts** - beyond Space, media keys and Esc, the desktops have almost none (see Input)
- **Screen readers on Windows** - the album grid, track list, sidebar and search page are custom-drawn and invisible to UI Automation
- **AAC/M4A playback** - no decoder chosen
- **Wear OS** - a prototype: albums and a basic player; sync is a mock-up (SampleSync), no background playback, no playlists, artists or settings
- **Releases** - no Windows installer or signing, no Flathub listing, Android without R8 or license texts
- **Translations** - strings/en.json is the only language

### Parity at a glance

| Feature | Windows | Linux | Android | Wear OS |
|---|---|---|---|---|
| Albums | yes | yes | yes | yes |
| Artists | yes (no art, no menu) | album grid stand-in | own derivation | no |
| Playlists: browse | yes | yes | yes | no |
| Playlists: edit | yes | yes | no | no |
| Album groups | yes | no | no | no |
| Smart playlists | no | no | no | no |
| Recents | yes (no smart playlists) | albums and playlists only | own implementation | no |
| Search | yes | yes (unchecked) | no | no |
| Shuffle | core's | core's | Media3's, not the core's | no |
| Import | yes | yes | removed | no |
| Sync | yes | yes (barely tested) | yes | mock-up |
| Archive devices | no | yes | yes | no |
| Move library | yes | yes | n/a (app storage) | n/a |
| Scrobbling | no UI | no UI | no bridge | no |
| OS media controls | SMTC (no shuffle) | MPRIS (no shuffle) | Media3 session | none |

## Library and browsing

### Artists
- Core - a track has one artist, so "A & B" or "A feat. B" credits only that combined artist; crediting each needs a track_artists table (and tag parsing on import)
- All - a toggle between album artist and track artist in the artist list?
- Android - switch to mc_library_list_artists() and the artist page's mc_library_list_artist_albums()/_tracks()
- Linux - an artist page (Artists shows the album grid for now)
- Windows - artist pictures, and a context menu (Add to playlist of all their tracks, say)

### Playlists
- Android - smart playlists' art (still their first track's cover; mc_playlist_art() only knows manual playlists)
- Playlists with nothing to play on this device are hidden (MC_LIST_PLAYABLE) on every Playlists page and in Android's Recents; Add to playlist and the sync profile pickers still list them all. Left to do:
  - Windows and Linux - when they show smart playlists, pass MC_LIST_PLAYABLE too (the sidebar's Recents already do)
  - Windows - "Add to playlist → New playlist…" with a track that has no file here makes a playlist that's hidden at once; say so, or grey the menu for such tracks
  - Linux - a playlist made in the Add to playlist dialog and left empty is only reachable from that dialog until a track is added
  - All - removing a playlist's last playable track makes the playlist vanish from the grid and its track list close; consider keeping the open one shown until it's left

### Album groups
- Linux and Android - the UI Windows has (core API: mc_album_group_*() in playlist.h, mc_library_list_album_group_albums())
- Core - mc_album_group_move(), so albums in a group can be reordered
- Windows - reorder albums in a group, and drag albums onto a group
- Windows - play a whole group (queue its albums in order)

### Smart playlists
- All - rule editor (from mc_smart_field_get()), a page to open one on, list them alongside playlists and album groups, reshuffle button
- Core - limit by total duration ("an hour of music")

### Recents
- Linux - artists, album groups and smart playlists are left out (and opening them isn't recorded) until there are pages for them
- Windows - smart playlists are left out (and opening them isn't recorded) until there's a page for them
- Android - switch to the core's mc_library_record_opened()/mc_library_list_recents() (with MC_LIST_PLAYABLE); artists need core uids rather than names
- Windows and Linux - remove one, or clear them all (mc_library_remove_recent(), mc_library_clear_recents()), e.g. from a context menu
- Windows and Linux - show cover art rather than the page's icon (no sidebar row has art yet)

### Search
- Android - a Search page
- Windows and Linux - smart playlist results don't open until there's a page for them; nor do artist and album group results on Linux
- Windows - Up and Down move through album tiles one at a time, not by row

### Home
- Windows and Linux - a home page (Home in the sidebar is greyed out until then)

## Playback

- Core - volume control, so Android can duck itself on Android 7
- Core - AAC/M4A: pick a decoder; the free ones are GPL or awkwardly licensed, and the OS codec breaks "no OS codecs"
- Core - turning shuffle on or off in a track's last second, once the next track is buffered, changes the order after that track rather than after the current one
- Android - use the core's shuffle (the toggle, Play/Shuffle buttons and the setting)
- Android - resume after the process dies (e.g. Bluetooth play with the app not running), and save the queue across restarts
- Linux - MPRIS's Shuffle property, so the desktop's media controls show and set it
- Windows - the media flyout's shuffle button (SMTC IsShuffleEnabled and ShuffleEnabledChangeRequested)

### Scrobbling
- All - UI: sign in (mc_scrobbler_auth_begin() opens the browser, then poll mc_scrobbler_auth_finish()), sign out, choose the service (Last.fm, Libre.fm or another URL), show pending plays and errors
- Android - a JNI bridge for mc_scrobbler, then pass on plays (CoreMediaPlayer's onPlayed) and now playing
- Core - cancel a call in progress on mc_scrobbler_free(), so quitting while the service is slow doesn't wait for curl's timeout (up to 30 seconds)

## Import and tagging

- Core - AcoustID fingerprints (Chromaprint) for untagged files
- Core - cover art for the singles album
- Core - the offline check (mc_musicbrainz_reachable) bounds the connect but not the name lookup, so a network with broken DNS can stall each check; it runs before matching and every 30 s while offline
- Core - duplicates: a swap keeps the library's album and track metadata and files the new file under the old name; the new file's tags come from its own match, so a release-group or by-name swap can leave tags naming another release than the library row
- Core - duplicates: a byte-identical file is only recognised against files that were imported as-is or synced (matched imports are re-tagged in the library, so their bytes differ from the source)
- Android - bring import back:
  - the UI (header button, folder sheet, progress card), the READ_MEDIA_AUDIO/READ_EXTERNAL_STORAGE permissions and their request flow, and the foreground ImportService (see the Phase 5 commit)
  - the offline banner the desktops have (MC_IMPORT_EV_OFFLINE, mc_import_skip_tagging)
  - a screen to review items one at a time
  - files MediaStore hasn't indexed didn't appear in the folder sheet
  - the "name: error" format for failed items was hard-coded

## Sync

### Core
- Albums and artists without MusicBrainz ids never merge; a playlist or album group that held both copies lists the survivor twice
- Changelog compaction must keep the rows that decide merges and archive demotions
- Move removed untracked files to the Recycle Bin/trash instead of deleting them (mc_trash() in util/platform.h does it; import's swaps use it)
- A stale copy replaced after another device swapped in a better file (tracks.file_rev) arrives as "<name> (2).<ext>" when the old copy had the same name, and keeps that name

### Frontends
- All - run mc_sync_verify(deep) weekly, and show the unknown files an archive reports
- Windows - make a device an archive (mc_sync_set_archive) in the profile editor, as Linux and Android do (a switch that asks first, with the size as originals)
- Linux - a pairing request while the window is in the background goes unnoticed (Windows flashes the taskbar button); send a GNotification
- Linux - with no sync.db yet (first run), sync can fail to start with "sync.db open failed: not an error" and Devices says "Sync is off (database error)"; it starts on the next run (seen twice in Docker under Xvfb)

### Windows firewall
- After "Don't ask again", a way to ask again (Settings, or the device page's Connections row when this PC is blocked)
- Offer it when the network is public too (today it stays quiet unless Private is uncovered, and only says so after adding the rule)
- A stale rule left for an old exe path (portable zip moved) is never cleaned up
- The installer, once there is one, should add the rule (program-scoped, Private/Domain, as firewall.c does) and remove it on uninstall, so the prompt never shows

## Move library

- Core - can't be cancelled part-way (the progress window has no Cancel); progress is per file, so one huge file shows no movement while it copies
- Windows - read-only files copy with their attribute, so undoing a failed copy, or removing the old files, can't delete them (they're counted as left behind); clear the attribute first
- Windows - a desktop.ini at the top of the library stays in the old folder (it's the folder's own); Linux's .directory isn't treated the same way
- Windows - no OneDrive warning for the new folder, as Open another library gives

## Input and accessibility

### Keyboard shortcuts

Existing: Space (play/pause), media keys (WM_APPCOMMAND and SMTC on Windows, MPRIS on Linux), Esc closes the track pane, arrows/Home/End on a focused seek bar (5 s), Alt+letter mnemonics in the import page and dialogs (so no bare Alt+letter shortcuts). Windows' track list also has Enter, Delete, F2, Alt+Up/Down and the Menu key.

Windows and Linux:
- Ctrl+Right/Ctrl+Left next/previous track (not Alt+Left/Right, which is Back)
- Shift+Right/Shift+Left seek 5 s anywhere but text fields, as the seek bar's arrows do
- Ctrl+Up/Ctrl+Down volume and Ctrl+M mute, once volume exists
- Ctrl+F focus search; Windows also Ctrl+E/F3 as in Explorer, Linux also type to search (GNOME)
- Ctrl+1/2/3 switch to Albums/Artists/Playlists (sidebar order)
- Alt+Left and the mouse's back button go back (album -> grid, device page -> library); Esc keeps closing the pane
- Ctrl+O import, Ctrl+N new playlist
- Delete on a selected sidebar playlist asks to delete it; F2 renames it
- Show shortcuts in button tooltips (e.g. "Next (Ctrl+Right)")

Linux:
- Delete, F2, Enter and Alt+Up/Down in the track list, as Windows has
- The track menu from the keyboard (Menu key, Shift+F10)
- Ctrl+, settings, Ctrl+? shortcuts dialog (AdwShortcutsDialog), F1 help/about, Ctrl+W close window, Ctrl+Q quit (GNOME HIG)

Windows:
- Ctrl+, settings too (no menu bar, so no Alt+T,O); F1 about; Alt+F4 only to quit

### Windows screen readers and custom-drawn controls
- A UI Automation provider for the custom-drawn controls: album grid, track list, sidebar (a list box until recently) and search page
- The track list's Play and Shuffle buttons: no keyboard access either
- The album group header's Back and More: no tooltips or accessible names

## Android

- Upgrade Kotlin to 2.4, then Coil and MaterialKolor
- Everything listed under Broad strokes: playlist editing, album groups, artist pages, search, import, the core's shuffle and recents, scrobbling

## Wear OS

- Playback - a foreground MediaSessionService, so the system doesn't kill playback in the background and the watch's media controls work
- Playback - route to Bluetooth headphones and prompt to connect them, rather than the watch speaker
- Playback - progress and seeking on the player screen
- Playback - a way back to the player from the home screen
- Playback - count plays and scrobble them: CorePlayer's Played event, as the phone does
- Sync - drive the sync screen from the core instead of SampleSync
- Sync - request a Wi-Fi network while syncing; Wear OS keeps Wi-Fi off while the phone is connected over Bluetooth
- Sync - sync in the background, not only while the sync screen is open
- Artists, Playlists, Settings

## Testing and verification

Things built but not checked properly.

### Linux (mostly only compiled, or run under Xvfb)
- Search page in the running app
- Shuffle toggle and the track list's Play/Shuffle buttons
- Import outcomes ("Replaced with higher quality", "Already in library", "Added missing tracks")
- Playlists page, now the -std=c11 core fix (68d61bf) and track_list_item_at() fix (3eaa789) are in
- Sync: pairing with Windows or Android, the over-budget estimate, a watch's icon, switching library while a device page is open, and a real LAN (only two Docker containers so far)
- Move library's refused close-request under a real window manager

### Windows
- Firewall prompt: the success path after UAC, beyond one run

### Android
- Profile editor: the over-budget message, renaming a member, a group renaming the phone, dark theme
- mDNS discovery and sync over real Wi-Fi, on a real device
- API 24-32 devices, audio focus during calls, plays landing in the core's database
- The themed launcher icon, and the sync notification icon

### Cross-platform
- Media keys with the window in the background: SMTC on Windows, MPRIS on GNOME and KDE
- Recents arriving or going with a real sync between two devices
- Sync PREPARE_REQ end to end (needs three devices), and a merged album going through the file phase

## Release

- Windows - installer, MSIX/Store package, code signing
- Linux - Flathub (every dependency in third_party/CMakeLists.txt as a Flatpak source), architectures other than x86_64
- Android - enable R8
- Android - third-party license texts in the apps
- All - translations (strings/en.json is the only language)

## Docs

- architecture.md - the playlist module is listed as a stub
- README - the status note ("early scaffold… otherwise the frontends show an empty window") and the Mobile UI section are out of date
