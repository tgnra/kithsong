# Library schema

The source of truth is [core/src/library/schema_v1.sql](../core/src/library/schema_v1.sql)
(embedded into the binary at build time) plus the later migrations in `k_migrations` in
[core/src/library/library.c](../core/src/library/library.c).

| Version | Change |
|---------|--------|
| 1 | initial schema |
| 2 | singles: `albums.kind`, `tracks.year`, `tracks.mb_albumid` |
| 3 | `tracks.added_ms`, `track_plays` |
| 4 | `files.sha256`, `meta.library_uid`; `peers` dropped (sync data lives in `sync.db`) |
| 5 | `album_groups`, `album_group_entries` |
| 6 | `smart_lists`, `track_plays.last_played_ms` |
| 7 | `recents` |

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

The database has two identities of its own in `meta`, both generated on first open:

- `library_uid` says which library this is. It's the same on every device that syncs the
  library, and a sync group never touches a library with a different one. A device joining a
  group takes on the group's `library_uid` with `mc_library_adopt_uid()`, which only works
  while the library has no tracks, so two libraries are never merged.
- `node_uid` is the origin stamped on every change made here. If sync finds that another
  device already uses it (for example, the library folder was copied),
  `mc_library_rotate_node_uid()` gives the library a new one. Changes logged under the old
  one stay valid; to peers they're just another origin.

## Sync data

