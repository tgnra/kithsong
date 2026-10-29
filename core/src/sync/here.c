/* Removing an album or artist from this device, and syncing it here again. See
 * musiccore/sync_here.h and "Removing from one device" in docs/sync-protocol.md.
 *
 * The profile edit is a profile_items row of an exclusion kind (sync/plan.h), written like
 * any other item so it syncs field by field. The files go as reconcile would drop them after
 * the next session (transfer.c), under the same one-at-a-time lock, so a session fetching
 * meanwhile can't bring back what was just removed. */
#include "musiccore/sync_here.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "sync/archive.h"
#include "sync/merge.h"
#include "sync/plan.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"
#include "util/platform.h"

struct mc_here_plan {
  mc_profile_item_kind kind;
  char uid[MC_UID_STRLEN]; /* resolved through merges */
  int files, kept;
  char (*tracks)[MC_UID_STRLEN]; /* with a file here, sorted */
  int ntracks;
};

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

static int uid_cmp(const void *a, const void *b) {
  return strcmp(a, b);
}

static const char *exclusion_kind(mc_profile_item_kind kind) {
  return kind == MC_PROFILE_ALBUM ? MC_PLAN_EXCLUDE_ALBUM : MC_PLAN_EXCLUDE_ARTIST;
}

static int valid_kind(mc_profile_item_kind kind) {
  return kind == MC_PROFILE_ALBUM || kind == MC_PROFILE_ARTIST;
}

/* uid as the album or artist it was merged into, if it was (sync/merge.h). */
static void resolve(mc_library *lib, const char *uid, char out[MC_UID_STRLEN]) {
  if (mc_merge_resolve(lib, uid, out) != MC_OK) memcpy(out, uid, MC_UID_STRLEN);
}

/* --- One file phase at a time (as transfer.c's files_wait/_release) --- */

static void files_wait(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  while (sync->files_busy) mc_cond_wait_ms(&sync->cond, &sync->lock, 200);
  sync->files_busy = 1;
  mc_mutex_unlock(&sync->lock);
}

static void files_release(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  sync->files_busy = 0;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

/* --- State --- */

/* Whether this device's profile has an exclusion of kind that resolves to uid. 1, 0, -1. */
static int excluded(mc_sync *sync, const char *kind, const char *uid) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sync->api.sdb),
                         "SELECT ref_uid FROM profile_items WHERE device_uid = ?1 AND kind = ?2",
                         -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, sync->device_uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_STATIC);
  int found = 0, rc;
  while (!found && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *ref = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(ref)) continue;
    char r[MC_UID_STRLEN];
    resolve(sync->api.lib, ref, r);
    found = !strcmp(r, uid);
  }
  if (!found && rc != SQLITE_DONE) found = -1;
  sqlite3_finalize(stmt);
  return found;
}

/* The state of uid (resolved), under api_lock, the library being the group's. */
static mc_status read_state(mc_sync *sync, mc_profile_item_kind kind, const char *uid,
                            mc_here_state *out) {
  *out = MC_HERE_UNAVAILABLE;
  mc_sync_db *sdb = sync->api.sdb;
  sqlite3 *db = mc_sync_db_handle(sdb);
  if (!mc_sync_db_group_uid(sdb)) return MC_OK;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM devices WHERE uid = ?1 AND removed = 0", -1, &stmt,
                         NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, sync->device_uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW) return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
  int archive = mc_archive_is(db, sync->device_uid);
  if (archive < 0) return MC_ERR_DB;
  if (archive) return MC_OK; /* it keeps the whole library */
  int ex = excluded(sync, exclusion_kind(kind), uid);
  if (ex < 0) return MC_ERR_DB;
  if (ex) {
    *out = MC_HERE_REMOVED;
    return MC_OK;
  }
  if (kind == MC_PROFILE_ALBUM) {
    /* Its artist removed from here takes it along, and only the artist can bring it back. */
    char artist[MC_UID_STRLEN] = "";
    if (sqlite3_prepare_v2(mc_library_db(sync->api.lib),
                           "SELECT artist_uid FROM albums WHERE uid = ?1", -1, &stmt,
                           NULL) != SQLITE_OK)
      return MC_ERR_DB;
    sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *a = (const char *)sqlite3_column_text(stmt, 0);
      if (is_uid(a)) resolve(sync->api.lib, a, artist);
    }
    sqlite3_finalize(stmt);
    if (artist[0]) {
      ex = excluded(sync, MC_PLAN_EXCLUDE_ARTIST, artist);
      if (ex < 0) return MC_ERR_DB;
      if (ex) return MC_OK;
    }
  }
  *out = MC_HERE_HELD;
  return MC_OK;
}

