# Artist credits: implementation plan

Working notes for the multi-artist change. Folded into tagging-rules.md, schema.md and
sync-protocol.md as each part lands, then deleted.

## Goal

A track or album credited to "A feat. B", "A, B & C", "A x B" belongs to each of A, B and C:
each is its own `artists` row, the track shows on each one's artist page, and the files carry
the split (ARTISTS, multi-valued MUSICBRAINZ_ARTISTID). The credited string is still what's
shown and what's written to ARTIST / ALBUMARTIST.

## Where credits come from

1. **MusicBrainz** (matched imports): the `artist-credit` array of the release and of each
   track, as it is: name as credited, artist id, sort name, join phrase. Never re-split.
2. **Multi-valued tags** (as-is imports, re-imports): ARTISTS / ALBUMARTISTS with
   MUSICBRAINZ_ARTISTID / MUSICBRAINZ_ALBUMARTISTID values, when their count matches. Join
   phrases are recovered by finding the names in order in the ARTIST string.
3. **The string** (`mc_credit_split_text`, credit.c), otherwise:
   - "feat."-style separators always split: ` feat. `, ` feat `, ` ft. `, ` ft `, ` featuring `,
     and a trailing `(feat. B)` / `[feat. B]`.
   - Ambiguous separators — ` & `, `, `, ` and `, ` x `, ` X `, ` vs. `, ` vs `, ` with `, ` / `,
     `; ` — split only when every resulting part is a known artist (the library has an
     artists row of that name that is one artist, i.e. not itself a credit of several) and the
     joined string isn't one. Splits are tried greedily so "Earth, Wind & Fire & B" keeps
     "Earth, Wind & Fire" whole when it's known. Unknown parts: keep the whole string.
   - A part is never empty; whitespace is trimmed; separators match case-insensitively.

## Schema (library v13)

- `tracks.artist_credit TEXT`, `albums.artist_credit TEXT`: the credit as shown ("A feat. B");
  NULL means the artist's own name. Logged (synced).
- `artists` rows are single artists. `tracks.artist_uid` / `albums.artist_uid` are the
  **first** credited artist, so older versions and every existing query still work.
- `track_artists (uid, track_uid, artist_uid, position, joinphrase)` and
  `album_artists (uid, album_uid, artist_uid, position, joinphrase)`: synced, logged, FKs
  to their parents (`ON DELETE CASCADE` for the owner), index on `artist_uid`.
  The row uid is **deterministic**: derived from (owner uid, position) (a name-based UUID
  from a SHA-256 of both), so two devices crediting the same track write the same rows and
  field-level sync merges them; a shorter credit deletes the rows past its end.
- Migration v13 adds the columns and tables, fills one row per track and album from the
  current `artist_uid` (position 0, so the tables are complete from the start), and sets
  `meta.artist_refresh = 'pending'`.
- Listings: the shown artist string is `coalesce(artist_credit, artist name)`. An artist's
  page (albums, singles, tracks with no album) and the artist list go through the
  credit tables, so a featured artist lists the track. Search matches the credit string.
- Duplicate merging: the credit tables' `artist_uid` follows aliases like `tracks.artist_uid`;
  when a track or album is merged away, its credit rows are dropped (the survivor keeps its
  own).
- Older peers don't know the tables or columns: check how sync handles a changelog row for
  an unknown entity / field and make sure it is ignored, not an error, both ways.

## Tags

`mc_tags` gains `artists`, `album_artists`, `mb_artistids`, `mb_albumartistids`, '\n'-separated
(see musiccore/tagger.h). A matched import writes ARTISTS, ALBUMARTISTS and every artist id;
ARTIST and ALBUMARTIST stay the credited strings. Folders stay under the full album credit.

## Refresh after upgrading

When `meta.artist_refresh` is `pending` the core runs a background pass once (resumable,
progress kept in meta, never blocks opening the library or other work):

1. Albums with `mb_albumid`: one release lookup each (1 req/s, shared limiter), then their
   credits (album and each track, matched by `mb_releasetrackid`, else disc/number) are
   rewritten from the release's artist-credit arrays. Skipped up front when neither the
   album's nor any track's credit string has a separator and the album has an artist id.
2. Everything else (and everything while offline): the string splitter against the library's
   own artists.
3. Files whose credit changed get their tags rewritten in place (ARTISTS, ids; ARTIST stays),
   no rename, no `file_rev` change; `files.size_bytes`/`mtime_ms`/`sha256` updated so sync
   doesn't see a changed file.
4. `artists` rows that no longer have a track, album or credit row are deleted, as delete.c
   does for empty artists.
Offline or interrupted: it picks up where it was next time. When done: `artist_refresh = 'done'`.
