# Todo

What's left to build, fix and check. Platform first, then the item. "Decided:" marks a choice already made.

## For 1.0

- **Home page** - Home in the desktop sidebars stays greyed out until it exists
- **Repeat and a play queue** - Core: repeat (one, all). All: Play next / Play last (Windows shows them greyed out in the track menu; Linux and Android have neither) and a view of what's queued. Windows and Linux: Ctrl+R for repeat (kept free for it)

## Release

- Windows - Store submission (scripts/package-msix.ps1 packs an unsigned MSIX; the Store signs it), a winget manifest for the website to show, and the portable zip under the website's Other downloads. Decided: no traditional installer
- Windows - the MSIX declares the firewall rule (the desktop2 windows.firewallRules extension, Private/Domain, as firewall.c adds it), so a Store install never shows the prompt; the zip keeps the prompt
- Linux - Flathub. Decided: yes. Every dependency in third_party/CMakeLists.txt becomes a Flatpak source in packaging/flatpak/app.kithsong.Kithsong.yml (it fetches them at build time today, which Flathub forbids); then architectures other than x86_64

## Library and browsing

- Windows and Linux - Recents show cover art rather than the page's icon (no sidebar row has art yet)
- Windows - an all-singles Albums page says "No music yet"; use Linux's "No albums" text, which points to the artists' Singles and search (move its strings from the linux section to shared)
- Windows - albums can't be dragged from the Artists page or search results, nor tracks from search results
- Windows - search doesn't filter album groups, so a group hidden as empty here can appear in results and open with its albums greyed
- Linux - the empty Album groups page has no Choose what to sync button, as Windows has
- Android - Android Auto's Home can list an empty playlist from Recents; its queue items now expose art through ArtProvider, untested there
- Android - the main page has no predictive back preview, as the handler has to stay registered so back can quit
- Windows - the edit dialog: no way to unlink an album from MusicBrainz (mc_album_tags_set() with "" does it), no Alt+letter keys for its fields and buttons, and Save can't be cancelled once it's started; the search opens with focus on Artist, so the results are three Tabs away
- Linux - the edit dialog: no way to unlink an album from MusicBrainz; search results have no menu, so no Edit… there; the Menu key on an artist page's album tile opens the artist's menu
- Android - artist pages have no menu button, so Edit… is only reached by long-pressing the artist in a list
- All - no way back to the automatic cover once one is chosen (it would clear art_chosen), and a replaced cover is deleted rather than sent to the Recycle Bin
- Windows - the media controls (SMTC) keep the old thumbnail until the next track after a new cover
- Linux - the now-playing strip doesn't take a cover that arrives by sync; playlist and album group composite pictures weren't checked after a cover change
- Windows - search results have no right-click menu, so no Edit... there; nor do the album tiles on an artist's page have an album menu of their own (Edit... there is the album's, the rest the artist's)
- Windows and Linux - after an edit, what's queued and the now-playing strip keep the old title and artist until the track changes, and a moved file further down the queue is stale (Android reloads the queue; a core call swapping paths without stopping would suit all three, and spare Android's possible half-second jump back)
- All - tracks imported before 81dcc23 keep a timestamp each, a moment apart, so "Recently added" plays those albums last track first (no migration: the column syncs)

- All - a continuous mix's tracks still show on the artist's page track list (mc_library_list_artist_tracks())
- All - nothing shows that an album is a continuous mix: mc_album_info doesn't carry the flags, so a frontend would have to ask mc_album_flags_get() per album
- Core - a play queued for scrobbling before its album was flagged no_scrobble still goes (scrobble_queue keeps what to send, not the track)
- Core - a mix made from a loose track and unmade again stays a single on its one-track album; a track filed at the upgrade to v16 keeps its file in <artist>/ until it's next imported; a long recording matched on MusicBrainz but on no release stays loose at import
- All - a chosen selection (not Sync everything) drops a phone's own imports outside it once the archive has them: intended, but it may surprise people, so say so somewhere

