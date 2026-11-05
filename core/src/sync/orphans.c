/* The orphan sweep. See orphans.h.
 *
 * What the app keeps under the library folder, and how each is kept from the sweep:
 *  - tracks' originals and transcodes: files rows, the only thing that counts as "known";
 *  - what a set-aside library's rows name: a merge leaves tracks it couldn't bring in where
 *    they are, out of the library, and they may be the only copy anywhere (see known_snapshot);
 *  - covers: cover/folder/front .jpg/.jpeg/.png (mc_coverart_is_name()) in a folder with a
 *    known file in or under it. There's no row for one, and the album's folder is all that
 *    ties it to anything;
 *  - library.db and anything "library.db-*" at the top (-wal, -shm, -journal);
 *  - library-before-merge-*.db at the top, the set-aside copies, and their -wal or -shm;
 *  - ".merge-restore", the folder that undoing a merge works in, at the top;
 *  - ".kithsong-*" anywhere: an edit's copies and backups, the artist refresh's copies, the
 *    library move's write probe, a source run's marker (musiccore/source_run.h). Some have an
 *    edit's journal to answer to, and one kept needlessly costs a little space, where a backup
 *    deleted loses a track;
 *  - the config folder, should the user have put it inside: sync.db, deleted.db, settings.json,
 *    transcode-cache, playlist art, sync-before-merge and the rest. Sitting at the library
 *    folder itself it's refused outright, since those names could then be anywhere at the top.
 * Nothing else is the app's: logs go to stderr, and thumbnails and art caches live in the config
 * or cache folder. In-flight files need no name of their own: a ".part" being received, a
 * "cover.jpg.tmp" being written and an import's copy all belong to someone holding the folder
 * (library/folder_gate.h), so no sweep runs beside them, and they're too young for one anyway.
 * One left by a crash is an orphan like any other.
 *
 * What isn't the app's or the user's to lose, so it's left alone: desktop.ini, KDE's .directory,
 * whatever has Windows' system attribute, and at the top a volume's own folders ($RECYCLE.BIN, System Volume
 * Information, lost+found, and .Trash or .Trash-1000, which mc_trash() itself fills when the
 * library is at the top of a drive). Links and junctions are neither followed nor touched.
 *
 * The order is what makes a file added meanwhile safe. The walk comes first and the rows are
 * read after it, so a file seen in the walk with a row by then is known. Only once the walk has
 * finished without an error is anything judged. Then before each removal the folder must still
 * be free, and the file must still have no row and be as the walk saw it. That last check is
 * also what stands between two apps on one library (config folders differ, so each instance
 * claim passes), which the gate can't see. */
#include "sync/orphans.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "library/edit_internal.h"
#include "library/folder_gate.h"
#include "musiccore/delete.h"
#include "sync/archive.h"
#include "sync/libmerge.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"
#include "tagger/coverart.h"
#include "util/db_open.h"
#include "util/pathkey.h"
#include "util/platform.h"

#ifdef __ANDROID__
int mc_orphans_permanent = 1;
#else
int mc_orphans_permanent = 0;
#endif
int mc_orphans_test_mtime_only = 0;

/* A line of the sweep's, on stderr with the rest of core's and, on Android, where stderr goes
 * nowhere, in logcat. */
