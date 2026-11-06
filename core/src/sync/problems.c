/* What the checks left for the user. See problems.h. */
#include "sync/problems.h"

#include <stdlib.h>
#include <string.h>

#include "kithcore/i18n.h"
#include "library/library_internal.h"
#include "sync/archive.h"
#include "sync/plan.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"
#include "sync/waiting.h"
#include "util/platform.h"

#define MAX_TEXT_PART 256

static int uid_cmp(const void *key, const void *elem) {
  return strcmp(key, ((const mc_sync_lost_track *)elem)->uid);
}

int mc_problems_is_lost(const mc_sync_problems *p, const char *track_uid) {
  return p && p->lost && track_uid &&
         bsearch(track_uid, p->tracks, (size_t)p->lost, sizeof *p->tracks, uid_cmp) != NULL;
}

/* Whether a file of the track is here that could still be worth something: on disk, and not
 * found corrupt. Any error says yes. */
static int holds_something(mc_library *lib, sqlite3_stmt *stmt, const char *track) {
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, track, -1, SQLITE_STATIC);
  int yes = 0, rc = SQLITE_DONE;
  while (!yes && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *path = (const char *)sqlite3_column_text(stmt, 0);
    if (sqlite3_column_int(stmt, 1) == MC_CHECK_CORRUPT) continue;
    char *abs = path ? mc_library_abspath(lib, path) : NULL;
    yes = !abs || mc_file_exists(abs);
    free(abs);
  }
  if (!yes && rc != SQLITE_DONE) yes = 1;
  sqlite3_reset(stmt);
  return yes;
}

/* Whether member said lately that it has no file of the track at all. */
static int member_lacks(sqlite3_stmt *stmt, const char *member, const char *track, int64_t since) {
  sqlite3_reset(stmt);
  sqlite3_bind_text(stmt, 1, member, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, track, -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 3, since);
  int yes = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_reset(stmt);
  return yes;
}

/* The other current members' uids. */
static mc_status member_uids(mc_sync_db *sdb, char (**out)[MC_UID_STRLEN], int *count) {
  *out = NULL;
  *count = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "SELECT uid FROM devices WHERE removed = 0 AND uid <> ?1", -1, &stmt,
                         NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(sdb), -1, SQLITE_STATIC);
  mc_status st = MC_OK;
  int rc;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    if (!uid || strlen(uid) != MC_UID_STRLEN - 1) {
      st = MC_ERR_DB; /* a member that can't be asked: nothing is lost */
      break;
    }
    char(*more)[MC_UID_STRLEN] = realloc(*out, (size_t)(*count + 1) * sizeof *more);
    if (!more) {
      st = MC_ERR_NOMEM;
      break;
    }
    *out = more;
    memcpy((*out)[(*count)++], uid, MC_UID_STRLEN);
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

static mc_status add_lost(mc_sync_problems *p, sqlite3_stmt *names, const char *track, int *cap) {
  if (p->lost == *cap) {
    int grown = *cap ? *cap * 2 : 16;
    mc_sync_lost_track *more = realloc(p->tracks, (size_t)grown * sizeof *more);
    if (!more) return MC_ERR_NOMEM;
    p->tracks = more;
    *cap = grown;
  }
  mc_sync_lost_track *t = &p->tracks[p->lost];
  memset(t, 0, sizeof *t);
  memcpy(t->uid, track, MC_UID_STRLEN);
  sqlite3_reset(names);
  sqlite3_bind_text(names, 1, track, -1, SQLITE_STATIC);
  if (sqlite3_step(names) == SQLITE_ROW) {
    const char *title = (const char *)sqlite3_column_text(names, 0);
    const char *artist = (const char *)sqlite3_column_text(names, 1);
    t->title = mc_strdup(title ? title : "");
    if (artist) t->artist = mc_strdup(artist);
  }
  sqlite3_reset(names);
  p->lost++;
  return t->title ? MC_OK : MC_ERR_NOMEM;
}

/* Fills in p->tracks. Nothing is added on any doubt, an error included. */
static mc_status find_lost(mc_sync_db *sdb, mc_library *lib, mc_sync_problems *p) {
  if (!mc_waiting_fetching(sdb, lib)) return MC_OK;
  if (mc_archive_is(mc_sync_db_handle(sdb), mc_sync_db_device_uid(sdb)) != 1) return MC_OK;
  mc_plan *plan = NULL;
  mc_status st = mc_plan_compute(sdb, lib, mc_sync_db_device_uid(sdb), mc_sync_free_space(lib), &plan);
  if (st != MC_OK || plan->status != MC_PLAN_OK) {
    mc_plan_free(plan);
    return st;
  }
  char(*members)[MC_UID_STRLEN] = NULL;
  int n = 0;
  st = member_uids(sdb, &members, &n);
  sqlite3_stmt *held = NULL, *lacks = NULL, *names = NULL, *satisfied = NULL;
  if (st == MC_OK &&
      (sqlite3_prepare_v2(mc_library_db(lib),
                          "SELECT f.path, x.state FROM files f"
                          " LEFT JOIN file_failed x ON x.file_uid = f.uid WHERE f.track_uid = ?1",
                          -1, &held, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(mc_library_db(lib),
                          "SELECT t.title, coalesce(t.artist_credit, ar.name)"
                          " FROM tracks t LEFT JOIN artists ar ON ar.uid = t.artist_uid"
                          " WHERE t.uid = ?1",
                          -1, &names, NULL) != SQLITE_OK ||
       sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                          "SELECT 1 FROM peer_lacks WHERE device_uid = ?1 AND track_uid = ?2"
                          " AND has_copy = 0 AND said_ms >= ?3",
                          -1, &lacks, NULL) != SQLITE_OK))
    st = MC_ERR_DB;
  int64_t since = (int64_t)mc_wall_ms() - (int64_t)MC_SYNC_LOST_ASKED_DAYS * 24 * 60 * 60 * 1000;
  int cap = 0;
  for (int t = 0; st == MC_OK && t < plan->count; t++) {
    const char *uid = plan->tracks[t].track_uid;
    if (mc_transfer_track_satisfied(lib, &satisfied, &plan->tracks[t])) continue;
    if (holds_something(lib, held, uid)) continue;
    int all = 1;
    for (int i = 0; all && i < n; i++) all = member_lacks(lacks, members[i], uid, since);
    if (all) st = add_lost(p, names, uid, &cap);
  }
  sqlite3_finalize(held);
  sqlite3_finalize(lacks);
  sqlite3_finalize(names);
  sqlite3_finalize(satisfied);
  free(members);
  mc_plan_free(plan);
  return st;
}

