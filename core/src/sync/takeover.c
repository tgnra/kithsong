/* Lost archives and taking over. See takeover.h and "Taking over an archive" in
 * docs/sync-protocol.md. */
#include "sync/takeover.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "library/library_internal.h"
#include "sync/archive.h"
#include "sync/plan.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"
#include "sync/waiting.h"
#include "util/hlc.h"
#include "util/log.h"
#include "util/platform.h"

#define OFFER_TRACKS_MAX 100

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

static int same_text(const char *a, const char *b) {
  if (!a || !b || !*a || !*b) return 0;
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
  return !*a && !*b;
}

/* --- What an archive publishes --- */

void mc_archive_state_parse(const char *json, mc_archive_state *out) {
  memset(out, 0, sizeof *out);
  yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
  yyjson_val *root = yyjson_doc_get_root(doc);
  if (yyjson_is_obj(root)) {
    yyjson_val *v = yyjson_obj_get(root, "seen");
    if (yyjson_is_int(v)) out->seen_ms = yyjson_get_sint(v);
    yyjson_val *full = yyjson_obj_get(root, "full");
    if (yyjson_is_obj(full)) {
      out->full_tracks = (int)yyjson_get_sint(yyjson_obj_get(full, "tracks"));
      out->full_bytes = yyjson_get_sint(yyjson_obj_get(full, "bytes"));
    }
    yyjson_val *arr = yyjson_obj_get(root, "damaged");
    size_t i, max;
    yyjson_val *t;
    if (yyjson_is_arr(arr))
      yyjson_arr_foreach(arr, i, max, t) mc_uid_list_add(&out->damaged, yyjson_get_str(t));
    v = yyjson_obj_get(root, "damaged_n");
    out->damaged_n = yyjson_is_int(v) ? (int)yyjson_get_sint(v) : out->damaged.n;
  }
  yyjson_doc_free(doc);
}

void mc_archive_state_free(mc_archive_state *s) {
  mc_uid_list_free(&s->damaged);
}

static int self_archive(mc_sync_conn *c) {
  return mc_archive_is(mc_sync_db_handle(c->sdb), mc_sync_db_device_uid(c->sdb)) == 1;
}

void mc_archive_state_add(mc_sync *sync, mc_sync_conn *c, yyjson_mut_doc *doc, yyjson_mut_val *root) {
  if (!self_archive(c)) return;
  int64_t now = (int64_t)mc_wall_ms();
  yyjson_mut_obj_add_int(doc, root, "seen", now - now % MC_DAY_MS);

  int tracks = 0;
  int64_t bytes = 0;
  mc_mutex_lock(&sync->lock);
  for (int i = 0; i < sync->nfindings; i++) {
    const mc_findings *f = sync->findings[i];
    if (f->disk_full && f->need_tracks > tracks) {
      tracks = f->need_tracks;
      bytes = f->need_bytes + MC_SYNC_FETCH_SLACK - f->free_bytes;
    }
  }
  mc_mutex_unlock(&sync->lock);
  if (tracks > 0) {
    yyjson_mut_val *full = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, full, "tracks", tracks);
    yyjson_mut_obj_add_int(doc, full, "bytes", bytes > 0 ? bytes : 0);
    yyjson_mut_obj_add_val(doc, root, "full", full);
  }

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(c->lib),
                         "SELECT DISTINCT f.track_uid FROM file_failed x JOIN files f"
                         " ON f.uid = x.file_uid ORDER BY 1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return;
  yyjson_mut_val *arr = yyjson_mut_arr(doc);
  int n = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(uid)) continue;
    if (n++ < MC_ARCHIVE_DAMAGED_MAX) yyjson_mut_arr_add_strcpy(doc, arr, uid);
  }
  sqlite3_finalize(stmt);
  if (n) {
    yyjson_mut_obj_add_val(doc, root, "damaged", arr);
    yyjson_mut_obj_add_int(doc, root, "damaged_n", n);
  }
}

/* --- Where an archive has been --- */

