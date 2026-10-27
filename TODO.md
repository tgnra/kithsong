# Todo

## Playlists

- Linux - playlist cover art (a placeholder for now)
- Windows - playlist cover art (a placeholder for now)
- Linux - keyboard access to the track menu (Menu key, Shift+F10)

## Album Groups

- Add UI to show Album Groups
- Add UI to browse an Album Group - the grid

## Smart Playlists/Groups

- UI - rule editor (build it from mc_smart_field_get()), list smart lists alongside playlists and album groups, reshuffle button
- Playback - call mc_library_record_play() when a track counts as played (half of it, or 4 minutes, like Last.fm?)
- Core - limit by total duration ("an hour of music")

## Artists View

 - List of Artists - toggle for album artist/ artist?

## Sync

- UI to do selective sync
- Original file tags - if I add flacs on my phone which is set to sync opus - those files are originals. When syncing with an archive, sync them to the archive and then delete and sync back opus of those files.

## Other

- Last.fm scrobbler
- Playback - AAC/M4A - pick a decoder: the free ones are GPL or awkwardly licensed, and the OS codec breaks "no OS codecs"
- Volume control
- Spacebar to play/pause
- Recents list above devices
- Move text strings to a centralised list and include in build

## Wear OS

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

- One activity, Material 3 with dynamic colour, follows the system dark/light setting
- Edge to edge
- Main screen is one long scroll of sections
    - Recents (list), Playlists (list), Albums (grid), Artists (list)
    - Tapping a section heading animates open a list of the headings; tapping one scrolls there
    - Sections with no content have no heading. With no content at all, show an icon and a message
    - Performance matters: one lazy list, stable keys and content types, no nested scrolling grids
    - Preload album art ahead of the scroll position so it never visibly loads
- Header scrolls with the content (not sticky): app title on the left, import and settings on the right
    - Clears the status bar at rest; once scrolled, art shows under the status bar and camera cutout
- Mini player: a floating rounded rectangle above the content, clearing the gesture navigation bar
    - Album art in a rounded square, title with "Artist - Album" underneath, play/pause on the right
    - Full-width seekbar underneath
    - Tapping it opens the full player
- Full player: large album art, seekbar, play/pause and seeking. Swipe down to dismiss
- Album, artist and playlist screens show a track list. Swipe right anywhere to dismiss
- Predictive back, accessibility labels, colours taken from the current album art
- Background playback via a media session: notification and lock screen controls, headset
  buttons, audio focus
- Import: request the audio permission appropriate to the API level
- Sync stores files in the app's own storage, never on the SD card
- Search comes later