/* Tracks waiting for a device. See waiting.h and "Waiting for a device" in
 * docs/sync-protocol.md.
 *
 * This device's plan gives the wanted tracks it lacks; each other current member's plan
 * (one read of every track per member) and the library changelog's origins give who holds
 * them, less what a member said it hasn't (peer_lacks). Every plan is sorted by track uid, so
 * finding a track in one is a binary search. The same pass gives the outlook: what this
 * device lacks that a member holds, and what the members' own reports say they lack. */
#include "sync/waiting.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/i18n.h"
#include "library/library_internal.h"
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
  mc_plan *plan;            /* NULL: it holds nothing by its profile (none, no file phase
                             * yet, over budget) */
  int64_t missing;          /* its sync_status says it lacks this many; -1 with no report */
  int converts;             /* it may convert for others, or doesn't say it can't */
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
                         " c.free_bytes, s.missing, s.device_uid IS NOT NULL, c.can_transcode"
                         " FROM devices d LEFT JOIN profiles p ON p.device_uid = d.uid"
                         " LEFT JOIN capabilities c ON c.device_uid = d.uid"
                         " LEFT JOIN sync_status s ON s.device_uid = d.uid"
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
    m->missing = sqlite3_column_int(stmt, 6) ? sqlite3_column_int64(stmt, 5) : -1;
    m->converts = sqlite3_column_type(stmt, 7) == SQLITE_NULL ||
                  (sqlite3_column_int(stmt, 7) &&
                   (free_bytes[n] < 0 || free_bytes[n] >= MC_SYNC_TRANSCODE_MIN_FREE));
    n++;
  }
  sqlite3_finalize(stmt);
  for (int i = 0; st == MC_OK && i < n; i++) {
    /* No report of its files: it has had no file phase, so it holds nothing for its profile
     * yet (just paired, profile not chosen). */
    if (ms[i].missing < 0) continue;
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
 * node, a member since removed). origin (may be NULL): that node, "" if none. */
static int importer(sqlite3_stmt *rev, sqlite3_stmt *first, const char *track, const member *ms,
                    int n, char *origin_out) {
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
  if (origin_out) memcpy(origin_out, origin, MC_UID_STRLEN);
  if (!origin[0]) return -1;
  for (int i = 0; i < n; i++)
    if (ms[i].node[0] && !strcmp(ms[i].node, origin)) return i;
  return -1;
}

/* Whether m said in its last session with this device that it hasn't the track. */
static int lacks(sqlite3_stmt *stmt, const member *m, const char *track) {
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, m->uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, track, -1, SQLITE_STATIC);
  int yes = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_reset(stmt);
  return yes;
}

/* Whether m's profile has it hold what t needs: 2 for the very file (the original, or Opus at
 * t's very bitrate), 1 for the original of an Opus copy it would have to make, 0 for nothing. */
static int holds(const member *m, const mc_plan_track *t) {
  const mc_plan_track *theirs = mc_plan_find(m->plan, t->track_uid);
  if (!theirs) return 0;
  if (theirs->format.format == MC_FORMAT_ORIGINAL)
    return t->format.format == MC_FORMAT_OPUS ? 1 : 2;
  return t->format.format == MC_FORMAT_OPUS &&
                 theirs->format.bitrate_kbps == t->format.bitrate_kbps
             ? 2
             : 0;
}

/* The removed device whose library node is origin, into out; 0 if none. */
static int removed_device(sqlite3_stmt *stmt, const char *origin, char out[MC_UID_STRLEN]) {
  if (!stmt || !origin[0]) return 0;
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, origin, -1, SQLITE_STATIC);
  int found = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    if (is_uid(v)) {
      memcpy(out, v, MC_UID_STRLEN);
      found = 1;
    }
  }
  sqlite3_reset(stmt);
  return found;
}

/* A track nobody holds, put down to why, in x. */
static void classify_unheld(mc_waiting_extra *x, sqlite3_stmt *lacking, sqlite3_stmt *removed,
                            const member *ms, int n, int imp, const char *origin,
                            const char *track) {
  int by = imp >= 0 && lacks(lacking, &ms[imp], track) ? imp : -1;
  for (int i = 0; by < 0 && i < n; i++)
    if (lacks(lacking, &ms[i], track)) by = i;
  mc_track_group *g = NULL;
  char left[MC_UID_STRLEN];
  if (by >= 0) {
    g = mc_track_groups_get(&x->lacks, ms[by].uid);
  } else if (imp < 0 && removed_device(removed, origin, left)) {
    g = mc_track_groups_get(&x->left, left);
  } else {
    /* Only once every member has said what it holds. */
    for (int i = 0; i < n; i++)
      if (ms[i].missing < 0) return;
    mc_uid_list_add(&x->unheld, track);
    return;
  }
  if (g) mc_uid_list_add(&g->tracks, track);
}