### Hand edits
- Core - no way to cancel an edit or a release search under way, nor to see how far it is: an artist with many albums copies every file first, and mc_release_search() can wait minutes while MusicBrainz is busy (mb_call has the cancel; edit.h has no way to pass one)
- Core - renaming an artist is one swap of every file credited, so it needs that much free space; per album would need less, but a failure could leave it half done
- Core - merging two artists by hand (renaming one to the other's name leaves two)
- Core - a folder an album moves into that has a cover keeps it, so the album's own goes with its old folder and its art_hash no longer matches; only for two albums sharing a folder
- Core - disc_total is only ever raised by a track's disc number
- Windows - when a folder's name changes only in case, files take the new case in their rows and the folder keeps the old one
- All - the player could open files sharing delete, so tagging the track playing wouldn't need MC_ERR_IN_USE on Windows (dr_libs' _w openers use _wfopen)
- Decided: peers' files keep their old tags and places; the rows are what they show, and fetching audio again for a tag isn't worth it

### Smart playlists
- All - localise the v10 example lists' names (decided; they're stored as data, so always English today)
- Windows and Linux - a smart list whose picks are all away from this device is hidden, so a search result can't open it; on Linux a new one like that doesn't open after it's made. Decided: one that picks nothing in the whole library shows, as empty playlists do
- Linux - move a condition or group to another group, or up and down within one (today it's deleted and made again)
- Linux - text conditions could offer the library's genres and artists as you type
- Linux - the editor only edits a "not" that means a negative operator, or "none of"; any other rule opens read-only, as its sentence
- Core - limit by total duration ("an hour of music")
- Core - mc_smart_describe() is English-shaped ("field op value" clauses under "Tracks where …"): languages with grammatical case or agreement may need their own templates; dates use the smart_date template, not the OS's date format

### Delete
- Core - a peer that trashed an original after a remote delete keeps no restore record; restoring it there gets it swept as an unknown file

### Remove from this device
See "Removing from one device" in docs/sync-protocol.md.
- All - What to sync shows Sync everything as everything ticked except what was removed (decided). Store it as mode `all` plus exclusions, so later imports still sync; unticking an album there adds an exclusion. Today the editors mark such rows Removed and ticking one brings it back, but Sync everything doesn't list them
- Core - a ticked playlist keeps its tracks on the device even when their album or artist was removed, until the playlist is unticked (decided; today an exclusion wins over a playlist). Update the wanted set in docs/sync-protocol.md. The removal says when some tracks stay because of a playlist
- All - an album removed only through its artist shows neither item; Sync to this device on the artist brings it back
- All - an "Add to this device" for an album or artist a selection doesn't include (the menus only offer Sync to this device for what was removed)
- Core - versions before exclusions ignore them: such a device fetches the item again, and shows a device with exclusions under Sync everything as "changed"
- Decided: no removal of single tracks, as there's no syncing of single tracks

## Playback

- Core - turning shuffle on or off in a track's last second, once the next track is buffered, changes the order after that track rather than after the current one
- Android - resume after the process dies (e.g. Bluetooth play with the app not running), and save the queue across restarts
- Android - a paused track's place is kept when back quits, but only while the process lives (the item above would cover it)
- All - revoking access on Last.fm shows "turned down this sign-in" but doesn't sign out by itself

## Import and tagging

- Core - joining disc folders is tried once, at the set's first match: if MusicBrainz can't be reached then, the discs stay separate. Keep existing tags on a joined item files every disc under the first disc's album tag ("Title Disc1"), and a manual search on it replaces the "joined" reason with the search's status
- Core - source hashes (files.source_sha256) aren't recorded for files brought in by merging libraries, nor for files imported from inside the library folder
- Core - a release-group swap adds the tracks the library lacks with their own match's tags, so they can name another release than the album row
- Windows - long paths with Windows' long-path setting off (decided: support them throughout). The core prefixes `\\?\` itself, but mc_trash (the shell's Recycle Bin call), a library.db path over 260 and the frontend's own path calls (cover images, the folder picker) still fail
- Windows - a missing track's "Choose file… ▾" in the review is cell text with an arrow: UI Automation sees no ExpandCollapse pattern
- Android - the review lacks a match's track list, other items' files, Use as Track (mc_import_assign) and the duplicate outcomes (replaced / added missing / already in library)
- Android - the in-app Import menu still says an import is running; only shares and Open with add to it
- Android - a folder's zips are batches of their own, so the 32 GB zip cap is per zip there, not per import