static void say(const char *fmt, ...) {
  char line[2200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  fprintf(stderr, "musiccore: orphans: %s\n", line);
#ifdef __ANDROID__
  __android_log_print(ANDROID_LOG_INFO, "kithsong-orphans", "%s", line);
#endif
}

/* --- Lists --- */

typedef struct strs {
  char **items;
  int count, cap;
} strs;

static int strs_take(strs *l, char *s) {
  if (!s) return 0;
  if (l->count == l->cap) {
    int cap = l->cap ? l->cap * 2 : 256;
    char **more = realloc(l->items, (size_t)cap * sizeof *more);
    if (!more) {
      free(s);
      return 0;
    }
    l->items = more;
    l->cap = cap;
  }
  l->items[l->count++] = s;
  return 1;
}

static void strs_clear(strs *l) {
  for (int i = 0; i < l->count; i++) free(l->items[i]);
  free(l->items);
  memset(l, 0, sizeof *l);
}

static int str_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Sorts, for strs_has(), and drops repeats. */
static void strs_sort(strs *l) {
  if (l->count > 1) qsort(l->items, (size_t)l->count, sizeof *l->items, str_cmp);
  int n = 0;
  for (int i = 0; i < l->count; i++) {
    if (n && !strcmp(l->items[n - 1], l->items[i])) free(l->items[i]);
    else l->items[n++] = l->items[i];
  }
  l->count = n;
}

static int strs_has(const strs *l, const char *s) {
  return l->count && bsearch(&s, l->items, (size_t)l->count, sizeof *l->items, str_cmp) != NULL;
}

/* --- What's in the folder --- */

typedef struct found {
  char *rel, *key;
  int64_t newest_ms, mtime_ms;
} found;

typedef struct scan {
  mc_library *lib;
  const mc_orphans_args *args;
  found *files;
  int count, cap;
  strs snapshots; /* the set-aside libraries' file names */
  mc_status st;
} scan;

static void scan_clear(scan *s) {
  for (int i = 0; i < s->count; i++) {
    free(s->files[i].rel);
    free(s->files[i].key);
  }
  free(s->files);
  strs_clear(&s->snapshots);
}

static int starts_with(const char *s, const char *prefix) {
  return !strncmp(s, prefix, strlen(prefix));
}

/* A folder at the top of a drive that's the volume's. name is folded (mc_path_key()). */
static int volume_folder(const char *name) {
  return !strcmp(name, "$recycle.bin") || !strcmp(name, "system volume information") ||
         !strcmp(name, "lost+found") || !strcmp(name, ".trash") || starts_with(name, ".trash-");
}

static int on_entry(void *user, const mc_tree_entry *e) {
  scan *s = user;
  if (s->args->stopping && s->args->stopping(s->args->user)) {
    s->st = MC_ERR_IN_USE;
    return -1;
  }
  if (e->system) return 1; /* the OS's; for a file, 1 is the same as 0 */
  char *rel = mc_library_relpath(s->lib, e->path);
  char *key = rel ? mc_path_key(rel) : NULL;
  if (!key) {
    free(rel);
    s->st = MC_ERR_NOMEM;
    return -1;
  }
  const char *slash = strrchr(key, '/');
  const char *name = slash ? slash + 1 : key;
  int top = !slash, leave = 0;
  if (e->is_dir) {
    leave = mc_same_path(e->path, s->args->config_dir) ||
            (top && (volume_folder(name) || !strcmp(name, MC_LIBMERGE_RESTORE_DIR))) ||
            starts_with(name, ".kithsong-");
    free(rel);
    free(key);
    return leave;
  }
  if (top && starts_with(name, MC_LIBMERGE_PREFIX)) {
    size_t n = strlen(name);
    if (n > 3 && !strcmp(name + n - 3, ".db") && !strs_take(&s->snapshots, mc_strdup(rel))) {
      free(rel);
      free(key);
      s->st = MC_ERR_NOMEM;
      return -1;
    }
    leave = 1;
  }
  leave = leave || !strcmp(name, "desktop.ini") || !strcmp(name, ".directory") ||
          starts_with(name, ".kithsong-") ||
          (top && (!strcmp(name, MC_LIBRARY_DB_NAME) || starts_with(name, MC_LIBRARY_DB_NAME "-")));
  if (leave) {
    free(rel);
    free(key);
    return 0;
  }
  if (s->count == s->cap) {
    int cap = s->cap ? s->cap * 2 : 1024;
    found *more = realloc(s->files, (size_t)cap * sizeof *more);
    if (!more) {
      free(rel);
      free(key);
      s->st = MC_ERR_NOMEM;
      return -1;
    }
    s->files = more;
    s->cap = cap;
  }
  s->files[s->count++] = (found){rel, key, e->newest_ms, e->mtime_ms};
  return 0;
}

/* --- What the library knows --- */

typedef struct known {
  strs rows;   /* keys of this library's files rows */
  strs others; /* keys of what set-aside libraries' rows name */
  strs dirs;   /* keys of every folder holding either, at any depth */
} known;

static void known_clear(known *k) {
  strs_clear(&k->rows);
  strs_clear(&k->others);
  strs_clear(&k->dirs);
}

/* Adds every folder above key. */
static int add_dirs(known *k, const char *key) {
  for (const char *p = strchr(key, '/'); p; p = strchr(p + 1, '/')) {
    size_t n = (size_t)(p - key);
    char *dir = malloc(n + 1);
    if (!dir) return 0;
    memcpy(dir, key, n);
    dir[n] = '\0';
    if (!strs_take(&k->dirs, dir)) return 0;
  }
  return 1;
}

/* Every path a query's first column gives, into list. */
static mc_status read_paths(sqlite3 *db, known *k, strs *list) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT path FROM files", -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  mc_status st = MC_OK;
  int rc;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *path = (const char *)sqlite3_column_text(stmt, 0);
    if (!path) continue;
    char *key = mc_path_key(path);
    if (!key || !add_dirs(k, key) || !strs_take(list, key)) st = MC_ERR_NOMEM;
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

/* A made-up library for profiling, most of whose rows have no file: it says nothing of what a
 * folder should hold. */
#define STRESS_META "stress_library"

/* Whether an edit's swap (it may need its files put back) or a merge is unfinished, or this is a
 * stress library, under the caller's read transaction: the rows don't say where things are, or
 * mean to. -1 on an error. */
static int unsettled(sqlite3 *db) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT 1 FROM meta WHERE key IN ('" MC_EDIT_SWAP_META "', '" MC_LIBMERGE_META
                         "', '" MC_LIBMERGE_WAIT_META "', '" STRESS_META "')"
                         " AND coalesce(value, '') <> ''",
                         -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

/* The rows of a set-aside library. A copy that can't be read may be the only word on files a
 * merge left behind, so that's an error, not a copy to skip. */
static mc_status known_snapshot(mc_library *lib, const char *name, known *k) {
  char *abs = mc_library_abspath(lib, name);
  sqlite3 *db = NULL;
  mc_status st = abs ? MC_ERR_DB : MC_ERR_NOMEM;
  if (abs && mc_db_open(abs, &db, SQLITE_OPEN_READONLY) == SQLITE_OK) {
    sqlite3_busy_timeout(db, 5000);
    st = read_paths(db, k, &k->others);
  }
  sqlite3_close_v2(db);
  free(abs);
  return st;
}

/* --- What's kept of it ---
 * stderr is gone with a desktop app's window and never reaches the user, and a sweep's first run
 * on an old install can take a lot. So two files in the config folder: orphans-removed.log, a
 * line for each file removed, for finding them in the Recycle Bin (on Android, for knowing what
 * was lost); and orphans-found.txt, written afresh by each sweep that got as far as judging,
 * whether it then removed them, only reported them or refused, so what a sweep would take can
 * be read before it does. */

/* Past this the log becomes orphans-removed.log.old, replacing the one before. */
#define RECORD_MAX_BYTES ((int64_t)1 << 20)

static char *record_path(const char *config_dir, const char *name) {
  size_t n = strlen(config_dir) + strlen(name) + 2;
  char *path = malloc(n);
  if (path) snprintf(path, n, "%s%c%s", config_dir, MC_PATH_SEP, name);
  return path;
}

static void stamp(char out[24]) {
  time_t now = (time_t)(mc_wall_ms() / 1000);
  struct tm *t = gmtime(&now);
  if (!t || !strftime(out, 24, "%Y-%m-%dT%H:%M:%SZ", t)) snprintf(out, 24, "?");
}

static void record_removal(const char *config_dir, const char *how, const char *abs) {
  char *path = record_path(config_dir, MC_ORPHANS_LOG), *old = record_path(config_dir, MC_ORPHANS_LOG ".old");
  int64_t size = 0, mtime = 0;
  if (path && old && mc_file_info(path, &size, &mtime) == MC_OK && size > RECORD_MAX_BYTES)
    mc_rename(path, old, 1);
  FILE *f = path ? mc_fopen(path, "ab") : NULL;
  if (f) {
    char when[24];
    stamp(when);
    fprintf(f, "%s\t%s\t%s\n", when, how, abs);
    fclose(f);
  }
  free(path);
  free(old);
}

static void record_found(const char *config_dir, const char *dir, const char *verdict, const scan *s,
                         const int *orphan, const mc_orphans_result *r) {
  char *path = record_path(config_dir, MC_ORPHANS_FOUND);
  FILE *f = path ? mc_fopen(path, "wb") : NULL;
  if (f) {
    char when[24];
    stamp(when);
    fprintf(f, "%s\t%s\n%s: %d known, %d orphans, %d too new\n", when, dir, verdict, r->known,
            r->orphans, r->young);
    for (int i = 0; i < s->count; i++)
      if (orphan[i]) fprintf(f, "%s\n", s->files[i].rel);
    fclose(f);
  }
  free(path);
}

/* --- Removing --- */

static int row_names(mc_library *lib, const char *rel) {
  sqlite3_stmt *stmt;
  /* Any error says yes: then nothing goes. */
  if (sqlite3_prepare_v2(mc_library_db(lib), "SELECT 1 FROM files WHERE path = ?1 COLLATE NOCASE",
                         -1, &stmt, NULL) != SQLITE_OK)
    return 1;
  sqlite3_bind_text(stmt, 1, rel, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc != SQLITE_DONE;
}

/* The folders abs was in, from the deepest, for as long as they're empty. Never the top. */
static void prune_up(mc_library *lib, char *abs) {
  size_t top = strlen(mc_library_dir(lib));
  for (;;) {
    char *sep = strrchr(abs, MC_PATH_SEP);
    if (!sep || (size_t)(sep - abs) <= top) return;
    *sep = '\0';
    if (mc_rmdir(abs) != MC_OK) return;
    say("removed the empty folder %s", abs + top + 1);
  }
}

/* mc_transfer_trash(), but in a debug build KITHSONG_TEST_TRASH_DIR takes the place of the real
 * trash, so a run of the app on a scratch library leaves the Recycle Bin alone. */
static mc_status to_trash(const char *abs) {
#ifndef NDEBUG
  const char *dir = getenv("KITHSONG_TEST_TRASH_DIR");
  if (dir && *dir) {
    static int n;
    const char *name = strrchr(abs, MC_PATH_SEP);
    char to[2300];
    snprintf(to, sizeof to, "%s%c%d-%s", dir, MC_PATH_SEP, __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED),
             name ? name + 1 : abs);
    return mc_mkdirs(dir) == MC_OK ? mc_rename(abs, to, 0) : MC_ERR_IO;
  }
#endif
  return mc_transfer_trash(abs);
}

/* 1 if f went, 0 if it's still there and that's fine, -1 if it couldn't be removed. */
static int remove_one(mc_library *lib, const char *config_dir, const found *f) {
  char *abs = mc_transfer_deletable_path(lib, f->rel);
  if (!abs) return -1;
  int64_t size = 0, mtime = 0;
  int went = 0;
  if (row_names(lib, f->rel) || mc_file_info(abs, &size, &mtime) != MC_OK || mtime != f->mtime_ms) {
    say("left %s: it changed during the sweep", f->rel);
  } else if ((mc_orphans_permanent ? mc_remove(abs) : to_trash(abs)) == MC_OK) {
    const char *how = mc_orphans_permanent ? "deleted" : "moved to the trash";
    say("%s: %s", how, f->rel);
    record_removal(config_dir, how, abs);
    prune_up(lib, abs);
    went = 1;
  } else {
    /* Never deleted instead: where there's a trash, this was to be recoverable. */
    say("couldn't remove %s; it stays until the next sweep", f->rel);
    went = -1;
  }
  free(abs);
  return went;
}

/* --- The sweep --- */

static mc_status refuse(mc_status st, const char *why, const char *dir) {
  say("not sweeping %s: %s", dir, why);
  return st;
}

/* The rows, read after the walk and at one moment. */
static mc_status read_known(mc_library *lib, const scan *s, known *k) {
  sqlite3 *db = mc_library_db(lib);
  if (sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) return MC_ERR_DB;
  int open = unsettled(db);
  mc_status st = open < 0 ? MC_ERR_DB : open ? MC_ERR_LOCKED : read_paths(db, k, &k->rows);
  sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  for (int i = 0; st == MC_OK && i < s->snapshots.count; i++)
    st = known_snapshot(lib, s->snapshots.items[i], k);
  if (st == MC_OK) {
    strs_sort(&k->rows);
    strs_sort(&k->others);
    strs_sort(&k->dirs);
  }
  return st;
}

/* Whether nothing explains f, by the keys. */
static int is_orphan(const known *k, const found *f) {
  if (strs_has(&k->rows, f->key) || strs_has(&k->others, f->key)) return 0;
  char *slash = strrchr(f->key, '/');
  if (!slash) return !mc_coverart_is_name(f->key) || !k->rows.count;
  if (!mc_coverart_is_name(slash + 1)) return 1;
  *slash = '\0';
  int kept = strs_has(&k->dirs, f->key);
  *slash = '/';
  return !kept;
}

static mc_status sweep_watched(const mc_orphans_args *a, mc_folder_watch *w, mc_orphans_result *r) {
  mc_library *lib = a->lib;
  const char *dir = mc_library_dir(lib);
  char *db = mc_library_abspath(lib, MC_LIBRARY_DB_NAME);
  int here = db && mc_file_exists(db);
  free(db);
  /* A drive that didn't mount leaves an empty folder, or none: the open handle wouldn't tell. */
  if (!here) return refuse(MC_ERR_NOT_FOUND, "library.db isn't in the folder", dir);

  int64_t cutoff = (int64_t)mc_wall_ms() - MC_ORPHANS_GRACE_MS;
  scan s = {.lib = lib, .args = a, .st = MC_OK};
  known k = {0};
  mc_status st = mc_tree_walk(dir, on_entry, &s);
  if (st != MC_OK || s.st != MC_OK) {
    if (s.st != MC_OK) st = s.st;
    if (st != MC_ERR_IN_USE) refuse(st, "the folder couldn't be walked whole", dir);
    goto done;
  }
  st = read_known(lib, &s, &k);
  if (st != MC_OK) {
    refuse(st, st == MC_ERR_LOCKED ? "an edit or a merge is unfinished, or it's a stress library" : "the library couldn't be read", dir);
    goto done;
  }
  if (!k.rows.count) {
    st = refuse(MC_ERR_INVALID_ARG, "the library knows no files", dir);
    goto done;
  }

  int *orphan = calloc((size_t)s.count + 1, sizeof *orphan);
  if (!orphan) {
    st = MC_ERR_NOMEM;
    goto done;
  }
  for (int i = 0; i < s.count; i++) {
    const found *f = &s.files[i];
    if (strs_has(&k.rows, f->key)) r->known++;
    if (!is_orphan(&k, f)) continue;
    int64_t age = mc_orphans_test_mtime_only ? f->mtime_ms : f->newest_ms;
    if (age >= cutoff) r->young++;
    else orphan[i] = 1, r->orphans++;
  }
  int too_many = (int64_t)r->orphans > (int64_t)r->known * MC_ORPHANS_MAX_PER_KNOWN;
  record_found(a->config_dir, dir,
               !r->known || too_many ? "refused, nothing removed"
               : a->report_only      ? "reported only, nothing removed"
                                     : "being removed",
               &s, orphan, r);
  if (!r->known) {
    st = refuse(MC_ERR_INVALID_ARG, "none of the library's files are in the folder", dir);
  } else if (too_many) {
    say("%d orphans to %d known files", r->orphans, r->known);
    st = refuse(MC_ERR_INVALID_ARG, "orphans outnumber known files", dir);
  } else {
    for (int i = 0; i < s.count && st == MC_OK; i++) {
      if (!orphan[i]) continue;
      if (a->report_only) {
        say("unknown file in the library folder: %s", s.files[i].rel);
      } else if (a->stopping && a->stopping(a->user)) {
        st = MC_ERR_IN_USE;
      } else if (!mc_folder_watch_clear(w)) {
        st = refuse(MC_ERR_LOCKED, "something started working in the folder; stopped", dir);
      } else {
        int went = remove_one(lib, a->config_dir, &s.files[i]);
        if (went > 0) r->removed++;
        else if (went < 0) r->failed++;
      }
    }
    say("%s: %d known, %d orphans, %d removed, %d failed, %d too new", dir, r->known, r->orphans,
        r->removed, r->failed, r->young);
  }
  free(orphan);
done:
  known_clear(&k);
  scan_clear(&s);
  return st;
}

mc_status mc_orphans_sweep(const mc_orphans_args *a, mc_orphans_result *out) {
  mc_orphans_result r;
  memset(&r, 0, sizeof r);
  if (out) *out = r;
  if (!a || !a->lib || !a->config_dir) return MC_ERR_INVALID_ARG;
  const char *dir = mc_library_dir(a->lib);
  if (mc_same_path(dir, a->config_dir))
    return refuse(MC_ERR_INVALID_ARG, "it's also the config folder", dir);
  mc_folder_watch *w = mc_folder_watch_begin(dir, a->own_holds);
  if (!w) return refuse(MC_ERR_LOCKED, "something else is working in the folder", dir);
  mc_status st = sweep_watched(a, w, &r);
  mc_folder_watch_end(w);
  if (out) *out = r;
  return st;
}

/* Whether two paths are the same place or one is inside the other, by mc_path_key(). */
static int overlap(const char *a, const char *b) {
  char *fa = mc_full_path(a), *fb = mc_full_path(b);
  char *ka = fa ? mc_path_key(fa) : NULL, *kb = fb ? mc_path_key(fb) : NULL;
  /* Unsure counts as overlapping. */
  int yes = 1;
  if (ka && kb) {
    size_t la = strlen(ka), lb = strlen(kb), n = la < lb ? la : lb;
    const char *longer = la < lb ? kb : ka;
    yes = !strncmp(ka, kb, n) && (la == lb || longer[n] == '/');
  }
  free(fa);
  free(fb);
  free(ka);
  free(kb);
  return yes;
}

void mc_orphans_after_import(mc_library *lib, const char *const *sources, int source_count,
                             int own_holds, int (*stopping)(void *user), void *user) {
  const char *dir = mc_library_dir(lib);
  for (int i = 0; i < source_count; i++)
    if (overlap(sources[i], dir)) {
      refuse(MC_ERR_INVALID_ARG, "the import came from inside it", dir);
      return;
    }
  char *config = mc_folder_config(dir);
  if (!config) {
    refuse(MC_ERR_INVALID_ARG, "sync isn't running on it, so there's no config folder", dir);
    return;
  }
  size_t n = strlen(config) + sizeof MC_SYNC_DB_NAME + 1;
  char *path = malloc(n);
  int report_only = 0, go = path != NULL;
  if (path) snprintf(path, n, "%s%c%s", config, MC_PATH_SEP, MC_SYNC_DB_NAME);
  mc_sync_db *sdb = NULL;
  if (go && mc_file_exists(path)) {
    go = mc_sync_db_open(config, &sdb) == MC_OK;
    if (go && mc_sync_db_group_uid(sdb)) {
      sqlite3 *db = mc_sync_db_handle(sdb);
      const char *me = mc_sync_db_device_uid(sdb);
      report_only = MC_ORPHANS_ARCHIVE_ONLY_REPORTS && mc_archive_is(db, me) != 0; /* an error too */
      go = mc_profile_confirm_state(db, me) == MC_SYNC_CONFIRMED;
    }
    mc_sync_db_close(sdb);
    if (!go) refuse(MC_ERR_LOCKED, "this device's profile isn't confirmed", dir);
  }
  free(path);
  if (go) {
    mc_delete_restore(lib, config, NULL, NULL);
    mc_orphans_args a = {lib, config, report_only, own_holds, stopping, user};
    mc_orphans_sweep(&a, NULL);
  }
  free(config);
}