static int64_t int_query(sqlite3 *db, const char *sql, const char *arg) {
  sqlite3_stmt *stmt;
  int64_t v = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(stmt, 1, arg, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return v;
}

int64_t mc_archive_last_seen(sqlite3 *db, const char *uid) {
  int64_t seen = int_query(db,
                           "SELECT max(coalesce(last_sync_ms, 0), coalesce(seen_ms, 0))"
                           " FROM peer_contact WHERE device_uid = ?1",
                           uid);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT state FROM capabilities WHERE device_uid = ?1", -1, &stmt,
                         NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      mc_archive_state s;
      mc_archive_state_parse((const char *)sqlite3_column_text(stmt, 0), &s);
      if (s.seen_ms > seen) seen = s.seen_ms;
      mc_archive_state_free(&s);
    }
    sqlite3_finalize(stmt);
  }
  return seen;
}

/* When this device joined the group: its devices row's first change. */
static int64_t joined_ms(sqlite3 *db, const char *me) {
  return (int64_t)mc_hlc_physical_ms((uint64_t)int_query(
      db, "SELECT min(hlc) FROM changelog WHERE entity = 'devices' AND row_uid = ?1", me));
}

/* Whether the library's files are here: most of the first few rows' files exist. */
static int files_present(mc_library *lib) {
  const char *dir = mc_library_dir(lib);
  sqlite3_stmt *stmt;
  if (!dir || sqlite3_prepare_v2(mc_library_db(lib), "SELECT path FROM files LIMIT 20", -1, &stmt,
                                 NULL) != SQLITE_OK)
    return 0;
  int rows = 0, found = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *rel = (const char *)sqlite3_column_text(stmt, 0);
    if (!rel) continue;
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    rows++;
    found += mc_file_exists(path) != 0;
  }
  sqlite3_finalize(stmt);
  return found > 0 && found * 2 >= rows;
}

static int taking_over(mc_sync *sync) {
  mc_takeover_view v;
  return mc_takeover_view_get(sync, &v) &&
         (v.state == MC_TAKEOVER_FETCHING || v.state == MC_TAKEOVER_CHECKING);
}

int mc_archive_find_lost(mc_sync *sync, mc_sync_conn *c, mc_lost_archive *out) {
  memset(out, 0, sizeof *out);
  sqlite3 *db = mc_sync_db_handle(c->sdb);
  const char *me = mc_sync_db_device_uid(c->sdb);
  if (!mc_sync_db_group_uid(c->sdb) || taking_over(sync) || mc_archive_is(db, me) != 0) return 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT uid, name, host, kind FROM devices WHERE removed = 0 AND uid <> ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, me, -1, SQLITE_STATIC);
  int archives = 0;
  char name[MC_SYNC_NAME_MAX] = "", host[MC_SYNC_NAME_MAX] = "";
  mc_device_kind kind = MC_DEVICE_DESKTOP;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(uid) || mc_archive_is(db, uid) != 1) continue;
    if (archives++) continue;
    memcpy(out->uid, uid, MC_UID_STRLEN);
    const char *n = (const char *)sqlite3_column_text(stmt, 1), *h = (const char *)sqlite3_column_text(stmt, 2);
    snprintf(name, sizeof name, "%s", n ? n : "");
    snprintf(host, sizeof host, "%s", h ? h : "");
    kind = mc_sync_kind_parse((const char *)sqlite3_column_text(stmt, 3));
  }
  sqlite3_finalize(stmt);
  if (archives != 1 || mc_sync_peer_online(sync, out->uid)) {
    memset(out, 0, sizeof *out);
    return 0;
  }
  int64_t last = mc_archive_last_seen(db, out->uid), now = (int64_t)mc_wall_ms();
  out->last_seen_ms = last;
  if (last > 0) {
    out->away = now - last >= MC_SYNC_ARCHIVE_AWAY_DAYS * MC_DAY_MS;
    out->lost = now - last >= MC_SYNC_ARCHIVE_LOST_DAYS * MC_DAY_MS;
  }
  /* A reinstall: the same kind, the same host or name, the group's library with its files
   * here, and joined since the archive was last seen. Names alone are never enough. */
  char my_name[MC_SYNC_NAME_MAX];
  mc_mutex_lock(&sync->lock);
  memcpy(my_name, sync->name, sizeof my_name);
  mc_mutex_unlock(&sync->lock);
  out->reinstall = kind == sync->kind && (same_text(host, sync->host) || same_text(name, my_name)) &&
                   joined_ms(db, me) > last && mc_waiting_library_is_groups(c->sdb, c->lib) &&
                   files_present(c->lib);
  if (!out->away && !out->lost && !out->reinstall) {
    memset(out, 0, sizeof *out);
    return 0;
  }
  return 1;
}