## Sync

### Core
- Opus: decide on complexity 7 at 128 and 160 kbps after a listening test (75x to 101x realtime); libopus runs constrained VBR while the docs say VBR; cpu_score is wall-clock based and cached, so one taken under load is skewed
- Trace: send and recv lines use 8 uid characters, only the timestamp, so they can't name a file
- The plan's size estimate for originals could use the size in the peer's file row, making early ETAs better; the scaling covers for it for now
- A disc number on one track renames the album's other files only at a later save (seen on Android: they gained a `1-` prefix then). Check against "A disc number past the album's last" in docs/tagging-rules.md
- Albums and artists without MusicBrainz ids never merge, and a playlist or album group that held both copies lists the survivor twice. Decided: match them fuzzily (by name and artist), in sync's duplicates and when merging libraries
- Sync profile "artist" selections pick tracks by `tracks.artist_uid` and the album's artist only, not featured credits (`track_artists` / `album_artists` past the first): selecting Rihanna doesn't sync "Jay-Z feat. Rihanna". A selection of an old whole-credit artist selects nothing once the refresh after upgrading has split and deleted it
- The version vector (mc_changelog_version_vector) scans the whole changelog every time: about 0.3 s at 400k rows (a ~23k-track library) on a desktop, more on a phone or watch. An index on (origin_uid, hlc), with a max(hlc) per origin, makes it instant
- A stale copy replaced after another device swapped in a better file (tracks.file_rev) arrives as "<name> (2).<ext>" when the old copy had the same name, and keeps that name
- Tracks waiting for a device (mc_sync_get_waiting()) guess who holds what from profiles and who imported each track, so a member still short of its own profile is named as if it had them. A member that went away soon after a complete session is only noticed when the next attempt runs (up to MC_SYNC_AUTO_INTERVAL_MS)
- Changelog compaction, keeping the rows that decide merges and archive demotions. Not urgent: a real 786-track library's changelog is 13.6k rows (3 MB of 4.4 MB) with only 5 superseded, so it would free almost nothing

### Verifying files
- All - run mc_sync_verify(deep) weekly while on mains power (a desktop always; a laptop, phone or watch on the charger)
- All - problems show as a warning card at the top of the library, like the waiting-for-a-device one, including the unknown files an archive reports
- All - on an archive, files missing for good get a button that removes those tracks from the library. First fetch any that another member still has, since removing deletes them on every device

### Merging libraries
See "Merging libraries" in docs/sync-protocol.md.
- Core - a merged file better than the kept device's copy is only attached as the merged device's own original; the archive keeps its worse copy. The archive would need to swap it in, as an import there would
- Core - quality is compared from files.format and bitrate_kbps only, so two lossless files are always equal (no sample rate or bit depth). A merge resumed after a restart has no MERGE_FILES and attaches every matched track
- Core - originals an archive fetches from another device are stored without a bitrate (transfer.c asks for 0 kbps), so a later merge compares quality without it
- Core - tracks of the merged library with no file on that device are left out unless the kept library has them by mb_releasetrackid
- Core - the old group's other devices aren't told: they keep the merged device as a member that never answers until they remove it
- Core - declining on the device that shows the code before the code is typed holds its pairing slot for up to 5 minutes
- Core - album folders emptied by trashed duplicates aren't pruned; the set-aside library-before-merge-<date>.db is never cleaned up
- Core - Recents keep their old opened_ms under a new hlc, so "newest hlc is newest open" no longer holds for those rows
- Core - progress counts albums, so one long album holds the bar still
- All - nothing warns beforehand that restarting or leaving the group before Sync is pressed undoes a waiting merge. Linux's own-page banner offers Sync now while the merge waits; Windows and Android only say so in merge_pending's text
- Windows and Linux - after the merge's summary, nothing says again to choose what syncs and press Sync now
- Linux - on the device that started the pairing, after Merge nothing has keyboard focus, so typing the code does nothing until the field is clicked or Tab pressed (on_merge_clicked() does focus it; not understood yet)

