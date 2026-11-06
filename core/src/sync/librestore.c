/* Abandoning a merge: while the merge of the set-aside library still waits for Sync now, the
 * app restarting, or this device leaving or being removed from the kept group, undoes the
 * pairing and puts the device back as it was before it. See libmerge.h ("Abandoning a
 * merge") and docs/sync-protocol.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "library/folder_gate.h"
#include "sync/libmerge.h"
#include "util/db_open.h"
#include "util/platform.h"

/* In the library folder while the set-aside copy is brought up to date to be put back. */
#define RESTORE_TMP MC_LIBMERGE_RESTORE_DIR

/* --- This run --- */

static char g_run[MC_UID_STRLEN];

/* The restore and the run's token, process-wide: several connections may open the library
 * at once at startup, each on its own thread. Not recursive. */
static sqlite3_mutex *restore_lock(void) {
  return sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_APP1);
}

static void new_run(void) {
  if (mc_uid_new(g_run) != MC_OK)
    snprintf(g_run, sizeof g_run, "run-%llu", (unsigned long long)mc_mono_ms());
}

const char *mc_libmerge_run_token(void) {
  sqlite3_mutex *m = restore_lock();
  sqlite3_mutex_enter(m);
  if (!g_run[0]) new_run();
  sqlite3_mutex_leave(m);
  return g_run;
}

void mc_libmerge_test_new_run(void) {
  sqlite3_mutex *m = restore_lock();
  sqlite3_mutex_enter(m);
  new_run();
  sqlite3_mutex_leave(m);
}

/* --- Helpers --- */

static char *join(const char *dir, const char *name) {
  size_t n = strlen(dir) + strlen(name) + 2;
  char *p = malloc(n);
  if (p) snprintf(p, n, "%s%c%s", dir, MC_PATH_SEP, name);
  return p;
}

static mc_status exec_sql(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? MC_OK : MC_ERR_DB;
}

static mc_status attach(sqlite3 *db, const char *path) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "ATTACH DATABASE ?1 AS bak", -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  mc_db_bind_path(stmt, 1, path);
  mc_status st = sqlite3_step(stmt) == SQLITE_DONE ? MC_OK : MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) fprintf(stderr, "kithcore: merge: restore: %s\n", sqlite3_errmsg(db));
  return st;
}

/* The columns of table that both main and bak have, in main's order, quoted and joined with
 * commas; "" if none. sqlite3_malloc'd, or NULL. */
static char *shared_columns(sqlite3 *db, const char *table) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT m.name FROM pragma_table_info(?1, 'main') m"
                         " WHERE m.name IN (SELECT b.name FROM pragma_table_info(?1, 'bak') b)"
                         " ORDER BY m.cid",
                         -1, &stmt, NULL) != SQLITE_OK)
    return NULL;
  sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  sqlite3_str *s = sqlite3_str_new(db);
  int n = 0, rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    sqlite3_str_appendf(s, "%s\"%w\"", n++ ? ", " : "", (const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  char *cols = sqlite3_str_finish(s);
  if (rc != SQLITE_DONE) {
    sqlite3_free(cols);
    return NULL;
  }
  return cols ? cols : sqlite3_mprintf("");
}

/* In the caller's transaction, with logging off: every table of main gets the rows the same
 * table has in bak, in the columns both have (a column the copy lacks takes its default; a
 * table it lacks is left empty). Every table is emptied before any is filled, so a cascading
 * delete can't take rows already put back; foreign keys are checked at commit. */
static mc_status replace_tables(sqlite3 *db) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT name FROM main.sqlite_master WHERE type = 'table'"
                         " AND name NOT LIKE 'sqlite!_%' ESCAPE '!' ORDER BY name",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  char **names = NULL;
  int n = 0;
  mc_status st = MC_OK;
  while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) {
    char **more = realloc(names, (size_t)(n + 1) * sizeof *more);
    if (!more) {
      st = MC_ERR_NOMEM;
      break;
    }
    names = more;
    if (!(names[n] = mc_strdup((const char *)sqlite3_column_text(stmt, 0))))
      st = MC_ERR_NOMEM;
    else
      n++;
  }
  sqlite3_finalize(stmt);
  if (st == MC_OK) st = exec_sql(db, "PRAGMA defer_foreign_keys = ON");
  for (int i = 0; i < n && st == MC_OK; i++) {
    char *sql = sqlite3_mprintf("DELETE FROM main.\"%w\"", names[i]);
    st = sql ? exec_sql(db, sql) : MC_ERR_NOMEM;
    sqlite3_free(sql);
  }
  for (int i = 0; i < n && st == MC_OK; i++) {
    char *cols = shared_columns(db, names[i]);
    if (!cols) {
      st = MC_ERR_DB;
      break;
    }
    if (cols[0]) {
      char *sql = sqlite3_mprintf("INSERT INTO main.\"%w\" (%s) SELECT %s FROM bak.\"%w\"",
                                  names[i], cols, cols, names[i]);
      st = sql ? exec_sql(db, sql) : MC_ERR_NOMEM;
      if (st != MC_OK)
        fprintf(stderr, "kithcore: merge: restore: %s: %s\n", names[i], sqlite3_errmsg(db));
      sqlite3_free(sql);
    }
    sqlite3_free(cols);
  }
  for (int i = 0; i < n; i++) free(names[i]);
  free(names);
  return st;
}

