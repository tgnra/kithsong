#include "library/library_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "search/search_internal.h"
#include "util/platform.h"

extern const unsigned char mc_schema_v1_sql[];

/* Migration i takes the database from user_version i to i + 1. Append only. */
static const char *const k_migrations[] = {
  (const char *)mc_schema_v1_sql,
  /* v2: singles. Tracks that aren't part of an album live in one "<artist> Singles" album
   * per artist (kind = 'singles'), so each track keeps its own year and release id. */
  "ALTER TABLE albums ADD COLUMN kind TEXT NOT NULL DEFAULT 'album';"
  "ALTER TABLE tracks ADD COLUMN year INTEGER;"
  "ALTER TABLE tracks ADD COLUMN mb_albumid TEXT;"
  "CREATE INDEX albums_singles ON albums (artist_uid) WHERE kind = 'singles';",
  /* v3: date added and play counts. Tracks imported before this have no added_ms. Plays are
   * counted per device (one row per track and node, only ever written by that node), so
   * syncing never loses plays made on two devices; the total is the sum. */
  "ALTER TABLE tracks ADD COLUMN added_ms INTEGER;"
  "CREATE TABLE track_plays ("
  "  uid        TEXT PRIMARY KEY,"
  "  track_uid  TEXT NOT NULL REFERENCES tracks (uid) ON DELETE CASCADE,"
  "  node_uid   TEXT NOT NULL,"
  "  play_count INTEGER NOT NULL DEFAULT 0,"
  "  UNIQUE (track_uid, node_uid)"
  ") WITHOUT ROWID;",
  /* v4: sync data moves out to sync.db, so the unused peers table goes. files.sha256 is the
   * SHA-256 (hex) of the file's bytes, recorded when sync transfers or verifies it. The
   * library's own identity, meta.library_uid, is created in C at open (see load_meta_uid). */
  "DROP TABLE peers;"
  "ALTER TABLE files ADD COLUMN sha256 TEXT;",
  /* v5: album groups, playlists of albums. Entries are ordered by fractional index like
   * playlist_entries, so concurrent edits on two devices merge the same way. */
  "CREATE TABLE album_groups ("
  "  uid  TEXT PRIMARY KEY,"
  "  name TEXT NOT NULL"
  ") WITHOUT ROWID;"
  "CREATE TABLE album_group_entries ("
  "  uid       TEXT PRIMARY KEY,"
  "  group_uid TEXT NOT NULL REFERENCES album_groups (uid) ON DELETE CASCADE,"
  "  album_uid TEXT NOT NULL REFERENCES albums (uid) ON DELETE CASCADE,"
  "  position  TEXT NOT NULL"
  ") WITHOUT ROWID;"
  "CREATE INDEX album_group_entries_order ON album_group_entries (group_uid, position, uid);"
  "CREATE INDEX album_group_entries_album ON album_group_entries (album_uid);",
  /* v6: smart lists and last played. A smart list is a playlist or album group defined by a
   * rule (JSON, see docs/schema.md) that's evaluated each time it's listed. Like play_count,
   * last_played_ms is per device; a track's last play is the newest over its rows. */
  "ALTER TABLE track_plays ADD COLUMN last_played_ms INTEGER;"
  "CREATE TABLE smart_lists ("
  "  uid     TEXT PRIMARY KEY,"
  "  name    TEXT NOT NULL,"
  "  subject TEXT NOT NULL,"
  "  rule    TEXT NOT NULL"
  ") WITHOUT ROWID;",
  /* v7: recents, what was last opened. A row's uid is the opened item's own uid, so opening
   * one item on two devices writes the same row and sync keeps the newest open. */
  "CREATE TABLE recents ("
  "  uid       TEXT PRIMARY KEY,"
  "  kind      TEXT NOT NULL,"
  "  opened_ms INTEGER NOT NULL"
  ") WITHOUT ROWID;"
  "CREATE INDEX recents_opened ON recents (opened_ms);",
  /* v8: duplicate merging (sync/merge.c). The same artist, album or track imported on two
   * devices is one: aliases maps each merged-away uid to the smallest uid of its class, and
   * merge_keys holds the MusicBrainz ids (and singles albums' artists) ever logged for each
   * uid, which decide the classes. Both are derived from the changelog on every device, so
   * they are never logged; meta.merge_seq is how far the changelog has been read. */
  "CREATE TABLE aliases ("
  "  uid       TEXT PRIMARY KEY,"
  "  entity    TEXT NOT NULL,"
  "  canonical TEXT NOT NULL"
  ") WITHOUT ROWID;"
  "CREATE INDEX aliases_canonical ON aliases (canonical);"
  "CREATE TABLE merge_keys ("
  "  entity TEXT NOT NULL,"
  "  field  TEXT NOT NULL,"
  "  value  TEXT NOT NULL,"
  "  uid    TEXT NOT NULL,"
  "  PRIMARY KEY (entity, field, value, uid)"
  ") WITHOUT ROWID;"
  "CREATE INDEX merge_keys_uid ON merge_keys (uid);",
  /* v9: replacing a track's file. An import that swaps a track's original for a better file
   * (tagger.h, "Duplicates") gives the track a new file_rev (a uid; NULL until the first
   * swap). It is logged, so every device learns of the swap. files.file_rev is the file_rev
   * a file was made from: a received copy or transcode whose file_rev differs from its
   * track's is stale, and sync replaces it (transfer.c). Not logged: files are per device. */
  "ALTER TABLE tracks ADD COLUMN file_rev TEXT;"
  "ALTER TABLE files ADD COLUMN file_rev TEXT;",
  /* v10: example smart lists, so a library has some to open before there's an editor. Their
   * uids are fixed, so every device makes the same rows: they aren't logged (migrations
   * never are, see migrate), and a later rename, reshuffle or delete logs against the uid
   * every device already has. A rule with no matches just lists nothing, as on an empty
   * library. */
  "INSERT OR IGNORE INTO smart_lists (uid, name, subject, rule) VALUES"
  " ('5e1ec7ed-0000-4000-8000-000000000001', 'Recently added', 'tracks',"
  "  '{\"v\":1,\"match\":{\"field\":\"added\",\"op\":\"not_empty\"},"
  "\"order\":[{\"field\":\"added\",\"dir\":\"desc\"}],\"limit\":100}'),"
  " ('5e1ec7ed-0000-4000-8000-000000000002', 'Most played', 'tracks',"
  "  '{\"v\":1,\"match\":{\"field\":\"plays\",\"op\":\">\",\"value\":0},"
  "\"order\":[{\"field\":\"plays\",\"dir\":\"desc\"},{\"field\":\"last_played\",\"dir\":\"desc\"}],"
  "\"limit\":100}'),"
  " ('5e1ec7ed-0000-4000-8000-000000000003', 'Never played', 'tracks',"
  "  '{\"v\":1,\"match\":{\"field\":\"plays\",\"op\":\"=\",\"value\":0},"
  "\"order\":[{\"field\":\"random\",\"seed\":1}],\"limit\":100}'),"
  " ('5e1ec7ed-0000-4000-8000-000000000004', 'Recently added albums', 'albums',"
  "  '{\"v\":1,\"match\":{\"field\":\"added\",\"op\":\"not_empty\"},"
  "\"order\":[{\"field\":\"added\",\"dir\":\"desc\"}],\"limit\":24}'),"
  " ('5e1ec7ed-0000-4000-8000-000000000005', 'Random albums', 'albums',"
  "  '{\"v\":1,\"match\":{\"field\":\"singles\",\"op\":\"is\",\"value\":false},"
  "\"order\":[{\"field\":\"random\",\"seed\":1}],\"limit\":24}');",
};
#define MC_SCHEMA_VERSION ((int)(sizeof k_migrations / sizeof k_migrations[0]))

