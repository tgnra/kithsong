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

#include "library/delete_internal.h"
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
  int files, kept, staying;
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

/* Whether this device's profile has a row of kind that resolves to uid. 1, 0, -1. */
static int has_row(mc_sync *sync, const char *kind, const char *uid) {
  return mc_profile_has_row(mc_sync_db_handle(sync->api.sdb), sync->api.lib, sync->device_uid,
                            kind, uid);
}

/* Whether an artist credited on album has a row of kind. 1, 0, -1. */
static int artist_row(mc_sync *sync, const char *kind, const char *album) {
  return mc_profile_album_artist_row(mc_sync_db_handle(sync->api.sdb), sync->api.lib,
                                     sync->device_uid, kind, album);
}

/* Whether a selected album group has album. 1, 0, -1. */
static int in_selected_group(mc_sync *sync, const char *album) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(sync->api.lib),
                         "SELECT group_uid FROM album_group_entries WHERE album_uid = ?1", -1,
                         &stmt, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, album, -1, SQLITE_STATIC);
  int found = 0, rc;
  while (!found && (rc = sqlite3_step(stmt)) == SQLITE_ROW)
    found = has_row(sync, "album_group", (const char *)sqlite3_column_text(stmt, 0));
  if (!found && rc != SQLITE_DONE) found = -1;
  sqlite3_finalize(stmt);
  return found;
}

/* Whether this device's selection has album, itself or through its artist or an album group.
 * A playlist with some of its tracks doesn't make it the album's. */
static int selection_has_album(mc_sync *sync, const char *album) {
  int has = has_row(sync, "album", album);
  if (!has) has = artist_row(sync, "artist", album);
  return has ? has : in_selected_group(sync, album);
}

/* Whether this device's selection has artist, or any album of theirs: there's something of
 * theirs here to remove then. */
static int selection_has_artist(mc_sync *sync, const char *artist) {
  int has = has_row(sync, "artist", artist);
  if (has) return has;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(sync->api.lib), CREDITED_ALBUMS("?1"), -1, &stmt, NULL) !=
      SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, artist, -1, SQLITE_STATIC);
  int rc;
  while (!has && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *album = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(album)) continue;
    char r[MC_UID_STRLEN];
    resolve(sync->api.lib, album, r);
    has = selection_has_album(sync, r);
  }
  if (!has && rc != SQLITE_DONE) has = -1;
  sqlite3_finalize(stmt);
  return has;
}

/* Whether this device keeps a part of the library: it's a member of a group (and not
 * removed) and not an archive, which keeps the whole library. Under api_lock, the library
 * being the group's. 1, 0, -1. */
static int keeps_part(mc_sync *sync) {
  mc_sync_db *sdb = sync->api.sdb;
  sqlite3 *db = mc_sync_db_handle(sdb);
  if (!mc_sync_db_group_uid(sdb)) return 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM devices WHERE uid = ?1 AND removed = 0", -1, &stmt,
                         NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, sync->device_uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW) return rc == SQLITE_DONE ? 0 : -1;
  int archive = mc_archive_is(db, sync->device_uid);
  return archive < 0 ? -1 : !archive;
}

/* Whether this device's profile is a selection (mode 'selected'). */
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

