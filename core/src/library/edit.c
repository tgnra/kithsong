/* Hand edits (musiccore/edit.h; docs/tagging-rules.md, "Hand edits").
 *
 * An edit changes rows, and through them what this device's files say and where they are, and
 * has to do all of that or none of it. So it goes in three steps, on a connection of its own
 * (it holds a write transaction, and the frontend's handle may be in use on another thread):
 *
 *   plan     in a write transaction, the rows are changed and what that changes for each file
 *            is worked out by comparing the tags and paths of before and after; then it's
 *            rolled back. With no file to touch it's committed instead, and that's the edit.
 *   prepare  with no lock held, a tagged copy of each file is made beside it. This is the slow
 *            part, and everything that's likely to fail fails here, with nothing changed.
 *   swap     in a second write transaction the rows are changed again, the plan checked, and
 *            only renames are left: every file to a backup name, then every copy into its
 *            place, then the commit. A rename that fails puts everything back and rolls back.
 *
 * Backups, not rename-over: a rename can then fail on any file of the album with nothing lost,
 * and new names may be other tracks' old ones. They go when the commit is through. meta's
 * edit_swap lists them meanwhile, so a crash is put right at the next open (mc_edit_recover()):
 * it reads "undo" until the commit, which changes it to "done" in the same transaction. */
#include "library/folder_gate.h"
#include "library/edit_internal.h"
#include "kithcore/edit.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/tagger.h"
#include "library/library_internal.h"
#include "sync/transfer.h"
#include "tagger/coverart.h"
#include "tagger/credit.h"
#include "tagger/dupes.h"
#include "tagger/import_db.h"
#include "tagger/match.h"
#include "tagger/musicbrainz.h"
#include "tagger/organize.h"
#include "util/platform.h"

#define KEY_SWAP MC_EDIT_SWAP_META
#define RELEASE_LOOKUPS 5 /* a search looks up this many; each costs a second */
#define MAX_ATTEMPTS 3    /* plans that may turn out out of date before giving up */
#define ABANDONED ((mc_status)-1000) /* mc_edit_test_abandon_rename went off */

int mc_edit_test_fail_tags;
int mc_edit_test_fail_rename;
int mc_edit_test_abandon_rename;
mc_status (*mc_edit_trash)(const char *path) = mc_trash;

/* One edit at a time in the process, so two can't pick the same new path, and a library opened
 * meanwhile doesn't take an edit under way for a crashed one. */
static mc_mutex g_lock;
static int g_ready;

void mc_edit_global_init(void) {
  if (g_ready) return;
  mc_mutex_init(&g_lock);
  g_ready = 1;
}

/* --- Small helpers --- */

static sqlite3_stmt *prepare(mc_library *lib, const char *sql) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
  return stmt;
}

static mc_status step_done(sqlite3_stmt *stmt) {
  if (!stmt) return MC_ERR_DB;
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

static char *col_dup(sqlite3_stmt *stmt, int col) {
  const unsigned char *v = sqlite3_column_text(stmt, col);
  return mc_strdup(v ? (const char *)v : "");
}

static void bind_str(sqlite3_stmt *stmt, int i, const char *s) {
  sqlite3_bind_text(stmt, i, s, -1, SQLITE_STATIC);
}

/* "" or NULL is none. */
static void bind_opt(sqlite3_stmt *stmt, int i, const char *s) {
  if (s && *s) bind_str(stmt, i, s);
  else sqlite3_bind_null(stmt, i);
}

/* 0 is unknown. */
static void bind_num(sqlite3_stmt *stmt, int i, int v) {
  if (v > 0) sqlite3_bind_int(stmt, i, v);
  else sqlite3_bind_null(stmt, i);
}

static int same(const char *a, const char *b) {
  return !strcmp(a ? a : "", b ? b : "");
}

static const char *or_empty(const char *s) {
  return s ? s : "";
}

/* --- What's watching --- */

void mc_edit_set_watch(mc_library *lib, const mc_edit_watch *watch) {
  if (lib) lib->edit_watch = watch;
}

static int watch_cancelled(const mc_library *lib) {
  const mc_edit_watch *w = lib->edit_watch;
  return w && w->cancelled && w->cancelled(w->user);
}

static void watch_progress(const mc_library *lib, int done, int total) {
  const mc_edit_watch *w = lib->edit_watch;
  if (w && w->progress) w->progress(w->user, done, total);
}

/* An mb_call the watch can give up on; both of them are the caller's, to outlast the call. */
static mb_call *watch_call(const mc_library *lib, mc_http_cancel *cancel, mb_call *call) {
  const mc_edit_watch *w = lib->edit_watch;
  memset(call, 0, sizeof *call);
  if (w && w->cancelled) {
    *cancel = (mc_http_cancel){w->cancelled, w->user};
    call->cancel = cancel;
  }
  return call;
}

typedef struct uids {
  char (*items)[MC_UID_STRLEN];
  int count, cap;
} uids;

static int uids_has(const uids *l, const char *uid) {
  for (int i = 0; i < l->count; i++)
    if (!strcmp(l->items[i], uid)) return 1;
  return 0;
}

static int uids_add(uids *l, const char *uid) {
  if (!uid || strlen(uid) >= MC_UID_STRLEN) return 1;
  if (uids_has(l, uid)) return 1;
  if (l->count == l->cap) {
    int grown = l->cap ? l->cap * 2 : 16;
    char (*more)[MC_UID_STRLEN] = realloc(l->items, (size_t)grown * sizeof *more);
    if (!more) return 0;
    l->items = more;
    l->cap = grown;
  }
  strcpy(l->items[l->count++], uid);
  return 1;
}

/* Adds column 0 of every row of a query with ?1 bound to arg. */
static mc_status uids_query(mc_library *lib, uids *l, const char *sql, const char *arg) {
  sqlite3_stmt *stmt = prepare(lib, sql);
  if (!stmt) return MC_ERR_DB;
  bind_str(stmt, 1, arg);
  mc_status st = MC_OK;
  int rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW)
    if (!uids_add(l, (const char *)sqlite3_column_text(stmt, 0))) st = MC_ERR_NOMEM;
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

static char *join_path(const char *a, const char *b) {
  if (!a || !b) return NULL;
  size_t len = strlen(a) + strlen(b) + 2;
  char *out = malloc(len);
  if (out) snprintf(out, len, "%s/%s", a, b);
  return out;
}

/* rel's folder, malloc'd; "" for one at the top. */
static char *folder_of(const char *rel) {
  const char *slash = strrchr(rel, '/');
  size_t n = slash ? (size_t)(slash - rel) : 0;
  char *out = malloc(n + 1);
  if (out) {
    memcpy(out, rel, n);
    out[n] = '\0';
  }
  return out;
}

static int rel_exists(mc_library *lib, const char *rel) {
  char *abs = mc_library_abspath(lib, rel);
  int yes = abs && mc_file_exists(abs);
  free(abs);
  return yes;
}

/* --- What a track's file says, and where it goes --- */

/* The tags mc_tags_write() writes that an edit can change. */
static const size_t k_tag_strings[] = {
  offsetof(mc_tags, title),         offsetof(mc_tags, artist),
  offsetof(mc_tags, album),         offsetof(mc_tags, album_artist),
  offsetof(mc_tags, genre),         offsetof(mc_tags, mb_trackid),
  offsetof(mc_tags, mb_releasetrackid), offsetof(mc_tags, mb_albumid),
  offsetof(mc_tags, mb_artistid),   offsetof(mc_tags, mb_albumartistid),
  offsetof(mc_tags, mb_releasegroupid), offsetof(mc_tags, artists),
  offsetof(mc_tags, album_artists), offsetof(mc_tags, mb_artistids),
  offsetof(mc_tags, mb_albumartistids),
};
#define TAG_STRINGS (sizeof k_tag_strings / sizeof k_tag_strings[0])

static char **tag_string(const mc_tags *t, size_t i) {
  return (char **)((char *)t + k_tag_strings[i]);
}

/* What was to now: only the tags that differ, those gone as "" or -1, which is what
 * mc_tags_write() removes. So an edit leaves alone whatever else the file says, the rows having
 * no say in it. *changed 0 if nothing differs. */
static mc_status tags_delta(const mc_tags *was, const mc_tags *now, mc_tags *out, int *changed) {
  memset(out, 0, sizeof *out);
  *changed = 0;
  for (size_t i = 0; i < TAG_STRINGS; i++) {
    const char *a = *tag_string(was, i), *b = *tag_string(now, i);
    if (same(a, b)) continue;
    *changed = 1;
    if (!(*tag_string(out, i) = mc_strdup(or_empty(b)))) {
      mc_tags_clear(out);
      return MC_ERR_NOMEM;
    }
  }
  if (was->year != now->year) {
    out->year = now->year ? now->year : -1;
    *changed = 1;
  }
  if (was->original_year != now->original_year) {
    out->original_year = now->original_year ? now->original_year : -1;
    *changed = 1;
  }
  /* A number goes with its total, in one tag. */
  if (was->track != now->track || was->track_total != now->track_total) {
    out->track = now->track ? now->track : -1;
    out->track_total = now->track_total;
    *changed = 1;
  }
  if (was->disc != now->disc || was->disc_total != now->disc_total) {
    out->disc = now->disc ? now->disc : -1;
    out->disc_total = now->disc_total;
    *changed = 1;
  }
  return MC_OK;
}

static int tags_equal(const mc_tags *a, const mc_tags *b) {
  for (size_t i = 0; i < TAG_STRINGS; i++) {
    const char *x = *tag_string(a, i), *y = *tag_string(b, i);
    if (!x != !y || !same(x, y)) return 0;
  }
  return a->year == b->year && a->original_year == b->original_year && a->track == b->track &&
         a->track_total == b->track_total && a->disc == b->disc && a->disc_total == b->disc_total;
}

typedef struct file_snap {
  char uid[MC_UID_STRLEN];
  char *path;  /* relative, as in its row */
  char *filed; /* where organize.c puts it, as the rows are */
  int transcode;
  int64_t size, mtime;
  int has_mtime;
  char sha[MC_SHA256_STRLEN]; /* "" if none is recorded */
} file_snap;

typedef struct track_snap {
  char uid[MC_UID_STRLEN];
  mc_tags tags;
  file_snap *files;
  int file_count;
} track_snap;

/* A snapshot of the tracks an edit may change, each with its files. */
typedef struct snapshot {
  track_snap *tracks;
  int count;
} snapshot;

static void snapshot_clear(snapshot *s) {
  for (int i = 0; i < s->count; i++) {
    track_snap *t = &s->tracks[i];
    for (int j = 0; j < t->file_count; j++) {
      free(t->files[j].path);
      free(t->files[j].filed);
    }
    free(t->files);
    mc_tags_clear(&t->tags);
  }
  free(s->tracks);
  memset(s, 0, sizeof *s);
}

/* Where import and sync file a file of this track with extension ext (dest_path in transfer.c,
 * but for a path being taken); stmt is the row of TRACK_PLACE. */
#define TRACK_PLACE                                                                              \
  "SELECT t.title, t.track_no, t.disc_no, al.uid, al.title, al.year, al.disc_total,"            \
  " coalesce(al.artist_credit, aar.name), coalesce(t.artist_credit, ar.name, al.artist_credit, aar.name)" \
  " FROM tracks t LEFT JOIN albums al ON al.uid = t.album_uid"                                   \
  " LEFT JOIN artists aar ON aar.uid = al.artist_uid"                                            \
  " LEFT JOIN artists ar ON ar.uid = t.artist_uid WHERE t.uid = ?1"

static char *filed_path(sqlite3_stmt *stmt, const char *ext) {
  const char *title = (const char *)sqlite3_column_text(stmt, 0);
  const char *album_artist = (const char *)sqlite3_column_text(stmt, 7);
  const char *artist = (const char *)sqlite3_column_text(stmt, 8);
  if (!album_artist || !*album_artist) album_artist = artist;
  char *dir, *name;
  if (sqlite3_column_type(stmt, 3) == SQLITE_NULL) { /* no album */
    dir = mc_artist_dir(artist);
    name = mc_track_file(title, 0, 0, 0, ext);
  } else {
    dir = mc_album_dir(album_artist, (const char *)sqlite3_column_text(stmt, 4),
                       sqlite3_column_int(stmt, 5));
    name = mc_track_file(title, sqlite3_column_int(stmt, 2), sqlite3_column_int(stmt, 6),
                         sqlite3_column_int(stmt, 1), ext);
  }
  char *rel = join_path(dir, name);
  free(dir);
  free(name);
  return rel;
}

static const char *extension(const char *rel) {
  const char *slash = strrchr(rel, '/'), *dot = strrchr(rel, '.');
  return dot && (!slash || dot > slash) ? dot + 1 : "";
}

static mc_status read_files(mc_library *lib, track_snap *t) {
  sqlite3_stmt *where = prepare(lib, TRACK_PLACE);
  sqlite3_stmt *stmt = prepare(lib, "SELECT uid, path, source_file_uid IS NOT NULL, size_bytes, mtime_ms,"
                                    " mtime_ms IS NOT NULL, sha256 FROM files WHERE track_uid = ?1"
                                    " ORDER BY path");
  mc_status st = where && stmt ? MC_OK : MC_ERR_DB;
  if (st == MC_OK) {
    bind_str(where, 1, t->uid);
    bind_str(stmt, 1, t->uid);
    if (sqlite3_step(where) != SQLITE_ROW) st = MC_ERR_NOT_FOUND;
  }
  while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    const char *sha = (const char *)sqlite3_column_text(stmt, 6);
    if (!uid || strlen(uid) >= MC_UID_STRLEN) continue;
    file_snap *more = realloc(t->files, (size_t)(t->file_count + 1) * sizeof *more);
    if (!more) {
      st = MC_ERR_NOMEM;
      break;
    }
    t->files = more;
    file_snap *f = &t->files[t->file_count++];
    memset(f, 0, sizeof *f);
    strcpy(f->uid, uid);
    f->path = col_dup(stmt, 1);
    f->transcode = sqlite3_column_int(stmt, 2);
    f->size = sqlite3_column_int64(stmt, 3);
    f->mtime = sqlite3_column_int64(stmt, 4);
    f->has_mtime = sqlite3_column_int(stmt, 5);
    if (sha && strlen(sha) == MC_SHA256_STRLEN - 1) strcpy(f->sha, sha);
    f->filed = f->path ? filed_path(where, extension(f->path)) : NULL;
    if (!f->path || !f->filed) st = MC_ERR_NOMEM;
  }
  sqlite3_finalize(where);
  sqlite3_finalize(stmt);
  return st;
}

