# Library schema

The source of truth is [core/src/library/schema_v1.sql](../core/src/library/schema_v1.sql)
(embedded into the binary at build time) plus the later migrations in `k_migrations` in
[core/src/library/library.c](../core/src/library/library.c).

| Version | Change |
|---------|--------|
| 1 | initial schema |
| 2 | singles: `albums.kind`, `tracks.year`, `tracks.mb_albumid` |
| 3 | `tracks.added_ms`, `track_plays` |

## Location

The database is `library.db` at the top of the library folder, next to the music it
describes (`mc_library_open()` takes the folder). `files.path` is relative to that folder
and always uses `/`, so the folder can be moved or mounted elsewhere. `albums.art_hash` is
the SHA-256 of the album folder's `cover.jpg`. The folder itself is recorded in per-device
settings, not in the database. See
[tagging-rules.md](tagging-rules.md#library-folder).

## Identity

Every syncable row has a `uid`: a UUIDv7 that is generated once and is the same on every
device. MusicBrainz ids (`mb_trackid`, `mb_albumid`, ...) are stored as attributes, never as
keys. Untagged files don't have them, and the same recording can appear on several releases.

When two devices independently import the same album, they create different uids. Merging
those duplicates (matched by MusicBrainz release id or by a content hash) is a sync-level
operation; see [sync-protocol.md](sync-protocol.md). The same applies to singles albums,
which are matched by artist and `kind = 'singles'` instead.

## Singles

Tracks that aren't part of an album live in one album per artist with `kind = 'singles'`,
titled "&lt;artist&gt; Singles". Because the album has no year or release of its own, each of
its tracks carries `year` and `mb_albumid` (the release it came from). Regular albums have
`kind = 'album'`. See [tagging-rules.md](tagging-rules.md#singles).

## Play counts

A track's play count is `SUM(play_count)` over its `track_plays` rows. Each device keeps its
own row (`node_uid` is that device) and only ever updates that one, so field-level sync can't
lose plays made on two devices: no two devices ever write the same field. Rows are deleted
with their track.

## Tables

| Table              | Synced | Notes |
|--------------------|--------|-------|
| `artists`          | yes    | |
| `albums`           | yes    | `art_hash` is the SHA-256 of the album folder's `cover.jpg` |
| `tracks`           | yes    | `added_ms`: when the track was first imported (Unix ms); NULL for tracks imported before v3 |
| `track_plays`      | yes    | play counts, one row per track per device; see [Play counts](#play-counts) |
| `playlists`        | yes    | |
| `playlist_entries` | yes    | ordered by the fractional-index string `position` |
| `files`            | no     | this device's files, by path relative to the library folder; `source_file_uid`/`source_peer_uid` mark transcodes received from a peer |
| `changelog`        | -      | field-level change log that sync is built on |
| `peers`            | no     | paired devices and their sync cursors |
| `meta`             | no     | `node_uid` and other per-device settings |

## Change tracking

`library.c` generates `AFTER INSERT/UPDATE/DELETE` triggers from its tracked-column list
every time the library is opened. Each change to a tracked column writes one `changelog`
row (`hlc`, `origin_uid`, `entity`, `row_uid`, `field`, `value`).

Several connections to one library can be open at once: the importer opens its own on its
thread. Each has its own clock, so writes go through `mc_library_begin_write()`, which takes
the write lock and then catches the clock up to the newest logged hlc. That keeps the log
strictly increasing per device.

The triggers call SQL functions that musiccore registers (`mc_hlc_now()`, `mc_node_uid()`,
`mc_applying_remote()`). Any other tool that opens the database, such as the sqlite3 CLI,
will therefore fail on writes to tracked tables instead of silently making untracked
changes. That is deliberate.

## Migrations

`k_migrations[i]` takes the database from `user_version` *i* to *i + 1*. Each migration runs
in its own transaction. Only ever append. A database newer than the app is refused
(`MC_ERR_FORMAT`).