int mc_waiting_library_is_groups(mc_sync_db *sdb, mc_library *lib) {
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
  return group_lib[0] && !strcmp(group_lib, mine);
}

int mc_waiting_fetching(mc_sync_db *sdb, mc_library *lib) {
  return mc_waiting_library_is_groups(sdb, lib) && mc_profile_chosen(sdb) == 1;
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
                             void *user, mc_sync_waiting **out, mc_sync_outlook *outlook,
                             mc_waiting_extra *extra) {
  if (out) *out = NULL;
  if (outlook) memset(outlook, 0, sizeof *outlook);
  if (extra) memset(extra, 0, sizeof *extra);
  if (!sdb || !lib || !reachable || !out) return MC_ERR_INVALID_ARG;
  mc_sync_waiting *w = calloc(1, sizeof *w);
  if (!w) return MC_ERR_NOMEM;
  if (!mc_waiting_library_is_groups(sdb, lib)) {
    *out = w;
    return MC_OK;
  }
  int chosen = mc_profile_chosen(sdb);
  if (outlook) outlook->chosen = chosen == 1;

  member *ms = NULL;
  int n = 0;
  mc_status st = load_members(sdb, lib, &ms, &n);
  for (int i = 0; st == MC_OK && i < n; i++) ms[i].reachable = reachable(user, ms[i].uid) != 0;
  /* What the members say they lack, at their plans' average size per track. */
  for (int i = 0; st == MC_OK && outlook && i < n; i++) {
    if (!ms[i].plan || ms[i].missing <= 0) continue;
    int64_t missing = ms[i].missing < ms[i].plan->wanted ? ms[i].missing : ms[i].plan->wanted;
    if (missing <= 0) continue;
    outlook->send_tracks += (int)missing;
    outlook->send_bytes += ms[i].plan->total_bytes / ms[i].plan->wanted * missing;
    outlook->send_devices++;
  }

  mc_plan *plan = NULL;
  if (st == MC_OK && chosen == 1) {
    st = mc_plan_compute(sdb, lib, mc_sync_db_device_uid(sdb), mc_sync_free_space(lib), &plan);
    if (st == MC_OK && plan->status != MC_PLAN_OK) {
      mc_plan_free(plan);
      plan = NULL;
    }
  }

  sqlite3 *ldb = mc_library_db(lib);
  sqlite3_stmt *rev = NULL, *first = NULL, *satisfied = NULL, *lacking = NULL, *removed = NULL;
  if (st == MC_OK && plan && extra)
    sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                       "SELECT uid FROM devices WHERE removed = 1 AND node_uid = ?1 LIMIT 1", -1,
                       &removed, NULL);
  if (st == MC_OK && plan &&
      (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                          "SELECT 1 FROM peer_lacks WHERE device_uid = ?1 AND track_uid = ?2", -1,
                          &lacking, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(ldb,
                          "SELECT origin_uid FROM changelog WHERE entity = 'tracks'"
                          " AND row_uid = ?1 AND field = 'file_rev' ORDER BY hlc DESC LIMIT 1",
                          -1, &rev, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(ldb,
                          "SELECT origin_uid FROM changelog WHERE entity = 'tracks'"
                          " AND row_uid = ?1 ORDER BY hlc LIMIT 1",
                          -1, &first, NULL) != SQLITE_OK))
    st = MC_ERR_DB;

  int uids_cap = 0;
  char *held = n ? malloc((size_t)n) : NULL;
  if (st == MC_OK && n && !held) st = MC_ERR_NOMEM;
  for (int t = 0; st == MC_OK && plan && t < plan->count; t++) {
    const mc_plan_track *track = &plan->tracks[t];
    if (!n && !extra) break;
    if (mc_transfer_track_satisfied(lib, &satisfied, track)) continue;
    char origin[MC_UID_STRLEN];
    int imp = importer(rev, first, track->track_uid, ms, n, origin);
    int opus = track->format.format == MC_FORMAT_OPUS;
    int any = 0, sources = 0, reached = 0;
    for (int i = 0; i < n; i++) {
      int how = ms[i].plan ? holds(&ms[i], track) : 0;
      if (i == imp && how < 2) how = opus ? 1 : 2;
      held[i] = 0;
      if (!how || lacks(lacking, &ms[i], track->track_uid)) continue;
      any = 1;
      /* An original is a source of an Opus copy only where it may be converted. */
      held[i] = how == 2 || ms[i].converts;
      sources |= held[i];
      reached |= held[i] && ms[i].reachable;
    }
    if (!any) {
      if (extra)
        classify_unheld(extra, lacking, removed, ms, n, imp, origin, track->track_uid);
      continue;
    }
    if (!sources) {
      /* Held, but nobody may convert it. */
      for (int i = 0; extra && i < n; i++) {
        if (!ms[i].plan && i != imp) continue;
        int how = ms[i].plan ? holds(&ms[i], track) : 0;
        if ((how || i == imp) && !lacks(lacking, &ms[i], track->track_uid)) {
          mc_track_group *g = mc_track_groups_get(&extra->no_transcoder, ms[i].uid);
          if (g) mc_uid_list_add(&g->tracks, track->track_uid);
        }
      }
      continue;
    }
    if (outlook) {
      outlook->receive_tracks++;
      outlook->receive_bytes += track->est_bytes;
    }
    if (reached) continue;
    if (w->tracks == uids_cap) {
      int cap = uids_cap ? uids_cap * 2 : 64;
      char(*more)[MC_UID_STRLEN] = realloc(w->track_uids, (size_t)cap * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      w->track_uids = more;
      uids_cap = cap;
    }
    memcpy(w->track_uids[w->tracks++], track->track_uid, MC_UID_STRLEN);
    for (int i = 0; i < n; i++) ms[i].tracks += held[i];
  }
  free(held);
  sqlite3_finalize(removed);
  sqlite3_finalize(rev);
  sqlite3_finalize(first);
  sqlite3_finalize(satisfied);
  sqlite3_finalize(lacking);
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
    if (outlook) memset(outlook, 0, sizeof *outlook);
    if (extra) mc_waiting_extra_free(extra);
    return st;
  }
  if (w->count > 1) qsort(w->devices, (size_t)w->count, sizeof *w->devices, device_cmp);
  *out = w;
  return MC_OK;
}

