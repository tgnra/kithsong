# Todo

## Broad strokes

Big pieces that haven't been started, or that only one platform has.

- **Smart playlists editing** - every platform can list, open, play and reshuffle smart playlists and smart album groups; only Windows can create, edit, rename or delete one (its rule editor, smart_editor.c). Linux and Android need theirs (built from mc_smart_field_get() and the mc_smart_*_label() words)
- **Volume UI** - the core has volume and mute, and the desktops change them from the keyboard (Linux over MPRIS too); no on-screen control anywhere, and Android uses the core's volume only to duck (before Android 8)
- **Home page** - Home in the desktop sidebars is greyed out until it exists
- **Screen readers on Windows** - the album grid, track list, sidebar, search page and now-playing flash have UI Automation providers, checked with UI Automation clients but not yet with Narrator or NVDA; the artist page and the now-playing seek bar are still invisible to it
- **Wear OS** - a prototype: albums and a basic player; sync is a mock-up (SampleSync), no background playback, no playlists or artists; Settings has only the licences
- **Releases** - a portable zip on Windows (no installer or signing) and a Flatpak bundle on Linux (no Flathub listing); Android and Wear OS: signed APKs and AABs, R8 on, licences in Settings

### Parity at a glance

| Feature | Windows | Linux | Android | Wear OS |
|---|---|---|---|---|
| Albums | yes | yes | yes | yes |
| Artists | yes (no art) | yes (no art) | yes | no |
| Playlists: browse | yes | yes | yes | no |
| Playlists: edit | yes | yes | yes | no |
| Album groups | yes | yes | yes | no |
| Smart playlists | yes (rule editor, rename, delete) | open, play, reshuffle (no editor) | open, play, reshuffle (no editor) | no |
| Recents | yes | yes | yes | no |
| Search | yes | yes | yes | no |
| Delete (with restore) | albums, tracks | albums, tracks | albums, tracks (permanent: no trash to restore from) | no |
| Remove from this device | albums, artists | albums, artists (only compiled) | albums, artists | no |
| Shuffle | core's | core's | core's | no |
| Volume | keyboard only | keyboard, MPRIS | system's (core's for ducking) | no |
| Import | yes | yes | yes (simpler review) | no |
| Sync | yes | yes (two instances on one host) | yes | mock-up |
| Merging libraries (pairing) | yes | yes | yes | no |
| Archive devices | yes | yes | yes | no |
| Move library | yes | yes | n/a (app storage) | n/a |
| Scrobbling | yes | yes (fake service only) | yes | no |
| OS media controls | SMTC (with shuffle) | MPRIS (with shuffle and volume) | Media3 session (with shuffle) | none |

## Library and browsing

### Artists
- Windows and Linux - artist pictures

### Album groups
- Android - a new group can only be made from an album's Add to album group… (groups with no album here are hidden, as playlists are)
- Android - Shuffle albums and Shuffle tracks for a group, as the desktops have beside Play
- Android - adding to, removing from or reordering a playlist or group doesn't put it first in the recents; the desktops do, which is what lets a track or album be dragged onto it there
- Windows - albums can't be dragged from the Artists page or search results, nor tracks from search results; only from the album grid and the track list

### Smart playlists
- Linux and Android - rule editor (from mc_smart_field_get()), and rename/delete; Windows has them
- Windows - the editor's date picker (Date added or Last played before or after a date) stays light in dark mode: there's no dark theme for it
- Windows - text conditions don't offer the library's genres and artists as you type
- Windows - the editor only shows a "not" where it means a negative operator (isn't, doesn't contain, isn't in the last, is empty's opposite), and groups one level deep; any other rule opens read-only, as its sentence
- Windows - "months" in the editor are 30 days, and reopen as days (or weeks)
- Windows - New Smart Playlist... and New Smart Album Group... are only on right-click menus (the pages' and the sidebar rows'), as there's no New button for manual playlists either
- Core - mc_smart_describe() is English-shaped: conditions are joined as "field op value" clauses under "Tracks where …", and a language that needs grammatical case or agreement for a field or playlist name may need its own templates; dates use the smart_date template (local time), not the OS's date format
- All - the v10 example lists' names are stored as data, so they're always English
- Windows and Linux - a smart list with nothing playable here is hidden, so its Recents entry or search result can't open it, and on Windows a new smart playlist that picks nothing playable isn't selected after Create (a new smart group still opens)
- Core - limit by total duration ("an hour of music")

