/* Deleting tracks to the trash, and restoring them (see musiccore/delete.h).
 *
 * A track's rows are saved generically, as JSON objects of column -> value read with
 * PRAGMA table_info, and put back by inserting whichever of those columns the table still
 * has. So a record survives schema migrations that add columns (they get their defaults),
 * and nothing here needs to change when the schema does. */
#include "musiccore/delete.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "library/delete_internal.h"
#include "library/library_internal.h"
#include "util/platform.h"

#define DELETED_DB_NAME "deleted.db"
#define SHA_HEX 65
#define DAY_MS (24LL * 60 * 60 * 1000)
/* A restored file's modification time may be rounded (FAT keeps two seconds). */
#define MTIME_SLACK_MS 2000

mc_status (*mc_delete_trash)(const char *path) = mc_trash;
mc_status (*mc_delete_remove)(const char *path) = mc_remove;

/* The tables a track's record holds, parents first, in the order they're restored. Each
 * selects the rows for track ?1. The file's own row goes last, under "files". */
static const struct {
  const char *table, *where;
} k_saved[] = {
  {"artists", "uid IN (SELECT artist_uid FROM tracks WHERE uid = ?1"
              " UNION SELECT al.artist_uid FROM tracks t JOIN albums al ON al.uid = t.album_uid"
              " WHERE t.uid = ?1)"},
  {"albums", "uid = (SELECT album_uid FROM tracks WHERE uid = ?1)"},
  {"tracks", "uid = ?1"},
  {"track_plays", "track_uid = ?1"},
  {"playlist_entries", "track_uid = ?1"},
  {"album_group_entries", "album_uid = (SELECT album_uid FROM tracks WHERE uid = ?1)"},
};

static const char k_store_schema[] =
  "CREATE TABLE IF NOT EXISTS deleted ("
  "  id          INTEGER PRIMARY KEY,"
  "  library_uid TEXT NOT NULL,"
  "  deleted_ms  INTEGER NOT NULL,"
  "  path        TEXT NOT NULL," /* absolute: where the trash puts it back */
  "  rel         TEXT NOT NULL," /* as files.path, relative to the library folder */
  "  size_bytes  INTEGER NOT NULL,"
  "  mtime_ms    INTEGER NOT NULL,"
  "  sha256      TEXT,"          /* files.sha256, if it was known */
  "  rows        TEXT NOT NULL"  /* JSON: table name -> array of row objects */
  ");"
  "CREATE INDEX IF NOT EXISTS deleted_library ON deleted (library_uid, deleted_ms);";

/* --- Small helpers --- */

static char *join(const char *dir, const char *name) {
  size_t a = strlen(dir), b = strlen(name);
  char *s = malloc(a + b + 2);
  if (!s) return NULL;
  memcpy(s, dir, a);
  s[a] = MC_PATH_SEP;
  memcpy(s + a + 1, name, b + 1);
  return s;
}

/* The folder holding path, malloc'd; NULL if it has none. */
static char *parent_dir(const char *path) {
  const char *sep = strrchr(path, MC_PATH_SEP);
  if (!sep || sep == path) return NULL;
  char *dir = malloc((size_t)(sep - path) + 1);
  if (!dir) return NULL;
  memcpy(dir, path, (size_t)(sep - path));
  dir[sep - path] = '\0';
  return dir;
}

static char *store_path(const char *config_dir) {
  return join(config_dir, DELETED_DB_NAME);
}

static int is_cover(const char *name) {
  return !strcmp(name, "cover.jpg") || !strcmp(name, "cover.png");
}

static int uid_cmp(const void *a, const void *b) {
  return strcmp(a, b);
}

/* A sorted set of uids. */
typedef struct uid_set {
  char (*items)[MC_UID_STRLEN];
  int count, cap;
} uid_set;

static mc_status set_add(uid_set *s, const char *uid) {
  if (!uid || strlen(uid) >= MC_UID_STRLEN) return MC_OK;
  if (s->count == s->cap) {
    int cap = s->cap ? s->cap * 2 : 16;
    void *more = realloc(s->items, (size_t)cap * sizeof *s->items);
    if (!more) return MC_ERR_NOMEM;
    s->items = more;
    s->cap = cap;
  }
  strcpy(s->items[s->count++], uid);
  return MC_OK;
}

/* Sorts and drops duplicates; call before set_has(). */
static void set_finish(uid_set *s) {
  if (!s->count) return;
  qsort(s->items, (size_t)s->count, sizeof *s->items, uid_cmp);
  int n = 1;
  for (int i = 1; i < s->count; i++)
    if (strcmp(s->items[i], s->items[n - 1])) memcpy(s->items[n++], s->items[i], MC_UID_STRLEN);
  s->count = n;
}