static mc_status snapshot_read(mc_library *lib, const uids *tracks, snapshot *out) {
  memset(out, 0, sizeof *out);
  if (!tracks->count) return MC_OK;
  out->tracks = calloc((size_t)tracks->count, sizeof *out->tracks);
  if (!out->tracks) return MC_ERR_NOMEM;
  mc_status st = MC_OK;
  for (int i = 0; st == MC_OK && i < tracks->count; i++) {
    track_snap *t = &out->tracks[out->count];
    strcpy(t->uid, tracks->items[i]);
    if ((st = mc_dupes_library_tags(lib, t->uid, &t->tags)) != MC_OK) break;
    out->count++;
    st = read_files(lib, t);
  }
  if (st != MC_OK) snapshot_clear(out);
  return st;
}

/* --- The plan --- */

typedef struct job {
  char file[MC_UID_STRLEN];
  char *rel;  /* where it is */
  char *dest; /* where it's going: the same, for one only tagged */
  char *tmp;  /* its new copy, beside it */
  char *bak;  /* the name it has during the swap */
  int fresh;  /* there's nothing at rel yet: a cover for a folder with none */
  int cover;  /* a cover, so what it replaces goes to the trash, not away for good */
  int tagged;
  mc_tags tags; /* what to write, if tagged */
  char sha[MC_SHA256_STRLEN];     /* its row's */
  char new_sha[MC_SHA256_STRLEN]; /* the copy's, if it has a recorded one */
  int64_t new_size, new_mtime;
} job;

typedef struct plan {
  job *jobs;
  int count;
} plan;

static void plan_clear(plan *p) {
  for (int i = 0; i < p->count; i++) {
    job *j = &p->jobs[i];
    free(j->rel);
    free(j->dest);
    free(j->tmp);
    free(j->bak);
    mc_tags_clear(&j->tags);
  }
  free(p->jobs);
  memset(p, 0, sizeof *p);
}

static int same_rel(const char *a, const char *b) {
  return mc_same_path(a, b);
}

/* Whether a files row other than the edit's has rel. */
static int path_in_files(mc_library *lib, const char *rel) {
#ifdef _WIN32
  sqlite3_stmt *stmt = prepare(lib, "SELECT 1 FROM files WHERE path = ?1 COLLATE NOCASE");
#else
  sqlite3_stmt *stmt = prepare(lib, "SELECT 1 FROM files WHERE path = ?1");
#endif
  if (!stmt) return 1;
  bind_str(stmt, 1, rel);
  int yes = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return yes;
}

/* Whether job n may not have path: a job before it was given it, or something else is there. A
 * file the edit moves away doesn't count, so tracks can trade places. */
static int path_taken(mc_library *lib, const plan *p, int n, const char *path) {
  for (int i = 0; i < n; i++)
    if (same_rel(p->jobs[i].dest, path)) return 1;
  for (int i = 0; i < p->count; i++) {
    const job *j = &p->jobs[i];
    if (i != n && !strcmp(j->dest, j->rel) && same_rel(j->dest, path)) return 1; /* staying */
    if (same_rel(j->rel, path)) return 0;
  }
  return path_in_files(lib, path) || rel_exists(lib, path);
}

/* Gives each job that moves its place: job n's dest is where the rows want it, or if that's
 * taken "name (2).ext" and so on, as import and sync do. */
static mc_status place_jobs(mc_library *lib, plan *p) {
  for (int n = 0; n < p->count; n++) {
    job *j = &p->jobs[n];
    if (!strcmp(j->dest, j->rel) || !path_taken(lib, p, n, j->dest)) continue;
    const char *dot = strrchr(j->dest, '.');
    size_t base = dot ? (size_t)(dot - j->dest) : strlen(j->dest), len = strlen(j->dest) + 16;
    char *alt = malloc(len);
    int found = 0;
    for (int k = 2; alt && !found && k < 1000; k++) {
      snprintf(alt, len, "%.*s (%d)%s", (int)base, j->dest, k, dot ? dot : "");
      found = !path_taken(lib, p, n, alt);
    }
    if (!found) {
      free(alt);
      return alt ? MC_ERR_IO : MC_ERR_NOMEM;
    }
    free(j->dest);
    j->dest = alt;
  }
  return MC_OK;
}

/* Whether f is here and as its row says: inside the library folder, the size and mtime it
 * should have and, to be tagged, writable. A file that isn't is left as it is, its row too, as
 * the artist refresh leaves it: the rows are what's shown either way. */
static int file_usable(mc_library *lib, const file_snap *f, int tagged) {
  char *abs = mc_transfer_deletable_path(lib, f->path);
  int64_t size, mtime;
  int ok = abs && mc_file_info(abs, &size, &mtime) == MC_OK && size == f->size &&
           (!f->has_mtime || mtime == f->mtime);
  if (ok && tagged) {
    FILE *probe = mc_fopen(abs, "r+b");
    if (probe) fclose(probe);
    else ok = 0;
  }
  free(abs);
  return ok;
}

static int file_open(mc_library *lib, const char *rel) {
  char *abs = mc_library_abspath(lib, rel);
  int yes = abs && mc_file_in_use(abs);
  free(abs);
  return yes;
}

/* The jobs that take the files from before to after. Only an original is tagged, and a file is
 * moved only if the edit changes where it's filed, so one that's filed elsewhere for a reason
 * (a " (2)", an older version's name) stays there through edits that don't concern it. skip
 * holds the files found unusable on an earlier attempt. MC_ERR_IN_USE for a file that's open. */
static mc_status make_plan(mc_library *lib, const snapshot *before, const snapshot *after,
                           const uids *skip, plan *out) {
  memset(out, 0, sizeof *out);
  mc_status st = MC_OK;
  for (int i = 0; st == MC_OK && i < before->count && i < after->count; i++) {
    const track_snap *b = &before->tracks[i], *a = &after->tracks[i];
    if (strcmp(b->uid, a->uid) || b->file_count != a->file_count) continue;
    for (int k = 0; st == MC_OK && k < b->file_count; k++) {
      const file_snap *f = &b->files[k];
      if (uids_has(skip, f->uid)) continue;
      int retag, moves = strcmp(f->filed, a->files[k].filed) != 0 && strcmp(f->path, a->files[k].filed) != 0;
      mc_tags delta;
      if ((st = tags_delta(&b->tags, &a->tags, &delta, &retag)) != MC_OK) break;
      if (f->transcode) retag = 0;
      if ((!retag && !moves) || !file_usable(lib, f, retag)) {
        mc_tags_clear(&delta);
        continue;
      }
      if (file_open(lib, f->path)) {
        mc_tags_clear(&delta);
        st = MC_ERR_IN_USE;
        break;
      }
      job *more = realloc(out->jobs, (size_t)(out->count + 1) * sizeof *more);
      if (!more) {
        mc_tags_clear(&delta);
        st = MC_ERR_NOMEM;
        break;
      }
      out->jobs = more;
      job *j = &out->jobs[out->count++];
      memset(j, 0, sizeof *j);
      strcpy(j->file, f->uid);
      strcpy(j->sha, f->sha);
      j->rel = mc_strdup(f->path);
      j->dest = mc_strdup(moves ? a->files[k].filed : f->path);
      j->tagged = retag;
      if (retag) j->tags = delta;
      else mc_tags_clear(&delta);
      if (!j->rel || !j->dest) st = MC_ERR_NOMEM;
    }
  }
  if (st == MC_OK) st = place_jobs(lib, out);
  if (st != MC_OK) plan_clear(out);
  return st;
}

static int plans_equal(const plan *a, const plan *b) {
  if (a->count != b->count) return 0;
  for (int i = 0; i < a->count; i++) {
    const job *x = &a->jobs[i], *y = &b->jobs[i];
    if (strcmp(x->file, y->file) || strcmp(x->rel, y->rel) || strcmp(x->dest, y->dest) ||
        x->tagged != y->tagged || !tags_equal(&x->tags, &y->tags))
      return 0;
  }
  return 1;
}

/* What an edit does: scope lists the tracks it may change, apply changes the rows. Both run in
 * the write transaction, and apply twice, so it mustn't change what user holds. */
typedef struct edit_ops {
  mc_status (*scope)(mc_library *lib, void *user, uids *tracks);
  mc_status (*apply)(mc_library *lib, void *user);
} edit_ops;

/* Changes the rows and works out the plan. With no files to touch the edit is committed and
 * out empty. Otherwise it's rolled back, unless keep is set, when the transaction stays open
 * for the swap. */
static mc_status plan_edit(mc_library *lib, const edit_ops *ops, void *user, const uids *skip,
                           int keep, plan *out) {
  memset(out, 0, sizeof *out);
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  uids tracks = {0};
  snapshot before = {0}, after = {0};
  st = ops->scope(lib, user, &tracks);
  if (st == MC_OK) st = snapshot_read(lib, &tracks, &before);
  if (st == MC_OK) st = ops->apply(lib, user);
  if (st == MC_OK) st = snapshot_read(lib, &tracks, &after);
  if (st == MC_OK) st = make_plan(lib, &before, &after, skip, out);
  snapshot_clear(&before);
  snapshot_clear(&after);
  free(tracks.items);
  if (st == MC_OK && !out->count) {
    if ((st = mc_library_commit(lib)) != MC_OK) mc_library_rollback(lib);
    return st;
  }
  if (st != MC_OK || !keep) mc_library_rollback(lib);
  return st;
}

/* --- Preparing the copies --- */

/* "<folder>/.kithsong-<random>-<name><suffix>". */
static char *name_beside(const char *rel, const unsigned char rnd[4], const char *suffix) {
  const char *slash = strrchr(rel, '/');
  const char *name = slash ? slash + 1 : rel;
  size_t n = strlen(rel) + strlen(suffix) + 32;
  char *out = malloc(n);
  if (out)
    snprintf(out, n, "%.*s.kithsong-%02x%02x%02x%02x-%s%s", (int)(name - rel), rel, rnd[0], rnd[1],
             rnd[2], rnd[3], name, suffix);
  return out;
}

/* A copy keeps the extension, for TagLib; a backup mustn't have one sync's scan for unknown
 * files would take for music (it may be months old, and the scan would trash it). */
static mc_status name_jobs(plan *p) {
  for (int i = 0; i < p->count; i++) {
    job *j = &p->jobs[i];
    unsigned char rnd[4];
    mc_status st = mc_random_bytes(rnd, sizeof rnd);
    if (st != MC_OK) return st;
    j->tmp = name_beside(j->rel, rnd, "");
    j->bak = name_beside(j->rel, rnd, ".old");
    if (!j->tmp || !j->bak) return MC_ERR_NOMEM;
  }
  return MC_OK;
}

static int tag_writes;

/* Makes j's new copy. A file whose bytes aren't those its row's hash vouches for is set aside,
 * *unusable: tagged, it would get a new hash and the deep check would never hear of it. */
static mc_status prepare_job(mc_library *lib, job *j, int *unusable) {
  char *abs = mc_library_abspath(lib, j->rel);
  char *tmp = mc_library_abspath(lib, j->tmp);
  mc_status st = abs && tmp ? mc_copy_file(abs, tmp, 0) : MC_ERR_NOMEM;
  if (st == MC_OK) st = mc_clear_read_only(tmp);
  if (st == MC_OK) st = mc_touch(tmp); /* or the copy has the old mtime: see name_jobs() */
  if (st == MC_OK && j->sha[0]) {
    char sha[MC_SHA256_STRLEN];
    if ((st = mc_sha256_file(tmp, sha)) == MC_OK && strcmp(sha, j->sha)) *unusable = 1;
    strcpy(j->new_sha, j->sha);
  }
  if (st == MC_OK && !*unusable && j->tagged) {
    st = ++tag_writes == mc_edit_test_fail_tags ? MC_ERR_IO : mc_tags_write(tmp, &j->tags);
    if (st == MC_OK && j->sha[0]) st = mc_sha256_file(tmp, j->new_sha);
  }
  free(abs);
  free(tmp);
  return st;
}

/* --- The swap --- */

/* The jobs as meta's edit_swap has them: state, then a line of paths for each, and "new" after
 * them for a fresh one. */
static char *journal_text(const plan *p, const char *state) {
  size_t len = strlen(state) + 2;
  for (int i = 0; i < p->count; i++) {
    const job *j = &p->jobs[i];
    len += strlen(j->rel) + strlen(j->dest) + strlen(j->tmp) + strlen(j->bak) + 9;
  }
  char *out = malloc(len);
  if (!out) return NULL;
  size_t n = (size_t)snprintf(out, len, "%s\n", state);
  for (int i = 0; i < p->count; i++) {
    const job *j = &p->jobs[i];
    n += (size_t)snprintf(out + n, len - n, "%s\t%s\t%s\t%s%s\n", j->rel, j->dest, j->tmp, j->bak,
                          j->fresh ? "\tnew" : "");
  }
  return out;
}

static mc_status journal_write(mc_library *lib, const plan *p, const char *state) {
  char *text = journal_text(p, state);
  if (!text) return MC_ERR_NOMEM;
  mc_status st = mc_library_meta_set(lib, KEY_SWAP, text);
  free(text);
  return st;
}

/* Puts the files back as they were, whatever step the swap got to, and can be interrupted and
 * done again: a copy that's gone from tmp with its backup still there is in its place, so it
 * goes back to tmp first (dest may be another job's rel), then the backups go back, then the
 * copies are deleted. Returns whether every file is back. */