### Delete
- Core - a peer that trashed an original after a remote delete keeps no restore record; restoring it there gets it swept as an unknown file
- Android - Remove from this phone carries on playing a track it removes (and skips the queue's others); Windows stops playback first, as Android now does for Delete from library

### Remove from this device
Albums and artists can be removed from one device ("Remove from this PC", computer, phone) and synced to it again; see "Removing from one device" in docs/sync-protocol.md. Left to do:
- All - single tracks: the profile model would take an `exclude_track` kind like the album and artist ones, but no track menu offers it yet
- All - the profile editors (What to sync) don't show exclusions, so with Sync everything nothing lists what was removed from a device, and another device can't undo it; only that device's Sync to this device on the album's or artist's menu can (or, in a selection, ticking it)
- All - an album removed only through its artist shows neither item; Sync to this device on the artist brings it back
- All - an "Add to this device" for an album or artist a selection doesn't include (the menus only offer Sync to this device for what was removed)
- Core - versions before exclusions ignore them: such a device would fetch the item again, and shows a device with exclusions under Sync everything as "changed" (its fingerprint differs)

### Recents
- Windows and Linux - show cover art rather than the page's icon (no sidebar row has art yet)

## Playback

- Core - turning shuffle on or off in a track's last second, once the next track is buffered, changes the order after that track rather than after the current one
- Core - repeat (one, all); no frontend offers it (Ctrl+R is kept for it on the desktops)
- All - a play queue: Play next / Play last (Windows shows them greyed out in the track menu; Linux and Android have neither), and a view of what's queued
- All - an on-screen volume control (the desktops flash the level in the now-playing strip for now)
- Android - resume after the process dies (e.g. Bluetooth play with the app not running), and save the queue across restarts (settings now live in files/settings.json)

### Scrobbling
- All - choose another service (Libre.fm or a custom URL) in the UI; mc_scrobbler_set_service() exists but no screen uses it
- All - revoking access on Last.fm shows "turned down this sign-in" but doesn't sign out by itself

## Import and tagging

- Windows - a missing track's "Choose file… ▾" in the review is cell text with an arrow, not a real dropdown: UI Automation sees the row's text but no ExpandCollapse pattern (the menu opens with a click, Enter or the Menu key)
- Core - joining disc folders is tried once, at the set's first match: if MusicBrainz can't be reached then, the discs stay separate. Keep existing tags on a joined item files every disc under the first disc's album tag ("Title Disc1"), and a manual search on it replaces the "joined" reason with the search's status
- Core - source hashes (files.source_sha256) aren't recorded for files brought in by merging libraries, nor for files imported from inside the library folder
- Core - a release-group swap adds the tracks the library lacks with their own match's tags, so they can name another release than the album row
- Windows - long paths with Windows' long-path setting off: the core prefixes `\\?\` itself, but mc_trash (the shell's Recycle Bin call), a library.db path over 260 and the frontend's own path calls (cover images, the folder picker) still fail
- Android - import (the system picker, or files shared with or opened in the app; no storage permission). Left to do:
  - the review shows the candidates, Keep existing tags, Skip, Search again and Search more widely, but not the track list of a match, other items' files or Use as Track (mc_import_assign), nor the duplicate outcomes (replaced / added missing / already in library)
  - the in-app Import menu still says an import is running; only shares and Open with add to it
  - a folder's zips are batches of their own, so the 32 GB zip cap is per zip there, not per import

## Sync

### Core
- Albums and artists without MusicBrainz ids never merge; a playlist or album group that held both copies lists the survivor twice
- Changelog compaction must keep the rows that decide merges and archive demotions
- A stale copy replaced after another device swapped in a better file (tracks.file_rev) arrives as "<name> (2).<ext>" when the old copy had the same name, and keeps that name

