#include "library/library_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/platform.h"

extern const unsigned char mc_schema_v1_sql[];

/* Migration i takes the database from user_version i to i + 1. Append only. */
static const char *const k_migrations[] = {
  (const char *)mc_schema_v1_sql,
};
#define MC_SCHEMA_VERSION ((int)(sizeof k_migrations / sizeof k_migrations[0]))

/* Columns whose changes are logged for sync. Keep in step with the latest schema. */
static const char *const k_artist_cols[] = {"name", "sort_name", "mb_artistid", NULL};
static const char *const k_album_cols[] = {"title", "artist_uid", "year", "original_year",
                                           "disc_total", "mb_albumid", "mb_releasegroupid",
                                           "art_hash", NULL};
static const char *const k_track_cols[] = {"album_uid", "artist_uid", "title", "track_no",
                                           "disc_no", "duration_ms", "genre", "mb_trackid",
                                           "mb_releasetrackid", NULL};
static const char *const k_playlist_cols[] = {"name", NULL};
static const char *const k_entry_cols[] = {"playlist_uid", "track_uid", "position", NULL};

static const struct {
  const char *table;
  const char *const *cols;
} k_tracked[] = {
  {"artists", k_artist_cols},
  {"albums", k_album_cols},
  {"tracks", k_track_cols},
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

mc_status mc_library_open(const char *path, mc_library **out) {
  if (!path || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;

  mc_library *lib = calloc(1, sizeof *lib);
  if (!lib) return MC_ERR_NOMEM;
  mc_hlc_init(&lib->hlc, mc_wall_ms, 0);

  mc_status st = MC_ERR_DB;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(path, &lib->db, flags, NULL) != SQLITE_OK) goto fail;

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
  mc_library_close(lib);
  return st;
}

void mc_library_close(mc_library *lib) {
  if (!lib) return;
  sqlite3_close_v2(lib->db);
  free(lib);
}

int mc_library_schema_version(const mc_library *lib) {
  return user_version(lib->db);
}

const char *mc_library_node_uid(const mc_library *lib) {
  return lib->node_uid;
}

const char *mc_library_errmsg(const mc_library *lib) {
  return sqlite3_errmsg(lib->db);
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