static int undo_jobs(mc_library *lib, const plan *p) {
  int all = 1;
  for (int i = p->count - 1; i >= 0; i--) {
    const job *j = &p->jobs[i];
    if (rel_exists(lib, j->tmp) || (!j->fresh && !rel_exists(lib, j->bak))) continue;
    char *dest = mc_library_abspath(lib, j->dest), *tmp = mc_library_abspath(lib, j->tmp);
    if (dest && tmp && mc_file_exists(dest)) mc_rename(dest, tmp, 0);
    free(dest);
    free(tmp);
  }
  for (int i = p->count - 1; i >= 0; i--) {
    const job *j = &p->jobs[i];
    if (!rel_exists(lib, j->bak)) continue;
    char *bak = mc_library_abspath(lib, j->bak), *rel = mc_library_abspath(lib, j->rel);
    if (!bak || !rel || mc_rename(bak, rel, 0) != MC_OK) {
      fprintf(stderr, "musiccore: edit: couldn't put back %s\n", j->rel);
      all = 0;
    }
    free(bak);
    free(rel);
  }
  for (int i = 0; i < p->count; i++) {
    const job *j = &p->jobs[i];
    if (rel_exists(lib, j->tmp)) {
      char *tmp = mc_library_abspath(lib, j->tmp);
      if (tmp) mc_remove(tmp);
      free(tmp);
    }
    if (strcmp(j->rel, j->dest)) mc_transfer_prune_dirs(lib, j->dest); /* folders made for it */
  }
  return all;
}

static int swap_renames;

/* A rename of the swap, with the test hooks. */
static mc_status swap_rename(mc_library *lib, const char *from, const char *to) {
  swap_renames++;
  if (mc_edit_test_abandon_rename && swap_renames == mc_edit_test_abandon_rename) return ABANDONED;
  if (mc_edit_test_fail_rename && swap_renames == mc_edit_test_fail_rename) return MC_ERR_IO;
  char *src = mc_library_abspath(lib, from), *dst = mc_library_abspath(lib, to);
  mc_status st = src && dst ? mc_rename_in_place(src, dst) : MC_ERR_NOMEM;
  free(src);
  free(dst);
  return st;
}

/* Every file to its backup name, then every copy into place. All the old ones go first, as a
 * new name may be another track's old one. An open file is what stops the first rename on
 * Windows, nothing having been lost yet. */
static mc_status swap_files(mc_library *lib, plan *p) {
  swap_renames = 0;
  for (int i = 0; i < p->count; i++) {
    job *j = &p->jobs[i];
    if (j->fresh) continue;
    mc_status st = swap_rename(lib, j->rel, j->bak);
    if (st == MC_ERR_IO && file_open(lib, j->rel)) st = MC_ERR_IN_USE;
    if (st != MC_OK) return st;
  }
  for (int i = 0; i < p->count; i++) {
    job *j = &p->jobs[i];
    char *dest = mc_library_abspath(lib, j->dest);
    char *folder = folder_of(j->dest);
    char *dir = folder ? mc_library_abspath(lib, folder) : NULL;
    mc_status st = dest && dir ? mc_mkdirs(dir) : MC_ERR_NOMEM;
    free(folder);
    free(dir);
    if (st == MC_OK) st = swap_rename(lib, j->tmp, j->dest);
    if (st == MC_OK && mc_file_info(dest, &j->new_size, &j->new_mtime) != MC_OK) st = MC_ERR_IO;
    free(dest);
    if (st != MC_OK) return st;
  }
  return MC_OK;
}

/* The files rows after the swap. Same file, same uid and file_rev: to sync it's unchanged, and
 * peers keep what they have. source_sha256 stays, being what it was imported from. Paths go
 * through a name of the row's own first, as tracks trading places would break UNIQUE. */
static mc_status record_files(mc_library *lib, const plan *p) {
  mc_status st = MC_OK;
  for (int i = 0; st == MC_OK && i < p->count; i++) {
    sqlite3_stmt *stmt = prepare(lib, "UPDATE files SET path = ?2 WHERE uid = ?1");
    if (stmt) {
      bind_str(stmt, 1, p->jobs[i].file);
      bind_str(stmt, 2, p->jobs[i].bak);
    }
    st = step_done(stmt);
  }
  for (int i = 0; st == MC_OK && i < p->count; i++) {
    const job *j = &p->jobs[i];
    sqlite3_stmt *stmt = prepare(lib, "UPDATE files SET path = ?2, size_bytes = ?3, mtime_ms = ?4,"
                                      " sha256 = CASE WHEN sha256 IS NULL THEN NULL ELSE ?5 END"
                                      " WHERE uid = ?1");
    if (stmt) {
      bind_str(stmt, 1, j->file);
      bind_str(stmt, 2, j->dest);
      sqlite3_bind_int64(stmt, 3, j->new_size);
      sqlite3_bind_int64(stmt, 4, j->new_mtime);
      bind_opt(stmt, 5, j->new_sha);
    }
    st = step_done(stmt);
  }
  return st;
}

/* A cover moves with its album. It's copied, and pruning takes the old one with the folder: if
 * something else keeps that folder, it keeps its cover too. A cover already there stays. */
static void carry_cover(mc_library *lib, const job *j) {
  char *from_rel = folder_of(j->rel), *to_rel = folder_of(j->dest);
  char *from = from_rel && *from_rel ? mc_library_abspath(lib, from_rel) : NULL;
  char *to = to_rel && *to_rel ? mc_library_abspath(lib, to_rel) : NULL;
  if (from && to && strcmp(from_rel, to_rel)) {
    char *cover = mc_coverart_find(from), *there = mc_coverart_find(to);
    if (cover && !there) {
      const char *sep = strrchr(cover, MC_PATH_SEP);
      size_t len = strlen(to) + strlen(cover) + 2;
      char *dst = malloc(len);
      if (dst) {
        snprintf(dst, len, "%s%c%s", to, MC_PATH_SEP, sep ? sep + 1 : cover);
        mc_copy_file(cover, dst, 0);
      }
      free(dst);
    }
    free(cover);
    free(there);
  }
  free(from_rel);
  free(to_rel);
  free(from);
  free(to);
}

/* A folder a cover stayed in shows that cover, whatever the album moved in had: it's the album's
 * now. Only for an album's own files, and a cover the user chose is the choice all the same. */
static void follow_folder_covers(mc_library *lib, const plan *p) {
  char *last = NULL;
  for (int i = 0; i < p->count; i++) {
    const job *j = &p->jobs[i];
    char *from = folder_of(j->rel), *to = folder_of(j->dest);
    if (from && to && *to && !same_rel(from, to) && (!last || strcmp(last, to))) {
      char *dir = mc_library_abspath(lib, to);
      char *cover = dir ? mc_coverart_find(dir) : NULL;
      char hash[MC_ART_HASH_LEN];
      if (cover && mc_coverart_hash(cover, hash) == MC_OK && mc_library_begin_write(lib) == MC_OK) {
        sqlite3_stmt *stmt = prepare(lib, "UPDATE albums SET art_hash = ?2 WHERE art_chosen IS NULL"
                                          " AND art_hash IS NOT ?2 AND uid ="
                                          " (SELECT t.album_uid FROM files f JOIN tracks t ON t.uid = f.track_uid"
                                          "  WHERE f.uid = ?1 AND f.source_file_uid IS NULL)");
        if (stmt) {
          bind_str(stmt, 1, j->file);
          bind_str(stmt, 2, hash);
        }
        if (step_done(stmt) != MC_OK || mc_library_commit(lib) != MC_OK) mc_library_rollback(lib);
      }
      free(cover);
      free(dir);
      free(last);
      last = to;
      to = NULL;
    }
    free(from);
    free(to);
  }
  free(last);
}

/* Files moved into a folder whose name changes only in case go into it as it is, where the
 * system finds it, so the folder takes the new case itself: each part of the path that differs
 * so, for as long as the two are the same but for case. Others' rows may still say the old one,
 * which finds it just as well. Where case matters they're two folders, and nothing here applies. */
static void match_folder_case(mc_library *lib, const char *rel, const char *dest) {
  char *from = folder_of(rel), *to = folder_of(dest);
  char *old = from && to ? malloc(strlen(from) + strlen(to) + 2) : NULL;
  if (old) {
    const char *a = from, *b = to;
    while (*a && *b) {
      const char *a_end = strchr(a, '/'), *b_end = strchr(b, '/');
      size_t a_len = a_end ? (size_t)(a_end - a) : strlen(a), b_len = b_end ? (size_t)(b_end - b) : strlen(b);
      size_t done = (size_t)(b - to);
      char *x = malloc(a_len + 1), *y = malloc(b_len + 1);
      int same_bytes = 0, same_name = 0;
      if (x && y) {
        memcpy(x, a, a_len);
        x[a_len] = '\0';
        memcpy(y, b, b_len);
        y[b_len] = '\0';
        same_bytes = !strcmp(x, y);
        same_name = same_rel(x, y);
      }
      if (same_name && !same_bytes) {
        memcpy(old, to, done);
        memcpy(old + done, x, a_len + 1);
        char end = b[b_len];
        char *cut = to + done + b_len;
        *cut = '\0';
        char *src = mc_library_abspath(lib, old), *dst = mc_library_abspath(lib, to);
        if (src && dst) mc_rename_in_place(src, dst);
        free(src);
        free(dst);
        *cut = end;
      }
      free(x);
      free(y);
      if (!same_name || !a_end || !b_end) break;
      a = a_end + 1;
      b = b_end + 1;
    }
  }
  free(old);
  free(from);
  free(to);
}

/* A replaced cover goes to the trash, as something the user may want back: called what it is,
 * and not the backup's name. Where there's no trash, as on Android, it's deleted. */
static mc_status cover_away(mc_library *lib, const job *j, const char *bak) {
  const char *slash = strrchr(j->rel, '/');
  const char *name = slash ? slash + 1 : j->rel;
  char *folder = folder_of(j->rel);
  size_t len = strlen(j->rel) + 16;
  char *rel = folder ? malloc(len) : NULL, *named = NULL;
  if (rel) {
    snprintf(rel, len, "%s%sreplaced %s", folder, *folder ? "/" : "", name);
    named = mc_library_abspath(lib, rel);
  }
  free(folder);
  free(rel);
  mc_status st = MC_ERR_IO;
  if (named && mc_rename(bak, named, 1) == MC_OK) {
    st = mc_edit_trash(named);
    if (st != MC_OK) st = mc_remove(named);
  } else {
    st = mc_remove(bak);
  }
  free(named);
  return st;
}

/* After the commit: the backups go, then the folders that left empty. Returns whether every
 * backup went; a file that can't be deleted stays listed for the next open. */
static int finish_jobs(mc_library *lib, const plan *p, int covers) {
  int all = 1;
  for (int i = 0; i < p->count; i++) {
    const job *j = &p->jobs[i];
    char *bak = mc_library_abspath(lib, j->bak);
    if (bak && mc_file_exists(bak)) {
      mc_clear_read_only(bak);
      if ((j->cover ? cover_away(lib, j, bak) : mc_remove(bak)) != MC_OK) {
        fprintf(stderr, "musiccore: edit: couldn't delete %s\n", j->bak);
        all = 0;
      }
    }
    free(bak);
  }
  for (int i = 0; covers && i < p->count; i++) carry_cover(lib, &p->jobs[i]);
  for (int i = 0; i < p->count; i++) {
    if (!strcmp(p->jobs[i].rel, p->jobs[i].dest)) continue;
    mc_transfer_prune_dirs(lib, p->jobs[i].rel);
    match_folder_case(lib, p->jobs[i].rel, p->jobs[i].dest);
  }
  return all;
}

/* The temporary copies of an edit that won't go on, and its journal. */
static void discard_copies(mc_library *lib, const plan *p) {
  if (undo_jobs(lib, p)) mc_library_meta_set(lib, KEY_SWAP, NULL);
}

/* The swap, in the transaction plan_edit() left open. */
static mc_status swap_and_commit(mc_library *lib, plan *p) {
  mc_status st = swap_files(lib, p);
  if (st == ABANDONED) {
    mc_library_rollback(lib);
    return st;
  }
  if (st == MC_OK) st = record_files(lib, p);
  if (st == MC_OK) st = journal_write(lib, p, "done");
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) {
    mc_library_rollback(lib);
    discard_copies(lib, p);
    return st;
  }
  if (finish_jobs(lib, p, 1)) mc_library_meta_set(lib, KEY_SWAP, NULL);
  follow_folder_covers(lib, p);
  return MC_OK;
}

/* One try. *again is set if the plan went out of date, or a file turned out unusable. */
static mc_status attempt_edit(mc_library *lib, const edit_ops *ops, void *user, uids *skip, int *again) {
  *again = 0;
  plan first, second;
  mc_status st = plan_edit(lib, ops, user, skip, 0, &first);
  if (st != MC_OK || !first.count) return st;
  if ((st = name_jobs(&first)) == MC_OK) st = journal_write(lib, &first, "undo");
  if (st != MC_OK) {
    plan_clear(&first);
    return st;
  }
  tag_writes = 0;
  for (int i = 0; st == MC_OK && i < first.count; i++) {
    int unusable = 0;
    watch_progress(lib, i, first.count);
    if (watch_cancelled(lib)) {
      st = MC_ERR_CANCELLED;
      break;
    }
    st = prepare_job(lib, &first.jobs[i], &unusable);
    if (st == MC_OK && unusable) {
      fprintf(stderr, "musiccore: edit: %s isn't what its hash says: left as it is\n", first.jobs[i].rel);
      if (!uids_add(skip, first.jobs[i].file)) st = MC_ERR_NOMEM;
      *again = 1;
    }
  }
  if (st != MC_OK || *again) {
    discard_copies(lib, &first);
    plan_clear(&first);
    return st;
  }
  st = plan_edit(lib, ops, user, skip, 1, &second);
  if (st != MC_OK || !second.count) { /* nothing left for the copies to do: rows are done */
    discard_copies(lib, &first);
  } else if (!plans_equal(&first, &second)) {
    mc_library_rollback(lib);
    discard_copies(lib, &first);
    *again = 1;
  } else {
    st = swap_and_commit(lib, &first);
  }
  plan_clear(&first);
  plan_clear(&second);
  return st;
}

