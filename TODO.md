# Todo

What's left to build, fix and check. Platform first, then the item. "Decided:" marks a choice already made.

## Library and browsing

- Android - Android Auto's queue items now expose art through ArtProvider, untested there
- Android - stations in Android Auto are untested; after a seed that won't open, Media3's legacy session queue keeps its old size, which Auto could show
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

## Google Cast

See docs/cast.md.
- Linux - the cast button and picker have only been run headless, with devices put in the list by hand: the list, Connecting to (a device that never answers), Couldn't connect to, and the light and dark styles look right. Never against a real device: the button showing as one is found on the network, casting itself, the volume slider and the app's volume (control, keys, MPRIS) moving the device's, the device's own volume buttons moving them back, Lost the connection to, stopping from the TV or Google Home, and a speaker group. Screen readers (Orca) are untested
- Linux - discovery runs for as long as the app does, so the button knows when to show: its cost on a laptop's battery, and on a network with many Cast devices, is unmeasured. It also starts the moment the app does, whether or not anyone casts
- Linux - picking another device while casting freezes the window until the first cast has ended (mc_cast_connect() waits for it): moments on a good network, longer for a device that has stopped answering
- Linux - Settings' Move Library (and anything else that calls playback_clear()) frees the player, which ends a cast without a word (MC_CAST_END_PLAYER), as it stops and empties the queue here
- Linux - the Flatpak's --share=network should cover mDNS and the music server the device fetches from: untested in the Flatpak
- Windows - the cast button, picker (cast_popup.c) and device volume are not compiled yet. They have been checked only against mingw-w64 headers with clang (zig cc: no errors or warnings), never linked or built with llvm-mingw or MSVC, and never run
- Windows - untried on a device: the button showing and going as devices come and go (and the strip reflowing), connecting, the connecting pulse, the picker resizing between the list and a session, the volume slider while the device reports its own levels, Stop casting, and the failed and lost messages in the now-playing strip
- Windows - untried with Narrator: the picker's list, slider and Stop casting, the heading read out as a live region when the state changes, and the button's name changing ("Casting to …")
- Windows - the keyboard's volume keys set the device's volume while casting only when they reach the window as WM_APPCOMMAND. Keyboards whose volume keys Windows handles itself still change the computer's volume, and so does Windows' own volume flyout
- Windows - casting on a Public network (or before the firewall rule is added) fails every track: the rule sync asks for (firewall.c) covers the music server but only on Private and Domain networks, and nothing tells the user while casting. The prompt is also only offered while sync runs. The device page's firewall banner could be shown in the picker, or after the first track fails
- Windows - discovery runs all the time, so the button can show only when there's a device: one mDNS query every 20 s once settled. Check that this is acceptable on battery
- Windows - the cast glyph is Segoe Fluent's MiracastLogoSmall (U+EC15). Check that it reads as Cast next to shuffle, in both themes and in high contrast
- Core - the sender is tested against a fake receiver only (test_cast_session), never a real Chromecast, Cast TV, speaker or Google Home speaker group. Unchecked there: QUEUE_LOAD's start time and autoplay off, QUEUE_INSERT/QUEUE_REMOVE and the item ids statuses give, the receiver going on to the queued item gaplessly, getting a dropped connection back, and another sender or Google Home stopping or taking over the cast
- Core - not built for Windows yet. The Cast, msock and socket sources and the Cast tests compile with clang against mingw-w64 headers (zig cc, x86_64 and aarch64) with no errors or warnings, but nothing has been linked, and nothing has been built with MSVC
- Core - a host firewall that blocks incoming connections (Windows asks on first use; firewalld on Fedora) stops the device fetching from the music server, whose port is random; nothing says so but the device failing every track
- Core - FLAC over 96 kHz or 24-bit, and more than two channels, are sent as they are, which some devices can't play (each track is then skipped); they could be converted as AIFF is. Only AIFF is converted
- Core - devices reached only over IPv6 are untested (the server listens on the connection's own address)
- Decided: no device authentication (urn:x-cast:com.google.cast.tp.deviceauth); senders may skip it, and the certificate isn't checked

## Windows

- In dark mode the About dialog's link keeps the system blue: readable, but low contrast
