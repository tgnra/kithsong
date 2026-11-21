# Tagging rules

> Status: the importer in `core/src/tagger` implements everything below, except where it's
> marked *not yet*. Public API: the "Import" section of `core/include/kithcore/tagger.h`.

## Services

| Service | Key needed | Limits | Use |
|---------|------------|--------|-----|
| MusicBrainz `ws/2` | No | 1 req/s; descriptive User-Agent with contact info (`-DKITHSONG_CONTACT`) | release and recording search and lookup |
| Cover Art Archive | No | - | front covers, by release, then by release group |
| iTunes Search API | No | about 20 req/min (unofficial; we wait 3 s between requests) | front covers when the Cover Art Archive has none |

## Pipeline

Everything runs on one thread per import session, owned by the core (`tagger.c`).

1. **Scan**: walk the dropped folders and files, and read the existing tags of every audio
   file the player can play (`flac mp3 opus wav aif aiff`). Hidden files, symlinks and
   junctions are skipped.
   - A `.zip` given to the import, or found in a folder given to it, is extracted (`unzip.c`,
     miniz) and then scanned like a folder, so its `cover.jpg` counts too.
   - It's extracted into a staging folder named after the zip. The staging folder sits in a
     temporary folder that belongs to the session, and that's deleted when the session is
     freed.
   - A folder's zips are extracted once the folder has been walked, in path order.
   - Zips are only opened one level deep. A zip that comes out of a zip is left alone, not
     extracted.
   - Entry names are UTF-8 if the entry's flag bit 11 is set. Otherwise it's the name in the
     entry's Info-ZIP Unicode Path extra field (0x7075), when that field's CRC-32 matches the
     stored name. Otherwise it's CP437.
   - Entries with an absolute path or `..` are dropped (zip slip), and so are `__MACOSX/` and
     `.DS_Store`.
   - Zip bombs: sizes are checked against what the zip claims before anything is written, and
     an entry that writes more than it claimed is stopped.
   - A zip is refused if:
     - it's over 8 GB unzipped
     - it would take all the session's zips together over 32 GB
     - it's bigger than the free space left, counting the zips the session has already
       extracted a second time (they still have to be copied into the library).
   - A zip that can't be extracted shows as a failed item, with the reason.