/* Runs an edit from start to end, on a connection of its own. */
static mc_status run_edit(mc_library *lib, const edit_ops *ops, void *user) {
  mc_library *own = NULL;
  mc_status st = mc_library_open_internal(mc_library_dir(lib), &own);
  if (st != MC_OK) return st;
  own->edit_watch = lib->edit_watch;
  mc_folder_hold(mc_library_dir(lib));
  if (g_ready) mc_mutex_lock(&g_lock);
  uids skip = {0};
  int again = 1;
  for (int i = 0; st == MC_OK && again && i < MAX_ATTEMPTS; i++)
    st = attempt_edit(own, ops, user, &skip, &again);
  if (st == MC_OK && again) st = MC_ERR_IN_USE; /* something keeps changing them */
  if (st == ABANDONED) st = MC_ERR_IO;
  if (g_ready) mc_mutex_unlock(&g_lock);
  mc_folder_release(mc_library_dir(lib));
  free(skip.items);
  mc_library_close(own);
  return st;
}

static char *journal_read(mc_library *lib) {
  sqlite3_stmt *stmt = prepare(lib, "SELECT value FROM meta WHERE key = '" KEY_SWAP "'");
  char *text = stmt && sqlite3_step(stmt) == SQLITE_ROW ? col_dup(stmt, 0) : NULL;
  sqlite3_finalize(stmt);
  return text;
}

void mc_edit_recover(mc_library *lib) {
  char *text = journal_read(lib);
  if (!text) return;
  free(text);
  /* One may be an edit under way on another handle, which finishes first. */
  if (g_ready) mc_mutex_lock(&g_lock);
  text = journal_read(lib);
  plan p = {0};
  char *line = text ? strchr(text, '\n') : NULL;
  int undo = text && !strncmp(text, "undo\n", 5), ok = line != NULL;
  while (ok && line && line[1]) {
    char *start = line + 1;
    line = strchr(start, '\n');
    if (line) *line = '\0';
    char *field[5];
    int n = 0;
    for (char *f = start; f && n < 5; n++) {
      field[n] = f;
      if ((f = strchr(f, '\t'))) *f++ = '\0';
    }
    job *more = n >= 4 ? realloc(p.jobs, (size_t)(p.count + 1) * sizeof *more) : NULL;
    if (!more) {
      ok = 0;
      break;
    }
    p.jobs = more;
    job *j = &p.jobs[p.count++];
    memset(j, 0, sizeof *j);
    j->rel = mc_strdup(field[0]);
    j->dest = mc_strdup(field[1]);
    j->tmp = mc_strdup(field[2]);
    j->bak = mc_strdup(field[3]);
    j->fresh = n == 5;
    if (!j->rel || !j->dest || !j->tmp || !j->bak) ok = 0;
  }
  if (ok && (undo ? undo_jobs(lib, &p) : finish_jobs(lib, &p, 0)))
    mc_library_meta_set(lib, KEY_SWAP, NULL);
  plan_clear(&p);
  free(text);
  if (g_ready) mc_mutex_unlock(&g_lock);
}

/* --- Credits --- */

/* Replaces owner's credit with text, split as an import splits a credit it has only as a string
 * (docs/tagging-rules.md, "Artist credits"). Artists are found or made as an import's. */
static mc_status set_credit_text(mc_library *lib, int is_album, const char *uid, const char *text) {
  mc_db_artist whole = {text, NULL, NULL};
  mc_credit_split sp = {0};
  mc_status st = *text ? mc_credit_split_text(text, mc_credit_known_in_library, lib, &sp) : MC_OK;
  mc_db_credit_part *parts = NULL;
  if (st == MC_OK && sp.count > 1) {
    if (!(parts = calloc((size_t)sp.count, sizeof *parts))) st = MC_ERR_NOMEM;
    for (int i = 0; parts && i < sp.count; i++) {
      parts[i].artist.name = sp.parts[i].name;
      parts[i].join = sp.parts[i].join;
    }
  }
  if (st == MC_OK) st = mc_import_db_set_credit_locked(lib, is_album, uid, &whole, parts, parts ? sp.count : 0);
  int lone = !parts;
  free(parts);
  mc_credit_split_clear(&sp);
  /* One artist, found under another spelling ("radiohead"): the credit is as typed. */
  if (st == MC_OK && lone && *text) {
    sqlite3_stmt *stmt = prepare(lib, is_album
      ? "UPDATE albums SET artist_credit = ?2 WHERE uid = ?1"
        " AND ?2 <> (SELECT name FROM artists WHERE uid = albums.artist_uid)"
      : "UPDATE tracks SET artist_credit = ?2 WHERE uid = ?1"
        " AND ?2 <> (SELECT name FROM artists WHERE uid = tracks.artist_uid)");
    if (stmt) {
      bind_str(stmt, 1, uid);
      bind_str(stmt, 2, text);
    }
    st = step_done(stmt);
  }
  return st;
}

/* Track uid's credit from a release's, as an import writes it: never split again. */
static mc_status set_credit_mb(mc_library *lib, int is_album, const char *uid, const mc_db_artist *whole,
                               const mb_credit *credit) {
  mc_db_credit_part *parts = credit->count ? calloc((size_t)credit->count, sizeof *parts) : NULL;
  if (credit->count && !parts) return MC_ERR_NOMEM;
  for (int i = 0; i < credit->count; i++)
    parts[i] = (mc_db_credit_part){{credit->parts[i].name, credit->parts[i].sort_name, credit->parts[i].id},
                                   credit->parts[i].join};
  mc_status st = mc_import_db_set_credit_locked(lib, is_album, uid, whole, parts, credit->count);
  free(parts);
  return st;
}

/* A track back to its album's credit: no artist of its own. */
static mc_status clear_track_credit(mc_library *lib, const char *uid) {
  sqlite3_stmt *stmt = prepare(lib, "UPDATE tracks SET artist_uid = NULL, artist_credit = NULL WHERE uid = ?1");
  if (stmt) bind_str(stmt, 1, uid);
  mc_status st = step_done(stmt);
  if (st != MC_OK) return st;
  if (!(stmt = prepare(lib, "DELETE FROM track_artists WHERE track_uid = ?1"))) return MC_ERR_DB;
  bind_str(stmt, 1, uid);
  return step_done(stmt);
}

static mc_status note_track_artists(mc_library *lib, const char *uid, uids *artists) {
  mc_status st = uids_query(lib, artists,
                            "SELECT artist_uid FROM tracks WHERE uid = ?1 AND artist_uid IS NOT NULL", uid);
  if (st != MC_OK) return st;
  return uids_query(lib, artists, "SELECT artist_uid FROM track_artists WHERE track_uid = ?1", uid);
}

static mc_status note_album_artists(mc_library *lib, const char *uid, uids *artists) {
  mc_status st = uids_query(lib, artists,
                            "SELECT artist_uid FROM albums WHERE uid = ?1 AND artist_uid IS NOT NULL", uid);
  if (st == MC_OK)
    st = uids_query(lib, artists, "SELECT artist_uid FROM album_artists WHERE album_uid = ?1", uid);
  if (st == MC_OK)
    st = uids_query(lib, artists,
                    "SELECT artist_uid FROM tracks WHERE album_uid = ?1 AND artist_uid IS NOT NULL", uid);
  if (st == MC_OK)
    st = uids_query(lib, artists,
                    "SELECT c.artist_uid FROM track_artists c JOIN tracks t ON t.uid = c.track_uid"
                    " WHERE t.album_uid = ?1", uid);
  return st;
}

/* Deletes the artists noted that nothing names any more, logged so the deletes sync, as
 * delete.c and the artist refresh do: a typo put right leaves no artist behind. */
static mc_status delete_unused_artists(mc_library *lib, const uids *artists) {
  mc_status st = MC_OK;
  for (int i = 0; st == MC_OK && i < artists->count; i++) {
    sqlite3_stmt *stmt = prepare(lib, "DELETE FROM artists WHERE uid = ?1"
                                      " AND NOT EXISTS (SELECT 1 FROM tracks WHERE artist_uid = ?1)"
                                      " AND NOT EXISTS (SELECT 1 FROM albums WHERE artist_uid = ?1)"
                                      " AND NOT EXISTS (SELECT 1 FROM track_artists WHERE artist_uid = ?1)"
                                      " AND NOT EXISTS (SELECT 1 FROM album_artists WHERE artist_uid = ?1)");
    if (stmt) bind_str(stmt, 1, artists->items[i]);
    st = step_done(stmt);
  }
  return st;
}

/* --- Albums --- */

typedef struct album_row {
  char *title, *artist, *mb_albumid, *mb_releasegroupid;
  int year, original_year;
} album_row;

static void album_row_clear(album_row *a) {
  free(a->title);
  free(a->artist);
  free(a->mb_albumid);
  free(a->mb_releasegroupid);
  memset(a, 0, sizeof *a);
}

static mc_status album_row_read(mc_library *lib, const char *uid, album_row *out) {
  memset(out, 0, sizeof *out);
  sqlite3_stmt *stmt = prepare(lib, "SELECT a.title, coalesce(a.artist_credit, ar.name), a.mb_albumid,"
                                    " a.mb_releasegroupid, a.year, a.original_year"
                                    " FROM albums a LEFT JOIN artists ar ON ar.uid = a.artist_uid"
                                    " WHERE a.uid = ?1");
  if (!stmt) return MC_ERR_DB;
  bind_str(stmt, 1, uid);
  mc_status st = MC_ERR_NOT_FOUND;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    out->title = col_dup(stmt, 0);
    out->artist = col_dup(stmt, 1);
    out->mb_albumid = col_dup(stmt, 2);
    out->mb_releasegroupid = col_dup(stmt, 3);
    out->year = sqlite3_column_int(stmt, 4);
    out->original_year = sqlite3_column_int(stmt, 5);
    st = out->title && out->artist && out->mb_albumid && out->mb_releasegroupid ? MC_OK : MC_ERR_NOMEM;
  }
  sqlite3_finalize(stmt);
  if (st != MC_OK) album_row_clear(out);
  return st;
}