### Pairing
- Android - there's no Create a new library (the library lives in app storage), so a library mismatch with an older peer can only be solved by updating it
- Windows and Linux - after Create a new library, a device seen only by address (multicast blocked) on the side that was asked to pair gets no Pair again; the user pairs from the Devices list
- Windows and Linux - Create a new library refuses a folder that already holds an empty library from an earlier try
- Windows and Linux - a library mismatch always adds "may be too old to merge", even when both run this version (can_merge() in pairing.c); PAIR_REFUSE would need to say why
- All - a member running an older version doesn't send the archive's name, so the archive explainer isn't shown, and stays unshown for a later pairing

### Frontends
- Windows and Linux - Select all writes on the UI thread: 3-4 s on a 10,000-album list on a debug build under load, unmeasured in release
- All - What to sync's Albums tab leaves out one-track albums (ALBUM_IS_SINGLE), so singles can't be picked by album and Select all doesn't cover them
- Android - Select all's skip rule is Kotlin, as edits are an overlay on the stored profile; the core has it in mc_sync_select_items()
- Android - the sync notification updates up to twice a second during a file; only when its text changes would do
- All - the waiting-for-a-device banner has no Show that filters the library to those tracks (the core only gives counts)
- Windows and Linux - the empty Playlists page's Choose what to sync opens this computer's page but doesn't scroll to or focus its What to sync section
- Windows - offer the firewall rule when the network is public too (today it stays quiet unless Private is uncovered)

## Move library

- Core - can't be cancelled part-way, and progress is per file, so one huge file shows no movement while it copies
- Windows - a desktop.ini at the top of the library stays in the old folder; Linux's .directory isn't treated the same way

## Windows

- Dialogs still light in dark mode: about 60 MessageBoxW calls, and the About and archive TaskDialogs. message_dialog() in prompt_dialog.c can replace them
- Screen readers: the artist page (kithsong_artists) has no UI Automation provider; the now-playing strip exposes no seek bar or times; the volume slider has no RangeValue pattern

## Testing

Debug hooks: KITHSONG_DEBUG_PAIR=<host>:<port> on Windows and `--es debug_pair <host>:<port>` on Android pair two scratch instances without clicks; `--es fake_merge <scenario>` on Android shows one of the merge's screens with made-up data.

### First
- test_sync_merge on a real build: abandoning a merge (librestore.c) was only syntax-checked where it was written

### Linux
- First run and the edit dialog on Wayland, in the Flatpak (portal paths) and with a screen reader; only seen on X11 under Xvfb
- Other dialogs opened from a menu use adw_dialog_set_focus() before present (playlist_dialogs.c, album_group_dialogs.c), so may lose focus as the edit dialog did
- GTK logs a gtk_drop_target_async_handle_crossing check from the second cover drag on; no visible effect, cause not found
- Known bugs to fix:
  - after Use as Track, focus returns to the first row rather than the next missing track
  - the Choose file menu's items have no accessible names
  - removing an album while it plays leaves the removed track in the bar, with Play doing nothing
  - the artist page's album tiles have no menu (singles' do)
  - an album tile stays highlighted after its right-click menu closes

