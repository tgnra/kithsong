-- musicapp library schema, version 1. See docs/schema.md.
--
-- Every row is keyed by a UUIDv7 `uid` that is identical on every device, so rows can be
-- matched up during sync. MusicBrainz ids are stored when known but are never keys:
-- untagged files don't have them, and the same recording can appear on several releases.
--
-- Change tracking triggers are not defined here; they are generated from the tracked
-- column list in library.c so the two can't drift apart.

CREATE TABLE meta (
  key   TEXT PRIMARY KEY,
  value TEXT
) WITHOUT ROWID;

CREATE TABLE artists (
  uid         TEXT PRIMARY KEY,
  name        TEXT NOT NULL,
  sort_name   TEXT,
  mb_artistid TEXT
) WITHOUT ROWID;
CREATE INDEX artists_sort ON artists (sort_name COLLATE NOCASE, name COLLATE NOCASE);
CREATE INDEX artists_mb ON artists (mb_artistid) WHERE mb_artistid IS NOT NULL;

CREATE TABLE albums (
  uid               TEXT PRIMARY KEY,
  title             TEXT NOT NULL,
  artist_uid        TEXT REFERENCES artists (uid) ON DELETE SET NULL,
  year              INTEGER,
  original_year     INTEGER,
  disc_total        INTEGER,
  mb_albumid        TEXT,   -- MusicBrainz release
  mb_releasegroupid TEXT,
  art_hash          TEXT    -- content hash of the cover image in the art cache
) WITHOUT ROWID;
CREATE INDEX albums_artist ON albums (artist_uid);
CREATE INDEX albums_title ON albums (title COLLATE NOCASE);
CREATE INDEX albums_mb ON albums (mb_albumid) WHERE mb_albumid IS NOT NULL;

CREATE TABLE tracks (
  uid               TEXT PRIMARY KEY,
  album_uid         TEXT REFERENCES albums (uid) ON DELETE SET NULL,
  artist_uid        TEXT REFERENCES artists (uid) ON DELETE SET NULL,
  title             TEXT NOT NULL,
  track_no          INTEGER,
  disc_no           INTEGER,
  duration_ms       INTEGER,
  genre             TEXT,
  mb_trackid        TEXT,   -- MusicBrainz recording
  mb_releasetrackid TEXT
) WITHOUT ROWID;
CREATE INDEX tracks_album ON tracks (album_uid, disc_no, track_no);
CREATE INDEX tracks_artist ON tracks (artist_uid);
CREATE INDEX tracks_mb ON tracks (mb_trackid) WHERE mb_trackid IS NOT NULL;

-- Physical files on this device. A track can have several: the original plus lossy copies.
-- Files are device-local and not synced; sync transfers tracks, and each device records
-- what it has on disk.
CREATE TABLE files (
  uid             TEXT PRIMARY KEY,
  track_uid       TEXT NOT NULL REFERENCES tracks (uid) ON DELETE CASCADE,
  path            TEXT NOT NULL UNIQUE,
  format          TEXT NOT NULL,  -- 'flac', 'mp3', 'opus', ...
  bitrate_kbps    INTEGER,
  size_bytes      INTEGER,
  mtime_ms        INTEGER,
  -- NULL for an original. For a transcode received from a peer: the uid of the original
  -- file on that peer, so it is never imported again as a new track.
  source_file_uid TEXT,
  source_peer_uid TEXT
) WITHOUT ROWID;
CREATE INDEX files_track ON files (track_uid);

CREATE TABLE playlists (
  uid  TEXT PRIMARY KEY,
  name TEXT NOT NULL
) WITHOUT ROWID;

-- Ordered by `position`, a fractional index string (e.g. "a0", "a0V", "a1"). Inserting
-- between two entries only creates one new row, which is what lets concurrent edits merge.
CREATE TABLE playlist_entries (
  uid          TEXT PRIMARY KEY,
  playlist_uid TEXT NOT NULL REFERENCES playlists (uid) ON DELETE CASCADE,
  track_uid    TEXT NOT NULL REFERENCES tracks (uid) ON DELETE CASCADE,
  position     TEXT NOT NULL
) WITHOUT ROWID;
CREATE INDEX playlist_entries_order ON playlist_entries (playlist_uid, position, uid);

-- One row per field-level change, local or received. Sync sends peers every row with
-- hlc greater than the newest they have already seen from this device.
CREATE TABLE changelog (
  seq        INTEGER PRIMARY KEY,
  hlc        INTEGER NOT NULL,
  origin_uid TEXT NOT NULL,     -- node that made the change
  entity     TEXT NOT NULL,     -- table name
  row_uid    TEXT NOT NULL,
  field      TEXT NOT NULL,     -- column name, or '_deleted'
  value                         -- untyped on purpose: no affinity, stored exactly as written
);
CREATE INDEX changelog_hlc ON changelog (hlc);
CREATE INDEX changelog_row ON changelog (entity, row_uid, field, hlc);

CREATE TABLE peers (
  uid              TEXT PRIMARY KEY,
  name             TEXT NOT NULL,
  public_key       BLOB,
  paired_at_ms     INTEGER,
  last_seen_ms     INTEGER,
  last_synced_hlc  INTEGER NOT NULL DEFAULT 0, -- newest of their changes we've applied
  preferred_format TEXT                        -- what they want us to send, e.g. 'opus:128'
) WITHOUT ROWID;