/* Columns whose changes are logged for sync. Keep in step with the latest schema. */
static const char *const k_artist_cols[] = {"name", "sort_name", "mb_artistid", NULL};
static const char *const k_album_cols[] = {"title", "artist_uid", "year", "original_year",
                                           "disc_total", "mb_albumid", "mb_releasegroupid",
                                           "art_hash", "kind", NULL};
static const char *const k_track_cols[] = {"album_uid", "artist_uid", "title", "track_no",
                                           "disc_no", "duration_ms", "genre", "mb_trackid",
                                           "mb_releasetrackid", "year", "mb_albumid", "added_ms",
                                           "file_rev", NULL};
static const char *const k_play_cols[] = {"track_uid", "node_uid", "play_count", "last_played_ms",
                                          NULL};
static const char *const k_playlist_cols[] = {"name", NULL};
static const char *const k_entry_cols[] = {"playlist_uid", "track_uid", "position", NULL};
static const char *const k_group_cols[] = {"name", NULL};
static const char *const k_group_entry_cols[] = {"group_uid", "album_uid", "position", NULL};
static const char *const k_smart_cols[] = {"name", "subject", "rule", NULL};
static const char *const k_recent_cols[] = {"kind", "opened_ms", NULL};

static const mc_changelog_table k_tracked[] = {
  {"artists", "uid", k_artist_cols},
  {"albums", "uid", k_album_cols},
  {"tracks", "uid", k_track_cols},
  {"track_plays", "uid", k_play_cols},
  {"playlists", "uid", k_playlist_cols},
  {"playlist_entries", "uid", k_entry_cols},
  {"album_groups", "uid", k_group_cols},
  {"album_group_entries", "uid", k_group_entry_cols},
  {"smart_lists", "uid", k_smart_cols},
  {"recents", "uid", k_recent_cols},
};

/* --- Schema --- */

static mc_status exec(mc_library *lib, const char *sql) {
  return sqlite3_exec(lib->db, sql, NULL, NULL, NULL) == SQLITE_OK ? MC_OK : MC_ERR_DB;
}

static int user_version(sqlite3 *db) {
  sqlite3_stmt *stmt;
  int version = -1;
  if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return version;
}

static mc_status migrate(mc_library *lib) {
  int version = user_version(lib->db);
  if (version < 0) return MC_ERR_DB;
  if (version > MC_SCHEMA_VERSION) return MC_ERR_FORMAT; /* written by a newer app */

  /* Rows a migration writes aren't logged: every device runs the same migration, and the
   * triggers left by the last open would log them before node_uid is loaded. */
  lib->applying_remote = 1;
  mc_status st = MC_OK;
  for (int v = version; v < MC_SCHEMA_VERSION && st == MC_OK; v++) {
    char bump[64];
    snprintf(bump, sizeof bump, "PRAGMA user_version = %d", v + 1);
    if (exec(lib, "BEGIN IMMEDIATE") != MC_OK) {
      st = MC_ERR_DB;
    } else if (exec(lib, k_migrations[v]) != MC_OK || exec(lib, bump) != MC_OK) {
      exec(lib, "ROLLBACK");
      st = MC_ERR_DB;
    } else if (exec(lib, "COMMIT") != MC_OK) {
      st = MC_ERR_DB;
    }
  }
  lib->applying_remote = 0;
  return st;
}

/* mc_shuffle_key(seed, uid): a stable pseudo-random sort key, so a list ordered by it is
 * shuffled the same way every time for the same seed. FNV-1a over the uid, mixed with the
 * seed by splitmix64's finalizer. */
static void sql_shuffle_key(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc;
  uint64_t h = 14695981039346656037u;
  const unsigned char *s = sqlite3_value_text(argv[1]);
  for (; s && *s; s++) h = (h ^ *s) * 1099511628211u;
  h ^= (uint64_t)sqlite3_value_int64(argv[0]) + 0x9e3779b97f4a7c15u;
  h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9u;
  h = (h ^ (h >> 27)) * 0x94d049bb133111ebu;
  h ^= h >> 31;
  sqlite3_result_int64(ctx, (sqlite3_int64)(h >> 1));
}

/* --- Identity --- */

