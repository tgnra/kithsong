# Todo

What's left to build, fix and check. Platform first, then the item. "Decided:" marks a choice already made.

## For 1.0

- **Home page** - On Windows and Linux, from core's kithcore/home_picks.h: a station for each kind of music the audio analysis finds (kithcore/radio_stations.h), and the played rows from the play log, with track_plays for what's older than it. The phone app has only the Radio row, on its main page
- **Repeat and a play queue** - A major item, to be designed by hand rather than handed to agents. Decided: repeat all is always on when shuffling, which is intended, and there's no repeat one, as it's not much use and would clutter the UI. Ctrl+R for repeat on Windows and Linux is Tania's to add. The queue itself is in core (up next, in playback.h) and on Windows and Linux, with Play Next and Play Last everywhere, the toolbar popup or popover, and the setting for adding tracks one by one, and on Android, where a chevron on the full player slides to it

## Needs input

- Analysis version 2: judge a few stations by ear, as the weights were only checked against albums and artists. `bpm` reads about a third of one 150 BPM album as 100, a dotted beat, which only matters if it's ever shown
- Release - architectures other than x64. Windows and Linux build for ARM too, but the signed Flatpak repo is x86_64 only, so Linux ARM has a .flatpak bundle and a tarball with no flatpak update; merging architectures into one repo is its own piece of work. Neither Linux job runs the tests. Needs a list of any other architectures, and a way to test Linux ARM
- Core - mc_smart_describe() is English-shaped ("field op value" clauses under "Tracks where …"): languages with grammatical case or agreement may need their own templates; dates use the smart_date template, not the OS's date format. Needs a real translation to show what the templates have to do

- Android - the tablet layout's decisions: Sync now, Leave group and the recycle bin's notice are on this phone's own page in the sidebar, as on the desktops (a Sync now in the Devices heading, or a Sync page, are the other answers); the sidebar's rows are 44dp, under the 48dp touch target, which is what lets a Pixel Fold held sideways have the layout; and where a fold leaves too narrow a strip past the sidebar (the 7.6" Fold-in) the sidebar widens to the fold, so the grid isn't split there
## Library and browsing

- Android - Android Auto's queue items now expose art through ArtProvider, untested there
- Android - stations in Android Auto are untested; after a seed that won't open, Media3's legacy session queue keeps its old size, which Auto could show
- All - radio skips: a restart after a long pause still counts as a skip, and going back to a skipped track keeps its push until the station next opens
- Windows and Linux - Home's tile menus: physical clicks and the Menu key on Windows, screen readers on both, and touch long-press on Linux are untested
- All - a chosen cover undone on one device stays on its peers, and sync replacing a chosen cover on a peer deletes the old one rather than sending it to the Recycle Bin (mc_coverart_replace)
- Windows - a cover arriving by sync or import doesn't refresh the now-playing strip or the media controls' thumbnail until the next track (refresh_library doesn't call playback_tracks_changed)
- Android - a cover decoded for a large view also serves smaller ones, drawn without mipmaps, so fine detail can shimmer in small art (seen as a moiré on a 1 px test grid in the album header); not blur, and real covers may not show it
- Android - first paint is about 0.1-0.2 s later since covers stopped being shared across sizes (measured on the emulator; check on a phone)

- Android - the tablet and foldable work is untested on a device or emulator: the switch between layouts (600dp wide, and the sidebar with 3 recents high) while something is open, the album grid's gap over a fold, the tabletop player (auto-open and close, the queue beside the controls, a flip phone's fallback), keyboard focus and shortcuts (Space and the arrows in Search, Tab between the parts), and drag and drop (scrolling and taps starting no drag, right-click not taken as Back, a target scrolled into view mid-drag). Drag and drop is off in the made-up library, which can't be edited
- Android - the tablet layout has no fast scroll strip on its grids, and an empty library's welcome is only on Home
### Hand edits
- Windows and Linux - a MusicBrainz search thread still running during Save shares the handle's edit watch
- Decided: peers' files keep their old tags and places; the rows are what they show, and fetching audio again for a tag isn't worth it

### Smart playlists
- Windows and Android - the editor only edits a "not" that means a negative operator, or "none of"; any other rule opens read-only (Linux shows it as a group of one, and has Not all of the following). They also have no moving a condition or group, and no offering genres and artists as you type
- Android - a smart list whose picks are all away from this device: search may not open it (not checked; the desktops do)

### Remove from this device
See "Removing from one device" in docs/sync-protocol.md.
- Decided: no removal of single tracks, as there's no syncing of single tracks

## Import and tagging

- Windows - with Windows' long-path setting off, the folder picker can't browse past 260 characters (the shell's limit; it opens at the nearest parent), a file recycled from a long path restores to a kithsong-trash folder rather than where it was, and --config-dir over 260 is refused
- Core - merging libraries: rows added to an existing album keep their own mb_albumid, so they can name another release than the album row

## Sync

### Core
- A member that went away soon after a complete session is only noticed when the next attempt runs (up to MC_SYNC_AUTO_INTERVAL_MS), so tracks it alone holds that were learnt from a third device don't show as waiting until then

### Verifying files
- All - an imported file that only changed stays on the card with no remedy; unknown files have no in-app list, only orphans-found.txt; a lossy copy taken as a rescue lingers after the original returns; a member gone for good holds tracks back until it's removed from the group
- Wear OS - the verify warning and an archive watch's gone-for-good line are compiled only; a watch has no remove button, so removal has to happen on a phone or desktop

### Merging libraries
See "Merging libraries" in docs/sync-protocol.md.
- Core - tracks of the merged library with no file on that device and no match in the kept library (id, or name on the same album) are left out; only their old group's devices hold them
- Core - after a merge, files the selection leaves out go in the same sync, before the summary can be read: correct, but abrupt
- Core - if a leave notice from an earlier undone pairing is still waiting, the old group isn't told about a merge

### Frontends
- Windows and Linux - after Select all stores on its thread, the window can still stall: up to 0.7-1.6 s for 7,631 albums on a debug build, unmeasured in release; the reload after the write is suspected, a UI call waiting on api_lock is possible
- All - the waiting-for-a-device banner has no Show that filters the library to those tracks. The core lists them (track_uids in mc_sync_waiting); each frontend needs a track list by uids, and those tracks have no file here
- Android - a new library after a library mismatch (export as zips of up to 8 GB, then restart into an empty library and Pair again) is untested on a phone: an export to a USB stick or cloud folder, cancelling, a full disk, and re-importing the zips. A single or loose track credited to no artist isn't exported, as no listing has it. Decided: the export is files only (no playlists or plays), and a failed or cancelled export deletes all its zips, not just the last
- Android - the first run logs "Couldn't open the library" (status -5) once before the folder exists; one sync session logged status -6 straight after a good one, perhaps both sides syncing at once; opening a stress library logs "Couldn't open sync" (status -15)

## Windows

- In dark mode the About dialog's link keeps the system blue: readable, but low contrast