static int set_has(const uid_set *s, const char *uid) {
  return s->count && bsearch(uid, s->items, (size_t)s->count, sizeof *s->items, uid_cmp) != NULL;
}

/* A list of malloc'd strings. */
typedef struct str_list {
  char **items;
  int count, cap;
} str_list;

static mc_status list_add(str_list *l, char *s) {
  if (!s) return MC_ERR_NOMEM;
  if (l->count == l->cap) {
    int cap = l->cap ? l->cap * 2 : 16;
    void *more = realloc(l->items, (size_t)cap * sizeof *l->items);
    if (!more) {
      free(s);
      return MC_ERR_NOMEM;
    }
    l->items = more;
    l->cap = cap;
  }
  l->items[l->count++] = s;
  return MC_OK;
}

static int list_has(const str_list *l, const char *s) {
  for (int i = 0; i < l->count; i++)
    if (mc_same_path(l->items[i], s)) return 1;
  return 0;
}

static void list_free(str_list *l) {
  for (int i = 0; i < l->count; i++) free(l->items[i]);
  free(l->items);
  memset(l, 0, sizeof *l);
}

static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "musiccore: delete: %s\n", sqlite3_errmsg(db));
    return NULL;
  }
  return stmt;
}

/* Runs sql with text parameter p1 (and p2 if not NULL). */
static mc_status run(sqlite3 *db, const char *sql, const char *p1, const char *p2) {
  sqlite3_stmt *stmt = prepare(db, sql);
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, p1, -1, SQLITE_STATIC);
  if (p2) sqlite3_bind_text(stmt, 2, p2, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

/* Adds the first column of every row of sql, bound to p1, to set. */
static mc_status select_uids(sqlite3 *db, const char *sql, const char *p1, uid_set *set) {
  sqlite3_stmt *stmt = prepare(db, sql);
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, p1, -1, SQLITE_STATIC);
  mc_status st = MC_OK;
  int rc;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW)
    st = set_add(set, (const char *)sqlite3_column_text(stmt, 0));
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

static mc_status hash_file(const char *path, char out[SHA_HEX]) {
  FILE *f = mc_fopen(path, "rb");
  if (!f) return MC_ERR_IO;
  unsigned char *buf = malloc(1 << 16);
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mc_status st = buf && mbedtls_sha256_starts(&ctx, 0) == 0 ? MC_OK : MC_ERR_NOMEM;
  size_t n;
  while (st == MC_OK && (n = fread(buf, 1, 1 << 16, f)) > 0)
    if (mbedtls_sha256_update(&ctx, buf, n) != 0) st = MC_ERR_IO;
  unsigned char digest[32];
  if (st == MC_OK && (ferror(f) || mbedtls_sha256_finish(&ctx, digest) != 0)) st = MC_ERR_IO;
  for (int i = 0; st == MC_OK && i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
  mbedtls_sha256_free(&ctx);
  free(buf);
  fclose(f);
  return st;
}

/* --- Rows as JSON --- */

/* table's columns, from PRAGMA table_info. */
static mc_status table_columns(sqlite3 *db, const char *table, str_list *out) {
  char sql[128];
  snprintf(sql, sizeof sql, "SELECT name FROM pragma_table_info('%s')", table);
  sqlite3_stmt *stmt = prepare(db, sql);
  if (!stmt) return MC_ERR_DB;
  mc_status st = MC_OK;
  while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW)
    st = list_add(out, mc_strdup((const char *)sqlite3_column_text(stmt, 0)));
  sqlite3_finalize(stmt);
  return st == MC_OK && !out->count ? MC_ERR_DB : st;
}

/* The rows of table matching where (bound to p1) as a JSON array of objects, malloc'd. */
static mc_status rows_json(sqlite3 *db, const char *table, const char *where, const char *p1,
                           char **out) {
  *out = NULL;
  str_list cols = {0};
  mc_status st = table_columns(db, table, &cols);
  size_t cap = 256 + strlen(where);
  for (int i = 0; st == MC_OK && i < cols.count; i++) cap += 2 * strlen(cols.items[i]) + 8;
  char *sql = st == MC_OK ? malloc(cap) : NULL;
  if (st == MC_OK && !sql) st = MC_ERR_NOMEM;
  if (st == MC_OK) {
    size_t n = (size_t)snprintf(sql, cap, "SELECT json_group_array(json_object(");
    for (int i = 0; i < cols.count; i++)
      n += (size_t)snprintf(sql + n, cap - n, "%s'%s', \"%s\"", i ? ", " : "", cols.items[i],
                            cols.items[i]);
    snprintf(sql + n, cap - n, ")) FROM \"%s\" WHERE %s", table, where);
    sqlite3_stmt *stmt = prepare(db, sql);
    if (!stmt) st = MC_ERR_DB;
    if (stmt) sqlite3_bind_text(stmt, 1, p1, -1, SQLITE_STATIC);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0))
      *out = mc_strdup((const char *)sqlite3_column_text(stmt, 0));
    else if (stmt)
      st = MC_ERR_DB;
    if (st == MC_OK && !*out) st = MC_ERR_NOMEM;
    sqlite3_finalize(stmt);
  }
  free(sql);
  list_free(&cols);
  return st;
}