/* A uid as mc_uid_new() writes it: 36 characters, lowercase hex and hyphens. */
static int is_uid(const char *s) {
  for (int i = 0; i < MC_UID_STRLEN - 1; i++) {
    int hyphen = i == 8 || i == 13 || i == 18 || i == 23;
    char c = s[i];
    if (hyphen ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
  }
  return s[MC_UID_STRLEN - 1] == '\0';
}

/* Reads a uid from meta into out: MC_ERR_NOT_FOUND if the key is absent, MC_ERR_FORMAT if
 * the value isn't a uid. */
static mc_status read_meta_uid(mc_library *lib, const char *key, char out[MC_UID_STRLEN]) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, "SELECT value FROM meta WHERE key = ?1", -1, &stmt, NULL) !=
      SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  mc_status st = MC_ERR_DB;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const unsigned char *v = sqlite3_column_text(stmt, 0);
    if (v && strlen((const char *)v) == MC_UID_STRLEN - 1) {
      memcpy(out, v, MC_UID_STRLEN);
      st = MC_OK;
    } else {
      st = MC_ERR_FORMAT;
    }
  } else if (rc == SQLITE_DONE) {
    st = MC_ERR_NOT_FOUND;
  }
  sqlite3_finalize(stmt);
  return st;
}

static mc_status write_meta(mc_library *lib, const char *key, const char *value) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, "INSERT OR REPLACE INTO meta (key, value) VALUES (?1, ?2)", -1,
                         &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
  mc_status st = sqlite3_step(stmt) == SQLITE_DONE ? MC_OK : MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

/* Loads a uid kept in meta, generating it the first time: node_uid (this device's identity
 * in the changelog) and library_uid (which library this is, so sync never mixes two). */
static mc_status load_meta_uid(mc_library *lib, const char *key, char out[MC_UID_STRLEN]) {
  mc_status st = read_meta_uid(lib, key, out);
  if (st != MC_ERR_NOT_FOUND) return st;
  if ((st = mc_uid_new(out)) != MC_OK) return st;
  return write_meta(lib, key, out);
}

/* --- Public API --- */

mc_status mc_library_open(const char *dir, mc_library **out) {
  if (!dir || !*dir || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;

  mc_library *lib = calloc(1, sizeof *lib);
  if (!lib) return MC_ERR_NOMEM;
  mc_hlc_init(&lib->hlc, mc_wall_ms, 0);
  lib->log.hlc = &lib->hlc;
  lib->log.node_uid = lib->node_uid;
  lib->log.applying_remote = &lib->applying_remote;

  /* Keep the folder without a trailing separator, so joining paths is uniform. */
  size_t len = strlen(dir);
  while (len > 1 && (dir[len - 1] == '/' || dir[len - 1] == '\\')) len--;
  char *db_path = malloc(len + sizeof MC_LIBRARY_DB_NAME + 1);
  lib->dir = malloc(len + 1);
  mc_status st = MC_ERR_NOMEM;
  if (!db_path || !lib->dir) goto fail;
  memcpy(lib->dir, dir, len);
  lib->dir[len] = '\0';
  snprintf(db_path, len + sizeof MC_LIBRARY_DB_NAME + 1, "%s%c" MC_LIBRARY_DB_NAME, lib->dir,
           MC_PATH_SEP);

  if ((st = mc_mkdirs(lib->dir)) != MC_OK) goto fail;
  st = MC_ERR_DB;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
  int rc = sqlite3_open_v2(db_path, &lib->db, flags, NULL);
  free(db_path);
  db_path = NULL;
  if (rc != SQLITE_OK) goto fail;

  sqlite3_busy_timeout(lib->db, 5000);
  if ((st = exec(lib, "PRAGMA journal_mode = WAL;"
                      "PRAGMA synchronous = NORMAL;"
                      "PRAGMA foreign_keys = ON;"
                      "PRAGMA trusted_schema = OFF;")) != MC_OK)
    goto fail;
  if ((st = mc_changelog_register(lib->db, &lib->log)) != MC_OK) goto fail;
  if (sqlite3_create_function(lib->db, "mc_shuffle_key", 2,
                              SQLITE_UTF8 | SQLITE_DETERMINISTIC | SQLITE_INNOCUOUS, NULL,
                              sql_shuffle_key, NULL, NULL) != SQLITE_OK) {
    st = MC_ERR_DB;
    goto fail;
  }
  if ((st = migrate(lib)) != MC_OK) goto fail;
  if ((st = mc_changelog_install_triggers(lib->db, k_tracked,
                                          sizeof k_tracked / sizeof k_tracked[0])) != MC_OK)
    goto fail;
  if ((st = load_meta_uid(lib, "node_uid", lib->node_uid)) != MC_OK) goto fail;
  if ((st = load_meta_uid(lib, "library_uid", lib->library_uid)) != MC_OK) goto fail;
  mc_hlc_init(&lib->hlc, mc_wall_ms, mc_changelog_max_hlc(lib->db));

  *out = lib;
  return MC_OK;

fail:
  if (lib->db) fprintf(stderr, "musiccore: library open failed: %s\n", sqlite3_errmsg(lib->db));
  free(db_path);
  mc_library_close(lib);
  return st;
}

void mc_library_close(mc_library *lib) {
  if (!lib) return;
  mc_search_free(lib->search);
  sqlite3_close_v2(lib->db);
  free(lib->dir);
  free(lib);
}

char *mc_library_relpath(const mc_library *lib, const char *abs) {
  size_t n = strlen(lib->dir);
  if (strlen(abs) <= n + 1 || (abs[n] != '/' && abs[n] != '\\')) return NULL;
  char *prefix = malloc(n + 1);
  if (!prefix) return NULL;
  memcpy(prefix, abs, n);
  prefix[n] = '\0';
  int inside = mc_same_path(prefix, lib->dir);
  free(prefix);
  if (!inside) return NULL;

  char *rel = mc_strdup(abs + n + 1);
  if (rel)
    for (char *p = rel; *p; p++)
      if (*p == '\\') *p = '/';
  return rel;
}

char *mc_library_abspath(const mc_library *lib, const char *rel) {
  size_t n = strlen(lib->dir), m = strlen(rel);
  char *abs = malloc(n + m + 2);
  if (!abs) return NULL;
  memcpy(abs, lib->dir, n);
  abs[n] = MC_PATH_SEP;
  memcpy(abs + n + 1, rel, m + 1);
  for (char *p = abs + n + 1; *p; p++)
    if (*p == '/') *p = MC_PATH_SEP;
  return abs;
}

mc_status mc_library_begin_write(mc_library *lib) {
  if (mc_changelog_begin_write(lib->db, &lib->hlc) != MC_OK) return MC_ERR_DB;
  /* Another connection may have rotated the node uid since this one opened. */
  char node[MC_UID_STRLEN];
  mc_status st = read_meta_uid(lib, "node_uid", node);
  if (st != MC_OK) {
    exec(lib, "ROLLBACK");
    return st;
  }
  memcpy(lib->node_uid, node, MC_UID_STRLEN);
  return MC_OK;
}

mc_status mc_library_commit(mc_library *lib) {
  return exec(lib, "COMMIT");
}

void mc_library_rollback(mc_library *lib) {
  exec(lib, "ROLLBACK");
}

int mc_library_schema_version(const mc_library *lib) {
  return user_version(lib->db);
}

const char *mc_library_dir(const mc_library *lib) {
  return lib->dir;
}

const char *mc_library_node_uid(const mc_library *lib) {
  return lib->node_uid;
}

const char *mc_library_uid(const mc_library *lib) {
  return lib->library_uid;
}

mc_status mc_library_rotate_node_uid(mc_library *lib) {
  if (!lib) return MC_ERR_INVALID_ARG;
  char fresh[MC_UID_STRLEN];
  mc_status st = mc_uid_new(fresh);
  if (st != MC_OK) return st;
  /* Not tracked: meta never syncs. Older changelog rows keep the old uid as their origin. */
  if ((st = write_meta(lib, "node_uid", fresh)) != MC_OK) return st;
  memcpy(lib->node_uid, fresh, MC_UID_STRLEN);
  return MC_OK;
}

mc_status mc_library_adopt_uid(mc_library *lib, const char *library_uid) {
  if (!lib || !library_uid || !is_uid(library_uid)) return MC_ERR_INVALID_ARG;
  if (strcmp(library_uid, lib->library_uid) == 0) return MC_OK;

  /* Count under the write lock, so an import on another connection can't slip in. */
  if (exec(lib, "BEGIN IMMEDIATE") != MC_OK) return MC_ERR_DB;
  int64_t tracks = -1;
  mc_status st = mc_library_count(lib, MC_ENTITY_TRACKS, &tracks);
  if (st == MC_OK && tracks > 0) st = MC_ERR_LIBRARY_MISMATCH;
  if (st == MC_OK) st = write_meta(lib, "library_uid", library_uid);
  if (st == MC_OK) st = exec(lib, "COMMIT");
  if (st != MC_OK) {
    exec(lib, "ROLLBACK");
    return st;
  }
  memcpy(lib->library_uid, library_uid, MC_UID_STRLEN);
  return MC_OK;
}

mc_status mc_library_meta_get(mc_library *lib, const char *key, char *out, size_t cap) {
  if (!lib || !key || !out || !cap) return MC_ERR_INVALID_ARG;
  out[0] = '\0';
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, "SELECT value FROM meta WHERE key = ?1", -1, &stmt, NULL) !=
      SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  mc_status st = rc == SQLITE_ROW ? MC_OK : rc == SQLITE_DONE ? MC_ERR_NOT_FOUND : MC_ERR_DB;
  if (st == MC_OK) {
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    snprintf(out, cap, "%s", v ? v : "");
  }
  sqlite3_finalize(stmt);
  return st;
}

