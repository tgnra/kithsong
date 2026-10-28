# Tagging rules

> Status: the importer in `core/src/tagger` implements everything below except where marked
> *not yet*. Public API: the "Import" section of `core/include/musiccore/tagger.h`.

## Services

| Service | Key needed | Limits | Use |
|---------|------------|--------|-----|
| MusicBrainz `ws/2` | No | 1 req/s; descriptive User-Agent with contact info (`-DKITHSONG_CONTACT`) | release and recording search and lookup |
| Cover Art Archive | No | - | front covers, by release, then by release group |
| iTunes Search API | No | about 20 req/min (unofficial; we wait 3 s between requests) | front covers when the Cover Art Archive has none |
| AcoustID | Yes, a free *application* key, set with `-DKITHSONG_ACOUSTID_KEY` and never committed | 3 req/s | fingerprint to recording id (*not yet*) |

## Pipeline

Everything runs on one core-owned thread per import session (`tagger.c`).

1. **Scan**: walk the dropped folders and files and read the existing tags of every audio
   file (`flac mp3 m4a mp4 aac ogg oga opus wav aif aiff wv ape mpc wma dsf`). Hidden files,
   symlinks and junctions are skipped.
2. **Group** files into items:
   - key = album folder + album tag (or the release id if there is no album tag). The album
     folder is the file's folder, or its parent for disc folders such as `CD1` or `Disc 2`,
     so a multi-disc album stays together while two copies of an album in different folders
     stay apart.
   - files with neither an album tag nor a release id are **singles**, one item each.
   - a lone file whose album tag equals its title is also a single.
   - untagged singles get artist and title from the file name: `03 - Artist - Title.mp3`.
3. **Identify**:
   - albums: if every file carries the same `MUSICBRAINZ_ALBUMID`, look that release up;
     then search releases by album and artist (not by artist when it is "Various Artists")
     and look up the best 3. Hits are ranked by expected track count, then search score,
     then earliest date, so the original release beats reissues.
   - a one-file album also gets recording candidates, in case it is one track off a release.
   - singles: look up `MUSICBRAINZ_TRACKID` if present, then search recordings by title and
     artist (5 results).
   - albums with no candidates, or only poor ones (best under 75%): the
     [extended search](#extended-search), by catalogue number, other artists and the label.
   - fingerprinting with Chromaprint + AcoustID when tags aren't enough (*not yet*).
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
   tracks and no leftover files. Everything else waits for the user in the review dialog,
   where they can pick another candidate, search again with their own terms, keep the
   existing tags, or skip the item. Results of a manual search are never auto-accepted.
   For an album they can also **search more widely** (the extended search on request,
   whose results join the candidates and aren't auto-accepted either).
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
   is skipped as "Moved to another album".
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
  `[bracketed]` groups in the folder name, split at `/`. A catalogue number has an ASCII
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
  `[group]`, without a trailing `(2004)`) when it differs.
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

**When**: automatically for an album whose best candidate is below **75%** confidence, or that
has none. The threshold comes from the scoring: the album artist is 5 of the release
distance's 32.5 weight units, so the right release with a wrong album artist tag loses at
most about 15 points and still scores around 85-90%; below 75%, the title, a quarter of the
tracks or more, or both disagree too, which means the wrong release. Results are treated
like the normal search's: the same threshold and completeness rules decide whether the best
one is accepted. Above 75% but not accepted, the review offers **Search more widely**, which
runs the same search on request; its results join the candidates and are never accepted by
themselves. Singles don't have it (their keys are album clues). After a manual search, the
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
OutKast/OutKast Singles/Hey Ya!.flac
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
- files with an album tag of "&lt;album artist&gt; Singles" are treated as singles when
  scanned again, so re-importing the library folder keeps them in place.
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
  track list; for a single, the same recording in the artist's singles album. Tracks pair
  up by release track id, recording id, disc and track number with the title, then title.
- **Quality** of a file: lossless beats lossy; then bits per sample and sample rate for
  lossless, or bitrate weighted by codec for lossy (MP3 1.0, AAC and Vorbis 1.3, Opus 1.6;
  less than 10% apart is equal).
- **Better as a whole**: the incoming release has every track the library holds a file of,
  none worse, and more than half better. Then its files replace the library's in place: the
  album and tracks keep their uids (so playlists, album groups, plays, recents and sync
  profiles keep them), the new file takes the old one's name with its own extension, and
  the old file goes to the Recycle Bin or trash. The track's `file_rev` changes, which tells
  other devices to replace their copies (see "Files" in sync-protocol.md).
- Otherwise the tracks the library has are **skipped**, with no ` (2)` copies; tracks it
  lacks are added to its album either way. A file whose bytes are already in the library is
  skipped too.
- The item says what happened: replaced with higher quality, added the missing tracks, or
  already in the library (the item is then skipped).

## Singles

beets imports a single as a one-track album named after the song. We file every single
under one album per artist instead:

- an item is a single if it is a loose track (see Group), or if its best match is a release
  whose MusicBrainz release group has primary type *Single*.
- it goes into the album **"&lt;artist&gt; Singles"**, where the artist is the first credited
  artist. "Beyoncé feat. Jay-Z" files under "Beyoncé Singles", and the track keeps the full
  credit.
- in the library that album has `albums.kind = 'singles'`, one per artist. Each track keeps
  its own `year` and the `mb_albumid` of the release it came from.
- in the file tags, `ALBUM` is "&lt;artist&gt; Singles" and `ALBUMARTIST` is the artist.
  `MUSICBRAINZ_ALBUMID` and `MUSICBRAINZ_RELEASEGROUPID` are removed, because they would
  point at a release whose title isn't the album tag. Track and disc numbers aren't written.
- the release a recording is credited to: official beats unofficial, a single beats an album
  or EP, compilations come last, and ties go to the earliest date.
- the singles album gets no cover art for now.

## Field mapping

| Library field | Tag property (TagLib) | Notes |
|---------------|-----------------------|-------|
| track title | `TITLE` | |
| track artist | `ARTIST` | credited name as on the release |
| album | `ALBUM` | "&lt;artist&gt; Singles" for singles |
| album artist | `ALBUMARTIST` | |
| year | `DATE` | release date; original year for singles |
| original year | `ORIGINALDATE` | release group's first release |
| track/disc | `TRACKNUMBER`, `DISCNUMBER` | written as `n/total`; not for singles |
| mb ids | `MUSICBRAINZ_TRACKID` (recording), `MUSICBRAINZ_RELEASETRACKID`, `MUSICBRAINZ_ALBUMID`, `MUSICBRAINZ_ARTISTID`, `MUSICBRAINZ_ALBUMARTISTID`, `MUSICBRAINZ_RELEASEGROUPID` | artist ids only when a single artist is credited |

Writing an empty string through `mc_tags_write()` removes that tag.

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

Only JPEG and PNG up to 16 MB are downloaded. Singles albums get no cover. Art isn't
embedded in the files yet.

## Still to decide

How to set sort names for multi-artist credits, which genre source to use (MusicBrainz
tags/genres), whether the path format should be configurable, whether to embed cover art,
whether the singles album should get a generated cover, and how to detect duplicates beyond
"same release track, same place".