mc_status mc_album_tags_get(mc_library *lib, const char *album_uid, mc_album_tags *out) {
  if (!lib || !album_uid || !out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  album_row a;
  mc_status st = album_row_read(lib, album_uid, &a);
  if (st != MC_OK) return st;
  out->title = a.title;
  out->artist = a.artist;
  out->mb_albumid = a.mb_albumid;
  out->year = a.year;
  out->original_year = a.original_year;
  free(a.mb_releasegroupid);
  return MC_OK;
}

void mc_album_tags_clear(mc_album_tags *tags) {
  if (!tags) return;
  free(tags->title);
  free(tags->artist);
  free(tags->mb_albumid);
  memset(tags, 0, sizeof *tags);
}

/* An edit of album uid: tags as asked, and for a new release that release, with the release
 * track each of tracks takes (map, -1 for none). */
typedef struct album_edit {
  const char *uid;
  const mc_album_tags *tags;
  const mb_release *rel;
  uids tracks;
  int *map;
} album_edit;

static mc_status album_scope(mc_library *lib, void *user, uids *tracks) {
  album_edit *e = user;
  return uids_query(lib, tracks, "SELECT uid FROM tracks WHERE album_uid = ?1"
                                 " ORDER BY coalesce(disc_no, 1), coalesce(track_no, 0), uid", e->uid);
}

static mc_status apply_release_track(mc_library *lib, const char *uid, const mb_track *tr, int year,
                                     const char *mb_albumid) {
  sqlite3_stmt *stmt = prepare(lib, "UPDATE tracks SET title = coalesce(?2, title), track_no = ?3,"
                                    " disc_no = ?4, year = ?5, mb_trackid = ?6, mb_releasetrackid = ?7, mb_albumid = ?8"
                                    " WHERE uid = ?1");
  if (stmt) {
    bind_str(stmt, 1, uid);
    bind_opt(stmt, 2, tr->title);
    bind_num(stmt, 3, tr->number);
    bind_num(stmt, 4, tr->disc);
    bind_num(stmt, 5, year);
    bind_opt(stmt, 6, tr->recording_id);
    bind_opt(stmt, 7, tr->id);
    bind_str(stmt, 8, mb_albumid);
  }
  mc_status st = step_done(stmt);
  if (st != MC_OK || (!tr->artist && !tr->credit.count)) return st;
  mc_db_artist whole = {tr->artist, NULL, tr->artist_id};
  return set_credit_mb(lib, 0, uid, &whole, &tr->credit);
}

static mc_status album_apply(mc_library *lib, void *user) {
  const album_edit *e = user;
  const mc_album_tags *t = e->tags;
  const mb_release *rel = e->rel;
  album_row cur;
  mc_status st = album_row_read(lib, e->uid, &cur);
  if (st != MC_OK) return st;
  uids artists = {0}, tracks = {0};
  st = note_album_artists(lib, e->uid, &artists);

  /* With a new release, what the caller left as it was takes the release's. */
  const char *title = t->title && *t->title ? t->title : "Unknown Album";
  if (rel && rel->title && same(t->title, cur.title)) title = rel->title;
  int year = rel && t->year == cur.year && rel->year ? rel->year : t->year;
  int original_year = t->original_year;
  if (rel && original_year == cur.original_year && rel->original_year) original_year = rel->original_year;
  const char *credit = or_empty(t->artist);
  int credit_changed = strcmp(credit, cur.artist) != 0;

  int rel_credit = rel && (rel->artist || rel->credit.count) && (!credit_changed || same(credit, rel->artist));
  if (st == MC_OK && rel_credit) {
    mc_db_artist whole = {rel->artist, rel->artist_sort, rel->artist_id};
    st = set_credit_mb(lib, 1, e->uid, &whole, &rel->credit);
  } else if (st == MC_OK && credit_changed) {
    /* Tracks credited as the album was say what it says. With a release they're its, or keep
     * their tags. */
    sqlite3_stmt *stmt = prepare(lib, "SELECT t.uid FROM tracks t JOIN artists ar ON ar.uid = t.artist_uid"
                                      " WHERE t.album_uid = ?1 AND coalesce(t.artist_credit, ar.name) = ?2");
    if (!stmt) st = MC_ERR_DB;
    else {
      bind_str(stmt, 1, e->uid);
      bind_str(stmt, 2, cur.artist);
      while (!rel && st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW)
        if (!uids_add(&tracks, (const char *)sqlite3_column_text(stmt, 0))) st = MC_ERR_NOMEM;
      sqlite3_finalize(stmt);
    }
    if (st == MC_OK) st = set_credit_text(lib, 1, e->uid, credit);
    for (int i = 0; st == MC_OK && i < tracks.count; i++) st = set_credit_text(lib, 0, tracks.items[i], credit);
  }

  if (st == MC_OK) {
    sqlite3_stmt *stmt = prepare(lib, "UPDATE albums SET title = ?2, year = ?3, original_year = ?4"
                                      " WHERE uid = ?1");
    if (stmt) {
      bind_str(stmt, 1, e->uid);
      bind_str(stmt, 2, title);
      bind_num(stmt, 3, year);
      bind_num(stmt, 4, original_year);
    }
    st = step_done(stmt);
  }
  /* Tracks have a year of their own, which an import makes the album's. With a release the
   * matched ones get its, and the rest keep their tags. */
  if (st == MC_OK && year != cur.year && !rel) {
    sqlite3_stmt *stmt = prepare(lib, "UPDATE tracks SET year = ?3 WHERE album_uid = ?1 AND year IS ?2");
    if (stmt) {
      bind_str(stmt, 1, e->uid);
      bind_num(stmt, 2, cur.year);
      bind_num(stmt, 3, year);
    }
    st = step_done(stmt);
  }

  if (st == MC_OK && rel) {
    sqlite3_stmt *stmt = prepare(lib, "UPDATE albums SET mb_albumid = ?2, mb_releasegroupid = ?3,"
                                      " disc_total = ?4 WHERE uid = ?1");
    if (stmt) {
      bind_str(stmt, 1, e->uid);
      bind_str(stmt, 2, rel->id);
      bind_opt(stmt, 3, rel->releasegroup_id);
      bind_num(stmt, 4, rel->disc_total);
    }
    st = step_done(stmt);
    for (int i = 0; st == MC_OK && i < e->tracks.count; i++)
      if (e->map[i] >= 0)
        st = apply_release_track(lib, e->tracks.items[i], &rel->tracks[e->map[i]], year, rel->id);
  } else if (st == MC_OK && !*or_empty(t->mb_albumid) && *cur.mb_albumid) {
    /* Unlinked: the release's ids go. A recording is the recording on any release. */
    sqlite3_stmt *stmt = prepare(lib, "UPDATE albums SET mb_albumid = NULL, mb_releasegroupid = NULL"
                                      " WHERE uid = ?1");
    if (stmt) bind_str(stmt, 1, e->uid);
    st = step_done(stmt);
    if (st == MC_OK) {
      stmt = prepare(lib, "UPDATE tracks SET mb_albumid = NULL, mb_releasetrackid = NULL WHERE album_uid = ?1");
      if (stmt) bind_str(stmt, 1, e->uid);
      st = step_done(stmt);
    }
  }
  if (st == MC_OK) st = delete_unused_artists(lib, &artists);
  free(artists.items);
  free(tracks.items);
  album_row_clear(&cur);
  return st;
}

/* The uid a changelog row's row_uid goes by now, for c a changelog alias. */
#define CANONICAL(c) "coalesce((SELECT canonical FROM aliases WHERE uid = " c ".row_uid), " c ".row_uid)"

/* Whether release id is another album's, or ever was. Sync takes two rows that have shared an
 * id at any time for one, for good (see "Duplicates" in sync-protocol.md), so linking this
 * album to it would merge the two on every device; to a deleted album's, and it would be
 * deleted with it, a class's delete being any of its rows'. */
static int release_taken(mc_library *lib, const char *uid, const char *id) {
  sqlite3_stmt *stmt = prepare(lib, "SELECT 1 FROM albums WHERE mb_albumid = ?2 AND uid <> ?1"
                                    " UNION ALL SELECT 1 FROM changelog c WHERE c.entity = 'albums'"
                                    " AND c.field = 'mb_albumid' AND c.value = ?2"
                                    " AND " CANONICAL("c") " <> ?1 LIMIT 1");
  if (!stmt) return 1;
  bind_str(stmt, 1, uid);
  bind_str(stmt, 2, id);
  int yes = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return yes;
}

/* Tracks that other tracks' release track ids would merge them with (as release_taken(), a
 * pairing that differs from an earlier one with this release) are left unmatched. One pass over
 * the changelog: a value has no index. */
static mc_status drop_claimed_tracks(mc_library *lib, album_edit *e) {
  sqlite3_stmt *stmt = prepare(lib, "SELECT " CANONICAL("c") ", c.value FROM changelog c"
                                    " WHERE c.entity = 'tracks' AND c.field = 'mb_releasetrackid'"
                                    " AND c.value IS NOT NULL");
  if (!stmt) return MC_ERR_DB;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *owner = (const char *)sqlite3_column_text(stmt, 0);
    const char *value = (const char *)sqlite3_column_text(stmt, 1);
    for (int i = 0; owner && value && i < e->tracks.count; i++) {
      if (e->map[i] < 0) continue;
      const char *id = e->rel->tracks[e->map[i]].id;
      if (id && !strcmp(id, value) && strcmp(owner, e->tracks.items[i])) e->map[i] = -1;
    }
  }
  sqlite3_finalize(stmt);
  return MC_OK;
}

/* The album's tracks as tags to match with (match.c works on files' tags), in e->tracks' order.
 * Release with album_tags_free(). */
static mc_status album_tracks_tags(mc_library *lib, const char *album_uid, uids *tracks, mc_tags **out) {
  *out = NULL;
  mc_status st = uids_query(lib, tracks, "SELECT uid FROM tracks WHERE album_uid = ?1"
                                         " ORDER BY coalesce(disc_no, 1), coalesce(track_no, 0), uid",
                            album_uid);
  if (st != MC_OK || !tracks->count) return st;
  mc_tags *tags = calloc((size_t)tracks->count, sizeof *tags);
  sqlite3_stmt *stmt = prepare(lib, "SELECT duration_ms FROM tracks WHERE uid = ?1");
  if (!tags) st = MC_ERR_NOMEM;
  else if (!stmt) st = MC_ERR_DB;
  for (int i = 0; st == MC_OK && i < tracks->count; i++) {
    st = mc_dupes_library_tags(lib, tracks->items[i], &tags[i]);
    sqlite3_reset(stmt);
    bind_str(stmt, 1, tracks->items[i]);
    if (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) tags[i].duration_ms = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  *out = tags;
  return st;
}

static void album_tags_free(mc_tags *tags, int n) {
  for (int i = 0; tags && i < n; i++) mc_tags_clear(&tags[i]);
  free(tags);
}

/* Matches the album's tracks to release rel, as an import matches files. */
static mc_status match_album(mc_library *lib, const album_row *cur, album_edit *e) {
  mc_tags *tags = NULL;
  mc_status st = album_tracks_tags(lib, e->uid, &e->tracks, &tags);
  int n = e->tracks.count;
  if (st == MC_OK && n) {
    mc_release_score score;
    if (!(e->map = malloc((size_t)n * sizeof *e->map))) st = MC_ERR_NOMEM;
    else st = mc_score_release(e->rel, tags, n, cur->title, cur->artist, e->map, &score);
  }
  album_tags_free(tags, n);
  if (st == MC_OK && n) st = drop_claimed_tracks(lib, e);
  return st;
}

/* After a new release: the album gets a cover if it has none and one is easily had. There's
 * nowhere to put one without a file here, and failing to get one doesn't fail the edit. */
static void fetch_cover(mc_library *lib, const char *album_uid, const char *artist, const mb_release *rel) {
  sqlite3_stmt *stmt = prepare(lib, "SELECT a.art_hash IS NOT NULL, a.title, " MC_ALBUM_FILE("a")
                                    " FROM albums a WHERE a.uid = ?1");
  if (!stmt) return;
  bind_str(stmt, 1, album_uid);
  char *rel_path = NULL, *title = NULL;
  if (sqlite3_step(stmt) == SQLITE_ROW && !sqlite3_column_int(stmt, 0) &&
      sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
    rel_path = col_dup(stmt, 2);
    title = col_dup(stmt, 1);
  }
  sqlite3_finalize(stmt);
  char *folder = rel_path ? folder_of(rel_path) : NULL;
  char *dir = folder && *folder ? mc_library_abspath(lib, folder) : NULL;
  char *there = dir ? mc_coverart_find(dir) : NULL;
  if (dir && !there) {
    mc_buffer img = {0};
    const char *ext = NULL;
    if (mc_coverart_fetch(rel->id, rel->releasegroup_id, artist, title, &img, &ext, NULL) == MC_OK) {
      if (mc_coverart_save(dir, &img, ext) == MC_OK && (there = mc_coverart_find(dir))) {
        char hash[MC_ART_HASH_LEN];
        if (mc_coverart_hash(there, hash) == MC_OK && mc_library_begin_write(lib) == MC_OK) {
          stmt = prepare(lib, "UPDATE albums SET art_hash = ?2 WHERE uid = ?1 AND art_hash IS NULL");
          if (stmt) {
            bind_str(stmt, 1, album_uid);
            bind_str(stmt, 2, hash);
          }
          if (step_done(stmt) != MC_OK || mc_library_commit(lib) != MC_OK) mc_library_rollback(lib);
        }
      }
      mc_buffer_free(&img);
    }
  }
  free(there);
  free(dir);
  free(folder);
  free(rel_path);
  free(title);
}

mc_status mc_album_tags_set(mc_library *lib, const char *album_uid, const mc_album_tags *tags) {
  if (!lib || !album_uid || !tags) return MC_ERR_INVALID_ARG;
  album_row cur;
  mc_status st = album_row_read(lib, album_uid, &cur);
  if (st != MC_OK) return st;
  const char *id = or_empty(tags->mb_albumid);
  album_edit e = {album_uid, tags, NULL, {0}, NULL};
  mb_release rel;
  memset(&rel, 0, sizeof rel);
  if (*id && strcmp(id, cur.mb_albumid)) {
    mc_http_cancel cancel;
    mb_call call;
    if (release_taken(lib, album_uid, id)) st = MC_ERR_EXISTS;
    else st = mb_lookup_release(id, &rel, watch_call(lib, &cancel, &call));
    if (watch_cancelled(lib)) st = MC_ERR_CANCELLED;
    if (st == MC_OK && !rel.id) st = MC_ERR_NOT_FOUND;
    if (st == MC_OK) {
      e.rel = &rel;
      st = match_album(lib, &cur, &e);
    }
  }
  static const edit_ops ops = {album_scope, album_apply};
  if (st == MC_OK) st = run_edit(lib, &ops, &e);
  if (st == MC_OK && e.rel) {
    mc_library *own = NULL;
    if (mc_library_open_internal(mc_library_dir(lib), &own) == MC_OK) {
      album_row now;
      if (album_row_read(own, album_uid, &now) == MC_OK) {
        fetch_cover(own, album_uid, now.artist, &rel);
        album_row_clear(&now);
      }
      mc_library_close(own);
    }
  }
  free(e.tracks.items);
  free(e.map);
  mb_release_clear(&rel);
  album_row_clear(&cur);
  return st;
}

/* --- Album art --- */

typedef struct strs {
  char **items;
  int count;
} strs;

static void strs_clear(strs *l) {
  for (int i = 0; i < l->count; i++) free(l->items[i]);
  free(l->items);
  memset(l, 0, sizeof *l);
}

static int strs_has(const strs *l, const char *s) {
  for (int i = 0; i < l->count; i++)
    if (!strcmp(l->items[i], s)) return 1;
  return 0;
}

static int strs_equal(const strs *a, const strs *b) {
  if (a->count != b->count) return 0;
  for (int i = 0; i < a->count; i++)
    if (strcmp(a->items[i], b->items[i])) return 0;
  return 1;
}

/* The folders, relative, that hold a file of the album here, inside the library folder. A row
 * whose file is gone doesn't count, or the swap's pruning could take a cover's folder with it. */
static mc_status art_folders(mc_library *lib, const char *album_uid, strs *out) {
  sqlite3_stmt *stmt = prepare(lib, "SELECT f.path FROM files f JOIN tracks t ON t.uid = f.track_uid"
                                    " WHERE t.album_uid = ?1 ORDER BY f.source_file_uid IS NOT NULL, f.path");
  if (!stmt) return MC_ERR_DB;
  bind_str(stmt, 1, album_uid);
  mc_status st = MC_OK;
  while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *rel = (const char *)sqlite3_column_text(stmt, 0);
    char *folder = rel ? folder_of(rel) : NULL;
    if (!folder) {
      st = MC_ERR_NOMEM;
      break;
    }
    char *abs = *folder && !strs_has(out, folder) ? mc_transfer_deletable_path(lib, rel) : NULL;
    if (abs && mc_file_exists(abs)) {
      char **more = realloc(out->items, (size_t)(out->count + 1) * sizeof *more);
      if (more) {
        out->items = more;
        out->items[out->count++] = folder;
        folder = NULL;
      } else {
        st = MC_ERR_NOMEM;
      }
    }
    free(abs);
    free(folder);
  }
  sqlite3_finalize(stmt);
  if (st != MC_OK) strs_clear(out);
  return st;
}

static int named_cover(const char *name) {
  for (int i = 0; i < 6; i++)
    if (tolower((unsigned char)name[i]) != "cover."[i]) return 0;
  return 1;
}

