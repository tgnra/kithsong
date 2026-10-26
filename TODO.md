# Todo

## Playlists

- Linux - playlist cover art (a placeholder for now)
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

- Main UI has a big list of 
    - Recents
    - Playlists
    - Albums
    - Artists
- Clicking the header allows you to jump to each
- Playback controls at the bottom
- Sync and settings right at the top in the header?
    - Make the header not sticky so that album art shows near the camera cut out