# Todo

## Broad strokes

Big pieces that haven't been started, or that only one platform has.

- **Smart playlists editing** - every platform can list, open, play and reshuffle smart playlists and smart album groups; only Windows can create, edit, rename or delete one (its rule editor, smart_editor.c). Linux and Android need theirs (built from mc_smart_field_get() and the mc_smart_*_label() words)
- **Import on Android** - removed for now (music arrives by sync); see Android below
- **Volume UI** - the core has volume and mute, and the desktops change them from the keyboard; no on-screen control anywhere, and Android doesn't duck with it yet
- **Home page** - Home in the desktop sidebars is greyed out until it exists
- **Screen readers on Windows** - the album grid, track list, sidebar and search page have UI Automation providers, checked with a UI Automation tree walk but not yet with Narrator or NVDA; the artist page and the now-playing seek bar are still invisible to it
- **AAC/M4A playback** - no decoder chosen
- **Merging libraries** - core, Windows, Linux and Android merge two libraries with music when pairing; see Sync → Merging libraries below
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
| Smart playlists | yes (rule editor, rename, delete) | open, play, reshuffle (no editor) | open, play, reshuffle (no editor) | no |
| Recents | yes | yes | yes (core's) | no |
| Search | yes | yes | yes | no |
| Delete (with restore) | yes | yes | yes (permanent: no trash to restore from) | no |
| Remove from this device | albums, artists | albums, artists (only compiled) | albums, artists | no |
| Shuffle | core's | core's | core's | no |
| Volume | keyboard only | keyboard, MPRIS | core only, no UI | no |
| Import | yes | yes | removed | no |
| Sync | yes | yes (barely tested) | yes | mock-up |
| Merging libraries (pairing) | yes | yes (not built or run yet) | yes | no |
| Archive devices | yes | yes | yes | no |
| Move library | yes | yes | n/a (app storage) | n/a |
| Scrobbling | yes | yes (UI untested on screen) | yes | no |
| OS media controls | SMTC (with shuffle) | MPRIS (with shuffle and volume) | Media3 session (with shuffle) | none |

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
- Linux and Android - rule editor (from mc_smart_field_get()), and rename/delete; Windows has them
- Windows - the editor's date picker (Date added or Last played before or after a date) stays light in dark mode: there's no dark theme for it
- Windows - text conditions don't offer the library's genres and artists as you type
- Windows - the editor only shows a "not" where it means a negative operator (isn't, doesn't contain, isn't in the last, is empty's opposite), and groups one level deep; any other rule opens read-only, as its sentence
- Windows - "months" in the editor are 30 days, and reopen as days (or weeks)
- Windows - the editor's live counts run one query per top-level condition after each change (on its own thread and connection); a core mc_smart_count() would save materialising every track
- Windows - the editor doesn't lay itself out again when it moves to a monitor with another DPI
- Windows - New Smart Playlist... and New Smart Album Group... are only on right-click menus (the pages' and the sidebar rows'), as there's no New button for manual playlists either
- Core - mc_smart_describe() is English-shaped: conditions are joined as "field op value" clauses under "Tracks where …", and a language that needs grammatical case or agreement for a field or playlist name may need its own templates; dates use the smart_date template (local time), not the OS's date format
- All - the v10 example lists' names are stored as data, so they're always English
- Windows and Linux - a smart list with nothing playable here is hidden, so its Recents entry or search result can't open it, and on Windows a new smart playlist that picks nothing playable isn't selected after Create (a new smart group still opens)
- Core - limit by total duration ("an hour of music")

### Delete
- Linux - mc_trash() fails for a library on another file system from the home Trash; support `$topdir/.Trash-$uid`, or offer a permanent delete
- Core - a peer that trashed an original after a remote delete keeps no restore record; restoring it there gets it swept as an unknown file
- Windows and Linux - a "Restored N tracks" message after a restore from the bin
- Android - delete from an album's page or the player too (only the album, artist and track long-press menus have it)
- Android - a track that's deleted while it plays keeps playing to its end, and its later queue entries are skipped; the desktops stop playback first (Windows can't trash an open file)

