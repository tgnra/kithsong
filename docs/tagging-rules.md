# Tagging rules

> Status: the importer in `core/src/tagger` implements everything below except where marked
> *not yet*. Public API: the "Import" section of `core/include/musiccore/tagger.h`.

## Services

| Service | Key needed | Limits | Use |
|---------|------------|--------|-----|
| MusicBrainz `ws/2` | No | 1 req/s; descriptive User-Agent with contact info (`-DKITHSONG_CONTACT`) | release and recording search and lookup |
| Cover Art Archive | No | - | front covers, by release, then by release group |
| iTunes Search API | No | about 20 req/min (unofficial; we wait 3 s between requests) | front covers when the Cover Art Archive has none |

## Pipeline

Everything runs on one core-owned thread per import session (`tagger.c`).

1. **Scan**: walk the dropped folders and files and read the existing tags of every audio
   file the player can play (`flac mp3 opus wav aif aiff`). Hidden files,
   symlinks and junctions are skipped. A `.zip` given to the import, or found in a folder
   given to it, is extracted (`unzip.c`, miniz) into a staging folder named after it, in a
   temporary folder of the session's own that is deleted when the session is freed, then
   scanned like a folder, so its `cover.jpg` counts too. A folder's zips are extracted once
   the folder is walked, in path order. One level deep: a zip that comes out of a zip is
   left as it is, not extracted. Names are UTF-8 if the entry's flag bit 11 says so, else
   the name in its Info-ZIP Unicode Path extra field (0x7075) when that field's CRC-32
   matches the stored name, else CP437; entries with an absolute path or `..` are dropped (zip slip), and so are
   `__MACOSX/` and `.DS_Store`. Zip bombs: sizes are checked against what the zip claims
   before anything is written, and an entry that writes more than it claimed is stopped.
   Refused: a zip over 8 GB unzipped; one that would take all the session's zips together
   over 32 GB; and one bigger than the free space left once the zips the session already
   extracted are counted again (they're still to be copied into the library). A zip that
   can't be extracted shows as a failed item with the reason.
2. **Group** files into items:
   - key = album folder + album tag (or the release id if there is no album tag). The album
     folder is the file's folder, or its parent for disc folders such as `CD1` or `Disc 2`,
     so a multi-disc album stays together while two copies of an album in different folders
     stay apart.
   - files with neither an album tag nor a release id are **singles**, one item each.
   - a lone file whose album tag equals its title is also a single.
   - untagged singles get artist and title from the file name: `03 - Artist - Title.mp3`.
   - **disc folders**: albums whose album tag ends with a disc ("危ないベースライン Disc1",
     "… Disc2"; also `Disc 2`, `CD1`, `disk_3`, `(Disc 1 of 2)`, `[CD 2/2]`), or whose folder
     name does while the album tags agree, are items of their own at first. When several have
     the same title once the disc is taken off, each a different disc, in the same folder or
     folders side by side, they may be one album. Before they're matched, MusicBrainz is asked
     (one release search for the title and album artist, up to 3 lookups): a release with as
     many discs as there are items, on which at least half of each item's files are paired
     with tracks of its own disc. If there is one, the items are joined into the first: its
     files are all of theirs, in disc order (a file with no disc tag takes its item's disc),
     its album is the title, its candidates those releases scored as usual, and the other
     items are skipped as "Moved to another album". The joined item **always** waits for the
     user, saying it was joined ("Joined 2 discs into one album: check that they belong
     together"), however high it scores. If MusicBrainz has no such release, or can't be
     reached, the items stay apart and are matched one by one. Two items claiming the same
     disc are never joined.
   - **matching order**: albums first, then one-file items (singles, a lone file of an
     album, a DJ mix in one file), each in the order they were grouped, so the albums reach
     the review first.
3. **Identify**:
   - albums: if every file carries the same `MUSICBRAINZ_ALBUMID`, look that release up;
     then search releases by album and artist (not by artist when it is "Various Artists")
     and look up the best 3. Hits are ranked by expected track count, then search score,
     then earliest date, so the original release beats reissues.
   - a one-file album also gets recording candidates, in case it is one track off a release.
   - singles: look up `MUSICBRAINZ_TRACKID` if present, then search recordings by title and
     artist (5 results). Each recording is credited to one of its releases (see
     [Singles](#singles)).
   - albums and one-file items with no candidates, or only poor ones (best under 75%): the
     [extended search](#extended-search), by catalogue number, other artists and the label.
     It gives a one-file item release candidates, in case the file is a whole release (a DJ
     mix whose album tag is its title, say).
4. **Score** each candidate with a weighted distance, 0 to 1 (`match.c`):
   - strings are compared by normalised edit distance. Case, punctuation, common Latin
     accents, a leading "The" and `&`/"and" are ignored, and a bracketed or ` - ` suffix such
     as "(2011 Remaster)" costs at most 0.1.
   - per track: title 3, length 2 (no penalty within ±10 s, full penalty from ±30 s), track
     number 1 (per disc or straight through), artist 1.
   - per release: album title 5, album artist 5, mean track distance 6, fraction of release
     tracks with no file 10, fraction of files with no track 6, year 0.5.
   - per recording: title 3, artist 3, length 2.
   - ties go to the earliest original release.
5. **Assign** files to release tracks with the Hungarian algorithm over the per-track
   distance matrix. A pair with track distance over 0.6 is dropped, so a wrong title is never
   written just to fill a slot.
6. **Confirm**: confidence = 1 - distance. The best candidate is accepted automatically if
   its confidence is at least the threshold (default 0.9) **and** it is complete: no missing
   tracks and no leftover files (a recording candidate always is). Everything else waits for
   the user in the review dialog,
   where they can pick another candidate, search again with their own terms, keep the
   existing tags, or skip the item. Results of a manual search are never auto-accepted, and
   nor are joined disc folders (see Group). They can also **search more widely** (the
   extended search on request, for an album or a single, whose results join the candidates
   and aren't auto-accepted either).
   **Use as track N** fixes an assignment the tagger got wrong: the review's track list shows
   the missing tracks, the files no track took, and the files of other items from the same
   folder (`mc_import_related`, e.g. a file whose album tag put it in an item of its own).
   Clicking a missing track (or Enter on it; on Linux its Choose File… button) lists the
   files it can take, and choosing one (`mc_import_assign`), or selecting the missing track
   and a file together and choosing Use as Track N from the right-click menu, makes the file
   that track and writes the item with that match,
   whether it was in review or already imported (then its files are re-tagged and moved in
   place, and its library rows updated). A file taken from another item leaves it; if that
   item had imported it, its library row moves to this album, and an item left with no files
   is skipped as "Moved to another album". The match's confidence (and its missing tracks and
   leftover files) is then worked out again from the files and tracks as they're paired now,
   with no new request, so a match that was low only for want of that track goes up; every
   pair counts, even one the tagger would have dropped as too far apart. The other matches of
   both items are rescored too, but keep their order.
7. **Compare** with the library (`dupes.c`, see [Duplicates](#duplicates)): skip what it
   already has, or swap in a better copy of the release.
8. **Write** (see [Library folder](#library-folder)): copy the files into the library folder
   (or move them, if they're already inside it), write the tags to the copies, make sure
   the album folder has a cover, then write the library rows in one transaction
   (`import_db.c`). If any step fails, the files are put back. Files that weren't matched to
   a track aren't imported.

## Extended search

Albums often carry a label or circle in the album artist tag instead of the artist: a
doujin label's releases tagged "SHARPNELSOUND" or "SHARPNELSOUND (DJ CHUCKY)" when MusicBrainz
credits "DJ CHUCKY", "DJ SHARPNEL" or "Various Artists". The normal search (album title with
that album artist) then finds nothing, or only the wrong releases. The real artist and the
release usually show up elsewhere (`search_keys.c` gathers them):

- **catalogue numbers**: `CATALOGNUMBER` (or `CATALOGID`, `CATALOG`, `LABELNO`) tags, and
  `[bracketed]` groups in the album tag ("BREAK IT OLDSCHOOL [SRPC-L007]") and the folder
  name, split at `/`. A catalogue number has an ASCII
  letter and at least three digits, so dates (`[2002.08.11]`), events (`(C89)`) and notes
  like `[24bit]` or `[FLAC]` don't count. `SRPC-0008LTD` also tries `SRPC-0008`.
- **artists**: the parenthesised part of a `Label (Artist) - Title [CAT-001] (Event)` folder
  name or album artist tag, without "and V.A."; and the artist that the track artist tags,
  the file names (`03. Artist — Title.flac`, with `-`, `–` or `—`, the side that isn't the
  title tag) or, where the artist tag just repeats the album artist, the titles
  (`Artist - Title`) agree on for at least half the files. "feat." parts are dropped.
- **labels**: `LABEL` (or `ORGANIZATION`, `PUBLISHER`) tags, and the name standing where the
  artist would (the folder name's prefix, the album artist tag) when it has a parenthesised
  artist, or differs from the artist the tracks agree on, or the tracks are a compilation.
- **titles**: the album tag, and the folder name's title (after ` - `, up to the first
  `[group]`, without a trailing `(2004)`) when it differs. For a single, the album tag (its
  title is the track's); without one, the folder name counts only when it looks like a
  release (`Prefix - Title`, or with a catalogue number), since a loose track's folder is
  often just where it was saved.
- a **compilation**: "V.A." in the parentheses, or no artist on at least half the files.

The queries, most specific first, each skipped when its keys are missing:

1. `catno:(A OR B)`, looking up at most 3 of the hits;
2. title with the first artist; 3. title with the labels (`label:`); 4. the title alone (only
   if the normal search had an artist); 5. title with the second artist; 6. the folder's
   title alone. At most 2 lookups each.

The budget is 4 searches and 6 release lookups beyond the normal search (about 10 s at
1 request/second), and it stops early once a candidate would be accepted. Hits already
among the candidates aren't looked up again.

**Scoring** is the normal one (tracks, lengths, track numbers, missing and extra files all
count as before), except that the album title and album artist parts use whichever fits the
release best among the item's own, the keys' titles and artists, and "Various Artists" for a
compilation. Every release candidate is rescored this way, including the normal search's.
"V.A." as a release artist counts as Various Artists.

**When**: automatically for an album or a one-file item whose best candidate is below **75%**
confidence, or that has none. The threshold comes from the scoring: the album artist is 5 of the release
distance's 32.5 weight units, so the right release with a wrong album artist tag loses at
most about 15 points and still scores around 85-90%; below 75%, the title, a quarter of the
tracks or more, or both disagree too, which means the wrong release. Results are treated
like the normal search's: the same threshold and completeness rules decide whether the best
one is accepted. Above 75% but not accepted, the review offers **Search more widely**, which
runs the same search on request; its results join the candidates and are never accepted by
themselves. Singles have it too: a one-file item may be a whole release, and its release
candidates are scored like an album's, so one is accepted only if it has that one track (a
recording candidate for a single isn't held to that). One-file items are matched after the
albums, so their extended searches don't hold the albums up. After a manual search, the
button is offered again.

**Offline**: it isn't started while MusicBrainz can't be reached (the normal search failed
with a network error, or the session is offline), and a network error ends it early: the
item then keeps what it has, or waits for MusicBrainz like any other. The frontends disable
the button while offline.

## Library folder

The library is one folder the user chooses (default: their Music folder). It holds the
database as `library.db` at the top and the music beneath it (`organize.c`):

```
Radiohead/OK Computer (1997)/01 Airbag.flac
Radiohead/OK Computer (1997)/cover.jpg
Some Band/Double Album (2004)/2-07 Title.flac      (disc-track, when there are several discs)
OutKast/Hey Ya! (2003)/01 Hey Ya!.flac             (a single: its release, of one track)
OutKast/Demo.flac                                  (a track with no album)
library.db
```

- **Originals are never modified.** Files from outside the library are copied in and the
  copy is tagged. Files already inside the library are moved (renamed) to their new place,
  and folders left empty, or holding only a cover, are removed.
- the year is left out of the folder name when it isn't known. As-is imports use the
  existing tags, then the file name for the title.
- names are made safe for Windows everywhere (the strictest of our platforms), so a library
  can move between systems: `<>:"/\|?*` and control characters become `_`, leading dots and
  trailing dots or spaces are dropped, device names such as `CON` get a `_`, and each part
  is cut to about 100 bytes.
- if the destination is taken: the same release track imported again from outside the
  library replaces its old copy; anything else gets ` (2)`, ` (3)`, ... appended. (A release
  that's already in the library doesn't get this far: see [Duplicates](#duplicates).)
- a track with no album goes in its artist's folder, `<artist>/<title>.<ext>`.
- files filed under "&lt;artist&gt; Singles" by older versions are albums of that title like any
  other when scanned again: nothing treats the name specially any more.
- `files.path` is stored relative to the library folder, with `/`, so the folder can be moved
  or mounted under another drive letter.
- the library folder lives in per-device settings (`settings.json`, see
  `core/include/musiccore/settings.h`), in `%LOCALAPPDATA%\kithsong` on Windows and
  `~/.config/kithsong` on Linux.
- a database inside a OneDrive/Dropbox-style synced folder can be corrupted by the sync
  client; the Windows frontend warns when the chosen folder is under OneDrive.

## Duplicates

Before an item from outside the library folder is written, it's compared with the library.
The full rules are in `core/include/musiccore/tagger.h` ("Duplicates"); in short:

- **Same release**: an album with the same MusicBrainz release id, else release group id,
  else the same album artist and title with at least half of the incoming tracks in its
  track list. A single is an album like these. A track with no album: the same recording
  (else title) among the artist's tracks with no album, each judged on its own. Tracks pair
  up by release track id, recording id, disc and track number with the title, then title.
- **Quality** of a file: lossless beats lossy; then bits per sample and sample rate for
  lossless, or bitrate weighted by codec for lossy (MP3 1.0, Opus 1.6;
  less than 10% apart is equal).
- **Better as a whole**: the incoming release has every track the library holds a file of,
  none worse, and more than half better. Then its files replace the library's in place: the
  album and tracks keep their uids (so playlists, album groups, plays, recents and sync
  profiles keep them), the new file takes the old one's name with its own extension, and
  the old file goes to the Recycle Bin or trash. The track's `file_rev` changes, which tells
  other devices to replace their copies (see "Files" in sync-protocol.md).
- **A swap re-tags the new file to match the library**: it is a better copy of the track the
  library has, so it gets the library's title, artist, track and disc numbers, year and
  MusicBrainz ids, and the album's title, album artist and ids (ids the library lacks are
  removed), not those of its own match, which can be another release of the group or, found
  by name, no release at all. A file attached to a track with no file on this device is
  tagged and named the same way. A track with no album gets no album tags, ids or numbers.
- **Unless it's the whole release**: the album was found by release or release group id
  (not by name, not tracks with no album), every one of its tracks pairs with an incoming file,
  and every file of it on this device is swapped. Then the import's match wins: the new
  files keep its tags and names, and the album and its tracks take its title, artists,
  numbers, years and ids, keeping their uids. Typically a remaster or hi-res release of the
  same release group replacing the whole CD rip. The old folder goes if that empties it,
  its cover moving to the new one unless the import brings its own. An item imported with
  its existing tags has no match to win, so it is re-tagged as above.
- Otherwise the tracks the library has are **skipped**, with no ` (2)` copies; tracks it
  lacks are added to its album either way.
- **The same file again** is skipped. Every file imported from outside the library records
  the SHA-256 of the bytes it was imported from, before its copy was tagged
  (`files.source_sha256`; sync passes it on with an original). An incoming file whose bytes
  hash to a recorded source hash, or to a library file's `sha256` (or, without one, to the
  bytes of a library file of the same size), is already in the library. Files imported
  before this have no source hash and are only recognised by their current bytes.
- The item says what happened: replaced with higher quality, added the missing tracks, or
  already in the library (the item is then skipped).

## Singles

A single is filed as what it is on MusicBrainz: a track of a release, like any album track.

- an item is a single if it is a loose track (see Group). It's matched to a recording, and
  the recording is credited to one of its releases: official beats unofficial, a single
  beats an album or EP, compilations come last, and ties go to the earliest date.
- once accepted, that release is looked up in full and the file is written as its track
  there: the release's `ALBUM`, `ALBUMARTIST`, `MUSICBRAINZ_ALBUMID`,
  `MUSICBRAINZ_RELEASEGROUPID`, track and disc numbers and `DATE`, under
  `<album artist>/<album> (<year>)/`. A one-track release is an album of one track; one
  track of an album imported on its own is that album's track, of its total.
- if the release can't be looked up, the search's release title and ids and the track's
  number are written, with the recording's first credited artist as the album artist and
  no track total. A recording on no release gets no album.
- in the library a single is an ordinary album (`albums.kind` is unused, see schema.md).
  An album with **one track in the library** is shown as a single: the album listings and
  search leave it out, and the artist's page lists it with the artist's tracks that have
  no album (`mc_library_list_artist_singles`). Importing more of its release makes it an
  album again.
- a loose track imported as-is with no album tag gets no album: `album_uid` is NULL and the
  file goes in `<artist>/<title>.<ext>`.
- there's no "&lt;artist&gt; Singles" album any more. Albums and files made as one before
  stay as they are, as an album of that title, until the music is imported again.

## Artist credits

A track or album credited to "A feat. B", "A, B & C" or "A x B" belongs to each of A, B and
C. The credit as written is what's shown and what goes in `ARTIST` / `ALBUMARTIST`; its
artists, in order, each with the join phrase that follows it (" feat. ", " & "), are what
the import passes on to the library (`mc_db_album.credit`, `mc_db_track.credit`). Folders
stay under the whole album credit: `Jay-Z & Kanye West/Watch the Throne (2011)/`.

Where the artists come from:

1. **MusicBrainz**, for a matched import: the `artist-credit` array of the release and of
   each track (of the recording, for a recording found on its own), as it is: each artist's
   name as credited, id, sort name and join phrase. It is never split again. The files get
   `ARTISTS` and `ALBUMARTISTS` (one value per artist, by name as credited) and every id in
   `MUSICBRAINZ_ARTISTID` and `MUSICBRAINZ_ALBUMARTISTID`. A track with no album has its
   album lists removed; a recording whose release can't be looked up has its first credited
   artist as the album artist, so the album lists hold that one.
2. **Multi-valued tags**, for an import as-is: `ARTISTS` with `MUSICBRAINZ_ARTISTID` for the
   track, `ALBUMARTISTS` with `MUSICBRAINZ_ALBUMARTISTID` for the album (`ARTISTS` when the
   album artist is the track artist). The ids go with the names only when there are as many
   of each. The join phrases are what's between the names in the `ARTIST` string when they
   are all there in order (ignoring case), else "; ". A single name that is the credit
   itself is just the credit.
3. **The credit string**, otherwise, split by `mc_credit_split_text` (`credit.c`):
   - "feat."-style separators always split: ` feat. `, ` feat `, ` ft. `, ` ft `,
     ` featuring `, and a closing `(feat. B)` or `[feat. B]` (with any of those words).
   - Ambiguous separators split only where every resulting part is an artist the library
     knows and the whole isn't one: ` & `, `, `, ` and `, ` x `, ` vs. `, ` vs `, ` with `,
     ` / `, `; `, and the serial-comma forms `, & ` and `, and `. The longest known name is
     taken first, so "Earth, Wind & Fire & The Emotions" keeps "Earth, Wind & Fire" whole
     when it's known; when any part isn't known, the whole stretch stays one artist
     ("Tyler, the Creator").
   - An artist is known when the library has an `artists` row of that name (ignoring case)
     that has a MusicBrainz id, or whose name has no separator at all. Older versions made
     rows like "A & B" from whole credits; those don't count, but "Simon & Garfunkel" with
     its MusicBrainz id does, and stays whole.
   - Separators match ignoring case and need the spaces around them ("AC/DC", "Aftermath"
     stay whole). Parts are trimmed and never empty: a credit that would give an empty part
     ("A & & B", "A feat.") is one artist.

From tags or the string, a credit of one artist under its own name is passed as the credit
alone (`credit_count` 0), with the `MUSICBRAINZ_ARTISTID` the file has. The whole credit's
MusicBrainz id is only set when one artist is credited.

### Refresh after upgrading

Versions before artist credits (library v13) imported "Jay-Z feat. Rihanna" as one artist.
Migration v13 sets `meta.artist_refresh` to `'pending'`, and opening the library then splits
those credits once, by itself, in the background (`core/src/tagger/refresh.c`). Frontends
don't start it and needn't show it; `mc_library_refresh_progress()` says whether it's running
and how far it is.

- **Where it runs.** `mc_library_open()` starts it on a thread of its own, with a connection
  of its own, when the library is pending and `mc_init()` has run; `mc_library_close()` stops
  it. It waits 3 s first, so a handle opened for a moment does nothing. One runs per library
  folder in the process: the connections the core opens for itself (an import's, sync's, a
  library move's check) use `mc_library_open_internal()`, which starts none, and a second
  frontend handle on the same folder finds one running. Opening never waits for it.
- **Albums with a release id** (`albums.mb_albumid`): one release lookup each (the shared 1
  request/s limit, so it queues behind an import's), and the album's credit and each of its
  tracks' (those with an artist of their own, paired by `mb_releasetrackid`, else by disc and
  number) are rewritten from the release's artist-credit arrays, as an import writes them.
  Skipped without a lookup: an album whose artist has a MusicBrainz id, and none of whose
  credits (its own, its tracks') has a separator with an artist lacking an id, so music a
  v13 import credited from MusicBrainz is left alone. A release MusicBrainz no longer has
  goes to the splitter. If MusicBrainz can't be reached (`mc_musicbrainz_reachable()`, or a
  lookup's network error), this step stops where it is and is tried again on a later open.
- **Everything else**, and the albums the first step hasn't reached while offline: the
  string splitter (`mc_credit_split_text`, against the library's artists), for credits with a
  separator that aren't split yet. An artist it names that the library doesn't have gets a
  uid derived from the name (`mc_uid_derive("artist/<name, ASCII lowercase>", 0)`), so two
  devices splitting the same credit offline make one row rather than two that never merge.
  Looked up later, such an artist is the one MusicBrainz names (matched by name, its id
  filled in).
- **Files.** Each track whose credit changed, or whose album's did, gets its files on this
  device retagged: `ARTIST` and `ALBUMARTIST` (the credits as shown, unchanged), `ARTISTS`
  and `ALBUMARTISTS`, and the artist id lists when every artist has an id; nothing else.
  A tagged copy is written beside the file and renamed over it, so the player or sync reading
  it sees the old bytes or the new, never half of each, and a crash leaves the old file. No
  rename, no `file_rev`: the same file, newly tagged. Its `files` row takes the new size and
  mtime, and the new `sha256` if it had one, in the same transaction as the rename, so sync's
  checks and the import's duplicates check see a known, unchanged file; `source_sha256`
  stays (it is what the file was imported from). A file that's missing, read-only, or not
  what its row says (changed outside, or by an import just now) is left as it is.
- **Empty artists.** An artist that no track, album or credit row names any more is deleted,
  as `delete.c` deletes them, and the delete is logged so it syncs: the old whole-credit rows
  go once the last credit naming them is split.
- **Resuming.** Each album (or track with no album) is one write transaction: its credits,
  the empty artists, the step's cursor in `meta` (`artist_refresh_mb`, `artist_refresh_split`,
  `artist_refresh_loose`: the last uid done, or `'done'`) and the tracks to retag
  (`artist_refresh_files`, cleared once they are). Closing stops it at its next step: a
  lookup under way is given up, and a busy wait for another connection's lock too. The next
  open retags what was left, then goes on after the cursors. When all three are `'done'`,
  `artist_refresh` is `'done'` and the cursors go.
- **Sync.** The changes are ordinary logged rows. Two devices refreshing the same library
  write the same credit rows (their uids derive from the owner), the same split artists'
  rows, and artists with MusicBrainz ids created on both merge by `mb_artistid` as any
  duplicates do. A track's files are retagged on each device by its own refresh; sync
  doesn't send them again (the track's `file_rev` didn't change).

## Field mapping

| Library field | Tag property (TagLib) | Notes |
|---------------|-----------------------|-------|
| track title | `TITLE` | |
| track artist | `ARTIST` | credited name as on the release ("A feat. B") |
| track's artists | `ARTISTS` | one value per credited artist; see [Artist credits](#artist-credits) |
| album | `ALBUM` | removed for a track with no album |
| album artist | `ALBUMARTIST` | credited name as on the release |
| album's artists | `ALBUMARTISTS` | one value per credited artist; removed for a track with no album |
| year | `DATE` | release date; original year for a track with no album |
| original year | `ORIGINALDATE` | release group's first release |
| track/disc | `TRACKNUMBER`, `DISCNUMBER` | written as `n/total`; not for a track with no album |
| mb ids | `MUSICBRAINZ_TRACKID` (recording), `MUSICBRAINZ_RELEASETRACKID`, `MUSICBRAINZ_ALBUMID`, `MUSICBRAINZ_ARTISTID`, `MUSICBRAINZ_ALBUMARTISTID`, `MUSICBRAINZ_RELEASEGROUPID` | `MUSICBRAINZ_ARTISTID` and `MUSICBRAINZ_ALBUMARTISTID` hold one id per credited artist, in order |

Writing an empty string through `mc_tags_write()` removes that tag. Multi-valued properties
are '\n'-separated lists in `mc_tags` (`artists`, `album_artists`, `mb_artistids`,
`mb_albumartistids`); `mb_artistid` and `mb_albumartistid` read as the first value. In ID3v2
(MP3, WAV) `ARTISTS` and the artist ids are `TXXX` frames ("ARTISTS", "MUSICBRAINZ ARTIST
ID", as TagLib names them) with one value per string; in FLAC and Opus, one Vorbis comment
per value.

## Cover art

Each album folder gets a `cover.jpg` (or `cover.png`). `albums.art_hash` holds its SHA-256,
so devices can tell whether they have the same image. That file is the album's art on every
device: pictures embedded in audio files are never shown, only copied out on import. In order:

1. a cover already in the album folder is kept.
2. a cover in the folder the files came from (`cover`, `folder` or `front`, `.jpg`/`.jpeg`/
   `.png`, any case) is copied.
3. a picture embedded in one of the imported files (the front cover, else the first one;
   JPEG or PNG up to 16 MB) is written out.
4. Cover Art Archive `release/<id>/front-1200`.
5. Cover Art Archive `release-group/<id>/front-1200`.
6. iTunes Search (`entity=album`). A result is used only if its album and artist names are
   both close (combined distance below 0.25). The 100 px artwork URL is rewritten to
   1200 px.

Only JPEG and PNG up to 16 MB are downloaded. A single gets its release's cover like any
album; a track with no album gets none. Art isn't embedded in the files yet.

## Still to decide

How to set sort names for multi-artist credits, which genre source to use (MusicBrainz
tags/genres), whether the path format should be configurable, whether to embed cover art,
and how to detect duplicates beyond "same release track, same place".