The library holds no sync data: keys, the group, its devices and their sync profiles live in
a separate `sync.db` in the per-device config dir. See
[sync-protocol.md](sync-protocol.md#two-databases) and its schema,
[core/src/sync/sync_schema_v1.sql](../core/src/sync/sync_schema_v1.sql), plus migration v2 in
[core/src/sync/sync_db.c](../core/src/sync/sync_db.c) (`devices.host`, `capabilities.addresses`,
and the local-only `peer_contact`).

## Singles

Tracks that aren't part of an album live in one album per artist with `kind = 'singles'`,
titled "&lt;artist&gt; Singles". Because the album has no year or release of its own, each of
its tracks carries `year` and `mb_albumid` (the release it came from). Regular albums have
`kind = 'album'`. See [tagging-rules.md](tagging-rules.md#singles).

## Play counts

A track's play count is `SUM(play_count)` over its `track_plays` rows. Each device keeps its
own row (`node_uid` is that device) and only ever updates that one, so field-level sync can't
lose plays made on two devices: no two devices ever write the same field. Rows are deleted
with their track. `mc_library_record_play()` counts a play.

A track's last play is `MAX(last_played_ms)` over the same rows, and comes from the same
per-device rows for the same reason. Rows from before v6 have no `last_played_ms` until
that device plays the track again.

## Album groups

An album group is a named, ordered list of albums: a playlist whose entries are albums
instead of tracks. `album_group_entries` works like `playlist_entries` (one row per entry,
ordered by `(position, uid)`), so concurrent edits merge the same way. Deleting an album
removes it from every group, and deleting a group removes its entries; the albums
themselves are untouched.

## Smart lists

A smart list is a playlist (`subject = 'tracks'`) or album group (`subject = 'albums'`)
defined by a rule instead of by hand. The rule is evaluated each time the list is listed,
against this device's data: nothing it selects is stored. Two devices can therefore differ
on what a list holds when a rule depends on local data, such as "on this device". The rule
is one JSON value in `smart_lists.rule`, so an edit syncs whole and two edits never merge
into a mix of both. The API is in `musiccore/smartlist.h`, and the evaluator in
`core/src/smart/smartlist.c`.

```json
{
  "v": 1,
  "match": {"all": [
    {"field": "genre", "op": "contains", "value": "jazz"},
    {"any": [
      {"field": "plays", "op": "<", "value": 3},
      {"not": {"field": "last_played", "op": "within", "value": {"days": 180}}}
    ]}
  ]},
  "order": [{"field": "plays", "dir": "asc"}, {"field": "added", "dir": "desc"}],
  "limit": 10
}
```

- **`v`**: the rule format's version, currently 1. A rule with a newer version, or a field
  or op this build doesn't know, is refused with `MC_ERR_FORMAT` rather than half-applied.
- **`match`** (optional; without it, everything matches) is one of the following.
  Nesting is limited to 32 levels.
  - A condition, `{"field", "op", "value"}`.
  - `{"all": [...]}`. An empty `all` matches everything.
  - `{"any": [...]}`. An empty `any` matches nothing.
  - `{"not": {...}}`.
- **Missing values:** a condition on a missing value is false, so `not` and the negative ops
  include it. "Not played within 180 days" includes tracks never played, and "genre is not
  rock" includes tracks with no genre.
- **`order`** (optional) is a list of sort keys, applied in turn.
  - A sort key is `{"field": <sortable field>, "dir": "asc"|"desc"}`; `dir` defaults to
    `asc`.
  - Text sorts ignore case, and `artist`/`album_artist` sort by the artist's sort name.
  - Missing values sort last in both directions.
  - After the keys, a list falls back to album grid order (tracks then follow album
    order), and a tie always breaks the same way.
- **Random order:** `{"field": "random", "seed": <int>}` must be the last order key.
  - It shuffles the list: the same seed gives the same order on every device.
  - `mc_smart_reshuffle()` stores a new seed.
- **`limit`** (optional) is a positive integer. It applies after the order, so
  `"order": [{"field": "plays"}], "limit": 10` is the 10 least played.

Fields (`T` for smart playlists, `A` for smart album groups). For albums the value is
worked out from the album's tracks.

| Field | Type | | Notes |
|-------|------|---|-------|
| `title` | text | T A | |
| `artist` | text | T A | track: the track's artist, else its album's; album: the album's |
| `album_artist` | text | T | |
| `album` | text | T | |
| `genre` | text | T A | album: true if any track matches; can't be sorted on |
| `year` | int | T A | track: the track's year, else its album's |
| `duration` | duration | T A | seconds; album: total |
| `track_no`, `disc_no` | int | T | |
| `track_count` | int | A | |
| `added` | date | T A | album: its newest track's |
| `plays` | int | T A | summed over devices; album: over its tracks |
| `last_played` | date | T A | newest over devices; album: over its tracks |
| `singles` | bool | T A | the track is on, or the album is, an artist's singles album |
| `on_device` | bool | T A | has a file on this device; album: any track does |
| `format` | format | T A | has a file in this format, e.g. `"flac"`; album: any track does |
| `playlist` | playlist | T | the track is in this playlist (uid) |
| `group` | group | T A | the track's album is, or the album is, in this album group (uid) |

Operators by type:
- **text:** `is`, `is_not`, `contains`, `not_contains`, `starts_with`, `empty`,
  `not_empty`. These ignore case (ASCII).
- **int, duration:** `=`, `!=`, `<`, `<=`, `>`, `>=`, `between` (value `[low, high]`,
  inclusive), `empty`, `not_empty`.
- **date:** `before`, `after` (a Unix ms value), `within`, `not_within` (value `{"days": n}`,
  counted back from now), `empty`, `not_empty`.
- **bool:** `is` (value `true` or `false`).
- **playlist, group, format:** `is`, `is_not`.

`empty` and `not_empty` take no value.

`mc_smart_field_count()`/`mc_smart_field_get()` return this table for a UI to build its
editor from.

A rule can't refer to another smart list. A playlist or group a rule names that has since
been deleted just matches nothing.

## Recents

`recents` holds what was last opened: albums, artists, playlists and album groups (smart
ones included), with `kind` naming which (`'album'`, `'artist'`, `'playlist'`,
`'album_group'`, as sync profile items do). A row's `uid` is the item's own uid, so an item
has one row and opening it on two devices writes the same fields: sync keeps the newer open,
and a removal wins over an earlier open. `opened_ms` is the wall-clock part of the hlc the
open is stamped with, so the newest change by hlc is also the newest `opened_ms`. There is
no foreign key, since a row may name any of four tables; listing leaves out rows whose item
is gone. The API is `mc_library_record_opened()`, `mc_library_list_recents()`,
`mc_library_remove_recent()` and `mc_library_clear_recents()`.

## Tables

| Table              | Synced | Notes |
|--------------------|--------|-------|
| `artists`          | yes    | |
| `albums`           | yes    | `art_hash` is the SHA-256 of the album folder's `cover.jpg` |
| `tracks`           | yes    | `added_ms`: when the track was first imported (Unix ms); NULL for tracks imported before v3 |
| `track_plays`      | yes    | play counts and last plays, one row per track per device; see [Play counts](#play-counts) |
| `playlists`        | yes    | |
| `playlist_entries` | yes    | ordered by the fractional-index string `position` |
| `album_groups`     | yes    | playlists of albums; see [Album groups](#album-groups) |
| `album_group_entries` | yes | ordered by `position`, like `playlist_entries` |
| `smart_lists`      | yes    | `subject` is `'tracks'` or `'albums'`; `rule` is JSON; see [Smart lists](#smart-lists) |
| `recents`          | yes    | last opened items, one row per item keyed by its uid; see [Recents](#recents) |
| `files`            | no     | this device's files, by path relative to the library folder; `source_file_uid`/`source_peer_uid` mark transcodes received from a peer; `sha256` (hex) of the file's bytes, recorded when sync transfers or verifies it |
| `changelog`        | -      | field-level change log that sync is built on |
| `meta`             | no     | `node_uid`, `library_uid` and other per-library settings; see [Identity](#identity) |

## Change tracking

`library.c` generates `AFTER INSERT/UPDATE/DELETE` triggers from its tracked-column list
every time the library is opened. Each change to a tracked column writes one `changelog`
row (`hlc`, `origin_uid`, `entity`, `row_uid`, `field`, `value`). The generator lives in
`core/src/util/changelog.c` and is shared with `sync.db`, whose changelog has the same shape.

The version vector sync exchanges (the newest hlc per origin) is computed from the changelog
each time, never stored, so it can't disagree with the data after a crash.

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