### Windows
- Firewall: the success path after UAC (the themed prompt's Allow was only run up to start_helper), Settings' "Ask about Windows Firewall again", and the elevated helper removing rules left for a moved exe (only compiled to objects, never linked or run)
- Narrator and NVDA with the UI Automation providers
- The edit dialog with Narrator; on Windows 10, in high contrast and across a DPI change. A cover from a real drag from Explorer and from the file picker (drops were only driven through IDropTarget). The same display checks for the Mica smart editor, and UI Automation on its now owner-drawn Back, Save and Cancel
- The volume drop-down on Windows 10 (no rounded corners) and in high contrast; Play albums in order's screen-reader name
- The empty Albums page's Choose What to Sync in a group, the empty Playlists page's Choose what to sync, and the empty-page button through UI Automation
- The pairing dialog's ended page with a real pairing: the archive explainer (the dialog growing to fit it), Create a new library and Pair again
- The import review's Choose file menu with files of related items ("Grouped separately: …")

### Android and Wear OS
- Android Auto on the Desktop Head Unit and in a car: the four tabs, art in its lists (ArtProvider), playing from the middle of an album, voice and on-screen search, refreshing after a sync, and the app's own player and lock screen still working
- Remove from this phone / Sync to this phone in a real group, including stopping playback when it takes a queued track
- Smart lists' and groups' pictures, and empty playlists in Playlists and Recents
- An album group's Play, Shuffle albums and Shuffle tracks wrapping on a narrow screen; an edited playlist or group moving first in Recents
- A recent's long-press menu; Settings' Play albums in order
- Profile editor: the over-budget message, renaming a member, a group renaming the phone, dark theme
- After pairing: Choose what to sync and the one-time archive explainer
- Merging: the progress card, the forgotten-devices line, the merge_pending note, the failed summary and the merge's notification (only seen with fake_merge)
- Search with a large library
- A pairing finishing after leaving the app, and an upgrade from 0.4.2 leaving no WorkManager job (`adb shell dumpsys jobscheduler`)
- The one-time handover of the old prefs recents to the core
- API 24-32 devices (API 24-25 duck through the core's volume)
- Wear OS: sync on a real watch and with a desktop peer (only seen between Wear and phone emulators); a wrong code, the refusals, the waiting sentence and the failure line; the service's notification
- Wear OS: headphones connecting or unplugging mid-play, a real crown's seeking, album art in the media controls, a real scrobble
- Wear OS: playlists' and groups' long-press haptic, failure notice and TalkBack labels, and the watch's additions reaching a peer
- Wear OS: a licence's long text scrolling with the crown and showing its name
- Android: the smart list editor and the recycle bin on a device (Restore, Empty, the sync button's dot)
- Android: the smart list editor in landscape with the keyboard up (the tag editor needed adjustResize there; the manifest could set it app-wide)
- Android: swipes on a physical phone and with 3-button navigation; back on API levels below 33; the scrubber in landscape and at large font scales; a failed Save of a cover, a JPEG cover, and emoji in tags through the byte-array path
- Android: a first launch on a fresh install once logged "Couldn't open the library" (status -5) and then opened fine; cause unknown
- Android: :app:lintDebug fails on 20 errors from before this round (LocalContextGetResourceValueCall, StateFlowValueCalledInComposition)

### Cross-platform
- Sync speed and time left over a real network, between real devices, and against a peer without FILE_REQ's est; a chosen cover reaching a real older peer
- Opus: run `manual_opus_bench pool <dir> 11 128 <cache> 1` on an idle PC to replace opus.md's estimate, and a sync to a real phone
- merge_restored's notice (never seen anywhere); on Windows, that it fits the now-playing strip
- The library_mismatch update hint (needs a peer running an older version)
- The waiting-for-a-device banner and card running
- Sync PREPARE_REQ end to end (needs three devices), and a merged album going through the file phase