### Merging libraries
Pairing two devices whose libraries both have music merges them (see "Merging libraries" in docs/sync-protocol.md). The larger library is kept; the user confirms on the device whose library is merged, which sets its library aside as library-before-merge-<date>.db, forgets its old group, signs its Last.fm account out if both have one, and after the first session brings its current state (no history) in through the import's duplicates check. Windows, Linux and Android show the proposal, the results, the merge's progress and summary, and the pending note (strings pair_merge_*, paired_merged_*, merge_*). Peers without merging still get library_mismatch, with a hint to update them.

Left in the core:
- A merged file better than the kept device's copy is only attached as the merged device's own original; the archive keeps its worse copy (a swap needs the copy on the device doing it, and the archive's own import is "never stale"). It would need the archive to swap it in, as an import there would
- Quality is compared only with the kept device's originals (MERGE_FILES), from files.format and bitrate_kbps: no sample rate or bit depth, so two lossless files are always equal. A merge resumed after a restart has no MERGE_FILES and attaches every matched track
- Tracks of the merged library with no file on that device (it synced a selection from its old group's archive) are left out unless the kept library has them by mb_releasetrackid; pairing the old group's archive instead brings everything
- The old group's other devices aren't told: they keep the merged device as a member that never answers until they remove it
- Declining on the device that shows the code before the code is typed holds its pairing slot (others get busy) until the other device enters the code or gives up (5 minutes)
- Album folders emptied by trashed duplicates aren't pruned; the set-aside copy is never cleaned up (a user can delete it)
- Recents keep their old opened_ms under a new hlc, so "newest hlc is newest open" no longer holds for those rows
- Originals an archive fetches from another device are stored without a bitrate (transfer.c asks for 0 kbps), so a later merge keeping that archive's library compares quality without it
- Progress counts albums, so one long album holds the bar still (about 8 s for 27 tracks)

Left in the frontends:
- Windows, Linux - after merging, the merged device's profile is unconfirmed like any new member's: its files only move after Sync is pressed (on both devices, for the archive to fetch them). The Paired message already says to choose what syncs and press Sync now; nothing says it again after the merge's summary
- Linux - on the device that started the pairing, after Merge nothing has keyboard focus, so typing the code does nothing until the field is clicked or Tab pressed (on_merge_clicked() does focus it; not yet understood)
- All - "Merge" is the only choice on the merged device: there's no way to merge the other way round (keep the smaller library). Settled by the user, but worth checking it doesn't surprise anyone

### Pairing
- Android - a pairing refused because the phone has music of its own (library mismatch) now only comes from a peer running a version without merging, and the message says to update it; there's still no Create a new library (the library lives in app storage), so updating the other device is the only way out
- Windows and Linux - after Create a new library, Pair again asks by uid, else at the address discovery last saw; a device seen only by address (multicast blocked) on the side that was asked to pair has neither, so Pair again isn't offered there and the user pairs from the Devices list
- Windows and Linux - Create a new library takes only an empty folder; a folder with an empty library already in it (from an earlier try) is refused too, so it's opened with Open another library instead
- All - the archive explainer shows once per device, after it joins a group; a member running an older version doesn't send the archive's name (PAIR_ACCEPT's archive_names), and then nothing is shown (and it stays unshown for a later pairing)
- Windows and Linux - a library mismatch always adds that the other device "may be too old to merge", also when both run this version and one side is in a group with another library than its group's open (can_merge() in pairing.c). Only that side knows why: the other needs PAIR_REFUSE to say
- All - after pairing, the dialog no longer opens the other device's page by itself; Choose what to sync opens the page of the device that joined (this one's own when it joined). Check this feels right

### Frontends
- All - run mc_sync_verify(deep) weekly, and show the unknown files an archive reports
- Windows and Linux - the empty Playlists page's Choose what to sync opens this computer's page but doesn't scroll to or focus its What to sync section
- Windows and Linux - an empty library in a group (nothing imported anywhere yet) just says "Use Import to add music."; Android's says to choose what to sync, with a button to this phone's page

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
The desktops' shortcuts are listed in README.md (Linux also shows them with Ctrl+?), and buttons' tooltips name them. Left to do:
- Windows and Linux - Ctrl+R repeat, once the core has repeat
- Linux - Delete and F2 on a selected smart playlist, once Linux can rename and delete smart playlists at all (Windows can)

### Windows screen readers and custom-drawn controls
The album grid, track list, sidebar and search page have UI Automation providers (uia.c) that raise name and status changes as property-change events; the now-playing strip is a live region for its flash ("Volume 40%"); the track list's and album grid's header buttons are reachable from the keyboard. Still to do:
- The artist page (kithsong_artists) is custom-drawn without a provider, and the now-playing strip exposes only the flash or the track's name: no seek bar or times

## Wear OS

- Playback - a foreground MediaSessionService, so the system doesn't kill playback in the background and the watch's media controls work
- Playback - route to Bluetooth headphones and prompt to connect them, rather than the watch speaker
- Playback - progress and seeking on the player screen
- Playback - a way back to the player from the home screen
- Playback - count plays and scrobble them: CorePlayer's Played event, as the phone does
- Sync - drive the sync screen from the core instead of SampleSync
- Sync - request a Wi-Fi network while syncing; Wear OS keeps Wi-Fi off while the phone is connected over Bluetooth
- Sync - sync in the background, not only while the sync screen is open
- Artists, Playlists, and more in Settings (it has only Open source licences)

## Testing and verification

Things built but not checked properly. Debug hooks for test scripts: KITHSONG_DEBUG_PAIR=<host>:<port> on Windows and `--es debug_pair <host>:<port>` on Android pair two scratch instances without clicks; `--es fake_merge <scenario>` on Android shows one of the merge's screens with made-up data (Syncer.debugShowMerge).

### Linux (run under Xvfb, never on a real desktop)
Run on Ubuntu 24.04 (GTK 4.14, libadwaita 1.5) under Xvfb, mostly with openbox, audio to a PulseAudio null sink, and sync between two instances on one host; earlier runs were in Docker (GTK 4.18, libadwaita 1.7). Seen working there: smart lists' and groups' pictures, empty playlists staying, artist page and smart list playback, album group covers, the Playlists page, shuffle and the Play/Shuffle buttons, delete and restore (a delete taking a queued track stops playback first), the import outcomes, Use as Track with the keyboard and with related items' files, Search More Widely, the over-budget estimates, switching library with a device page open, the archive's sidebar row, the empty pages' buttons, the pairing dialog's ended page, Remove from / Sync to this computer (tiles, artist page, artist rows), the Add to playlist note, merging libraries in both roles, Move library's refused close under openbox, and the AdwShortcutsDialog (built against the GNOME 49 SDK). Left:
- A real GNOME and KDE session (Wayland too) for all of the above
- Sync: pairing with Windows or Android, and a real LAN. A watch's icon was only seen with a core-only stand-in: this Adwaita has no smartwatch-symbolic, so it showed the phone's
- Merging libraries: the library_mismatch update hint (needs a peer running an older version) and the merge's notification
- Restore from a real file manager's Trash (only done by hand: file moved back, .trashinfo removed)
- Scrobbling with a real Last.fm account (the whole flow ran against a fake service). With no browser, Sign in fails silently and the row says "Waiting…" with no link to copy
- Import: Search More Widely while offline. After a Use as Track choice, focus goes back to the first row rather than the next missing track, and the Choose file menu's items have no accessible names (nor have other popover menus' on GTK 4.14)
- The track list's long press (no touch device here)
- The notification while the window is in the background: seen with stand-in notification servers (both protocols), its action bringing the window up with the code, and going when the window is active, the request ends or the dialog closes. Not seen: the merge's summary (openbox focuses the merge's window, so it's rightly skipped), real GNOME Shell and KDE bubbles, and the portal
- Move library's refused close under mutter or KWin; the AdwShortcutsDialog in a real Flatpak build of the app
- Removing an album while it plays lets the track play to its end, then stops; the bar then keeps the removed track, with Play doing nothing. The artist page's album tiles have no menu (singles' do)
- An Albums page whose every album has one track (all singles) says "No music yet"
- The archive's own sidebar row ("This computer. Keeps the whole library") is cut off at the default width
- An album tile stays highlighted after its right-click menu closes, until the page reloads (maybe GTK 4.14)
- "Recently added" plays each album last track first (each track is added a moment after the one before)