/* Commits, or rolls back if that fails (a foreign key the copy breaks). */
static mc_status commit_or_rollback(sqlite3 *db, mc_status st) {
  if (st == MC_OK) st = exec_sql(db, "COMMIT");
  if (st != MC_OK) exec_sql(db, "ROLLBACK");
  return st;
}

/* --- sync.db --- */

/* Copies sdb to <dir>/sync.db by way of "<dir>.part", so dir only appears whole. */
static mc_status copy_sync_db(mc_sync_db *sdb, const char *dir) {
  size_t n = strlen(dir) + 6;
  char *part = malloc(n);
  if (!part) return MC_ERR_NOMEM;
  snprintf(part, n, "%s.part", dir);
  char *path = join(part, MC_SYNC_DB_NAME);
  mc_remove_tree(part);
  mc_status st = path ? mc_mkdirs(part) : MC_ERR_NOMEM;
  sqlite3_stmt *stmt = NULL;
  if (st == MC_OK &&
      sqlite3_prepare_v2(mc_sync_db_handle(sdb), "VACUUM INTO ?1", -1, &stmt, NULL) != SQLITE_OK)
    st = MC_ERR_DB;
  if (st == MC_OK) {
    mc_db_bind_path(stmt, 1, path);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      fprintf(stderr, "kithcore: merge: couldn't copy sync.db aside: %s\n",
              sqlite3_errmsg(mc_sync_db_handle(sdb)));
      st = MC_ERR_IO;
    }
  }
  sqlite3_finalize(stmt);
  if (st == MC_OK) {
    mc_remove_tree(dir);
    st = mc_rename_in_place(part, dir);
  }
  if (st != MC_OK) mc_remove_tree(part);
  free(path);
  free(part);
  return st;
}

static void remove_with_part(const char *dir) {
  size_t n = strlen(dir) + 6;
  char *part = malloc(n);
  if (part) {
    snprintf(part, n, "%s.part", dir);
    mc_remove_tree(part);
    free(part);
  }
  mc_remove_tree(dir);
}

mc_status mc_libmerge_backup_sync(mc_sync_db *sdb, const char *config_dir) {
  if (!sdb || !config_dir) return MC_ERR_INVALID_ARG;
  char *dir = join(config_dir, MC_LIBMERGE_SYNC_BACKUP_DIR);
  mc_status st = dir ? copy_sync_db(sdb, dir) : MC_ERR_NOMEM;
  free(dir);
  return st;
}

/* sdb's contents replaced by the copy's at path (brought up to date by the caller), in one
 * unlogged transaction. */
static mc_status put_back_sync(mc_sync_db *sdb, const char *path) {
  sqlite3 *db = mc_sync_db_handle(sdb);
  mc_status st = attach(db, path);
  if (st != MC_OK) return st;
  st = exec_sql(db, "BEGIN IMMEDIATE");
  if (st == MC_OK) {
    sdb->applying_remote = 1;
    st = replace_tables(db);
    sdb->applying_remote = 0;
    st = commit_or_rollback(db, st);
  }
  exec_sql(db, "DETACH DATABASE bak");
  return st;
}

/* The sync.db copy in dir, opened (which brings it up to this version's schema) and closed
 * again; group_uid gets its group, "" if none, and *session_key (may be NULL) its scrobbling
 * session key, malloc'd, NULL if signed out. */
