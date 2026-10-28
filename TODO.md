# Todo

## Broad strokes

Big pieces that haven't been started, or that only one platform has.

- **Smart playlists editing** - every platform can list, open, play and reshuffle smart playlists and smart album groups; none can create, edit, rename or delete one. Needs a rule editor (built from mc_smart_field_get())
- **Import on Android** - removed for now (music arrives by sync); see Android below
- **Volume UI** - the core has volume and mute, and the desktops change them from the keyboard; no on-screen control anywhere, and Android doesn't duck with it yet
- **Home page** - Home in the desktop sidebars is greyed out until it exists
- **Screen readers on Windows** - the album grid, track list, sidebar and search page have UI Automation providers, checked with a UI Automation tree walk but not yet with Narrator or NVDA; the artist page and the now-playing seek bar are still invisible to it
- **AAC/M4A playback** - no decoder chosen
- **Merging libraries** - two devices that both have music can't pair (library_mismatch); see Sync → Merging libraries below
- **Wear OS** - a prototype: albums and a basic player; sync is a mock-up (SampleSync), no background playback, no playlists, artists or settings
- **Releases** - no Windows installer or signing, no Flathub listing (Android: R8 on, licences in Settings; the watch has no licences screen yet)
- **Translations** - strings/en.json is the only language

### Parity at a glance

| Feature | Windows | Linux | Android | Wear OS |
|---|---|---|---|---|
| Albums | yes | yes | yes | yes |
| Artists | yes (no art) | yes (no art) | yes (matched by name) | no |
| Playlists: browse | yes | yes | yes | no |
| Playlists: edit | yes | yes | yes | no |
| Album groups | yes | yes | yes (no reorder) | no |
| Smart playlists | open, play, reshuffle (no editor) | open, play, reshuffle (no editor) | open, play, reshuffle (no editor) | no |
| Recents | yes | yes | yes (core's) | no |
| Search | yes | yes | yes | no |
| Delete (with restore) | yes | yes | yes (permanent: no trash to restore from) | no |
| Shuffle | core's | core's | core's | no |
| Volume | keyboard only | keyboard only | core only, no UI | no |
| Import | yes | yes | removed | no |
| Sync | yes | yes (barely tested) | yes | mock-up |
| Archive devices | yes | yes | yes | no |
| Move library | yes | yes | n/a (app storage) | n/a |
| Scrobbling | yes | yes (UI untested on screen) | yes | no |
| OS media controls | SMTC (no shuffle) | MPRIS (no shuffle) | Media3 session (with shuffle) | none |

## Library and browsing

### Artists
- Core - a track has one artist, so "A & B" or "A feat. B" credits only that combined artist; crediting each needs a track_artists table (and tag parsing on import)
- Core - a per-device artist listing (album and track counts here) and artist uids on albums and tracks, so Android stops matching artists by name (two artists of the same name are both listed if either has something here)
- All - a toggle between album artist and track artist in the artist list?
- Windows and Linux - artist pictures; Windows - Add to playlist of all their tracks from the artist menu

### Playlists
- Android and the desktops - smart playlists' art (still their first track's cover or a placeholder; mc_playlist_art() only knows manual playlists)
- Playlists with nothing to play on this device are hidden (MC_LIST_PLAYABLE) on every Playlists page and in Recents; Add to playlist and the sync profile pickers still list them all. Left to do:
  - Windows - "Add to playlist → New playlist…" with a track that has no file here makes a playlist that's hidden at once; say so, or grey the menu for such tracks
  - Linux - a playlist made in the Add to playlist dialog and left empty is only reachable from that dialog until a track is added
  - Windows and Linux - removing a playlist's last playable track makes the playlist vanish from the grid and its track list close; consider keeping the open one shown until it's left (Android closes the page)
  - Windows and Linux - a playlist made with the Playlists page's + stays shown (and selected) only until another page is picked; after that an empty one is reachable from Add to playlist alone
