/* Moving a library to another folder. See musiccore/library_move.h for what moves and the
 * order that keeps the old library usable until the new one is complete.
 *
 * The library folder is listed once, depth first with each folder before what's in it, into
 * a list of entries relative to it. Copying walks the list forwards; undoing a copy and
 * removing the old files walk it backwards, so files go before their folders. library.db
 * and its -wal and -shm come last at the top, so a rename that stops part-way has moved the
 * music rather than the database. */
#include "musiccore/library_move.h"

#include "musiccore/settings.h"

#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "library/move_internal.h"
#include "util/platform.h"

int64_t mc_move_test_free_space = -1;
int mc_move_test_fail_at = 0;
int mc_move_test_no_rename = 0;

/* Debug builds: the hooks from the environment, see move_internal.h. */
static void env_hooks(void) {
#ifndef NDEBUG
  const char *copy = getenv("KITHSONG_TEST_MOVE_COPY"), *space = getenv("KITHSONG_TEST_MOVE_FREE_SPACE");
  if (copy && *copy) mc_move_test_no_rename = atoi(copy) != 0;
  if (space && *space) mc_move_test_free_space = strtoll(space, NULL, 10);
#endif
}

/* Room for the filesystem's own overhead on top of the library's bytes. */
static int64_t needed_for(int64_t bytes) {
  return bytes + bytes / 50 + ((int64_t)64 << 20);
}

/* --- Paths --- */

static int is_sep(char c) {
#ifdef _WIN32
  return c == '\\' || c == '/';
#else
  return c == '/';
#endif
}

static char *join(const char *a, const char *b) {
  size_t n = strlen(a), m = strlen(b);
  int sep = n && !is_sep(a[n - 1]);
  char *out = malloc(n + sep + m + 1);
  if (!out) return NULL;
  memcpy(out, a, n);
  if (sep) out[n] = MC_PATH_SEP;
  memcpy(out + n + sep, b, m + 1);
  return out;
}

/* Whether the first n bytes of path name the same folder as dir. */
static int prefix_is(const char *path, size_t n, const char *dir) {
  char *prefix = malloc(n + 1);
  if (!prefix) return 0;
  memcpy(prefix, path, n);
  prefix[n] = '\0';
  int same = mc_same_path(prefix, dir);
  free(prefix);
  return same;
}

/* Whether path is strictly inside dir. Both are full paths (mc_full_path()). */
static int is_inside(const char *path, const char *dir) {
  size_t n = strlen(dir);
  if (strlen(path) <= n) return 0;
  if (n && is_sep(dir[n - 1])) return prefix_is(path, n, dir); /* a root: "/" or "C:\" */
  return is_sep(path[n]) && prefix_is(path, n, dir);
}

/* The nearest folder at or above full that exists; NULL if none does. */
static char *existing_parent(const char *full) {
  char *p = mc_strdup(full);
  while (p && !mc_file_exists(p)) {
    char *cut = p + strlen(p);
    while (cut > p && !is_sep(cut[-1])) cut--;
    size_t len = strlen(p);
    if (cut - 1 == p || (cut - 2 >= p && cut[-2] == ':')) *cut = '\0'; /* keep the root's separator */
    else if (cut > p) cut[-1] = '\0';
    if (strlen(p) == len) { /* nothing left to take off */
      free(p);
      return NULL;
    }
  }
  return p;
}