/* --- Who holds the originals ---
 * Of a track of the archive's plan (everything, as originals): this device, or a current
 * member other than the archive whose profile has it hold the original, or that imported it,
 * and that hasn't said it lacks it. */

typedef struct holder {
  char uid[MC_UID_STRLEN];
  char node[MC_UID_STRLEN];
  mc_plan *plan;
  int64_t free_bytes; /* -1: not known */
  int64_t held;       /* bytes of originals it holds, as the walk adds them up */
} holder;

typedef struct holders {
  mc_sync_conn *c;
  mc_plan *all;
  holder *ms;
  int n;
  sqlite3_stmt *rev, *first, *lacking, *satisfied;
} holders;

static void holders_close(holders *h) {
  for (int i = 0; i < h->n; i++) mc_plan_free(h->ms[i].plan);
  free(h->ms);
  mc_plan_free(h->all);
  sqlite3_finalize(h->rev);
  sqlite3_finalize(h->first);
  sqlite3_finalize(h->lacking);
  sqlite3_finalize(h->satisfied);
  memset(h, 0, sizeof *h);
}

/* MC_OK with h->all NULL if the archive's plan can't be had. */
static mc_status holders_open(mc_sync_conn *c, const char *archive, holders *h) {
  memset(h, 0, sizeof *h);
  h->c = c;
  sqlite3 *db = mc_sync_db_handle(c->sdb);
  mc_status st = mc_plan_compute(c->sdb, c->lib, archive, -1, &h->all);
  if (st != MC_OK) return st;
  if (h->all->status != MC_PLAN_OK) {
    mc_plan_free(h->all);
    h->all = NULL;
    return MC_OK;
  }
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT d.uid, d.node_uid, c.free_bytes, s.device_uid IS NOT NULL"
                         " FROM devices d LEFT JOIN capabilities c ON c.device_uid = d.uid"
                         " LEFT JOIN sync_status s ON s.device_uid = d.uid"
                         " WHERE d.removed = 0 AND d.uid <> ?1 AND d.uid <> ?2",
                         -1, &stmt, NULL) != SQLITE_OK) {
    holders_close(h);
    return MC_ERR_DB;
  }
  sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(c->sdb), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, archive, -1, SQLITE_STATIC);
  while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(uid)) continue;
    holder *more = realloc(h->ms, (size_t)(h->n + 1) * sizeof *more);
    if (!more) {
      st = MC_ERR_NOMEM;
      break;
    }
    h->ms = more;
    holder *m = &h->ms[h->n++];
    memset(m, 0, sizeof *m);
    memcpy(m->uid, uid, MC_UID_STRLEN);
    const char *node = (const char *)sqlite3_column_text(stmt, 1);
    if (is_uid(node)) memcpy(m->node, node, MC_UID_STRLEN);
    m->free_bytes = sqlite3_column_type(stmt, 2) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 2);
    /* Without a report of its files it has had no file phase: it holds nothing yet. */
    if (sqlite3_column_int(stmt, 3)) {
      st = mc_plan_compute(c->sdb, c->lib, uid, m->free_bytes, &m->plan);
      if (st == MC_OK && m->plan->status != MC_PLAN_OK) {
        mc_plan_free(m->plan);
        m->plan = NULL;
      }
    }
  }
  sqlite3_finalize(stmt);
  sqlite3 *ldb = mc_library_db(c->lib);
  if (st == MC_OK &&
      (sqlite3_prepare_v2(db, "SELECT 1 FROM peer_lacks WHERE device_uid = ?1 AND track_uid = ?2",
                          -1, &h->lacking, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(ldb,
                          "SELECT origin_uid FROM changelog WHERE entity = 'tracks'"
                          " AND row_uid = ?1 AND field = 'file_rev' ORDER BY hlc DESC LIMIT 1",
                          -1, &h->rev, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(ldb,
                          "SELECT origin_uid FROM changelog WHERE entity = 'tracks'"
                          " AND row_uid = ?1 ORDER BY hlc LIMIT 1",
                          -1, &h->first, NULL) != SQLITE_OK))
    st = MC_ERR_DB;
  if (st != MC_OK) holders_close(h);
  return st;
}

static int here(holders *h, const mc_plan_track *t) {
  return mc_transfer_track_satisfied(h->c->lib, &h->satisfied, t);
}

static int importer(holders *h, const char *track) {
  char origin[MC_UID_STRLEN] = "";
  sqlite3_stmt *q[2] = {h->rev, h->first};
  for (int k = 0; k < 2 && !origin[0]; k++) {
    sqlite3_reset(q[k]);
    sqlite3_bind_text(q[k], 1, track, -1, SQLITE_STATIC);
    if (sqlite3_step(q[k]) == SQLITE_ROW) {
      const char *v = (const char *)sqlite3_column_text(q[k], 0);
      if (is_uid(v)) memcpy(origin, v, MC_UID_STRLEN);
    }
    sqlite3_reset(q[k]);
  }
  for (int i = 0; origin[0] && i < h->n; i++)
    if (h->ms[i].node[0] && !strcmp(h->ms[i].node, origin)) return i;
  return -1;
}

static int holds(holders *h, int i, const mc_plan_track *t, int imp) {
  const mc_plan_track *theirs = h->ms[i].plan ? mc_plan_find(h->ms[i].plan, t->track_uid) : NULL;
  if (i != imp && !(theirs && theirs->format.format == MC_FORMAT_ORIGINAL)) return 0;
  sqlite3_reset(h->lacking);
  sqlite3_bind_text(h->lacking, 1, h->ms[i].uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(h->lacking, 2, t->track_uid, -1, SQLITE_STATIC);
  int lacks = sqlite3_step(h->lacking) == SQLITE_ROW;
  sqlite3_reset(h->lacking);
  return !lacks;
}

/* Whether a member holds t's original, adding its size to each one that does. */
static int elsewhere(holders *h, const mc_plan_track *t) {
  int imp = importer(h, t->track_uid), any = 0;
  for (int i = 0; i < h->n; i++)
    if (holds(h, i, t, imp)) {
      h->ms[i].held += t->est_bytes;
      any = 1;
    }
  return any;
}

mc_status mc_archive_only_there(mc_sync_conn *c, const char *archive, const mc_uid_list *tracks,
                                mc_uid_list *out) {
  memset(out, 0, sizeof *out);
  holders h;
  mc_status st = holders_open(c, archive, &h);
  if (st != MC_OK || !h.all) return st;
  for (int i = 0; i < tracks->n; i++) {
    const mc_plan_track *t = mc_plan_find(h.all, tracks->uids[i]);
    if (t && !here(&h, t) && !elsewhere(&h, t) && !mc_uid_list_add(out, t->track_uid)) {
      st = MC_ERR_NOMEM;
      break;
    }
  }
  holders_close(&h);
  return st;
}

/* --- Track names --- */

static int lost_cmp(const void *x, const void *y) {
  const mc_sync_lost_track *a = x, *b = y;
  int c = strcmp(a->title ? a->title : "", b->title ? b->title : "");
  return c ? c : strcmp(a->uid, b->uid);
}

/* Up to OFFER_TRACKS_MAX of l, named and sorted by title. */
static mc_status name_tracks(mc_library *lib, const mc_uid_list *l, mc_sync_lost_track **out,
                             int *count) {
  *out = NULL;
  *count = 0;
  int n = l->n < OFFER_TRACKS_MAX ? l->n : OFFER_TRACKS_MAX;
  if (!n) return MC_OK;
  mc_sync_lost_track *v = calloc((size_t)n, sizeof *v);
  sqlite3_stmt *stmt = NULL;
  if (!v) return MC_ERR_NOMEM;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT t.title, coalesce(t.artist_credit, ar.name) FROM tracks t"
                         " LEFT JOIN artists ar ON ar.uid = t.artist_uid WHERE t.uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK) {
    free(v);
    return MC_ERR_DB;
  }
  mc_status st = MC_OK;
  for (int i = 0; i < n; i++) {
    memcpy(v[i].uid, l->uids[i], MC_UID_STRLEN);
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, l->uids[i], -1, SQLITE_STATIC);
    const char *title = NULL, *artist = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      title = (const char *)sqlite3_column_text(stmt, 0);
      artist = (const char *)sqlite3_column_text(stmt, 1);
    }
    v[i].title = mc_strdup(title ? title : "");
    v[i].artist = artist ? mc_strdup(artist) : NULL;
    if (!v[i].title) st = MC_ERR_NOMEM;
  }
  sqlite3_finalize(stmt);
  qsort(v, (size_t)n, sizeof *v, lost_cmp);
  *out = v;
  *count = n;
  return st;
}

