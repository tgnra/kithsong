/* Sync issues. See issues.h and "Issues" in docs/sync-protocol.md. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* localtime_r */
#endif
#include "sync/issues.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <yyjson.h>

#include "kithcore/i18n.h"
#include "library/library_internal.h"
#include "sync/archive.h"
#include "sync/problems.h"
#include "sync/takeover.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"
#include "util/platform.h"

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

/* --- Lists --- */

int mc_uid_list_add(mc_uid_list *l, const char *uid) {
  if (!is_uid(uid)) return 1;
  if (l->n == l->cap) {
    int cap = l->cap ? l->cap * 2 : 16;
    char(*more)[MC_UID_STRLEN] = realloc(l->uids, (size_t)cap * sizeof *more);
    if (!more) return 0;
    l->uids = more;
    l->cap = cap;
  }
  memcpy(l->uids[l->n++], uid, MC_UID_STRLEN);
  return 1;
}

void mc_uid_list_free(mc_uid_list *l) {
  free(l->uids);
  memset(l, 0, sizeof *l);
}

int mc_uid_list_copy(mc_uid_list *out, const mc_uid_list *from) {
  memset(out, 0, sizeof *out);
  if (!from->n) return 1;
  out->uids = malloc((size_t)from->n * sizeof *out->uids);
  if (!out->uids) return 0;
  memcpy(out->uids, from->uids, (size_t)from->n * sizeof *out->uids);
  out->n = out->cap = from->n;
  return 1;
}

mc_track_group *mc_track_groups_get(mc_track_groups *g, const char *uid) {
  for (int i = 0; i < g->n; i++)
    if (!strcmp(g->items[i].uid, uid)) return &g->items[i];
  mc_track_group *more = realloc(g->items, (size_t)(g->n + 1) * sizeof *more);
  if (!more) return NULL;
  g->items = more;
  mc_track_group *it = &g->items[g->n++];
  memset(it, 0, sizeof *it);
  snprintf(it->uid, sizeof it->uid, "%s", uid);
  return it;
}

void mc_track_groups_free(mc_track_groups *g) {
  for (int i = 0; i < g->n; i++) mc_uid_list_free(&g->items[i].tracks);
  free(g->items);
  memset(g, 0, sizeof *g);
}

static int groups_copy(mc_track_groups *out, const mc_track_groups *from) {
  memset(out, 0, sizeof *out);
  for (int i = 0; i < from->n; i++) {
    mc_track_group *g = mc_track_groups_get(out, from->items[i].uid);
    if (!g || !mc_uid_list_copy(&g->tracks, &from->items[i].tracks)) {
      mc_track_groups_free(out);
      return 0;
    }
  }
  return 1;
}

void mc_findings_free(mc_findings *f) {
  if (!f) return;
  for (int i = 0; i < MC_REFUSED_COUNT; i++) mc_uid_list_free(&f->refused[i]);
  mc_track_groups_free(&f->away);
  free(f);
}

static mc_findings *findings_copy(const mc_findings *f) {
  mc_findings *c = malloc(sizeof *c);
  if (!c) return NULL;
  *c = *f;
  int ok = groups_copy(&c->away, &f->away);
  for (int i = 0; i < MC_REFUSED_COUNT; i++) {
    if (!ok) {
      memset(&c->refused[i], 0, sizeof c->refused[i]);
      continue;
    }
    ok = mc_uid_list_copy(&c->refused[i], &f->refused[i]);
  }
  if (!ok) {
    mc_findings_free(c);
    return NULL;
  }
  return c;
}

void mc_waiting_extra_free(mc_waiting_extra *x) {
  if (!x) return;
  mc_track_groups_free(&x->no_transcoder);
  mc_track_groups_free(&x->lacks);
  mc_track_groups_free(&x->left);
  mc_uid_list_free(&x->unheld);
}

static int extra_copy(mc_waiting_extra *out, const mc_waiting_extra *x) {
  memset(out, 0, sizeof *out);
  if (!x) return 1;
  if (groups_copy(&out->no_transcoder, &x->no_transcoder) && groups_copy(&out->lacks, &x->lacks) &&
      groups_copy(&out->left, &x->left) && mc_uid_list_copy(&out->unheld, &x->unheld))
    return 1;
  mc_waiting_extra_free(out);
  return 0;
}

/* --- Issues as values --- */

static void issue_clear(mc_sync_issue *is) {
  free(is->device_name);
  for (int i = 0; i < is->nexamples; i++) free(is->examples[i]);
  free(is->detail);
  free(is->error);
}

void mc_issues_free(mc_sync_issue *items, int n) {
  for (int i = 0; i < n; i++) issue_clear(&items[i]);
  free(items);
}

static int str_eq(const char *a, const char *b) {
  return !strcmp(a ? a : "", b ? b : "");
}

static int issue_equal(const mc_sync_issue *a, const mc_sync_issue *b) {
  if (a->kind != b->kind || strcmp(a->device_uid, b->device_uid) ||
      !str_eq(a->device_name, b->device_name) || a->device_kind != b->device_kind ||
      a->notice != b->notice || a->tracks != b->tracks || a->albums != b->albums ||
      a->nexamples != b->nexamples || a->count != b->count || a->refusal != b->refusal ||
      a->hold != b->hold || a->bytes != b->bytes || a->bytes_free != b->bytes_free ||
      a->until_ms != b->until_ms || !str_eq(a->detail, b->detail) || !str_eq(a->error, b->error))
    return 0;
  for (int i = 0; i < a->nexamples; i++)
    if (!str_eq(a->examples[i], b->examples[i])) return 0;
  return 1;
}

int mc_issues_equal(const mc_sync_issue *a, int na, const mc_sync_issue *b, int nb) {
  if (na != nb) return 0;
  for (int i = 0; i < na; i++)
    if (!issue_equal(&a[i], &b[i])) return 0;
  return 1;
}

static char *dup_opt(const char *s, int *ok) {
  if (!s) return NULL;
  char *d = mc_strdup(s);
  if (!d) *ok = 0;
  return d;
}

static int issue_copy(mc_sync_issue *to, const mc_sync_issue *from) {
  *to = *from;
  int ok = 1;
  to->device_name = dup_opt(from->device_name, &ok);
  to->detail = dup_opt(from->detail, &ok);
  to->error = dup_opt(from->error, &ok);
  for (int i = 0; i < from->nexamples; i++) to->examples[i] = dup_opt(from->examples[i], &ok);
  if (!ok) {
    issue_clear(to);
    memset(to, 0, sizeof *to);
  }
  return ok;
}

int mc_issues_copy(mc_sync_issue **out, const mc_sync_issue *from, int n) {
  *out = NULL;
  if (n <= 0) return 1;
  mc_sync_issue *c = calloc((size_t)n, sizeof *c);
  if (!c) return 0;
  for (int i = 0; i < n; i++) {
    if (!issue_copy(&c[i], &from[i])) {
      mc_issues_free(c, i);
      return 0;
    }
  }
  *out = c;
  return 1;
}

/* --- Holds --- */

static const char *const k_hold_names[MC_HOLD_COUNT] = {
  [MC_HOLD_BIN] = "bin",
  [MC_HOLD_CANCELLED] = "cancelled",
  [MC_HOLD_AUTO_OFF] = "auto_off",
  [MC_HOLD_NOT_CHOSEN] = "not_chosen",
};

const char *mc_hold_name(mc_sync_hold hold) {
  return (unsigned)hold < MC_HOLD_COUNT ? k_hold_names[hold] : NULL;
}

