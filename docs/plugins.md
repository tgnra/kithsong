# Plugins

> Status: being implemented on the `experimental` branch. Change this
> document first, then the code.

## Goals

- Let features that don't belong in core ship as separate native libraries (`.dll` on
  Windows, `.so` on Linux): niche hardware, codecs with licensing baggage, and third-party
  services with terms of their own.
- The first plugin is **iPod sync**, for click-wheel iPods that predate the iTunesDB hash
  (the 4th generation and earlier, iPod photo, mini, and nano 1G/2G). It adds a USB-connected
  iPod as a sync target, writing lossless originals as ALAC and lossy ones as MP3.
- Plugins never draw UI. Each desktop frontend (Win32, GTK) renders what a plugin declares,
  so a plugin is written once and needs no toolkit.
- Core stays a static library. Plugins don't link against it.

Not goals, for now: Android and Wear OS plugins (no loading native code from outside the APK,
and USB mass storage is a poor fit on a phone), sandboxing, a plugin store, and audio-path
(DSP) plugins.

## The boundary

A plugin is a shared library that exports one C function:

```c
KITH_PLUGIN_EXPORT const kith_plugin *kith_plugin_init(const kith_host *host);
```

- `kith_host` is a table of function pointers that core fills in: what a plugin may call. It
  starts with `abi` (an integer) and `size` (`sizeof(kith_host)`).
- `kith_plugin` is the plugin's answer: its `abi`, id (reverse-DNS, such as
  `ng.kithso.ipod`), name, version, and the extension points it provides (below), each a table
  of function pointers.
- The ABI version must match exactly. If it doesn't, core refuses the plugin and Settings says
  so. There are no compatibility shims before 1.0 (the same rule as sync), so every release
  that changes `kithcore/plugin.h` bumps `KITH_PLUGIN_ABI`, and plugins are rebuilt alongside.
- The header, `core/include/kithcore/plugin.h`, uses only C types: no kithcore handles cross
  the boundary. Strings are UTF-8. Memory a plugin returns is freed by that plugin's own
  `free` callback, and memory core returns by `host->free`, because the two may use
  different C runtimes. Most data is lent rather than returned: lists (a device's tracks, its
  plays) are handed over one item at a time through visitor callbacks, valid during the call.
- Why a host table rather than exported kithcore symbols: core is linked statically into
  each frontend, so it has no symbols to export without becoming a shared library. A table
  also keeps the API plugins see small and deliberate.

## Loading

- Core loads plugins (`mc_plugins_load()`), so both desktop frontends behave the same way.
- Where from:
  - Windows: `<install dir>\plugins\*.dll` and `%LOCALAPPDATA%\Kithsong\plugins\*.dll`;
  - Linux: `<prefix>/lib/kithsong/plugins/*.so` and `~/.local/share/kithsong/plugins/*.so`.
    The Flatpak loads plugins from a Flatpak extension point (`ng.kithso.Kithsong.Plugin`)
    mounted at `/app/plugins`.
- **A plugin is native code with the app's full rights.** Every plugin found is listed under
  Settings › Plugins, showing its name, version and file path, and stays off until its user
  turns it on. The list of enabled plugins goes in `settings.json`, by file id (the file's name
  without its extension), since a plugin that is off is never loaded, not just left uncalled,
  and so can't be asked its own id. Once loaded, its id, name and version are remembered there
  too, so the list shows them while it's off; until then it shows the file id.
- A plugin whose `kith_plugin_init` returns NULL, or whose ABI doesn't match, is listed with
  the reason and isn't loaded.
- Plugins are unloaded at shutdown, after every session using them has ended.

## Extension points

Version 1 has two extension points, both needed for the iPod: device targets and encoders.
Each one is a `struct` of callbacks that a plugin may provide. A plugin can provide several.

### Device targets

A **device target** is a device that can't run Kithsong: core works out what it should hold,
and the plugin writes it.

The plugin provides:

| Callback | What it does |
|----------|--------------|
| `probe(mount_path)` | Whether this volume is one of its devices: for the iPod, whether `iPod_Control/iTunes/iTunesDB` exists and `iPod_Control/Device/SysInfo` names a model without the hash. Cheap, no writes. |
| `open(mount_path) → target` | Opens it. Reports a stable id (the iPod's FireWire GUID from SysInfo, falling back to its serial number), a name (the iPod's own name, from its master playlist), capacity, free space, and the formats it plays, with their limits (below). |
| `list(target)` | What it holds: for each track, the plugin's own key, the `track_uid` and `file_uid` it was written for (if Kithsong wrote it), format, encoder setting, bitrate, size and hash, plus the title, artist, album, length and play count core matches and compares by. |
| `put(target, track, file_path, key)` | Adds one track from an encoded file, or replaces the one under `key`, with its tags, cover, and play count and last played time, and reports the key it is under. |
| `update(target, key, track)` | Rewrites a track's tags, play count and last played time without copying its file again. |
| `remove(target, key)` | Removes one track. |
| `file_path(target, key)` | The path of a track's file on the volume, for verification. |
| `plays_read(target)` *(optional)* | Plays made on the device since the last sync: for each track key, how many and when the last one was (see Plays from the device). |
| `playlists_read(target)` *(optional)* | Playlists made on the device: for the iPod, its On-The-Go playlists, each a name and a list of track keys. |
| `playlists_write(target, playlists)` *(optional)* | Makes the device's Kithsong playlists the ones the profile selects, leaving playlists made on the device alone. |
| `device_done(target, what)` | Clears what core has taken in (`plays`, `playlists`), once it is safely in the library: for the iPod, deletes `Play Counts` and the `OTGPlaylistInfo` files at the next `commit()`. |
| `commit(target)` | Writes the device's database. Core calls it every 50 tracks or 2 minutes during a sync, and at the end, so unplugging loses little. |
| `close(target)` | Closes it. |
| `eject(mount_path)` *(optional)* | Flushes and ejects, if the device needs more than the OS's own eject. |

Core does everything else, as for a Kithsong device:

- **Detection.** Each frontend tells core when a volume mounts or unmounts
  (`WM_DEVICECHANGE` on Windows, `GVolumeMonitor` on Linux). Core probes every enabled
  device target, and a recognised volume appears in the device list like a group member
  with its own kind (`tethered`).
- **What it holds** comes from a sync profile like every device's: everything or a
  selection, edited in the same What to sync editor. The format choice is the target's own
  (see Format policy). A budget works as it does now, from the free space the plugin reports.
- **The plan** is `plan.c`'s wanted set, worked out from that profile, less what `list()`
  reports. Core then calls `put()` and `remove()` and shows progress as it does for a session.
- **Encoding** goes through the encoders plugins provide (below), cached the same way as Opus
  transcodes.
- **Verification**: after `put()`, core asks the plugin for the file's path on the volume,
  hashes it, and compares it with the encoded file's hash. A mismatch is retried once, then
  reported on the card.

#### Where a target's profile lives

A tethered device belongs to **the computer it's plugged into**. Its profile goes in tables
of that computer's `sync.db` that are never replicated (`tethered_devices`,
`tethered_profile_items`), keyed by the target's stable id. Consequences:

- It works with no sync group, straight from the library.
- In a group, it can only receive tracks this computer has a file for (an original, or an
  original-quality copy). Tracks the profile wants that aren't here show as missing, as with
  Waiting for a device. Fetching them from an archive first is a later step.
- Other members don't see the iPod. Making tethered devices group members, so a phone could
  edit what goes on the iPod, is an open question (below).

**Tracks Kithsong didn't write are never removed or changed.** An iPod that iTunes filled
keeps that music beside Kithsong's: `list()` reports such tracks with no `track_uid`, and the
plan leaves them alone, though their size counts against the free space.

Each track the plugin writes is also recorded on the device itself
(`iPod_Control/kithsong.json`: track key → `track_uid`, `file_uid`, format and hash), so
another computer running Kithsong recognises the files and doesn't write them again.

#### Format policy

A target declares the formats it plays and their limits. Core picks a format for each track
from the profile's policy:

| Source | Default for the iPod | Choices |
|--------|----------------------|---------|
| Lossless (FLAC, WAV, AIFF, ALAC) | ALAC | ALAC, or MP3 at a bitrate (to save space) |
| Lossy, in a format the device plays (MP3, AAC) | copied as it is | as is, or re-encoded to MP3 if over a bitrate cap |
| Lossy, in one it doesn't (Opus, Vorbis) | MP3 V0 | MP3 at V0, V2, 320, 256, 192 or 128 kbps |

Limits the target declares are applied while encoding. 4th-generation iPods play ALAC at
16 bits and 44.1 or 48 kHz, so a 24-bit/96 kHz FLAC is resampled to 44.1 kHz and dithered to
16 bits. The iPod also doesn't play files with more than two channels, so multichannel
sources are downmixed to stereo, as Opus transcodes already are.

#### Plays from the device

Play counts go both ways. Each sync starts by taking in what was played on the device, then
writes the library's counts back:

1. `plays_read()` gives, for each track key, a count of plays since the last sync and the time
   of the last one. Keys with no `track_uid` (music Kithsong didn't write) are matched to
   library tracks by artist, album, title and duration within 2 seconds. A track that matches
   nothing is skipped and counted on the card as "plays of tracks not in the library".
2. Each play becomes a play log row (`mc_library_log_play()`) with context `device`, the
   tethered device's id as `context_uid`, end `finished` and `listened_ms` set to the track's
   length. The device records only the last play's time, so with several plays the last one
   gets that time and each earlier one is placed one track length (plus 30 seconds) before
   the one after it. That's a guess, but it keeps the times in order and believable.
3. Each play is also scrobbled (`mc_scrobbler_scrobble()`) if a scrobbling account is signed
   in on this computer. Last.fm drops scrobbles older than 14 days, so those plays are only
   logged, and the card says how many weren't scrobbled.
4. Plays are taken in exactly once. Before `device_done(plays)`, core records the import in
   `sync.db` (`tethered_imports`: device id, a hash of the `Play Counts` file, and the time).
   A `Play Counts` file whose hash was already imported is cleared without being read again,
   so an unplug between the import and the commit never doubles anything.
5. Then every track Kithsong wrote gets the library's play count and last played time (from
   the play log, the device's own plays included) through `update()`, so the iPod shows the
   same totals as the app.

#### Playlists from the device

On-The-Go playlists made on the iPod become manual playlists in the library, named
"On-The-Go 1" and so on, plus the iPod's name if the library already has one by that name.
Their tracks are matched as for plays. Once the playlists are created, `device_done(playlists)`
clears them from the iPod. A playlist is created only once, guarded like plays by
`tethered_imports`. If the profile selects that playlist, it then goes back to the iPod as an
ordinary playlist at the end of the same sync.

### Encoders

An **encoder** turns a decoded source into a file of one format. The plugin provides:

| Callback | What it does |
|----------|--------------|
| `formats` | What it writes: an id (`alac`, `mp3`), a file extension, whether it's lossless, and its quality settings (bitrates or VBR levels). |
| `encode(source, format, setting, out_path, job)` | Reads PCM from `source` and writes `out_path`, checking `host->cancelled(job)` now and then. |

`source` comes from core (`host->decode_open(path)`): PCM frames at a sample rate, bit depth
and channel count core can convert to what the encoder asks for, plus the track's tags and
cover bytes. Encoders don't decode anything themselves, so every format core reads (and every
decoder plugin, later) feeds every encoder.

Core generalises the transcode cache from Opus to any encoder:
`<config>/transcode-cache/<file_uid>/<format>-<setting>.<ext>`, with the same sidecars,
part files, LRU trim and encode pool. A device's limits are part of the name, since they change
what's encoded: `alac-max16b48000r2c.m4a` is ALAC capped at 16 bits, 48 kHz and two channels
(a format with no quality settings has no `-<setting>`). An encoder must be safe to call from
several threads at once on different files.

## What the host table offers

Version 1 offers only what device targets and encoders need:

- `log(level, message)`, which goes to the app's log;
- `free(ptr)`;
- `decode_open`, `decode_info`, `decode_set_format`, `decode_read`, `decode_close`, `decode_tags`,
  `decode_cover` (above);