int mc_waiting_equal(const mc_sync_waiting *a, const mc_sync_waiting *b) {
  static const mc_sync_waiting none = {0, NULL, NULL, 0};
  if (!a) a = &none;
  if (!b) b = &none;
  if (a->tracks != b->tracks || a->count != b->count) return 0;
  if (a->tracks && memcmp(a->track_uids, b->track_uids, (size_t)a->tracks * MC_UID_STRLEN))
    return 0;
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
  c->track_uids = malloc((size_t)w->tracks * MC_UID_STRLEN);
  if (!c->devices || !c->track_uids) {
    free(c->devices);
    free(c->track_uids);
    free(c);
    return NULL;
  }
  c->tracks = w->tracks;
  memcpy(c->track_uids, w->track_uids, (size_t)w->tracks * MC_UID_STRLEN);
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
  free(waiting->track_uids);
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

size_t mc_sync_outlook_text(const mc_sync_outlook *o, char *buf, size_t size) {
  char send[64] = "", receive[64] = "";
  if (o && o->send_tracks > 0) {
    char bytes[32];
    mc_format_size(bytes, sizeof bytes, o->send_bytes);
    mc_format(send, sizeof send, mc_str(MC_STR_SYNC_OUTLOOK_SEND), MC_S(bytes));
  }
  if (o && o->chosen && o->receive_tracks > 0)
    mc_format(receive, sizeof receive, mc_str_plural(MC_STR_SYNC_OUTLOOK_RECEIVE, o->receive_tracks),
              MC_D(o->receive_tracks));
  if (send[0] && receive[0])
    return mc_format(buf, size, mc_str(MC_STR_SYNC_OUTLOOK_PARTS), MC_S(send), MC_S(receive));
  if (send[0] || receive[0])
    return mc_format(buf, size, "{0}", MC_S(send[0] ? send : receive));
  return mc_format_args(buf, size, mc_str(MC_STR_SYNC_OUTLOOK_UP_TO_DATE), NULL, 0);
}
