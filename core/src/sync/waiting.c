/* Tracks waiting for a device. See waiting.h and "Waiting for a device" in
 * docs/sync-protocol.md.
 *
 * This device's plan gives the wanted tracks it lacks; each other current member's plan
 * (one read of every track per member) and the library changelog's origins give who holds
 * them. Every plan is sorted by track uid, so finding a track in one is a binary search. */
#include "sync/waiting.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/i18n.h"
#include "sync/libmerge.h"
#include "sync/plan.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"
#include "util/platform.h"

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

/* Another current member, as far as the waiting tracks go. */
typedef struct member {
  char uid[MC_UID_STRLEN];
  char node[MC_UID_STRLEN]; /* its library's node_uid, "" if unknown */
  char name[MC_SYNC_NAME_MAX];
  mc_device_kind kind;
  mc_plan *plan;            /* NULL: it holds nothing by its profile (none, never confirmed,
                             * over budget) */
  int reachable;
  int tracks;               /* waiting tracks it holds */
} member;

static void members_free(member *ms, int n) {
  for (int i = 0; i < n; i++) mc_plan_free(ms[i].plan);
  free(ms);
}

/* The other current members, with what their profiles have them hold. */
static mc_status load_members(mc_sync_db *sdb, mc_library *lib, member **out, int *count) {
  *out = NULL;
  *count = 0;
  sqlite3 *db = mc_sync_db_handle(sdb);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT d.uid, d.node_uid, coalesce(nullif(p.name, ''), d.name), d.kind,"
                         " c.free_bytes FROM devices d LEFT JOIN profiles p ON p.device_uid = d.uid"
                         " LEFT JOIN capabilities c ON c.device_uid = d.uid"
                         " WHERE d.removed = 0 AND d.uid <> ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(sdb), -1, SQLITE_STATIC);
  mc_status st = MC_OK;
  member *ms = NULL;
  int n = 0;
  int64_t *free_bytes = NULL;
  while (st == MC_OK && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(uid)) continue;
    member *more = realloc(ms, (size_t)(n + 1) * sizeof *more);
    int64_t *more_free = realloc(free_bytes, (size_t)(n + 1) * sizeof *more_free);
    if (more) ms = more;
    if (more_free) free_bytes = more_free;
    if (!more || !more_free) {
      st = MC_ERR_NOMEM;
      break;
    }
    member *m = &ms[n];
    memset(m, 0, sizeof *m);
    memcpy(m->uid, uid, MC_UID_STRLEN);
    const char *node = (const char *)sqlite3_column_text(stmt, 1);
    if (is_uid(node)) memcpy(m->node, node, MC_UID_STRLEN);
    const char *name = (const char *)sqlite3_column_text(stmt, 2);
    snprintf(m->name, sizeof m->name, "%s", name ? name : "");
    m->kind = mc_sync_kind_parse((const char *)sqlite3_column_text(stmt, 3));
    free_bytes[n] =
        sqlite3_column_type(stmt, 4) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 4);
    n++;
  }
  sqlite3_finalize(stmt);
  for (int i = 0; st == MC_OK && i < n; i++) {
    int confirm = mc_profile_confirm_state(db, ms[i].uid);
    if (confirm < 0) {
      st = MC_ERR_DB;
      break;
    }
    /* Never confirmed: it has fetched nothing for its profile yet. */
    if (confirm == MC_SYNC_UNCONFIRMED) continue;
    mc_plan *plan = NULL;
    st = mc_plan_compute(sdb, lib, ms[i].uid, free_bytes[i], &plan);
    if (st == MC_OK && plan->status != MC_PLAN_OK) {
      mc_plan_free(plan);
      plan = NULL;
    }
    ms[i].plan = plan;
  }
  free(free_bytes);
  if (st != MC_OK) {
    members_free(ms, n);
    return st;
  }
  *out = ms;
  *count = n;
  return MC_OK;
}

/* The member whose library node logged the track's current original: the newest file_rev
 * change, else the track's first change. -1 for none (this device, an unknown or rotated
 * node, a member since removed). */
static int importer(sqlite3_stmt *rev, sqlite3_stmt *first, const char *track, const member *ms,
                    int n) {
  char origin[MC_UID_STRLEN] = "";
  sqlite3_stmt *q[2] = {rev, first};
  for (int k = 0; k < 2 && !origin[0]; k++) {
    sqlite3_reset(q[k]);
    sqlite3_bind_text(q[k], 1, track, -1, SQLITE_STATIC);
    if (sqlite3_step(q[k]) == SQLITE_ROW) {
      const char *v = (const char *)sqlite3_column_text(q[k], 0);
      if (is_uid(v)) memcpy(origin, v, MC_UID_STRLEN);
    }
    sqlite3_reset(q[k]);
  }
  if (!origin[0]) return -1;
  for (int i = 0; i < n; i++)
    if (ms[i].node[0] && !strcmp(ms[i].node, origin)) return i;
  return -1;
}