- Android - a new playlist can only be made from Add to playlist (empty ones are hidden there); the desktops' Playlists page has a +
- Android - when every playlist is hidden for having nothing to play here, the Playlists section just isn't there; the desktops say so and link to Choose what to sync
- Windows - the Playlists and Album groups headers' + can't be reached with the keyboard (as the group header's buttons); Ctrl+N (see Keyboard shortcuts) would do
- Android - drag-to-reorder only scrolls while the finger is moving
- Android - failed edits are only logged; show a message
- All - multi-select in track lists, so several tracks can be deleted or added to a playlist at once

### Album groups
- Android - a new group can only be made from an album's Add to album group… (groups with no album here are hidden, as playlists are); failed edits are only logged
- Core - mc_album_group_move(), so albums in a group can be reordered (then Android too)
- Windows - reorder albums in a group, and drag albums onto a group
- Windows and Linux - play a whole group, smart groups too (queue its albums in order; Android has Play)

### Smart playlists
- All - rule editor (from mc_smart_field_get()), and rename/delete
- All - the v10 example lists' names are stored as data, so they're always English
- Windows and Linux - a smart list with nothing playable here is hidden, so its Recents entry or search result can't open it
- Core - limit by total duration ("an hour of music")

### Delete
- Linux - mc_trash() fails for a library on another file system from the home Trash; support `$topdir/.Trash-$uid`, or offer a permanent delete
- Core - a peer that trashed an original after a remote delete keeps no restore record; restoring it there gets it swept as an unknown file
- Windows and Linux - a "Restored N tracks" message after a restore from the bin
- Android - delete from an album's page or the player too (only the album, artist and track long-press menus have it)
- Android - a track that's deleted while it plays keeps playing to its end, and its later queue entries are skipped; the desktops stop playback first (Windows can't trash an open file)

### Recents
- Windows and Linux - remove one, or clear them all (mc_library_remove_recent(), mc_library_clear_recents()), e.g. from a context menu; Android too
- Windows and Linux - show cover art rather than the page's icon (no sidebar row has art yet)
- Android - the one-time handover of the old prefs recents to the core is untested

### Search
- Windows - Up and Down move through album tiles one at a time, not by row
- Android - only tested on the emulator with a tiny library

### Home
- Windows and Linux - a home page (Home in the sidebar is greyed out until then)

## Playback

- Core - AAC/M4A: pick a decoder; the free ones are GPL or awkwardly licensed, and the OS codec breaks "no OS codecs"
- Core - turning shuffle on or off in a track's last second, once the next track is buffered, changes the order after that track rather than after the current one
- Core - a public cover-file lookup (mc_coverart_find) so frontends don't look for cover.jpg/png themselves
- All - an on-screen volume control (the desktops flash the level in the now-playing strip for now)
- Android - duck with the core's volume (CorePlayer.setVolume; AudioFocusManager's setVolumeMultiplier is a no-op)
- Android - resume after the process dies (e.g. Bluetooth play with the app not running), and save the queue across restarts (settings now live in files/settings.json)
- Linux - MPRIS's Shuffle and Volume properties, so the desktop's media controls show and set them
- Windows - the media flyout's shuffle button (SMTC IsShuffleEnabled and ShuffleEnabledChangeRequested)

### Scrobbling
- All - choose another service (Libre.fm or a custom URL) in the UI; mc_scrobbler_set_service() exists but no screen uses it
- All - revoking access on Last.fm shows "turned down this sign-in" but doesn't sign out by itself
- Core - cancel a call in progress on mc_scrobbler_free(), so quitting (or cancelling a sign-in, or closing the library during one) doesn't wait for curl's timeout (up to 30 seconds)

## Import and tagging