static void tracks_free(mc_sync_lost_track *v, int n) {
  for (int i = 0; i < n; i++) {
    free(v[i].title);
    free(v[i].artist);
  }
  free(v);
}

/* --- The offer --- */

mc_status mc_takeover_offer(mc_sync *sync, mc_sync_conn *c, mc_sync_archive_offer **out) {
  *out = NULL;
  mc_lost_archive la;
  if (!mc_archive_find_lost(sync, c, &la) || (!la.lost && !la.reinstall)) return MC_OK;
  holders h;
  mc_status st = holders_open(c, la.uid, &h);
  if (st != MC_OK || !h.all) return st;
  int64_t total = 0, held = 0;
  mc_uid_list gone = {0};
  for (int i = 0; st == MC_OK && i < h.all->count; i++) {
    const mc_plan_track *t = &h.all->tracks[i];
    total += t->est_bytes;
    int mine = here(&h, t), others = elsewhere(&h, t);
    if (mine) held += t->est_bytes;
    else if (!others && !mc_uid_list_add(&gone, t->track_uid)) st = MC_ERR_NOMEM;
  }
  mc_sync_archive_offer *o = st == MC_OK ? calloc(1, sizeof *o) : NULL;
  if (st == MC_OK && !o) st = MC_ERR_NOMEM;
  if (o) {
    sqlite3 *db = mc_sync_db_handle(c->sdb);
    memcpy(o->device_uid, la.uid, MC_UID_STRLEN);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
                           "SELECT coalesce(nullif(p.name, ''), d.name), d.kind FROM devices d"
                           " LEFT JOIN profiles p ON p.device_uid = d.uid WHERE d.uid = ?1",
                           -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, la.uid, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        if (name) o->name = mc_strdup(name);
        o->kind = mc_sync_kind_parse((const char *)sqlite3_column_text(stmt, 1));
      }
      sqlite3_finalize(stmt);
    }
    o->last_seen_ms = la.last_seen_ms;
    o->reinstall = la.reinstall;
    o->bytes_needed = total - held;
    o->bytes_free = mc_sync_free_space(c->lib);
    o->can_take = o->bytes_free >= 0 && o->bytes_free >= o->bytes_needed + MC_SYNC_FETCH_SLACK;
    o->nobody_can = !o->can_take && o->bytes_free >= 0;
    for (int i = 0; o->nobody_can && i < h.n; i++)
      o->nobody_can = h.ms[i].free_bytes >= 0 &&
                      h.ms[i].free_bytes < total - h.ms[i].held + MC_SYNC_FETCH_SLACK;
    o->asked = int_query(db, "SELECT offer_asked FROM peer_contact WHERE device_uid = ?1", la.uid) != 0;
    o->unrecoverable = gone.n;
    st = name_tracks(c->lib, &gone, &o->tracks, &o->ntracks);
  }
  mc_uid_list_free(&gone);
  holders_close(&h);
  if (st != MC_OK) {
    mc_sync_archive_offer_free(o);
    return st;
  }
  *out = o;
  return MC_OK;
}