/* Appends s to the growing string *buf (length *len). */
static mc_status append(char **buf, size_t *len, const char *s) {
  size_t n = strlen(s);
  char *more = realloc(*buf, *len + n + 1);
  if (!more) return MC_ERR_NOMEM;
  memcpy(more + *len, s, n + 1);
  *buf = more;
  *len += n;
  return MC_OK;
}

/* A track's record without its closing brace: {"artists":[...],...,"album_group_entries":[...]
 * NULL (and MC_OK) if there's no such track. */
static mc_status track_json(sqlite3 *db, const char *track_uid, char **out) {
  *out = NULL;
  char *buf = NULL;
  size_t len = 0;
  mc_status st = MC_OK;
  for (size_t i = 0; st == MC_OK && i < sizeof k_saved / sizeof k_saved[0]; i++) {
    char *rows = NULL, key[64];
    st = rows_json(db, k_saved[i].table, k_saved[i].where, track_uid, &rows);
    if (st == MC_OK && !strcmp(k_saved[i].table, "tracks") && !strcmp(rows, "[]")) { /* gone */
      free(rows);
      free(buf);
      return MC_OK;
    }
    snprintf(key, sizeof key, "%s\"%s\":", i ? "," : "{", k_saved[i].table);
    if (st == MC_OK) st = append(&buf, &len, key);
    if (st == MC_OK) st = append(&buf, &len, rows);
    free(rows);
  }
  if (st != MC_OK) {
    free(buf);
    return st;
  }
  *out = buf;
  return MC_OK;
}

/* Inserts the row object obj (JSON) into table, with the columns of it the table has.
 * *inserted is whether it went in; a row that's there already, or whose parent is gone
 * (a playlist deleted since), is skipped. */
static mc_status insert_json(sqlite3 *db, const char *table, const char *obj, int *inserted) {
  *inserted = 0;
  str_list cols = {0}, keys = {0};
  mc_status st = table_columns(db, table, &cols);
  sqlite3_stmt *stmt = st == MC_OK ? prepare(db, "SELECT key FROM json_each(?1)") : NULL;
  if (st == MC_OK && !stmt) st = MC_ERR_DB;
  if (stmt) {
    sqlite3_bind_text(stmt, 1, obj, -1, SQLITE_STATIC);
    while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) {
      const char *key = (const char *)sqlite3_column_text(stmt, 0);
      for (int i = 0; key && i < cols.count; i++)
        if (!strcmp(cols.items[i], key)) st = list_add(&keys, mc_strdup(key));
    }
    sqlite3_finalize(stmt);
  }
  size_t cap = 128 + strlen(table);
  for (int i = 0; i < keys.count; i++) cap += 3 * strlen(keys.items[i]) + 32;
  char *sql = st == MC_OK && keys.count ? malloc(cap) : NULL;
  if (sql) {
    size_t n = (size_t)snprintf(sql, cap, "INSERT OR IGNORE INTO \"%s\" (", table);
    for (int i = 0; i < keys.count; i++)
      n += (size_t)snprintf(sql + n, cap - n, "%s\"%s\"", i ? ", " : "", keys.items[i]);
    n += (size_t)snprintf(sql + n, cap - n, ") VALUES (");
    for (int i = 0; i < keys.count; i++)
      n += (size_t)snprintf(sql + n, cap - n, "%sjson_extract(?1, '$.\"%s\"')", i ? ", " : "",
                            keys.items[i]);
    snprintf(sql + n, cap - n, ")");
    if ((stmt = prepare(db, sql))) {
      sqlite3_bind_text(stmt, 1, obj, -1, SQLITE_STATIC);
      int rc = sqlite3_step(stmt);
      if (rc == SQLITE_DONE) *inserted = sqlite3_changes(db) > 0;
      else if ((rc & 0xff) != SQLITE_CONSTRAINT) st = MC_ERR_DB;
      sqlite3_finalize(stmt);
    } else {
      st = MC_ERR_DB;
    }
  } else if (st == MC_OK && keys.count) {
    st = MC_ERR_NOMEM;
  }
  free(sql);
  list_free(&keys);
  list_free(&cols);
  return st;
}

/* Puts back every row of record rows (JSON), the file's own row with its path set to rel.
 * In the caller's transaction. MC_ERR_NOT_FOUND if the file's row couldn't go in. */
