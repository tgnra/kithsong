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
| library     | `core/src/library`     | library folder, schema, migrations, change-log triggers, counts |
| settings    | `core/src/settings.c`  | per-device settings (library folder location) as JSON |
| tagger      | `core/src/tagger`      | tag read/write (TagLib), importer: MusicBrainz matching, scoring, singles, cover art, copying into the library folder (see [tagging-rules.md](tagging-rules.md)); no AcoustID yet |
| net         | `core/src/net`         | HTTPS GET with embedded CA bundle, rate limiter; capped at TLS 1.2 (see `http.c`) |
| playlist    | `core/src/playlist`    | stub |
| playback    | `core/src/playback`    | gapless play queue: a decoder thread fills a ring buffer that the miniaudio device reads; FLAC, MP3 (LAME delay/padding trimmed), WAV, AIFF via dr_libs |
| sync        | `core/src/sync`        | stub |
| discovery   | `core/src/discovery`   | stub |
| transcode   | `core/src/transcode`   | stub |
| util        | `core/src/util`        | clocks, RNG, threads, directory walk, UTF-8 file helpers, UUIDv7, HLC |

## Startup budget

Launching opens the SQLite file, runs `PRAGMA user_version` and recreates the triggers.
Nothing else happens before the first frame. The network, discovery and the player start
lazily.
