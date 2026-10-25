# Library schema

The source of truth is [core/src/library/schema_v1.sql](../core/src/library/schema_v1.sql).
It is embedded into the binary at build time.

## Identity

Every syncable row has a `uid`: a UUIDv7 that is generated once and is the same on every
device. MusicBrainz ids (`mb_trackid`, `mb_albumid`, ...) are stored as attributes, never as
keys. Untagged files don't have them, and the same recording can appear on several releases.

When two devices independently import the same album, they create different uids. Merging
those duplicates (matched by MusicBrainz release id or by a content hash) is a sync-level
operation; see [sync-protocol.md](sync-protocol.md).

## Tables

| Table              | Synced | Notes |
|--------------------|--------|-------|
| `artists`          | yes    | |
| `albums`           | yes    | `art_hash` points into the cover-art cache |
| `tracks`           | yes    | |
| `playlists`        | yes    | |
| `playlist_entries` | yes    | ordered by the fractional-index string `position` |
| `files`            | no     | this device's files; `source_file_uid`/`source_peer_uid` mark transcodes received from a peer |
| `changelog`        | -      | field-level change log that sync is built on |
| `peers`            | no     | paired devices and their sync cursors |
| `meta`             | no     | `node_uid` and other per-device settings |

## Change tracking

`library.c` generates `AFTER INSERT/UPDATE/DELETE` triggers from its tracked-column list
every time the library is opened. Each change to a tracked column writes one `changelog`
row (`hlc`, `origin_uid`, `entity`, `row_uid`, `field`, `value`).

The triggers call SQL functions that musiccore registers (`mc_hlc_now()`, `mc_node_uid()`,
`mc_applying_remote()`). Any other tool that opens the database, such as the sqlite3 CLI,
will therefore fail on writes to tracked tables instead of silently making untracked
changes. That is deliberate.

## Migrations

`k_migrations[i]` takes the database from `user_version` *i* to *i + 1*. Each migration runs
in its own transaction. Only ever append. A database newer than the app is refused
(`MC_ERR_FORMAT`).