/* Whether m's profile has it hold what t needs: the original, or Opus at t's very bitrate. */
static int holds(const member *m, const mc_plan_track *t) {
  const mc_plan_track *theirs = mc_plan_find(m->plan, t->track_uid);
  if (!theirs) return 0;
  if (theirs->format.format == MC_FORMAT_ORIGINAL) return 1;
  return t->format.format == MC_FORMAT_OPUS &&
         theirs->format.bitrate_kbps == t->format.bitrate_kbps;
}

/* Whether this device fetches files now: in a group, the group's library, its profile
 * confirmed and no merged library still to come in. */
static int fetching(mc_sync_db *sdb, mc_library *lib) {
  const char *group = mc_sync_db_group_uid(sdb);
  if (!group) return 0;
  char group_lib[MC_UID_STRLEN] = "", mine[MC_UID_STRLEN] = "";
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "SELECT library_uid FROM sync_group WHERE uid = ?1", -1, &stmt,
                         NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, group, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *v = (const char *)sqlite3_column_text(stmt, 0);
      if (is_uid(v)) memcpy(group_lib, v, MC_UID_STRLEN);
    }
    sqlite3_finalize(stmt);
  }
  if (sqlite3_prepare_v2(mc_library_db(lib), "SELECT value FROM meta WHERE key = 'library_uid'", -1,
                         &stmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *v = (const char *)sqlite3_column_text(stmt, 0);
      if (is_uid(v)) memcpy(mine, v, MC_UID_STRLEN);
    }
    sqlite3_finalize(stmt);
  }
  if (!group_lib[0] || strcmp(group_lib, mine) != 0) return 0;
  if (mc_profile_confirm_state(mc_sync_db_handle(sdb), mc_sync_db_device_uid(sdb)) !=
      MC_SYNC_CONFIRMED)
    return 0;
  return !mc_libmerge_pending(lib, NULL);
}

/* Names ignoring ASCII case, as the device list orders them (COLLATE NOCASE). */
static int name_cmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
    if (x != y || !x) return x - y;
  }
}

static int device_cmp(const void *a, const void *b) {
  const mc_sync_waiting_device *x = a, *y = b;
  if (x->tracks != y->tracks) return x->tracks > y->tracks ? -1 : 1;
  int c = name_cmp(x->name ? x->name : "", y->name ? y->name : "");
  return c ? c : strcmp(x->uid, y->uid);
}