mc_status mc_sync_get_archive_offer(mc_sync *sync, mc_sync_archive_offer **out) {
  if (out) *out = NULL;
  if (!sync || !out) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->api_lock);
  mc_status st = mc_takeover_offer(sync, &sync->api, out);
  mc_mutex_unlock(&sync->api_lock);
  return st;
}

void mc_sync_archive_offer_free(mc_sync_archive_offer *offer) {
  if (!offer) return;
  free(offer->name);
  tracks_free(offer->tracks, offer->ntracks);
  free(offer);
}

static mc_status exec_uid(mc_sync_db *sdb, const char *sql, const char *uid) {
  mc_status st = mc_sync_db_begin_write(sdb);
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt;
  st = MC_ERR_DB;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb), sql, -1, &stmt, NULL) == SQLITE_OK) {
    if (uid) sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_DONE) st = MC_OK;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) mc_sync_db_rollback(sdb);
  return st;
}

mc_status mc_sync_archive_offer_seen(mc_sync *sync, const char *device_uid) {
  if (!sync || !is_uid(device_uid)) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->api_lock);
  mc_status st = exec_uid(sync->api.sdb,
                          "INSERT INTO peer_contact (device_uid, offer_asked) VALUES (?1, 1)"
                          " ON CONFLICT (device_uid) DO UPDATE SET offer_asked = 1",
                          device_uid);
  mc_mutex_unlock(&sync->api_lock);
  return st;
}