/* MC_OK with the state, the library state checked first (it takes api_lock itself). The
 * caller then holds api_lock. */
static mc_status lock_state(mc_sync *sync, mc_profile_item_kind kind, const char *uid,
                            char resolved[MC_UID_STRLEN], mc_here_state *out) {
  *out = MC_HERE_UNAVAILABLE;
  mc_sync_lib_state lib_state;
  mc_status st = mc_sync_library_state(sync, &lib_state);
  mc_mutex_lock(&sync->api_lock);
  if (st != MC_OK || lib_state != MC_SYNC_LIBRARY_GROUP) {
    memcpy(resolved, uid, MC_UID_STRLEN);
    return st;
  }
  resolve(sync->api.lib, uid, resolved);
  return read_state(sync, kind, resolved, out);
}

mc_status mc_sync_here_state(mc_sync *sync, mc_profile_item_kind kind, const char *uid,
                             mc_here_state *out) {
  if (out) *out = MC_HERE_UNAVAILABLE;
  if (!sync || !valid_kind(kind) || !is_uid(uid) || !out) return MC_ERR_INVALID_ARG;
  char resolved[MC_UID_STRLEN];
  mc_status st = lock_state(sync, kind, uid, resolved, out);
  mc_mutex_unlock(&sync->api_lock);
  return st;
}

/* --- Files --- */

typedef struct here_file {
  char uid[MC_UID_STRLEN];
  char track[MC_UID_STRLEN];
  char *path;
  int imported; /* an original imported here */
} here_file;

static void files_free(here_file *f, int n) {
  for (int i = 0; i < n; i++) free(f[i].path);
  free(f);
}

/* This device's files of the item's tracks, by track: an album's, or an artist's as a
 * selection of the artist counts them (sync/plan.c). Under api_lock. */
static mc_status read_files(mc_library *lib, mc_profile_item_kind kind, const char *uid,
                            here_file **out, int *count) {
  *out = NULL;
  *count = 0;
  const char *sql =
    kind == MC_PROFILE_ALBUM
      ? "SELECT f.uid, f.track_uid, f.path,"
        " f.source_file_uid IS NULL AND f.source_peer_uid IS NULL"
        " FROM files f JOIN tracks t ON t.uid = f.track_uid WHERE t.album_uid = ?1"
        " ORDER BY f.track_uid"
      : "SELECT f.uid, f.track_uid, f.path,"
        " f.source_file_uid IS NULL AND f.source_peer_uid IS NULL"
        " FROM files f JOIN tracks t ON t.uid = f.track_uid WHERE t.artist_uid = ?1"
        " OR t.album_uid IN (SELECT uid FROM albums WHERE artist_uid = ?1)"
        " ORDER BY f.track_uid";
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  here_file *files = NULL;
  int n = 0, cap = 0, rc;
  mc_status st = MC_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *fu = (const char *)sqlite3_column_text(stmt, 0);
    const char *tu = (const char *)sqlite3_column_text(stmt, 1);
    const char *path = (const char *)sqlite3_column_text(stmt, 2);
    if (!is_uid(fu) || !is_uid(tu) || !path) continue;
    if (n == cap) {
      int grown = cap ? cap * 2 : 32;
      here_file *more = realloc(files, (size_t)grown * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      files = more;
      cap = grown;
    }
    here_file *f = &files[n];
    memcpy(f->uid, fu, MC_UID_STRLEN);
    memcpy(f->track, tu, MC_UID_STRLEN);
    f->imported = sqlite3_column_int(stmt, 3) != 0;
    if (!(f->path = mc_strdup(path))) {
      st = MC_ERR_NOMEM;
      break;
    }
    n++;
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    files_free(files, n);
    return st;
  }
  *out = files;
  *count = n;
  return MC_OK;
}

/* Whether originals imported here may go: an archive holds verified copies of everything.
 * Under api_lock. 1, 0, -1. */
static int originals_safe(mc_sync *sync) {
  int64_t latest = mc_profile_library_latest_ms(sync->api.lib);
  if (latest < 0) return -1;
  return mc_profile_other_archive_verified(mc_sync_db_handle(sync->api.sdb), sync->device_uid,
                                           latest);
}

/* --- Plan --- */

