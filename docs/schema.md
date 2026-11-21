# Library schema

The source of truth is [core/src/library/schema_v1.sql](../core/src/library/schema_v1.sql)
(embedded into the binary at build time), plus the later migrations in `k_migrations` in
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
| 9 | `tracks.file_rev`, `files.file_rev` (for when a track's file is replaced by a better one) |
| 10 | example smart lists |
| 11 | `files.source_sha256` (the bytes an imported file came from) |
| 12 | singles albums removed: `albums_singles` and `albums.kind`'s triggers dropped. No data changed |
| 13 | artist credits: `tracks.artist_credit`, `albums.artist_credit`, `track_artists`, `album_artists` |
| 14 | `recycle_bin` (deletes made on this device that no other device knows about yet), `meta.changelog_epoch` |
| 15 | album flags: `albums.no_scrobble`, `albums.mix` |
| 16 | `albums.mix` becomes the user's choice (1 or -1), and 0 leaves it to the album's length. Long tracks with no album get an album, once, when the library opens |
| 17 | `albums.art_chosen`: the cover the user chose |
| 18 | `play_events`: the play log |
| 19 | `track_analysis`, `analysis_failed`: local audio analysis (both tables are unused) |
| 20 | index `changelog_origin`, for the version vector |

## Location

The database is `library.db` at the top of the library folder, next to the music it
describes (`mc_library_open()` takes the folder). `files.path` is relative to that folder
and always uses `/`, so you can move the folder or mount it somewhere else. `albums.art_hash`
is the SHA-256 of the album folder's `cover.jpg`. The folder itself is recorded in per-device
settings, not in the database. See
[tagging-rules.md](tagging-rules.md#library-folder).

## Identity

Every syncable row has a `uid`. It's a UUIDv7 that's generated once and is the same on every
device. MusicBrainz ids (`mb_trackid`, `mb_albumid`, ...) are stored as attributes, never as
keys, because untagged files don't have them, and the same recording can appear on several
releases.

When two devices import the same album separately, they create different uids. Merging those
duplicates (matched by MusicBrainz release id or by a content hash) happens at the sync
level. See [sync-protocol.md](sync-protocol.md).

The database itself also has two identities, stored in `meta`. Both are generated the first
time it's opened:

- `library_uid` identifies the library. It's the same on every device that syncs the
  library, and a sync group never touches a library with a different one. When a device
  joins a group, it takes on the group's `library_uid` with `mc_library_adopt_uid()`. This
  only works while the library has no tracks, so two changelogs are never merged, and two
  libraries that both have music never pair (see [Join rules](sync-protocol.md#join-rules)).
  Opening the library deletes some leftover keys from `meta` (they're listed in the `meta`
  row of the table at the end).
- `node_uid` is the origin stamped on every change made on this device. If sync finds that
  another device already uses it (for example, because the library folder was copied),
  `mc_library_rotate_node_uid()` gives the library a new one. Changes logged under the old
  one stay valid. Peers just see them as another origin.

## Sync data

The library doesn't hold any sync data. Keys, the group, its devices and their sync profiles
are kept in a separate `sync.db` in the per-device config dir. See
[sync-protocol.md](sync-protocol.md#two-databases) and its schema,
[core/src/sync/sync_schema_v1.sql](../core/src/sync/sync_schema_v1.sql), plus the migrations in
[core/src/sync/sync_db.c](../core/src/sync/sync_db.c):

- v2: `devices.host`, `capabilities.addresses`, and the local-only `peer_contact`
- v3: `profiles.name`
- v4: `scrobble_account`, and the local-only `scrobble_queue`
- v5: `devices.confirmed_profile`
- v6: `scrobble_queue.track_uid`
- v7: index `changelog_origin`
- v8: the local-only `peer_lacks` (see
  [Waiting for a device](sync-protocol.md#waiting-for-a-device))
- v9: `peer_lacks.has_copy` and `said_ms`, `local.deep_check_ms`, `deep_check_at` and
  `unknown_files` (see [Verification](sync-protocol.md#verification))
- v10: the local-only `tethered_devices`, `tethered_profile_items` and `tethered_imports`
- v11: drops those three again
- v12: `local.profile_chosen` (see [Choosing a profile](sync-protocol.md#choosing-a-profile)).
  `devices.confirmed_profile` is left unused.

Removing an album or artist from one device doesn't have a table of its own. It's a
`profile_items` row of kind `exclude_album` or `exclude_artist` (see
[Removing from one device](sync-protocol.md#removing-from-one-device)). A track's delete
that's taken back out of the recycle bin works the same way, with `exclude_track`.

## Deleted tracks

Deleting a track (`mc_delete_run()`, see
[kithcore/delete.h](../core/include/kithcore/delete.h)) moves its files to the Recycle Bin
or trash, and deletes its rows like any other change, so the delete syncs. Everything needed
to bring the track back is kept outside the library, in `deleted.db` in the per-device config
dir. There's one row per trashed file, holding:

- its old path, size, modification time and hash (if known)
- the rows it brings back, as JSON objects of column to value: the track, its album and
  artists, both credits' rows and their artists, plays, `track_analysis` rows, playlist and
  album group entries, and the file's own row. `features` is stored as hex, since JSON has
  no BLOBs.

Restoring inserts the rows again with their old uids, using whichever columns the table still
has, so records survive migrations. Records are dropped once their file is back, or after 90
days.

## Recycle bin

A device with no trash (Android) removes a deleted track's files completely. If it's in a
sync group and isn't an archive, `mc_delete_run_to_bin()` also keeps the delete in the
library's `recycle_bin` table, so it can be undone until the next sync (see
[Recycle bin](sync-protocol.md#recycle-bin)). The table belongs to this device only. It isn't
logged, and its rows only make sense against this library's changelog. There's one row per
delete:

| Column | |
|--------|---|
| `id` | the entry, `mc_bin_entry.id` |
| `deleted_ms` | when it was deleted (Unix ms) |
| `kind` | `'albums'` or `'tracks'`: what the user asked to delete |
| `items` | JSON array of the album or track uids asked for (for `'tracks'`, the tracks that went) |
| `title`, `artist` | the first item's title and artist credit as shown, for the bin's list |
| `tracks` | tracks the delete removed from the library |
| `seq_first`, `seq_last` | the `changelog` rows the delete logged. Every row in between is the delete's, since it ran in one write transaction |
| `rows` | JSON array with one record per track, in the form `deleted.db` keeps (the track, its album and artists, both credits' rows, plays, audio analysis, playlist and album group places), without the file's row |

The delete writes its row in the same transaction as the delete itself. Restoring an entry
(`mc_delete_bin_take()`, in one transaction):

1. deletes its changelog rows
2. puts the rows back with logging off (the changelog already holds their inserts and edits
   from before the delete)
3. deletes the entry

If a later entry's delete removed a row that this entry's records hold (for example, a
track's album, which went with the album's last track), that entry is restored too. A sync
that sends the library's rows deletes every entry first (`mc_delete_bin_seal()`), so a delete
can't be undone once another device might have it.

Removing changelog rows can lower `max(seq)`, and then `seq` (an `INTEGER PRIMARY KEY`, not
`AUTOINCREMENT`) gets reused. Two readers rely on it only ever growing:

- duplicate merging, whose `meta.merge_seq` and `meta.merge_name_seq` are wound back to
  before the removed rows
- search, which also compares `meta.changelog_epoch`, a counter that each restore bumps, so
  an index built earlier isn't mistaken for a current one

## Singles

A single is an album with exactly one track in the library. It's an ordinary `albums` row for
the release the track came from, or one whose other tracks aren't here. It's a count, not a
kind, so importing more of the release turns it into an album. A track can also have no
album (`tracks.album_uid` NULL). That's a loose file imported unchanged, with no album tag.
The album listings, search and the artist's albums leave singles out.
`mc_library_list_artist_singles()` lists them, plus the tracks with no album, for the
artist's page. See [tagging-rules.md](tagging-rules.md#singles). The exception is a mix of
one track. All of these treat it as an album, not a single (see
[Album flags](#album-flags)).

Before v12, a loose track went into one album per artist, titled "&lt;artist&gt; Singles",
with `kind = 'singles'` (v2). v12 doesn't move or rewrite anything. These albums stay, as
ordinary albums with that title, until their music is imported again. `albums.kind` stays in
the table, but nothing reads it and it isn't logged (its triggers and the
`albums_singles` index are dropped), so new albums keep the column's default, `'album'`.
`tracks.year` and `tracks.mb_albumid` stay too, since every track has them.

## Album flags

Album flags are things an album's tags can't say. They're set by hand
(`mc_album_flags_set()`, `kithcore/edit.h`) and never written to the album's files. They sync
as logged fields of the album.

- `albums.no_scrobble`, 0 or 1: the album's tracks are never scrobbled or sent as now
  playing, whichever frontend plays them. The scrobbler checks when it looks the track up,
  and again before it sends its queue, so a play that's waiting in the queue when the flag is
  set isn't sent. Plays are still counted.
- `albums.mix`: a continuous mix, either one piece of music split into tracks, or one long
  track (a DJ mix). Its tracks don't show up in search results. A query that matches one
  finds the album instead, once, scored as the best match of the album and its tracks. An
  artist's page doesn't list them among the artist's tracks either, only the album. Smart
  playlists never pick its tracks, whatever the rule, but smart album groups can pick the
  album. A mix of one track is an album, not a single. It's in album listings and counts, and
  in the artist's albums, but not in the artist's singles. Everything else lists its tracks
  like any album's. The album's page says "Continuous mix" (`mc_album_info.mix`).

  1 means the user said it's a mix, -1 means they said it isn't, and 0 means nobody has said.
  With 0, an album whose only track is at least `MC_MIX_MIN_MS` long (30 minutes; the reasons
  are given at its definition) is a mix. That's a default, worked out each time it's needed
  (`MC_ALBUM_IS_MIX`) from the track count and `duration_ms`, which both sync. It's never
  stored, so:

  - there's nothing for two devices to disagree on
  - there's no migration to run for albums that are already there
  - there's nothing to undo when more of the release is imported and the album stops being a
    single

  `mc_album_flags_set()` only records a choice if the value differs from what the album
  already is, so saving the dialog for the other flag leaves a detected mix as detected. Once
  a choice is made, either way, length no longer counts, and it can't go back to 0.

A mix needs an album row. A track with no album (see [Singles](#singles)) gets a one-track
album with the track's title, credit and year:

- at import, if it's long enough
- by hand, when it's made a mix (`mc_track_mix_set()`, which files it there like an edit,
  and takes it out again when the choice is undone and the album has no other flags set)
- for tracks already in a library, once, on the first open after upgrading to v16

That last case only writes rows, and leaves the file where it is. Unlike a migration's rows,
these rows are logged, because a device paired later has no migration to run. The album's
uid is derived from the track's (`mc_uid_derive()`), so devices that upgrade at different
times make the same row and write the same values. The only row they can't agree on is one
the user edited before the other device had upgraded and heard about it.

Flags are logged when they're set, not when a row is created. An album inserted with a flag
at 0 logs nothing for it (`mc_changelog_table.flags`). So the only values in the changelog
are someone's choices. The last one wins, like any field, and when duplicates merge, a flag
set on either one comes through, however much later the other was imported (see
[Duplicates](sync-protocol.md#duplicates)). Importing never touches flags. An album that's
imported again keeps its flags, and an album whose last track is moved to another album by
an import passes them on. Between two choices of `mix`, 1 beats -1.

## Artist credits

A track or album credited to "A feat. B" or "A, B & C" belongs to each of them. Every artist
has their own `artists` row (never one row for the whole credit), and the track is listed on
each artist's page. [tagging-rules.md](tagging-rules.md#artist-credits) explains where the
credits come from and how a string is split.

- `tracks.artist_credit` / `albums.artist_credit` is the credit as shown ("A feat. B"). NULL
  means the first artist's name. Listings, search and smart lists show
  `coalesce(artist_credit, artist name)`, and a track without an artist shows its album's.
- `tracks.artist_uid` / `albums.artist_uid` stay the **first** artist, so every query that
  only knows about one artist keeps working.
- `track_artists` / `album_artists` hold the credit's artists in order: `(uid, track_uid` or
  `album_uid, artist_uid, position, joinphrase)`. `position` starts from 0. `joinphrase` is
  what follows the artist in the credit (" feat. ", " & "). It's `''` after the last artist
  and never NULL, so a row that's written again logs every field. Deleting the owner or the
  artist deletes the row.
- A row's uid is derived from its owner and position (`mc_uid_derive()`: a version 8 UUID
  from the SHA-256 of `<owner uid>/<position>`). So two devices crediting the same track
  write the same rows, and field-level sync merges them. A shorter credit deletes the rows
  past its end, and if the credit grows again they come back under the same uids.
- For the first artist, the credit tables go by `artist_uid`, not row 0. Row 0 repeats it
  and is kept for completeness, but a peer with a schema before v13 only changes
  `artist_uid`. So readers take the first artist from `artist_uid`, and only the rows after 0
  from the tables
  (`CREDITED_TRACKS` and `CREDITED_ALBUMS` in `library_internal.h`). A device that joins
  later has no row 0 for tracks credited before v13 (the backfill isn't logged), but nothing
  needs it.
- An artist's albums, singles and tracks, and the artist list, all go through the credit
  tables. An album or track belongs to an artist if they're its first artist or in its credit
  rows. A track without an artist belongs to an artist if its album does.
  `mc_library_list_credit()` lists a credit's artists, so a frontend can link to each one.
- An import writes every credit it's given (each artist is upserted as before: by
  MusicBrainz id, then by name). `mc_import_db_set_credit()` replaces one in place.

Migration v13 adds the columns and tables, and writes a row 0 for every track and album with
an artist. Like every migration's rows, these aren't logged, because each device derives the
same ones. Existing credits stay as they were imported. A track tagged "A feat. B" before
v13 keeps a single artist with that name until it's imported again or edited by hand. Rows
that a peer logged while this library's schema was still before v13
(an unknown table and column at the time; see
[Applying rows](sync-protocol.md#applying-rows)) are applied when the migration runs.

## Play counts

A track's play count is `SUM(play_count)` over its `track_plays` rows. Each device keeps its
own row (`node_uid` is that device) and only ever updates that row. No two devices ever
write the same field, so field-level sync can't lose plays made on two devices. Rows are
deleted with their track. `mc_library_record_play()` counts a play.

The player decides when a play counts, so every frontend counts the same way and scrobbles
the same plays. A play counts after half the track or four minutes heard, whichever is less
(`mc_play_counts()`), and it's reported as `MC_PLAYER_EVENT_PLAYED`.

A track's last play is `MAX(last_played_ms)` over the same per-device rows, for the same
reason. Rows from before v6 have no `last_played_ms` until that device plays the track
again.

## Play log

`play_events` is the raw history that `track_plays` sums up. There's one row for each time a
track was started, whether or not it counted as a play. Readers either go through
`mc_library_list_play_events()`, which returns every device's rows in a time range, oldest
first, like the Home page does (see [Home's played rows](#homes-played-rows)), or read the
table directly. Nothing here filters or interprets the rows. A reader decides what a
two-second skip means. None of this leaves the user's own devices.

| Column | |
|--------|-|
| `uid` | the row's uid |
| `track_uid` | the track. The row is deleted with it |
| `node_uid` | the device it was played on |
| `started_ms` | wall clock time when it was first heard, or when it was started if it never was |
| `listened_ms` | time actually heard. Pauses add nothing. A seek forward adds nothing for the part it skips, and a seek back adds what's heard again, so it can be longer than the track. A track that's started and left straight away has 0 or close to it |
| `ended` | `finished`, `skipped` (next, previous, another queue item, or started again), `stopped` (stopped, or the app or library closed), `replaced` (a new queue took over) or `failed` (the file couldn't be played, and `listened_ms` is 0) |
| `context` | how the queue was started: `album`, `playlist` (smart lists too), `album_group`, `artist`, `manual` (one track by itself), `shuffle` (any of those started shuffled with no track picked), `device` (played on a portable player, such as an iPod, and brought in when it synced; nothing logs it now) or `unknown`. A row may also say `radio`, which reads as `unknown` |
| `context_uid` | the album, playlist, group or artist (also under `shuffle`), a track for `radio`, or the portable player for `device` (its uid in `sync.db` before v11). NULL if there isn't one. It's not a reference: the history stays when the album or playlist is deleted, and a merged-away album's uid can be looked up in `aliases` |

The player keeps the rows, so all frontends log the same way. A frontend:

- gives it the queue's track uids and context (`mc_play_origin`)
- takes each row as it ends (`MC_PLAYER_EVENT_ENDED`, `mc_player_take_ended()`) and writes it
  (`mc_library_log_play()`)
- calls `mc_player_end_play()` before closing, so the current play is written as `stopped`

If the app is killed outright, it loses that one play.

Rows sync like `track_plays`, but more simply. Each row belongs to one device and is written
once, never updated, so no two devices ever write the same field. That means a row is logged
whole (see [Change tracking](#change-tracking)). Rows are deleted with their track, and come
back with it from the Recycle Bin. A duplicate's rows move to the track that's kept, and
that move isn't logged.

Rows are kept forever. Each row takes about 700 bytes of `library.db`: 350 for the row and
its indexes, and the rest for its changelog row. So 10,000 starts a year (about 30 a day) add
up to around 7 MB, on every device in the group, no matter which device played what.

### Home's played rows

"Played a lot recently" and "Not played in a while" come from `pick_played_lately()` and
`pick_not_played_lately()` in [`home_picks.c`](../core/src/library/home_picks.c). Nothing is
stored. Each load reads the last 90 days of the log, however long it's grown, and uses
`track_plays` for anything older. A track's plays count towards its album and the playlists
it's in right now, however it was played, so `context_uid` isn't used.

**A real play** is a row that didn't fail, with 30 seconds heard, or half the track if that's
shorter. Otherwise it doesn't matter how it ended. A skip after two minutes was still
listened to, and `finished` can be a seek to the end. The half-a-track rule is for
interludes, which never reach 30 seconds. It's less than `mc_play_counts()` asks for,
because that's about what's fair to scrobble, and a minute of a long track says the album
was on.

**Played a lot recently** takes, in order:

1. Three or more real plays in the last 14 days, most plays first.
2. From before the log began: three or more tracks last played in those 14 days.

Then, while the row has fewer than 8:

3. A real play in the last 14 days.
4. From before the log began: a track last played in those 14 days.
5. A real play in the last 90 days.
6. From before the log began: a track last played in those 90 days.

So a single play after years of silence comes after anything on repeat, and only gets in if
there's room.

**Not played in a while** shows what has a play, from either source, but none in the last 90
days. The longest ago comes first, with those that had two or more tracks played ahead of the
rest. A skip or a failure in the log doesn't change anything. If there are fewer than 8, it
carries on down to 14 days ago, leaving out anything that's in the other row.

**Before the log began.** `track_plays` is all there is for older plays, so the "from before
the log began" steps rely on its guess. It can only tell how many tracks were last played
recently, not how often they were played. The guess is only used for an album or playlist
whose last play in `track_plays` is older than the log's oldest row, from any device and of
any kind. After that, the log has seen the play and knows better. So:

- an empty log uses the guess for everything
- a short log puts its own rows first and the guess behind them
- a log that's older than 90 days leaves the guess out of "Played a lot recently" completely

If a device only started logging after another one, its earlier plays are treated as seen,
and they drop out of that row until they're older than the windows anyway.

## Audio analysis

Schema v19's `track_analysis` and `analysis_failed` tables are unused, but they're kept:

- `track_analysis` holds the results of audio analysis (timbre, rhythm and loudness), and
  syncs.
- `analysis_failed` lists files that wouldn't decode, and is kept only on this device.

Keeping them means every library has the same schema, and `track_analysis` rows from peers
still apply and get passed on. Nothing reads them. The only things that write them are sync
and the deletes that go with a track. A later migration could drop them once no peer sends
them.

## Stats

`mc_library_read_stats()` (`kithcore/library.h`) adds up everything above in one read query.
Nothing is stored for it, so there's nothing to keep up to date or sync. Tracks, albums,
singles, artists, genres and playing time are each counted twice: once for the whole library,
and once for what has a file on this device, since a device doesn't need to hold all of it.
Albums, singles and artists are counted the same way the listings show them, so the numbers
match what a frontend shows. Size on disk is the sum of `files.size_bytes`, including a
track's transcodes. Plays come from `track_plays`. Time listened comes from `play_events`, so
it starts at v18 and is an undercount for a library older than that.

## Album groups

An album group is a named, ordered list of albums. It's a playlist whose entries are albums
instead of tracks. `album_group_entries` works like `playlist_entries` (one row per entry,
ordered by `(position, uid)`), so edits made at the same time merge the same way. Deleting
an album removes it from every group, and deleting a group removes its entries. The albums
themselves aren't touched.

## Smart lists

A smart list is a playlist (`subject = 'tracks'`) or album group (`subject = 'albums'`)
that's defined by a rule instead of by hand. The rule is evaluated against this device's data
each time the list is listed, and nothing it selects is stored. So two devices can disagree
on what a list holds when a rule depends on local data, such as "on this device". The rule
is a single JSON value in `smart_lists.rule`, so an edit syncs whole, and two edits never
merge into a mix of both. The API is in `kithcore/smartlist.h`, and the evaluator is in
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
  or op this build doesn't know, is refused with `MC_ERR_FORMAT` instead of being
  half-applied.
- **`match`** (optional; without it, everything matches) is one of the following.
  Nesting is limited to 32 levels.
  - A condition, `{"field", "op", "value"}`.
  - `{"all": [...]}`.
  - `{"any": [...]}`.
  - `{"not": {...}}`.
- **Empty groups are ignored:** a group that's empty, or only holds ignored groups, is left
  out of its parent group as if it weren't there. The same goes for a `not` around one. This
  means `{"all": [A, {"any": []}]}` is just A, and a `match` that's ignored completely
  matches everything, like having no `match`. An ignored group still has to be valid.
- **Names:** a group can have a `"name"`. It's a label for an editor to show the group by,
  and `mc_smart_describe()` uses it too. It's only a label: nothing refers to it, names
  don't need to be unique, and the rule matches the same without them.
  - It goes on the group, `{"name": "Fresh stuff", "any": [...]}`, or on a `not` around a
    group, `{"name": "Old stuff", "not": {"any": [...]}}`. The second form is how an
    editor writes a "none of" group. A group directly inside a `not` can't have its own name.
  - It must be a string. A blank one (empty, or only spaces) counts as no name.
  - No other key goes beside `all`, `any` or `not`.
- **Missing values:** a condition on a missing value is false, so `not` and the negative ops
  include it. "Not played within 180 days" includes tracks that were never played, and
  "genre is not rock" includes tracks with no genre.
- **`order`** (optional) is a list of sort keys, applied in turn.
  - A sort key is `{"field": <sortable field>, "dir": "asc"|"desc"}`. `dir` defaults to
    `asc`.
  - Text sorts ignore case, and `artist`/`album_artist` sort by the artist's sort name.
  - Missing values sort last in both directions.
  - After the keys, a list falls back to album grid order (tracks then follow album
    order), and ties always break the same way.
- **Random order:** `{"field": "random", "seed": <int>}` must be the last order key.
  - It shuffles the list, and the same seed gives the same order on every device.
  - `mc_smart_reshuffle()` stores a new seed.
- **`limit`** (optional) is a positive integer. It applies after the order, so
  `"order": [{"field": "plays"}], "limit": 10` is the 10 least played.
- **`limit_duration`** (optional, can't be used with `limit`) is a positive number of
  seconds, so "an hour of music" is `3600`. It also applies after the order. It keeps tracks
  or albums while their running total is within the limit, and stops at the first one that
  would go over. The first one is always kept, whatever its length, so a limit never empties
  a list. An album's length is the total of its tracks.

Fields (`T` for smart playlists, `A` for smart album groups). For albums, the value is
worked out from the album's tracks. A smart playlist never picks the tracks of a continuous
mix, whatever its rule says (see [Album flags](#album-flags)).

| Field | Type | | Notes |
|-------|------|---|-------|
| `title` | text | T A | |
| `artist` | text | T A | track: the track's artist credit, else its album's; album: the album's. True if it matches the credit as shown ("A feat. B") or any one of its artists, so `is "B"` finds what B is featured on. Sorts by the first artist |
| `album_artist` | text | T | the track's album's credit, like `artist` |
| `album` | text | T | |
| `genre` | text | T A | album: true if any track matches; can't be sorted on |
| `year` | int | T A | track: the track's year, else its album's |
| `duration` | duration | T A | seconds; album: total |
| `track_no`, `disc_no` | int | T | |
| `track_count` | int | A | |
| `added` | date | T A | album: its newest track's |
| `plays` | int | T A | summed over devices; album: over its tracks |
| `last_played` | date | T A | newest over devices; album: over its tracks |
| `singles` | bool | T A | a single: the track is the only one from its album in the library, or has no album; the album has one track |
| `on_device` | bool | T A | has a file on this device; album: any track does |
| `format` | format | T A | has a file in this format, e.g. `"flac"`; album: any track does |
| `playlist` | playlist | T | the track is in this playlist (uid) |
| `group` | group | T A | the track's album, or the album, is in this album group (uid) |

Operators by type:
- **text:** `is`, `is_not`, `contains`, `not_contains`, `starts_with`, `empty`,
  `not_empty`. These ignore case (ASCII).
- **int, duration:** `=`, `!=`, `<`, `<=`, `>`, `>=`, `between` (value `[low, high]`,
  inclusive), `empty`, `not_empty`.
- **date:** `before`, `after` (a Unix ms value), `within`, `not_within` (value `{"days": n}`,
  counted back from now), `empty`, `not_empty`.
- **bool:** `is` (value `true` or `false`).
- **playlist, group, format:** `is`, `is_not`.

`empty` and `not_empty` don't take a value.

`mc_smart_field_count()`/`mc_smart_field_get()` return this table so a UI can build its
editor from it. `mc_smart_field_label()`, `mc_smart_op_label()` and `mc_smart_order_label()`
return the words for its pickers ("Last played", "is less than", "least played first"), so
every platform uses the same ones. `mc_smart_describe()` turns a whole rule into one sentence
in the current language, e.g. "Tracks where genre contains “jazz” and (play count is less
than 3 or last played isn't in the last 180 days) · least played first, then newest added
first · 10 tracks", naming playlists and groups by their current names. A named group inside
the rule reads as "it matches “Fresh stuff”". The `match` itself, or a group that makes up
the whole of it, is described in full. So an editor can describe one group by itself as
`{"v": 1, "match": <group>}`, and count what it matches with `mc_smart_count()` the same
way. The words come from the `smart_*` strings in `strings/en.json`.

A rule can't refer to another smart list. If a rule names a playlist or group that's since
been deleted, that part just matches nothing.

Migration v10 adds example lists, so a library has some to open before there's an editor:

- "Recently added", "Most played" and "Never played" (random order) playlists
- "Recently added albums" and "Random albums" groups

Their uids are fixed (`5e1ec7ed-0000-4000-8000-00000000000N`), so every device makes the same
rows. They aren't logged (no migration's rows are), so a rename, reshuffle or delete is
logged against a uid that every device already has. The rows hold English names. Until a
list is renamed, listings, recents and search show it using a UI string instead, so it
appears in the current language (`k_examples` in `smartlist.c`).

## Recents

`recents` holds what was last opened: albums, artists, playlists and album groups (including
smart ones). `kind` says which it is (`'album'`, `'artist'`, `'playlist'`, `'album_group'`,
the same as sync profile items). Rows of kind `'station'`, keyed by a track's uid, are kept
and synced, but they aren't listed.

A row's `uid` is the item's own uid, so each item has one row, and opening it on two devices
writes the same fields. Sync keeps the newer open, and a removal wins over an earlier open.
`opened_ms` is the wall-clock part of the hlc the open is stamped with, so the newest change
by hlc is normally the newest `opened_ms`. The exception is a row that a merge carried over.
It keeps the old library's `opened_ms` under the merge's hlc, so an open that raced the merge
can lose to an older one. Listing goes by `opened_ms` alone, so the order stays right, and
the next open fixes it.

There's no foreign key, since a row can name any of five tables. Listing leaves out rows
whose item has been deleted. The API is `mc_library_record_opened()`,
`mc_library_list_recents()`, `mc_library_remove_recent()` and `mc_library_clear_recents()`.

With `MC_LIST_PLAYABLE`, listing also leaves out playlists, album groups and smart lists that
are empty only on this device, because of selective sync. They have entries (or for a smart
list, its rule picks something), but none of them has a `files` row here. Lists that are
empty everywhere are kept, since they're waiting for tracks rather than missing them:

- a playlist or album group with no entries at all
- a smart list whose rule picks nothing in the whole library, except on a device with no
  `files` rows at all, so the example smart lists don't take the place of the empty pages

A rule that can't be evaluated is left out. The playlist, album group and smart list listings
take the same flag. Because `files` is device-local, the answer can differ between devices.
`mc_library_list_smart_keeping()` keeps one smart list whatever the answer, so a smart list
that's hidden here but opened from a search or just created still has a tile to open.

`kind` is free text that nothing checks, so a new kind doesn't need a schema version. An
older app stores and passes on the row like any other, and its listing leaves the row out,
since each kind only joins its own table. That's why kinds are only ever added.

## Tables

| Table              | Synced | Notes |
|--------------------|--------|-------|
| `artists`          | yes    | one artist each, never a credit of several |
| `albums`           | yes    | `art_hash` is the SHA-256 of the album folder's `cover.jpg`. `artist_uid` is the first credited artist, and `artist_credit` the credit as shown (NULL: that artist's name). `no_scrobble`, `mix`: see [Album flags](#album-flags). `art_chosen` is the SHA-256 of a cover the user chose, or NULL. Import and downloads leave that cover alone, and sync brings it to devices showing a different one (see "A cover the user chose" in [tagging-rules.md](tagging-rules.md)) |
| `tracks`           | yes    | `added_ms`: when the track was first imported (Unix ms), NULL for tracks imported before v3. `file_rev`: a new uid each time an import replaces the track's original with a higher-quality file, NULL until the first time. `artist_uid` and `artist_credit` as for albums |
| `track_artists`    | yes    | a track's credited artists in order, uid derived from the track and position; see [Artist credits](#artist-credits) |
| `album_artists`    | yes    | an album's credited artists, the same way |
| `track_plays`      | yes    | play counts and last plays, one row per track per device; see [Play counts](#play-counts) |
| `play_events`      | yes    | one row each time a track was started, each belonging to one device; see [Play log](#play-log) |
| `playlists`        | yes    | |
| `playlist_entries` | yes    | ordered by the fractional-index string `position` |
| `album_groups`     | yes    | playlists of albums; see [Album groups](#album-groups) |
| `album_group_entries` | yes | ordered by `position`, like `playlist_entries` |
| `smart_lists`      | yes    | `subject` is `'tracks'` or `'albums'`; `rule` is JSON; see [Smart lists](#smart-lists) |
| `recents`          | yes    | last opened items, one row per item keyed by its uid; see [Recents](#recents) |
| `track_analysis`   | yes    | unused; see [Audio analysis](#audio-analysis) |
| `files`            | no     | this device's files, by path relative to the library folder. `source_file_uid`/`source_peer_uid` mark transcodes received from a peer. `sha256` (hex) is of the file's bytes, recorded when sync transfers or verifies it. `file_rev` is the track's `file_rev` the file was made from (a copy whose `file_rev` differs from its track's is stale, see [Files](sync-protocol.md#files)). `source_sha256` (hex) is of the bytes an original was imported from, before the import tagged it. It identifies a re-import of the same file (see [Duplicates](tagging-rules.md#duplicates)), and sync passes it on with an original |
| `aliases`          | no     | merged-away artist, album and track uids, each with the `canonical` uid it became. Derived from the changelog on each device, see [Duplicates](sync-protocol.md#duplicates) |
| `merge_keys`       | no     | the MusicBrainz ids and `merge_name`s ever logged per uid, which decide what merges (rows for singles albums' artists from before v12 are left, unused) |
| `changelog`        | -      | field-level change log that sync is built on |
| `analysis_failed`  | no     | unused, like `track_analysis`; see [Audio analysis](#audio-analysis) |
| `file_failed`      | no     | originals imported here that failed a check; see [Failed originals](sync-protocol.md#failed-originals) |
| `recycle_bin`      | no     | deletes made here that no other device knows about yet, which can be undone; see [Recycle bin](#recycle-bin) |
| `meta`             | no     | `node_uid`, `library_uid`, `merge_seq` (the changelog `seq` duplicate merging has read up to), `merge_name_seq` (the `seq` names were last logged at, see [Duplicates](sync-protocol.md#duplicates)), `changelog_epoch` (bumped each time a delete is restored from the recycle bin, which removes changelog rows; see [Recycle bin](#recycle-bin)), `edit_swap` (while a hand edit swaps its files, what to fix after a crash; see [Hand edits](tagging-rules.md#hand-edits)), `stress_library` (the preset, in a generated stress test library; sync and scrobbling refuse such a library, see `kithcore/stress.h`), and other per-library settings. Opening the library deletes the leftover keys `merge_source`, `merge_wait`, `merge_with`, `merge_name`, `merge_restored`, `merge_restore_sync` and `merge_restored_name`, and `artist_refresh` and its cursors |

## Change tracking

`library.c` generates `AFTER INSERT/UPDATE/DELETE` triggers from its list of tracked columns
every time the library is opened. Each change to a tracked column writes one `changelog`
row (`hlc`, `origin_uid`, `entity`, `row_uid`, `field`, `value`). The generator is in
`core/src/util/changelog.c` and is shared with `sync.db`, whose changelog has the same shape.

A table whose rows are written once and never edited can log whole rows instead
(`mc_changelog_whole_row`; currently only `play_events`). Inserting a row writes one
`changelog` row with `field` = `row` and `value` = a JSON array of the tracked columns in
their listed order. The array can only be appended to: a missing element at the end reads as
NULL, and extra ones are ignored. Updates aren't logged, so the table can't have any updates
except the unlogged kind (a merge moving a reference). Deletes are logged like any table's.
The applier treats that one row as a change to every column.

The version vector that sync exchanges (the newest hlc per origin) is computed from the
changelog each time and never stored, so it can't disagree with the data after a crash. The
index `changelog_origin` on (`origin_uid`, `hlc`), which `sync.db` also has, makes that one
lookup per origin, however long the changelog is.

Several connections to one library can be open at once. For example, the importer opens its
own on its thread. Each connection has its own clock, so writes go through
`mc_library_begin_write()`, which takes the write lock and then catches the clock up to the
newest logged hlc. That keeps the log strictly increasing per device.

The triggers call SQL functions that kithcore registers (`mc_hlc_now()`, `mc_node_uid()`,
`mc_applying_remote()`). So any other tool that opens the database, such as the sqlite3 CLI,
will fail on writes to tracked tables instead of silently making untracked changes. That's
deliberate.

## Migrations

`k_migrations[i]` takes the database from `user_version` *i* to *i + 1*. Each migration runs
in its own transaction. Only ever append to it. If the database is newer than the app, it's
refused (`MC_ERR_FORMAT`).

A migration that adds a logged table or column also lists in `k_newly_known` which changelog
rows it makes known. Peers on a newer version may already have sent rows like these, which
were logged but not applied. Once the migration has run, they're applied
(`mc_rows_reapply()`), so the device catches up without waiting for the next change.