mc_status mc_problems_compute(mc_sync_db *sdb, mc_library *lib, mc_sync_problems **out) {
  if (out) *out = NULL;
  if (!sdb || !lib || !out) return MC_ERR_INVALID_ARG;
  mc_sync_problems *p = calloc(1, sizeof *p);
  if (!p) return MC_ERR_NOMEM;
  mc_status st = find_lost(sdb, lib, p);
  sqlite3_stmt *stmt;
  if (st == MC_OK &&
      sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT f.track_uid FROM file_failed x JOIN files f ON f.uid = x.file_uid",
                         -1, &stmt, NULL) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW)
      p->failed += !mc_problems_is_lost(p, (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK &&
      sqlite3_prepare_v2(mc_sync_db_handle(sdb), "SELECT unknown_files FROM local", -1, &stmt,
                         NULL) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) p->unknown = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  if (st != MC_OK) {
    mc_sync_problems_free(p);
    return st;
  }
  *out = p;
  return MC_OK;
}

int mc_problems_equal(const mc_sync_problems *a, const mc_sync_problems *b) {
  static const mc_sync_problems none = {0, 0, 0, NULL};
  if (!a) a = &none;
  if (!b) b = &none;
  if (a->failed != b->failed || a->unknown != b->unknown || a->lost != b->lost) return 0;
  for (int i = 0; i < a->lost; i++)
    if (strcmp(a->tracks[i].uid, b->tracks[i].uid)) return 0;
  return 1;
}

mc_sync_problems *mc_problems_copy(const mc_sync_problems *p) {
  mc_sync_problems *c = calloc(1, sizeof *c);
  if (!c || !p) return c;
  c->failed = p->failed;
  c->unknown = p->unknown;
  if (!p->lost) return c;
  c->tracks = calloc((size_t)p->lost, sizeof *c->tracks);
  if (!c->tracks) {
    free(c);
    return NULL;
  }
  for (; c->lost < p->lost; c->lost++) {
    mc_sync_lost_track *t = &c->tracks[c->lost];
    const mc_sync_lost_track *from = &p->tracks[c->lost];
    memcpy(t->uid, from->uid, MC_UID_STRLEN);
    t->title = mc_strdup(from->title ? from->title : "");
    t->artist = from->artist ? mc_strdup(from->artist) : NULL;
    if (!t->title) {
      c->lost++;
      mc_sync_problems_free(c);
      return NULL;
    }
  }
  return c;
}

/* --- Public API (the rest is in sync.c and transfer.c) --- */

void mc_sync_problems_free(mc_sync_problems *problems) {
  if (!problems) return;
  for (int i = 0; i < problems->lost; i++) {
    free(problems->tracks[i].title);
    free(problems->tracks[i].artist);
  }
  free(problems->tracks);
  free(problems);
}

size_t mc_sync_problems_text(const mc_sync_problems *p, char *buf, size_t size) {
  char parts[3][MAX_TEXT_PART];
  int n = 0;
  if (p && p->lost > 0)
    mc_format(parts[n++], MAX_TEXT_PART, mc_str_plural(MC_STR_SYNC_PROBLEMS_LOST, p->lost),
              MC_D(p->lost));
  if (p && p->failed > 0)
    mc_format(parts[n++], MAX_TEXT_PART, mc_str_plural(MC_STR_SYNC_PROBLEMS_FAILED, p->failed),
              MC_D(p->failed));
  if (p && p->unknown > 0)
    mc_format(parts[n++], MAX_TEXT_PART, mc_str_plural(MC_STR_SYNC_PROBLEMS_UNKNOWN, p->unknown),
              MC_D(p->unknown));
  size_t len = 0;
  if (buf && size) buf[0] = '\0';
  for (int i = 0; i < n; i++) {
    size_t part = strlen(parts[i]) + (i ? 1 : 0);
    if (buf && len + part < size) {
      if (i) buf[len] = ' ';
      memcpy(buf + len + (i ? 1 : 0), parts[i], strlen(parts[i]) + 1);
    }
    len += part;
  }
  return len;
}