mc_sync_hold mc_hold_parse(const char *name) {
  for (int i = 0; name && i < MC_HOLD_COUNT; i++)
    if (k_hold_names[i] && !strcmp(k_hold_names[i], name)) return (mc_sync_hold)i;
  return MC_HOLD_NONE;
}

void mc_state_parse_hold(const char *json, mc_sync_hold *hold, int64_t *until_ms) {
  *hold = MC_HOLD_NONE;
  *until_ms = 0;
  yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
  yyjson_val *root = yyjson_doc_get_root(doc);
  if (yyjson_is_obj(root)) {
    *hold = mc_hold_parse(yyjson_get_str(yyjson_obj_get(root, "hold")));
    yyjson_val *until = yyjson_obj_get(root, "until");
    if (*hold != MC_HOLD_NONE && yyjson_is_int(until)) *until_ms = yyjson_get_sint(until);
  }
  yyjson_doc_free(doc);
}

char *mc_self_state(mc_sync *sync, mc_sync_conn *c) {
  mc_sync_hold hold;
  int64_t until;
  mc_sync_self_hold(sync, c, &hold, &until);
  const char *name = mc_hold_name(hold);
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return NULL;
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  if (name) {
    yyjson_mut_obj_add_str(doc, root, "hold", name);
    yyjson_mut_obj_add_int(doc, root, "until", until);
  }
  mc_archive_state_add(sync, c, doc, root);
  char *json = yyjson_mut_obj_size(root) ? yyjson_mut_write(doc, 0, NULL) : NULL;
  yyjson_mut_doc_free(doc);
  char *out = json ? mc_strdup(json) : NULL;
  free(json);
  return out;
}

/* --- What sessions and sync itself hand over --- */