mc_status mc_library_meta_set(mc_library *lib, const char *key, const char *value) {
  if (!lib || !key) return MC_ERR_INVALID_ARG;
  if (value) return write_meta(lib, key, value);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, "DELETE FROM meta WHERE key = ?1", -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  mc_status st = sqlite3_step(stmt) == SQLITE_DONE ? MC_OK : MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

/* The migration that seeds the example smart lists (v10); see mc_library_reset_for_merge(). */
#define EXAMPLES_MIGRATION 9

mc_status mc_library_reset_for_merge(mc_library *lib) {
  if (!lib) return MC_ERR_INVALID_ARG;
  char fresh[MC_UID_STRLEN];
  mc_status st = mc_uid_new(fresh);
  if (st != MC_OK) return st;
  if (exec(lib, "BEGIN IMMEDIATE") != MC_OK) return MC_ERR_DB;
  /* Nothing here is logged: the library starts again with an empty changelog under a new
   * node uid, as if just created (children before their parents, for the foreign keys). */
  lib->applying_remote = 1;
  static const char *const k_clear[] = {
    "DELETE FROM files", "DELETE FROM playlist_entries", "DELETE FROM album_group_entries",
    "DELETE FROM track_plays", "DELETE FROM recents", "DELETE FROM smart_lists",
    "DELETE FROM playlists", "DELETE FROM album_groups", "DELETE FROM tracks",
    "DELETE FROM albums", "DELETE FROM artists", "DELETE FROM aliases",
    "DELETE FROM merge_keys", "DELETE FROM changelog", "DELETE FROM meta WHERE key = 'merge_seq'",
  };
  for (size_t i = 0; i < sizeof k_clear / sizeof k_clear[0] && st == MC_OK; i++)
    st = exec(lib, k_clear[i]);
  /* The example lists are migration rows every library has, unlogged; the kept library's
   * edits to them arrive with its changelog. */
  if (st == MC_OK && EXAMPLES_MIGRATION < MC_SCHEMA_VERSION)
    st = exec(lib, k_migrations[EXAMPLES_MIGRATION]);
  if (st == MC_OK) st = write_meta(lib, "node_uid", fresh);
  lib->applying_remote = 0;
  if (st == MC_OK) st = exec(lib, "COMMIT");
  if (st != MC_OK) {
    exec(lib, "ROLLBACK");
    return st;
  }
  memcpy(lib->node_uid, fresh, MC_UID_STRLEN);
  return MC_OK;
}

const char *mc_library_errmsg(const mc_library *lib) {
  return sqlite3_errmsg(lib->db);
}

static char *column_dup(sqlite3_stmt *stmt, int col) {
  const unsigned char *s = sqlite3_column_text(stmt, col);
  return s ? mc_strdup((const char *)s) : NULL;
}

/* Copies a uid column into dst, leaving it "" if the column is NULL or too long. */
static void copy_uid(char dst[MC_UID_STRLEN], sqlite3_stmt *stmt, int col) {
  const unsigned char *s = sqlite3_column_text(stmt, col);
  if (s && strlen((const char *)s) < MC_UID_STRLEN) strcpy(dst, (const char *)s);
}

/* Grows a list's items array to hold one more, doubling as needed. */
static int grow(void **items, int count, int *cap, size_t size) {
  if (count < *cap) return 1;
  int grown = *cap ? *cap * 2 : 16;
  void *p = realloc(*items, (size_t)grown * size);
  if (!p) return 0;
  *items = p;
  *cap = grown;
  return 1;
}

mc_status mc_library_read_albums(mc_library *lib, sqlite3_stmt *stmt, mc_album_list **out) {
  *out = NULL;
  mc_album_list *list = calloc(1, sizeof *list);
  mc_status st = list ? MC_OK : MC_ERR_NOMEM;
  int cap = 0, rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (!grow((void **)&list->items, list->count, &cap, sizeof *list->items)) {
      st = MC_ERR_NOMEM;
      break;
    }
    mc_album_info *a = &list->items[list->count++];
    memset(a, 0, sizeof *a);
    const unsigned char *uid = sqlite3_column_text(stmt, 0);
    if (uid && strlen((const char *)uid) < MC_UID_STRLEN) strcpy(a->uid, (const char *)uid);
    a->title = column_dup(stmt, 1);
    a->artist = column_dup(stmt, 2);
    a->year = sqlite3_column_int(stmt, 3);
    a->is_singles = sqlite3_column_int(stmt, 4);
    a->has_art = sqlite3_column_int(stmt, 5);
    const unsigned char *file = sqlite3_column_text(stmt, 6);
    if (file) {
      char *abs = mc_library_abspath(lib, (const char *)file);
      char *sep = abs ? strrchr(abs, MC_PATH_SEP) : NULL;
      if (sep) *sep = '\0';
      a->folder = abs;
    }
    copy_uid(a->artist_uid, stmt, 7);
    if (sqlite3_column_count(stmt) > 9) { /* an artist's albums */
      a->track_count = sqlite3_column_int(stmt, 8);
      a->credited_count = sqlite3_column_int(stmt, 9);
    }
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    mc_album_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
}

mc_status mc_library_list_albums(mc_library *lib, mc_album_list **out) {
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  static const char sql[] = ALBUM_COLS " FROM albums a" ALBUM_JOINS " ORDER BY " ALBUM_ORDER;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  return mc_library_read_albums(lib, stmt, out);
}

void mc_album_list_free(mc_album_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    free(list->items[i].title);
    free(list->items[i].artist);
    free(list->items[i].folder);
  }
  free(list->items);
  free(list);
}