static mc_status restore_rows(sqlite3 *db, const char *rows, const char *rel) {
  mc_status st = MC_OK;
  size_t ntables = sizeof k_saved / sizeof k_saved[0];
  for (size_t i = 0; st == MC_OK && i <= ntables; i++) {
    const char *table = i < ntables ? k_saved[i].table : "files";
    char path[80];
    snprintf(path, sizeof path, "$.%s", table);
    sqlite3_stmt *stmt = prepare(db, i < ntables ? "SELECT value FROM json_each(?1, ?2)"
                                                 : "SELECT json_set(value, '$.path', ?3)"
                                                   " FROM json_each(?1, ?2)");
    if (!stmt) return MC_ERR_DB;
    sqlite3_bind_text(stmt, 1, rows, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, path, -1, SQLITE_TRANSIENT);
    if (i == ntables) sqlite3_bind_text(stmt, 3, rel, -1, SQLITE_STATIC);
    str_list objs = {0};
    while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW)
      st = list_add(&objs, mc_strdup((const char *)sqlite3_column_text(stmt, 0)));
    sqlite3_finalize(stmt);
    int filed = 0;
    for (int j = 0; st == MC_OK && j < objs.count; j++) {
      int inserted;
      st = insert_json(db, table, objs.items[j], &inserted);
      filed |= inserted;
    }
    list_free(&objs);
    if (st == MC_OK && i == ntables && !filed) st = MC_ERR_NOT_FOUND;
  }
  return st;
}

/* --- The store --- */

static mc_status store_open(const char *config_dir, int create, sqlite3 **out) {
  *out = NULL;
  char *path = store_path(config_dir);
  if (!path) return MC_ERR_NOMEM;
  if (!create && !mc_file_exists(path)) {
    free(path);
    return MC_ERR_NOT_FOUND;
  }
  if (create && mc_mkdirs(config_dir) != MC_OK) {
    free(path);
    return MC_ERR_IO;
  }
  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                           NULL);
  free(path);
  if (rc == SQLITE_OK) {
    sqlite3_busy_timeout(db, 5000);
    rc = sqlite3_exec(db, "PRAGMA trusted_schema = OFF;", NULL, NULL, NULL);
  }
  if (rc == SQLITE_OK) rc = sqlite3_exec(db, k_store_schema, NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    fprintf(stderr, "musiccore: %s open failed: %s\n", DELETED_DB_NAME, db ? sqlite3_errmsg(db) : "?");
    sqlite3_close_v2(db);
    return MC_ERR_DB;
  }
  *out = db;
  return MC_OK;
}

/* --- Plans --- */

struct mc_delete_plan {
  uid_set tracks;
  int files;
};

/* Appends the files of track_uid that exist here to files (absolute paths). */
static mc_status track_files(mc_library *lib, const char *track_uid, str_list *files) {
  sqlite3_stmt *stmt = prepare(lib->db, "SELECT path FROM files WHERE track_uid = ?1");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, track_uid, -1, SQLITE_STATIC);
  mc_status st = MC_OK;
  while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) {
    char *abs = mc_library_abspath(lib, (const char *)sqlite3_column_text(stmt, 0));
    if (abs && !mc_file_exists(abs)) {
      free(abs);
      continue;
    }
    st = list_add(files, abs);
  }
  sqlite3_finalize(stmt);
  return st;
}

typedef struct folder_scan {
  const char *dir;
  const str_list *going; /* files that are going anyway */
  int covers, others;
} folder_scan;

static void scan_entry(void *user, const char *name, int is_dir) {
  folder_scan *f = user;
  if (!is_dir && is_cover(name)) {
    f->covers++;
    return;
  }
  char *path = is_dir || !f->going ? NULL : join(f->dir, name);
  if (!path || !list_has(f->going, path)) f->others++;
  free(path);
}

/* Whether dir is one of the library's own folders (not the library folder itself). */
static int inside_library(mc_library *lib, const char *dir) {
  char *rel = mc_library_relpath(lib, dir);
  int inside = rel != NULL;
  free(rel);
  return inside;
}

/* The covers in dir that go once going (or, with NULL, what's gone already) has: all of
 * them, if nothing else is left there. */
static int covers_left(mc_library *lib, const char *dir, const str_list *going) {
  if (!inside_library(lib, dir)) return 0;
  folder_scan f = {dir, going, 0, 0};
  if (mc_list_dir(dir, scan_entry, &f) != MC_OK || f.others) return 0;
  return f.covers;
}

/* The distinct folders of files. */
static mc_status folders_of(const str_list *files, str_list *out) {
  for (int i = 0; i < files->count; i++) {
    char *dir = parent_dir(files->items[i]);
    if (dir && list_has(out, dir)) {
      free(dir);
      continue;
    }
    mc_status st = list_add(out, dir);
    if (st != MC_OK) return st;
  }
  return MC_OK;
}

