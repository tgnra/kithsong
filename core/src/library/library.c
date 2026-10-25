#include "library/library_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
                                           NULL};
static const char *const k_play_cols[] = {"track_uid", "node_uid", "play_count", NULL};
static const char *const k_playlist_cols[] = {"name", NULL};
static const char *const k_entry_cols[] = {"playlist_uid", "track_uid", "position", NULL};

static const struct {
  const char *table;
  const char *const *cols;
} k_tracked[] = {
  {"artists", k_artist_cols},
  {"albums", k_album_cols},
  {"tracks", k_track_cols},
  {"track_plays", k_play_cols},
  {"playlists", k_playlist_cols},
  {"playlist_entries", k_entry_cols},
};

/* --- SQL functions used by the change-log triggers --- */

static void sql_hlc_now(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc;
  (void)argv;
  mc_library *lib = sqlite3_user_data(ctx);
  sqlite3_result_int64(ctx, (sqlite3_int64)mc_hlc_now(&lib->hlc));
}

static void sql_node_uid(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc;
  (void)argv;
  mc_library *lib = sqlite3_user_data(ctx);
  sqlite3_result_text(ctx, lib->node_uid, -1, SQLITE_TRANSIENT);
}

static void sql_applying_remote(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc;
  (void)argv;
  mc_library *lib = sqlite3_user_data(ctx);
  sqlite3_result_int(ctx, lib->applying_remote);
}

static mc_status register_functions(mc_library *lib) {
  static const struct {
    const char *name;
    void (*fn)(sqlite3_context *, int, sqlite3_value **);
  } fns[] = {
    {"mc_hlc_now", sql_hlc_now},
    {"mc_node_uid", sql_node_uid},
    {"mc_applying_remote", sql_applying_remote},
  };
  for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
    /* SQLITE_INNOCUOUS is required for use inside triggers while trusted_schema is OFF. */
    int rc = sqlite3_create_function(lib->db, fns[i].name, 0, SQLITE_UTF8 | SQLITE_INNOCUOUS,
                                     lib, fns[i].fn, NULL, NULL);
    if (rc != SQLITE_OK) return MC_ERR_DB;
  }
  return MC_OK;
}

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

  for (int v = version; v < MC_SCHEMA_VERSION; v++) {
    char bump[64];
    snprintf(bump, sizeof bump, "PRAGMA user_version = %d", v + 1);
    if (exec(lib, "BEGIN IMMEDIATE") != MC_OK) return MC_ERR_DB;
    if (exec(lib, k_migrations[v]) != MC_OK || exec(lib, bump) != MC_OK) {
      exec(lib, "ROLLBACK");
      return MC_ERR_DB;
    }
    if (exec(lib, "COMMIT") != MC_OK) return MC_ERR_DB;
  }
  return MC_OK;
}

/* One INSERT and one UPDATE trigger per tracked column, plus a DELETE trigger per table.
 * Recreated on every open so changes to the column lists take effect immediately. */
static mc_status install_triggers(mc_library *lib) {
  char sql[1024];
  if (exec(lib, "BEGIN IMMEDIATE") != MC_OK) return MC_ERR_DB;

  for (size_t t = 0; t < sizeof k_tracked / sizeof k_tracked[0]; t++) {
    const char *table = k_tracked[t].table;
    for (const char *const *c = k_tracked[t].cols; *c; c++) {
      snprintf(sql, sizeof sql,
               "DROP TRIGGER IF EXISTS mc_log_ins_%s_%s;"
               "CREATE TRIGGER mc_log_ins_%s_%s AFTER INSERT ON %s"
               " WHEN new.%s IS NOT NULL AND mc_applying_remote() = 0 BEGIN"
               " INSERT INTO changelog (hlc, origin_uid, entity, row_uid, field, value)"
               " VALUES (mc_hlc_now(), mc_node_uid(), '%s', new.uid, '%s', new.%s);"
               " END;",
               table, *c, table, *c, table, *c, table, *c, *c);
      if (exec(lib, sql) != MC_OK) goto fail;

      snprintf(sql, sizeof sql,
               "DROP TRIGGER IF EXISTS mc_log_upd_%s_%s;"
               "CREATE TRIGGER mc_log_upd_%s_%s AFTER UPDATE OF %s ON %s"
               " WHEN old.%s IS NOT new.%s AND mc_applying_remote() = 0 BEGIN"
               " INSERT INTO changelog (hlc, origin_uid, entity, row_uid, field, value)"
               " VALUES (mc_hlc_now(), mc_node_uid(), '%s', new.uid, '%s', new.%s);"
               " END;",
               table, *c, table, *c, *c, table, *c, *c, table, *c, *c);
      if (exec(lib, sql) != MC_OK) goto fail;
    }

    snprintf(sql, sizeof sql,
             "DROP TRIGGER IF EXISTS mc_log_del_%s;"
             "CREATE TRIGGER mc_log_del_%s AFTER DELETE ON %s"
             " WHEN mc_applying_remote() = 0 BEGIN"
             " INSERT INTO changelog (hlc, origin_uid, entity, row_uid, field, value)"
             " VALUES (mc_hlc_now(), mc_node_uid(), '%s', old.uid, '_deleted', 1);"
             " END;",
             table, table, table, table);
    if (exec(lib, sql) != MC_OK) goto fail;
  }
  return exec(lib, "COMMIT");

fail:
  exec(lib, "ROLLBACK");
  return MC_ERR_DB;
}