/* The state of uid (resolved), under api_lock, the library being the group's. */
static mc_status read_state(mc_sync *sync, mc_profile_item_kind kind, const char *uid,
                            mc_here_state *out) {
  *out = MC_HERE_UNAVAILABLE;
  int part = keeps_part(sync);
  if (part <= 0) return part < 0 ? MC_ERR_DB : MC_OK;
  int ex = has_row(sync, exclusion_kind(kind), uid);
  if (!ex && kind == MC_PROFILE_ALBUM) {
    /* Its artist removed from here takes it along, unless it was brought back since. */
    ex = artist_row(sync, MC_PLAN_EXCLUDE_ARTIST, uid);
    int back = ex > 0 ? has_row(sync, MC_PLAN_KEEP_ALBUM, uid) : 0;
    if (back) ex = back < 0 ? -1 : 0;
  }
  if (ex < 0) return MC_ERR_DB;
  if (ex) {
    *out = MC_HERE_REMOVED;
    return MC_OK;
  }
  int selected = is_selection(mc_sync_db_handle(sync->api.sdb), sync->device_uid);
  int has = selected <= 0 ? 1
            : kind == MC_PROFILE_ALBUM ? selection_has_album(sync, uid)
                                       : selection_has_artist(sync, uid);
  if (selected < 0 || has < 0) return MC_ERR_DB;
  *out = has ? MC_HERE_HELD : MC_HERE_NOT_INCLUDED;
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

/* Takes out of *files those of tracks a ticked playlist keeps here (sync/plan.h), and says how
 * many tracks that was. Under api_lock. */
static mc_status drop_staying(mc_sync *sync, here_file *files, int *count, int *staying) {
  *staying = 0;
  char (*kept)[MC_UID_STRLEN] = NULL;
  int nkept = 0;
  mc_status st = mc_plan_kept_by_playlists(sync->api.sdb, sync->api.lib, sync->device_uid, &kept,
                                           &nkept);
  if (st != MC_OK || !nkept) return st;
  int n = 0;
  char last_track[MC_UID_STRLEN] = "";
  for (int i = 0; i < *count; i++) {
    if (!bsearch(files[i].track, kept, (size_t)nkept, sizeof *kept, uid_cmp)) {
      files[n++] = files[i];
      continue;
    }
    if (strcmp(last_track, files[i].track)) (*staying)++; /* they come sorted by track */
    memcpy(last_track, files[i].track, MC_UID_STRLEN);
    free(files[i].path);
  }
  *count = n;
  free(kept);
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
  if (st == MC_OK) st = drop_staying(sync, files, &n, &plan->staying);
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

int mc_here_plan_staying(const mc_here_plan *plan) {
  return plan ? plan->staying : 0;
}

int mc_here_plan_has(const mc_here_plan *plan, const char *track_uid) {
  return plan && track_uid && plan->ntracks &&
         bsearch(track_uid, plan->tracks, (size_t)plan->ntracks, sizeof *plan->tracks, uid_cmp);
}

/* --- Editing the profile --- */

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
    if (st == MC_OK && kind == MC_PROFILE_ALBUM)
      st = mc_profile_delete_rows(db, sync->api.lib, me, MC_PLAN_KEEP_ALBUM, &item);
    if (st == MC_OK) st = mc_profile_insert_row(db, me, exclusion_kind(kind), uid);
  } else {
    if (st == MC_OK && kind == MC_PROFILE_ALBUM)
      st = mc_profile_album_back(db, sync->api.lib, me, uid);
    else if (st == MC_OK)
      st = mc_profile_delete_rows(db, sync->api.lib, me, exclusion_kind(kind), &item);
    /* In a selection it's then selected; with everything it's back as it is. Any copy of
     * the row already there is dropped first, so there's one. */
    if (st == MC_OK && selected)
      st = mc_profile_delete_rows(db, sync->api.lib, me, mc_profile_kind_str(kind), &item);
    if (st == MC_OK && selected) st = mc_profile_insert_row(db, me, mc_profile_kind_str(kind), uid);
  }
  if (st == MC_OK && kind == MC_PROFILE_ARTIST) st = mc_profile_drop_keeps(db, sync->api.lib, me, uid);
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
  int staying = 0;
  if (st == MC_OK && read_files(sync->api.lib, plan->kind, uid, &files, &n) == MC_OK &&
      drop_staying(sync, files, &n, &staying) == MC_OK) {
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
  if (st == MC_OK && state != MC_HERE_REMOVED && state != MC_HERE_NOT_INCLUDED) st = MC_ERR_LOCKED;
  if (st == MC_OK) st = edit_profile(sync, kind, resolved, 0);
  mc_mutex_unlock(&sync->api_lock);
  if (st == MC_OK) mc_sync_notify_changed(sync);
  return st;
}

/* --- The recycle bin (musiccore/delete.h) --- */

mc_status mc_sync_bin_send_begin(mc_sync *sync, mc_library *lib, int pressed) {
  if (!lib) return MC_OK;
  if (sync) {
    mc_mutex_lock(&sync->lock);
    while (sync->bin_restoring && !sync->stopping) mc_cond_wait_ms(&sync->cond, &sync->lock, 200);
    sync->bin_sending++;
    mc_mutex_unlock(&sync->lock);
  }
  int pending = 0;
  mc_status st = mc_delete_bin_seal(lib, pressed, &pending);
  if (st == MC_OK && pending)
    fprintf(stderr, "musiccore: sync: sending %d delete%s from the recycle bin\n", pending,
            pending == 1 ? "" : "s");
  if (st != MC_OK && sync) {
    mc_mutex_lock(&sync->lock);
    sync->bin_sending--;
    mc_cond_broadcast(&sync->cond);
    mc_mutex_unlock(&sync->lock);
  }
  return st;
}

void mc_sync_bin_send_end(mc_sync *sync, mc_library *lib) {
  if (!lib) return;
  /* A delete made while the exchange read its rows may have gone with them. */
  if (mc_delete_bin_seal(lib, 1, NULL) != MC_OK)
    fprintf(stderr, "musiccore: sync: couldn't empty the recycle bin after a sync\n");
  if (!sync) return;
  mc_mutex_lock(&sync->lock);
  sync->bin_sending--;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

int mc_sync_bin_offered(mc_sync *sync) {
  if (!sync) return 0;
  mc_sync_lib_state lib_state;
  mc_status st = mc_sync_library_state(sync, &lib_state);
  if (st != MC_OK || lib_state != MC_SYNC_LIBRARY_GROUP) return 0;
  mc_mutex_lock(&sync->api_lock);
  int part = keeps_part(sync);
  mc_mutex_unlock(&sync->api_lock);
  return part > 0;
}

int mc_sync_bin_count(mc_sync *sync) {
  if (!sync) return -1;
  mc_mutex_lock(&sync->api_lock);
  int n = sync->api.lib ? mc_delete_bin_count(sync->api.lib) : 0;
  mc_mutex_unlock(&sync->api_lock);
  return n;
}

/* Under api_lock: takes what taken names off this device's profile, in a write of its own,
 * then confirms it again if it was confirmed before (as edit_profile()). An item excluded
 * already is left as it is. */
static mc_status exclude_taken(mc_sync *sync, const mc_bin_taken *taken) {
  mc_sync_db *sdb = sync->api.sdb;
  sqlite3 *db = mc_sync_db_handle(sdb);
  const char *me = sync->device_uid;
  int confirmed = mc_profile_confirm_state(db, me) == MC_SYNC_CONFIRMED;
  mc_status st = mc_sync_db_begin_write(sdb);
  if (st != MC_OK) return st;
  int selected = is_selection(db, me);
  if (selected < 0) st = MC_ERR_DB;
  for (int i = 0; st == MC_OK && i < taken->count; i++) {
    int album = taken->items[i].kind == MC_DELETE_ALBUMS;
    const char *kind = album ? MC_PLAN_EXCLUDE_ALBUM : MC_PLAN_EXCLUDE_TRACK;
    char uid[MC_UID_STRLEN];
    resolve(sync->api.lib, taken->items[i].uid, uid);
    int ex = has_row(sync, kind, uid);
    if (ex < 0) st = MC_ERR_DB;
    if (st != MC_OK || ex) continue;
    if (album && selected) {
      /* As Remove from this device does: the editor shows it unticked. */
      mc_profile_item item;
      memset(&item, 0, sizeof item);
      item.kind = MC_PROFILE_ALBUM;
      memcpy(item.ref_uid, uid, MC_UID_STRLEN);
      st = mc_profile_delete_rows(db, sync->api.lib, me, mc_profile_kind_str(MC_PROFILE_ALBUM), &item);
    }
    if (st == MC_OK) st = mc_profile_insert_row(db, me, kind, uid);
  }
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) {
    mc_sync_db_rollback(sdb);
    return st;
  }
  if (confirmed && mc_profile_confirm(sdb, NULL) != MC_OK)
    fprintf(stderr, "musiccore: sync: couldn't confirm the profile again after a restore\n");
  return MC_OK;
}

mc_status mc_sync_bin_restore(mc_sync *sync, int64_t id, int *entries) {
  if (entries) *entries = 0;
  if (!sync) return MC_ERR_INVALID_ARG;
  if (!mc_sync_bin_offered(sync)) return MC_ERR_LOCKED;
  /* Not while a library exchange may be sending rows: it empties the bin as it ends. */
  mc_mutex_lock(&sync->lock);
  int sending = sync->bin_sending > 0 || sync->bin_restoring;
  if (!sending) sync->bin_restoring = 1;
  mc_mutex_unlock(&sync->lock);
  if (sending) return MC_ERR_LOCKED;

  mc_mutex_lock(&sync->api_lock);
  mc_bin_taken taken;
  mc_status st = mc_delete_bin_take(sync->api.lib, id, &taken);
  if (st == MC_OK) {
    if (entries) *entries = taken.entries;
    mc_status pst = exclude_taken(sync, &taken);
    if (pst != MC_OK)
      fprintf(stderr, "musiccore: sync: a restore from the recycle bin couldn't edit the profile (%d);"
                      " it syncs here again\n", pst);
    mc_bin_taken_free(&taken);
  }
  mc_mutex_unlock(&sync->api_lock);

  mc_mutex_lock(&sync->lock);
  sync->bin_restoring = 0;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  /* The profile edit is group data for the others, and with the bin empty the scheduler may
   * start sessions again. */
  if (st == MC_OK) mc_sync_notify_changed(sync);
  return st;
}