mc_status mc_delete_plan_new(mc_library *lib, mc_delete_kind kind, const char *const *uids,
                             int count, mc_delete_plan **out) {
  if (!lib || !out || count < 0 || (count && !uids)) return MC_ERR_INVALID_ARG;
  *out = NULL;
  static const char *const k_select[] = {
    "SELECT uid FROM tracks WHERE uid = ?1",
    "SELECT uid FROM tracks WHERE album_uid = ?1",
    "SELECT uid FROM tracks WHERE artist_uid = ?1"
    " OR album_uid IN (SELECT uid FROM albums WHERE artist_uid = ?1)",
  };
  if ((int)kind < 0 || (int)kind > MC_DELETE_ARTISTS) return MC_ERR_INVALID_ARG;
  mc_delete_plan *plan = calloc(1, sizeof *plan);
  if (!plan) return MC_ERR_NOMEM;
  mc_status st = MC_OK;
  for (int i = 0; st == MC_OK && i < count; i++)
    if (uids[i]) st = select_uids(lib->db, k_select[kind], uids[i], &plan->tracks);
  set_finish(&plan->tracks);

  str_list files = {0}, dirs = {0};
  for (int i = 0; st == MC_OK && i < plan->tracks.count; i++)
    st = track_files(lib, plan->tracks.items[i], &files);
  if (st == MC_OK) st = folders_of(&files, &dirs);
  plan->files = files.count;
  for (int i = 0; st == MC_OK && i < dirs.count; i++)
    plan->files += covers_left(lib, dirs.items[i], &files);
  list_free(&files);
  list_free(&dirs);
  if (st != MC_OK) {
    mc_delete_plan_free(plan);
    return st;
  }
  *out = plan;
  return MC_OK;
}

void mc_delete_plan_free(mc_delete_plan *plan) {
  if (!plan) return;
  free(plan->tracks.items);
  free(plan);
}

int mc_delete_plan_tracks(const mc_delete_plan *plan) {
  return plan ? plan->tracks.count : 0;
}

int mc_delete_plan_files(const mc_delete_plan *plan) {
  return plan ? plan->files : 0;
}

int mc_delete_plan_has(const mc_delete_plan *plan, const char *track_uid) {
  return plan && track_uid && set_has(&plan->tracks, track_uid);
}

/* --- Deleting --- */