2. **Group** files into items:
   - The key is album folder + album tag (or the release id if there's no album tag). The
     album folder is the file's folder, or its parent for disc folders such as `CD1` or
     `Disc 2`. That way a multi-disc album stays together, but two copies of an album in
     different folders stay apart.
   - Files with neither an album tag nor a release id are **singles**, one item each.
   - A lone file whose album tag is the same as its title is also a single.
   - Untagged singles get their artist and title from the file name: `03 - Artist - Title.mp3`.
   - **disc folders**: an album whose album tag ends with a disc ("危ないベースライン Disc1",
     "… Disc2"; also `Disc 2`, `CD1`, `disk_3`, `(Disc 1 of 2)`, `[CD 2/2]`) starts out as a
     separate item. So does one whose folder name ends with a disc, as long as its album tags
     agree. Several of these may be one album, if they have the same title once the disc is
     taken off, each is a different disc, and they're in the same folder or in folders side
     by side.
     - Before they're matched, Kithsong asks MusicBrainz for a release with as many discs as
       there are items (not counting video discs, see Score). On that release, at least half
       of each item's files must pair with tracks on that item's disc.
     - It starts with one release search for the title and album artist, with up to 3
       lookups.
     - If that finds nothing (for example when the album artist tag names a label), it uses
       the [extended search](#extended-search)'s keys from all the discs' files and the
       folder name: `catno:` with their catalogue numbers, then the title with each other
       artist. That's at most 3 more searches and 4 more lookups, and it stops once a release
       would be accepted.
     - If there's a release like that, the items are joined into the first one. Its files are
       all of theirs, in disc order (a file with no disc tag takes its item's disc). Its album
       is the title. Its candidates are those releases, scored as usual (with the keys'
       best-fitting title and artist, see Score). The other items are skipped as
       "Moved to another album".
     - The joined item **always** waits for the user, however high it scores, and says it was
       joined ("Joined 2 discs into one album. Check that they belong together."). It still
       says so after the user runs their own search.
     - If the user keeps its existing tags, every disc is filed under the title, not the
       first disc's album tag.
     - If MusicBrainz has no such release, the items stay apart and are matched one by one.
       If MusicBrainz can't be reached, they wait for it like any other item, and Kithsong
       tries to join them again when they're looked up again.
     - Two items that claim the same disc are never joined.
   - **other discs**: when an album is matched to a release that has discs it has no files
     on (disc 1 of 2, say; a video disc doesn't count), it waits instead of going to the user
     while items that may be those discs are still to be matched. Those are albums whose
     files carry the release's id, or whose title without a disc is the album's or the
     release's, and that have no more files than those discs have tracks.
     - It shows as waiting ("Waiting for the album's other disc").
     - Meanwhile, an item may be matched whose candidates include that release, with at least
       half its files paired with tracks on discs the waiting album has no files on. That
       item is joined into the waiting album, the same way as disc folders. Its files join the
       album's in disc order (a file with no disc tag takes its track's disc). Both items'
       releases are scored again against all the files. It's skipped as
       "Moved to another album".
     - The album goes to the user once it has every disc, or once there's nothing left to
       match that might be them. That's at the end of the matching at the latest.
     - It's never accepted automatically, and it says it was joined if it was.
   - **matching order**: albums come first, then one-file items (singles, a lone file of an
     album, a DJ mix in one file), each in the order they were grouped. That way the albums
     reach the review first.
3. **Identify**:
   - Albums: if every file carries the same `MUSICBRAINZ_ALBUMID`, that release is looked up.
     Then releases are searched by album and artist (not by artist when it's "Various
     Artists"), and the best 3 are looked up.
     - Hits are ranked by expected track count, then search score, then earliest date, so
       the original release beats reissues.
     - The album title is searched without bracketed edition notes and catalogue numbers.
       "SEX -Sound of EXtreme- (Promo CDr)" is searched as "SEX -Sound of EXtreme-", and the
       same goes for "(Limited Edition)", "(Comiket Version)", "[Bonus Track Version]",
       "（初回限定盤）" and "[SRPC-L007]".
     - A group is removed if it has a word such as promo, CD-R, limited, edition, version,
       bonus, reissue or remaster(ed), or 限定, 初回, 盤 or 版. It's also removed if it's a
       catalogue number (see [Extended search](#extended-search)).
     - This applies to every release search, including the extended search and a manual
       search. The scoring still compares the whole title.
   - A one-file album also gets recording candidates, in case it's one track from a release.
   - Singles: `MUSICBRAINZ_TRACKID` is looked up if it's there, then recordings are searched
     by title and artist (5 results). Each recording is credited to one of its releases (see
     [Singles](#singles)).
   - Albums and one-file items with no candidates, or only poor ones (best under 75%), get
     the [extended search](#extended-search), by catalogue number, other artists and the
     label. It gives a one-file item release candidates too, in case the file is a whole
     release (a DJ mix whose album tag is its title, say).
4. **Score** each candidate with a weighted distance, from 0 to 1 (`match.c`):
   - Strings are compared by normalised edit distance.
     - Case, punctuation, common Latin accents, a leading "The" and `&`/"and" are ignored.
     - A bracketed or ` - ` suffix such as "(2011 Remaster)" costs at most 0.1.
     - Since punctuation is ignored, so are dashes and tildes around part of a title
       ("SHARPNELSOUND COLLECTION -壱-" is "SHARPNELSOUND COLLECTION 壱"), and hyphens inside
       words ("X-RATED", "J-CORE").
   - A subtitle after a space and a tilde (`~`, `～`, `〜`: "HARDDRIVE 150BPM ～LIFE IS GAME"),
     or wrapped in dashes after a space ("S.E.X. -Sound of EXtreme-"), counts as a suffix
     like "(2011 Remaster)". A hyphen with no space before it, or one that isn't closed,
     doesn't.
   - A translation added to a title costs at most 0.1: "アニメガバイト ANIME GABBA IT!" vs
     "アニメガバイト".
     - The translation is the words after a space that follows letters of one script (Latin,
       Greek, Cyrillic, or CJK: kana, Han, Hangul), when every letter after that space is in
       another script.
     - Only one of the two titles loses it, so two titles that share just their first word
       stay apart.
     - More words in the same script ("SPEEDCORE DANDY" vs "SPEEDCORE DANDY XXX") or a number
       ("SPF INFINI 2", "アニメガバイト 2") are compared in full, as different releases.
   - A subtitle after a colon (`: ` or a fullwidth `：`) added to one of the two titles also
     costs at most 0.1: "DupleX EP: 204 vs SHARPNEL" vs "DupleX EP".
     - As with translations, only one title loses it, so "Op. 5: I. Allegro" and "Op. 5: II.
       Adagio" stay apart.
     - A colon inside a word ("Re:Mix") doesn't count.
     - Nor does a colon before something that only numbers a part (digits, roman numerals up
       to four letters, "One" to "Ten", with words such as "Vol.", "Part", "Disc",
       "Chapter"). "Title" vs "Title: Part 2" are compared in full, as different releases.
     - ` - Subtitle` is already a suffix (above).
   - Album titles only (track titles that differ in these ways are more often different
     tracks):
     - The same words in another order cost 0.05: "SHOCKWAVE GREATEST HITS 1994-2002
       MEGAMIX!!!" vs "SHOCKWAVE GREATEST HITS MEGAMIX 1994-2002". This needs at least two
       words, all of them, each as often, with any numbers in the same order. So "SPEEDCORE
       DANDY" vs "SPEEDCORE DANDY XXX" and "Part 1 Chapter 2" vs "Part 2 Chapter 1" are
       compared in full.
     - A title quoted in full in the other (`「…」`, `『…』`, `“…”` or `"…"`) costs 0.1 plus
       the distance to the quoted part: "未収録作品集 「別名：おにぎりトラックス」1998-2001" vs
       "別名おにぎりトラックス".
   - DJ-mix markers at the end of a title are ignored, since the release's track titles leave
     them out: "(Mixed)", "[Mixed]", " - Mixed", "(Mixed Version)", "(Mixed Edit)", "(Mix
     Cut)" and "(Original Mix)". They're also ignored as the last part of a bracket ("(Paul
     Copping Remix - Mixed)" is "(Paul Copping Remix)"). A title that's only "Mixed" is kept.
   - Digit 0 and letter O count as the same character ("H20" is "H2O").
   - Per track: title 3, length 2 (no penalty within ±10 s, full penalty from ±30 s), track
     number 1 (per disc or straight through), artist 1.
   - Per release: album title 5, album artist 5, mean track distance 6, fraction of release
     tracks with no file 10, fraction of files with no track 6, year 0.5.
     - The album title and artist are whichever fit the release best, out of the item's own
       and the [extended search](#extended-search)'s keys. So an album artist tag such as
       "SHARPNELSOUND (m1dy)" doesn't cost a release by "m1dy" anything.
     - This needs no requests. It's done for every release candidate before the confirm
       step, whether the extended search runs or not.
   - An artist (album artist, track artist, recording artist) is compared with the credit as
     shown, with the credit in its artists' own names, and with the credit in their sort
     names. A person's sort name is also turned around ("Smith, John" as "John Smith").
     - The closest one counts: "Niwapnel" fits a release credited to "ニワプネル", whose sort
       name is "Niwapnel".
     - Each form is tried for the whole credit, so one artist out of two isn't a match for
       both.
   - An album artist tag that names one of the release's labels has no album artist part,
     like a compilation. It has to be a close match (distance under 0.1), so "SHARPNELSOUND
     (m1dy)" doesn't count. Doujin labels' releases are often tagged "SHARPNELSOUND" whoever
     made them. The exception is when the release's artist fits the tag even better. Any
     other label still counts against it.
   - A **compilation** is a release whose credit includes Various Artists (MusicBrainz's
     special artist, or "Various Artists" or "V.A." by name), alone or beside others ("DJ
     SHARPNEL & V.A."). It has no album artist part. Its album artist tag usually names the
     label, the circle or the DJ, and the track artists are scored anyway.
   - **video tracks** are left out when the release has audio tracks too. Video tracks are:
     - tracks on a video medium: any format with "Video" in it (such as "DVD-Video", "DVD-R
       Video" or the DVD-Video side of a DualDisc), "Blu-ray", "Blu-ray-R", "HD-DVD", "VCD",
       "SVCD", "UMD", LaserDiscs, "VHS", "VHD", "Betamax", "CED", and a plain "DVD" when any
       of its recordings is flagged as a video
     - any recording flagged as a video.

     No file is paired with a video track, and one with no file isn't missing, so a CD +
     DVD-Video release matches the CD's files fully. Otherwise they stay in the release. The
     review lists them, a file can be made one with Use as Track N, and the tags keep
     MusicBrainz's numbering, so the CD's files are `DISCNUMBER` 1/2 and named `1-01 Title`.
     A release that's all video (the sound of a concert DVD) is scored as usual.
   - Per recording: title 3, artist 3, length 2.
   - Ties go to the earliest original release.
5. **Assign** files to release tracks with the Hungarian algorithm over the per-track
   distance matrix. A pair with a track distance over 0.6 is dropped, so a wrong title is
   never written just to fill a slot.
6. **Confirm**: confidence = 1 - distance. The best candidate is accepted automatically if
   its confidence is at least the threshold (default 0.9) **and** it's complete: no missing
   tracks and no leftover files (a recording candidate is always complete). Everything else
   waits for the user in the review dialog. There they can pick another candidate, search
   again with their own terms, keep the existing tags, or skip the item.
   - Results of a manual search are never accepted automatically, and neither are joined disc
     folders or other discs (see Group).
   - They can also **search more widely**: the extended search on request, for an album or a
     single. Its results join the candidates, and aren't accepted automatically either.

   **Use as track N** fixes an assignment the tagger got wrong.
   - The review's track list shows the missing tracks, the files no track took, and the files
     of other items from the same folder (`mc_import_related`, e.g. a file whose album tag
     put it in a separate item).
   - Clicking a missing track (or pressing Enter on it; on Linux, its Choose File… button)
     lists the files it can take. Choosing one (`mc_import_assign`) makes the file that track
     and writes the item with that match. So does selecting the missing track and a file
     together and choosing Use as Track N from the right-click menu.
   - This works whether the item was in review or already imported. If it was imported, its
     files are re-tagged and moved in place, and its library rows are updated.
   - A file taken from another item leaves that item. If that item had imported it, its
     library row moves to this album. An item left with no files is skipped as
     "Moved to another album".
   - The match's confidence (and its missing tracks and leftover files) is then worked out
     again from the files and tracks as they're paired now, with no new request. So a match
     that was low only because that track was missing goes up. Every pair counts, even one
     the tagger would have dropped as too far apart.
   - The other matches of both items are rescored too, but keep their order.

   **Remove from import** takes a file out of an item in review (`mc_import_remove`).
   - A file no track took, or one from another item of the folder, has a delete button on its
     row. A file on a release track has Remove from Import in its right-click menu (on
     Android, in the row's menu).
   - The file isn't tagged or imported. It stays where it is in the source, untouched.
   - It's no longer one of the item's files, so it doesn't count anywhere. A removed extra
     file no longer stops the match from being complete. A track whose file was removed is
     missing like any other, so then the match isn't complete either.
   - The matches are rescored from the pairs that are left (no new request), and the item
     stays in review whatever its confidence is now.
   - Nothing pulls a removed file back in. Use as Track N doesn't offer it, a new search
     doesn't score it, and keeping the existing tags doesn't import it.
   - An item left with no files is skipped as "Removed from the import".
   - The removed files are listed under the item with **Put back** (`mc_import_put_back`).
     This returns the file to the item's files and pairs it again with the track it had, if
     no other file has taken that track since. Otherwise it's paired with the free track that
     suits it best, if one is close enough. A skipped item comes back to review.
   - Neither works once the item is imported.
7. **Compare** with the library (`dupes.c`, see [Duplicates](#duplicates)): skip what the
   library already has, or swap in a better copy of the release.
8. **Write** (see [Library folder](#library-folder)): copy the files into the library folder
   (or move them, if they're already inside it) and write the tags to the copies. Make sure
   the album folder has a cover (see [Cover art](#cover-art)), then write the library rows
   in one transaction (`import_db.c`). If any step fails, the files are put back. Files
   that weren't matched to a track aren't imported.

## Extended search

Albums often have a label or circle in the album artist tag instead of the artist. For
example, a doujin label's releases may be tagged "SHARPNELSOUND" or "SHARPNELSOUND (DJ
CHUCKY)" when MusicBrainz credits "DJ CHUCKY", "DJ SHARPNEL" or "Various Artists". The normal
search (album title with that album artist) then finds nothing, or only the wrong releases.
The real artist and the release usually show up somewhere else, and `search_keys.c` gathers
them:

- **catalogue numbers**: `CATALOGNUMBER` (or `CATALOGID`, `CATALOG`, `LABELNO`) tags, and
  `[bracketed]` groups in the album tag ("BREAK IT OLDSCHOOL [SRPC-L007]") and the folder
  name, split at `/`.
  - A catalogue number has an ASCII letter and at least three digits, so dates
    (`[2002.08.11]`), events (`(C89)`) and notes like `[24bit]` or `[FLAC]` don't count.
  - `SRPC-0008LTD` also tries `SRPC-0008`.
- **artists**:
  - the part in parentheses of a `Label (Artist) - Title [CAT-001] (Event)` folder name or
    album artist tag, without "and V.A."
  - the artist that at least half the files agree on. This comes from the track artist tags,
    the file names (`03. Artist — Title.flac`, with `-`, `–` or `—`, taking the side that
    isn't the title tag) or, where the artist tag just repeats the album artist, the titles
    (`Artist - Title`).

  "feat." parts are dropped.
- **labels**: `LABEL` (or `ORGANIZATION`, `PUBLISHER`) tags, and the name where the artist
  would be (the folder name's prefix, the album artist tag). That name only counts when it
  has an artist in parentheses, or differs from the artist the tracks agree on, or the
  tracks are a compilation.
- **titles**: the album tag, and the folder name's title when it's different (after ` - `,
  up to the first `[group]`, without a trailing `(2004)`).
  - For a single, it's the album tag (its title is the track's).
  - Without an album tag, the folder name only counts when it looks like a release
    (`Prefix - Title`, or with a catalogue number), since a loose track's folder is often
    just where it was saved.
- a **compilation**: "V.A." in the parentheses, or no artist on at least half the files.

These are the queries, most specific first. Each one is skipped when its keys are missing:

1. `catno:(A OR B)`, looking up at most 3 of the hits
2. title with the first artist
3. title with the labels (`label:`)
4. the title alone (only if the normal search had an artist)
5. title with the second artist
6. the folder's title alone

Queries 2 to 6 look up at most 2 hits each.

The budget is 4 searches and 6 release lookups on top of the normal search (about 10 s at
1 request/second), and it stops early once a candidate would be accepted. Hits that are
already candidates aren't looked up again. Disc folders that the title and album artist
don't find are searched for with the same keys before they're joined (see Group), with at
most 3 more searches and 4 more lookups for the whole set. If they're then matched one by
one, each one may still get its own extended search.

**Scoring** works as normal: tracks, lengths, track numbers, and missing and extra files all
count as before. The only difference is that the album title and album artist parts use
whichever fits the release best, out of the item's own, the keys' titles and artists, and
"Various Artists" for a compilation.

- Every release candidate is scored this way, including the normal search's, whether or not
  the extended search runs. The keys are gathered from the files and the folder name, so it
  costs no requests.
- A release the normal search found with a label in the album artist tag is accepted if it's
  otherwise complete and right.
- "V.A." as a release artist counts as Various Artists.

**When**: it runs automatically for an album or a one-file item whose best candidate, scored
as above, is below **75%** confidence, or that has no candidates.

- The threshold comes from the scoring. The album artist is 5 of the release distance's 32.5
  weight units. So the right release with a wrong album artist tag that no key corrects
  loses at most about 15 points, and still scores around 85-90%. Below 75%, the title, a
  quarter of the tracks or more, or both disagree too, which means it's the wrong release.
- Results are treated like the normal search's: the same threshold and completeness rules
  decide whether the best one is accepted.
- Above 75% but not accepted, the review offers **Search more widely**, which runs the same
  search on request. Its results join the candidates and are never accepted automatically.
- Singles have it too, since a one-file item may be a whole release. Its release candidates
  are scored like an album's, so one is only accepted if it has that one track. (A
  recording candidate for a single isn't held to that.)
- One-file items are matched after the albums, so their extended searches don't hold the
  albums up.
- After a manual search, the button is offered again.

**Offline**: it doesn't start while MusicBrainz can't be reached (the normal search failed
with a network error, or the session is offline). A network error ends it early, and the
item then keeps what it has, or waits for MusicBrainz like any other item. The frontends
disable the button while offline.

## Library folder

The library is one folder the user chooses (by default, their Music folder). It holds the
database as `library.db` at the top, with the music beneath it (`organize.c`):

```
Radiohead/OK Computer (1997)/01 Airbag.flac
Radiohead/OK Computer (1997)/cover.jpg
Some Band/Double Album (2004)/2-07 Title.flac      (disc-track, when there are several discs)
OutKast/Hey Ya! (2003)/01 Hey Ya!.flac             (a single: its release, of one track)
OutKast/Demo.flac                                  (a track with no album)
library.db
```

- **Originals are never modified.** Files from outside the library are copied in, and the
  copy is tagged. Files already inside the library are moved (renamed) to their new place.
  Folders left empty, or holding only a cover, are removed.
- The year is left out of the folder name when it isn't known. As-is imports use the
  existing tags, then the file name for the title.
- Names are made safe for Windows everywhere (it's the strictest of our platforms), so a
  library can move between systems:
  - `<>:"/\|?*` and control characters become `_`
  - leading dots and trailing dots or spaces are dropped
  - device names such as `CON` get a `_`
  - each part is cut to about 100 bytes.
- If the destination is taken, the same release track imported again from outside the
  library replaces its old copy. Anything else gets ` (2)`, ` (3)`, ... appended. (A release
  that's already in the library doesn't get this far: see [Duplicates](#duplicates).)
- A track with no album goes in its artist's folder, `<artist>/<title>.<ext>`.
- Nothing treats an album called "&lt;artist&gt; Singles" specially. Its files are scanned
  like any other album's.
- `files.path` is stored relative to the library folder, with `/`, so the folder can be moved
  or mounted under another drive letter.
- The library folder is kept in per-device settings (`settings.json`, see
  `core/include/kithcore/settings.h`), in `%LOCALAPPDATA%\kithsong` on Windows and
  `~/.config/kithsong` on Linux. A build run from inside a checkout uses `kithsong-debug`
  there instead, and a separate library in the temp folder
  (`core/include/kithcore/source_run.h`).
- The sync client can corrupt a database inside a OneDrive/Dropbox-style synced folder. The
  Windows frontend warns you when the chosen folder is under OneDrive.

## Duplicates

Before an item from outside the library folder is written, it's compared with the library.
The full rules are in `core/include/kithcore/tagger.h` ("Duplicates"). In short:

- **Same release**: an album with the same MusicBrainz release id, or else the same release
  group id, or else the same album artist and title with at least half of the incoming
  tracks in its track list.
  - A single counts as an album here.
  - For a track with no album, it's the same recording (or else the same title) among the
    artist's tracks with no album, and each one is judged separately.
  - Tracks pair up by release track id, then recording id, then disc and track number with
    the title, then title.
- **Quality** of a file: lossless beats lossy. After that it's bits per sample and sample
  rate for lossless, or bitrate weighted by codec for lossy (MP3 1.0, Opus 1.6). Less than
  10% apart counts as equal.
- **Better as a whole**: the incoming release has every track the library has a file for,
  none of them worse, and more than half better. Then its files replace the library's in
  place:
  - the album and tracks keep their uids (so playlists, album groups, plays, recents and
    sync profiles keep them)
  - the new file takes the old one's name, with its own extension
  - the old file goes to the Recycle Bin or trash.

  The track's `file_rev` changes, which tells other devices to replace their copies (see
  "Files" in sync-protocol.md).
- **A swap re-tags the new file to match the library**: it's a better copy of the track the
  library has, so it gets the library's tags.
  - That's the library's title, artist, track and disc numbers, year and MusicBrainz ids,
    and the album's title, album artist and ids. Ids the library doesn't have are removed.
  - It doesn't get the tags of its own match, which might be another release of the group
    or, if it was found by name, no release at all.
  - A file attached to a track that has no file on this device is tagged and named the same
    way.
  - A track with no album gets no album tags, ids or numbers.
- **Unless it's the whole release**: that's when
  - the album was found by release or release group id (not by name, and not tracks with no
    album)
  - every one of its tracks pairs with an incoming file
  - every file of it on this device is swapped.

  Then the import's match wins. The new files keep its tags and names, and the album and its
  tracks take its title, artists, numbers, years and ids, keeping their uids. This is
  typically a remaster or hi-res release of the same release group replacing the whole CD
  rip. The old folder is removed if that empties it, and its cover moves to the new one
  unless the import brings its own. An item imported with its existing tags has no match to
  win, so it's re-tagged as above.
- Otherwise the tracks the library has are **skipped**, with no ` (2)` copies. Tracks it
  doesn't have are added to its album either way. Short of a whole release, a matched track
  takes the album's title, album artist, year and ids rather than its match's, which can be
  another release of the group. It has no release track id, as its own belongs to that
  release.
- **The same file again** is skipped.
  - Every file new to the library records the SHA-256 of the bytes it was imported from,
    before its copy was tagged (`files.source_sha256`; sync passes it on with an original).
  - A merged library's files bring their hashes with them, or else record the bytes they
    arrive as.
  - An incoming file is already in the library if its bytes hash to a recorded source hash,
    or to a library file's `sha256` (or, without one, to the bytes of a library file of the
    same size).
  - Files imported before this have no source hash, and are only recognised by their current
    bytes.
- The item says what happened: replaced with higher quality, added the missing tracks, or
  already in the library (the item is then skipped).

## Singles

A single is filed as what it is on MusicBrainz: a track of a release, like any album track.

- An item is a single if it's a loose track (see Group). It's matched to a recording, and
  the recording is credited to one of its releases. Official beats unofficial, a single
  beats an album or EP, compilations come last, and ties go to the earliest date.
- Once it's accepted, that release is looked up in full and the file is written as its track
  there: the release's `ALBUM`, `ALBUMARTIST`, `MUSICBRAINZ_ALBUMID`,
  `MUSICBRAINZ_RELEASEGROUPID`, track and disc numbers and `DATE`, under
  `<album artist>/<album> (<year>)/`. A one-track release is an album of one track. One
  track of an album imported by itself is that album's track, out of its total.
- If the release can't be looked up, the search's release title and ids and the track's
  number are written. The recording's first credited artist is the album artist, and there's
  no track total. A recording on no release gets no album.
- In the library a single is an ordinary album (`albums.kind` is unused, see schema.md).
  An album with **one track in the library** is shown as a single. The album listings and
  search leave it out, and the artist's page lists it with the artist's tracks that have
  no album (`mc_library_list_artist_singles`). Importing more of its release makes it an
  album again.
- A loose track imported as-is with no album tag gets no album: `album_uid` is NULL and the
  file goes in `<artist>/<title>.<ext>`. There are two exceptions, where it gets an album
  named after its title, under `<artist>/<title>/`:
  - It's long enough to be a continuous mix (30 minutes, see
    [Album flags](schema.md#album-flags)), so the flag can be on. A mix with no match on
    MusicBrainz is usually a file like this.
  - It has a picture embedded, so the picture can be its cover (see
    [Cover art](#cover-art)). Art belongs to an album, and a track with no album has none.

  A recording on no release that's that long or embeds a picture gets an album the same way.
  Any other gets no album.
- A single of that length, whether it has an album already or just got one, is a mix unless
  the user says otherwise. That makes it an album to the listings and search, not a single.
- Kithsong doesn't make "&lt;artist&gt; Singles" albums. One that's already in the library
  stays as it is, as an album of that title, until the music is imported again.

## Artist credits

A track or album credited to "A feat. B", "A, B & C" or "A x B" belongs to each of A, B and
C. The credit as written is what's shown, and what goes in `ARTIST` / `ALBUMARTIST`. The
import passes the credit's artists on to the library in order, each with the join phrase
that follows it (" feat. ", " & ") (`mc_db_album.credit`, `mc_db_track.credit`). Folders
stay under the whole album credit: `Jay-Z & Kanye West/Watch the Throne (2011)/`.

Where the artists come from:

1. **MusicBrainz**, for a matched import: the `artist-credit` array of the release and of
   each track (of the recording, for a recording found by itself), unchanged. That's each
   artist's name as credited, id, sort name and join phrase. It's never split again.
   - The files get `ARTISTS` and `ALBUMARTISTS` (one value per artist, by name as credited)
     and every id in `MUSICBRAINZ_ARTISTID` and `MUSICBRAINZ_ALBUMARTISTID`.
   - A track with no album has its album lists removed.
   - A recording whose release can't be looked up has its first credited artist as the album
     artist, so the album lists hold just that one.
2. **Multi-valued tags**, for an as-is import: `ARTISTS` with `MUSICBRAINZ_ARTISTID` for the
   track, and `ALBUMARTISTS` with `MUSICBRAINZ_ALBUMARTISTID` for the album (`ARTISTS` when
   the album artist is the track artist).
   - The ids only go with the names when there are as many of each.
   - The join phrases are what's between the names in the `ARTIST` string, when they're all
     there in order (ignoring case). Otherwise they're "; ".
   - A single name that is the credit itself is just the credit.
3. **The credit string** otherwise, split by `mc_credit_split_text` (`credit.c`):
   - "feat."-style separators always split: ` feat. `, ` feat `, ` ft. `, ` ft `,
     ` featuring `, and a closing `(feat. B)` or `[feat. B]` (with any of those words).
   - Ambiguous separators only split where every resulting part is an artist the library
     knows, and the whole isn't one: ` & `, `, `, ` and `, ` x `, ` vs. `, ` vs `, ` with `,
     ` / `, `; `, and the serial-comma forms `, & ` and `, and `. The longest known name is
     taken first, so "Earth, Wind & Fire & The Emotions" keeps "Earth, Wind & Fire" whole
     when it's known. When any part isn't known, the whole stretch stays one artist
     ("Tyler, the Creator").
   - An artist is known when the library has an `artists` row of that name (ignoring case)
     that has a MusicBrainz id, or whose name has no separator at all. Older versions made
     rows like "A & B" from whole credits. Those don't count, but "Simon & Garfunkel" with
     its MusicBrainz id does, and stays whole.
   - Separators match ignoring case, and need spaces around them ("AC/DC" and "Aftermath"
     stay whole). Parts are trimmed and never empty: a credit that would give an empty part
     ("A & & B", "A feat.") is one artist.

From tags or the string, a credit of one artist under their own name is passed as just the
credit (`credit_count` 0), with the file's `MUSICBRAINZ_ARTISTID`. The whole credit's
MusicBrainz id is only set when one artist is credited.

Credits imported before library v13 (which added artist credits), such as "Jay-Z feat.
Rihanna" as one artist, stay as they were. Nothing splits them automatically. Importing the
album again, or editing the credit by hand (below), splits it as above.

## Hand edits

This covers the edit dialog's album, artist and track tags, and matching an album to another
release (`core/include/kithcore/edit.h`, `core/src/library/edit.c`). The rows are the source
of truth. An edit changes them, logged like any change, and they're what every device shows.
Files only follow on this device.

- **Credits.** A changed credit is split the way an import splits a string (see
  [Artist credits](#artist-credits)). Artists are found or made as in an import, and the
  credit rows, `artist_uid` and `artist_credit` are written the same way.
  - A credit that wasn't changed isn't touched, so a credit from MusicBrainz keeps its ids
    however often the dialog is saved.
  - Tracks credited exactly like their album follow its new credit, and tracks with the
    album's year follow its year.
  - Clearing a track's artist makes it use the album's again, with no separate artist.
  - An artist that nothing names any more is deleted and logged, the same as any delete, so
    fixing a typo leaves nothing behind.
- **Renaming an artist.** The row changes, and so do credits that spell out the old name
  ("Ann & Bob", when the artists are Ann and Bob). A credit that credits them some other way
  stays. Giving an artist a name another artist already has doesn't merge them by itself.
  Artists are told apart by uid, and only merged by `mb_artistid`, as sync does, since two
  artists without an id may well be different people. So the form asks.
- **Merging two artists.** Renaming an artist to a name another one has
  (`mc_artist_same_name()`, any case) asks whether to merge them. Yes calls
  `mc_artist_merge()`: everything that credited either of them now credits one, under the
  other's name and sort name. Files and tags follow as for a rename.
  - It's a hand edit, so it's all or nothing. It syncs as rows: credits change, and the
    artist that goes is deleted.
  - The row that stays is the one with an `mb_artistid`, if only one has. An id is never
    copied across. Two rows that have shared one are one for good, and deleting either
    would then delete both (see [Duplicates](sync-protocol.md#duplicates)).
  - A sync profile that chose the artist that went loses that item.
- **File tags.** Each original on this device whose tags the edit changes is rewritten with
  `mc_tags_write()`. Only the tags that changed are written, so anything the rows don't know
  about stays.
  - A transcode keeps its tags, and nothing outside the library folder is touched.
  - The `files` row takes the new size, mtime and `sha256` (if it had one; a file with none
    is left for a deep check). `source_sha256` stays, because it's what was imported.
  - `file_rev` doesn't change, so no device fetches the file again, and peers' copies keep
    their old tags. That's safe for sync because `files.sha256` is only ever compared on the
    device that recorded it:
    - a sender hashes what it sends
    - `HAVE_RESP.verified` only says that a hash is recorded
    - a deep check compares a file with its own row
    - an archive's status counts files, not hashes.
  - The transcode cache is keyed by the file's uid, which stays, and the audio it holds is
    still right.
- **A file that doesn't match.** A file with a recorded hash is checked against it first.
  If its bytes don't match, it's left as it is. Otherwise it would get a new hash over bad
  bytes, and the deep check would never report it. The same goes for a file whose size or
  mtime differs from its row, or that's missing or read-only. None of these stops the edit:
  the rows change, and the file keeps its place and tags.
- **Filing.** A file moves when the edit changes where `organize.c` files it. It moves to
  where an import would put it, with the same ` (2)` when that's taken, on disk or in
  `files`.
  - A file this edit moves away doesn't count, so tracks can swap names.
  - A file filed somewhere else stays there through edits that don't change its place.
  - Transcodes move too.
  - The cover is copied to the album's new folder, unless that folder already has one. Then
    that's the album's cover now and `art_hash` follows it, unless the user chose the
    album's cover.
  - Folders left empty or holding only a cover are removed, as usual.
  - On Windows, a folder whose name only changes in case is renamed to match.
  - `files` doesn't sync, so only the device that made the edit moves anything.
  - A disc number past the album's last raises `disc_total`, which renames all its files.
    Any disc above 1 puts the album's tracks with no disc number on disc 1, so they're named
    and tagged that way straight away. Taking the last track off the album's last disc
    lowers `disc_total` to the last disc with a track, which renames them again.
- **Stopping one.** `mc_edit_set_watch()` reports how far along an edit or a release search
  is. It can stop it (`MC_ERR_CANCELLED`) while copies are being made, before anything has
  changed. The renames after that are quick, and can't be stopped.
- **All or nothing.** A failed copy, tag write or rename leaves rows and files exactly as
  they were. Neither goes first.
  - In a write transaction, the rows change and the effect on each file is worked out by
    comparing tags and paths before and after. Then the transaction is rolled back.
  - With no lock held, a tagged copy of each file is made beside it. Everything likely to
    fail fails here.
  - In a second transaction, the rows change again and the plan is checked. Every file is
    renamed to a backup, every copy is moved into place, and the transaction is committed.
    The backups are removed last.
  - A failed rename puts everything back.
  - `meta.edit_swap` lists what's under way, and changes from `undo` to `done` in the
    commit. So a crash is put right either way the next time a frontend opens the library.
  - If something else changed the rows or files in the meantime, the edit plans again.
- **An open file.** The player opens files sharing delete, on Windows too. So the track
  that's playing is renamed like any other, and keeps playing the old bytes. On Windows,
  sync and other programs open files without sharing delete, so those can't be renamed.
  - Each file is checked as the plan is made, before any copying, and a rename catches one
    opened since.
  - The edit then returns `MC_ERR_IN_USE` with nothing changed. The frontend tells the user,
    and the edit can be tried again.
  - Only the files the edit touches count.
  - The player's queue still has the old path, so after an edit every frontend gives it the
    queue's new paths (`mc_player_queue_set_path()`) and shows the new titles. An edit that
    moves no file (a new cover or title, say) reaches the player without restarting the
    track.
- **Another release.** `mc_release_search()` searches the way an import does, sharing
  MusicBrainz's rate limit. It looks up the best 5 and scores them against the album's
  tracks with `match.c`.
  - `mc_album_tags_set()` with a new `mb_albumid` looks that release up and pairs the tracks
    the same way, dropping pairs over 0.6 apart.
  - The album takes the release's title, credit, years, disc total and ids, except for what
    the caller changed itself. Paired tracks take its title, credit, number, disc, year and
    ids, and the others keep theirs. Then tags and filing happen as above.
  - A cover is only fetched if the album has none and has a file here to put it beside.
    Failing to get one doesn't fail the edit.
  - `""` unlinks: `mb_albumid`, `mb_releasegroupid` and the tracks' `mb_albumid` and
    `mb_releasetrackid` are removed, and the rest stays.
- **A release that's taken.** Sync treats two rows that have ever shared an id as one, for
  good, and a delete of one as a delete of both (see "Duplicates" in sync-protocol.md).
  - So a release that another album has, or ever had, is refused with `MC_ERR_EXISTS`,
    including a deleted album's.
  - A track isn't paired with a release track that another track has ever had. It keeps its
    tags.
  - The same rule means an album stays tied to every release it was linked to. An album
    imported later with a release this album once held merges into it.
- **A chosen cover.** `mc_album_art_set()` makes an image the album's cover: see
  [A cover the user chose](#a-cover-the-user-chose).

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

Writing an empty string through `mc_tags_write()` removes that tag, and so does a negative
year, track or disc.

Multi-valued properties are '\n'-separated lists in `mc_tags` (`artists`, `album_artists`,
`mb_artistids`, `mb_albumartistids`). `mb_artistid` and `mb_albumartistid` read as the first
value. In ID3v2 (MP3, WAV), `ARTISTS` and the artist ids are `TXXX` frames ("ARTISTS",
"MUSICBRAINZ ARTIST ID", as TagLib names them), with one value per string. In FLAC and Opus,
there's one Vorbis comment per value.

## Cover art

Each album folder gets a `cover.jpg` (or `cover.png`). `albums.art_hash` holds its SHA-256,
so devices can tell whether they have the same image. That file is the album's art on every
device. Pictures embedded in audio files are never shown, only copied out on import (the
files themselves are left unchanged). The rule is **local art first**, with a download only
where the local art is missing or low resolution:

1. A cover that's already in the album folder is kept as it is, whatever its size. An album
   that's already in the library (tracks added to it, files moved or re-tagged in place)
   keeps its cover, and nothing is downloaded for it. The same goes for a cover brought from
   inside the library (a whole release moving to a new folder takes its old cover with it,
   see [Duplicates](#duplicates)).
2. Otherwise the **local cover** is the first of:
   - a cover in the folder the files came from (`cover`, `folder` or `front`, `.jpg`/`.jpeg`/
     `.png`, any case, in that order). For files in a disc folder (`Album/CD1`, see Group),
     the cover in the album folder above it comes first, as the whole album's cover. Then
     comes the disc folder's.
   - a picture embedded in one of the imported files (the front cover, or else the first
     one; JPEG or PNG up to 16 MB). It comes from the first file that has one, in disc and
     track order. So where only some tracks embed art, the first of those provides it. Where
     each disc embeds its own, disc 1's is the album's.

   For a single (a loose track, see Group), the order is reversed: the picture embedded in
   it comes first, then the folder's cover. The picture belongs to that file, whereas a
   folder of loose files (Downloads, say) may have a cover that belongs to any of them, or
   to none.
3. A local cover whose shorter side is at least **500 px** (`MC_COVER_LOW_RES` in
   `coverart.h`) is used, with no request. Its size comes from the JPEG or PNG header
   (`stbi_info`), and nothing is decoded. A header that can't be read counts as big enough,
   so the cover is kept as before rather than replaced on a guess.
4. If the local cover is below 500 px, or there's none, one download is tried. That needs
   the item to be matched, or imported as-is with a `MUSICBRAINZ_ALBUMID` or
   `MUSICBRAINZ_RELEASEGROUPID` tag, and downloading to be on
   (`mc_import_options.fetch_art`). It's the first of:
   1. Cover Art Archive `release/<id>/front-1200`,
   2. Cover Art Archive `release-group/<id>/front-1200`,
   3. iTunes Search (`entity=album`), for a match only (as-is tags could say anything). A
      result is only used if its album and artist names are both close (combined distance
      below 0.25). The 100 px artwork URL is rewritten to 1200 px.

   With no local cover, the download is the cover. Otherwise it only replaces the local one
   if its shorter side is bigger. If it's the same size or smaller, or its size can't be
   read, the local cover stays. The downloaded image is written once, as `cover.jpg` or
   `cover.png`, and the source folder and files aren't touched.

Why 500: the biggest views draw a cover at about 300-350 logical pixels. Those are the
desktop album grids' tiles (160 px on Linux; at least 150 px on Windows, stretched to fill
the row, so up to about 320) and the phone's full player (the screen's width less 48 dp, at
most 480 dp). At 2-3x that's 600-1100 device pixels, so below 500 a cover is stretched more
than twice there and looks soft. Covers of 500 and 600 px pass, which is what most stores
embedded for years, so most local art is kept without a request. Downloads are at most
1200 px anyway.

### A cover the user chose

`mc_album_art_set()` and `mc_album_art_set_file()` (`kithcore/edit.h`) take the image as bytes or
a path. It must be a JPEG or PNG of up to 16 MB and 8192 px a side, and it must decode in
full. `mc_album_art_check()` checks this without changing anything. The image is kept byte
for byte.

- **Where it goes.** It's written as `cover.jpg` or `cover.png`, going by its bytes, in
  every folder on this device that holds a file of the album, inside the library folder.
  - An album with no file on this device can't take one (`MC_ERR_NOT_FOUND`), and
    `mc_album_art_get()` says so before a form offers it.
  - Any other `cover.*` in those folders is removed too, or it could still be found first.
    `folder.*` and `front.*` stay, because they rank below it.
  - What's replaced goes to the Recycle Bin or trash, the one in its place as
    `replaced cover.jpg`. It's only deleted where there's no bin, as on Android.
  - `albums.art_hash` and `albums.art_chosen` take its SHA-256, logged like any change.
- **Going back.** `mc_album_art_reset()` undoes it. `art_chosen` is cleared, and the album
  gets what an import would give it, by the same steps: the picture embedded in its files or
  a downloaded one, whichever is bigger. If there's none to be had, it has no cover (a
  `folder.*` or `front.*` then shows), and if the network is down nothing changes. Other
  devices clear the choice but keep the cover they have, as a folder with a cover does,
  until something replaces it.
- **Files' tags.** Nothing is embedded, as with all art here, so no audio file is touched
  and nothing else about the tracks changes.
- **All or nothing.** It follows the steps of any hand edit. The copies are written beside
  the old covers with no lock held. Then, in one transaction, the old covers are moved to
  backups, the copies are moved into place and the row changes, with `meta.edit_swap` to put
  things right after a crash. `MC_ERR_IN_USE` means an old cover was open, which on Windows
  means sync is sending it. If an album's folders change in the meantime, it starts again.
- **Other devices.** They get it as a change of art: see "Covers" in
  [sync-protocol.md](sync-protocol.md).
- **What leaves it alone.** `albums.art_chosen` is the reason the choice is kept separate
  from `art_hash`, and nothing but another choice changes it.
  - An import never replaces it or downloads a cover for the album. `upsert_album()` keeps
    `art_hash`, and `ensure_cover()` gives the folder the import writes to the chosen image
    (from another of the album's folders if need be). It doesn't use one from the source
    folder, a picture embedded in a file, or a download. That includes a whole release
    swapped for a better one, which would otherwise bring its own cover.
  - If this device doesn't have the image yet, the folder stays as it is, for sync to bring
    it.
  - Matching the album to another release only fetches a cover for an album with none.
  - An import that moves an album's last track to another album passes the choice on, as it
    does the album's flags.
  - Merging libraries keeps the choice of the kept library's album, or else the other's.

Only JPEG and PNG up to 16 MB are downloaded. A single gets its release's cover like any
album, and a track with no album gets none. Art isn't embedded in the files yet. Frontends
read the folder's cover through `mc_coverart_find()`, and the Linux and Android caches key
on its path and modification time. So a cover is picked up as soon as it's written (before
the album's rows are).

## Still to decide

How to set sort names for multi-artist credits, which genre source to use (MusicBrainz
tags/genres), whether the path format should be configurable, whether to embed cover art,
and how to detect duplicates beyond "same release track, same place".
