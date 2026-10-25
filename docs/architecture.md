# Architecture

```
 Win32 UI        GTK4/libadwaita UI       Compose UI
 (desktop/windows) (desktop/linux)         (android, via JNI)
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
- **One static binary per platform.** Dependencies are vendored or fetched by pinned hash and
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
| library     | `core/src/library`     | schema, migrations, change-log triggers, counts |
| tagger      | `core/src/tagger`      | tag read/write (TagLib), MusicBrainz ping; autotagger stubbed |
| net         | `core/src/net`         | HTTPS GET with embedded CA bundle, rate limiter |
| playlist    | `core/src/playlist`    | stub |
| playback    | `core/src/playback`    | stub |
| sync        | `core/src/sync`        | stub |
| discovery   | `core/src/discovery`   | stub |
| transcode   | `core/src/transcode`   | stub |
| util        | `core/src/util`        | clocks, RNG, mutex, UUIDv7, HLC |

## Startup budget

Launching opens the SQLite file, runs `PRAGMA user_version` and recreates the triggers.
Nothing else happens before the first frame. The network, discovery and the player start
lazily.