/* --- Taking over --- */

typedef struct mc_takeover_run {
  mc_takeover_state state;
  char uid[MC_UID_STRLEN];
  char *name;
  int total, done;
  mc_status status;
  int reinstall;
  mc_uid_list unrecovered;
  mc_sync_lost_track *tracks;
  int ntracks;
} mc_takeover_run;

static void run_free(mc_takeover_run *r) {
  if (!r) return;
  free(r->name);
  mc_uid_list_free(&r->unrecovered);
  tracks_free(r->tracks, r->ntracks);
  free(r);
}

void mc_takeover_free(mc_sync *sync) {
  run_free(sync->takeover);
  sync->takeover = NULL;
}

int mc_takeover_view_get(mc_sync *sync, mc_takeover_view *out) {
  memset(out, 0, sizeof *out);
  mc_mutex_lock(&sync->lock);
  const mc_takeover_run *r = sync->takeover;
  if (r) {
    out->state = r->state;
    memcpy(out->uid, r->uid, MC_UID_STRLEN);
    out->total = r->total;
    out->done = r->done;
    out->status = r->status;
  }
  mc_mutex_unlock(&sync->lock);
  return r != NULL;
}

int mc_takeover_unrecovered(mc_sync *sync, mc_uid_list *out) {
  mc_mutex_lock(&sync->lock);
  int ok = sync->takeover && mc_uid_list_copy(out, &sync->takeover->unrecovered);
  mc_mutex_unlock(&sync->lock);
  if (!ok) memset(out, 0, sizeof *out);
  return ok;
}

static void set_state(mc_sync *sync, mc_takeover_state state, int total, int done, mc_status status) {
  mc_mutex_lock(&sync->lock);
  mc_takeover_run *r = sync->takeover;
  if (r && (r->state != state || r->total != total || r->done != done || r->status != status)) {
    r->state = state;
    r->total = total;
    r->done = done;
    r->status = status;
    sync->waiting_dirty = 1;
    mc_cond_broadcast(&sync->cond);
  }
  mc_mutex_unlock(&sync->lock);
}

/* How far it has got: tracks held here, tracks a member could still send, and the rest
 * (into left, which may be NULL). */
static mc_status progress(mc_sync_conn *c, const char *lost, int *total, int *done, int *pending,
                          mc_uid_list *left) {
  *total = *done = *pending = 0;
  holders h;
  mc_status st = holders_open(c, lost, &h);
  if (st != MC_OK) return st;
  if (!h.all) return MC_ERR_DB;
  for (int i = 0; i < h.all->count; i++) {
    const mc_plan_track *t = &h.all->tracks[i];
    (*total)++;
    if (here(&h, t)) (*done)++;
    else if (elsewhere(&h, t)) (*pending)++;
    else if (left && !mc_uid_list_add(left, t->track_uid)) st = MC_ERR_NOMEM;
  }
  holders_close(&h);
  return st;
}

static int still_member(mc_sync_conn *c, const char *uid) {
  return int_query(mc_sync_db_handle(c->sdb),
                   "SELECT count(*) FROM devices WHERE uid = ?1 AND removed = 0", uid) > 0;
}

static void finish(mc_sync *sync, mc_sync_conn *c, const char *lost, mc_takeover_state state,
                   mc_status status) {
  exec_uid(c->sdb, "UPDATE local SET takeover_uid = NULL", NULL);
  int total = 0, done = 0, pending = 0;
  mc_uid_list left = {0};
  mc_sync_lost_track *tracks = NULL;
  int ntracks = 0;
  if (state == MC_TAKEOVER_DONE) {
    progress(c, mc_sync_db_device_uid(c->sdb), &total, &done, &pending, &left);
    name_tracks(c->lib, &left, &tracks, &ntracks);
  }
  mc_mutex_lock(&sync->lock);
  mc_takeover_run *r = sync->takeover;
  if (r) {
    mc_uid_list_free(&r->unrecovered);
    tracks_free(r->tracks, r->ntracks);
    r->unrecovered = left;
    r->tracks = tracks;
    r->ntracks = ntracks;
    memset(&left, 0, sizeof left);
    tracks = NULL;
    ntracks = 0;
    if (state == MC_TAKEOVER_DONE) {
      r->total = total;
      r->done = done;
    }
  }
  mc_mutex_unlock(&sync->lock);
  mc_uid_list_free(&left);
  tracks_free(tracks, ntracks);
  set_state(sync, state, total, done, status);
  if (state == MC_TAKEOVER_DONE) {
    mc_log("kithcore: sync: took over from archive %s\n", lost);
    mc_sync_add_notice(sync, MC_ISSUE_TAKEOVER, lost, 0);
  } else {
    mc_log("kithcore: sync: taking over from %s failed: %s\n", lost, mc_status_str(status));
  }
  mc_sync_waiting_changed(sync);
}