/* Whether a file can be made in dir. */
static int can_write(const char *dir) {
  unsigned char r[6];
  char name[32];
  if (mc_random_bytes(r, sizeof r) != MC_OK) memset(r, 0, sizeof r);
  snprintf(name, sizeof name, ".kithsong-probe-%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2], r[3], r[4], r[5]);
  char *path = join(dir, name);
  FILE *f = path ? mc_fopen(path, "wb") : NULL;
  int ok = f != NULL;
  if (f) {
    fclose(f);
    mc_remove(path);
  }
  free(path);
  return ok;
}

/* --- Listing the library --- */

typedef struct entry {
  char *rel; /* relative to the library folder, with native separators */
  int64_t size;
  int is_dir;
} entry;

typedef struct tree {
  entry *items;
  int count, cap;
  int64_t bytes;
  int files;
  mc_status st;
} tree;

typedef struct names {
  char **items;
  int *dirs;
  int count, cap;
  int failed;
} names;

static void collect(void *user, const char *name, int is_dir) {
  names *n = user;
  if (n->count == n->cap) {
    int cap = n->cap ? 2 * n->cap : 32;
    char **items = realloc(n->items, (size_t)cap * sizeof *items);
    if (items) n->items = items;
    int *dirs = realloc(n->dirs, (size_t)cap * sizeof *dirs);
    if (dirs) n->dirs = dirs;
    if (!items || !dirs) {
      n->failed = 1;
      return;
    }
    n->cap = cap;
  }
  if (!(n->items[n->count] = mc_strdup(name))) {
    n->failed = 1;
    return;
  }
  n->dirs[n->count++] = is_dir;
}

static void names_free(names *n) {
  for (int i = 0; i < n->count; i++) free(n->items[i]);
  free(n->items);
  free(n->dirs);
}

static void tree_add(tree *t, char *rel, int64_t size, int is_dir) {
  if (t->count == t->cap) {
    int cap = t->cap ? 2 * t->cap : 256;
    entry *items = realloc(t->items, (size_t)cap * sizeof *items);
    if (!items) {
      free(rel);
      t->st = MC_ERR_NOMEM;
      return;
    }
    t->items = items;
    t->cap = cap;
  }
  t->items[t->count++] = (entry){rel, size, is_dir};
  if (!is_dir) {
    t->bytes += size;
    t->files++;
  }
}

static void tree_free(tree *t) {
  for (int i = 0; i < t->count; i++) free(t->items[i].rel);
  free(t->items);
  memset(t, 0, sizeof *t);
}

/* library.db, library.db-wal, library.db-shm, library.db-journal. */
static int is_db_file(const char *name) {
  size_t n = strlen(MC_LIBRARY_DB_NAME);
  return !strncmp(name, MC_LIBRARY_DB_NAME, n) && (!name[n] || name[n] == '-');
}

/* Folder settings that belong to the folder rather than the library. */
static int stays_at_top(const char *name) {
  return mc_same_path(name, "desktop.ini");
}

static void scan(tree *t, const char *root, const char *rel);

static void scan_entry(tree *t, const char *root, const char *rel, const char *name, int is_dir) {
  char *child = rel ? join(rel, name) : mc_strdup(name);
  char *full = child ? join(root, child) : NULL;
  if (!full) {
    free(child);
    t->st = MC_ERR_NOMEM;
    return;
  }
  int64_t size = 0, mtime;
  if (mc_is_link(full)) { /* links aren't followed or moved */
    free(child);
  } else if (is_dir) {
    tree_add(t, child, 0, 1);
    if (t->st == MC_OK) scan(t, root, t->items[t->count - 1].rel);
  } else if (mc_file_info(full, &size, &mtime) == MC_OK) {
    tree_add(t, child, size, 0);
  } else {
    free(child);
    t->st = MC_ERR_IO;
  }
  free(full);
}

static void scan(tree *t, const char *root, const char *rel) {
  char *dir = rel ? join(root, rel) : mc_strdup(root);
  names n = {0};
  if (!dir || mc_list_dir(dir, collect, &n) != MC_OK || n.failed) {
    t->st = !dir || n.failed ? MC_ERR_NOMEM : MC_ERR_IO;
  } else {
    /* At the top, the database goes last. */
    for (int pass = 0; pass < (rel ? 1 : 2) && t->st == MC_OK; pass++)
      for (int i = 0; i < n.count && t->st == MC_OK; i++) {
        if (!rel && (stays_at_top(n.items[i]) || is_db_file(n.items[i]) != pass)) continue;
        scan_entry(t, root, rel, n.items[i], n.dirs[i]);
      }
  }
  names_free(&n);
  free(dir);
}

/* --- Checks --- */

static void count_entry(void *user, const char *name, int is_dir) {
  (void)name;
  (void)is_dir;
  ++*(int *)user;
}

int mc_folder_is_empty(const char *dir) {
  if (!dir || !mc_file_exists(dir)) return 1;
  int n = 0;
  if (mc_list_dir(dir, count_entry, &n) != MC_OK) return 0; /* a file, or unreadable */
  return n == 0;
}

mc_status mc_folder_free_space(const char *dir, int64_t *out) {
  if (!dir || !out) return MC_ERR_INVALID_ARG;
  char *full = mc_full_path(dir), *base = full ? existing_parent(full) : NULL;
  mc_status st = base ? mc_free_space(base, out) : MC_ERR_NOT_FOUND;
  free(base);
  free(full);
  return st;
}

mc_status mc_folder_size(const char *dir, int64_t *bytes, int *files) {
  if (!dir || !bytes || !files) return MC_ERR_INVALID_ARG;
  tree t = {0};
  scan(&t, dir, NULL);
  mc_status st = t.st;
  *bytes = t.bytes;
  *files = t.files;
  tree_free(&t);
  return st;
}

/* mc_library_move_check(), keeping the listing for the move. src_full gets src made full. */
static mc_status check(const char *src, const char *dst, mc_library_move_info *info, tree *t, char **src_full) {
  env_hooks();
  memset(info, 0, sizeof *info);
  info->free_bytes = -1;
  *src_full = NULL;
  if (!src || !*src || !dst || !*dst) return MC_ERR_INVALID_ARG;
  char *fs = mc_full_path(src), *fd = mc_full_path(dst), *base = NULL;
  mc_status st = MC_OK;
  if (!fs || !fd) {
    st = MC_ERR_NOMEM;
  } else if (!mc_file_exists(fs)) {
    st = MC_ERR_NOT_FOUND;
  } else if (mc_same_path(fs, fd)) {
    info->problem = MC_MOVE_SAME_FOLDER;
  } else if (is_inside(fd, fs)) {
    info->problem = MC_MOVE_INSIDE_LIBRARY;
  } else if (is_inside(fs, fd)) {
    info->problem = MC_MOVE_CONTAINS_LIBRARY;
  } else if (!mc_folder_is_empty(fd)) {
    info->problem = MC_MOVE_NOT_EMPTY;
  } else if (!(base = existing_parent(fd)) || !can_write(base)) {
    info->problem = MC_MOVE_NOT_WRITABLE;
  } else {
    scan(t, fs, NULL);
    st = t->st;
    info->library_bytes = t->bytes;
    info->file_count = t->files;
    info->same_volume = !mc_move_test_no_rename && mc_same_volume(fs, base);
    if (mc_move_test_free_space >= 0) info->free_bytes = mc_move_test_free_space;
    else if (mc_free_space(base, &info->free_bytes) != MC_OK) info->free_bytes = -1;
    info->needed_bytes = info->same_volume ? 0 : needed_for(t->bytes);
    if (st == MC_OK && !info->same_volume && info->free_bytes >= 0 && info->free_bytes < info->needed_bytes)
      info->problem = MC_MOVE_NO_SPACE;
  }
  free(base);
  free(fd);
  if (st == MC_OK) *src_full = fs;
  else free(fs);
  return st;
}

mc_status mc_library_move_check(const char *src, const char *dst, mc_library_move_info *info) {
  if (!info) return MC_ERR_INVALID_ARG;
  tree t = {0};
  char *src_full = NULL;
  mc_status st = check(src, dst, info, &t, &src_full);
  free(src_full);
  tree_free(&t);
  return st;
}

/* --- Moving --- */

typedef struct mover {
  const char *src, *dst; /* full paths */
  const char *src_given; /* src as the caller gave it, for absolute paths in the database */
  tree *t;
  mc_library_move_fn fn;
  void *user;
  mc_library_move_result *result;
  mc_library_move_progress p;
} mover;

static void report(mover *m, mc_move_phase phase, const char *current) {
  m->p.phase = phase;
  m->p.current = current;
  if (m->fn) m->fn(m->user, &m->p);
}

static int top_level(const entry *e) {
  for (const char *c = e->rel; *c; c++)
    if (is_sep(*c)) return 0;
  return 1;
}

/* Moves entry e from one folder to the other, by renaming. */
static mc_status rename_entry(const entry *e, const char *from, const char *to) {
  char *a = join(from, e->rel), *b = join(to, e->rel);
  mc_status st = a && b ? mc_rename_in_place(a, b) : MC_ERR_NOMEM;
  free(a);
  free(b);
  return st;
}

/* Renames the top-level entries into dst. On failure renames back what it moved. */
static mc_status rename_all(mover *m) {
  report(m, MC_MOVE_PHASE_COPYING, NULL);
  int i;
  mc_status st = MC_OK;
  for (i = 0; i < m->t->count && st == MC_OK; i++)
    if (top_level(&m->t->items[i])) st = rename_entry(&m->t->items[i], m->src, m->dst);
  if (st == MC_OK) {
    m->p.files_done = m->p.files_total;
    m->p.bytes_done = m->p.bytes_total;
    return MC_OK;
  }
  for (i -= 2; i >= 0; i--) /* the one that failed didn't move */
    if (top_level(&m->t->items[i])) rename_entry(&m->t->items[i], m->dst, m->src);
  return st;
}

/* Removes entries [0, end) of the tree from dir, last first. Returns how many files stayed. */
static int remove_entries(mover *m, const char *dir, int end) {
  int stayed = 0;
  for (int i = end - 1; i >= 0; i--) {
    const entry *e = &m->t->items[i];
    char *path = join(dir, e->rel);
    if (!path) {
      stayed += !e->is_dir;
      continue;
    }
    if (e->is_dir) mc_rmdir(path); /* fails if something stayed in it */
    else if (mc_remove(path) != MC_OK && mc_file_exists(path)) stayed++;
    free(path);
  }
  return stayed;
}

/* Copies everything into dst, checking each copy's size. On failure removes the copies. */
static mc_status copy_all(mover *m) {
  int n = 0; /* files so far */
  for (int i = 0; i < m->t->count; i++) {
    const entry *e = &m->t->items[i];
    char *from = join(m->src, e->rel), *to = join(m->dst, e->rel);
    mc_status st = from && to ? MC_OK : MC_ERR_NOMEM;
    if (st == MC_OK && e->is_dir) {
      st = mc_mkdirs(to);
    } else if (st == MC_OK) {
      report(m, MC_MOVE_PHASE_COPYING, e->rel);
      int64_t size = -1, mtime;
      if (mc_move_test_fail_at > 0 && ++n == mc_move_test_fail_at) st = MC_ERR_IO;
      else st = mc_copy_file(from, to, 0);
      if (st == MC_OK && (mc_file_info(to, &size, &mtime) != MC_OK || size != e->size)) st = MC_ERR_IO;
      m->p.files_done++;
      m->p.bytes_done += e->size;
    }
    if (st != MC_OK) {
      m->result->failed_path = from;
      free(to);
      report(m, MC_MOVE_PHASE_UNDOING, NULL);
      remove_entries(m, m->dst, i + 1);
      return st;
    }
    free(from);
    free(to);
  }
  report(m, MC_MOVE_PHASE_COPYING, NULL);
  return MC_OK;
}

/* Makes absolute files.path values relative, if they're inside the library's old folder.
 * The library has always stored them relative; this is for a database written otherwise. */
static mc_status fix_paths(mover *m, mc_library *lib) {
  sqlite3 *db = mc_library_db(lib);
  sqlite3_stmt *sel = NULL, *upd = NULL;
  if (sqlite3_prepare_v2(db,
                         "SELECT uid, path FROM files"
                         " WHERE path LIKE '/%' OR path LIKE '\\%' OR path GLOB '[A-Za-z]:*'",
                         -1, &sel, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  if (sqlite3_prepare_v2(db, "UPDATE OR IGNORE files SET path = ?2 WHERE uid = ?1", -1, &upd, NULL) != SQLITE_OK) {
    sqlite3_finalize(sel);
    return MC_ERR_DB;
  }
  mc_status st = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK ? MC_OK : MC_ERR_DB;
  const char *dirs[] = {m->src, m->src_given};
  int rc;
  while (st == MC_OK && (rc = sqlite3_step(sel)) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(sel, 0), *path = (const char *)sqlite3_column_text(sel, 1);
    for (size_t d = 0; d < sizeof dirs / sizeof dirs[0] && path; d++) {
      if (!is_inside(path, dirs[d])) continue;
      size_t n = strlen(dirs[d]);
      char *rel = mc_strdup(path + n + !is_sep(dirs[d][n - 1]));
      if (!rel) {
        st = MC_ERR_NOMEM;
        break;
      }
      for (char *c = rel; *c; c++)
        if (*c == '\\') *c = '/';
      sqlite3_bind_text(upd, 1, uid, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(upd, 2, rel, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(upd) != SQLITE_DONE) st = MC_ERR_DB;
      sqlite3_reset(upd);
      free(rel);
      break;
    }
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(sel);
  sqlite3_finalize(upd);
  sqlite3_exec(db, st == MC_OK ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
  return st;
}

/* Opens the moved database, checks it and fixes up its paths. */
static mc_status check_db(mover *m) {
  report(m, MC_MOVE_PHASE_CHECKING, NULL);
  char *db_path = join(m->dst, MC_LIBRARY_DB_NAME);
  int has_db = db_path && mc_file_exists(db_path);
  free(db_path);
  if (!has_db) return MC_OK; /* a library that has never been opened */

  mc_library *lib = NULL;
  mc_status st = mc_library_open(m->dst, &lib);
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(mc_library_db(lib), "PRAGMA quick_check", -1, &stmt, NULL) != SQLITE_OK ||
      sqlite3_step(stmt) != SQLITE_ROW || strcmp((const char *)sqlite3_column_text(stmt, 0), "ok") != 0)
    st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st == MC_OK) st = fix_paths(m, lib);
  mc_library_close(lib);
  return st;
}

/* Removes dst and the folders above it that the move made, up to and including top. */
static void remove_made(const char *dst, const char *top) {
  char *p = mc_strdup(dst);
  while (p && mc_rmdir(p) == MC_OK && !mc_same_path(p, top)) {
    char *cut = p + strlen(p);
    while (cut > p && !is_sep(cut[-1])) cut--;
    if (cut <= p + 1) break;
    cut[-1] = '\0';
  }
  free(p);
}

mc_status mc_library_move(const char *src, const char *dst, mc_settings *settings,
                          mc_library_move_fn progress, void *user, mc_library_move_result *result) {
  if (!result) return MC_ERR_INVALID_ARG;
  memset(result, 0, sizeof *result);
  mc_library_move_info info;
  tree t = {0};
  char *src_full = NULL, *dst_full = NULL, *made = NULL;
  mc_status st = check(src, dst, &info, &t, &src_full);
  if (st == MC_OK && info.problem != MC_MOVE_OK) {
    result->problem = info.problem;
    st = MC_ERR_INVALID_ARG;
  }
  if (st == MC_OK && !(dst_full = mc_full_path(dst))) st = MC_ERR_NOMEM;
  if (st != MC_OK) goto done;

  /* Make dst, remembering the topmost folder that's new, for an undo. */
  if (!mc_file_exists(dst_full)) {
    char *base = existing_parent(dst_full);
    size_t n = base ? strlen(base) : 0;
    const char *rest = dst_full + n;
    while (*rest && is_sep(*rest)) rest++;
    while (*rest && !is_sep(*rest)) rest++;
    if (base && (made = malloc((size_t)(rest - dst_full) + 1))) {
      memcpy(made, dst_full, (size_t)(rest - dst_full));
      made[rest - dst_full] = '\0';
    }
    free(base);
    if (!made) st = MC_ERR_NOMEM;
    else if ((st = mc_mkdirs(dst_full)) != MC_OK) result->failed_path = mc_strdup(dst);
    if (st != MC_OK) goto done;
  }

  mover m = {src_full, dst_full, src, &t, progress, user, result, {0}};
  int copied = 0;
  m.p.files_total = t.files;
  m.p.bytes_total = t.bytes;
  if (info.same_volume) result->renamed = rename_all(&m) == MC_OK;
  if (!result->renamed) {
    /* Copying after all (a rename failed): that needs the room the check didn't ask for. */
    int64_t free_bytes = -1;
    if (info.same_volume && mc_free_space(dst_full, &free_bytes) == MC_OK && free_bytes < needed_for(t.bytes)) {
      result->problem = MC_MOVE_NO_SPACE;
      st = MC_ERR_IO;
    } else {
      copied = (st = copy_all(&m)) == MC_OK; /* if not, it removed what it copied */
    }
  }
  if (st == MC_OK) st = check_db(&m);
  if (st == MC_OK && settings) {
    st = mc_settings_set_library_dir(settings, dst);
    if (st == MC_OK) st = mc_settings_save(settings);
    if (st != MC_OK) mc_settings_set_library_dir(settings, src); /* as it is on disk */
  }

  if (st != MC_OK) {
    if (result->renamed) {
      report(&m, MC_MOVE_PHASE_UNDOING, NULL);
      for (int i = t.count - 1; i >= 0; i--)
        if (top_level(&t.items[i])) rename_entry(&t.items[i], dst_full, src_full);
      result->renamed = 0;
    } else if (copied) { /* then the check or the settings failed */
      report(&m, MC_MOVE_PHASE_UNDOING, NULL);
      remove_entries(&m, dst_full, t.count);
    }
    if (made) remove_made(dst_full, made);
  } else if (!result->renamed) {
    report(&m, MC_MOVE_PHASE_REMOVING, NULL);
    result->left_behind = remove_entries(&m, src_full, t.count);
  }

done:
  free(made);
  free(dst_full);
  free(src_full);
  tree_free(&t);
  return st;
}

void mc_library_move_result_free(mc_library_move_result *result) {
  if (!result) return;
  mc_free(result->failed_path);
  result->failed_path = NULL;
}