mc_status mc_sync_here_plan_new(mc_sync *sync, mc_profile_item_kind kind, const char *uid,
                                mc_here_plan **out) {
  if (out) *out = NULL;
  if (!sync || !valid_kind(kind) || !is_uid(uid) || !out) return MC_ERR_INVALID_ARG;
  mc_here_plan *plan = calloc(1, sizeof *plan);
  if (!plan) return MC_ERR_NOMEM;
  plan->kind = kind;
  mc_here_state state;
  mc_status st = lock_state(sync, kind, uid, plan->uid, &state);
  if (st == MC_OK && state != MC_HERE_HELD) st = MC_ERR_LOCKED;
  here_file *files = NULL;
  int n = 0, safe = 0;
  if (st == MC_OK) st = read_files(sync->api.lib, kind, plan->uid, &files, &n);
  if (st == MC_OK && (safe = originals_safe(sync)) < 0) st = MC_ERR_DB;
  mc_mutex_unlock(&sync->api_lock);
  if (st == MC_OK && n && !(plan->tracks = malloc((size_t)n * sizeof *plan->tracks)))
    st = MC_ERR_NOMEM;
  for (int i = 0; st == MC_OK && i < n; i++) {
    if (files[i].imported && !safe) {
      plan->kept++;
      continue;
    }
    plan->files++;
    if (!plan->ntracks || strcmp(plan->tracks[plan->ntracks - 1], files[i].track))
      memcpy(plan->tracks[plan->ntracks++], files[i].track, MC_UID_STRLEN);
  }
  files_free(files, n);
  if (st != MC_OK) {
    mc_here_plan_free(plan);
    return st;
  }
  *out = plan;
  return MC_OK;
}

void mc_here_plan_free(mc_here_plan *plan) {
  if (!plan) return;
  free(plan->tracks);
  free(plan);
}

int mc_here_plan_files(const mc_here_plan *plan) {
  return plan ? plan->files : 0;
}

int mc_here_plan_kept(const mc_here_plan *plan) {
  return plan ? plan->kept : 0;
}

int mc_here_plan_has(const mc_here_plan *plan, const char *track_uid) {
  return plan && track_uid && plan->ntracks &&
         bsearch(track_uid, plan->tracks, (size_t)plan->ntracks, sizeof *plan->tracks, uid_cmp);
}

/* --- Editing the profile --- */

/* Whether this device's profile is a selection (mode 'selected'). Under the write. */
static int is_selection(sqlite3 *db, const char *me) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT mode FROM profiles WHERE device_uid = ?1", -1, &stmt, NULL) !=
      SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, me, -1, SQLITE_STATIC);
  int selected = 0, rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const char *mode = (const char *)sqlite3_column_text(stmt, 0);
    selected = mode && !strcmp(mode, "selected");
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW || rc == SQLITE_DONE ? selected : -1;
}

/* A new profile_items row, which the insert trigger logs for the group. */
static mc_status insert_item(sqlite3 *db, const char *me, const char *kind, const char *ref) {
  char row[MC_UID_STRLEN];
  mc_status st = mc_uid_new(row);
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO profile_items (uid, device_uid, kind, ref_uid)"
                         " VALUES (?1, ?2, ?3, ?4)",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, row, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, me, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, ref, -1, SQLITE_STATIC);
  st = sqlite3_step(stmt) == SQLITE_DONE ? MC_OK : MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

/* Under api_lock: removing (remove = 1) or restoring uid in this device's profile, in a write
 * of its own, then confirming it again if it was confirmed before. */
static mc_status edit_profile(mc_sync *sync, mc_profile_item_kind kind, const char *uid,
                              int remove) {
  mc_sync_db *sdb = sync->api.sdb;
  sqlite3 *db = mc_sync_db_handle(sdb);
  const char *me = sync->device_uid;
  int confirmed = mc_profile_confirm_state(db, me) == MC_SYNC_CONFIRMED;
  mc_profile_item item;
  memset(&item, 0, sizeof item);
  item.kind = kind;
  memcpy(item.ref_uid, uid, MC_UID_STRLEN);
  mc_status st = mc_sync_db_begin_write(sdb);
  if (st != MC_OK) return st;
  int selected = is_selection(db, me);
  if (selected < 0) st = MC_ERR_DB;
  if (remove) {
    /* A selection's own row for it goes, so the editor doesn't show it ticked; the
     * exclusion keeps out what else selects it (an artist, a group, a playlist). */
    if (st == MC_OK && selected)
      st = mc_profile_delete_rows(db, sync->api.lib, me, mc_profile_kind_str(kind), &item);
    if (st == MC_OK) st = insert_item(db, me, exclusion_kind(kind), uid);
  } else {
    if (st == MC_OK) st = mc_profile_delete_rows(db, sync->api.lib, me, exclusion_kind(kind), &item);
    /* In a selection it's then selected; with everything it's back as it is. Any copy of
     * the row already there is dropped first, so there's one. */
    if (st == MC_OK && selected)
      st = mc_profile_delete_rows(db, sync->api.lib, me, mc_profile_kind_str(kind), &item);
    if (st == MC_OK && selected) st = insert_item(db, me, mc_profile_kind_str(kind), uid);
  }
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) {
    mc_sync_db_rollback(sdb);
    return st;
  }
  /* The device's own user asked for it, as pressing Sync would; a profile someone else
   * changed meanwhile stays paused for its user to look at. */
  if (confirmed && mc_profile_confirm(sdb, NULL) != MC_OK)
    fprintf(stderr, "musiccore: sync: couldn't confirm the profile again after an edit here\n");
  return MC_OK;
}