- `settings_get` and `settings_set` for the plugin's own settings, stored under its id in
  `settings.json`;
- `string(key)`: for now only `string("language")`, the UI language, so a plugin can pick its
  own translated strings;
- `cancelled(job)`: whether the user stopped the sync, for long callbacks to check.

A plugin's settings are declared rather than drawn: a list of options, each a switch, a
choice, or a number with a range, with a label in the UI language. Settings › Plugins shows
them under the plugin.

## The iPod plugin

It lives in this repository as `plugins/ipod/`, built when `KITH_BUILD_PLUGINS` is on, and
ships as a separate download rather than inside the app.

- **iTunesDB** is written by the plugin itself, without libgpod: libgpod drags in GLib, and
  only the pre-hash database is needed (`mhbd`, `mhsd`, `mhlt`, `mhit`, `mhod`, `mhlp`,
  `mhyp`, `mhip`). Files go in `iPod_Control/Music/Fnn/` with short random names, as iTunes
  writes them. Kithsong's playlists that the profile selects become iPod playlists.
- **Artwork** (iPod photo and nano): `ArtworkDB` and `.ithmb` files at the sizes SysInfo's
  model calls for. The grayscale 4th generation has no artwork.
- **Encoders**: ALAC from Apple's reference encoder (Apache-2.0), MP3 from LAME (LGPL-2.0,
  so the plugin's source and the means to relink it stay available, which shipping it from
  this repository covers). Both are listed in `LICENSES.md` under the plugin, not the app.
- **Play counts and On-The-Go**: the plugin reads `iPod_Control/iTunes/Play Counts` (the
  `mhdp` header and one entry per `mhit`, in the iTunesDB's track order) and
  `OTGPlaylistInfo` plus `OTGPlaylistInfo_N` (`mhpo`: indexes into that same order), and
  writes `play_count`, `last_played` (Mac epoch, 1904) in each `mhit`. The indexes only hold
  for the iTunesDB they were made against, so the plugin reads plays and playlists before
  it writes a new database.

## Later extension points

In rough order of usefulness. Each one is added when a plugin needs it, not before:

1. **Play sources** that aren't device targets, such as a Rockbox `playback.log` read without
   syncing, using the same import path as Plays from the device.
2. **Decoders**: formats core doesn't read (AAC/M4A, WavPack, Monkey's Audio, DSD), which
   then feed playback, analysis and every encoder.
3. **Metadata providers**: lyrics, artist images and bios, other tag sources (Discogs),
   called beside MusicBrainz in the tagger.
4. **Listeners**: now playing, track changed, play logged. For Discord presence, a
   now-playing file for OBS, MQTT for Home Assistant, ListenBrainz.

## Implementation order

1. `kithcore/plugin.h`, loading, Settings › Plugins with on/off, and a test plugin built
   from `core/tests` that the tests load.
2. Encoders: generalise the transcode cache and `mc_format` beyond Opus; `decode_open` over
   `playback/decoder.c`.
3. Device targets: volume events from both frontends, the tethered tables in `sync.db`, the
   plan and the file loop, and a **folder target** test plugin (writes into a directory) so
   all of it can be tested without an iPod.
4. Plays and playlists from the device: the import, `tethered_imports`, scrobbling, writing
   counts back. The folder test plugin fakes plays and a playlist to test it.
5. The iPod plugin: iTunesDB, Play Counts and OTGPlaylistInfo reader and writer, ALAC and
   LAME, `kithsong.json`, tested against fixture databases and then a real iPod.

## Open questions

- **Tethered devices as group members.** If an iPod's profile replicated, any member could
  edit it, and whichever computer it's plugged into next could fill it. That costs a schema
  change to `devices` and the group changelog, and needs rules for a device two computers
  write to. It's not worth it until local-only proves too limited.
- **Folder targets in core?** A generic "USB stick / SD card as an MP3 folder" target is
  useful to many people (car stereos), and has nothing niche about it. It may belong in core,
  with the plugin version kept only as the test fixture.
- **Signing.** Should official plugins be signed, and others marked as unverified in
  Settings? Not before there are plugins from anyone else.