- Core - AcoustID fingerprints (Chromaprint) for untagged files
- Core - cover art for the singles album
- Core - the offline check (mc_musicbrainz_reachable) bounds the connect but not the name lookup, so a network with broken DNS can stall each check; it runs before matching and every 30 s while offline
- Core - duplicates: a swap keeps the library's album and track metadata and files the new file under the old name; the new file's tags come from its own match, so a release-group or by-name swap can leave tags naming another release than the library row
- Core - duplicates: a byte-identical file is only recognised against files that were imported as-is or synced (matched imports are re-tagged in the library, so their bytes differ from the source)
- All - Use as Track (mc_import_assign) works only within an import session: once Done is clicked the session's matches are gone, so an album already in the library can't be fixed that way. It would need the release's track list kept (or looked up again by mb_albumid) and a library-side "assign this file to that track"
- Core - Use as Track doesn't recompute the match's confidence, and rewriting an already imported item skips the duplicates check (its files are in the library), so a file brought in from outside the library replaces a library copy at the same path rather than being compared for quality
- Core - extended search (docs/tagging-rules.md): not for singles, so a one-file album whose album tag equals its title (a DJ mix, e.g. "BREAK IT OLDSCHOOL [SRPC-L007]") gets only recording candidates; it would need release candidates for a single item
- Core - grouping: an album tag with the disc in it ("危ないベースライン Disc1", "… Disc2") makes one item per disc folder; the folder name's title (search_keys.c) could join them
- Windows - paths over MAX_PATH: the manifest isn't longPathAware, so TagLib can't open (and the copy can't read) files whose full path is 260 characters or more; they're left out of the import without a word
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
- Move removed untracked files to the Recycle Bin/trash instead of deleting them (mc_trash() in util/platform.h does it); deleted tracks' originals on peers already go to the trash, unknown files are still removed outright
- A stale copy replaced after another device swapped in a better file (tracks.file_rev) arrives as "<name> (2).<ext>" when the old copy had the same name, and keeps that name

### Merging libraries
Pairing two devices whose libraries both have music fails with library_mismatch. Instead, offer to merge: keep one library (its database, uids and changelog) and import the other device's music into it, as an import would, rather than merging the two changelogs. Syncing two changelogs would hit the duplicate rules' edges: a delete in one library kills the other's copy of that album, merge classes that never split join a once-mismatched album to the wrong release, metadata mixes field by field, and both archives would claim the whole library. Edge cases to settle:
- Which library is kept: the archive's, or the larger one's? Let the user choose
- Music without MusicBrainz ids (imported with Keep existing tags or without tagging) has nothing to match on; import it as tagged, and send albums the library already has through the import's duplicates check (higher quality replaces, same or lower is skipped)
- Playlists, album groups, play counts and recents of the library that isn't kept: carry them over by mapping its tracks to the kept library's (a playlist of the same name: keep both, or merge?)
- Its own sync group: its other devices must leave and pair again, or join with it
- Two Last.fm accounts: unclear what's best; at least don't sign either out silently. Plays not yet scrobbled stay with the account they were played under
- Files: originals copy over the network to the kept library's archive, then the other device drops them as its profile says

### Frontends
- All - run mc_sync_verify(deep) weekly, and show the unknown files an archive reports
- Windows and Linux - the empty Playlists page's Choose what to sync opens this computer's page but doesn't scroll to or focus its What to sync section
- Windows and Linux - an empty library in a group (nothing imported anywhere yet) just says "Use Import to add music."; it could say the music arrives from the group, or link to Choose what to sync
- Android - the empty main screen always says to pair, even when the phone is already in a group and its profile holds nothing; say "choose what to sync" there instead
- Linux - a pairing request while the window is in the background goes unnoticed (Windows flashes the taskbar button); send a GNotification
- Linux - with no sync.db yet (first run), sync can fail to start with "sync.db open failed: not an error" and Devices says "Sync is off (database error)"; it starts on the next run (seen several times in Docker under Xvfb)

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

Existing (see README.md): Space, media keys, Esc, Ctrl+Left/Right next/previous, Shift+Left/Right seek 5 s, Ctrl+Up/Down and +/- volume, Ctrl+M mute, Ctrl+S shuffle, Ctrl+F search, Ctrl+1-4 Albums/Artists/Playlists/Album groups, Ctrl+, settings, Alt+Left/Backspace/mouse back to go back, plus Alt+letter mnemonics in the import page and dialogs. Windows' track list also has Enter, Delete, F2, Alt+Up/Down and the Menu key.

Windows and Linux:
- Ctrl+R repeat, once the core has repeat
- Windows also Ctrl+E/F3 for search as in Explorer, Linux also type to search (GNOME)
- Ctrl+O import, Ctrl+N new playlist
- Delete on a selected sidebar playlist asks to delete it; F2 renames it
- Show shortcuts in button tooltips (e.g. "Next (Ctrl+Right)")

Linux:
- Delete, F2, Enter and Alt+Up/Down in the track list, as Windows has
- The track menu from the keyboard (Menu key, Shift+F10)
- Ctrl+? shortcuts dialog (AdwShortcutsDialog), F1 help/about, Ctrl+W close window, Ctrl+Q quit (GNOME HIG)

Windows:
- F1 about; Alt+F4 only to quit

### Windows screen readers and custom-drawn controls
The album grid, track list, sidebar and search page have UI Automation providers (uia.c), and the track list's Play and Shuffle are reachable from the keyboard. Still to do:
- The artist page (kithsong_artists) is still custom-drawn without a provider, and the now-playing strip only has its title as its name: no seek bar or times
- Name changes (a renamed album, a device going offline, "Playing" moving) aren't raised as property-change events; screen readers pick them up when they next look
- The album group header's Back and More, and the track list's playlist menu and close buttons, can't be reached with the keyboard (Backspace, F2, the menu key and Escape do the same)
- Live regions for the now-playing flash ("Volume 40%", "Shuffle on")

## Android

- Upgrade Kotlin to 2.4, then Coil and MaterialKolor
- Import (see above)

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
- Settings - an "Open source licences" screen like the phone's; the watch APK already carries the texts (assets/licenses/)

## Testing and verification

Things built but not checked properly.

### Linux (mostly only compiled, or run under Xvfb)
- Shuffle toggle and the track list's Play/Shuffle buttons
- Artist page playback with real files, and album group covers
- Smart list playback (no audio in the container)
- The Settings scrobbling group on screen
- Delete and restore in the running app
- Import outcomes ("Replaced with higher quality", "Already in library", "Added missing tracks")
- Import review's Use as Track N (multi-select track list, right-click menu, files of related items): written without a GTK build at hand, not even compiled yet
- The review's Search More Widely button (only compiled)
- Playlists page, now the -std=c11 core fix (68d61bf) and track_list_item_at() fix (3eaa789) are in
- Sync: pairing with Windows or Android, the over-budget estimate, a watch's icon, switching library while a device page is open, and a real LAN (only two Docker containers so far)
- Move library's refused close-request under a real window manager
- An archive's sidebar row ("Offline. Keeps the whole library" under its name), only compiled
- The empty Albums page's Pair with a device… menu (opening it, pairing a device picked there, Add device by address…), the empty Playlists page's Choose what to sync, and the Album groups page's +: only compiled; under Xvfb the button showed, and the Playlists + made a playlist that stayed shown until another page was picked

### Windows
- Firewall prompt: the success path after UAC, beyond one run
- Deleting a track while it plays
- Volume's fade (only silent test files so far)
- The UI Automation providers with a real screen reader (Narrator, NVDA): only checked with System.Windows.Automation tree walks, patterns and focus events
- The empty Playlists page's Choose what to sync (needs this PC in a group with every playlist hidden), pairing from the empty Albums page's menu, and the new header buttons and empty-page button through UI Automation

### Android
- Profile editor: the over-budget message, renaming a member, a group renaming the phone, dark theme
- mDNS discovery and sync over real Wi-Fi, on a real device
- API 24-32 devices, audio focus during calls, plays landing in the core's database
- The themed launcher icon, and the sync notification icon

### Cross-platform
- A real Last.fm sign-in and scrobble (only fake sessions so far), and the sign-in reaching another device over a real sync
- Media keys with the window in the background: SMTC on Windows, MPRIS on GNOME and KDE
- Recents arriving or going with a real sync between two devices
- An Android delete reaching a desktop over a real sync (copies removed there, originals to its trash)
- A delete and a restore going through a real sync
- Sync PREPARE_REQ end to end (needs three devices), and a merged album going through the file phase

## Release

- Windows - installer, MSIX/Store package, code signing
- Linux - Flathub (every dependency in third_party/CMakeLists.txt as a Flatpak source), architectures other than x86_64
- Android - the release workflow keeps only the APKs and AABs, so the R8 mapping files the scripts put in dist/ don't reach the GitHub release (the AABs carry them for Play)
- All - translations (strings/en.json is the only language)

## Docs

- architecture.md - the playlist module is listed as a stub
- README - the status note ("early scaffold… otherwise the frontends show an empty window") and the Mobile UI section are out of date