mc_status mc_library_read_tracks(mc_library *lib, sqlite3_stmt *stmt, mc_track_list **out) {
  *out = NULL;
  mc_track_list *list = calloc(1, sizeof *list);
  mc_status st = list ? MC_OK : MC_ERR_NOMEM;
  int cap = 0, rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (!grow((void **)&list->items, list->count, &cap, sizeof *list->items)) {
      st = MC_ERR_NOMEM;
      break;
    }
    mc_track_info *t = &list->items[list->count++];
    memset(t, 0, sizeof *t);
    const unsigned char *uid = sqlite3_column_text(stmt, 0);
    if (uid && strlen((const char *)uid) < MC_UID_STRLEN) strcpy(t->uid, (const char *)uid);
    t->title = column_dup(stmt, 1);
    t->track_no = sqlite3_column_int(stmt, 2);
    t->disc_no = sqlite3_column_int(stmt, 3);
    t->duration_ms = sqlite3_column_int(stmt, 4);
    const unsigned char *file = sqlite3_column_text(stmt, 5);
    if (file) t->path = mc_library_abspath(lib, (const char *)file);
    t->artist = column_dup(stmt, 6);
    t->album = column_dup(stmt, 7);
    /* The cover sits in the album folder, which is where the track's file is. */
    if (t->path && sqlite3_column_int(stmt, 8)) {
      t->cover_dir = mc_strdup(t->path);
      char *sep = t->cover_dir ? strrchr(t->cover_dir, MC_PATH_SEP) : NULL;
      if (sep) *sep = '\0';
    }
    copy_uid(t->artist_uid, stmt, 9);
    if (sqlite3_column_count(stmt) > 10) copy_uid(t->entry_uid, stmt, 10);
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    mc_track_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
}

/* Runs sql, a TRACK_COLS query with key as its one parameter, into a track list. */
static mc_status list_tracks(mc_library *lib, const char *sql, const char *key, mc_track_list **out) {
  if (!lib || !key || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  if (sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return MC_ERR_DB;
  }
  return mc_library_read_tracks(lib, stmt, out);
}

mc_status mc_library_list_album_tracks(mc_library *lib, const char *album_uid, mc_track_list **out) {
  static const char sql[] =
    TRACK_COLS " FROM tracks t" TRACK_JOINS " WHERE t.album_uid = ?"
    " ORDER BY coalesce(t.disc_no, 0), t.track_no IS NULL, t.track_no, t.title COLLATE NOCASE";
  return list_tracks(lib, sql, album_uid, out);
}

mc_status mc_library_list_playlist_tracks(mc_library *lib, const char *playlist_uid, mc_track_list **out) {
  static const char sql[] =
    TRACK_COLS ", e.uid FROM playlist_entries e JOIN tracks t ON t.uid = e.track_uid" TRACK_JOINS
    " WHERE e.playlist_uid = ? ORDER BY e.position, e.uid";
  return list_tracks(lib, sql, playlist_uid, out);
}

void mc_track_list_free(mc_track_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    free(list->items[i].title);
    free(list->items[i].path);
    free(list->items[i].artist);
    free(list->items[i].album);
    free(list->items[i].cover_dir);
  }
  free(list->items);
  free(list);
}

