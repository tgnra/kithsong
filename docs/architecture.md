# Architecture

```
 Win32 UI        GTK4/libadwaita UI       Compose UI (phone + Wear OS)
 (desktop/windows) (desktop/linux)         (android/app, android/wear, via JNI)
        \               |                     /
         +--------------+--------------------+
                        |
               libmusiccore (C11, core/)
   library | tagger | playback | sync | discovery | transcode
                        |
   SQLite · TagLib · curl+mbedTLS · yyjson · miniaudio · dr_libs · (mdns, opus, noise)
```

## Rules

- **All behaviour lives in the core.** Frontends render state and forward user actions. If
  two frontends would need the same logic, it belongs in `core/`.
- **One static binary per platform.** Dependencies are fetched by pinned hash (never committed) and
  linked statically. No OS codecs, OS TLS or OS mDNS daemons.
- **UTF-8 everywhere** at the core boundary, including Windows paths (the core converts to
  UTF-16 internally where Win32 or TagLib needs it).
- **Opaque handles plus `mc_status` returns** (see `core/include/musiccore/musiccore.h`).
- **Threading.** A handle is used by one thread at a time. Long-running work (import,
  discovery, sync, playback) runs on core-owned threads and reports back through callbacks.
  Frontends marshal those callbacks onto their UI thread.

## Core modules

| Module      | Path                   | Status   |
|-------------|------------------------|----------|
| library     | `core/src/library`     | library folder, schema, migrations, change-log triggers, counts; deleting tracks to a trash and restoring them (`delete.c`); moving the library to another folder (`library_move.c`) |
| search      | `core/src/search`      | in-memory index of artist, album, track, playlist and album group names, rebuilt on a core thread (own read-only connection) after one changes, while searches use the previous index; typo-tolerant word matching (bounded Damerau-Levenshtein), the last word as a prefix |
| settings    | `core/src/settings.c`  | per-device settings (library folder location) as JSON |
| tagger      | `core/src/tagger`      | tag read/write (TagLib), importer: MusicBrainz matching, scoring, singles (a lone track as its release's), cover art, copying into the library folder (see [tagging-rules.md](tagging-rules.md)) |
| net         | `core/src/net`         | HTTPS GET and form POST with embedded CA bundle, rate limiter; capped at TLS 1.2 (see `http.c`); TCP sockets with timeouts over POSIX and Winsock |
| scrobble    | `core/src/scrobble`    | Last.fm 2.0 API (or a service with the same API, such as Libre.fm): browser sign-in, now playing, plays queued in `sync.db` and sent in batches with backoff; the account syncs to the group |
| playlist    | `core/src/playlist`    | playlists (create, rename, delete, insert, append, move and remove entries), ordered by fractional index so concurrent edits merge; album groups; cover art as a 2x2 grid of album covers picked by a playlist-seeded hash, composited with stb_image and cached per device as a JPEG |
| smart       | `core/src/smart`       | smart playlists and smart album groups: a JSON rule compiled to one parameterised SQL query each time the list is listed (format in [schema.md](schema.md)) |
| playback    | `core/src/playback`    | gapless play queue: a decoder thread fills a ring buffer that the miniaudio device reads; FLAC, MP3 (LAME delay/padding trimmed), WAV, AIFF via dr_libs; Opus via opusfile; decides when a play counts; shuffle (Miller Shuffle, endless rounds) |
| sync        | `core/src/sync`        | `sync.db`; framing, Noise KK/XXpsk3, encrypted channel, J-PAKE pairing over the network with the join rules (merging two libraries that both have music, `libmerge.c`), group and library changelog sessions (field-level LWW), a scheduler, address hints; the file phase (`transfer.c`: fetching what each device's profile wants, originals or Opus copies, then reconciling its library folder) |
| discovery   | `core/src/discovery`   | mDNS/DNS-SD announce and browse of `_kithsong._tcp` on a core thread (mjansson/mdns); peers reported online/offline through a callback; announces the host name |
| transcode   | `core/src/transcode`   | Opus (libopusenc) with tags and cover, cached per file and bitrate with an LRU trim; size estimate; encode benchmark |
| util        | `core/src/util`        | clocks, RNG, threads, directory walk, UTF-8 file helpers, moving files to the Recycle Bin or Trash (`mc_trash()`), text folding (case, accents) for comparison, UUIDv7, HLC, the UI strings table and plural rules (`i18n.c`) |

## Startup budget

Launching opens the SQLite file, runs `PRAGMA user_version` and recreates the triggers.
Nothing else happens before the first frame. The network, discovery and the player start
lazily.