void mc_sync_store_findings(mc_sync *sync, mc_findings *f) {
  mc_mutex_lock(&sync->lock);
  int at = -1;
  for (int i = 0; i < sync->nfindings && at < 0; i++)
    if (!strcmp(sync->findings[i]->peer, f->peer)) at = i;
  if (at >= 0) {
    mc_findings_free(sync->findings[at]);
    sync->findings[at] = f;
  } else {
    mc_findings **more = realloc(sync->findings, (size_t)(sync->nfindings + 1) * sizeof *more);
    if (more) {
      sync->findings = more;
      sync->findings[sync->nfindings++] = f;
    } else {
      mc_findings_free(f);
    }
  }
  sync->waiting_dirty = 1;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_add_notice(mc_sync *sync, mc_sync_issue_kind kind, const char *uid, int64_t bytes) {
  if (!sync || !is_uid(uid)) return;
  mc_mutex_lock(&sync->lock);
  mc_sync_issue *is = NULL;
  for (int i = 0; i < sync->nnotices && !is; i++)
    if (sync->notices[i].kind == kind && !strcmp(sync->notices[i].device_uid, uid))
      is = &sync->notices[i];
  if (!is) {
    mc_sync_issue *more = realloc(sync->notices, (size_t)(sync->nnotices + 1) * sizeof *more);
    if (more) {
      sync->notices = more;
      is = &sync->notices[sync->nnotices++];
      memset(is, 0, sizeof *is);
      is->kind = kind;
      is->notice = 1;
      is->bytes_free = -1;
      memcpy(is->device_uid, uid, MC_UID_STRLEN);
    }
  }
  if (is) is->bytes = bytes;
  sync->waiting_dirty = 1;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_dismiss_notices(mc_sync *sync) {
  if (!sync) return;
  mc_mutex_lock(&sync->lock);
  mc_sync_issue *old = sync->notices;
  int n = sync->nnotices;
  sync->notices = NULL;
  sync->nnotices = 0;
  sync->waiting_dirty = 1;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  mc_issues_free(old, n);
}

/* --- Building the list ---
 * One function per source, each appending through add(). */

typedef struct builder {
  mc_sync *sync;
  mc_sync_conn *c;
  mc_sync_problems *p;
  int cap, failed;
  int64_t now, due;
  sqlite3_stmt *names, *titles;
} builder;

static void due_at(builder *b, int64_t ms) {
  if (ms > b->now && (!b->due || ms < b->due)) b->due = ms;
}

/* A new issue about uid, its name and kind as the group shows them; NULL if out of memory. */
static mc_sync_issue *add(builder *b, mc_sync_issue_kind kind, const char *uid) {
  mc_sync_problems *p = b->p;
  if (p->issue_count == b->cap) {
    int cap = b->cap ? b->cap * 2 : 8;
    mc_sync_issue *more = realloc(p->issues, (size_t)cap * sizeof *more);
    if (!more) {
      b->failed = 1;
      return NULL;
    }
    p->issues = more;
    b->cap = cap;
  }
  mc_sync_issue *is = &p->issues[p->issue_count++];
  memset(is, 0, sizeof *is);
  is->kind = kind;
  is->bytes_free = -1;
  is->notice = kind == MC_ISSUE_NOTICE_ARCHIVE || kind == MC_ISSUE_NOTICE_BUDGET;
  snprintf(is->device_uid, sizeof is->device_uid, "%s", uid ? uid : "");
  if (b->names) {
    sqlite3_reset(b->names);
    sqlite3_bind_text(b->names, 1, is->device_uid, -1, SQLITE_STATIC);
    if (sqlite3_step(b->names) == SQLITE_ROW) {
      const char *name = (const char *)sqlite3_column_text(b->names, 0);
      if (name) is->device_name = mc_strdup(name);
      is->device_kind = mc_sync_kind_parse((const char *)sqlite3_column_text(b->names, 1));
    }
    sqlite3_reset(b->names);
  }
  return is;
}

static void set_text(builder *b, char **field, const char *s) {
  if (!s || !*s) return;
  *field = mc_strdup(s);
  if (!*field) b->failed = 1;
}

typedef struct album_count {
  char uid[MC_UID_STRLEN];
  char *title;
  int n, first;
} album_count;

static int album_cmp(const void *x, const void *y) {
  const album_count *a = x, *b = y;
  if (a->n != b->n) return a->n > b->n ? -1 : 1;
  return a->first - b->first;
}

/* is->tracks and its examples from the tracks in l (less the lost ones, for any but LOST):
 * the albums most of them are on when they share albums, else the tracks' titles. 0 if none
 * are left. */
static int examples(builder *b, mc_sync_issue *is, const mc_uid_list *l) {
  album_count *albums = NULL;
  int nalbums = 0, tracks = 0, loose = 0;
  char *titles[MC_SYNC_ISSUE_EXAMPLES] = {0};
  int ntitles = 0;
  for (int i = 0; i < l->n; i++) {
    if (is->kind != MC_ISSUE_LOST && is->kind != MC_ISSUE_TAKEOVER &&
        mc_problems_is_lost(b->p, l->uids[i]))
      continue;
    tracks++;
    if (!b->titles) continue;
    sqlite3_reset(b->titles);
    sqlite3_bind_text(b->titles, 1, l->uids[i], -1, SQLITE_STATIC);
    if (sqlite3_step(b->titles) == SQLITE_ROW) {
      const char *title = (const char *)sqlite3_column_text(b->titles, 0);
      const char *album = (const char *)sqlite3_column_text(b->titles, 1);
      const char *album_title = (const char *)sqlite3_column_text(b->titles, 2);
      if (title && *title && ntitles < MC_SYNC_ISSUE_EXAMPLES) titles[ntitles++] = mc_strdup(title);
      if (!is_uid(album) || !album_title || !*album_title) {
        loose++;
      } else {
        int at = -1;
        for (int k = 0; k < nalbums && at < 0; k++)
          if (!strcmp(albums[k].uid, album)) at = k;
        if (at < 0) {
          album_count *more = realloc(albums, (size_t)(nalbums + 1) * sizeof *more);
          if (more) {
            albums = more;
            at = nalbums++;
            memcpy(albums[at].uid, album, MC_UID_STRLEN);
            albums[at].title = mc_strdup(album_title);
            albums[at].n = 0;
            albums[at].first = i;
          }
        }
        if (at >= 0) albums[at].n++;
      }
    }
    sqlite3_reset(b->titles);
  }
  is->tracks = tracks;
  if (!loose && nalbums && nalbums < tracks) {
    qsort(albums, (size_t)nalbums, sizeof *albums, album_cmp);
    is->albums = nalbums;
    for (int k = 0; k < nalbums && is->nexamples < MC_SYNC_ISSUE_EXAMPLES; k++)
      if (albums[k].title) is->examples[is->nexamples++] = mc_strdup(albums[k].title);
  } else {
    for (int k = 0; k < ntitles; k++) {
      is->examples[is->nexamples++] = titles[k];
      titles[k] = NULL;
    }
  }
  for (int k = 0; k < ntitles; k++) free(titles[k]);
  for (int k = 0; k < nalbums; k++) free(albums[k].title);
  free(albums);
  for (int k = 0; k < is->nexamples; k++)
    if (!is->examples[k]) b->failed = 1;
  return tracks > 0;
}

/* An issue about the tracks of l, dropped again if none of them is left to name. */
static mc_sync_issue *add_tracks(builder *b, mc_sync_issue_kind kind, const char *uid,
                                 const mc_uid_list *l) {
  if (!l->n) return NULL;
  mc_sync_issue *is = add(b, kind, uid);
  if (is && !examples(b, is, l)) {
    issue_clear(is);
    b->p->issue_count--;
    return NULL;
  }
  return is;
}

/* Whether uid is online, by discovery or a session that went through. */
static int online(builder *b, const char *uid) {
  return mc_sync_peer_online(b->sync, uid);
}

/* What the checks found here (problems.c). */
static void from_checks(builder *b) {
  const char *me = mc_sync_db_device_uid(b->c->sdb);
  mc_sync_issue *is;
  if (b->p->lost > 0) {
    mc_uid_list l = {0};
    for (int i = 0; i < b->p->lost; i++) mc_uid_list_add(&l, b->p->tracks[i].uid);
    add_tracks(b, MC_ISSUE_LOST, me, &l);
    mc_uid_list_free(&l);
  }
  if (b->p->failed > 0 && (is = add(b, MC_ISSUE_FAILED_FILES, me))) is->count = b->p->failed;
  if (b->p->unknown > 0 && (is = add(b, MC_ISSUE_UNKNOWN_FILES, me))) is->count = b->p->unknown;
}

/* What the last receive turn with each member found. */
static void from_findings(builder *b) {
  mc_findings **f = NULL;
  int n = 0;
  mc_mutex_lock(&b->sync->lock);
  if (b->sync->nfindings) f = calloc((size_t)b->sync->nfindings, sizeof *f);
  for (int i = 0; f && i < b->sync->nfindings; i++) {
    f[n] = findings_copy(b->sync->findings[i]);
    if (f[n]) n++;
  }
  mc_mutex_unlock(&b->sync->lock);
  const char *me = mc_sync_db_device_uid(b->c->sdb);
  int folder = 0;
  for (int i = 0; i < n; i++) {
    const mc_findings *x = f[i];
    if (!mc_sync_peer_member(b->sync, x->peer)) continue;
    mc_sync_issue *is;
    /* The folder is this device's: said once, whichever member it was found with. */
    if (x->unwritable && !folder && (is = add(b, MC_ISSUE_FOLDER_UNWRITABLE, me))) {
      folder = 1;
      set_text(b, &is->detail, mc_library_dir(b->c->lib));
      set_text(b, &is->error, x->folder_error);
    } else if (x->disk_full && !folder && (is = add(b, MC_ISSUE_DISK_FULL, me))) {
      folder = 1;
      is->bytes = x->need_bytes;
      is->bytes_free = x->free_bytes;
      set_text(b, &is->detail, mc_library_dir(b->c->lib));
    }
    if (x->write_failed && (is = add(b, MC_ISSUE_WRITE_FAILED, x->peer))) {
      is->count = x->write_failed;
      set_text(b, &is->error, x->write_error);
    }
    if (x->files_busy) add(b, MC_ISSUE_FILES_BUSY, x->peer);
    for (int r = 0; r < MC_REFUSED_COUNT; r++)
      if ((is = add_tracks(b, MC_ISSUE_REFUSED, x->peer, &x->refused[r]))) is->refusal = r;
    for (int k = 0; k < x->away.n; k++) {
      const mc_track_group *g = &x->away.items[k];
      if (!online(b, g->uid) && mc_sync_peer_member(b->sync, g->uid))
        add_tracks(b, MC_ISSUE_TRANSCODER_AWAY, g->uid, &g->tracks);
    }
  }
  for (int i = 0; i < n; i++) mc_findings_free(f[i]);
  free(f);
}

/* The wanted tracks that aren't waiting for anyone to come online (waiting.c). */
static void from_waiting(builder *b) {
  mc_waiting_extra x;
  mc_mutex_lock(&b->sync->lock);
  int ok = extra_copy(&x, b->sync->wextra);
  mc_mutex_unlock(&b->sync->lock);
  if (!ok) {
    b->failed = 1;
    return;
  }
  for (int i = 0; i < x.no_transcoder.n; i++)
    add_tracks(b, MC_ISSUE_NO_TRANSCODER, x.no_transcoder.items[i].uid,
               &x.no_transcoder.items[i].tracks);
  for (int i = 0; i < x.lacks.n; i++)
    add_tracks(b, MC_ISSUE_DEVICE_LACKS, x.lacks.items[i].uid, &x.lacks.items[i].tracks);
  for (int i = 0; i < x.left.n; i++)
    add_tracks(b, MC_ISSUE_DEVICE_LEFT, x.left.items[i].uid, &x.left.items[i].tracks);
  add_tracks(b, MC_ISSUE_NO_HOLDER, NULL, &x.unheld);
  mc_waiting_extra_free(&x);
}

/* What each member gave for refusing a session, and where something else answers for it. */
typedef struct peer_note {
  char uid[MC_UID_STRLEN];
  mc_sync_hold hold;
  int64_t until;
  int wrong;
  char addr[MC_SYNC_ADDR_MAX + 8];
} peer_note;

static int from_peers(builder *b, peer_note **out) {
  *out = NULL;
  int n = 0;
  mc_mutex_lock(&b->sync->lock);
  peer_note *notes = b->sync->npeers ? calloc((size_t)b->sync->npeers, sizeof *notes) : NULL;
  for (int i = 0; notes && i < b->sync->npeers; i++) {
    const mc_sync_peer *p = &b->sync->peers[i];
    if (!p->member) continue;
    peer_note *x = &notes[n];
    memcpy(x->uid, p->uid, MC_UID_STRLEN);
    if (p->refused_hold != MC_HOLD_NONE && (!p->refused_until_ms || p->refused_until_ms > b->now)) {
      x->hold = p->refused_hold;
      x->until = p->refused_until_ms;
    }
    if (p->wrong_count >= MC_SYNC_WRONG_SHOW && p->wrong_addr[0]) {
      x->wrong = p->wrong_count;
      memcpy(x->addr, p->wrong_addr, sizeof x->addr);
    }
    if (x->hold || x->wrong) n++;
  }
  mc_mutex_unlock(&b->sync->lock);
  for (int i = 0; i < n; i++) {
    mc_sync_issue *is;
    if (notes[i].hold && (is = add(b, MC_ISSUE_HELD, notes[i].uid))) {
      is->hold = notes[i].hold;
      is->until_ms = notes[i].until;
      due_at(b, notes[i].until);
    }
    if (notes[i].wrong && (is = add(b, MC_ISSUE_WRONG_DEVICE, notes[i].uid))) {
      is->count = notes[i].wrong;
      set_text(b, &is->detail, notes[i].addr);
    }
  }
  *out = notes;
  return n;
}

/* What the members publish of themselves: holds and want of space. */
static void from_capabilities(builder *b, const peer_note *notes, int nnotes) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(b->c->sdb),
                         "SELECT c.device_uid, c.can_transcode, c.free_bytes, c.state"
                         " FROM capabilities c JOIN devices d ON d.uid = c.device_uid"
                         " WHERE d.removed = 0",
                         -1, &stmt, NULL) != SQLITE_OK) {
    b->failed = 1;
    return;
  }
  const char *me = mc_sync_db_device_uid(b->c->sdb);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    char uid[MC_UID_STRLEN];
    const char *u = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(u)) continue;
    memcpy(uid, u, MC_UID_STRLEN);
    mc_sync_issue *is;
    if (sqlite3_column_int(stmt, 1) && sqlite3_column_type(stmt, 2) != SQLITE_NULL &&
        sqlite3_column_int64(stmt, 2) < MC_SYNC_TRANSCODE_MIN_FREE &&
        (is = add(b, MC_ISSUE_LOW_SPACE, uid)))
      is->bytes_free = sqlite3_column_int64(stmt, 2);
    if (!strcmp(uid, me)) continue;
    int refused = 0;
    for (int i = 0; i < nnotes && !refused; i++) refused = notes[i].hold && !strcmp(notes[i].uid, uid);
    mc_sync_hold hold;
    int64_t until;
    mc_state_parse_hold((const char *)sqlite3_column_text(stmt, 3), &hold, &until);
    if (refused || hold == MC_HOLD_NONE || (until && until <= b->now)) continue;
    if ((is = add(b, MC_ISSUE_HELD, uid))) {
      is->hold = hold;
      is->until_ms = until;
      due_at(b, until);
    }
  }
  sqlite3_finalize(stmt);
}

