# Todo

What's left to build, fix and check. Platform first, then the item. "Decided:" marks a choice already made.

## For 1.0

- **Repeat and a play queue** - A major item, to be designed by hand rather than handed to agents. Decided: repeat all is always on when shuffling, which is intended, and there's no repeat one, as it's not much use and would clutter the UI. Ctrl+R for repeat on Windows and Linux is Tania's to add. The queue itself is in core (up next, in playback.h) and on Windows, with Play Next and Play Last everywhere, the toolbar popup, and the setting for adding tracks one by one; Linux and Android have neither yet, and only need to call it
- **Home page** - On Windows and Linux, from core's musiccore/home_picks.h: a station for each kind of music the audio analysis finds (musiccore/radio_stations.h), and the played rows from the play log, with track_plays for what's older than it. The phone app has only the Radio row, on its main page

## Needs input

- Release - architectures other than x64. Needs a list of which ones, and a way to test them
- Core - mc_smart_describe() is English-shaped ("field op value" clauses under "Tracks where …"): languages with grammatical case or agreement may need their own templates; dates use the smart_date template, not the OS's date format. Needs a real translation to show what the templates have to do
- Android - there's no Create a new library, so a library mismatch can't be solved from the phone. Needs a decision on what replacing a library held in app storage should mean, as the desktops' answer is a new folder

## Library and browsing

- Android - Android Auto's queue items now expose art through ArtProvider, untested there
- Android - stations in Android Auto are untested; after a seed that won't open, Media3's legacy session queue keeps its old size, which Auto could show
- All - radio skips: a restart after a long pause still counts as a skip, and going back to a skipped track keeps its push until the station next opens
- Windows and Linux - Home's tile menus: physical clicks and the Menu key on Windows, screen readers on both, and touch long-press on Linux are untested
- All - a chosen cover undone on one device stays on its peers, and sync replacing a chosen cover on a peer deletes the old one rather than sending it to the Recycle Bin (mc_coverart_replace)
- Windows - a cover arriving by sync or import doesn't refresh the now-playing strip or the media controls' thumbnail until the next track (refresh_library doesn't call playback_tracks_changed)
- Android - a cover decoded for a large view also serves smaller ones, drawn without mipmaps, so fine detail can shimmer in small art (seen as a moiré on a 1 px test grid in the album header); not blur, and real covers may not show it
- Android - first paint is about 0.1-0.2 s later since covers stopped being shared across sizes (measured on the emulator; check on a phone)

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
- Android - the first run logs "Couldn't open the library" (status -5) once before the folder exists; one sync session logged status -6 straight after a good one, perhaps both sides syncing at once; opening a stress library logs "Couldn't open sync" (status -15)

## Windows

- In dark mode the About dialog's link keeps the system blue: readable, but low contrast
- test_analysis's check on a growing run failed twice in the release job on windows-latest at v0.7.6, and passed there once with diagnostics added; 56 local runs, debug and release, didn't reproduce it, and it passed on Linux. Cause not found. The check is split now, so the next failure says which part