mc_status mc_library_list_playlists(mc_library *lib, unsigned flags, mc_playlist_list **out) {
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  /* A track is playable here if it has a file here: the test TRACK_COLS's path makes. */
  static const char sql[] =
    "SELECT p.uid, p.name, (SELECT count(*) FROM playlist_entries e WHERE e.playlist_uid = p.uid),"
    " (SELECT count(*) FROM playlist_entries e WHERE e.playlist_uid = p.uid"
    "   AND EXISTS (SELECT 1 FROM files f WHERE f.track_uid = e.track_uid))"
    " FROM playlists p ORDER BY p.name COLLATE NOCASE, p.uid";

  mc_playlist_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  sqlite3_stmt *stmt = NULL;
  mc_status st = sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) == SQLITE_OK ? MC_OK : MC_ERR_DB;
  int cap = 0, rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    int entries = sqlite3_column_int(stmt, 2), playable = sqlite3_column_int(stmt, 3);
    /* An empty one is kept: it's waiting for tracks, not missing them. */
    if ((flags & MC_LIST_PLAYABLE) && entries > 0 && playable == 0) {
      list->hidden++;
      continue;
    }
    if (!grow((void **)&list->items, list->count, &cap, sizeof *list->items)) {
      st = MC_ERR_NOMEM;
      break;
    }
    mc_playlist_info *p = &list->items[list->count++];
    memset(p, 0, sizeof *p);
    const unsigned char *uid = sqlite3_column_text(stmt, 0);
    if (uid && strlen((const char *)uid) < MC_UID_STRLEN) strcpy(p->uid, (const char *)uid);
    p->name = column_dup(stmt, 1);
    p->track_count = entries;
    p->playable_count = playable;
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    mc_playlist_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
}

void mc_playlist_list_free(mc_playlist_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) free(list->items[i].name);
  free(list->items);
  free(list);
}

#define ARTIST_ORDER " ORDER BY coalesce(ar.sort_name, ar.name) COLLATE NOCASE, ar.name COLLATE NOCASE, ar.uid"

/* Runs sql (uid, name, album count, then a track count if it has four columns) into a list. */
static mc_status list_artists(mc_library *lib, const char *sql, mc_artist_list **out) {
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_artist_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  sqlite3_stmt *stmt = NULL;
  mc_status st = sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) == SQLITE_OK ? MC_OK : MC_ERR_DB;
  int cap = 0, rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (!grow((void **)&list->items, list->count, &cap, sizeof *list->items)) {
      st = MC_ERR_NOMEM;
      break;
    }
    mc_artist_info *a = &list->items[list->count++];
    memset(a, 0, sizeof *a);
    const unsigned char *uid = sqlite3_column_text(stmt, 0);
    if (uid && strlen((const char *)uid) < MC_UID_STRLEN) strcpy(a->uid, (const char *)uid);
    a->name = column_dup(stmt, 1);
    a->album_count = sqlite3_column_int(stmt, 2);
    if (sqlite3_column_count(stmt) > 3) a->track_count = sqlite3_column_int(stmt, 3);
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    mc_artist_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
}

mc_status mc_library_list_artists(mc_library *lib, mc_artist_list **out) {
  /* Artists nothing refers to any more are left out, as the albums and tracks views do. */
  static const char sql[] =
    "SELECT ar.uid, ar.name, (SELECT count(*) FROM albums a WHERE a.artist_uid = ar.uid) AS n_albums"
    " FROM artists ar"
    " WHERE n_albums > 0 OR EXISTS (SELECT 1 FROM tracks t WHERE t.artist_uid = ar.uid)" ARTIST_ORDER;
  return list_artists(lib, sql, out);
}

mc_status mc_library_list_artists_here(mc_library *lib, mc_artist_list **out) {
  /* Counted once for every artist rather than per artist: a track is its own artist's, else
   * its album's, which no index finds. */
  static const char sql[] =
    "SELECT ar.uid, ar.name, coalesce(na.n, 0), coalesce(nt.n, 0) FROM artists ar"
    " LEFT JOIN (SELECT a.artist_uid AS uid, count(*) AS n FROM albums a"
    "  WHERE EXISTS (SELECT 1 FROM tracks t JOIN files f ON f.track_uid = t.uid WHERE t.album_uid = a.uid)"
    "  GROUP BY a.artist_uid) na ON na.uid = ar.uid"
    " LEFT JOIN (SELECT coalesce(t.artist_uid, al.artist_uid) AS uid, count(*) AS n FROM tracks t"
    "  LEFT JOIN albums al ON al.uid = t.album_uid"
    "  WHERE EXISTS (SELECT 1 FROM files f WHERE f.track_uid = t.uid)"
    "  GROUP BY 1) nt ON nt.uid = ar.uid"
    " WHERE na.n > 0 OR nt.n > 0" ARTIST_ORDER;
  return list_artists(lib, sql, out);
}

void mc_artist_list_free(mc_artist_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) free(list->items[i].name);
  free(list->items);
  free(list);
}