mc_status mc_waiting_compute(mc_sync_db *sdb, mc_library *lib, mc_waiting_reach_fn reachable,
                             void *user, mc_sync_waiting **out) {
  if (out) *out = NULL;
  if (!sdb || !lib || !reachable || !out) return MC_ERR_INVALID_ARG;
  mc_sync_waiting *w = calloc(1, sizeof *w);
  if (!w) return MC_ERR_NOMEM;
  if (!fetching(sdb, lib)) {
    *out = w;
    return MC_OK;
  }

  int64_t free_bytes = -1;
  if (mc_free_space(mc_library_dir(lib), &free_bytes) != MC_OK) free_bytes = -1;
  mc_plan *plan = NULL;
  mc_status st = mc_plan_compute(sdb, lib, mc_sync_db_device_uid(sdb), free_bytes, &plan);
  if (st != MC_OK || plan->status != MC_PLAN_OK) {
    mc_plan_free(plan);
    if (st != MC_OK) {
      free(w);
      return st;
    }
    *out = w;
    return MC_OK;
  }

  member *ms = NULL;
  int n = 0;
  st = load_members(sdb, lib, &ms, &n);
  for (int i = 0; st == MC_OK && i < n; i++) ms[i].reachable = reachable(user, ms[i].uid) != 0;

  sqlite3 *ldb = mc_library_db(lib);
  sqlite3_stmt *rev = NULL, *first = NULL, *satisfied = NULL;
  if (st == MC_OK &&
      (sqlite3_prepare_v2(ldb,
                          "SELECT origin_uid FROM changelog WHERE entity = 'tracks'"
                          " AND row_uid = ?1 AND field = 'file_rev' ORDER BY hlc DESC LIMIT 1",
                          -1, &rev, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(ldb,
                          "SELECT origin_uid FROM changelog WHERE entity = 'tracks'"
                          " AND row_uid = ?1 ORDER BY hlc LIMIT 1",
                          -1, &first, NULL) != SQLITE_OK))
    st = MC_ERR_DB;

  char *held = n ? malloc((size_t)n) : NULL;
  if (st == MC_OK && n && !held) st = MC_ERR_NOMEM;
  for (int t = 0; st == MC_OK && n && t < plan->count; t++) {
    const mc_plan_track *track = &plan->tracks[t];
    if (mc_transfer_track_satisfied(lib, &satisfied, track)) continue;
    int any = 0, reached = 0;
    for (int i = 0; i < n; i++) {
      held[i] = ms[i].plan && holds(&ms[i], track);
      any |= held[i];
      reached |= held[i] && ms[i].reachable;
    }
    int imp = importer(rev, first, track->track_uid, ms, n);
    if (imp >= 0) {
      held[imp] = 1;
      any = 1;
      reached |= ms[imp].reachable;
    }
    if (!any || reached) continue;
    w->tracks++;
    for (int i = 0; i < n; i++) ms[i].tracks += held[i];
  }
  free(held);
  sqlite3_finalize(rev);
  sqlite3_finalize(first);
  sqlite3_finalize(satisfied);
  mc_plan_free(plan);

  for (int i = 0; st == MC_OK && i < n; i++) {
    if (!ms[i].tracks) continue;
    mc_sync_waiting_device *more = realloc(w->devices, (size_t)(w->count + 1) * sizeof *more);
    if (!more) {
      st = MC_ERR_NOMEM;
      break;
    }
    w->devices = more;
    mc_sync_waiting_device *d = &w->devices[w->count++];
    memset(d, 0, sizeof *d);
    memcpy(d->uid, ms[i].uid, MC_UID_STRLEN);
    d->name = mc_strdup(ms[i].name);
    d->kind = ms[i].kind;
    d->tracks = ms[i].tracks;
    if (!d->name) st = MC_ERR_NOMEM;
  }
  members_free(ms, n);
  if (st != MC_OK) {
    mc_sync_waiting_free(w);
    return st;
  }
  if (w->count > 1) qsort(w->devices, (size_t)w->count, sizeof *w->devices, device_cmp);
  *out = w;
  return MC_OK;
}

int mc_waiting_equal(const mc_sync_waiting *a, const mc_sync_waiting *b) {
  static const mc_sync_waiting none = {0, NULL, 0};
  if (!a) a = &none;
  if (!b) b = &none;
  if (a->tracks != b->tracks || a->count != b->count) return 0;
  for (int i = 0; i < a->count; i++) {
    const mc_sync_waiting_device *x = &a->devices[i], *y = &b->devices[i];
    if (strcmp(x->uid, y->uid) || x->tracks != y->tracks || x->kind != y->kind ||
        strcmp(x->name ? x->name : "", y->name ? y->name : ""))
      return 0;
  }
  return 1;
}

mc_sync_waiting *mc_waiting_copy(const mc_sync_waiting *w) {
  mc_sync_waiting *c = calloc(1, sizeof *c);
  if (!c || !w || !w->count) {
    if (c && w) c->tracks = w->tracks;
    return c;
  }
  c->devices = calloc((size_t)w->count, sizeof *c->devices);
  if (!c->devices) {
    free(c);
    return NULL;
  }
  c->tracks = w->tracks;
  for (; c->count < w->count; c->count++) {
    mc_sync_waiting_device *d = &c->devices[c->count];
    *d = w->devices[c->count];
    d->name = mc_strdup(d->name ? d->name : "");
    if (!d->name) {
      mc_sync_waiting_free(c);
      return NULL;
    }
  }
  return c;
}

/* --- Public API (the rest is in sync.c) --- */

void mc_sync_waiting_free(mc_sync_waiting *waiting) {
  if (!waiting) return;
  for (int i = 0; i < waiting->count; i++) free(waiting->devices[i].name);
  free(waiting->devices);
  free(waiting);
}

size_t mc_sync_waiting_text(const mc_sync_waiting *w, char *buf, size_t size) {
  if (!w || w->tracks <= 0 || w->count <= 0) {
    if (buf && size) buf[0] = '\0';
    return 0;
  }
  const char *name = w->devices[0].name && *w->devices[0].name ? w->devices[0].name
                                                                : mc_str(MC_STR_DEVICE_UNNAMED);
  if (w->count == 1)
    return mc_format(buf, size, mc_str_plural(MC_STR_SYNC_WAITING, w->tracks), MC_D(w->tracks),
                     MC_S(name));
  char others[128];
  mc_format(others, sizeof others, mc_str_plural(MC_STR_SYNC_WAITING_OTHER_DEVICES, w->count - 1),
            MC_D(w->count - 1));
  return mc_format(buf, size, mc_str_plural(MC_STR_SYNC_WAITING_OTHERS, w->tracks), MC_D(w->tracks),
                   MC_S(name), MC_S(others));
}