/* Things sync did by itself: the ones handed over, and another device that stays the archive
 * as the last one (a void demotion), seen as it happens. */
static void from_notices(builder *b) {
  char v[MC_UID_STRLEN] = "";
  if (mc_archive_void(mc_sync_db_handle(b->c->sdb), v) == MC_OK) {
    const char *me = mc_sync_db_device_uid(b->c->sdb);
    mc_mutex_lock(&b->sync->lock);
    int news = b->sync->void_seeded && v[0] && strcmp(v, b->sync->void_noticed) && strcmp(v, me);
    memcpy(b->sync->void_noticed, v, MC_UID_STRLEN);
    b->sync->void_seeded = 1;
    mc_mutex_unlock(&b->sync->lock);
    if (news) mc_sync_add_notice(b->sync, MC_ISSUE_NOTICE_ARCHIVE, v, 0);
  }
  mc_sync_issue *copy = NULL;
  mc_mutex_lock(&b->sync->lock);
  int n = b->sync->nnotices;
  if (!mc_issues_copy(&copy, b->sync->notices, n)) n = 0;
  mc_mutex_unlock(&b->sync->lock);
  for (int i = 0; i < n; i++) {
    mc_sync_issue *is = add(b, copy[i].kind, copy[i].device_uid);
    if (!is) continue;
    is->bytes = copy[i].bytes;
    if (copy[i].kind == MC_ISSUE_TAKEOVER) {
      /* Done: what couldn't be had as originals. */
      mc_uid_list l;
      is->notice = 1;
      if (mc_takeover_unrecovered(b->sync, &l)) {
        examples(b, is, &l);
        mc_uid_list_free(&l);
      }
    }
  }
  mc_issues_free(copy, n);
}

/* Lost archives and a takeover under way here (takeover.c), and what the other archives
 * publish of themselves: no room, and tracks gone that no other device has the original of. */
static void from_archives(builder *b) {
  mc_sync_issue *is;
  mc_takeover_view v;
  if (mc_takeover_view_get(b->sync, &v) &&
      (v.state == MC_TAKEOVER_FETCHING || v.state == MC_TAKEOVER_CHECKING ||
       v.state == MC_TAKEOVER_FAILED) &&
      (is = add(b, MC_ISSUE_TAKEOVER, v.uid))) {
    is->count = v.done;
    is->bytes = v.total;
    if (v.state == MC_TAKEOVER_FAILED) set_text(b, &is->error, mc_status_str(v.status));
  }
  mc_lost_archive la;
  if (mc_archive_find_lost(b->sync, b->c, &la)) {
    int64_t lost_at = la.last_seen_ms + MC_SYNC_ARCHIVE_LOST_DAYS * MC_DAY_MS;
    if (la.reinstall) {
      add(b, MC_ISSUE_ARCHIVE_REINSTALL, la.uid);
    } else if (la.lost) {
      mc_sync_archive_offer *o = NULL;
      mc_takeover_offer(b->sync, b->c, &o);
      if ((is = add(b, MC_ISSUE_ARCHIVE_LOST, la.uid))) {
        is->until_ms = la.last_seen_ms;
        is->count = o && o->can_take;
        if (o) {
          is->bytes = o->bytes_needed;
          is->bytes_free = o->bytes_free;
        }
      }
      mc_sync_archive_offer_free(o);
    } else if ((is = add(b, MC_ISSUE_ARCHIVE_AWAY, la.uid))) {
      is->until_ms = la.last_seen_ms;
    }
    if (la.last_seen_ms && !la.lost) due_at(b, lost_at);
  }
  const char *me = mc_sync_db_device_uid(b->c->sdb);
  sqlite3 *db = mc_sync_db_handle(b->c->sdb);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT c.device_uid, c.state FROM capabilities c JOIN devices d"
                         " ON d.uid = c.device_uid WHERE d.removed = 0 AND c.device_uid <> ?1",
                         -1, &stmt, NULL) != SQLITE_OK) {
    b->failed = 1;
    return;
  }
  sqlite3_bind_text(stmt, 1, me, -1, SQLITE_STATIC);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    char uid[MC_UID_STRLEN];
    const char *u = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(u) || mc_archive_is(db, u) != 1) continue;
    memcpy(uid, u, MC_UID_STRLEN);
    mc_archive_state as;
    mc_archive_state_parse((const char *)sqlite3_column_text(stmt, 1), &as);
    /* Becoming away is worked out again then. */
    int64_t seen = mc_archive_last_seen(db, uid);
    if (seen > 0 && !mc_sync_peer_online(b->sync, uid))
      due_at(b, seen + MC_SYNC_ARCHIVE_AWAY_DAYS * MC_DAY_MS);
    if (as.full_tracks > 0 && (is = add(b, MC_ISSUE_ARCHIVE_FULL, uid))) {
      is->tracks = as.full_tracks;
      is->bytes = as.full_bytes;
    }
    mc_uid_list only = {0};
    if (as.damaged.n && mc_archive_only_there(b->c, uid, &as.damaged, &only) == MC_OK &&
        (is = add_tracks(b, MC_ISSUE_ARCHIVE_DAMAGED, uid, &only)) && as.damaged_n > as.damaged.n)
      is->tracks += as.damaged_n - as.damaged.n;
    mc_uid_list_free(&only);
    mc_archive_state_free(&as);
  }
  sqlite3_finalize(stmt);
}

static int name_cmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
    if (x != y || !x) return x - y;
  }
}

typedef struct sort_item {
  const mc_sync_issue *is;
  int self, at;
} sort_item;