mc_status mc_library_list_artist_albums(mc_library *lib, const char *artist_uid, mc_album_list **out) {
  if (!lib || !artist_uid || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  /* c counts the tracks of just the albums the artist is on (by the two artist indexes). An
   * album artist's album with no tracks has no c row, and counts as all theirs. */
  static const char sql[] =
    ALBUM_COLS ", coalesce(c.n, 0), coalesce(c.credited, 0) FROM albums a" ALBUM_JOINS
    " LEFT JOIN (SELECT t.album_uid, count(*) AS n,"
    "   count(CASE WHEN coalesce(t.artist_uid, al.artist_uid) = ?1 THEN 1 END) AS credited"
    "  FROM tracks t JOIN albums al ON al.uid = t.album_uid"
    "  WHERE t.album_uid IN (SELECT album_uid FROM tracks WHERE artist_uid = ?1"
    "   UNION SELECT uid FROM albums WHERE artist_uid = ?1)"
    "  GROUP BY t.album_uid) c ON c.album_uid = a.uid"
    " WHERE c.credited > 0 OR a.artist_uid = ?1"
    " ORDER BY max(a.artist_uid IS ?1, coalesce(c.credited * 1.0 / c.n, 0)) DESC, a.kind = 'singles', a.year,"
    "  a.title COLLATE NOCASE, a.uid";
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  if (sqlite3_bind_text(stmt, 1, artist_uid, -1, SQLITE_STATIC) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return MC_ERR_DB;
  }
  return mc_library_read_albums(lib, stmt, out);
}

mc_status mc_library_list_artist_tracks(mc_library *lib, const char *artist_uid, mc_track_list **out) {
  /* Their own tracks, and those with no artist on their albums, each by its index. */
  static const char sql[] =
    TRACK_COLS " FROM tracks t" TRACK_JOINS
    " WHERE t.uid IN (SELECT uid FROM tracks WHERE artist_uid = ?1"
    "  UNION ALL SELECT t2.uid FROM albums a2 JOIN tracks t2 ON t2.album_uid = a2.uid"
    "  WHERE a2.artist_uid = ?1 AND t2.artist_uid IS NULL)"
    " ORDER BY t.title COLLATE NOCASE, al.title COLLATE NOCASE, coalesce(t.disc_no, 0), t.track_no, t.uid";
  return list_tracks(lib, sql, artist_uid, out);
}

mc_status mc_library_list_album_groups(mc_library *lib, mc_album_group_list **out) {
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  /* Smart album groups live in smart_lists, so this table holds only the manual ones. */
  static const char sql[] =
    "SELECT g.uid, g.name,"
    " (SELECT count(DISTINCT e.album_uid) FROM album_group_entries e WHERE e.group_uid = g.uid)"
    " FROM album_groups g ORDER BY g.name COLLATE NOCASE, g.uid";

  mc_album_group_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  sqlite3_stmt *stmt = NULL;
  mc_status st = sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) == SQLITE_OK ? MC_OK : MC_ERR_DB;
  int cap = 0, rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (!grow((void **)&list->items, list->count, &cap, sizeof *list->items)) {
      st = MC_ERR_NOMEM;
      break;
    }
    mc_album_group_info *g = &list->items[list->count++];
    memset(g, 0, sizeof *g);
    const unsigned char *uid = sqlite3_column_text(stmt, 0);
    if (uid && strlen((const char *)uid) < MC_UID_STRLEN) strcpy(g->uid, (const char *)uid);
    g->name = column_dup(stmt, 1);
    g->album_count = sqlite3_column_int(stmt, 2);
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    mc_album_group_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
}

void mc_album_group_list_free(mc_album_group_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) free(list->items[i].name);
  free(list->items);
  free(list);
}

mc_status mc_library_list_album_group_albums(mc_library *lib, const char *group_uid, mc_album_list **out) {
  if (!lib || !group_uid || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, "SELECT 1 FROM album_groups WHERE uid = ?1", -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, group_uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE) return MC_ERR_NOT_FOUND;
  if (rc != SQLITE_ROW) return MC_ERR_DB;
  /* An album two devices added at once has two entries: it's listed once, at the first. */
  static const char sql[] = ALBUM_COLS " FROM albums a" ALBUM_JOINS
    " JOIN (SELECT album_uid, min(position) AS pos FROM album_group_entries WHERE group_uid = ?1"
    "  GROUP BY album_uid) e ON e.album_uid = a.uid"
    " ORDER BY e.pos, a.uid";
  if (sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, group_uid, -1, SQLITE_STATIC);
  return mc_library_read_albums(lib, stmt, out);
}

mc_status mc_library_count(mc_library *lib, mc_entity entity, int64_t *out) {
  static const char *const sql[] = {
    [MC_ENTITY_ARTISTS] = "SELECT count(*) FROM artists",
    [MC_ENTITY_ALBUMS] = "SELECT count(*) FROM albums",
    [MC_ENTITY_TRACKS] = "SELECT count(*) FROM tracks",
    [MC_ENTITY_FILES] = "SELECT count(*) FROM files",
    [MC_ENTITY_PLAYLISTS] = "SELECT count(*) FROM playlists",
  };
  if (!lib || !out || (unsigned)entity >= sizeof sql / sizeof sql[0]) return MC_ERR_INVALID_ARG;

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, sql[entity], -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  mc_status st = MC_ERR_DB;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    *out = sqlite3_column_int64(stmt, 0);
    st = MC_OK;
  }
  sqlite3_finalize(stmt);
  return st;
}

mc_status mc_library_record_play(mc_library *lib, const char *track_uid) {
  if (!lib || !track_uid) return MC_ERR_INVALID_ARG;
  char uid[MC_UID_STRLEN];
  mc_status st = mc_uid_new(uid);
  if (st != MC_OK || (st = mc_library_begin_write(lib)) != MC_OK) return st;
  /* Only this device's row, so plays on two devices never write the same field. */
  static const char sql[] =
    "INSERT INTO track_plays (uid, track_uid, node_uid, play_count, last_played_ms)"
    " VALUES (?1, ?2, ?3, 1, ?4)"
    " ON CONFLICT (track_uid, node_uid) DO UPDATE"
    " SET play_count = play_count + 1, last_played_ms = excluded.last_played_ms";
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    mc_library_rollback(lib);
    return MC_ERR_DB;
  }
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, track_uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, lib->node_uid, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4, (sqlite3_int64)lib->hlc.wall_ms());
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  st = rc == SQLITE_DONE ? MC_OK : (rc & 0xff) == SQLITE_CONSTRAINT ? MC_ERR_NOT_FOUND : MC_ERR_DB;
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

/* recents.kind, indexed by mc_recent_kind; the same names as sync profile items. */
static const char *const k_recent_kinds[] = {"album", "artist", "playlist", "album_group"};
#define RECENT_KIND_COUNT ((int)(sizeof k_recent_kinds / sizeof k_recent_kinds[0]))

/* Runs a write to recents in a transaction of its own, binding text parameters p1 and p2
 * (unless NULL) and now as ?3 if it has one. MC_ERR_NOT_FOUND if it changed no row. */
static mc_status write_recents(mc_library *lib, const char *sql, const char *p1, const char *p2) {
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    mc_library_rollback(lib);
    return MC_ERR_DB;
  }
  if (p1) sqlite3_bind_text(stmt, 1, p1, -1, SQLITE_STATIC);
  if (p2) sqlite3_bind_text(stmt, 2, p2, -1, SQLITE_STATIC);
  /* The open's time is the wall-clock part of a fresh hlc, just before the one that logs it.
   * An open on any device that has seen this one then gets a later time as well as a later
   * hlc, so sync's newest change by hlc is also the newest open, even when clocks disagree. */
  if (sqlite3_bind_parameter_count(stmt) >= 3)
    sqlite3_bind_int64(stmt, 3, (sqlite3_int64)mc_hlc_physical_ms(mc_hlc_now(&lib->hlc)));
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  st = rc != SQLITE_DONE ? MC_ERR_DB : sqlite3_changes(lib->db) ? MC_OK : MC_ERR_NOT_FOUND;
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) mc_library_rollback(lib);
  return st;
}

