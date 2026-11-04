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
| 8 | `aliases`, `merge_keys` (duplicate merging) |
| 9 | `tracks.file_rev`, `files.file_rev` (a track's file replaced by a better one) |
| 10 | example smart lists |
| 11 | `files.source_sha256` (the bytes an imported file came from) |
| 12 | no singles albums: `albums_singles` and `albums.kind`'s triggers dropped; no data changed |
| 13 | artist credits: `tracks.artist_credit`, `albums.artist_credit`, `track_artists`, `album_artists`, `meta.artist_refresh` |
| 14 | `recycle_bin` (deletes made here that no other device has heard of yet), `meta.changelog_epoch` |
| 15 | album flags: `albums.no_scrobble`, `albums.mix` |
| 16 | `albums.mix` is the user's choice, 1 or -1, and 0 leaves it to the album's length; long tracks with no album get one, once, on opening |

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
operation; see [sync-protocol.md](sync-protocol.md).

The database has two identities of its own in `meta`, both generated on first open:

- `library_uid` says which library this is. It's the same on every device that syncs the
  library, and a sync group never touches a library with a different one. A device joining a
  group takes on the group's `library_uid` with `mc_library_adopt_uid()`, which only works
  while the library has no tracks, so two changelogs are never merged. Pairing two libraries
  that both have music merges them another way: the smaller one is copied aside
  (`library-before-merge-<date>.db` in its folder), emptied in place without logging
  (`mc_library_reset_for_merge()`, which also gives it a new `node_uid`), joins as an empty
  library, and then, once its profile is confirmed, has the copy's current state imported as
  fresh edits; `meta.merge_source` names the copy until then, and `meta.merge_wait` says it
  still waits for the profile. If the pairing is abandoned meanwhile, the copy's tables
  replace the library's again, `meta` included, so the old `library_uid` and `node_uid` come
  back. No migration was needed. See
  [Merging libraries](sync-protocol.md#merging-libraries).
- `node_uid` is the origin stamped on every change made here. If sync finds that another
  device already uses it (for example, the library folder was copied),
  `mc_library_rotate_node_uid()` gives the library a new one. Changes logged under the old
  one stay valid; to peers they're just another origin.

## Sync data

The library holds no sync data: keys, the group, its devices and their sync profiles live in
a separate `sync.db` in the per-device config dir. See
[sync-protocol.md](sync-protocol.md#two-databases) and its schema,
[core/src/sync/sync_schema_v1.sql](../core/src/sync/sync_schema_v1.sql), plus the migrations in
[core/src/sync/sync_db.c](../core/src/sync/sync_db.c): v2 (`devices.host`,
`capabilities.addresses`, and the local-only `peer_contact`), v3 (`profiles.name`), v4
(`scrobble_account`, and the local-only `scrobble_queue`) and v5
(`devices.confirmed_profile`). Removing an album or artist from one device needed no
migration: it's a `profile_items` row of a new kind, `exclude_album` or `exclude_artist`
(see [Removing from one device](sync-protocol.md#removing-from-one-device)), which older
versions keep and ignore. A track's delete taken back out of the recycle bin is the same
with `exclude_track`.

## Deleted tracks

Deleting a track (`mc_delete_run()`, see
[musiccore/delete.h](../core/include/musiccore/delete.h)) moves its files to the Recycle Bin
or trash and deletes its rows like any other change, so the delete syncs. What's needed to
bring it back is kept out of the library, in `deleted.db` in the per-device config dir: one
row per trashed file with its old path, size, modification time, hash (if known), and the
rows it takes back (the track, its album and artists, both credits' rows and their artists,
plays, playlist and album group entries, the file's own row) as JSON objects of column to
value. Restoring inserts them
again with their old uids, using whichever columns the table still has, so records outlive
migrations. Records are dropped once their file is back, or after 90 days.

## Recycle bin

A device with no trash (Android) removes a deleted track's files outright. In a sync group,
where it isn't an archive, `mc_delete_run_to_bin()` keeps the delete in the library's
`recycle_bin` table as well, so it can be taken back until the next sync (see
[Recycle bin](sync-protocol.md#recycle-bin)). The table is this device's own: it isn't
logged, and its rows only make sense against this library's changelog. One row per delete:

| Column | |
|--------|---|
| `id` | the entry, `mc_bin_entry.id` |
| `deleted_ms` | when (Unix ms) |
| `kind` | `'albums'` or `'tracks'`: what was asked to be deleted |
| `items` | JSON array of the album or track uids asked for (the tracks that went, for `'tracks'`) |
| `title`, `artist` | the first item's title and artist credit as shown, for the bin's list |
| `tracks` | tracks the delete took out of the library |
| `seq_first`, `seq_last` | the `changelog` rows the delete logged: every one in between is the delete's, as it ran in one write transaction |
| `rows` | JSON array, one record per track as `deleted.db` keeps (the track, its album and artists, both credits' rows, plays, playlist and album group places), without the file's row |

The delete writes its row in the same transaction as the delete itself. Taking an entry back
(`mc_delete_bin_take()`, in one transaction) deletes its changelog rows, puts the rows back
with logging off (the changelog already holds their inserts and edits, as before the delete),
and deletes the entry. If a later entry's delete took a row this one's records hold (the album
of a track, gone with the album's last track), that entry is taken back too. A sync that sends
the library's rows deletes every entry first (`mc_delete_bin_seal()`), so a delete can't be
taken back once any other device may have it.

Taking changelog rows out can lower `max(seq)`, and `seq` (an `INTEGER PRIMARY KEY`, not
`AUTOINCREMENT`) is then given out again. Two readers count on it only growing: duplicate
merging, whose `meta.merge_seq` is wound back to before the rows taken out, and search, which
also compares `meta.changelog_epoch`, a counter each take bumps, so an index built before
isn't taken for current. Merging libraries' reset (`mc_library_reset_for_merge()`) empties the
bin with the changelog.


## Singles

A single is an album with exactly one track in the library: an ordinary `albums` row of the
release the track came from, or one whose other tracks aren't here. It's a count, not a
kind, so importing more of the release makes it an album. A track can also have no album
(`tracks.album_uid` NULL): a loose file imported as-is with no album tag. The album
listings, search and the artist's albums leave singles out;
`mc_library_list_artist_singles()` lists them, and the tracks with no album, for the
artist's page. See [tagging-rules.md](tagging-rules.md#singles). The exception is a mix of
one track, which is an album to all of them and not a single (see
[Album flags](#album-flags)).

Before v12 a loose track went into one album per artist, titled "&lt;artist&gt; Singles" with
`kind = 'singles'` (v2). v12 doesn't move or rewrite anything: such albums stay, as ordinary
albums of that title, until their music is imported again. `albums.kind` stays in the table
but nothing reads it, and it's no longer logged (its triggers and the `albums_singles` index
are dropped), so new albums keep the column's default, `'album'`. `tracks.year` and
`tracks.mb_albumid` stay too: every track has them.

## Album flags

What an album's tags can't say, set by hand (`mc_album_flags_set()`, `musiccore/edit.h`) and
never written to its files. They sync as logged fields of the album.

- `albums.no_scrobble`, 0 or 1: its tracks are never scrobbled nor sent as now playing, whichever
  frontend plays them; the scrobbler decides when it looks the track up, so a play queued
  before the flag was set still goes. Plays are still counted.
- `albums.mix`: a continuous mix, one piece of music split into tracks, or in one long track
  (a DJ mix). Its tracks aren't search results: a query matching one finds the album
  instead, once, scored as the best of the album and its tracks. Smart playlists never pick
  its tracks, whatever the rule; smart album groups can pick the album. A mix of one track
  is an album, not a single: album listings and counts have it, as the artist's albums, and
  the artist's singles don't. Everything else lists its tracks as any album's.

  1 is the user saying it's a mix, -1 saying it isn't, and 0 no one having said. With 0, an
  album whose only track is at least `MC_MIX_MIN_MS` long (30 minutes; the reasons are at its
  definition) is a mix. That's a default, worked out each time it's asked (`MC_ALBUM_IS_MIX`)
  from the track count and `duration_ms`, which sync: it's never stored, so there's nothing
  for two devices to disagree on, no migration to run for albums already there, and nothing
  to undo when more of the release is imported and the album stops being a single.
  `mc_album_flags_set()` only makes a choice of a value that differs from what the album is,
  so saving the dialog for the other flag leaves a detected mix detected. Once chosen, either
  way, length no longer counts, and there's no going back to 0.

A mix needs an album row. A track with no album (see [Singles](#singles)) gets one, of one
track, with the track's title, credit and year: at import if it's long enough; by hand when
it's made a mix (`mc_track_mix_set()`, which files it there like an edit); and for those
already in a library, once, on the first open after upgrading to v16. That last writes rows
only, leaving the file where it is, and is logged, as a migration's rows aren't: a device
paired later has no migration to run. Its uid is derived from the track's (`mc_uid_derive()`),
so devices upgrading apart make the same row and write the same values; the only row they
can't agree on is one the user edited before the other device had upgraded and heard of it.

Flags are logged when set, not when a row is made: an album inserted with a flag at 0 logs
nothing for it (`mc_changelog_table.flags`). So the only values in the changelog are
someone's choice, the last of which wins like any field's, and when duplicates merge a flag set on either comes through, however
much later the other was imported (see [Duplicates](sync-protocol.md#duplicates)). Importing
never touches them: an album imported again keeps its flags, and one whose last track an
import moves to another album passes them on as it goes. Merging libraries keeps a flag
either library's album had. In both, of two choices of `mix`, a 1 beats a -1.

## Artist credits

A track or album credited to "A feat. B" or "A, B & C" is each one's: every artist is an
`artists` row of their own (never one row for the whole credit), and the track lists on each
one's page. Where the credits come from and how a string is split is in
[tagging-rules.md](tagging-rules.md#artist-credits).

- `tracks.artist_credit` / `albums.artist_credit` is the credit as shown ("A feat. B"); NULL
  means the first artist's own name. Listings, search and smart lists show
  `coalesce(artist_credit, artist name)`, and a track with no artist of its own shows its
  album's.
- `tracks.artist_uid` / `albums.artist_uid` stay the **first** artist, so older versions and
  every query that knows one artist keep working.
- `track_artists` / `album_artists` hold the credit's artists in order: `(uid, track_uid` or
  `album_uid, artist_uid, position, joinphrase)`, `position` from 0, `joinphrase` what follows
  the artist in the credit (" feat. ", " & "; `''` after the last, never NULL, so a row
  written again logs every field). Deleting the owner or the artist deletes the row.
- A row's uid is derived from its owner and position (`mc_uid_derive()`: a version 8 UUID from
  the SHA-256 of `<owner uid>/<position>`), so two devices crediting one track write the same
  rows and field-level sync merges them. A shorter credit deletes the rows past its end; one
  that grows again brings them back under the same uids.
- What the credit tables say of the first artist is `artist_uid`, not row 0: row 0 repeats
  it and is kept for completeness, but a peer of an older version changes `artist_uid` alone,
  so readers take the first artist from `artist_uid` and only rows past 0 from the tables
  (`CREDITED_TRACKS` and `CREDITED_ALBUMS` in `library_internal.h`). A device that joins
  later has no row 0 for tracks credited before v13 (the backfill isn't logged); nothing
  needs it.
- An artist's albums, singles and tracks, and the artist list, go through the credit tables:
  an album or track is an artist's if they're its first artist or in its credit rows, or, for
  a track with no artist of its own, if its album is. `mc_library_list_credit()` lists a
  credit's artists, for a frontend to link each one.
- An import writes every credit it's given (each artist upserted as before: by MusicBrainz
  id, then by name); `mc_import_db_set_credit()` replaces one in place.

Migration v13 adds the columns and tables, writes one row 0 for every track and album with
an artist (unlogged, as every migration's rows are: each device derives the same ones), and
sets `meta.artist_refresh` to `'pending'` (`'done'` for a library with nothing in it). The
refresh then splits the credits already there, in the background, once: from MusicBrainz for
albums with a release id, else from the string, retagging the files whose credit changed
and deleting the artists left with nothing; see
[Refresh after upgrading](tagging-rules.md#refresh-after-upgrading). Rows a newer peer logged while this
device was older (an unknown table and column then; see
[Applying rows](sync-protocol.md#applying-rows)) are applied when the migration runs.

## Play counts

A track's play count is `SUM(play_count)` over its `track_plays` rows. Each device keeps its
own row (`node_uid` is that device) and only ever updates that one, so field-level sync can't
lose plays made on two devices: no two devices ever write the same field. Rows are deleted
with their track. `mc_library_record_play()` counts a play.

When a play counts is the player's call, so every frontend counts the same way and scrobbles
the same plays: half the track or four minutes heard, whichever is less
(`mc_play_counts()`), reported as `MC_PLAYER_EVENT_PLAYED`.


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
  - `{"all": [...]}`.
  - `{"any": [...]}`.
  - `{"not": {...}}`.
- **Empty groups are ignored:** a group with nothing in it, or nothing but ignored groups,
  is left out of the group it's in, as if it weren't there, and so is a `not` around one.
  So `{"all": [A, {"any": []}]}` is just A, and a `match` that's ignored altogether
  matches everything, as without one. An ignored group must still be valid.
- **Names:** a group may have a `"name"`, a label for an editor to show it by, which
  `mc_smart_describe()` also reads it by. It's only a label: nothing refers to it, names
  needn't be unique, and the rule matches the same without them.
  - It's on the group, `{"name": "Fresh stuff", "any": [...]}`, or on a `not` around a
    group, `{"name": "Old stuff", "not": {"any": [...]}}`: the latter is how an editor's
    "none of" group is written. A group right inside a `not` can't have one of its own.
  - It must be a string. A blank one (empty, or only spaces) is as good as none.
  - No other key goes beside `all`, `any` or `not`. Builds from before names refuse a rule
    with one, as one they can't read.
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
worked out from the album's tracks. A smart playlist never picks the tracks of a continuous
mix, whatever its rule says (see [Album flags](#album-flags)).

| Field | Type | | Notes |
|-------|------|---|-------|
| `title` | text | T A | |
| `artist` | text | T A | track: the track's artist credit, else its album's; album: the album's. True if it holds of the credit as shown ("A feat. B") or of any one of its artists, so `is "B"` finds what B is featured on; sorts by the first artist |
| `album_artist` | text | T | the track's album's credit, as `artist` |
| `album` | text | T | |
| `genre` | text | T A | album: true if any track matches; can't be sorted on |
| `year` | int | T A | track: the track's year, else its album's |
| `duration` | duration | T A | seconds; album: total |
| `track_no`, `disc_no` | int | T | |
| `track_count` | int | A | |
| `added` | date | T A | album: its newest track's |
| `plays` | int | T A | summed over devices; album: over its tracks |
| `last_played` | date | T A | newest over devices; album: over its tracks |
| `singles` | bool | T A | a single: the track is the only one of its album in the library, or has no album; the album has one track |
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
editor from, and `mc_smart_field_label()`, `mc_smart_op_label()` and `mc_smart_order_label()`
the words for its pickers ("Last played", "is less than", "least played first"), so every
platform uses the same ones. `mc_smart_describe()` puts a whole rule as one sentence in the
current language, e.g. "Tracks where genre contains “jazz” and (play count is less than 3 or
last played isn't in the last 180 days) · least played first, then newest added first · 10
tracks", naming playlists and groups by their current names. A named group inside the rule
reads as "it matches “Fresh stuff”", but the `match` itself, or a group that's all there is
to it, reads in full, so an editor can describe one group on its own as
`{"v": 1, "match": <group>}` (and count what it matches with `mc_smart_count()` the same
way). Its words are the `smart_*` strings in `strings/en.json`.

A rule can't refer to another smart list. A playlist or group a rule names that has since
been deleted just matches nothing.

Migration v10 adds example lists, so a library has some to open before there's an editor:
"Recently added", "Most played" and "Never played" (random order) playlists, and "Recently
added albums" and "Random albums" groups. Their uids are fixed (`5e1ec7ed-0000-4000-8000-00000000000N`),
so every device makes the same rows, and they aren't logged (no migration's rows are): a
rename, reshuffle or delete is logged against a uid every device already has. Their names
are English, as user data rather than UI strings.

## Recents

`recents` holds what was last opened: albums, artists, playlists and album groups (smart
ones included), with `kind` naming which (`'album'`, `'artist'`, `'playlist'`,
`'album_group'`, as sync profile items do). A row's `uid` is the item's own uid, so an item
has one row and opening it on two devices writes the same fields: sync keeps the newer open,
and a removal wins over an earlier open. `opened_ms` is the wall-clock part of the hlc the
open is stamped with, so the newest change by hlc is also the newest `opened_ms`. There is
no foreign key, since a row may name any of four tables; listing leaves out rows whose item
is gone. The API is `mc_library_record_opened()`, `mc_library_list_recents()`,
`mc_library_remove_recent()` and `mc_library_clear_recents()`. With `MC_LIST_PLAYABLE`,
listing also leaves out playlists, album groups and smart lists that are empty only on this
device, as selective sync leaves them: they have entries (or, for a smart one, its rule picks
something), but none has a `files` row here. What's empty everywhere is kept, since it's
waiting for tracks rather than missing them: a playlist or album group with no entries at
all, and a smart list whose rule picks nothing in the whole library, except on a device with
no `files` rows at all, so the example smart lists don't take the place of the empty pages. A
rule that can't be evaluated is left out. The playlist, album group and smart list listings
take the same flag; `files` being device-local is what makes the answer differ between
devices. Wear OS shows only what's present, so it drops the empty ones as well.

## Tables

| Table              | Synced | Notes |
|--------------------|--------|-------|
| `artists`          | yes    | one artist each, never a credit of several |
| `albums`           | yes    | `art_hash` is the SHA-256 of the album folder's `cover.jpg`. `artist_uid` is the first credited artist, `artist_credit` the credit as shown (NULL: that artist's name). `no_scrobble`, `mix`: see [Album flags](#album-flags) |
| `tracks`           | yes    | `added_ms`: when the track was first imported (Unix ms); NULL for tracks imported before v3. `file_rev`: a new uid each time an import replaces the track's original with a higher-quality file, NULL before the first time. `artist_uid` and `artist_credit` as for albums |
| `track_artists`    | yes    | a track's credited artists in order, uid derived from the track and position; see [Artist credits](#artist-credits) |
| `album_artists`    | yes    | an album's, the same way |
| `track_plays`      | yes    | play counts and last plays, one row per track per device; see [Play counts](#play-counts) |
| `playlists`        | yes    | |
| `playlist_entries` | yes    | ordered by the fractional-index string `position` |
| `album_groups`     | yes    | playlists of albums; see [Album groups](#album-groups) |
| `album_group_entries` | yes | ordered by `position`, like `playlist_entries` |
| `smart_lists`      | yes    | `subject` is `'tracks'` or `'albums'`; `rule` is JSON; see [Smart lists](#smart-lists) |
| `recents`          | yes    | last opened items, one row per item keyed by its uid; see [Recents](#recents) |
| `files`            | no     | this device's files, by path relative to the library folder; `source_file_uid`/`source_peer_uid` mark transcodes received from a peer; `sha256` (hex) of the file's bytes, recorded when sync transfers or verifies it; `file_rev` is the track's `file_rev` the file was made from (a copy whose `file_rev` differs from its track's is stale, see [Files](sync-protocol.md#files)); `source_sha256` (hex) of the bytes an original was imported from, before the import tagged it, which tells a re-import of the same file (see [Duplicates](tagging-rules.md#duplicates)), passed on with an original by sync |
| `aliases`          | no     | merged-away artist, album and track uids, each with the `canonical` uid it became; derived from the changelog on each device, see [Duplicates](sync-protocol.md#duplicates) |
| `merge_keys`       | no     | the MusicBrainz ids ever logged per uid, which decide what merges (rows of singles albums' artists from before v12 are left, unused) |
| `changelog`        | -      | field-level change log that sync is built on |
| `recycle_bin`      | no     | deletes made here that no other device has heard of yet, which can be taken back; see [Recycle bin](#recycle-bin) |
| `meta`             | no     | `node_uid`, `library_uid`, `merge_seq` (the changelog `seq` duplicate merging has read up to), `merge_source` (the set-aside library still to merge in, see [Identity](#identity)), `merge_wait` (set with it until this device's profile is confirmed: the merge waits for that, see [Merging libraries](sync-protocol.md#merging-libraries); its value is the run of the app that set it, and a later run finding it undoes the pairing), `merge_with` and `merge_name` (with them: the kept device's name and this device's own before pairing), `merge_restored` (the kept device's name, after a pairing was undone, until a frontend has said so), `merge_restore_sync` and `merge_restored_name` (while an undone pairing's `sync.db` and name are still to be put back; see "Abandoning a merge" there), `artist_refresh` (`'pending'` until the credits from before v13 are split, then `'done'`; see [Artist credits](#artist-credits)), while it's pending the refresh's cursors `artist_refresh_mb`, `artist_refresh_split`, `artist_refresh_loose` and its `artist_refresh_files` (see [Refresh after upgrading](tagging-rules.md#refresh-after-upgrading)), `changelog_epoch` (bumped each time a delete is taken back out of the recycle bin, which takes changelog rows out; see [Recycle bin](#recycle-bin)), `edit_swap` (while a hand edit swaps its files: what to put right after a crash; see [Hand edits](tagging-rules.md#hand-edits)), and other per-library settings |

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

A migration that adds a logged table or column also lists, in `k_newly_known`, which
changelog rows it makes known. Peers of a newer version may have sent such rows already,
which were logged but not applied; once the migration has run they're applied
(`mc_rows_reapply()`), so the device catches up without waiting for the next change.
