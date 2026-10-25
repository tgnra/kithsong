# Tagging rules (draft)

> Status: design notes for `core/src/tagger`. Decide each of these rules before
> implementing it.

## Services

| Service | Key needed | Limits | Use |
|---------|------------|--------|-----|
| MusicBrainz `ws/2` | No | 1 req/s; descriptive User-Agent with contact info (`-DMUSICAPP_CONTACT`) | release search and lookup |
| Cover Art Archive | No | - | front covers |
| AcoustID | Yes, a free *application* key, set with `-DMUSICAPP_ACOUSTID_KEY` and never committed | 3 req/s | fingerprint to recording id |

## Pipeline

1. **Scan**: walk the import directory and read the existing tags of every audio file.
2. **Group** files into album candidates. Use existing `MUSICBRAINZ_ALBUMID` first, then
   (album artist, album), then the folder.
3. **Identify**:
   - if the files already carry MusicBrainz ids, look those up directly
   - otherwise search releases by artist, album and track count
   - otherwise fingerprint with Chromaprint and look up on AcoustID
4. **Score** each candidate release with a weighted distance (lower is better):
   - album title, artist, track count, year, label/catalogue number, country/media
     preferences
   - per-track: title, track length (±10 s tolerance), track number
5. **Assign** files to release tracks with the Hungarian algorithm over the per-track
   distance matrix.
6. **Confirm**: send ranked candidates to the frontend. Auto-accept above a confidence
   threshold (setting).
7. **Write**: update the library rows and write tags to the files. Optionally move or rename
   files according to the path format (see below).

## Field mapping (our structure)

| Library field | Tag property (TagLib) | Notes |
|---------------|-----------------------|-------|
| track title | `TITLE` | |
| track artist | `ARTIST` | credited name as on the release |
| album | `ALBUM` | |
| album artist | `ALBUMARTIST` | |
| year | `DATE` | release date; `ORIGINALDATE` for original_year |
| track/disc | `TRACKNUMBER`, `DISCNUMBER` | written as `n/total` |
| mb ids | `MUSICBRAINZ_TRACKID`, `MUSICBRAINZ_RELEASETRACKID`, `MUSICBRAINZ_ALBUMID`, `MUSICBRAINZ_ARTISTID` | |

To decide: how to handle featured artists, how to set sort names, whether to use
"Various Artists" for compilations, which genre source (MusicBrainz tags/genres), and the
path format, e.g. `$albumartist/$album ($year)/$disc-$track $title`.