/* The job that puts a new cover in folder: dest takes the place of what's there. A cover called
 * something else (cover.jpg, for a new cover.png) is the one set aside, unless dest is taken as
 * well, when the other goes after the commit. folder.jpg and front.jpg stay: they rank below. */
static mc_status art_job(mc_library *lib, const char *folder, const char *ext, job *j) {
  char name[16];
  snprintf(name, sizeof name, "cover.%s", ext);
  if (!(j->dest = join_path(folder, name))) return MC_ERR_NOMEM;
  if (rel_exists(lib, j->dest)) {
    j->rel = mc_strdup(j->dest);
    return j->rel ? MC_OK : MC_ERR_NOMEM;
  }
  char *dir = mc_library_abspath(lib, folder);
  char *found = dir ? mc_coverart_find(dir) : NULL;
  const char *sep = found ? strrchr(found, MC_PATH_SEP) : NULL;
  if (sep && named_cover(sep + 1)) {
    j->rel = join_path(folder, sep + 1);
  } else {
    j->rel = mc_strdup(j->dest);
    j->fresh = 1;
  }
  free(found);
  free(dir);
  return j->rel ? MC_OK : MC_ERR_NOMEM;
}

/* Writes the image as j's copy. */
static mc_status art_copy(mc_library *lib, const job *j, const mc_buffer *img) {
  if (++tag_writes == mc_edit_test_fail_tags) return MC_ERR_IO;
  char *tmp = mc_library_abspath(lib, j->tmp);
  FILE *f = tmp ? mc_fopen(tmp, "wb") : NULL;
  mc_status st = tmp ? MC_ERR_IO : MC_ERR_NOMEM;
  if (f) {
    int ok = fwrite(img->data, 1, img->len, f) == img->len;
    if (fclose(f) == 0 && ok) st = MC_OK;
  }
  free(tmp);
  return st;
}

/* One try, by the edit's own steps: the copies with no lock held, then in one transaction the
 * row and the renames. *again is set if the album's folders changed meanwhile. */
static mc_status art_attempt(mc_library *lib, const char *album_uid, const mc_buffer *img,
                             const char *ext, int chosen, int *again) {
  *again = 0;
  strs folders = {0}, now = {0};
  plan p = {0};
  char hash[MC_ART_HASH_LEN];
  mc_status st = art_folders(lib, album_uid, &folders);
  if (st == MC_OK && !folders.count) st = MC_ERR_NOT_FOUND;
  if (st == MC_OK && !(p.jobs = calloc((size_t)folders.count, sizeof *p.jobs))) st = MC_ERR_NOMEM;
  for (int i = 0; st == MC_OK && i < folders.count; i++) {
    p.count = i + 1;
    st = art_job(lib, folders.items[i], ext, &p.jobs[i]);
    p.jobs[i].cover = 1;
    if (st == MC_OK && !p.jobs[i].fresh && file_open(lib, p.jobs[i].rel)) st = MC_ERR_IN_USE;
  }
  if (st == MC_OK) st = name_jobs(&p);
  if (st == MC_OK) st = journal_write(lib, &p, "undo");
  if (st != MC_OK) goto done;

  tag_writes = 0;
  for (int i = 0; st == MC_OK && i < p.count; i++) st = art_copy(lib, &p.jobs[i], img);
  if (st == MC_OK) {
    char *tmp = mc_library_abspath(lib, p.jobs[0].tmp);
    st = tmp ? mc_coverart_hash(tmp, hash) : MC_ERR_NOMEM;
    free(tmp);
  }
  if (st == MC_OK) st = mc_library_begin_write(lib);
  if (st != MC_OK) {
    discard_copies(lib, &p);
    goto done;
  }
  st = art_folders(lib, album_uid, &now);
  if (st == MC_OK && !strs_equal(&folders, &now)) *again = 1;
  if (st == MC_OK && !*again) {
    sqlite3_stmt *stmt = prepare(lib, "UPDATE albums SET art_hash = ?2, art_chosen = ?3 WHERE uid = ?1");
    if (stmt) {
      bind_str(stmt, 1, album_uid);
      bind_str(stmt, 2, hash);
      bind_opt(stmt, 3, chosen ? hash : NULL);
    }
    st = step_done(stmt);
    if (st == MC_OK && !sqlite3_changes(mc_library_db(lib))) st = MC_ERR_NOT_FOUND;
  }
  if (st != MC_OK || *again) {
    mc_library_rollback(lib);
    discard_copies(lib, &p);
    goto done;
  }
  st = swap_and_commit(lib, &p);
  for (int i = 0; st == MC_OK && i < folders.count; i++) {
    char *dir = mc_library_abspath(lib, folders.items[i]);
    const char *slash = strrchr(p.jobs[i].dest, '/');
    if (dir && slash) mc_coverart_trash_others(dir, slash + 1, mc_edit_trash);
    free(dir);
  }
done:
  plan_clear(&p);
  strs_clear(&folders);
  strs_clear(&now);
  return st;
}

mc_status mc_album_art_get(mc_library *lib, const char *album_uid, char **path) {
  if (!lib || !album_uid || !path) return MC_ERR_INVALID_ARG;
  *path = NULL;
  strs folders = {0};
  mc_status st = art_folders(lib, album_uid, &folders);
  if (st == MC_OK && !folders.count) st = MC_ERR_NOT_FOUND;
  for (int i = 0; st == MC_OK && i < folders.count && !*path; i++) {
    char *dir = mc_library_abspath(lib, folders.items[i]);
    if (dir) *path = mc_coverart_find(dir);
    free(dir);
  }
  strs_clear(&folders);
  return st;
}

mc_status mc_album_art_check(const void *image, size_t len) {
  if (!image) return MC_ERR_INVALID_ARG;
  mc_buffer img = {(char *)image, len};
  const char *ext;
  return mc_coverart_check(&img, &ext);
}

mc_status mc_album_art_check_file(const char *path) {
  if (!path) return MC_ERR_INVALID_ARG;
  mc_buffer img;
  mc_status st = mc_coverart_read_file(path, &img);
  if (st != MC_OK) return st;
  st = mc_album_art_check(img.data, img.len);
  mc_buffer_free(&img);
  return st;
}

/* Makes img, checked, the album's cover, as its user's choice or not. */
static mc_status art_write(mc_library *lib, const char *album_uid, const mc_buffer *img, const char *ext,
                           int chosen) {
  mc_library *own = NULL;
  mc_status st = mc_library_open_internal(mc_library_dir(lib), &own);
  if (st != MC_OK) return st;
  mc_folder_hold(mc_library_dir(lib));
  if (g_ready) mc_mutex_lock(&g_lock);
  int again = 1;
  for (int i = 0; st == MC_OK && again && i < MAX_ATTEMPTS; i++)
    st = art_attempt(own, album_uid, img, ext, chosen, &again);
  if (st == MC_OK && again) st = MC_ERR_IN_USE; /* something keeps moving it */
  if (st == ABANDONED) st = MC_ERR_IO;
  if (g_ready) mc_mutex_unlock(&g_lock);
  mc_folder_release(mc_library_dir(lib));
  mc_library_close(own);
  return st;
}

mc_status mc_album_art_set(mc_library *lib, const char *album_uid, const void *image, size_t len) {
  if (!lib || !album_uid || !image) return MC_ERR_INVALID_ARG;
  mc_buffer img = {(char *)image, len};
  const char *ext;
  mc_status st = mc_coverart_check(&img, &ext);
  if (st != MC_OK) return st;
  return art_write(lib, album_uid, &img, ext, 1);
}

/* The picture embedded in the first of the album's files here to have one, into img. */
static void embedded_cover(mc_library *lib, const char *album_uid, mc_buffer *img, const char **ext) {
  sqlite3_stmt *stmt = prepare(lib, "SELECT f.path FROM files f JOIN tracks t ON t.uid = f.track_uid"
                                    " WHERE t.album_uid = ?1 AND f.source_file_uid IS NULL ORDER BY f.path");
  if (!stmt) return;
  bind_str(stmt, 1, album_uid);
  while (!img->data && sqlite3_step(stmt) == SQLITE_ROW) {
    char *abs = mc_library_abspath(lib, (const char *)sqlite3_column_text(stmt, 0));
    if (abs) mc_coverart_read_embedded(abs, img, ext);
    free(abs);
  }
  sqlite3_finalize(stmt);
}

/* What an import would give the album, in img, as ensure_cover() in tagger.c picks it: its files'
 * picture, unless a download is bigger. MC_OK with img empty if there's none to be had. */
static mc_status automatic_cover(mc_library *lib, const char *album_uid, const album_row *a, mc_buffer *img,
                                 const char **ext) {
  memset(img, 0, sizeof *img);
  embedded_cover(lib, album_uid, img, ext);
  int side = img->data ? mc_coverart_short_side(img) : 0;
  if (side >= MC_COVER_LOW_RES) return MC_OK;
  mc_buffer got = {0};
  const char *got_ext = NULL;
  mc_http_cancel cancel;
  mb_call call;
  watch_call(lib, &cancel, &call);
  mc_status st = mc_coverart_fetch(*a->mb_albumid ? a->mb_albumid : NULL,
                                   *a->mb_releasegroupid ? a->mb_releasegroupid : NULL, a->artist, a->title,
                                   &got, &got_ext, call.cancel);
  if (watch_cancelled(lib)) st = MC_ERR_CANCELLED;
  if (st == MC_OK) {
    if (!img->data || mc_coverart_short_side(&got) > side) {
      mc_buffer_free(img);
      *img = got;
      *ext = got_ext;
    } else {
      mc_buffer_free(&got);
    }
    return MC_OK;
  }
  if (st == MC_ERR_CANCELLED) mc_buffer_free(img);
  if (img->data || st == MC_ERR_NOT_FOUND) return MC_OK;
  return st;
}

/* No cover to go back to: the row says so and the folders' cover.* go to the trash. A folder.* or
 * front.* of the user's then shows, and is what the row says. */
static mc_status art_drop(mc_library *lib, const char *album_uid, const strs *folders) {
  mc_status st = mc_library_begin_write(lib);
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt = prepare(lib, "UPDATE albums SET art_hash = NULL, art_chosen = NULL WHERE uid = ?1");
  if (stmt) bind_str(stmt, 1, album_uid);
  st = step_done(stmt);
  if (st == MC_OK) st = mc_library_commit(lib);
  if (st != MC_OK) {
    mc_library_rollback(lib);
    return st;
  }
  char hash[MC_ART_HASH_LEN] = "";
  for (int i = 0; i < folders->count; i++) {
    char *dir = mc_library_abspath(lib, folders->items[i]);
    if (dir) mc_coverart_trash_others(dir, "", mc_edit_trash);
    char *left = dir ? mc_coverart_find(dir) : NULL;
    if (left && !*hash && mc_coverart_hash(left, hash) != MC_OK) hash[0] = '\0';
    free(left);
    free(dir);
  }
  if (*hash && mc_library_begin_write(lib) == MC_OK) {
    if ((stmt = prepare(lib, "UPDATE albums SET art_hash = ?2 WHERE uid = ?1"))) {
      bind_str(stmt, 1, album_uid);
      bind_str(stmt, 2, hash);
    }
    if (step_done(stmt) != MC_OK || mc_library_commit(lib) != MC_OK) mc_library_rollback(lib);
  }
  return MC_OK;
}

mc_status mc_album_art_reset(mc_library *lib, const char *album_uid) {
  if (!lib || !album_uid) return MC_ERR_INVALID_ARG;
  mc_album_flags flags;
  mc_status st = mc_album_flags_get(lib, album_uid, &flags);
  if (st != MC_OK || !flags.art_chosen) return st;
  album_row a;
  if ((st = album_row_read(lib, album_uid, &a)) != MC_OK) return st;
  strs folders = {0};
  st = art_folders(lib, album_uid, &folders);
  if (st == MC_OK && !folders.count) st = MC_ERR_NOT_FOUND;
  mc_buffer img = {0};
  const char *ext = NULL;
  if (st == MC_OK) st = automatic_cover(lib, album_uid, &a, &img, &ext);
  if (st == MC_OK && img.data && mc_coverart_check(&img, &ext) != MC_OK) mc_buffer_free(&img);
  if (st == MC_OK) {
    if (img.data) {
      st = art_write(lib, album_uid, &img, ext, 0);
    } else {
      mc_library *own = NULL;
      if ((st = mc_library_open_internal(mc_library_dir(lib), &own)) == MC_OK) {
        if (g_ready) mc_mutex_lock(&g_lock);
        st = art_drop(own, album_uid, &folders);
        if (g_ready) mc_mutex_unlock(&g_lock);
        mc_library_close(own);
      }
    }
  }
  mc_buffer_free(&img);
  strs_clear(&folders);
  album_row_clear(&a);
  return st;
}

mc_status mc_album_art_set_file(mc_library *lib, const char *album_uid, const char *path) {
  if (!lib || !album_uid || !path) return MC_ERR_INVALID_ARG;
  mc_buffer img;
  mc_status st = mc_coverart_read_file(path, &img);
  if (st != MC_OK) return st;
  st = mc_album_art_set(lib, album_uid, img.data, img.len);
  mc_buffer_free(&img);
  return st;
}

char *mc_edit_chosen_cover(mc_library *lib, const char *album_uid, const char *hash) {
  strs folders = {0};
  char *found = NULL;
  if (art_folders(lib, album_uid, &folders) != MC_OK) return NULL;
  for (int i = 0; i < folders.count && !found; i++) {
    char *dir = mc_library_abspath(lib, folders.items[i]);
    char *cover = dir ? mc_coverart_find(dir) : NULL;
    char have[MC_ART_HASH_LEN];
    if (cover && mc_coverart_hash(cover, have) == MC_OK && !strcmp(have, hash)) found = cover;
    else free(cover);
    free(dir);
  }
  strs_clear(&folders);
  return found;
}

/* --- Artists --- */