/* --- Removing --- */

mc_status mc_sync_remove_here(mc_sync *sync, const mc_here_plan *plan, mc_here_result *out) {
  if (out) memset(out, 0, sizeof *out);
  if (!sync || !plan) return MC_ERR_INVALID_ARG;
  files_wait(sync);
  mc_here_state state;
  char uid[MC_UID_STRLEN];
  mc_status st = lock_state(sync, plan->kind, plan->uid, uid, &state);
  if (st == MC_OK && state != MC_HERE_HELD) st = MC_ERR_LOCKED;
  if (st == MC_OK) st = edit_profile(sync, plan->kind, uid, 1);
  mc_here_result res = {0, 0};
  here_file *files = NULL;
  int n = 0, safe = 0;
  if (st == MC_OK && read_files(sync->api.lib, plan->kind, uid, &files, &n) == MC_OK) {
    mc_library *lib = sync->api.lib;
    safe = originals_safe(sync) > 0;
    /* Files first, then their rows: a file that stays keeps its row. */
    int *gone = calloc((size_t)n + 1, sizeof *gone);
    for (int i = 0; gone && i < n; i++) {
      here_file *f = &files[i];
      if (f->imported && !safe) {
        res.files_kept++;
        continue;
      }
      char *abs = mc_transfer_deletable_path(lib, f->path);
      if (!abs) {
        gone[i] = 1; /* outside the library folder: left alone, as reconcile does */
      } else if (!mc_file_exists(abs)) {
        gone[i] = 1;
      } else if ((f->imported && mc_transfer_trash(abs) == MC_OK) || mc_remove(abs) == MC_OK) {
        gone[i] = 1;
        res.files_removed++;
      } else {
        res.files_kept++; /* in use: reconcile removes it after a later session */
      }
      free(abs);
    }
    if (gone && mc_library_begin_write(lib) == MC_OK) {
      sqlite3_stmt *del;
      mc_status wst = MC_ERR_DB;
      if (sqlite3_prepare_v2(mc_library_db(lib), "DELETE FROM files WHERE uid = ?1", -1, &del,
                             NULL) == SQLITE_OK) {
        wst = MC_OK;
        for (int i = 0; wst == MC_OK && i < n; i++) {
          if (!gone[i]) continue;
          sqlite3_bind_text(del, 1, files[i].uid, -1, SQLITE_STATIC);
          if (sqlite3_step(del) != SQLITE_DONE) wst = MC_ERR_DB;
          sqlite3_reset(del);
        }
        sqlite3_finalize(del);
      }
      if (wst == MC_OK) wst = mc_library_commit(lib);
      if (wst != MC_OK) mc_library_rollback(lib); /* the fast check drops rows of missing files */
    }
    for (int i = 0; gone && i < n; i++)
      if (gone[i]) mc_transfer_prune_dirs(lib, files[i].path);
    if (!gone) res.files_kept = n;
    free(gone);
  }
  files_free(files, n);
  mc_mutex_unlock(&sync->api_lock);
  files_release(sync);
  if (st == MC_OK) mc_sync_notify_changed(sync);
  if (out) *out = res;
  return st;
}

mc_status mc_sync_restore_here(mc_sync *sync, mc_profile_item_kind kind, const char *uid) {
  if (!sync || !valid_kind(kind) || !is_uid(uid)) return MC_ERR_INVALID_ARG;
  mc_here_state state;
  char resolved[MC_UID_STRLEN];
  mc_status st = lock_state(sync, kind, uid, resolved, &state);
  if (st == MC_OK && state != MC_HERE_REMOVED) st = MC_ERR_LOCKED;
  if (st == MC_OK) st = edit_profile(sync, kind, resolved, 0);
  mc_mutex_unlock(&sync->api_lock);
  if (st == MC_OK) mc_sync_notify_changed(sync);
  return st;
}
