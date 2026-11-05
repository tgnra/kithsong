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
| 17 | `albums.art_chosen`: the cover the user chose |
| 18 | `play_events`: the play log |
| 19 | `track_analysis`, `analysis_failed`: local audio analysis |
| 20 | index `changelog_origin`, for the version vector |

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
(`scrobble_account`, and the local-only `scrobble_queue`), v5
(`devices.confirmed_profile`), v6 (`scrobble_queue.track_uid`), v7 (index `changelog_origin`)
and v8 (the local-only `peer_lacks`, see
[Waiting for a device](sync-protocol.md#waiting-for-a-device)). Removing an album or artist from one device needed no
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
plays, audio analysis, playlist and album group entries, the file's own row) as JSON objects of
column to value, `features` as hex since JSON has no BLOBs. Restoring inserts them
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
| `rows` | JSON array, one record per track as `deleted.db` keeps (the track, its album and artists, both credits' rows, plays, audio analysis, playlist and album group places), without the file's row |

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
  frontend plays them; the scrobbler decides when it looks the track up, and again before it
  sends what's queued, so a play waiting there when the flag is set doesn't go. Plays are still
  counted.
- `albums.mix`: a continuous mix, one piece of music split into tracks, or in one long track
  (a DJ mix). Its tracks aren't search results: a query matching one finds the album
  instead, once, scored as the best of the album and its tracks. An artist's page doesn't list
  them among the artist's tracks either, only the album. Smart playlists never pick
  its tracks, whatever the rule; smart album groups can pick the album. A mix of one track
  is an album, not a single: album listings and counts have it, as the artist's albums, and
  the artist's singles don't. Everything else lists its tracks as any album's. The album's own
  page says "Continuous mix" (`mc_album_info.mix`).

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
it's made a mix (`mc_track_mix_set()`, which files it there like an edit, and takes it out
again when the choice is undone and the album's flagged nothing else); and for those
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

## Play log

`play_events` is the raw history that `track_plays` sums up: one row for each time a track was
started, whether or not it counted as a play. Its readers go through
`mc_library_list_play_events()`, every device's rows in a time range, oldest first, as the Home page
does (see [Home's played rows](#homes-played-rows)), or read it directly, as
[What skips teach a station](#what-skips-teach-a-station) does. Nothing here filters or interprets;
a reader decides what a two-second skip means. Nothing here leaves the user's own devices.

| Column | |
|--------|-|
| `uid` | the row's own |
| `track_uid` | the track; the row goes with it |
| `node_uid` | the device it was played on |
| `started_ms` | wall clock, when it was first heard, or if it never was, when it was started |
| `listened_ms` | time actually heard. Pauses add nothing, a seek forward adds nothing for what it jumped, and a seek back adds what's heard again, so it can pass the track's length. A track started and left at once has 0 or nearly |
| `ended` | `finished`, `skipped` (next, previous, another queue item, or started again), `stopped` (stopped, or the app or library closed), `replaced` (a new queue took over) or `failed` (the file couldn't be played: `listened_ms` is 0) |
| `context` | how the queue was started: `album`, `playlist` (smart lists too), `album_group`, `artist`, `manual` (one track by itself), `shuffle` (any of those started shuffled with no track picked), `radio` (a station) or `unknown` |
| `context_uid` | the album, playlist, group or artist, also under `shuffle`, or a station's seed track; NULL if there's none. Not a reference: the history stays when the album or playlist goes, and a merged-away album's uid can be looked up in `aliases` |

The player keeps the rows, so all frontends log alike. A frontend gives it the queue's track
uids and context (`mc_play_origin`), takes each row as it ends (`MC_PLAYER_EVENT_ENDED`,
`mc_player_take_ended()`) and writes it (`mc_library_log_play()`); before closing it calls
`mc_player_end_play()` so the play under way is written as `stopped`. An app killed outright
loses that one play.

Rows sync like `track_plays`, and more simply: each is one device's and is written once, never
updated, so no two devices ever write the same field. So a row is logged whole: see
[Change tracking](#change-tracking). They go with their track, and come back with it from the
Recycle Bin. A duplicate's rows move to the track that stays, which isn't logged. A merged
library's history is not carried over (see [Merging libraries](sync-protocol.md#merging-libraries)).

Rows are kept for good. A row costs about 700 bytes of `library.db`: 350 for the row and its
indexes, the rest for its changelog row. So 10,000 starts a year, about 30 a day, come to some
7 MB, on every device of the group, whichever of them played what.

### Home's played rows

"Played a lot recently" and "Not played in a while" come from `pick_played_lately()` and
`pick_not_played_lately()` in [`home_picks.c`](../core/src/library/home_picks.c). Nothing is stored:
each load reads the last 90 days of the log, however long it has grown, and `track_plays` for what's
older. A track's plays go to its album and the playlists holding it as they are now, however it was
played, so `context_uid` doesn't come into it.

**A real play** is a row that didn't fail, with 30 seconds heard, or half the track if that's less.
How it ended doesn't matter otherwise: a skip after two minutes was listened to, and `finished` can
be a seek to the end. Half a track is there for interludes, which never reach 30 seconds. It's less
than `mc_play_counts()` asks, as that's about what's fair to scrobble, and a minute of a long track
says the album was on.

**Radio plays.** A station's track was picked by the app, so it's weak evidence of wanting the album:
a station can touch dozens of albums in an evening. So radio plays don't count towards "Played a lot
recently", however many there are, even when the row is short. But they were heard, and the other
row says something hasn't been, so for "Not played in a while" a real radio play is a play like any
other: it keeps the album out for 90 days, and counts as having played it before.

**Played a lot recently** takes, in order:

1. Three or more real plays in the last 14 days, the most plays first.
2. From before the log began: three or more tracks last played in those 14 days.

And while the row has fewer than 8:

3. A real play in the last 14 days.
4. From before the log began: a track last played in those 14 days.
5. A real play in the last 90 days.
6. From before the log began: a track last played in those 90 days.

So one play after years of silence comes behind anything on repeat, and only gets in at all if
there's room.

**Not played in a while** is what has a play, from either source, and none in the last 90 days, the
longest ago first, those with two or more tracks played before the rest. A skip or a failure in the
log changes nothing. With fewer than 8, it goes on down to 14 days ago, leaving out what's in the
other row.

**Before the log began.** `track_plays` is all there is of older plays, so the steps "from before
the log began" keep its guess: it can only tell how many tracks had their last play lately, not how
often they were played. The guess is taken only for an album or playlist whose last play in
`track_plays` is older than the log's oldest row, of any device and any kind. After that the log has
seen the play and knows better, and `track_plays` counts a station's plays with the rest. So an empty
log gives the guess for everything, a short one gives the log's rows first and the guess behind them,
and one that's older than 90 days leaves the guess out of "Played a lot recently" altogether. A device
that only started logging after another has its earlier plays treated as seen, and they drop out of
that row until they're older than the windows anyway.

## Audio analysis

A fingerprint of how each track sounds, for telling which tracks sound alike, and its loudness,
worked out on the device from the audio alone: nothing goes over the network but sync's own
rows. The API is in `musiccore/analysis.h`, the code in `core/src/analysis/`. It changes nothing
about playback: the loudness is kept so that a volume feature won't need a second decode of the
library.

### What's measured

One decode of a file, all of it:

- **Timbre.** A mono mix, resampled to 22,050 Hz with the same cutoff from every higher rate.
  1024-sample Hann frames every 512 samples; each gives a spectral centroid and, through 40 mel
  bands (20 Hz to 11,025 Hz, triangles of equal area) in dB and an orthonormal DCT, MFCCs 0 to
  12. The mean and variance of each over the track are kept. Frames under -70 dBFS are left out,
  so silence isn't timbre. A band reads no lower than 100 dB under its own frame's power, so a
  gain moves MFCC 0 and nothing else.
- **Loudness.** Integrated loudness in LUFS (BS.1770-4: K-weighting worked out for the file's
  rate, 400 ms blocks with 75% overlap, gates at -70 LUFS and -10 LU), loudness range in LU (EBU
  Tech 3342: 3 s blocks, gates at -70 LUFS and -20 LU, 10th to 95th percentile) and true peak in
  dBTP (4x interpolation under 96 kHz, 2x under 192 kHz). Per channel at the file's own rate, not
  from the mono mix, with BS.1770's channel weights taken from the usual WAV and FLAC order. Opus
  always decodes to stereo.

`analysis_version` (`MC_ANALYSIS_VERSION`) names the algorithm. Raising it makes every track due
again on devices that scan; a result of a newer version than a device's is left alone.

### `track_analysis`

| Column | |
|--------|---|
| `uid` | `mc_uid_derive(track_uid, source)` |
| `track_uid` | the track; deleted with it |
| `source` | 0: read from an original, 1: from a lossy copy (`files.source_file_uid` set) |
| `file_rev` | `tracks.file_rev` when it was read, NULL as that is before the first swap |
| `analysis_version` | |
| `features` | the BLOB below |
| `lufs`, `lra`, `true_peak` | REAL; NULL when undefined: a track that's silent or under 400 ms has no `lufs`, one under 3 s no `lra`, a silent one no `true_peak` |

`features` is 116 bytes, the same on every platform. All numbers are little-endian IEEE 754
binary32:

| Offset | Size | |
|--------|------|---|
| 0 | 1 | layout, 1 |
| 1 | 1 | N, the MFCC count, 13 |
| 2 | 2 | reserved, 0 |
| 4 | 4N | mean of MFCC 0 to N-1, dB |
| 4 + 4N | 4N | variance of the same, dB squared |
| 4 + 8N | 4 | mean spectral centroid, Hz |
| 8 + 8N | 4 | variance of the centroid, Hz squared |

A reader skips a BLOB of a layout or N it doesn't know. A new layout needs a new
`analysis_version`; a new version needn't change the layout.

**Why a row per source.** Sync's fields are last-writer-wins, which can't say that an original
beats a transcode: a device holding only a copy that scans later would overwrite the better
result, and on every device. So the two are different rows, and neither can touch the other. The
uid comes from the track and the source, so two devices analysing one track from originals write
one row, which merges like any other, and that's harmless: both read the same audio. Field by
field a row could only end up a mix of two writers if they wrote within the same moment, and then
of two results of one file. `mc_analysis_get()` picks a track's best row: of the current
`file_rev`, then the lowest `source`, then the newest version. Once a track has an original's row
a lossy row stays, unread.

A merged-away track's rows move to the survivor like its plays (see
[Duplicates](sync-protocol.md#duplicates)). That can leave two rows of one source on a track,
under different uids, and the reader's order settles it. There's no UNIQUE key on purpose: rows
arrive field by field and a key would make applying them fail, and it would have nothing to add.

### Scanning

`mc_analysis_scanner_start_sized()`, which Windows and Linux call beside starting sync and
Android doesn't, for the battery: a phone or watch gets results by sync. The scanner has a thread
and a connection of its own, and starts 30 seconds after the library opens, and a pass soon after
an import or sync ends. It counts what's waiting (`mc_analysis_pending()`) at the start of each pass
and every 5 seconds as it goes, as an import files its tracks a few at a time and a pass can start
on the first of them. Its status adds that to what it has done since a pass last found nothing, so
the total is the whole run's however many passes it takes. Under five hours of audio it runs flat
out with nothing shown: about 45 seconds' work in a release build. From five hours its status says
it's large, and the frontend opens a dialog with a progress bar, Background and Cancel. It stays
flat out behind the dialog. Background calls
`mc_analysis_scanner_set_paced()`, which brings the rests and background priority
(`mc_thread_background()`), and only then the wait for AC power, and shows a row above Settings on
Windows or a button next to Settings on Linux that opens the dialog again. `analysis_background` in
settings.json keeps that choice across restarts until a pass finds nothing to do. Cancel stops the
scanner and sets `analysis_stopped` in settings.json, so it stays stopped over restarts, imports
included, until Resume analysis in Settings' Audio analysis section. That section says how many
tracks are waiting, and Resume starts a paced scanner at once
(`mc_analysis_scanner_start_resumed()`), as if Background had been the answer. One track at a time, it takes the next track, in
uid order, with a file that:

- is the track's current one (`files.file_rev IS tracks.file_rev`; a stale copy, or an original
  a peer has since replaced, isn't read);
- has no result of the current `file_rev`, from the same or a better source, of this version or
  newer;
- isn't in `analysis_failed` as it stands.

An original goes before a copy. That is all its state, so it picks up where it stopped after a
restart, and a result arriving by sync takes a track off its list. A stop gives up the file it's
reading, at once, and that track is done from the start next time. Having gone through the
library it looks again every 5 minutes, which is how it finds imports. It rests as long as it
works, so a long scan doesn't keep one core flat out.

It only works on AC power. Before each track it asks `mc_power_on_ac()` (`util/power.h`), so
unplugging lets the track in hand finish and plugging in sets it going within 30 seconds, which is
how often it looks again meanwhile. On Windows that's `GetSystemPowerStatus`: `ACLineStatus` 1 is
AC, as is a machine with no system battery (`BatteryFlag` 128), and an unknown line (255) is AC only
where no battery is known of. On Linux it's `/sys/class/power_supply`: any Mains or USB supply
online is AC, as is having no battery at all (desktops, containers) or a folder that can't be read,
and supplies with `scope` Device, a wireless mouse's say, don't count. In doubt it's AC.

A phone runs the scanner in two cases only: an import that added tracks there asks for one run,
as they may be on no desktop to analyse them, and so does Analyse in Settings' Audio analysis
section, which says how many tracks are waiting. `AnalysisWorker` is one-off WorkManager work that
needs a charger, unique with `REPLACE`: it has no list, so replacing loses nothing, while a run
under way may be past where the new tracks are. It calls `mc_analysis_scanner_start_once()`, the
same scanner on its own thread and connection, holds included, but going at once and leaving
power to WorkManager, and ending when it's been through the library. So anything sync brought
first is skipped, and so is everything that has a result; a transcode with none is read too. If
the system stops it the results stay and WorkManager runs it again on the next charge; it ends a
run itself as a retry after 9 minutes, under WorkManager's 10. There's no periodic work, no scan at
start and no worker on the watch.

It stands aside (`mc_analysis_hold()`) for the whole of a sync session and while an import is at
work, but not while an import waits for review. For sync that's more than good manners: a row
logged during a session moves this device's version vector, so the session's digests differ, it
sends every row of this device's origin again (`"resend"`) and, not having converged, it lifts the
rate limit for the next. Local edits can do that too, but they are rare and the scanner writes
every few seconds for hours. Between sessions each write is a local change as any other, which the
rate limit holds to one session an hour.

`analysis_failed` (`file_uid`, `size_bytes`, `mtime_ms`, `analysis_version`) lists the files that
wouldn't decode, so they aren't tried again until they change or the version does. It's this
device's own, keyed by `files.uid`: whether a file decodes says nothing of another device's. A
file that's only missing isn't listed, as the drive it's on may be back tomorrow; nor is one that
can't be opened for now, or changed while it was read.

### Distance

`mc_analysis_distance()` is a weighted Euclidean distance over 29 numbers in four groups:

| Group | Weight | Numbers |
|-------|--------|---------|
| shape | 0.45 | the means of MFCCs 1 to 12 |
| texture | 0.25 | the standard deviations of MFCCs 0 to 12 |
| brightness | 0.20 | the mean centroid in octaves, and its standard deviation as a fraction of the mean |
| energy | 0.10 | `lufs`, `lra` |

- MFCC 0's mean is left out: it's the level, and the same recording mastered louder has the same
  timbre. The loudness it would have said counts for what it is, under energy, at a low weight.
- Raw coefficients do need scaling: they differ in unit and their spreads by an order of
  magnitude, so MFCC 1 alone would decide. Each number is divided by its standard deviation over
  the library (`mc_analysis_stats_load()`, one result per track, worked out once and handed in).
  Every device has the same rows, so every device gets the same scales.
- The diagonal only, not a full covariance: MFCCs are close to uncorrelated by construction, and a
  covariance needs far more tracks than 29 numbers to be stable in a small library.
- Within a group the squared z-scores are averaged, so its weight doesn't depend on how many
  numbers it has. Halved, as two random tracks differ by 2 on average, it gives about 1 for two
  random tracks of the library and 0 for the same features.
- A small library's spread says little, so the fixed scales in `analysis.c` count for 16 tracks'
  worth; with no stats they're all there is. They and the weights were set from reasoning and
  synthetic signals, not tuned on real music.
- A number that's undefined (`lufs` of a silent track) counts as no difference. Results of
  different versions aren't compared at all: the distance is -1.

`core/tests/manual_analysis` analyses a file or folder and prints the features and each file's
nearest neighbours, for a person to judge.

Radio ([`radio.h`](../core/include/musiccore/radio.h)) picks from these results: a fixed mix with
`mc_radio_make()`, or a station that goes on for as long as it's asked (`mc_radio_station_open()`).
Candidates have a result of this version and a file on this device, and aren't in a continuous mix.
They run from a minute up to `max_track_ms`, 8 minutes unless it says, negative for no limit; a
duration of 0 or none is unknown and counts against a track neither way. A seed has to be a
candidate too. A station reads them once and then never touches the library, so it doesn't see
what changes later, but for the skips it's told of (see below). It plays every candidate once
before anything comes back, and then holds back its latest picks (half the pool, 64 at most).

`mc_player_play_radio()` plays a station: the seed starts at once, and a thread of the player's
loads the station on its own connection and keeps 5 tracks queued ahead of the current one. Rows
in the play log have the context `radio` and the seed as `context_uid`. The station ends when
something replaces the queue, when the library closes (`mc_player_stop_radio()`) or after 8 items
in a row that won't open, and the queue then plays out. A frontend that keeps something per queue
item looks up new ones on `MC_PLAYER_EVENT_QUEUE`.

`core/tests/manual_radio <library folder>` makes a radio mix from these results and saves it as an
ordinary playlist: 10 tracks unless `--count` says, from a search with `--seed <text>` or a track
drawn at random, named by `--name`, with `--max-length <minutes>` for `max_track_ms`.
`--scan` analyses what has no result first, and `--dry-run` only prints the mix. It makes no network
calls, and refuses a library of an older schema without `--migrate`, as an older app can't open
one that's been migrated.

### What skips teach a station

A track skipped early on a station didn't fit that station. That's all it means: music someone
owns isn't music they dislike, so a skip never counts against a track in general. The track and
what sounds like it score a little worse there, and on stations from seeds that sound like that
seed, for a while. Picks elsewhere don't change, nor does anything outside radio.

It's worked out from `play_events` each time a mix or station starts, so nothing new is stored:
it syncs with the rows, a skip on one device teaches the others, and there's nothing to migrate.

**What counts.** A row whose `context` is `radio`, with `context_uid` as the seed, `ended` as
`skipped`, and `listened_ms` from 3 up to 30 seconds.

- A skip in an album or a playlist teaches nothing: the listener was after something else.
- Under 3 seconds too little was heard to be a judgement. Previous goes back a track within its
  first 3 seconds, leaving a `skipped` row for one that may not have been listened to at all, and
  Next pressed a few times running goes past tracks on the way to another.
- From 30 seconds on it isn't early, as a track that's been given that long got a hearing, and the
  listener may just want the next one. The shortest candidate is a minute, so this is half way
  at worst and about a seventh of a usual track, in the first verse. It's where
  `mc_radio_stations_list()` takes a play to have counted too.
- `stopped` and `replaced` say the listening ended, not that the track didn't fit.
- A skip the same track's next row of this pairing picks up from isn't one: it starts within 5
  seconds of where the skip left off (`started_ms` + `listened_ms`), whichever way it ended. That's
  a track started again, by Previous or a seek to the start, which the log has as `skipped`, or
  gone back to from the first seconds of the next. Wanting more of a track is the opposite of a
  skip. A station never brings one back that soon itself.
- A row of the seed itself is left out, and so is this station's seed skipped on a station near
  it: a seed is what its station is built on, and pushing down what sounds like it would push the
  station away from itself.

The log can't tell a seek from listening, so a track jumped through in 20 seconds and then skipped
reads as an early skip. A restart after a long pause isn't seen as one, as nothing says how long
the pause was, so it counts as a skip, which the play it starts takes back if it's heard out.

**How much.** Taking each pairing of seed and track on its own, oldest row first:

- A skip adds 1, less what it has faded: straight down to nothing over 60 days, so 0.5 at 30 days.
  A skip made in the wrong mood wears off by itself, and the time limit is what bounds the read.
- A pairing stops at 3. Skipping one track every time means no more than three times does.
- A `finished` row of the pairing with 30 seconds or more heard takes 1 off what's built up so
  far, down to 0: it fits after all. Skips after it count as usual, and with none before it, it
  does nothing, as there's no bonus. Less than 30 seconds got to the end by seeking.
- A row dated after now doesn't count: that's a device with its clock wrong, and it would never
  fade.

Each pairing then reaches this station by how close its seed is to this station's seed, and each
candidate by how close it is to the skipped track. Both go from 1 at a distance of 0 straight down
to nothing at 0.6, on the library's scales, where two random tracks are about 1 apart and a track's
nearest few about 0.45. So those few get a quarter, and an unrelated station gets exactly nothing,
not just very little. A score gets worse by 0.05 for each skip that arrives. That's `DRAW_SOFTNESS`,
so among tracks otherwise level a fresh skip leaves a track about a third as likely to be drawn.
However many skips agree, a track's score goes up by 0.15 at most, what being played just now
costs it. It's a nudge to the order and no more: nothing is left out, a station still plays every
candidate in a round, and with no skips that apply the scores are the same to the last bit.

A station goes by the 32 skipped tracks that weigh the most, as each is a pass over the candidates.
Those needn't be candidates: a track or seed with a result and no file here still says what sounds
like it, which is how a phone's skips reach a desktop that lacks those files. A seed that has no
result at all any more says nothing, and that includes one merged away as a duplicate: its rows
keep the old uid in `context_uid`, and aren't looked up in `aliases`.

**During a session.** A skip counts straight away, not just the next time the station opens. The
player hands each `skipped` row to its station (`mc_radio_station_heard()`) as it closes it, which
costs less than a pick, on the thread that does the picking. The five tracks already queued stay,
so it shows after those. A row is taken as one fresh skip of this seed, and a finish is left for the
next opening. The player knows a restart when it does one and doesn't pass it on. Going back to a
track it can't take back, so that too is left for the next opening.

The numbers come from reasoning and the scales above, not from real skips. Reading the log costs
in proportion to the rows of the last 60 days, however long the log is: some 6 microseconds a row,
as `play_events_started` doesn't hold the columns wanted. Timed on 50,000 made-up tracks
(`manual_stations --make`, `--open`, in a release build) a station opened in 1.03 s with none of
this. With 12,000 radio rows in those 60 days, 200 a day, it took 1.36 s, about 0.1 s of it the
log and the rest all 32 passes. The seed is already playing by then.

### Radio stations

`mc_radio_stations_list()` ([`radio_stations.h`](../core/include/musiccore/radio_stations.h), the
code in `core/src/radio/stations.c`) picks the seed tracks of the Home page's Radio row. Nothing
is stored, so there's no schema to it: it's worked out again each time the page loads, from
`track_analysis`, `track_plays` and `play_events`.

A track can be a seed if it can be in a mix: it has a result of this version and a file here, it
isn't part of a continuous mix, and it's from 60 seconds to 8 minutes long.

How many stations there are:

| Eligible tracks | At most |
|-----------------|---------|
| under 25 | none, and the row is left out |
| 25 | 1 |
| 50 | 2 |
| 100 | 3 |
| 200 | 4 |
| 400 | 5 |
| 800 | 6 |
| 1,600 | 7 |
| 3,200 and up | 8 |

That's an upper limit. The other is how spread out the library is: two seeds must be at least 0.6
apart on the fixed scales (`mc_analysis_distance()` with no stats), and when nothing left is that
far from all the seeds so far, there are no more stations. It has to be the fixed scales, as the
library's own put any library's tracks about 1 apart, however alike they all sound. The 0.6 comes
from a real library of 700 tracks, where two tracks of one album were 0.51 apart at the median and
any two tracks 0.62. Like the distance's weights it hasn't been tuned by ear.

Choosing takes three steps:

1. **Who has company.** Up to 1,024 tracks are drawn by a hash of the uid and the day, and compared
   with each other on the library's scales. A track further from its neighbours (the eighth
   nearest in the library, in proportion in a sample) than 1.5 times the median track is, and than
   0.7, can't be a seed. Farthest-point selection goes straight for such tracks otherwise: in that
   real library it chose an intro and a noise track, and a mix from one is nothing like it from the
   second track on. The 0.7 keeps a loose genre beside a tight one from being left out whole.
2. **Spreading out.** Farthest-point selection over the rest of the sample, starting from the most
   played track there, the hash deciding among equals.
3. **Settling.** Each seed may then move to any track of the library within 0.45 of it, on the
   library's scales, that stays 0.6 from the other seeds. It moves to an artist no other station
   has, then to a track with a cover, then to what's been played most, counted in doublings. So
   being distinct comes first, and plays only choose among tracks that sound about the same.

The stations are shown most played first. A station is named after its seed's first artist with
`home_station`, or after the seed's title if it has no artist. Its sub line is `home_station_sub`;
stations of one name say instead what they're like, by the seed's title, and its album if the
titles match too. A station that would still read the same as another is dropped. The cover is
that of the seed's album, or else the artist's usual picture.

**Staying put.** The sample, the tie-breaks and the plays all go by the start of the local day.
Plays are `track_plays` less the `play_events` of 30 seconds or more since then, as `track_plays`
has no history of its own. So reloading Home, and playing music, changes nothing until the next
day, when the seeds may move, which keeps a library that's rarely played from showing the same
stations for ever. Results arriving, or tracks being imported or deleted, can change the stations
during the day, as the scanner does for hours on a new library.

`core/tests/manual_stations <library folder>` prints a library's stations and how long they took,
`--days <n>` the next days' too. `--make <tracks>` fills an empty folder with a made-up library to
time. In a release build a made-up library of 50,000 tracks took about 0.7 seconds on Home's own
background thread, and several times that with the machine busy. Of that about 300 ms is the query,
140 ms the sample, whatever the library's size, and 40 ms settling.

The menus of a track, album or artist start a station from one track. For an album or artist that's
`mc_radio_seed_for_album()` or `mc_radio_seed_for_artist()`
([`radio_seeds.h`](../core/include/musiccore/radio_seeds.h)): of the tracks that pass
`mc_radio_seed_check()`, the one with the least total distance to the others, on the fixed scales, so
a menu needn't read the whole library to open. Of more than 64 such tracks, 64 are compared. Plays
only decide between tracks that are level, as the two of a two-track album always are: anything more
would let the most played track stand for an album it may not sound like. Nothing is stored, and
`MC_ERR_NOT_FOUND` greys the item out.

## Stats

`mc_library_read_stats()` (`musiccore/library.h`) adds up what's above with one read query:
nothing is stored for it, so there's nothing to keep up to date or to sync. Tracks, albums,
singles, artists, genres and playing time come twice, for the whole library and for what has a
file on this device, as a device needn't hold all of it. Albums, singles and artists are counted
as the listings have them, so the numbers match what a frontend shows. Size on disk is the sum of
`files.size_bytes`, a track's transcodes included; plays come from `track_plays`, and time
listened from `play_events`, so it starts at v18 and is an undercount for a library older than
that.

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
- **`limit_duration`** (optional, not beside `limit`) is a positive number of seconds: "an
  hour of music" is `3600`. It also applies after the order, keeping tracks or albums while
  their running total is within it, and stopping at the first that would go over. The first
  is kept whatever its length, so a limit never empties a list. An album's length is its
  tracks'.

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
rename, reshuffle or delete is logged against a uid every device already has. The rows hold
English names; until one's renamed, listings, recents and search show it by a UI string
instead, so it reads in the language in use (`k_examples` in `smartlist.c`).

## Recents

`recents` holds what was last opened: albums, artists, playlists and album groups (smart
ones included), and radio stations, with `kind` naming which (`'album'`, `'artist'`,
`'playlist'`, `'album_group'`, as sync profile items do, and `'station'`). A row's `uid` is the
item's own uid, a station's being its seed track's, so an item
has one row and opening it on two devices writes the same fields: sync keeps the newer open,
and a removal wins over an earlier open. `opened_ms` is the wall-clock part of the hlc the
open is stamped with, so the newest change by hlc is also the newest `opened_ms`. There is
no foreign key, since a row may name any of five tables; listing leaves out rows whose item
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
devices. `mc_library_list_smart_keeping()` keeps one smart list however it answers, so one
hidden here that's opened from a search or just made has a tile to open on. Wear OS shows only what's present, so it drops the empty ones as well.

A station has no moment of being opened that the core sees, as a page has: `mc_library_log_play()`
writes its row, in the same transaction as the [play log](#play-log) row of the station's seed
(context `radio`, `track_uid` equal to `context_uid`, and not `failed`), unless this device's row
before it is the same station's and ended `finished` or `skipped`: a station comes back round to
its seed, and that isn't a new start. So a station goes in when its first track ends, and the
rest of its tracks write nothing here: it plays on without filling the changelog, and Remove from
Recents holds while it does. That row is stamped like an open,
except one ended by `replaced`. It arrives after whatever replaced it was recorded, so it keeps
its `started_ms` and only moves the row forward, or the station would go above what ended it,
and then `opened_ms` isn't the wall-clock part of its hlc. A
station's name isn't stored: it's made from its seed when listed, as the Home page's stations are.

Stations are only listed with `MC_LIST_STATIONS`, and with `MC_LIST_PLAYABLE` only if the seed
has a `files` row here, as there's nothing to start one from otherwise. Android passes both.

`kind` is free text that nothing checks, so a new kind needs no schema version. An older app
stores and passes on the row like any other, and its listing leaves the row out, since each kind
joins only its own table. That's why a kind is only ever added.

## Tables

| Table              | Synced | Notes |
|--------------------|--------|-------|
| `artists`          | yes    | one artist each, never a credit of several |
| `albums`           | yes    | `art_hash` is the SHA-256 of the album folder's `cover.jpg`. `artist_uid` is the first credited artist, `artist_credit` the credit as shown (NULL: that artist's name). `no_scrobble`, `mix`: see [Album flags](#album-flags). `art_chosen` is the SHA-256 of a cover the user chose, NULL if they didn't: import and downloads leave such a cover alone, and sync brings it to devices showing another (see "A cover the user chose" in [tagging-rules.md](tagging-rules.md)) |
| `tracks`           | yes    | `added_ms`: when the track was first imported (Unix ms); NULL for tracks imported before v3. `file_rev`: a new uid each time an import replaces the track's original with a higher-quality file, NULL before the first time. `artist_uid` and `artist_credit` as for albums |
| `track_artists`    | yes    | a track's credited artists in order, uid derived from the track and position; see [Artist credits](#artist-credits) |
| `album_artists`    | yes    | an album's, the same way |
| `track_plays`      | yes    | play counts and last plays, one row per track per device; see [Play counts](#play-counts) |
| `play_events`      | yes    | one row each time a track was started, each device's its own; see [Play log](#play-log) |
| `playlists`        | yes    | |
| `playlist_entries` | yes    | ordered by the fractional-index string `position` |
| `album_groups`     | yes    | playlists of albums; see [Album groups](#album-groups) |
| `album_group_entries` | yes | ordered by `position`, like `playlist_entries` |
| `smart_lists`      | yes    | `subject` is `'tracks'` or `'albums'`; `rule` is JSON; see [Smart lists](#smart-lists) |
| `recents`          | yes    | last opened items, one row per item keyed by its uid; see [Recents](#recents) |
| `track_analysis`   | yes    | what the audio analysis found, a row per track and source; see [Audio analysis](#audio-analysis) |
| `files`            | no     | this device's files, by path relative to the library folder; `source_file_uid`/`source_peer_uid` mark transcodes received from a peer; `sha256` (hex) of the file's bytes, recorded when sync transfers or verifies it; `file_rev` is the track's `file_rev` the file was made from (a copy whose `file_rev` differs from its track's is stale, see [Files](sync-protocol.md#files)); `source_sha256` (hex) of the bytes an original was imported from, before the import tagged it, which tells a re-import of the same file (see [Duplicates](tagging-rules.md#duplicates)), passed on with an original by sync |
| `aliases`          | no     | merged-away artist, album and track uids, each with the `canonical` uid it became; derived from the changelog on each device, see [Duplicates](sync-protocol.md#duplicates) |
| `merge_keys`       | no     | the MusicBrainz ids ever logged per uid, which decide what merges (rows of singles albums' artists from before v12 are left, unused) |
| `changelog`        | -      | field-level change log that sync is built on |
| `analysis_failed`  | no     | files that wouldn't decode for the audio analysis; see [Audio analysis](#audio-analysis) |
| `recycle_bin`      | no     | deletes made here that no other device has heard of yet, which can be taken back; see [Recycle bin](#recycle-bin) |
| `meta`             | no     | `node_uid`, `library_uid`, `merge_seq` (the changelog `seq` duplicate merging has read up to), `merge_source` (the set-aside library still to merge in, see [Identity](#identity)), `merge_wait` (set with it until this device's profile is confirmed: the merge waits for that, see [Merging libraries](sync-protocol.md#merging-libraries); its value is the run of the app that set it, and a later run finding it undoes the pairing), `merge_with` and `merge_name` (with them: the kept device's name and this device's own before pairing), `merge_restored` (the kept device's name, after a pairing was undone, until a frontend has said so), `merge_restore_sync` and `merge_restored_name` (while an undone pairing's `sync.db` and name are still to be put back; see "Abandoning a merge" there), `artist_refresh` (`'pending'` until the credits from before v13 are split, then `'done'`; see [Artist credits](#artist-credits)), while it's pending the refresh's cursors `artist_refresh_mb`, `artist_refresh_split`, `artist_refresh_loose` and its `artist_refresh_files` (see [Refresh after upgrading](tagging-rules.md#refresh-after-upgrading)), `changelog_epoch` (bumped each time a delete is taken back out of the recycle bin, which takes changelog rows out; see [Recycle bin](#recycle-bin)), `edit_swap` (while a hand edit swaps its files: what to put right after a crash; see [Hand edits](tagging-rules.md#hand-edits)), `stress_library` (the preset, in a generated stress test library: sync and scrobbling refuse such a library, see `musiccore/stress.h`), and other per-library settings |

## Change tracking

`library.c` generates `AFTER INSERT/UPDATE/DELETE` triggers from its tracked-column list
every time the library is opened. Each change to a tracked column writes one `changelog`
row (`hlc`, `origin_uid`, `entity`, `row_uid`, `field`, `value`). The generator lives in
`core/src/util/changelog.c` and is shared with `sync.db`, whose changelog has the same shape.

A table whose rows are written once and never edited can be logged whole instead
(`mc_changelog_whole_row`; today only `play_events`). Inserting a row writes one `changelog` row
with `field` = `row` and `value` = a JSON array of the tracked columns in their listed order,
which can only be appended to: an element missing from the end reads as NULL, and extra ones
are ignored. Updates aren't logged, so the table must have none but the unlogged kind (a merge
moving a reference). Deletes are logged as for any table. The applier treats that one row as a
change to every column.

The version vector sync exchanges (the newest hlc per origin) is computed from the changelog
each time, never stored, so it can't disagree with the data after a crash. The index
`changelog_origin` on (`origin_uid`, `hlc`), which `sync.db` has too, makes that a lookup per
origin, however long the changelog is.

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
