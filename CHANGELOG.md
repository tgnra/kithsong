# Changelog

What changed in each Kithsong release, newest first. Each release's full list of commits is on
its GitHub release page.

## Unreleased

### Android

- Google Cast: play to a Chromecast or Cast speaker from the full player's cast button, on
  phones with Google Play Services. The queue, shuffle, stations and up next carry on as on
  the phone, and plays still count and scrobble. While casting, the full player has a volume
  slider for the Cast device, and the volume keys set it too. A cast left paused for three
  minutes ends, and playback stops.
- Next goes on from the last track added to up next, where it used to do nothing.

## 0.9.0 (2026-11-08)

### All platforms

- Radio no longer learns from skips. Skipping a track doesn't change what a station or mix
  plays next; skips are still kept in the play log.
- Devices stay visible to each other while idle. After a sleep or a network reconnect, a
  device could drop off the others' device lists and not come back until it was restarted.
  Sync also starts with no network and picks one up when it appears.
- Import: a single gets its cover from its own embedded picture before a cover in the folder
  it came from. A lone track with no album and an embedded picture gets an album of its own
  title, so the picture has something to be the cover of. Music imported as-is downloads its
  cover from the Cover Art Archive by the MusicBrainz ids in its tags.
- Import: a file can be removed from an item in review and put back. A removed file is
  neither tagged nor imported and stays where it is, so a stray extra file no longer keeps a
  match from being complete.

### Windows and Linux

- Plugins: Kithsong can load plugins, native libraries that add things which don't belong in
  the app itself. Settings › Plugins lists every plugin found, with its name, version and
  file, and each stays off until you turn it on. Plugins aren't loaded by the Flatpak yet.
- Tethered devices: a plugin can add a device that syncs over USB rather than over the
  network. Tethered devices are listed in the sidebar with a page of their own, sync from a
  profile of what to put on them, and send their plays and playlists back to the library.
- The first plugin, for iPod sync, is distributed separately as kithsong-ipod, not with
  Kithsong.
- The import review has Remove from import in a matched track's menu, a delete button on
  extra and related files, and Put back.

### Android

- Import has a page for each item, as the desktops' review has: why it waits, Search again
  and Search more widely, the candidates with release type and catalogue number, and the
  chosen one's tracks with each file. An imported item can be opened to import it again with
  another match. Candidates and tracks sit side by side on a wide screen.
- Files can be removed from an item in review and put back, as on the desktops.

### Wear OS

- The Sync screen shows the watch's address while waiting to pair and once paired.

## 0.8.6 (2026-11-07)

### All platforms

- Cancelling a sync on Windows now reaches the other device even while it's busy sending
  files.

### Android

- On tablets and foldables, Library stats, Import and Settings sit at the foot of the sidebar
  under the divider, as on the desktops. Recents shows as many as fit beside Devices, and a
  recent that moves to the top stays in sight.

## 0.8.5 (2026-11-07)

### All platforms

- Cancelling a sync tells the other device, which reports it as cancelled rather than failed
  and doesn't start a sync with that device on its own for 10 minutes, unless you press Sync.

### Linux

- Zip files can be imported: Choose Zip Files on the import page, or drop them on the
  window. A zip that can't be extracted is listed as failed, saying why.
- Linux ARM (aarch64) gets Flatpak updates too, from the same signed repository as x86_64.

### Android

- A desktop-style layout for tablets and foldables, used whenever the window is wide and tall
  enough: a top bar with the controls, a sidebar of pages, Recents and Devices, and a track
  list beside the page when there's room. It switches live, and what's open stays open across
  a resize, rotation or fold. Album grids leave a gap over a fold, and half open on a table
  the full player puts the art above the fold and the controls below. Keyboard shortcuts,
  drag and drop and right-click menus work as on the desktops. The phone layout is
  unchanged.
- A Home page for the tablet layout, with Radio, Recents, Played a lot recently, Not played in
  a while and Random.
- When pairing finds that the phone and the other device each have music of their own, the
  phone can start a new, empty library, after exporting its music as zips to a folder you
  pick, or without exporting.

## 0.8.4 (2026-11-07)

No changes to the app. A release is now published as soon as every platform has built, and
the website, with its Flatpak repository, is updated with it.

## 0.8.3 (Release Candidate 3) (2026-11-07)

### All platforms

- Starting an album, playlist, radio or station clears Up next. Resuming where you left off
  keeps it.
- Music analysis now looks at rhythm, frequency bands and how the sound changes from moment
  to moment, so stations and mixes find closer neighbours. Every track is analysed again
  after updating.

### Linux

- Linux ARM (aarch64) has its own tarball and `.flatpak` bundle.
- Flatpak users get new versions again: the website wasn't being updated when a release was
  published, which left the Flatpak on 0.7.6.

### Android

- The full player has the play queue: the chevron at its foot slides up to it. Play next and
  Play last are in every menu but a station's, and Settings has Add to queue as
  individual tracks.