/* Saves the record of a trashed file. */
static mc_status save_record(sqlite3 *store, mc_library *lib, const char *abs, const char *rel,
                             int64_t size, int64_t mtime, const char *sha, const char *rows) {
  sqlite3_stmt *stmt = prepare(store, "INSERT INTO deleted (library_uid, deleted_ms, path, rel,"
                                      " size_bytes, mtime_ms, sha256, rows)"
                                      " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, mc_library_uid(lib), -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 2, (sqlite3_int64)mc_wall_ms());
  sqlite3_bind_text(stmt, 3, abs, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, rel, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 5, size);
  sqlite3_bind_int64(stmt, 6, mtime);
  if (sha && strlen(sha) == SHA_HEX - 1) sqlite3_bind_text(stmt, 7, sha, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 8, rows, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

/* A file's record: its track's (track_rows, from track_json()) with its own row added under
 * "files". malloc'd, or NULL if it couldn't be made. */
static char *record_json(sqlite3 *db, const char *track_rows, const char *file_uid) {
  char *file_rows = NULL, *rows = NULL;
  size_t len = 0;
  int ok = rows_json(db, "files", "uid = ?1", file_uid, &file_rows) == MC_OK &&
           append(&rows, &len, track_rows) == MC_OK && append(&rows, &len, ",\"files\":") == MC_OK &&
           append(&rows, &len, file_rows) == MC_OK && append(&rows, &len, "}") == MC_OK;
  free(file_rows);
  if (!ok) {
    free(rows);
    return NULL;
  }
  return rows;
}

/* Trashes track_uid's files, saving a record of each to store (which may be NULL); or if
 * permanent, removes them and records nothing. Their folders are added to dirs, and the
 * uids of those gone to trashed. *kept is set if one couldn't go. */
static mc_status trash_track(mc_library *lib, sqlite3 *store, int permanent,
                             const char *track_uid, const char *track_rows, str_list *dirs,
                             uid_set *trashed, int *files_trashed, int *kept) {
  *kept = 0;
  sqlite3_stmt *stmt = prepare(lib->db, "SELECT uid, path, sha256 FROM files WHERE track_uid = ?1");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, track_uid, -1, SQLITE_STATIC);
  mc_status st = MC_OK;
  while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    const char *rel = (const char *)sqlite3_column_text(stmt, 1);
    const char *sha = (const char *)sqlite3_column_text(stmt, 2);
    char *abs = uid && rel ? mc_library_abspath(lib, rel) : NULL;
    int64_t size = 0, mtime = 0;
    if (!abs || mc_file_info(abs, &size, &mtime) != MC_OK) { /* not here: only the row goes */
      free(abs);
      continue;
    }
    char *rows = store ? record_json(lib->db, track_rows, uid) : NULL;
    if ((permanent ? mc_delete_remove : mc_delete_trash)(abs) != MC_OK) {
      fprintf(stderr, "musiccore: delete: couldn't %s %s%s; kept\n", permanent ? "remove" : "move",
              abs, permanent ? "" : " to the trash");
      *kept = 1;
    } else {
      (*files_trashed)++;
      st = set_add(trashed, uid);
      if (st == MC_OK && !permanent &&
          (!rows || save_record(store, lib, abs, rel, size, mtime, sha, rows) != MC_OK))
        fprintf(stderr, "musiccore: delete: couldn't save how to restore %s\n", rel);
      char *dir = parent_dir(abs);
      if (st == MC_OK && dir && !list_has(dirs, dir)) st = list_add(dirs, dir);
      else free(dir);
    }
    free(rows);
    free(abs);
  }
  sqlite3_finalize(stmt);
  return st;
}

/* Removes the rows of the tracks in gone, the trashed files of the tracks that stay, and
 * the albums and artists that leaves empty. In one transaction. */
static mc_status delete_rows(mc_library *lib, const uid_set *gone, const uid_set *trashed,
                             const uid_set *albums, const uid_set *artists) {
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  sqlite3 *db = lib->db;
  /* The children one by one rather than by cascade, so each delete is plainly logged. */
  for (int i = 0; st == MC_OK && i < gone->count; i++) {
    const char *t = gone->items[i];
    if ((st = run(db, "DELETE FROM playlist_entries WHERE track_uid = ?1", t, NULL)) == MC_OK &&
        (st = run(db, "DELETE FROM track_plays WHERE track_uid = ?1", t, NULL)) == MC_OK &&
        (st = run(db, "DELETE FROM files WHERE track_uid = ?1", t, NULL)) == MC_OK)
      st = run(db, "DELETE FROM tracks WHERE uid = ?1", t, NULL);
  }
  for (int i = 0; st == MC_OK && i < trashed->count; i++)
    st = run(db, "DELETE FROM files WHERE uid = ?1", trashed->items[i], NULL);
  for (int i = 0; st == MC_OK && i < albums->count; i++) {
    const char *a = albums->items[i];
    const char *empty = " AND NOT EXISTS (SELECT 1 FROM tracks WHERE album_uid = ?1)";
    char sql[256];
    snprintf(sql, sizeof sql, "DELETE FROM album_group_entries WHERE album_uid = ?1%s", empty);
    if ((st = run(db, sql, a, NULL)) != MC_OK) break;
    snprintf(sql, sizeof sql, "DELETE FROM albums WHERE uid = ?1%s", empty);
    st = run(db, sql, a, NULL);
  }
  for (int i = 0; st == MC_OK && i < artists->count; i++)
    st = run(db,
             "DELETE FROM artists WHERE uid = ?1"
             " AND NOT EXISTS (SELECT 1 FROM tracks WHERE artist_uid = ?1)"
             " AND NOT EXISTS (SELECT 1 FROM albums WHERE artist_uid = ?1)",
             artists->items[i], NULL);
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) {
    fprintf(stderr, "musiccore: delete failed: %s\n", sqlite3_errmsg(db));
    mc_library_rollback(lib);
  }
  return st;
}

/* After its files went: a folder left with only a cover loses the cover (to the trash, or if
 * permanent removed) and goes, and then its parent (the artist's folder) if that's empty. */
static int tidy_folder(mc_library *lib, const char *dir, int permanent) {
  int trashed = 0;
  if (covers_left(lib, dir, NULL)) {
    const char *covers[] = {"cover.jpg", "cover.png"};
    for (int i = 0; i < 2; i++) {
      char *c = join(dir, covers[i]);
      if (c && mc_file_exists(c) && (permanent ? mc_delete_remove : mc_delete_trash)(c) == MC_OK)
        trashed++;
      free(c);
    }
  }
  if (inside_library(lib, dir) && mc_rmdir(dir) == MC_OK) {
    char *parent = parent_dir(dir);
    if (parent && inside_library(lib, parent)) mc_rmdir(parent); /* fails unless empty */
    free(parent);
  }
  return trashed;
}

/* mc_delete_run(), or with config_dir NULL mc_delete_run_permanently(): files are removed
 * rather than trashed, and nothing is recorded for restoring. */