static int issue_cmp(const void *x, const void *y) {
  const sort_item *a = x, *b = y;
  if (a->is->notice != b->is->notice) return a->is->notice - b->is->notice;
  if (a->self != b->self) return b->self - a->self;
  int c = name_cmp(a->is->device_name ? a->is->device_name : "",
                   b->is->device_name ? b->is->device_name : "");
  if (c) return c;
  c = strcmp(a->is->device_uid, b->is->device_uid);
  return c ? c : a->at - b->at;
}

/* Problems first, then notices; in each, this device's own, then by device name. */
static void sort_issues(mc_sync_problems *p, const char *me) {
  int n = p->issue_count;
  if (n < 2) return;
  sort_item *items = malloc((size_t)n * sizeof *items);
  mc_sync_issue *sorted = malloc((size_t)n * sizeof *sorted);
  if (!items || !sorted) {
    free(items);
    free(sorted);
    return;
  }
  for (int i = 0; i < n; i++)
    items[i] = (sort_item){&p->issues[i], !p->issues[i].device_uid[0] ||
                                              !strcmp(p->issues[i].device_uid, me),
                           i};
  qsort(items, (size_t)n, sizeof *items, issue_cmp);
  for (int i = 0; i < n; i++) sorted[i] = *items[i].is;
  memcpy(p->issues, sorted, (size_t)n * sizeof *sorted);
  free(items);
  free(sorted);
}

mc_status mc_issues_build(mc_sync *sync, mc_sync_conn *c, mc_sync_problems *p, int64_t *due_ms) {
  *due_ms = 0;
  if (!mc_sync_db_group_uid(c->sdb)) return MC_OK;
  builder b = {sync, c, p, 0, 0, (int64_t)mc_wall_ms(), 0, NULL, NULL};
  sqlite3_prepare_v2(mc_sync_db_handle(c->sdb),
                     "SELECT coalesce(nullif(p.name, ''), d.name), d.kind FROM devices d"
                     " LEFT JOIN profiles p ON p.device_uid = d.uid WHERE d.uid = ?1",
                     -1, &b.names, NULL);
  sqlite3_prepare_v2(mc_library_db(c->lib),
                     "SELECT t.title, t.album_uid, a.title FROM tracks t"
                     " LEFT JOIN albums a ON a.uid = t.album_uid WHERE t.uid = ?1",
                     -1, &b.titles, NULL);
  from_checks(&b);
  from_findings(&b);
  from_waiting(&b);
  peer_note *notes = NULL;
  int nnotes = from_peers(&b, &notes);
  from_capabilities(&b, notes, nnotes);
  free(notes);
  from_archives(&b);
  from_notices(&b);
  sqlite3_finalize(b.names);
  sqlite3_finalize(b.titles);
  sort_issues(p, mc_sync_db_device_uid(c->sdb));
  *due_ms = b.due;
  return b.failed ? MC_ERR_NOMEM : MC_OK;
}

/* --- Words --- */

static const char *shown(const char *name) {
  return name && *name ? name : mc_str(MC_STR_DEVICE_UNNAMED);
}

/* A wall time as the clock shows it, "14:30". */
static void clock_text(int64_t ms, char *buf, size_t size) {
  time_t t = (time_t)(ms / 1000);
  struct tm tm;
#ifdef _WIN32
  int ok = localtime_s(&tm, &t) == 0;
#else
  int ok = localtime_r(&t, &tm) != NULL;
#endif
  if (!ok || !strftime(buf, size, "%H:%M", &tm)) snprintf(buf, size, "?");
}

/* How long ago a wall time was, "12 days ago", as the device rows say it. */
static void ago_text(int64_t ms, char *buf, size_t size) {
  int64_t min = ((int64_t)mc_wall_ms() - ms) / 60000;
  if (min < 1)
    snprintf(buf, size, "%s", mc_str(MC_STR_SYNCED_JUST_NOW));
  else if (min < 60)
    mc_format(buf, size, mc_str_plural(MC_STR_AGO_MINUTES, (long)min), MC_D(min));
  else if (min < 24 * 60)
    mc_format(buf, size, mc_str_plural(MC_STR_AGO_HOURS, (long)(min / 60)), MC_D(min / 60));
  else if (min < 2 * 24 * 60)
    snprintf(buf, size, "%s", mc_str(MC_STR_AGO_YESTERDAY));
  else
    mc_format(buf, size, mc_str_plural(MC_STR_AGO_DAYS, (long)(min / (24 * 60))),
              MC_D(min / (24 * 60)));
}

/* "“Blue Train”, “Kind of Blue” and 3 other albums". */
static void examples_text(const mc_sync_issue *is, char *buf, size_t size) {
  char q[MC_SYNC_ISSUE_EXAMPLES][192];
  int n = is->nexamples;
  for (int i = 0; i < n; i++)
    mc_format(q[i], sizeof q[i], mc_str(MC_STR_SYNC_EXAMPLES_ONE), MC_S(is->examples[i]));
  char list[640] = "";
  if (n == 1)
    snprintf(list, sizeof list, "%s", q[0]);
  else if (n == 2)
    mc_format(list, sizeof list, mc_str(MC_STR_SYNC_EXAMPLES_TWO), MC_S(q[0]), MC_S(q[1]));
  else if (n >= 3)
    mc_format(list, sizeof list, mc_str(MC_STR_SYNC_EXAMPLES_THREE), MC_S(q[0]), MC_S(q[1]),
              MC_S(q[2]));
  int more = (is->albums > 0 ? is->albums : is->tracks) - n;
  if (n && more > 0)
    mc_format(buf, size,
              mc_str_plural(is->albums > 0 ? MC_STR_SYNC_EXAMPLES_MORE_ALBUMS
                                           : MC_STR_SYNC_EXAMPLES_MORE_TRACKS,
                            more),
              MC_S(list), MC_D(more));
  else
    snprintf(buf, size, "%s", list);
}