mc_status mc_library_record_opened(mc_library *lib, mc_recent_kind kind, const char *uid) {
  if (!lib || !uid || (unsigned)kind >= RECENT_KIND_COUNT) return MC_ERR_INVALID_ARG;
  static const char sql[] =
    "INSERT INTO recents (uid, kind, opened_ms) SELECT ?1, ?2, ?3"
    " WHERE CASE ?2"
    "  WHEN 'album' THEN EXISTS (SELECT 1 FROM albums WHERE uid = ?1)"
    "  WHEN 'artist' THEN EXISTS (SELECT 1 FROM artists WHERE uid = ?1)"
    "  WHEN 'playlist' THEN EXISTS (SELECT 1 FROM playlists WHERE uid = ?1)"
    "   OR EXISTS (SELECT 1 FROM smart_lists WHERE uid = ?1 AND subject = 'tracks')"
    "  WHEN 'album_group' THEN EXISTS (SELECT 1 FROM album_groups WHERE uid = ?1)"
    "   OR EXISTS (SELECT 1 FROM smart_lists WHERE uid = ?1 AND subject = 'albums')"
    " END"
    " ON CONFLICT (uid) DO UPDATE SET kind = excluded.kind, opened_ms = excluded.opened_ms";
  return write_recents(lib, sql, uid, k_recent_kinds[kind]);
}

mc_status mc_library_remove_recent(mc_library *lib, const char *uid) {
  if (!lib || !uid) return MC_ERR_INVALID_ARG;
  return write_recents(lib, "DELETE FROM recents WHERE uid = ?1", uid, NULL);
}

mc_status mc_library_clear_recents(mc_library *lib) {
  if (!lib) return MC_ERR_INVALID_ARG;
  mc_status st = write_recents(lib, "DELETE FROM recents", NULL, NULL);
  return st == MC_ERR_NOT_FOUND ? MC_OK : st;
}

mc_status mc_library_list_recents(mc_library *lib, int limit, unsigned flags, mc_recent_list **out) {
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  /* Each kind joins only its own table, so an entry whose item is gone, or of a kind this
   * version doesn't know, matches nothing and is left out. */
  static const char sql[] =
    "SELECT r.kind, r.uid, r.opened_ms, coalesce(al.title, ar.name, p.name, g.name, s.name),"
    " aar.name, s.uid IS NOT NULL,"
    " p.uid IS NULL OR NOT EXISTS (SELECT 1 FROM playlist_entries e WHERE e.playlist_uid = p.uid)"
    "  OR EXISTS (SELECT 1 FROM playlist_entries e JOIN files f ON f.track_uid = e.track_uid"
    "  WHERE e.playlist_uid = p.uid)"
    " FROM recents r"
    " LEFT JOIN albums al ON r.kind = 'album' AND al.uid = r.uid"
    " LEFT JOIN artists aar ON aar.uid = al.artist_uid"
    " LEFT JOIN artists ar ON r.kind = 'artist' AND ar.uid = r.uid"
    " LEFT JOIN playlists p ON r.kind = 'playlist' AND p.uid = r.uid"
    " LEFT JOIN album_groups g ON r.kind = 'album_group' AND g.uid = r.uid"
    " LEFT JOIN smart_lists s ON s.uid = r.uid AND s.subject ="
    "  CASE r.kind WHEN 'playlist' THEN 'tracks' WHEN 'album_group' THEN 'albums' END"
    " WHERE coalesce(al.uid, ar.uid, p.uid, g.uid, s.uid) IS NOT NULL"
    " ORDER BY r.opened_ms DESC, r.uid DESC LIMIT ?1";

  mc_recent_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  sqlite3_stmt *stmt = NULL;
  mc_status st = sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) == SQLITE_OK ? MC_OK : MC_ERR_DB;
  /* Leaving items out means the limit is counted here rather than in SQL. */
  int playable_only = (flags & MC_LIST_PLAYABLE) != 0;
  if (st == MC_OK) sqlite3_bind_int(stmt, 1, limit > 0 && !playable_only ? limit : -1);
  int cap = 0, rc = SQLITE_DONE;
  while (st == MC_OK && (limit <= 0 || list->count < limit) && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *kind = (const char *)sqlite3_column_text(stmt, 0);
    int k = 0;
    while (k < RECENT_KIND_COUNT && strcmp(kind, k_recent_kinds[k])) k++;
    if (playable_only && k == MC_RECENT_PLAYLIST) {
      int playable = sqlite3_column_int(stmt, 6);
      if (sqlite3_column_int(stmt, 5))
        st = mc_smart_has_playable(lib, (const char *)sqlite3_column_text(stmt, 1), &playable);
      if (st != MC_OK) break;
      if (!playable) continue;
    }
    if (!grow((void **)&list->items, list->count, &cap, sizeof *list->items)) {
      st = MC_ERR_NOMEM;
      break;
    }
    mc_recent_info *r = &list->items[list->count++];
    memset(r, 0, sizeof *r);
    r->kind = (mc_recent_kind)k;
    const unsigned char *uid = sqlite3_column_text(stmt, 1);
    if (uid && strlen((const char *)uid) < MC_UID_STRLEN) strcpy(r->uid, (const char *)uid);
    r->opened_ms = sqlite3_column_int64(stmt, 2);
    r->name = column_dup(stmt, 3);
    r->artist = column_dup(stmt, 4);
    r->is_smart = sqlite3_column_int(stmt, 5);
  }
  if (st == MC_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    mc_recent_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
}

void mc_recent_list_free(mc_recent_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    free(list->items[i].name);
    free(list->items[i].artist);
  }
  free(list->items);
  free(list);
}

const mc_changelog_table *mc_library_tracked(size_t *count) {
  *count = sizeof k_tracked / sizeof k_tracked[0];
  return k_tracked;
}