### Remove from this device
Albums and artists can be removed from one device ("Remove from this PC", computer, phone) and synced to it again; see "Removing from one device" in docs/sync-protocol.md. Left to do:
- All - single tracks: the profile model would take an `exclude_track` kind like the album and artist ones, but no track menu offers it yet
- All - the profile editors (What to sync) don't show exclusions, so with Sync everything nothing lists what was removed from a device, and another device can't undo it; only that device's Sync to this device on the album's or artist's menu can (or, in a selection, ticking it)
- All - an album removed only through its artist shows neither item; Sync to this device on the artist brings it back
- All - an "Add to this device" for an album or artist a selection doesn't include (the menus only offer Sync to this device for what was removed)
- Core - versions before exclusions ignore them: such a device would fetch the item again, and shows a device with exclusions under Sync everything as "changed" (its fingerprint differs)

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
- All - a play queue: Play next / Play last (Windows shows them greyed out in the track menu; Linux and Android have neither), and a view of what's queued
- All - an on-screen volume control (the desktops flash the level in the now-playing strip for now)
- Android - duck with the core's volume (CorePlayer.setVolume; AudioFocusManager's setVolumeMultiplier is a no-op)
- Android - resume after the process dies (e.g. Bluetooth play with the app not running), and save the queue across restarts (settings now live in files/settings.json)

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
- Windows - a missing track's "Choose file… ▾" in the review is cell text with an arrow, not a real dropdown: UI Automation sees the row's text but no ExpandCollapse pattern (the menu opens with a click, Enter or the Menu key)
- Core - Use as Track doesn't recompute the match's confidence, and rewriting an already imported item skips the duplicates check (its files are in the library), so a file brought in from outside the library replaces a library copy at the same path rather than being compared for quality
- Core - extended search (docs/tagging-rules.md): not for singles, so a one-file album whose album tag equals its title (a DJ mix, e.g. "BREAK IT OLDSCHOOL [SRPC-L007]") gets only recording candidates; it would need release candidates for a single item
- Core - grouping: an album tag with the disc in it ("危ないベースライン Disc1", "… Disc2") makes one item per disc folder; the folder name's title (search_keys.c) could join them
- Windows - paths over MAX_PATH: the manifest isn't longPathAware, so TagLib can't open (and the copy can't read) files whose full path is 260 characters or more; they're left out of the import without a word
- Android - import (SAF, no storage permission) is back; still missing:
  - the review shows the candidates, Keep existing tags, Skip, Search again and Search more widely, but not the track list of a match, other items' files or Use as Track (mc_import_assign), nor the duplicate outcomes (replaced / added missing / already in library)
  - an import cut short by the process dying isn't resumed: the next start deletes the copies and releases the folder's grant
  - sharing files while an import runs leaves them out (only logged)
  - a folder's .zip files are imported as batches of their own, but zips nested inside zips aren't extracted
- Core - zips: only paths given to mc_import_start are extracted (a zip inside a dropped folder isn't), and the Info-ZIP Unicode Path extra field (0x7075) isn't read, so a CP437-flagged entry with a UTF-8 name in that field comes out as CP437

## Sync

### Core
- Albums and artists without MusicBrainz ids never merge; a playlist or album group that held both copies lists the survivor twice
- Changelog compaction must keep the rows that decide merges and archive demotions
- Move removed untracked files to the Recycle Bin/trash instead of deleting them (mc_trash() in util/platform.h does it); deleted tracks' originals on peers already go to the trash, unknown files are still removed outright
- A stale copy replaced after another device swapped in a better file (tracks.file_rev) arrives as "<name> (2).<ext>" when the old copy had the same name, and keeps that name

### Merging libraries
Pairing two devices whose libraries both have music now merges them instead of failing with library_mismatch (core; see "Merging libraries" in docs/sync-protocol.md and musiccore/sync.h). Settled: the larger library is kept (tracks, then bytes held, then the older library uid), the user only confirms on the device whose library is merged; that device forgets its old group (its paired devices must pair again), signs its Last.fm account out if both have one (one try to send its queue, the rest dropped; if only it has one, it becomes the group's), copies its library aside as library-before-merge-<date>.db in its library folder, empties it and joins; after the first session its current state (no history) comes in through the import's duplicates check, with playlists ("Name (2)" on a clash: both kept), album groups, smart lists, play counts and recents mapped onto the kept tracks; its files become originals imported there, which the archive fetches. Peers without merging, and frontends that leave on_pair_merge NULL, still get library_mismatch.

Left in the core:
- A merged file better than the kept device's copy is only attached as the merged device's own original; the archive keeps its worse copy (a swap needs the copy on the device doing it, and the archive's own import is "never stale"). It would need the archive to swap it in, as an import there would
- Quality is compared only with the kept device's originals (MERGE_FILES), from files.format and bitrate_kbps: no sample rate or bit depth, so two lossless files are always equal. A merge resumed after a restart has no MERGE_FILES and attaches every matched track
- Tracks of the merged library with no file on that device (it synced a selection from its old group's archive) are left out unless the kept library has them by mb_releasetrackid; pairing the old group's archive instead brings everything
- The old group's other devices aren't told: they keep the merged device as a member that never answers until they remove it
- Declining on the device that shows the code before the code is typed holds its pairing slot (others get busy) until the other device enters the code or gives up (5 minutes)
- Album folders emptied by trashed duplicates aren't pruned; the set-aside copy is never cleaned up (a user can delete it)
- Recents keep their old opened_ms under a new hlc, so "newest hlc is newest open" no longer holds for those rows

#### For the UI agents (Windows, Linux, Android)
Windows and Linux (pair_dialog.c and sync_bridge.c on each, the pending note in device_view.c) and Android (jni_bridge.c, CoreSync.kt, Syncer.kt, DevicesUi.kt, MergeUi.kt) are done. The strings are in strings/en.json's shared section (pair_merge_*, paired_merged_*, merge_*), except the duplicates-moved line, which is per platform (windows.merge_tracks_recycled, linux.merge_tracks_trashed, android.merge_tracks_deleted: Android has no trash, so the core deletes them).
- [x] Windows, Linux - set mc_sync_events.on_pair_merge, on_merge_progress and on_merge_done (until on_pair_merge is set, merging stays off and pairing two libraries with music still says library mismatch)
- [x] Windows, Linux - on_pair_merge: the proposal in the pairing dialog on both devices. Merged device: which library is kept (track counts here and there), that this one is saved as a backup and replaced, the Last.fm account signed out (scrobble_user), the paired devices forgotten (forget_names), Merge and Cancel. The enter-code dialog shows it in place of the code until answered (title "Merge libraries?", Pair reads Merge), then goes back to the code with "The libraries will be merged once the devices are paired." The show-code dialog shows it under the code, with Merge beside Cancel; Cancel declines (mc_sync_pair_merge_confirm(0)) and closes the dialog. Kept device: a note under the code or the code entry ("... once <device> agrees")
- [x] Windows, Linux - on_pair_result: MC_PAIR_MERGE_DECLINED says "The libraries weren't merged, so the devices weren't paired." (not shown on the device whose user declined); with merged, the Paired message says whose library was kept and which Last.fm account was signed out
- [x] Windows, Linux - on_merge_progress and on_merge_done: a window of its own that doesn't disable the main one (albums done of total, then playlists and play counts, one bar), turning into a summary: tracks added, improved, already in the library, duplicates moved to the Recycle Bin/Trash, left out, failed; playlists carried over and renamed; Last.fm signed out and plays dropped; the backup's path; an error stated plainly. Closing it while it runs keeps it closed until the summary
- [x] Windows, Linux - mc_sync_merge_pending(): this device's own page says "Music from your old library is merged into this one after the next sync with the group." until it has run
- [x] Windows, Linux - library_mismatch now only comes from a device without merging (an older version): the message adds "Kithsong on <device> may be too old to merge libraries. Update it, then pair again to merge them.", and Create a new library stays
- [x] Android - all of the above. The JNI bridge sets the three callbacks (forget_names as a String[], mc_merge_result's counts as an int[]) and exposes pairMergeConfirm and mergePending. The pairing dialog: the proposal in place of the code entry until answered (Merge libraries?, Merge / Cancel), or under the code shown (Merge beside Cancel; Cancel declines and closes it, and the result that follows isn't shown, nor is one on an initiator that declined); the proposal that comes before on_pair_code waits in Syncer for the code; the kept-device note. Paired with merged and MergeDeclined as on the desktops; library mismatch adds the update hint (no Create a new library on Android). The merge runs in a card under the main screen's header, as an import does, and in the sync notification (the service holds sync while it runs), then shows the summary until Done. merge_pending is sync's notice (in a group) and a line on this phone's page
- Windows, Linux - after merging, the merged device's profile is unconfirmed like any new member's: its files only move after Sync is pressed (on both devices, for the archive to fetch them). The Paired message already says to choose what syncs and press Sync now; nothing says it again after the merge's summary
- Windows, Linux - the merge's progress window was only seen for an instant: a three-track test merge finishes at once, so the bar and the phase lines were never on screen long enough to check. The pending note after a restart wasn't seen either (the merge always ran before the app could be stopped), nor was the library_mismatch hint (it needs a peer running an older version)
- Linux - written to match the GTK code but not built or run here (no GTK on the Windows machine this was done on): build it and walk through the scenarios Windows was tested with
- Android - tested end to end on the emulator with a Windows scratch instance (both roles, Merge and decline, Last.fm sign-out, summary). Seen only with made-up data (debug builds: `--es fake_merge <scenario>`, see Syncer.debugShowMerge): the progress card and bar (a three-track merge ends at once), the merge's notification (not seen at all), the forgotten-devices line, the merge_pending note, the failed summary and the library_mismatch hint. Debug builds also take `--es debug_pair <host>:<port>` to pair at startup, as KITHSONG_DEBUG_PAIR does
- Windows - debug builds take KITHSONG_DEBUG_PAIR=<host>:<port> to ask that device to pair shortly after startup, so test scripts can pair two scratch instances over localhost without clicks
- All - "Merge" is the only choice on the merged device: there's no way to merge the other way round (keep the smaller library). Settled by the user, but worth checking it doesn't surprise anyone

### Pairing
- Android - a pairing refused because the phone has music of its own (library mismatch) now only comes from a peer running a version without merging, and the message says to update it; there's still no Create a new library (the library lives in app storage), so updating the other device is the only way out
- Windows and Linux - after Create a new library, Pair again asks by uid, else at the address discovery last saw; a device seen only by address (multicast blocked) on the side that was asked to pair has neither, so Pair again isn't offered there and the user pairs from the Devices list
- Windows and Linux - Create a new library takes only an empty folder; a folder with an empty library already in it (from an earlier try) is refused too, so it's opened with Open another library instead
- All - the archive explainer shows once per device, after it joins a group; a member running an older version doesn't send the archive's name (PAIR_ACCEPT's archive_names), and then nothing is shown (and it stays unshown for a later pairing)
- All - after pairing, the dialog no longer opens the other device's page by itself; Choose what to sync opens the page of the device that joined (this one's own when it joined). Check this feels right

### Frontends
- All - run mc_sync_verify(deep) weekly, and show the unknown files an archive reports
- Windows and Linux - the empty Playlists page's Choose what to sync opens this computer's page but doesn't scroll to or focus its What to sync section
- Windows and Linux - an empty library in a group (nothing imported anywhere yet) just says "Use Import to add music."; it could say the music arrives from the group, or link to Choose what to sync
- Android - the empty main screen always says to pair, even when the phone is already in a group and its profile holds nothing; say "choose what to sync" there instead
- Linux - with no sync.db yet (first run), sync can fail to start with "sync.db open failed: not an error" and Devices says "Sync is off (database error)"; it starts on the next run (seen several times in Docker under Xvfb)

### Windows firewall
- After "Don't ask again", a way to ask again (Settings, or the device page's Connections row when this PC is blocked)
- Offer it when the network is public too (today it stays quiet unless Private is uncovered, and only says so after adding the rule)
- A stale rule left for an old exe path (portable zip moved) is never cleaned up
- The installer, once there is one, should add the rule (program-scoped, Private/Domain, as firewall.c does) and remove it on uninstall, so the prompt never shows

## Move library

- Core - can't be cancelled part-way (the progress window has no Cancel); progress is per file, so one huge file shows no movement while it copies
- Windows - a desktop.ini at the top of the library stays in the old folder (it's the folder's own); Linux's .directory isn't treated the same way

## Input and accessibility

### Keyboard shortcuts

Existing (see README.md): Space, media keys, Esc, Ctrl+Left/Right next/previous, Shift+Left/Right seek 5 s, Ctrl+Up/Down and +/- volume, Ctrl+M mute, Ctrl+S shuffle, Ctrl+F search, Ctrl+1-4 Albums/Artists/Playlists/Album groups, Ctrl+, settings, Alt+Left/Backspace/mouse back to go back, plus Alt+letter mnemonics in the import page and dialogs. The track list also has Enter, Delete, F2, Alt+Up/Down and the Menu key (or Shift+F10).

Windows and Linux:
- Ctrl+R repeat, once the core has repeat
- Windows also Ctrl+E/F3 for search as in Explorer, Linux also type to search (GNOME)
- Ctrl+O import, Ctrl+N new playlist
- Delete on a selected sidebar playlist asks to delete it; F2 renames it
- Show shortcuts in button tooltips (e.g. "Next (Ctrl+Right)")

Linux:
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
- Import review's Use as Track: compiled, and run under Xvfb in Docker (GTK 4.18, libadwaita 1.7) with the mouse: a missing track's Choose File… menu (from a click on its row, Enter, or a right-click), Ctrl+click selecting without opening it, and the multi-select right-click Use as Track N all assign. Not checked: choosing inside the menu with the keyboard (keys didn't reach the popover under Xvfb), files of related items, and a real desktop session
- The review's Search More Widely button (only compiled)
- Playlists page, now the -std=c11 core fix (68d61bf) and track_list_item_at() fix (3eaa789) are in
- Sync: pairing with Windows or Android, the over-budget estimate, a watch's icon, switching library while a device page is open, and a real LAN (only two Docker containers so far)
- Move library's refused close-request under a real window manager
- An archive's sidebar row ("Offline. Keeps the whole library" under its name), only compiled
- The empty Albums page's Pair with a device… menu (opening it, pairing a device picked there, Add device by address…), the empty Playlists page's Choose what to sync, and the Album groups page's +: only compiled; under Xvfb the button showed, and the Playlists + made a playlist that stayed shown until another page was picked
- The pairing dialog's ended page (only compiled): Choose what to sync, the archive explainer's bold "archive" (Pango markup), Create a new library (the folder picker, refusing a folder that isn't empty, then Pair again) and the button styles
- Remove from this computer / Sync to this computer on the album tiles' and the artist page's menus (only compiled, in Docker)
- The notification while the window is in the background (only compiled), of a pairing request, a refusal shown before any code, and the merge's summary: that it shows on GNOME and KDE, that clicking it brings the window up with the code or message (the app.show-pair-request action; the merge's window over it), and that it goes once the window is active, the request ends, or its dialog or the merge's window closes
- The track list's keys and the artist list's row menu: run under Xvfb in Docker (GTK 4.18, libadwaita 1.7). Seen working: Alt+Up/Down, Delete and F2 in a playlist, Enter, the Menu key and Shift+F10 (the menu shows Alt+Up, Alt+Down, Delete and F2 beside its items, and they don't act outside the list), right-click and the Menu key on an artist row (its Delete from library... names that artist), and the Menu key in an artist page's tracks and elsewhere on the page. Not checked: the long press (no touchscreen), the Menu key with no track selected (the playlist's header menu) or with the selected track scrolled out of sight, Remove from this computer / Sync to this computer on an artist row (needs a group), and a real desktop session

### Windows
- Import review's Choose file menu with files of related items (grouped under "Grouped separately: …"): only tried with the item's own unmatched files
- Firewall prompt: the success path after UAC, beyond one run
- Remove from this PC / Sync to this PC in a real group (the core is tested between two devices; on screen only the menu outside a group was looked at), including removing an album while it plays
- Deleting a track while it plays
- Volume's fade (only silent test files so far)
- The UI Automation providers with a real screen reader (Narrator, NVDA): only checked with System.Windows.Automation tree walks, patterns and focus events
- The empty Playlists page's Choose what to sync (needs this PC in a group with every playlist hidden), pairing from the empty Albums page's menu, and the new header buttons and empty-page button through UI Automation
- The pairing dialog's ended page with a real pairing: Choose what to sync, the archive explainer (and the dialog growing to fit it), Create a new library and Pair again

### Android
- Remove from this phone / Sync to this phone on screen, in a real group (only built)
- Profile editor: the over-budget message, renaming a member, a group renaming the phone, dark theme
- After pairing: Choose what to sync opening the device's page, and the one-time archive explainer (only built)
- mDNS discovery and sync over real Wi-Fi, on a real device
- API 24-32 devices, audio focus during calls, plays landing in the core's database
- The themed launcher icon, and the sync notification icon

### Cross-platform
- A real Last.fm sign-in and scrobble (only fake sessions so far), and the sign-in reaching another device over a real sync
- Media keys with the window in the background: SMTC on Windows, MPRIS on GNOME and KDE. MPRIS's Shuffle and Volume were only checked with gdbus under Xvfb in Docker (get, set, clamping, and PropertiesChanged for changes from MPRIS and from the keyboard), not with GNOME's or KDE's controls
- Recents arriving or going with a real sync between two devices
- An Android delete reaching a desktop over a real sync (copies removed there, originals to its trash)
- A delete and a restore going through a real sync
- Sync PREPARE_REQ end to end (needs three devices), and a merged album going through the file phase

## Release

- Windows - installer, MSIX/Store package, code signing
- Linux - Flathub (every dependency in third_party/CMakeLists.txt as a Flatpak source), architectures other than x86_64
- All - translations (strings/en.json is the only language)