static mc_status delete_run(mc_library *lib, const char *config_dir, const mc_delete_plan *plan,
                            mc_delete_result *out) {
  int permanent = !config_dir;
  sqlite3 *store = NULL;
  if (!permanent && plan->tracks.count && store_open(config_dir, 1, &store) != MC_OK)
    fprintf(stderr, "musiccore: delete: deleted tracks can't be restored from the trash\n");
  if (store) sqlite3_exec(store, "BEGIN", NULL, NULL, NULL);

  uid_set gone = {0}, trashed = {0}, albums = {0}, artists = {0};
  str_list dirs = {0};
  mc_delete_result res = {0};
  mc_status st = MC_OK;
  for (int i = 0; st == MC_OK && i < plan->tracks.count; i++) {
    const char *t = plan->tracks.items[i];
    char *rows = NULL;
    if ((st = track_json(lib->db, t, &rows)) != MC_OK) break;
    if (!rows) continue; /* deleted since */
    st = select_uids(lib->db, "SELECT album_uid FROM tracks WHERE uid = ?1", t, &albums);
    if (st == MC_OK)
      st = select_uids(lib->db,
                       "SELECT artist_uid FROM tracks WHERE uid = ?1 UNION"
                       " SELECT al.artist_uid FROM tracks t JOIN albums al ON al.uid = t.album_uid"
                       " WHERE t.uid = ?1",
                       t, &artists);
    int kept = 0;
    if (st == MC_OK)
      st = trash_track(lib, store, permanent, t, rows, &dirs, &trashed, &res.files_trashed, &kept);
    free(rows);
    if (st == MC_OK && kept) res.tracks_kept++;
    else if (st == MC_OK) st = set_add(&gone, t);
  }
  if (store) {
    sqlite3_exec(store, "COMMIT", NULL, NULL, NULL);
    sqlite3_close_v2(store);
  }
  set_finish(&gone);
  set_finish(&trashed);
  set_finish(&albums);
  set_finish(&artists);

  /* Rows go even after an error above, for the files already trashed. */
  mc_status write = delete_rows(lib, &gone, &trashed, &albums, &artists);
  if (st == MC_OK) st = write;
  if (write == MC_OK) {
    res.tracks_deleted = gone.count;
    for (int i = 0; i < dirs.count; i++)
      res.files_trashed += tidy_folder(lib, dirs.items[i], permanent);
  }
  free(gone.items);
  free(trashed.items);
  free(albums.items);
  free(artists.items);
  list_free(&dirs);
  if (out) *out = res;
  return st;
}

mc_status mc_delete_run(mc_library *lib, const char *config_dir, const mc_delete_plan *plan,
                        mc_delete_result *out) {
  if (out) memset(out, 0, sizeof *out);
  if (!lib || !config_dir || !plan) return MC_ERR_INVALID_ARG;
  return delete_run(lib, config_dir, plan, out);
}

mc_status mc_delete_run_permanently(mc_library *lib, const mc_delete_plan *plan,
                                    mc_delete_result *out) {
  if (out) memset(out, 0, sizeof *out);
  if (!lib || !plan) return MC_ERR_INVALID_ARG;
  return delete_run(lib, NULL, plan, out);
}

/* --- Restoring --- */

/* Whether the file at path is the one recorded. */
static int same_file(const char *path, int64_t size, int64_t mtime, const char *sha) {
  int64_t s = 0, m = 0;
  if (mc_file_info(path, &s, &m) != MC_OK || s != size) return 0;
  if (m - mtime <= MTIME_SLACK_MS && mtime - m <= MTIME_SLACK_MS) return 1;
  char got[SHA_HEX];
  return sha && hash_file(path, got) == MC_OK && !strcmp(got, sha);
}

/* Where the recorded file is now, if it's back: at its old absolute path, or at its place in
 * the library (should the library have moved since). One back outside the library is moved
 * to its place inside. Returns its path relative to the library, malloc'd, or NULL. */
static char *find_restored(mc_library *lib, const char *abs, const char *rel, int64_t size,
                           int64_t mtime, const char *sha) {
  char *here = mc_library_abspath(lib, rel);
  if (!here) return NULL;
  char *found = NULL;
  if (same_file(abs, size, mtime, sha)) {
    found = mc_library_relpath(lib, abs);
    if (!found && !mc_file_exists(here)) {
      char *dir = parent_dir(here);
      if (dir && mc_mkdirs(dir) == MC_OK && mc_rename(abs, here, 0) == MC_OK) found = mc_strdup(rel);
      free(dir);
    }
  } else if (!mc_same_path(abs, here) && same_file(here, size, mtime, sha)) {
    found = mc_strdup(rel);
  }
  free(here);
  return found;
}

static int path_known(mc_library *lib, const char *rel) {
  sqlite3_stmt *stmt = prepare(lib->db, "SELECT 1 FROM files WHERE path = ?1");
  if (!stmt) return 0;
  sqlite3_bind_text(stmt, 1, rel, -1, SQLITE_STATIC);
  int known = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return known;
}