### Windows
- Import review's Choose file menu with files of related items (grouped under "Grouped separately: …"): only tried with the item's own unmatched files
- Firewall prompt: the success path after UAC, beyond one run
- Remove from this PC / Sync to this PC in a real group (the core is tested between two devices; on screen only the menu outside a group was looked at), including removing an album while it plays
- Deleting a track while it plays
- Volume's fade (only silent test files so far)
- The UI Automation providers with a real screen reader (Narrator, NVDA): only checked with System.Windows.Automation tree walks, patterns, focus events, and listeners for property changes and live regions
- The empty Playlists page's Choose what to sync (needs this PC in a group with every playlist hidden), pairing from the empty Albums page's menu, and the empty-page button through UI Automation
- The pairing dialog's ended page with a real pairing: Choose what to sync, the archive explainer (and the dialog growing to fit it), Create a new library and Pair again
- Merging libraries: the progress window was only seen for an instant (a three-track test merge finishes at once), and the pending note after a restart was never seen (both seen on Linux); the library_mismatch update hint (it needs a peer running an older version) has been seen nowhere

### Android and Wear OS
- Smart playlists' and groups' pictures (mc_smart_art()), and empty playlists in the Playlists section and Recents, on screen (only built)
- Remove from this phone / Sync to this phone on screen, in a real group (only built)
- A recent's long-press menu (Play, Shuffle, Remove from Recents, Clear Recents) on screen (only built; the desktops' menus were run)
- Profile editor: the over-budget message, renaming a member, a group renaming the phone, dark theme
- After pairing: Choose what to sync opening the device's page, and the one-time archive explainer (only built)
- Merging libraries: tested end to end on the emulator with a Windows scratch instance (both roles, Merge and decline, Last.fm sign-out, summary). Seen only with fake_merge: the progress card and bar, the forgotten-devices line, the merge_pending note, the failed summary and the library_mismatch hint; the merge's notification was never seen
- Search, only on the emulator with a tiny library
- The one-time handover of the old prefs recents to the core
- mDNS discovery and sync over real Wi-Fi, on a real device
- API 24-32 devices, audio focus during calls, ducking (only API 24-25 duck through the core's volume; the emulator is API 37), plays landing in the core's database
- The themed launcher icon, and the sync notification icon
- Wear OS - the Settings and licences screens are only built, never run (no Wear OS emulator here): check they scroll with the crown, the long texts' monospaced paragraphs are readable, and a licence opened from the list shows its name

### Cross-platform
- A real Last.fm sign-in and scrobble (only fake sessions so far), and the sign-in reaching another device over a real sync
- Media keys with the window in the background: SMTC on Windows, MPRIS on GNOME and KDE. MPRIS was only checked with gdbus and playerctl under Xvfb (play/pause, next/previous, seek, Raise, Shuffle, Volume, clamping, and PropertiesChanged, with the window unfocused or minimized), not with GNOME's or KDE's controls
- Recents arriving or going with a real sync between two devices
- An Android delete reaching a desktop over a real sync (copies removed there, originals to its trash)
- A delete and a restore going through a real sync
- Sync PREPARE_REQ end to end (needs three devices), and a merged album going through the file phase

## Release

- Windows - installer, MSIX/Store package, code signing
- Linux - Flathub (every dependency in third_party/CMakeLists.txt as a Flatpak source; the Flatpak manifest fetches them at build time, which Flathub forbids), architectures other than x86_64