mc_status mc_artist_tags_get(mc_library *lib, const char *artist_uid, mc_artist_tags *out) {
  if (!lib || !artist_uid || !out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  sqlite3_stmt *stmt = prepare(lib, "SELECT name, sort_name FROM artists WHERE uid = ?1");
  if (!stmt) return MC_ERR_DB;
  bind_str(stmt, 1, artist_uid);
  mc_status st = MC_ERR_NOT_FOUND;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    out->name = col_dup(stmt, 0);
    out->sort_name = col_dup(stmt, 1);
    st = out->name && out->sort_name ? MC_OK : MC_ERR_NOMEM;
  }
  sqlite3_finalize(stmt);
  if (st != MC_OK) mc_artist_tags_clear(out);
  return st;
}

void mc_artist_tags_clear(mc_artist_tags *tags) {
  if (!tags) return;
  free(tags->name);
  free(tags->sort_name);
  memset(tags, 0, sizeof *tags);
}

typedef struct artist_edit {
  const char *uid;
  const mc_artist_tags *tags;
} artist_edit;

static mc_status artist_scope(mc_library *lib, void *user, uids *tracks) {
  const artist_edit *e = user;
  return uids_query(lib, tracks, "SELECT uid FROM tracks WHERE uid IN (" CREDITED_TRACKS("?1") ")"
                                 " OR album_uid IN (" CREDITED_ALBUMS("?1") ")"
                                 " ORDER BY album_uid, disc_no, track_no, uid",
                    e->uid);
}

/* Owner's credit text if it spells out its artists' own names, with the artist's name
 * replaced; NULL (*st MC_OK) if it's credited otherwise ("as credited on the release"), and
 * then a rename doesn't touch it. */
static char *respelled_credit(mc_library *lib, const char *owner, const char *text, const char *artist_uid,
                              const char *name, mc_status *st) {
  mc_credit_list *c = NULL;
  *st = mc_library_list_credit(lib, owner, &c);
  if (*st != MC_OK) return NULL;
  size_t was = 1, now = 1;
  for (int i = 0; i < c->count; i++) {
    was += strlen(c->items[i].name) + strlen(c->items[i].join);
    now += strlen(!strcmp(c->items[i].uid, artist_uid) ? name : c->items[i].name) + strlen(c->items[i].join);
  }
  char *a = malloc(was), *b = malloc(now);
  char *out = NULL;
  if (a && b) {
    a[0] = b[0] = '\0';
    for (int i = 0; i < c->count; i++) {
      strcat(a, c->items[i].name);
      strcat(a, c->items[i].join);
      strcat(b, !strcmp(c->items[i].uid, artist_uid) ? name : c->items[i].name);
      strcat(b, c->items[i].join);
    }
    if (!strcmp(a, text)) out = b;
  } else {
    *st = MC_ERR_NOMEM;
  }
  if (out != b) free(b);
  free(a);
  mc_credit_list_free(c);
  return out;
}

/* The credits that spell the artist's name out, for each owner of a query of (uid, text). */
static mc_status respell_credits(mc_library *lib, const char *uid, const char *name, const char *query,
                                 const char *update) {
  typedef struct owner { char uid[MC_UID_STRLEN]; char *text; } owner;
  sqlite3_stmt *stmt = prepare(lib, query);
  if (!stmt) return MC_ERR_DB;
  bind_str(stmt, 1, uid);
  owner *owners = NULL;
  int n = 0;
  mc_status st = MC_OK;
  while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) {
    owner *more = realloc(owners, (size_t)(n + 1) * sizeof *more);
    const char *u = (const char *)sqlite3_column_text(stmt, 0);
    if (!more) {
      st = MC_ERR_NOMEM;
      break;
    }
    owners = more;
    if (!u || strlen(u) >= MC_UID_STRLEN) continue;
    strcpy(owners[n].uid, u);
    if (!(owners[n].text = col_dup(stmt, 1))) st = MC_ERR_NOMEM;
    else n++;
  }
  sqlite3_finalize(stmt);
  for (int i = 0; st == MC_OK && i < n; i++) {
    char *text = respelled_credit(lib, owners[i].uid, owners[i].text, uid, name, &st);
    if (st == MC_OK && text) {
      if ((stmt = prepare(lib, update))) {
        bind_str(stmt, 1, owners[i].uid);
        bind_str(stmt, 2, text);
      }
      st = step_done(stmt);
    }
    free(text);
  }
  for (int i = 0; i < n; i++) free(owners[i].text);
  free(owners);
  return st;
}

/* Artist uid's credits, on albums and tracks, spelled with name. While its old name is there to
 * compare with. */
static mc_status respell_artist(mc_library *lib, const char *uid, const char *name) {
  mc_status st = respell_credits(lib, uid, name,
                                 "SELECT uid, artist_credit FROM albums WHERE artist_credit IS NOT NULL"
                                 " AND uid IN (" CREDITED_ALBUMS("?1") ")",
                                 "UPDATE albums SET artist_credit = ?2 WHERE uid = ?1");
  if (st == MC_OK)
    st = respell_credits(lib, uid, name,
                         "SELECT uid, artist_credit FROM tracks WHERE artist_credit IS NOT NULL"
                         " AND (artist_uid = ?1 OR uid IN"
                         "  (SELECT track_uid FROM track_artists WHERE artist_uid = ?1))",
                         "UPDATE tracks SET artist_credit = ?2 WHERE uid = ?1");
  return st;
}

static mc_status name_artist(mc_library *lib, const char *uid, const mc_artist_tags *tags) {
  sqlite3_stmt *stmt = prepare(lib, "UPDATE artists SET name = ?2, sort_name = ?3 WHERE uid = ?1");
  if (stmt) {
    bind_str(stmt, 1, uid);
    bind_str(stmt, 2, tags->name);
    bind_opt(stmt, 3, tags->sort_name);
  }
  return step_done(stmt);
}

static mc_status artist_apply(mc_library *lib, void *user) {
  const artist_edit *e = user;
  sqlite3_stmt *stmt = prepare(lib, "SELECT 1 FROM artists WHERE uid = ?1");
  if (!stmt) return MC_ERR_DB;
  bind_str(stmt, 1, e->uid);
  int found = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  if (!found) return MC_ERR_NOT_FOUND;

  mc_status st = respell_artist(lib, e->uid, e->tags->name);
  return st == MC_OK ? name_artist(lib, e->uid, e->tags) : st;
}

mc_status mc_artist_tags_set(mc_library *lib, const char *artist_uid, const mc_artist_tags *tags) {
  if (!lib || !artist_uid || !tags || !tags->name || !*tags->name) return MC_ERR_INVALID_ARG;
  mc_artist_tags cur;
  mc_status st = mc_artist_tags_get(lib, artist_uid, &cur);
  if (st != MC_OK) return st;
  mc_artist_tags_clear(&cur);
  artist_edit e = {artist_uid, tags};
  static const edit_ops ops = {artist_scope, artist_apply};
  return run_edit(lib, &ops, &e);
}

mc_status mc_artist_same_name(mc_library *lib, const char *artist_uid, const char *name,
                              char other_uid[MC_UID_STRLEN]) {
  if (!lib || !artist_uid || !name || !other_uid) return MC_ERR_INVALID_ARG;
  other_uid[0] = '\0';
  sqlite3_stmt *stmt = prepare(lib, "SELECT uid FROM artists WHERE name = ?2 COLLATE NOCASE AND uid <> ?1"
                                    " ORDER BY name <> ?2, mb_artistid IS NULL, uid LIMIT 1");
  if (!stmt) return MC_ERR_DB;
  bind_str(stmt, 1, artist_uid);
  bind_str(stmt, 2, name);
  mc_status st = MC_ERR_NOT_FOUND;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    snprintf(other_uid, MC_UID_STRLEN, "%s", sqlite3_column_text(stmt, 0));
    st = MC_OK;
  }
  sqlite3_finalize(stmt);
  return st;
}

/* Two artists being made one: keep stays, under tags, and gone goes. */
typedef struct artist_merge {
  char keep[MC_UID_STRLEN], gone[MC_UID_STRLEN];
  mc_artist_tags tags;
} artist_merge;

static mc_status merge_scope(mc_library *lib, void *user, uids *tracks) {
  const artist_merge *m = user;
  artist_edit keep = {m->keep, NULL}, gone = {m->gone, NULL};
  mc_status st = artist_scope(lib, &keep, tracks);
  return st == MC_OK ? artist_scope(lib, &gone, tracks) : st;
}

/* Everything that credits gone credits keep. Rows as an edit writes them, so it syncs; recents
 * being keyed by what they're of, gone's goes. */
static mc_status merge_apply(mc_library *lib, void *user) {
  const artist_merge *m = user;
  mc_status st = respell_artist(lib, m->gone, m->tags.name);
  if (st == MC_OK) st = respell_artist(lib, m->keep, m->tags.name);
  static const char *const moves[] = {
    "UPDATE tracks SET artist_uid = ?2 WHERE artist_uid = ?1",
    "UPDATE albums SET artist_uid = ?2 WHERE artist_uid = ?1",
    "UPDATE track_artists SET artist_uid = ?2 WHERE artist_uid = ?1",
    "UPDATE album_artists SET artist_uid = ?2 WHERE artist_uid = ?1",
  };
  for (size_t i = 0; st == MC_OK && i < sizeof moves / sizeof moves[0]; i++) {
    sqlite3_stmt *stmt = prepare(lib, moves[i]);
    if (stmt) {
      bind_str(stmt, 1, m->gone);
      bind_str(stmt, 2, m->keep);
    }
    st = step_done(stmt);
  }
  if (st == MC_OK) {
    sqlite3_stmt *stmt = prepare(lib, "DELETE FROM recents WHERE uid = ?1");
    if (stmt) bind_str(stmt, 1, m->gone);
    st = step_done(stmt);
  }
  if (st == MC_OK) {
    sqlite3_stmt *stmt = prepare(lib, "DELETE FROM artists WHERE uid = ?1");
    if (stmt) bind_str(stmt, 1, m->gone);
    st = step_done(stmt);
  }
  return st == MC_OK ? name_artist(lib, m->keep, &m->tags) : st;
}

/* Whether the artist has a MusicBrainz id. */
static int artist_has_id(mc_library *lib, const char *uid) {
  sqlite3_stmt *stmt = prepare(lib, "SELECT 1 FROM artists WHERE uid = ?1 AND coalesce(mb_artistid, '') <> ''");
  if (!stmt) return 0;
  bind_str(stmt, 1, uid);
  int yes = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return yes;
}

mc_status mc_artist_merge(mc_library *lib, const char *artist_uid, const char *other_uid) {
  if (!lib || !artist_uid || !other_uid || !strcmp(artist_uid, other_uid)) return MC_ERR_INVALID_ARG;
  artist_merge m;
  memset(&m, 0, sizeof m);
  mc_artist_tags cur;
  mc_status st = mc_artist_tags_get(lib, artist_uid, &cur);
  if (st != MC_OK) return st;
  mc_artist_tags_clear(&cur);
  if ((st = mc_artist_tags_get(lib, other_uid, &m.tags)) != MC_OK) return st;
  /* An id is what makes two rows one (see "Duplicates" in docs/sync-protocol.md), and a row's
   * delete is its whole class's: so the one with an id stays, and it's never passed on. */
  int swap = artist_has_id(lib, artist_uid) && !artist_has_id(lib, other_uid);
  snprintf(m.keep, sizeof m.keep, "%s", swap ? artist_uid : other_uid);
  snprintf(m.gone, sizeof m.gone, "%s", swap ? other_uid : artist_uid);
  static const edit_ops ops = {merge_scope, merge_apply};
  st = run_edit(lib, &ops, &m);
  mc_artist_tags_clear(&m.tags);
  return st;
}

/* --- Tracks --- */

typedef struct track_row {
  mc_track_tags tags;
  char *album_uid; /* "" for none */
} track_row;

static void track_row_clear(track_row *t) {
  mc_track_tags_clear(&t->tags);
  free(t->album_uid);
  t->album_uid = NULL;
}

/* The artist is "" when the track has none of its own, or says what its album does. */
static mc_status track_row_read(mc_library *lib, const char *uid, track_row *out) {
  memset(out, 0, sizeof *out);
  sqlite3_stmt *stmt = prepare(lib, "SELECT t.title, CASE WHEN t.artist_uid IS NULL THEN ''"
                                    "  WHEN coalesce(t.artist_credit, ar.name)"
                                    "   IS coalesce(al.artist_credit, aar.name) THEN ''"
                                    "  ELSE coalesce(t.artist_credit, ar.name) END,"
                                    " t.track_no, t.disc_no, coalesce(t.year, al.year), t.genre, t.album_uid"
                                    " FROM tracks t LEFT JOIN artists ar ON ar.uid = t.artist_uid"
                                    " LEFT JOIN albums al ON al.uid = t.album_uid"
                                    " LEFT JOIN artists aar ON aar.uid = al.artist_uid WHERE t.uid = ?1");
  if (!stmt) return MC_ERR_DB;
  bind_str(stmt, 1, uid);
  mc_status st = MC_ERR_NOT_FOUND;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    out->tags.title = col_dup(stmt, 0);
    out->tags.artist = col_dup(stmt, 1);
    out->tags.track_no = sqlite3_column_int(stmt, 2);
    out->tags.disc_no = sqlite3_column_int(stmt, 3);
    out->tags.year = sqlite3_column_int(stmt, 4);
    out->tags.genre = col_dup(stmt, 5);
    out->album_uid = col_dup(stmt, 6);
    st = out->tags.title && out->tags.artist && out->tags.genre && out->album_uid ? MC_OK : MC_ERR_NOMEM;
  }
  sqlite3_finalize(stmt);
  if (st != MC_OK) track_row_clear(out);
  return st;
}