typedef struct record {
  int64_t id;
  char *path, *rel, *sha, *rows;
  int64_t size, mtime;
} record;

static void record_free(record *r) {
  free(r->path);
  free(r->rel);
  free(r->sha);
  free(r->rows);
}

/* The library's records, newest first. */
static mc_status read_records(sqlite3 *store, mc_library *lib, record **out, int *count) {
  *out = NULL;
  *count = 0;
  sqlite3_stmt *stmt = prepare(store, "SELECT id, path, rel, size_bytes, mtime_ms, sha256, rows"
                                      " FROM deleted WHERE library_uid = ?1"
                                      " ORDER BY deleted_ms DESC, id DESC");
  if (!stmt) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, mc_library_uid(lib), -1, SQLITE_STATIC);
  record *recs = NULL;
  int n = 0, cap = 0;
  mc_status st = MC_OK;
  while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) {
    if (n == cap) {
      cap = cap ? cap * 2 : 16;
      record *more = realloc(recs, (size_t)cap * sizeof *recs);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      recs = more;
    }
    record *r = &recs[n++];
    r->id = sqlite3_column_int64(stmt, 0);
    r->path = mc_strdup((const char *)sqlite3_column_text(stmt, 1));
    r->rel = mc_strdup((const char *)sqlite3_column_text(stmt, 2));
    r->size = sqlite3_column_int64(stmt, 3);
    r->mtime = sqlite3_column_int64(stmt, 4);
    r->sha = mc_strdup((const char *)sqlite3_column_text(stmt, 5));
    r->rows = mc_strdup((const char *)sqlite3_column_text(stmt, 6));
    if (!r->path || !r->rel || !r->rows) st = MC_ERR_NOMEM;
  }
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    for (int i = 0; i < n; i++) record_free(&recs[i]);
    free(recs);
    return st;
  }
  *out = recs;
  *count = n;
  return MC_OK;
}

/* Drops the records of the file at rel: once it's back, older ones are moot too. */
static void forget(sqlite3 *store, mc_library *lib, const char *rel) {
  run(store, "DELETE FROM deleted WHERE library_uid = ?1 AND rel = ?2", mc_library_uid(lib), rel);
}

/* Whether the track a record's rows are for is in the library: then its file coming back
 * (another of its files did already) brings back no track. */
static int track_present(sqlite3 *db, const char *rows) {
  sqlite3_stmt *stmt =
    prepare(db, "SELECT 1 FROM tracks WHERE uid = json_extract(?1, '$.tracks[0].uid')");
  if (!stmt) return 0;
  sqlite3_bind_text(stmt, 1, rows, -1, SQLITE_STATIC);
  int present = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return present;
}

mc_status mc_delete_restore(mc_library *lib, const char *config_dir, int *restored, int *tracks) {
  if (restored) *restored = 0;
  if (tracks) *tracks = 0;
  if (!lib || !config_dir) return MC_ERR_INVALID_ARG;
  sqlite3 *store;
  mc_status st = store_open(config_dir, 0, &store);
  if (st == MC_ERR_NOT_FOUND) return MC_OK;
  if (st != MC_OK) return st;

  sqlite3_stmt *stmt = prepare(store, "DELETE FROM deleted WHERE deleted_ms < ?1");
  if (stmt) {
    sqlite3_bind_int64(stmt, 1, (sqlite3_int64)mc_wall_ms() - MC_DELETE_KEEP_DAYS * DAY_MS);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  record *recs;
  int n;
  if ((st = read_records(store, lib, &recs, &n)) != MC_OK) {
    sqlite3_close_v2(store);
    return st;
  }
  for (int i = 0; st == MC_OK && i < n; i++) {
    record *r = &recs[i];
    char *rel = find_restored(lib, r->path, r->rel, r->size, r->mtime, r->sha);
    if (!rel) continue;
    if (path_known(lib, rel)) { /* imported again, or restored already */
      forget(store, lib, r->rel);
      free(rel);
      continue;
    }
    if ((st = mc_library_begin_write(lib)) != MC_OK) {
      free(rel);
      break;
    }
    int had_track = track_present(lib->db, r->rows);
    mc_status put = restore_rows(lib->db, r->rows, rel);
    if (put == MC_OK) put = mc_library_commit(lib);
    if (put == MC_OK) {
      if (restored) (*restored)++;
      if (tracks && !had_track) (*tracks)++;
      forget(store, lib, r->rel);
    } else {
      fprintf(stderr, "musiccore: couldn't restore %s: %s\n", rel, sqlite3_errmsg(lib->db));
      mc_library_rollback(lib);
      if (put != MC_ERR_NOT_FOUND) st = put;
    }
    free(rel);
  }
  for (int i = 0; i < n; i++) record_free(&recs[i]);
  free(recs);
  sqlite3_close_v2(store);
  return st;
}