static void wait_a_while(mc_sync *sync, uint32_t ms) {
  mc_mutex_lock(&sync->lock);
  if (!sync->stopping) mc_cond_wait_ms(&sync->cond, &sync->lock, ms);
  mc_mutex_unlock(&sync->lock);
}

static void takeover_main(void *arg) {
  mc_sync *sync = arg;
  char lost[MC_UID_STRLEN];
  int reinstall;
  mc_mutex_lock(&sync->lock);
  memcpy(lost, sync->takeover->uid, MC_UID_STRLEN);
  reinstall = sync->takeover->reinstall;
  mc_mutex_unlock(&sync->lock);
  mc_sync_conn c;
  if (mc_sync_conn_open(sync, &c) != MC_OK) {
    set_state(sync, MC_TAKEOVER_FAILED, 0, 0, MC_ERR_DB);
    return;
  }
  /* The files a reinstalled device has are checked before anything is fetched over them. */
  if (reinstall) {
    set_state(sync, MC_TAKEOVER_CHECKING, 0, 0, MC_OK);
    mc_sync_verify(sync, MC_VERIFY_DEEP, NULL);
  }
  int checked = 0;
  while (!mc_sync_is_stopping(sync)) {
    if (!still_member(&c, lost)) {
      /* Another device removed it: it had taken over, or the user did. */
      finish(sync, &c, lost, MC_TAKEOVER_DONE, MC_OK);
      break;
    }
    int total, done, pending;
    mc_status st = progress(&c, lost, &total, &done, &pending, NULL);
    if (st != MC_OK) {
      finish(sync, &c, lost, MC_TAKEOVER_FAILED, st);
      break;
    }
    if (pending) {
      checked = 0;
      set_state(sync, MC_TAKEOVER_FETCHING, total, done, MC_OK);
      wait_a_while(sync, 1000);
      continue;
    }
    if (!checked) {
      /* Nothing more to fetch: what's here is checked, and looked at again. */
      set_state(sync, MC_TAKEOVER_CHECKING, total, done, MC_OK);
      mc_sync_verify(sync, MC_VERIFY_FAST, NULL);
      checked = 1;
      continue;
    }
    st = mc_sync_remove_device(sync, lost);
    if (st == MC_OK || st == MC_ERR_NOT_FOUND) finish(sync, &c, lost, MC_TAKEOVER_DONE, MC_OK);
    else finish(sync, &c, lost, MC_TAKEOVER_FAILED, st);
    break;
  }
  mc_sync_conn_close(&c);
}

/* Makes this device an archive taking over from lost, and starts the worker. */
static mc_status begin(mc_sync *sync, const char *lost, const char *name, int reinstall) {
  mc_takeover_run *r = calloc(1, sizeof *r);
  if (!r) return MC_ERR_NOMEM;
  memcpy(r->uid, lost, MC_UID_STRLEN);
  r->name = name ? mc_strdup(name) : NULL;
  r->reinstall = reinstall;
  r->state = MC_TAKEOVER_FETCHING;
  mc_status st = mc_sync_set_archive(sync, sync->device_uid, 1);
  if (st != MC_OK) {
    run_free(r);
    return st;
  }
  mc_mutex_lock(&sync->lock);
  mc_takeover_run *old = sync->takeover;
  sync->takeover = r;
  sync->waiting_dirty = 1;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  run_free(old);
  st = mc_sync_spawn(sync, takeover_main, sync);
  if (st != MC_OK) set_state(sync, MC_TAKEOVER_FAILED, 0, 0, st);
  return st;
}