void mc_sync_issue_describe(const mc_sync_issue *is, int flags, mc_sync_issue_text *out) {
  (void)flags;
  memset(out, 0, sizeof *out);
  if (!is) return;
  const char *name = shown(is->device_name);
  char ex[768], a[64], c[64];
  examples_text(is, ex, sizeof ex);
  char *t = out->title, *body = out->body, *fix = out->fix;
  size_t ts = sizeof out->title, bs = sizeof out->body, fs = sizeof out->fix;
  switch (is->kind) {
    case MC_ISSUE_FOLDER_UNWRITABLE:
      mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_FOLDER_TITLE), MC_S(""));
      mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_FOLDER_BODY), MC_S(is->detail ? is->detail : ""),
                MC_S(is->error ? is->error : ""));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_FOLDER_FIX), MC_S(""));
      break;
    case MC_ISSUE_DISK_FULL:
      mc_format_size(a, sizeof a, is->bytes);
      mc_format_size(c, sizeof c, is->bytes_free > 0 ? is->bytes_free : 0);
      mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_DISK_FULL_TITLE), MC_S(""));
      mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_DISK_FULL_BODY), MC_S(a),
                MC_S(is->detail ? is->detail : ""), MC_S(c));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_DISK_FULL_FIX), MC_S(""));
      break;
    case MC_ISSUE_WRITE_FAILED:
      mc_format(t, ts, mc_str_plural(MC_STR_SYNC_ISSUE_WRITE_FAILED_TITLE, is->count),
                MC_D(is->count));
      mc_format(body, bs, mc_str_plural(MC_STR_SYNC_ISSUE_WRITE_FAILED_BODY, is->count),
                MC_D(is->count), MC_S(name), MC_S(is->error ? is->error : "?"));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_WRITE_FAILED_FIX), MC_S(""));
      break;
    case MC_ISSUE_FILES_BUSY:
      mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_FILES_BUSY_TITLE), MC_S(name));
      mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_FILES_BUSY_BODY), MC_S(name));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_FILES_BUSY_FIX), MC_S(name));
      break;
    case MC_ISSUE_FAILED_FILES:
      mc_format(t, ts, mc_str_plural(MC_STR_SYNC_PROBLEMS_FAILED, is->count), MC_D(is->count));
      mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_FAILED_FILES_BODY), MC_S(""));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_FAILED_FILES_FIX), MC_S(""));
      break;
    case MC_ISSUE_UNKNOWN_FILES:
      mc_format(t, ts, mc_str_plural(MC_STR_SYNC_PROBLEMS_UNKNOWN, is->count), MC_D(is->count));
      mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_UNKNOWN_FILES_BODY), MC_S(""));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_UNKNOWN_FILES_FIX), MC_S(""));
      break;
    case MC_ISSUE_NO_TRANSCODER:
      mc_format(t, ts, mc_str_plural(MC_STR_SYNC_ISSUE_NO_TRANSCODER_TITLE, is->tracks),
                MC_D(is->tracks));
      mc_format(body, bs, mc_str_plural(MC_STR_SYNC_ISSUE_NO_TRANSCODER_BODY, is->tracks),
                MC_D(is->tracks), MC_S(name), MC_S(ex));
      mc_format(fix, fs,
                mc_str(is->device_kind == MC_DEVICE_PHONE ? MC_STR_SYNC_ISSUE_NO_TRANSCODER_FIX_PHONE
                                                          : MC_STR_SYNC_ISSUE_NO_TRANSCODER_FIX_DESKTOP),
                MC_S(name));
      break;
    case MC_ISSUE_TRANSCODER_AWAY:
      mc_format(t, ts, mc_str_plural(MC_STR_SYNC_ISSUE_TRANSCODER_AWAY_TITLE, is->tracks),
                MC_D(is->tracks), MC_S(name));
      mc_format(body, bs, mc_str_plural(MC_STR_SYNC_ISSUE_TRANSCODER_AWAY_BODY, is->tracks),
                MC_D(is->tracks), MC_S(name), MC_S(ex));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_TRANSCODER_AWAY_FIX), MC_S(name));
      break;
    case MC_ISSUE_REFUSED: {
      static const enum mc_str_id titles[MC_REFUSED_COUNT] = {
        MC_STR_SYNC_ISSUE_REFUSED_IO_TITLE, MC_STR_SYNC_ISSUE_REFUSED_MISSING_TITLE,
        MC_STR_SYNC_ISSUE_REFUSED_NO_TRANSCODE_TITLE, MC_STR_SYNC_ISSUE_REFUSED_FORMAT_TITLE};
      static const enum mc_str_id bodies[MC_REFUSED_COUNT] = {
        MC_STR_SYNC_ISSUE_REFUSED_IO_BODY, MC_STR_SYNC_ISSUE_REFUSED_MISSING_BODY,
        MC_STR_SYNC_ISSUE_REFUSED_NO_TRANSCODE_BODY, MC_STR_SYNC_ISSUE_REFUSED_FORMAT_BODY};
      static const enum mc_str_id fixes[MC_REFUSED_COUNT] = {
        MC_STR_SYNC_ISSUE_REFUSED_IO_FIX, MC_STR_SYNC_ISSUE_REFUSED_MISSING_FIX,
        MC_STR_SYNC_ISSUE_REFUSED_NO_TRANSCODE_FIX, MC_STR_SYNC_ISSUE_REFUSED_FORMAT_FIX};
      int r = (unsigned)is->refusal < MC_REFUSED_COUNT ? (int)is->refusal : MC_REFUSED_FORMAT;
      if (r == MC_REFUSED_IO || r == MC_REFUSED_MISSING)
        mc_format(t, ts, mc_str_plural(titles[r], is->tracks), MC_D(is->tracks), MC_S(name));
      else
        mc_format(t, ts, mc_str(titles[r]), MC_S(name));
      mc_format(body, bs, mc_str(bodies[r]), MC_S(name), MC_S(ex));
      mc_format(fix, fs, mc_str(fixes[r]), MC_S(name));
      break;
    }
    case MC_ISSUE_DEVICE_LACKS:
    case MC_ISSUE_DEVICE_LEFT:
    case MC_ISSUE_NO_HOLDER:
      mc_format(t, ts, mc_str_plural(MC_STR_SYNC_ISSUE_NO_HOLDER_TITLE, is->tracks),
                MC_D(is->tracks));
      if (is->kind == MC_ISSUE_DEVICE_LACKS) {
        mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_DEVICE_LACKS_BODY), MC_S(name), MC_S(ex));
        mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_DEVICE_LACKS_FIX), MC_S(name));
      } else if (is->kind == MC_ISSUE_DEVICE_LEFT) {
        mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_DEVICE_LEFT_BODY), MC_S(name), MC_S(ex));
        mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_DEVICE_LEFT_FIX), MC_S(name));
      } else {
        mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_NO_HOLDER_BODY), MC_S(ex));
        mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_NO_HOLDER_FIX), MC_S(""));
      }
      break;
    case MC_ISSUE_LOST:
      mc_format(t, ts, mc_str_plural(MC_STR_SYNC_PROBLEMS_LOST, is->tracks), MC_D(is->tracks));
      mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_LOST_BODY), MC_S(ex));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_LOST_FIX), MC_S(""));
      break;
    case MC_ISSUE_HELD:
      switch (is->hold) {
        case MC_HOLD_BIN:
          mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_HELD_BIN_TITLE), MC_S(name));
          mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_HELD_BIN_BODY), MC_S(name));
          break;
        case MC_HOLD_CANCELLED:
          clock_text(is->until_ms, a, sizeof a);
          mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_HELD_CANCELLED_TITLE), MC_S(name));
          mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_HELD_CANCELLED_BODY), MC_S(name), MC_S(a));
          break;
        case MC_HOLD_AUTO_OFF:
          mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_HELD_AUTO_OFF_TITLE), MC_S(name));
          mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_HELD_AUTO_OFF_BODY), MC_S(name));
          break;
        default:
          mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_HELD_NOT_CHOSEN_TITLE), MC_S(name));
          mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_HELD_NOT_CHOSEN_BODY), MC_S(name));
          break;
      }
      mc_format(fix, fs,
                mc_str(is->hold == MC_HOLD_NOT_CHOSEN ? MC_STR_SYNC_ISSUE_HELD_NOT_CHOSEN_FIX
                                                      : MC_STR_SYNC_ISSUE_HELD_FIX),
                MC_S(name));
      break;
    case MC_ISSUE_LOW_SPACE:
      mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_LOW_SPACE_TITLE), MC_S(name));
      mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_LOW_SPACE_BODY), MC_S(name));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_LOW_SPACE_FIX), MC_S(name));
      break;
    case MC_ISSUE_WRONG_DEVICE:
      mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_WRONG_DEVICE_TITLE), MC_S(name));
      mc_format(body, bs, mc_str_plural(MC_STR_SYNC_ISSUE_WRONG_DEVICE_BODY, is->count),
                MC_D(is->count), MC_S(name), MC_S(is->detail ? is->detail : "?"));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_WRONG_DEVICE_FIX), MC_S(name));
      break;
    case MC_ISSUE_NOTICE_ARCHIVE:
      mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_NOTICE_ARCHIVE_TITLE), MC_S(name));
      mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_NOTICE_ARCHIVE_BODY), MC_S(name));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_NOTICE_ARCHIVE_FIX), MC_S(""));
      break;
    case MC_ISSUE_NOTICE_BUDGET:
      mc_format_size(a, sizeof a, is->bytes);
      mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_NOTICE_BUDGET_TITLE), MC_S(""));
      mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_NOTICE_BUDGET_BODY), MC_S(a));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_NOTICE_BUDGET_FIX), MC_S(""));
      break;
    case MC_ISSUE_ARCHIVE_AWAY:
      ago_text(is->until_ms, a, sizeof a);
      mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_ARCHIVE_AWAY_TITLE), MC_S(name), MC_S(a));
      mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_ARCHIVE_AWAY_BODY), MC_S(name), MC_S(a));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_ARCHIVE_AWAY_FIX), MC_S(name));
      break;
    case MC_ISSUE_ARCHIVE_DAMAGED:
      mc_format(t, ts, mc_str_plural(MC_STR_SYNC_ISSUE_ARCHIVE_DAMAGED_TITLE, is->tracks),
                MC_D(is->tracks), MC_S(name));
      mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_ARCHIVE_DAMAGED_BODY), MC_S(name), MC_S(ex));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_ARCHIVE_DAMAGED_FIX), MC_S(name));
      break;
    case MC_ISSUE_ARCHIVE_FULL:
      mc_format_size(a, sizeof a, is->bytes > 0 ? is->bytes : 0);
      mc_format(t, ts, mc_str_plural(MC_STR_SYNC_ISSUE_ARCHIVE_FULL_TITLE, is->tracks),
                MC_D(is->tracks), MC_S(name));
      mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_ARCHIVE_FULL_BODY), MC_S(name), MC_S(a));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_ARCHIVE_FULL_FIX), MC_S(name));
      break;
    case MC_ISSUE_ARCHIVE_LOST: {
      int days = (int)(((int64_t)mc_wall_ms() - is->until_ms) / ((int64_t)24 * 60 * 60 * 1000));
      mc_format_size(a, sizeof a, is->bytes > 0 ? is->bytes : 0);
      mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_ARCHIVE_LOST_TITLE), MC_S(name), MC_D(days));
      mc_format(body, bs, mc_str(MC_STR_SYNC_OFFER_BODY), MC_S(name), MC_D(days));
      mc_format(fix, fs,
                mc_str(is->count ? MC_STR_SYNC_ISSUE_ARCHIVE_LOST_FIX
                                 : MC_STR_SYNC_ISSUE_ARCHIVE_LOST_FIX_NO_ROOM),
                MC_S(name), MC_S(a));
      break;
    }
    case MC_ISSUE_ARCHIVE_REINSTALL:
      mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_ARCHIVE_REINSTALL_TITLE), MC_S(name));
      mc_format(body, bs, mc_str(MC_STR_SYNC_OFFER_BODY_REINSTALL), MC_S(name));
      mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_ARCHIVE_REINSTALL_FIX), MC_S(name));
      break;
    case MC_ISSUE_TAKEOVER:
      if (is->notice) {
        mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_TAKEOVER_DONE_TITLE), MC_S(name));
        if (is->tracks > 0)
          mc_format(body, bs, mc_str_plural(MC_STR_SYNC_ISSUE_TAKEOVER_DONE_BODY, is->tracks),
                    MC_S(name), MC_D(is->tracks), MC_S(ex));
        else
          mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_TAKEOVER_DONE_ALL), MC_S(name));
      } else if (is->error) {
        mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_TAKEOVER_FAILED_TITLE), MC_S(name));
        mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_TAKEOVER_FAILED_BODY), MC_S(name),
                  MC_S(is->error));
        mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_TAKEOVER_FAILED_FIX), MC_S(name));
      } else {
        mc_format(t, ts, mc_str(MC_STR_SYNC_ISSUE_TAKEOVER_TITLE), MC_S(name));
        mc_format(body, bs, mc_str(MC_STR_SYNC_ISSUE_TAKEOVER_BODY), MC_D(is->count),
                  MC_D(is->bytes), MC_S(name));
        mc_format(fix, fs, mc_str(MC_STR_SYNC_ISSUE_TAKEOVER_FIX), MC_S(name));
      }
      break;
    default:
      break;
  }
}