static mc_status ready_copy(const char *dir, char group_uid[MC_UID_STRLEN], char **session_key) {
  group_uid[0] = '\0';
  if (session_key) *session_key = NULL;
  mc_sync_db *copy = NULL;
  mc_status st = mc_sync_db_open(dir, &copy);
  if (st != MC_OK) return st;
  const char *g = mc_sync_db_group_uid(copy);
  snprintf(group_uid, MC_UID_STRLEN, "%s", g ? g : "");
  sqlite3_stmt *stmt;
  if (session_key &&
      sqlite3_prepare_v2(mc_sync_db_handle(copy),
                         "SELECT session_key FROM scrobble_account WHERE uid = '"
                         MC_SCROBBLE_ACCOUNT_UID "'",
                         -1, &stmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0))
      *session_key = mc_strdup((const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
  }
  mc_sync_db_close(copy);
  return MC_OK;
}

mc_status mc_libmerge_unprepare_sync(mc_sync_db *sdb, const char *config_dir) {
  if (!sdb || !config_dir) return MC_ERR_INVALID_ARG;
  char *dir = join(config_dir, MC_LIBMERGE_SYNC_BACKUP_DIR);
  char *path = dir ? join(dir, MC_SYNC_DB_NAME) : NULL;
  char group[MC_UID_STRLEN];
  mc_status st = !path ? MC_ERR_NOMEM : mc_file_exists(path) ? MC_OK : MC_ERR_NOT_FOUND;
  if (st == MC_OK) st = ready_copy(dir, group, NULL);
  if (st == MC_OK) st = put_back_sync(sdb, path);
  if (st == MC_OK) remove_with_part(dir);
  free(path);
  free(dir);
  return st;
}

/* Signs the group's scrobbling account out (logged, as a sign-out is) if it's still the one
 * with session_key (may be NULL: nothing to do). */
static mc_status sign_out_brought(mc_sync_db *sdb, const char *session_key) {
  if (!session_key) return MC_OK;
  mc_status st = mc_sync_db_begin_write(sdb);
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt;
  st = MC_ERR_DB;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "UPDATE scrobble_account SET session_key = NULL, username = NULL"
                         " WHERE uid = '" MC_SCROBBLE_ACCOUNT_UID "' AND session_key = ?1",
                         -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, session_key, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_DONE) st = MC_OK;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) mc_sync_db_rollback(sdb);
  return st;
}

mc_status mc_libmerge_sign_out_brought(mc_library *lib, mc_sync_db *sdb, const char *config_dir) {
  if (!lib || !sdb || !config_dir) return MC_ERR_INVALID_ARG;
  if (!mc_libmerge_waiting(lib) || !mc_libmerge_pending(lib, NULL)) return MC_OK;
  char *dir = join(config_dir, MC_LIBMERGE_SYNC_BACKUP_DIR);
  char *path = dir ? join(dir, MC_SYNC_DB_NAME) : NULL;
  char group[MC_UID_STRLEN], *key = NULL;
  mc_status st = path ? MC_OK : MC_ERR_NOMEM;
  if (st == MC_OK && mc_file_exists(path)) st = ready_copy(dir, group, &key);
  if (st == MC_OK) st = sign_out_brought(sdb, key);
  free(key);
  free(path);
  free(dir);
  return st;
}

/* In the sync.db l of a leave notice: this device marked removed, logged, as
 * mc_sync_leave_group() marks it. *others: the members left to tell; none, and nothing is done. */
static mc_status mark_left(mc_sync_db *l, int *others) {
  sqlite3 *db = mc_sync_db_handle(l);
  sqlite3_stmt *stmt;
  *others = 0;
  if (sqlite3_prepare_v2(db, "SELECT count(*) FROM devices WHERE removed = 0 AND uid <> ?1", -1,
                         &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(l), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) *others = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  if (*others == 0) return MC_OK;
  mc_status st = mc_sync_db_begin_write(l);
  if (st != MC_OK) return st;
  st = MC_ERR_DB;
  if (sqlite3_prepare_v2(db, "UPDATE devices SET removed = 1 WHERE uid = ?1", -1, &stmt, NULL) ==
      SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(l), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_DONE) st = MC_OK;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK) st = mc_sync_db_commit(l);
  if (st != MC_OK) mc_sync_db_rollback(l);
  return st;
}