static mc_status load_node_uid(mc_library *lib) {
  sqlite3_stmt *stmt;
  mc_status st = MC_ERR_DB;

  if (sqlite3_prepare_v2(lib->db, "SELECT value FROM meta WHERE key = 'node_uid'", -1, &stmt,
                         NULL) != SQLITE_OK)
    return MC_ERR_DB;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const unsigned char *v = sqlite3_column_text(stmt, 0);
    if (v && strlen((const char *)v) == MC_UID_STRLEN - 1) {
      memcpy(lib->node_uid, v, MC_UID_STRLEN);
      st = MC_OK;
    } else {
      st = MC_ERR_FORMAT;
    }
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) return st;

  /* First open: this device gets its identity. */
  if ((st = mc_uid_new(lib->node_uid)) != MC_OK) return st;
  if (sqlite3_prepare_v2(lib->db, "INSERT INTO meta (key, value) VALUES ('node_uid', ?1)", -1,
                         &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, lib->node_uid, -1, SQLITE_STATIC);
  st = sqlite3_step(stmt) == SQLITE_DONE ? MC_OK : MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

static uint64_t max_logged_hlc(mc_library *lib) {
  sqlite3_stmt *stmt;
  uint64_t hlc = 0;
  if (sqlite3_prepare_v2(lib->db, "SELECT max(hlc) FROM changelog", -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) hlc = (uint64_t)sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return hlc;
}

/* --- Public API --- */

mc_status mc_library_open(const char *dir, mc_library **out) {
  if (!dir || !*dir || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;

  mc_library *lib = calloc(1, sizeof *lib);
  if (!lib) return MC_ERR_NOMEM;
  mc_hlc_init(&lib->hlc, mc_wall_ms, 0);

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
  if ((st = register_functions(lib)) != MC_OK) goto fail;
  if ((st = migrate(lib)) != MC_OK) goto fail;
  if ((st = install_triggers(lib)) != MC_OK) goto fail;
  if ((st = load_node_uid(lib)) != MC_OK) goto fail;
  mc_hlc_init(&lib->hlc, mc_wall_ms, max_logged_hlc(lib));

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
  if (exec(lib, "BEGIN IMMEDIATE") != MC_OK) return MC_ERR_DB;
  uint64_t seen = max_logged_hlc(lib);
  if (seen > lib->hlc.last) lib->hlc.last = seen;
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

const char *mc_library_errmsg(const mc_library *lib) {
  return sqlite3_errmsg(lib->db);
}

static char *column_dup(sqlite3_stmt *stmt, int col) {
  const unsigned char *s = sqlite3_column_text(stmt, col);
  return s ? mc_strdup((const char *)s) : NULL;
}

mc_status mc_library_list_albums(mc_library *lib, mc_album_list **out) {
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  /* The folder is wherever the album's files are: any one file's directory will do. */
  static const char sql[] =
    "SELECT a.uid, a.title, ar.name, a.year, a.kind = 'singles', a.art_hash IS NOT NULL,"
    " (SELECT f.path FROM tracks t JOIN files f ON f.track_uid = t.uid"
    "  WHERE t.album_uid = a.uid LIMIT 1)"
    " FROM albums a LEFT JOIN artists ar ON ar.uid = a.artist_uid"
    " ORDER BY coalesce(ar.sort_name, ar.name) COLLATE NOCASE, a.kind = 'singles',"
    " a.year, a.title COLLATE NOCASE";

  mc_album_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  int64_t total = 0;
  mc_status st = mc_library_count(lib, MC_ENTITY_ALBUMS, &total);
  if (st == MC_OK && total > 0 && !(list->items = calloc((size_t)total, sizeof *list->items)))
    st = MC_ERR_NOMEM;

  sqlite3_stmt *stmt = NULL;
  if (st == MC_OK && sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) != SQLITE_OK) st = MC_ERR_DB;
  while (st == MC_OK && list->count < total && sqlite3_step(stmt) == SQLITE_ROW) {
    mc_album_info *a = &list->items[list->count++];
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
  }
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    mc_album_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
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

mc_status mc_library_list_album_tracks(mc_library *lib, const char *album_uid, mc_track_list **out) {
  if (!lib || !album_uid || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  static const char sql[] =
    "SELECT t.uid, t.title, t.track_no, t.disc_no, t.duration_ms,"
    " (SELECT f.path FROM files f WHERE f.track_uid = t.uid"
    "  ORDER BY f.source_file_uid IS NOT NULL LIMIT 1)"
    " FROM tracks t WHERE t.album_uid = ?"
    " ORDER BY coalesce(t.disc_no, 0), t.track_no IS NULL, t.track_no, t.title COLLATE NOCASE";

  mc_track_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  sqlite3_stmt *stmt = NULL;
  mc_status st = MC_OK;
  if (sqlite3_prepare_v2(lib->db, sql, -1, &stmt, NULL) != SQLITE_OK ||
      sqlite3_bind_text(stmt, 1, album_uid, -1, SQLITE_STATIC) != SQLITE_OK)
    st = MC_ERR_DB;
  int cap = 0, rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (list->count == cap) {
      int grown = cap ? cap * 2 : 16;
      mc_track_info *items = realloc(list->items, (size_t)grown * sizeof *items);
      if (!items) {
        st = MC_ERR_NOMEM;
        break;
      }
      list->items = items;
      cap = grown;
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

void mc_track_list_free(mc_track_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    free(list->items[i].title);
    free(list->items[i].path);
  }
  free(list->items);
  free(list);
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