mc_status mc_track_tags_get(mc_library *lib, const char *track_uid, mc_track_tags *out) {
  if (!lib || !track_uid || !out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  track_row t;
  mc_status st = track_row_read(lib, track_uid, &t);
  if (st != MC_OK) return st;
  *out = t.tags;
  free(t.album_uid);
  return MC_OK;
}

void mc_track_tags_clear(mc_track_tags *tags) {
  if (!tags) return;
  free(tags->title);
  free(tags->artist);
  free(tags->genre);
  memset(tags, 0, sizeof *tags);
}

typedef struct track_edit {
  const char *uid;
  const mc_track_tags *tags;
} track_edit;

mc_status mc_track_mix_get(mc_library *lib, const char *track_uid, mc_track_mix *out) {
  if (!lib || !track_uid || !out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  sqlite3_stmt *stmt = prepare(lib, "SELECT t.album_uid IS NULL OR (SELECT count(*) FROM tracks st"
                                    "  WHERE st.album_uid = t.album_uid) = 1,"
                                    " " TRACK_IN_MIX ", coalesce(al.mix <> 0, 0)"
                                    " FROM tracks t LEFT JOIN albums al ON al.uid = t.album_uid"
                                    " WHERE t.uid = ?1");
  if (!stmt) return MC_ERR_DB;
  bind_str(stmt, 1, track_uid);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    out->can_set = sqlite3_column_int(stmt, 0);
    out->mix = sqlite3_column_int(stmt, 1);
    out->mix_by_hand = sqlite3_column_int(stmt, 2);
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW ? MC_OK : rc == SQLITE_DONE ? MC_ERR_NOT_FOUND : MC_ERR_DB;
}

static mc_status mix_scope(mc_library *lib, void *user, uids *tracks) {
  return uids_query(lib, tracks, "SELECT uid FROM tracks WHERE uid = ?1", user);
}

/* Filed under an album of its own, which its file's tags and place then follow. */
static mc_status mix_apply(mc_library *lib, void *user) {
  mc_status st = mc_mix_file_tracks(lib, "t.uid = ?1", user);
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt = prepare(lib, "UPDATE albums SET mix = 1 WHERE uid ="
                                    " (SELECT album_uid FROM tracks WHERE uid = ?1)");
  if (stmt) bind_str(stmt, 1, user);
  return step_done(stmt);
}

typedef struct mix_unfiling {
  char track[MC_UID_STRLEN];
  const char *album;
} mix_unfiling;

static mc_status unfile_scope(mc_library *lib, void *user, uids *tracks) {
  return uids_query(lib, tracks, "SELECT uid FROM tracks WHERE uid = ?1", ((mix_unfiling *)user)->track);
}

static mc_status unfile_apply(mc_library *lib, void *user) {
  const mix_unfiling *m = user;
  sqlite3_stmt *stmt = prepare(lib, "UPDATE tracks SET album_uid = NULL WHERE uid = ?1");
  if (stmt) bind_str(stmt, 1, m->track);
  mc_status st = step_done(stmt);
  static const char *const gone[] = {
    "DELETE FROM album_group_entries WHERE album_uid = ?1",
    "DELETE FROM album_artists WHERE album_uid = ?1",
    "DELETE FROM albums WHERE uid = ?1",
  };
  for (size_t i = 0; st == MC_OK && i < sizeof gone / sizeof gone[0]; i++) {
    if ((stmt = prepare(lib, gone[i]))) bind_str(stmt, 1, m->album);
    st = step_done(stmt);
  }
  return st;
}

mc_status mc_mix_unfile(mc_library *lib, const char *album_uid) {
  mix_unfiling m = {"", album_uid};
  if (!mc_mix_album_of_track(lib, album_uid, m.track)) return MC_OK;
  static const edit_ops ops = {unfile_scope, unfile_apply};
  return run_edit(lib, &ops, &m);
}

mc_status mc_track_mix_set(mc_library *lib, const char *track_uid, int mix) {
  mc_track_mix cur;
  mc_status st = mc_track_mix_get(lib, track_uid, &cur);
  if (st != MC_OK) return st;
  if (!cur.can_set) return MC_ERR_INVALID_ARG;
  track_row t;
  if ((st = track_row_read(lib, track_uid, &t)) != MC_OK) return st;
  char album[MC_UID_STRLEN];
  snprintf(album, sizeof album, "%s", t.album_uid);
  track_row_clear(&t);
  if (!*album) {
    static const edit_ops ops = {mix_scope, mix_apply};
    return mix ? run_edit(lib, &ops, (void *)track_uid) : MC_OK;
  }
  mc_album_flags flags;
  if ((st = mc_album_flags_get(lib, album, &flags)) != MC_OK) return st;
  flags.mix = mix;
  return mc_album_flags_set(lib, album, &flags);
}

/* The whole album: a number changes the track totals in its other files, and a disc may make
 * the album one of several, which all its file names show. */
static mc_status track_scope(mc_library *lib, void *user, uids *tracks) {
  const track_edit *e = user;
  mc_status st = uids_query(lib, tracks, "SELECT uid FROM tracks WHERE uid = ?1", e->uid);
  if (st != MC_OK) return st;
  return uids_query(lib, tracks, "SELECT uid FROM tracks WHERE album_uid ="
                                 " (SELECT album_uid FROM tracks WHERE uid = ?1)"
                                 " ORDER BY coalesce(disc_no, 1), coalesce(track_no, 0), uid", e->uid);
}

static mc_status track_apply(mc_library *lib, void *user) {
  const track_edit *e = user;
  const mc_track_tags *t = e->tags;
  track_row cur;
  mc_status st = track_row_read(lib, e->uid, &cur);
  if (st != MC_OK) return st;
  uids artists = {0};
  const char *credit = or_empty(t->artist);
  if (strcmp(credit, cur.tags.artist)) {
    if ((st = note_track_artists(lib, e->uid, &artists)) == MC_OK)
      st = *credit || !*cur.album_uid ? set_credit_text(lib, 0, e->uid, credit)
                                      : clear_track_credit(lib, e->uid);
  }
  if (st == MC_OK) {
    sqlite3_stmt *stmt = prepare(lib, "UPDATE tracks SET title = ?2, track_no = ?3, disc_no = ?4, genre = ?5"
                                      " WHERE uid = ?1");
    if (stmt) {
      bind_str(stmt, 1, e->uid);
      bind_str(stmt, 2, t->title && *t->title ? t->title : "Untitled");
      bind_num(stmt, 3, t->track_no);
      bind_num(stmt, 4, t->disc_no);
      bind_opt(stmt, 5, t->genre);
    }
    st = step_done(stmt);
  }
  /* What's shown may be the album's year, which a track doesn't need a copy of. */
  if (st == MC_OK && t->year != cur.tags.year) {
    sqlite3_stmt *stmt = prepare(lib, "UPDATE tracks SET year = ?2 WHERE uid = ?1");
    if (stmt) {
      bind_str(stmt, 1, e->uid);
      bind_num(stmt, 2, t->year);
    }
    st = step_done(stmt);
  }
  /* A disc past the album's last: it has one more, and its file names say so. */
  if (st == MC_OK && *cur.album_uid && t->disc_no > 1) {
    sqlite3_stmt *stmt = prepare(lib, "UPDATE albums SET disc_total = ?2 WHERE uid = ?1"
                                      " AND coalesce(disc_total, 1) < ?2");
    if (stmt) {
      bind_str(stmt, 1, cur.album_uid);
      sqlite3_bind_int(stmt, 2, t->disc_no);
    }
    st = step_done(stmt);
    /* Tracks with no number are on the first, and their names and tags say so now. */
    if (st == MC_OK) {
      stmt = prepare(lib, "UPDATE tracks SET disc_no = 1 WHERE album_uid = ?1 AND disc_no IS NULL");
      if (stmt) bind_str(stmt, 1, cur.album_uid);
      st = step_done(stmt);
    }
  }
  /* And the last track gone from its last disc: it has fewer, down to the last with a track. A
   * release's total above the discs that are here isn't touched, as the edit didn't empty it. */
  if (st == MC_OK && *cur.album_uid && cur.tags.disc_no > 1 && t->disc_no < cur.tags.disc_no) {
    sqlite3_stmt *stmt = prepare(lib, "UPDATE albums SET disc_total ="
                                      " (SELECT max(1, coalesce(max(disc_no), 1)) FROM tracks WHERE album_uid = ?1)"
                                      " WHERE uid = ?1 AND disc_total = ?2 AND NOT EXISTS"
                                      " (SELECT 1 FROM tracks WHERE album_uid = ?1 AND disc_no >= ?2)");
    if (stmt) {
      bind_str(stmt, 1, cur.album_uid);
      sqlite3_bind_int(stmt, 2, cur.tags.disc_no);
    }
    st = step_done(stmt);
  }
  if (st == MC_OK) st = delete_unused_artists(lib, &artists);
  free(artists.items);
  track_row_clear(&cur);
  return st;
}

mc_status mc_track_tags_set(mc_library *lib, const char *track_uid, const mc_track_tags *tags) {
  if (!lib || !track_uid || !tags) return MC_ERR_INVALID_ARG;
  track_row cur;
  mc_status st = track_row_read(lib, track_uid, &cur);
  if (st != MC_OK) return st;
  track_row_clear(&cur);
  track_edit e = {track_uid, tags};
  static const edit_ops ops = {track_scope, track_apply};
  return run_edit(lib, &ops, &e);
}

/* --- Releases --- */

static int by_score(const void *a, const void *b) {
  double x = ((const mc_release_info *)a)->score, y = ((const mc_release_info *)b)->score;
  return (x < y) - (x > y);
}

static mc_status release_info_fill(mc_release_info *r, const mb_release *rel, double score) {
  memset(r, 0, sizeof *r);
  snprintf(r->mb_albumid, sizeof r->mb_albumid, "%s", or_empty(rel->id));
  r->title = mc_strdup(or_empty(rel->title));
  r->artist = mc_strdup(or_empty(rel->artist));
  r->country = rel->country ? mc_strdup(rel->country) : NULL;
  r->label = rel->label ? mc_strdup(rel->label) : NULL;
  r->format = rel->format ? mc_strdup(rel->format) : NULL;
  r->year = rel->year;
  r->track_count = rel->track_count;
  r->score = score < 0 ? 0 : score > 1 ? 1 : score;
  return r->title && r->artist && (r->country || !rel->country) && (r->label || !rel->label) &&
             (r->format || !rel->format)
           ? MC_OK
           : MC_ERR_NOMEM;
}

mc_status mc_release_search(mc_library *lib, const char *album_uid, const char *artist,
                            const char *title, mc_release_list **out) {
  if (!lib || !album_uid || !title || !*title || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  album_row cur;
  mc_status st = album_row_read(lib, album_uid, &cur);
  if (st != MC_OK) return st;
  uids tracks = {0};
  mc_tags *tags = NULL;
  st = album_tracks_tags(lib, album_uid, &tracks, &tags);
  int n = tracks.count, found = 0;
  int *map = n ? malloc((size_t)n * sizeof *map) : NULL;
  if (st == MC_OK && n && !map) st = MC_ERR_NOMEM;

  /* As the import searches. Each call waits its turn for the shared MusicBrainz rate limit. */
  char ids[RELEASE_LOOKUPS][MC_UID_STRLEN];
  const char *search_artist = artist && *artist && mc_string_distance(artist, "Various Artists") != 0.0
                                ? artist
                                : NULL;
  mc_http_cancel cancel;
  mb_call call;
  watch_call(lib, &cancel, &call);
  if (st == MC_OK) st = mb_search_releases(search_artist, title, n, ids, RELEASE_LOOKUPS, &found, &call);

  mc_release_list *list = st == MC_OK ? calloc(1, sizeof *list) : NULL;
  if (st == MC_OK && !list) st = MC_ERR_NOMEM;
  if (st == MC_OK && found && !(list->items = calloc((size_t)found, sizeof *list->items))) st = MC_ERR_NOMEM;
  for (int i = 0; st == MC_OK && i < found && !watch_cancelled(lib); i++) {
    mb_release rel;
    watch_progress(lib, i, found);
    mc_status got = mb_lookup_release(ids[i], &rel, &call);
    if (got == MC_OK) {
      mc_release_score score = {1.0, 0, 0};
      if (n) st = mc_score_release(&rel, tags, n, cur.title, cur.artist, map, &score);
      if (st == MC_OK) st = release_info_fill(&list->items[list->count++], &rel, 1.0 - score.distance);
    } else if (got == MC_ERR_NET && !list->count) {
      st = got; /* nothing to show for it */
    }
    mb_release_clear(&rel);
    if (got == MC_ERR_NET) break;
  }
  if (watch_cancelled(lib)) st = MC_ERR_CANCELLED;
  if (st == MC_OK) {
    if (list->count) qsort(list->items, (size_t)list->count, sizeof *list->items, by_score);
    *out = list;
  } else {
    mc_release_list_free(list);
  }
  free(map);
  album_tags_free(tags, n);
  free(tracks.items);
  album_row_clear(&cur);
  return st;
}

void mc_release_list_free(mc_release_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    free(list->items[i].title);
    free(list->items[i].artist);
    free(list->items[i].country);
    free(list->items[i].label);
    free(list->items[i].format);
  }
  free(list->items);
  free(list);
}

mc_status mc_release_tags(mc_library *lib, const char *album_uid, const char *mb_albumid,
                          mc_album_tags *out) {
  if (!lib || !album_uid || !mb_albumid || !out) return MC_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  album_row cur;
  mc_status st = album_row_read(lib, album_uid, &cur);
  if (st != MC_OK) return st;
  album_row_clear(&cur);
  mb_release rel;
  mc_http_cancel cancel;
  mb_call call;
  st = mb_lookup_release(mb_albumid, &rel, watch_call(lib, &cancel, &call));
  if (watch_cancelled(lib)) {
    if (st == MC_OK) mb_release_clear(&rel);
    return MC_ERR_CANCELLED;
  }
  if (st == MC_OK) {
    out->title = mc_strdup(or_empty(rel.title));
    out->artist = mc_strdup(or_empty(rel.artist));
    out->mb_albumid = mc_strdup(mb_albumid);
    out->year = rel.year;
    out->original_year = rel.original_year;
    if (!out->title || !out->artist || !out->mb_albumid) st = MC_ERR_NOMEM;
  }
  mb_release_clear(&rel);
  if (st != MC_OK) mc_album_tags_clear(out);
  return st;
}