int mc_libmerge_leave_old_group(const char *config_dir) {
  char *bdir = config_dir ? join(config_dir, MC_LIBMERGE_SYNC_BACKUP_DIR) : NULL;
  char *ldir = config_dir ? join(config_dir, MC_LIBMERGE_LEAVE_DIR) : NULL;
  char *bpath = bdir ? join(bdir, MC_SYNC_DB_NAME) : NULL;
  char *lpath = ldir ? join(ldir, MC_SYNC_DB_NAME) : NULL;
  int waits = 0;
  if (bpath && lpath) {
    char group[MC_UID_STRLEN] = "";
    waits = mc_file_exists(lpath); /* a pairing undone earlier: that notice goes first */
    if (!waits && mc_file_exists(bpath) && ready_copy(bdir, group, NULL) == MC_OK && group[0]) {
      mc_sync_db *l = NULL;
      int others = 0;
      mc_status st = mc_sync_db_open(bdir, &l);
      if (st == MC_OK) st = mark_left(l, &others);
      mc_sync_db_close(l);
      if (st == MC_OK && others > 0) {
        mc_remove_tree(ldir);
        waits = mc_rename_in_place(bdir, ldir) == MC_OK;
      }
    }
  }
  if (bdir) remove_with_part(bdir);
  free(lpath);
  free(bpath);
  free(ldir);
  free(bdir);
  return waits;
}

/* The kept group's sync.db, copied to MC_LIBMERGE_LEAVE_DIR with this device marked removed
 * there (logged, as mc_sync_leave_group() marks it), for sync.c to tell a member with. Not
 * made if one is there already, nor if no other member is left to tell. If the group's
 * scrobbling account is still the one this device brought (session_key, from before the
 * pairing: only this device was signed in, so the pairing made its account the group's), it
 * is signed out there too, as the group was before. */
static mc_status make_leave_notice(mc_sync_db *sdb, const char *config_dir,
                                   const char *session_key) {
  char *dir = join(config_dir, MC_LIBMERGE_LEAVE_DIR);
  char *path = dir ? join(dir, MC_SYNC_DB_NAME) : NULL;
  size_t n = dir ? strlen(dir) + 6 : 0;
  char *part = dir ? malloc(n) : NULL;
  if (part) snprintf(part, n, "%s.part", dir);
  char *copy = part ? join(part, MC_SYNC_DB_NAME) : NULL;
  mc_status st = path && copy ? MC_OK : MC_ERR_NOMEM;
  int made = st == MC_OK && mc_file_exists(path);
  if (st == MC_OK && !made) {
    /* Copied into the part folder, marked there, and only then renamed into place. */
    mc_remove_tree(part);
    mc_remove_tree(dir);
    st = mc_mkdirs(part);
  }
  if (st == MC_OK && !made) {
    sqlite3_stmt *stmt;
    st = MC_ERR_DB;
    if (sqlite3_prepare_v2(mc_sync_db_handle(sdb), "VACUUM INTO ?1", -1, &stmt, NULL) ==
        SQLITE_OK) {
      mc_db_bind_path(stmt, 1, copy);
      if (sqlite3_step(stmt) == SQLITE_DONE) st = MC_OK;
      sqlite3_finalize(stmt);
    }
  }
  int others = 0;
  if (st == MC_OK && !made) {
    mc_sync_db *l = NULL;
    st = mc_sync_db_open(part, &l);
    if (st == MC_OK) st = mark_left(l, &others);
    if (st == MC_OK && others > 0) st = sign_out_brought(l, session_key);
    mc_sync_db_close(l);
  }
  if (st == MC_OK && !made && others > 0) st = mc_rename_in_place(part, dir);
  if (part && (st != MC_OK || (!made && others == 0))) mc_remove_tree(part);
  free(copy);
  free(part);
  free(path);
  free(dir);
  return st;
}

/* --- The library --- */

static int meta_text(mc_library *lib, const char *key, char *out, size_t cap) {
  return mc_library_meta_get(lib, key, out, cap) == MC_OK && out[0];
}

static mc_status set_meta(sqlite3 *db, const char *key, const char *value) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO main.meta (key, value) VALUES (?1, ?2)", -1,
                         &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
  mc_status st = sqlite3_step(stmt) == SQLITE_DONE ? MC_OK : MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