/* --- Texts --- */

size_t mc_sync_finished_text(const mc_sync_result *r, char *buf, size_t size) {
  if (!r || r->files_missing <= 0) {
    if (buf && size) buf[0] = '\0';
    return 0;
  }
  return mc_format(buf, size, mc_str_plural(MC_STR_SYNC_FINISHED_MISSING, r->files_missing),
                   MC_D(r->files_missing));
}

static size_t copy_out(char *buf, size_t size, const char *s) {
  size_t n = strlen(s);
  if (buf && size) snprintf(buf, size, "%s", s);
  return n;
}

size_t mc_sync_reach_text(mc_sync *sync, const char *device_uid, const char *name,
                          int warning_only, char *buf, size_t size) {
  if (buf && size) buf[0] = '\0';
  if (!sync || !device_uid) return 0;
  int phone = sync->kind == MC_DEVICE_PHONE;
  const char *who = shown(name);
  if (!strcmp(device_uid, mc_sync_device_uid(sync))) {
    mc_sync_self_reach r;
    if (mc_sync_reach_self(sync, &r) != MC_OK) return 0;
    if (r.state == MC_REACH_BLOCKED) {
      char text[512], taken[256] = "";
      mc_format(text, sizeof text,
                mc_str_plural(phone ? MC_STR_REACH_SELF_BLOCKED_PHONE : MC_STR_REACH_SELF_BLOCKED,
                              r.blocked),
                MC_D(r.blocked), MC_D(r.port));
      if (!r.default_port) {
        mc_format(taken, sizeof taken, mc_str(MC_STR_REACH_PORT_TAKEN), MC_D(MC_SYNC_DEFAULT_PORT));
        size_t len = strlen(text);
        snprintf(text + len, sizeof text - len, " %s", taken);
      }
      return copy_out(buf, size, text);
    }
    if (warning_only) return 0;
    return copy_out(buf, size,
                    mc_str(r.state == MC_REACH_OK
                               ? (phone ? MC_STR_REACH_SELF_OK_PHONE : MC_STR_REACH_SELF_OK)
                               : MC_STR_REACH_UNKNOWN));
  }
  mc_sync_reach to_them, to_us;
  if (mc_sync_reach_peer(sync, device_uid, &to_them, &to_us) != MC_OK)
    to_them = to_us = MC_REACH_UNKNOWN;
  if (to_them == MC_REACH_BLOCKED) {
    enum mc_str_id id = to_us == MC_REACH_BLOCKED
                            ? (phone ? MC_STR_REACH_PEER_BLOCKED_BOTH_PHONE
                                     : MC_STR_REACH_PEER_BLOCKED_BOTH)
                            : (phone ? MC_STR_REACH_PEER_BLOCKED_PHONE : MC_STR_REACH_PEER_BLOCKED);
    return mc_format(buf, size, mc_str(id), MC_S(who));
  }
  /* Its not reaching this device shows on this device's row instead. */
  if (warning_only) return 0;
  if (to_us == MC_REACH_BLOCKED)
    return mc_format(buf, size,
                     mc_str(phone ? MC_STR_REACH_PEER_US_BLOCKED_PHONE : MC_STR_REACH_PEER_US_BLOCKED),
                     MC_S(who));
  if (to_them == MC_REACH_OK)
    return copy_out(buf, size,
                    mc_str(to_us == MC_REACH_OK
                               ? MC_STR_REACH_PEER_OK
                               : (phone ? MC_STR_REACH_PEER_OUT_OK_PHONE : MC_STR_REACH_PEER_OUT_OK)));
  return copy_out(buf, size, mc_str(MC_STR_REACH_UNKNOWN));
}