mc_status mc_sync_take_over_archive(mc_sync *sync, const char *lost_uid) {
  if (!sync || !is_uid(lost_uid)) return MC_ERR_INVALID_ARG;
  if (taking_over(sync)) return MC_ERR_LOCKED;
  mc_mutex_lock(&sync->api_lock);
  mc_sync_archive_offer *o = NULL;
  mc_status st = mc_takeover_offer(sync, &sync->api, &o);
  if (st == MC_OK && (!o || strcmp(o->device_uid, lost_uid))) st = MC_ERR_NOT_FOUND;
  else if (st == MC_OK && !o->can_take) st = MC_ERR_LOCKED;
  if (st == MC_OK)
    st = exec_uid(sync->api.sdb, "UPDATE local SET takeover_uid = ?1", lost_uid);
  mc_mutex_unlock(&sync->api_lock);
  if (st == MC_OK) {
    st = begin(sync, lost_uid, o->name, o->reinstall);
    if (st != MC_OK) {
      mc_mutex_lock(&sync->api_lock);
      exec_uid(sync->api.sdb, "UPDATE local SET takeover_uid = NULL", NULL);
      mc_mutex_unlock(&sync->api_lock);
    } else {
      mc_sync_now(sync, NULL);
    }
  }
  mc_sync_archive_offer_free(o);
  return st;
}

void mc_takeover_resume(mc_sync *sync) {
  char lost[MC_UID_STRLEN] = "", name[MC_SYNC_NAME_MAX] = "";
  int member = 0;
  mc_mutex_lock(&sync->api_lock);
  sqlite3 *db = mc_sync_db_handle(sync->api.sdb);
  sqlite3_stmt *stmt;
  if (mc_sync_db_group_uid(sync->api.sdb) &&
      sqlite3_prepare_v2(db, "SELECT takeover_uid FROM local", -1, &stmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *v = (const char *)sqlite3_column_text(stmt, 0);
      if (is_uid(v)) memcpy(lost, v, MC_UID_STRLEN);
    }
    sqlite3_finalize(stmt);
  }
  if (lost[0]) {
    member = still_member(&sync->api, lost);
    if (sqlite3_prepare_v2(db, "SELECT name FROM devices WHERE uid = ?1", -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, lost, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        snprintf(name, sizeof name, "%s", v ? v : "");
      }
      sqlite3_finalize(stmt);
    }
    if (!member) exec_uid(sync->api.sdb, "UPDATE local SET takeover_uid = NULL", NULL);
  }
  mc_mutex_unlock(&sync->api_lock);
  if (member && begin(sync, lost, name, 0) == MC_OK) mc_sync_background(sync, NULL);
}

mc_status mc_sync_takeover_get(mc_sync *sync, mc_sync_takeover **out) {
  if (out) *out = NULL;
  if (!sync || !out) return MC_ERR_INVALID_ARG;
  mc_sync_takeover *t = calloc(1, sizeof *t);
  if (!t) return MC_ERR_NOMEM;
  mc_status st = MC_OK;
  mc_mutex_lock(&sync->lock);
  const mc_takeover_run *r = sync->takeover;
  if (r) {
    t->state = r->state;
    memcpy(t->device_uid, r->uid, MC_UID_STRLEN);
    t->name = r->name ? mc_strdup(r->name) : NULL;
    t->tracks_total = r->total;
    t->tracks_done = r->done;
    t->status = r->status;
    t->unrecovered = r->unrecovered.n;
    if (r->ntracks) {
      t->tracks = calloc((size_t)r->ntracks, sizeof *t->tracks);
      if (!t->tracks) st = MC_ERR_NOMEM;
      for (int i = 0; t->tracks && i < r->ntracks; i++, t->ntracks++) {
        memcpy(t->tracks[i].uid, r->tracks[i].uid, MC_UID_STRLEN);
        t->tracks[i].title = mc_strdup(r->tracks[i].title ? r->tracks[i].title : "");
        t->tracks[i].artist = r->tracks[i].artist ? mc_strdup(r->tracks[i].artist) : NULL;
        if (!t->tracks[i].title) st = MC_ERR_NOMEM;
      }
    }
  }
  mc_mutex_unlock(&sync->lock);
  if (st != MC_OK) {
    mc_sync_takeover_free(t);
    return st;
  }
  *out = t;
  return MC_OK;
}

void mc_sync_takeover_free(mc_sync_takeover *t) {
  if (!t) return;
  free(t->name);
  tracks_free(t->tracks, t->ntracks);
  free(t);
}