/* lib's contents replaced by the copy's at path, with the keys that say what's left. */
static mc_status put_back_library(mc_library *lib, const char *path, const char *with,
                                  const char *own) {
  sqlite3 *db = mc_library_db(lib);
  mc_status st = attach(db, path);
  if (st != MC_OK) return st;
  st = exec_sql(db, "BEGIN IMMEDIATE");
  if (st == MC_OK) {
    /* Again under the write lock: another connection may have put it back meanwhile. */
    sqlite3_stmt *stmt;
    int waits = 0;
    if (sqlite3_prepare_v2(db,
                           "SELECT count(*) FROM main.meta WHERE key IN (?1, ?2) AND value <> ''",
                           -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, MC_LIBMERGE_META, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, MC_LIBMERGE_WAIT_META, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) waits = sqlite3_column_int(stmt, 0) == 2;
      sqlite3_finalize(stmt);
    }
    if (!waits) st = MC_ERR_NOT_FOUND;
    lib->applying_remote = 1;
    if (st == MC_OK) st = replace_tables(db);
    if (st == MC_OK) st = set_meta(db, MC_LIBMERGE_RESTORED_META, with);
    if (st == MC_OK) st = set_meta(db, MC_LIBMERGE_RESTORE_SYNC_META, "1");
    if (st == MC_OK) st = set_meta(db, MC_LIBMERGE_RESTORED_NAME_META, own);
    lib->applying_remote = 0;
    st = commit_or_rollback(db, st);
  }
  exec_sql(db, "DETACH DATABASE bak");
  if (st == MC_OK) st = mc_library_reload_ids(lib);
  return st;
}

/* Under restore_lock(). */
static mc_status restore_library(mc_library *lib) {
  char name[256], wait[64], with[MC_SYNC_DEVICE_NAME_MAX + 1] = "", own[MC_SYNC_DEVICE_NAME_MAX + 1] = "";
  if (!meta_text(lib, MC_LIBMERGE_META, name, sizeof name) ||
      !meta_text(lib, MC_LIBMERGE_WAIT_META, wait, sizeof wait))
    return MC_ERR_NOT_FOUND;
  meta_text(lib, MC_LIBMERGE_WITH_META, with, sizeof with);
  meta_text(lib, MC_LIBMERGE_NAME_META, own, sizeof own);
  char *backup = mc_library_abspath(lib, name);
  if (!backup) return MC_ERR_NOMEM;
  if (!mc_file_exists(backup)) {
    /* Nothing to put back: the merge stays pending, as it was. */
    fprintf(stderr, "kithcore: merge: the library from before pairing is gone; not restored\n");
    free(backup);
    return MC_ERR_NOT_FOUND;
  }
  /* A copy of the copy, brought up to this version's schema by opening it: the backup itself
   * stays as it was set aside. Its folder goes again at the end, or at the next try. */
  char *tmp = join(mc_library_dir(lib), RESTORE_TMP);
  char *tmp_db = tmp ? join(tmp, MC_LIBRARY_DB_NAME) : NULL;
  mc_status st = tmp_db ? MC_OK : MC_ERR_NOMEM;
  if (st == MC_OK) {
    mc_remove_tree(tmp);
    st = mc_mkdirs(tmp);
  }
  if (st == MC_OK) st = mc_copy_file(backup, tmp_db, 1);
  if (st == MC_OK) {
    mc_library *copy = NULL;
    st = mc_library_open_plain(tmp, &copy);
    mc_library_close(copy);
  }
  if (st == MC_OK) st = put_back_library(lib, tmp_db, with, own);
  if (tmp) mc_remove_tree(tmp);
  if (st == MC_OK)
    fprintf(stderr, "kithcore: merge: the pairing wasn't finished: the library is back as it was\n");
  else if (st != MC_ERR_NOT_FOUND)
    fprintf(stderr, "kithcore: merge: couldn't restore the library from before pairing (%d)\n",
            (int)st);
  free(tmp_db);
  free(tmp);
  free(backup);
  return st;
}

mc_status mc_libmerge_restore(mc_library *lib) {
  if (!lib) return MC_ERR_INVALID_ARG;
  sqlite3_mutex *m = restore_lock();
  mc_folder_hold(mc_library_dir(lib));
  sqlite3_mutex_enter(m);
  mc_status st = restore_library(lib);
  sqlite3_mutex_leave(m);
  mc_folder_release(mc_library_dir(lib));
  return st;
}