size_t mc_sync_device_warning(mc_sync *sync, const mc_sync_device *d, int flags, char *buf,
                              size_t size) {
  if (buf && size) buf[0] = '\0';
  if (!sync || !d || d->removed) return 0;
  if (d->is_self) {
    char reach[512];
    if (mc_sync_reach_text(sync, d->uid, d->name, 1, reach, sizeof reach) > 0)
      return copy_out(buf, size, reach);
    if (d->low_space) return copy_out(buf, size, mc_str(MC_STR_SYNC_DEVICE_LOW_SPACE));
    return 0;
  }
  if (d->version.outdated != MC_SYNC_VERSION_OK)
    return mc_sync_version_text(shown(d->name), &d->version, buf, size);
  if (d->peer_hold != MC_HOLD_NONE) {
    mc_sync_issue is = {.kind = MC_ISSUE_HELD, .device_name = d->name, .device_kind = d->kind,
                        .hold = d->peer_hold, .until_ms = d->peer_hold_until_ms, .bytes_free = -1};
    mc_sync_issue_text text;
    mc_sync_issue_describe(&is, flags, &text);
    char line[sizeof text.title + sizeof text.fix + 2];
    snprintf(line, sizeof line, "%s %s", text.title, text.fix);
    return copy_out(buf, size, line);
  }
  if (d->online) {
    char reach[512];
    if (mc_sync_reach_text(sync, d->uid, d->name, 1, reach, sizeof reach) > 0)
      return copy_out(buf, size, reach);
  }
  if (d->wrong_address) {
    mc_sync_issue is = {.kind = MC_ISSUE_WRONG_DEVICE, .device_name = d->name,
                        .device_kind = d->kind, .count = d->wrong_count,
                        .detail = d->wrong_address, .bytes_free = -1};
    mc_sync_issue_text text;
    mc_sync_issue_describe(&is, flags, &text);
    return copy_out(buf, size, text.body);
  }
  if (d->archive && !d->online) {
    int64_t seen = 0;
    mc_mutex_lock(&sync->lock);
    for (int i = 0; sync->problems && i < sync->problems->issue_count; i++) {
      const mc_sync_issue *is = &sync->problems->issues[i];
      if ((is->kind == MC_ISSUE_ARCHIVE_AWAY || is->kind == MC_ISSUE_ARCHIVE_LOST) &&
          !strcmp(is->device_uid, d->uid))
        seen = is->until_ms;
    }
    mc_mutex_unlock(&sync->lock);
    if (seen > 0) {
      char ago[64];
      ago_text(seen, ago, sizeof ago);
      return mc_format(buf, size, mc_str(MC_STR_SYNC_DEVICE_ARCHIVE_AWAY), MC_S(ago));
    }
  }
  if (d->low_space) return copy_out(buf, size, mc_str(MC_STR_SYNC_DEVICE_LOW_SPACE));
  return 0;
}

size_t mc_sync_device_note(const mc_sync_device *d, int flags, int always, char *buf, size_t size) {
  if (buf && size) buf[0] = '\0';
  if (!d || d->is_self || d->removed) return 0;
  int app = (flags & MC_SYNC_TEXT_APP_OPEN) != 0;
  char at[32];
  clock_text(d->next_auto_ms, at, sizeof at);
  switch (d->hold) {
    case MC_HOLD_CANCELLED:
      if (d->next_auto_ms) return mc_format(buf, size, mc_str(MC_STR_SYNC_HOLD_CANCELLED), MC_S(at));
      return 0;
    case MC_HOLD_PEER_CANCELLED:
      if (d->next_auto_ms)
        return mc_format(buf, size,
                         mc_str(app ? MC_STR_SYNC_HOLD_PEER_CANCELLED_APP
                                    : MC_STR_SYNC_HOLD_PEER_CANCELLED),
                         MC_S(at));
      return 0;
    case MC_HOLD_BIN: return copy_out(buf, size, mc_str(MC_STR_SYNC_HOLD_BIN));
    case MC_HOLD_RATE: break;
    default:
      if (!always) return 0;
      break;
  }
  if (!d->next_auto_ms || (d->hold == MC_HOLD_NONE && !always)) return 0;
  return mc_format(buf, size, mc_str(app ? MC_STR_SYNC_HOLD_NEXT_APP : MC_STR_SYNC_HOLD_NEXT),
                   MC_S(at));
}

/* --- Taking over an archive (the rest is in takeover.c) --- */

void mc_sync_archive_offer_describe(const mc_sync_archive_offer *offer, mc_device_kind self,
                                    mc_sync_offer_text *out) {
  memset(out, 0, sizeof *out);
  if (!offer) return;
  const char *name = shown(offer->name);
  snprintf(out->title, sizeof out->title, "%s",
           mc_str(self == MC_DEVICE_PHONE ? MC_STR_SYNC_OFFER_TITLE_PHONE
                                          : MC_STR_SYNC_OFFER_TITLE_COMPUTER));
  int days = offer->last_seen_ms > 0
                 ? (int)(((int64_t)mc_wall_ms() - offer->last_seen_ms) / ((int64_t)24 * 60 * 60 * 1000))
                 : MC_SYNC_ARCHIVE_LOST_DAYS;
  char first[512], space[512] = "", gone[480] = "", need[64], have[64];
  if (offer->reinstall)
    mc_format(first, sizeof first, mc_str(MC_STR_SYNC_OFFER_BODY_REINSTALL), MC_S(name));
  else
    mc_format(first, sizeof first, mc_str(MC_STR_SYNC_OFFER_BODY), MC_S(name), MC_D(days));
  mc_format_size(need, sizeof need, offer->bytes_needed > 0 ? offer->bytes_needed : 0);
  mc_format_size(have, sizeof have, offer->bytes_free > 0 ? offer->bytes_free : 0);
  if (offer->can_take && offer->reinstall)
    mc_format(space, sizeof space, mc_str(MC_STR_SYNC_OFFER_SPACE_REINSTALL), MC_S(name));
  else if (offer->can_take)
    mc_format(space, sizeof space, mc_str(MC_STR_SYNC_OFFER_SPACE), MC_S(need), MC_S(have),
              MC_S(name));
  else if (offer->nobody_can)
    mc_format(space, sizeof space, mc_str(MC_STR_SYNC_OFFER_NOBODY), MC_S(need), MC_S(name));
  else
    mc_format(space, sizeof space, mc_str(MC_STR_SYNC_OFFER_NO_ROOM_HERE), MC_S(need), MC_S(have));
  if (offer->unrecoverable > 0) {
    mc_sync_issue is = {.tracks = offer->unrecoverable, .bytes_free = -1};
    for (int i = 0; i < offer->ntracks && is.nexamples < MC_SYNC_ISSUE_EXAMPLES; i++)
      is.examples[is.nexamples++] = offer->tracks[i].title;
    char ex[768];
    examples_text(&is, ex, sizeof ex);
    mc_format(gone, sizeof gone, mc_str_plural(MC_STR_SYNC_OFFER_UNRECOVERABLE, offer->unrecoverable),
              MC_D(offer->unrecoverable), MC_S(ex));
  }
  snprintf(out->body, sizeof out->body, "%s\n\n%s%s%s", first, space, gone[0] ? "\n\n" : "", gone);
  if (offer->can_take)
    snprintf(out->button, sizeof out->button, "%s",
             mc_str(offer->reinstall ? MC_STR_SYNC_OFFER_BUTTON_REINSTALL : MC_STR_SYNC_OFFER_BUTTON));
}
