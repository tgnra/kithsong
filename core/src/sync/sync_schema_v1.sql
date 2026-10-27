-- musicapp sync database, version 1. See docs/sync-protocol.md.
--
-- sync.db lives in the per-device config dir, next to settings.json, and holds everything
-- about syncing: this device's keys, the group it belongs to, the group's devices and their
-- sync profiles, capabilities and status. The library holds no sync data; this database may
-- hold library data (the group's library_uid, each device's library node_uid).
--
-- Everything but `local` is replicated across the group with the same field-level change log
-- as the library. Rows are keyed by a uid that is identical on every device (a device_uid for
-- the per-device tables). There are no foreign keys: rows arrive from peers field by field,
-- in any order, and a reference may name a row this device hasn't received yet.
--
-- Change tracking triggers are not defined here; they are generated from the tracked
-- column list in sync_db.c so the two can't drift apart.

-- This device. Exactly one row, created on first open, never synced. The keys are raw
-- 32-byte X25519 keys (the private key is the clamped scalar).
CREATE TABLE local (
  id          INTEGER PRIMARY KEY CHECK (id = 1),
  device_uid  TEXT NOT NULL,
  private_key BLOB NOT NULL,
  public_key  BLOB NOT NULL,
  group_uid   TEXT              -- NULL while not in a group
);

-- The group, which syncs exactly one library (the library's meta.library_uid).
CREATE TABLE sync_group (
  uid         TEXT PRIMARY KEY,
  library_uid TEXT
) WITHOUT ROWID;

-- Group members, this device included. A removed device stays as a row with removed = 1, so
-- every member keeps refusing its key.
CREATE TABLE devices (
  uid        TEXT PRIMARY KEY,  -- the device's local.device_uid
  name       TEXT,
  kind       TEXT,              -- 'desktop', 'phone' or 'watch'
  public_key BLOB,
  node_uid   TEXT,              -- its library's meta.node_uid, so two devices never share one
  added_by   TEXT,              -- device uid of the member that paired it
  removed    INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;

-- What each device holds. Editable from any member; the target applies it when it syncs.
-- While archive = 1 the profile is fixed at mode 'all', format 'original'.
CREATE TABLE profiles (
  device_uid     TEXT PRIMARY KEY,
  archive        INTEGER NOT NULL DEFAULT 0,
  mode           TEXT NOT NULL DEFAULT 'all',      -- 'all' or 'selected' (profile_items)
  format         TEXT NOT NULL DEFAULT 'original', -- 'original' or 'opus'
  bitrate_kbps   INTEGER,                          -- for 'opus'
  budget_bytes   INTEGER,                          -- size budget; picks the best bitrate that fits
  budget_enabled INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;

-- A profile's selection while its mode is 'selected'. Adding is an insert and removing a
-- delete, so concurrent edits on two devices both apply. ref_uid is an album, artist, album
-- group or playlist uid in the library; it may name one that no longer exists, which is
-- skipped. Rows of a kind a device doesn't know are ignored.
CREATE TABLE profile_items (
  uid        TEXT PRIMARY KEY,
  device_uid TEXT NOT NULL,
  kind       TEXT NOT NULL,     -- 'album', 'artist', 'album_group' or 'playlist'
  ref_uid    TEXT NOT NULL
) WITHOUT ROWID;
CREATE INDEX profile_items_device ON profile_items (device_uid);

-- What each device can do, written only by that device; used to place transcodes.
CREATE TABLE capabilities (
  device_uid    TEXT PRIMARY KEY,
  cpu_score     INTEGER,
  free_bytes    INTEGER,
  on_power      INTEGER,
  can_transcode INTEGER,
  updated_ms    INTEGER
) WITHOUT ROWID;

-- How far each device's files match its profile, written only by that device after it
-- checks them.
CREATE TABLE sync_status (
  device_uid  TEXT PRIMARY KEY,
  verified_ms INTEGER,          -- when it last checked
  expected    INTEGER,          -- files its profile wants
  ok          INTEGER,
  missing     INTEGER,
  corrupt     INTEGER,
  bytes       INTEGER           -- size of the files it holds
) WITHOUT ROWID;

-- The group changelog: the same shape as the library's, but origin_uid is a device_uid.
CREATE TABLE changelog (
  seq        INTEGER PRIMARY KEY,
  hlc        INTEGER NOT NULL,
  origin_uid TEXT NOT NULL,     -- device that made the change
  entity     TEXT NOT NULL,     -- table name
  row_uid    TEXT NOT NULL,
  field      TEXT NOT NULL,     -- column name, or '_deleted'
  value                         -- untyped on purpose: no affinity, stored exactly as written
);
CREATE INDEX changelog_hlc ON changelog (hlc);
CREATE INDEX changelog_row ON changelog (entity, row_uid, field, hlc);