mc_status mc_libmerge_restore_on_open(mc_library *lib) {
  char wait[64];
  if (!lib || !meta_text(lib, MC_LIBMERGE_WAIT_META, wait, sizeof wait) ||
      !mc_libmerge_pending(lib, NULL))
    return MC_ERR_NOT_FOUND;
  /* Set aside in this run: it still waits for Sync now. */
  if (!strcmp(wait, mc_libmerge_run_token())) return MC_ERR_NOT_FOUND;
  return mc_libmerge_restore(lib);
}

/* --- Then sync.db --- */

/* Under restore_lock(). */
static mc_status restore_sync(mc_library *lib, mc_sync_db *sdb, const char *config_dir,
                              char *own_name, size_t cap, int *leave) {
  char *bdir = join(config_dir, MC_LIBMERGE_SYNC_BACKUP_DIR);
  char *bpath = bdir ? join(bdir, MC_SYNC_DB_NAME) : NULL;
  char *ldir = join(config_dir, MC_LIBMERGE_LEAVE_DIR);
  char *lpath = ldir ? join(ldir, MC_SYNC_DB_NAME) : NULL;
  mc_status st = bpath && lpath ? MC_OK : MC_ERR_NOMEM;
  char flag[8];
  if (st == MC_OK && !meta_text(lib, MC_LIBMERGE_RESTORE_SYNC_META, flag, sizeof flag)) {
    /* Nothing left to restore. A copy of sync.db that no merge needs (the merge ran, or the
     * restore was done but for removing it) goes. */
    if (!mc_libmerge_pending(lib, NULL)) remove_with_part(bdir);
    st = MC_ERR_NOT_FOUND;
  }
  char old_group[MC_UID_STRLEN] = "", *old_key = NULL;
  int have = st == MC_OK && mc_file_exists(bpath);
  if (have) st = ready_copy(bdir, old_group, &old_key);
  if (st == MC_OK) {
    const char *group = mc_sync_db_group_uid(sdb);
    /* Still in the kept group (a restart): it leaves it as mc_sync_leave_group() would, the
     * notice sent once sync runs. Not fatal: if it can't be made, the group keeps this device
     * as one that never answers, as when leaving reaches no member. */
    if (group && strcmp(group, old_group) && make_leave_notice(sdb, config_dir, old_key) != MC_OK)
      fprintf(stderr, "kithcore: merge: restore: couldn't keep a note to leave the group\n");
    if (have)
      st = put_back_sync(sdb, bpath);
    else if (group)
      st = mc_sync_db_clear_group(sdb); /* a copy from before there were sync.db copies */
  }
  if (st == MC_OK) {
    if (own_name) meta_text(lib, MC_LIBMERGE_RESTORED_NAME_META, own_name, cap);
    /* Done: both keys go together. */
    sqlite3 *db = mc_library_db(lib);
    st = exec_sql(db, "BEGIN IMMEDIATE");
    sqlite3_stmt *stmt;
    if (st == MC_OK) {
      st = MC_ERR_DB;
      if (sqlite3_prepare_v2(db, "DELETE FROM meta WHERE key IN (?1, ?2)", -1, &stmt, NULL) ==
          SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, MC_LIBMERGE_RESTORE_SYNC_META, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, MC_LIBMERGE_RESTORED_NAME_META, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE) st = MC_OK;
        sqlite3_finalize(stmt);
      }
      st = commit_or_rollback(db, st);
    }
  }
  if (st == MC_OK) remove_with_part(bdir);
  if (st == MC_OK && leave) *leave = mc_file_exists(lpath);
  if (st != MC_OK && st != MC_ERR_NOT_FOUND)
    fprintf(stderr, "kithcore: merge: couldn't restore sync.db from before pairing (%d)\n",
            (int)st);
  free(old_key);
  free(lpath);
  free(ldir);
  free(bpath);
  free(bdir);
  return st;
}

mc_status mc_libmerge_restore_sync(mc_library *lib, mc_sync_db *sdb, const char *config_dir,
                                   char *own_name, size_t cap, int *leave) {
  if (own_name && cap) own_name[0] = '\0';
  if (leave) *leave = 0;
  if (!lib || !sdb || !config_dir || (own_name && !cap)) return MC_ERR_INVALID_ARG;
  sqlite3_mutex *m = restore_lock();
  sqlite3_mutex_enter(m);
  mc_status st = restore_sync(lib, sdb, config_dir, own_name, cap, leave);
  sqlite3_mutex_leave(m);
  return st;
}
