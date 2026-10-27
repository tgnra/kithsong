/* A device's wanted set and the format it holds each track in. See plan.h and "Profiles
 * and what a device holds" in docs/sync-protocol.md.
 *
 * Every track of the library is read once, sorted by uid, with its duration and this
 * device's original of it (if any); the profile then marks the wanted ones, and budget mode
 * sizes the set at each step of the ladder until it fits. */
#include "sync/plan.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "sync/archive.h"
#include "sync/merge.h"

/* One track of the library while the plan is worked out. */
typedef struct cand {
  char uid[MC_UID_STRLEN];
  int duration_ms;
  int has_orig;      /* this device has an original of it */
  int64_t orig_size;
  char orig_format[16];
  int orig_bitrate;
  int64_t held;      /* bytes of every file of it here */
  int wanted;
} cand;

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

static int cand_cmp(const void *key, const void *elem) {
  return strcmp(key, ((const cand *)elem)->uid);
}

static int track_cmp(const void *key, const void *elem) {
  return strcmp(key, ((const mc_plan_track *)elem)->track_uid);
}

/* --- Formats --- */

mc_file_kind mc_plan_file_kind(const char *source_file_uid, const char *source_peer_uid) {
  if (source_file_uid && *source_file_uid) return MC_FILE_TRANSCODE;
  if (source_peer_uid && *source_peer_uid) return MC_FILE_RECEIVED;
  return MC_FILE_IMPORTED;
}

static int same_ci(const char *a, const char *b) {
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
  return *a == *b;
}

int mc_plan_is_lossy(const char *format, int bitrate_kbps) {
  static const char *const lossy[] = {"mp3", "aac", "ogg", "oga", "opus", "wma", "mpc", NULL};
  if (!format) return 0;
  for (const char *const *f = lossy; *f; f++)
    if (same_ci(format, *f)) return 1;
  /* .m4a and .mp4 hold AAC or ALAC; only a known, low bitrate says which. */
  if (same_ci(format, "m4a") || same_ci(format, "mp4")) return bitrate_kbps > 0 && bitrate_kbps < 500;
  return 0;
}

int mc_plan_keep_lossy(const char *format, int bitrate_kbps, int64_t size_bytes, int duration_ms,
                       int kbps) {
  int64_t est;
  if (kbps <= 0 || size_bytes <= 0 || !mc_plan_is_lossy(format, bitrate_kbps)) return 0;
  if (mc_transcode_estimate_bytes(duration_ms > 0 ? duration_ms : 0, kbps, &est) != MC_OK) return 0;
  return size_bytes <= est;
}

int mc_plan_file_satisfies(const mc_plan_track *t, mc_file_kind kind, const char *format,
                           int bitrate_kbps, int64_t size_bytes) {
  if (t->format.format == MC_FORMAT_ORIGINAL) return kind != MC_FILE_TRANSCODE;
  if (kind == MC_FILE_TRANSCODE) return bitrate_kbps == t->format.bitrate_kbps;
  return mc_plan_keep_lossy(format, bitrate_kbps, size_bytes, t->duration_ms,
                            t->format.bitrate_kbps);
}

/* --- Reading --- */

/* Every track, sorted by uid, with what this device holds of it. */
static mc_status read_tracks(mc_library *lib, cand **out, int *count) {
  *out = NULL;
  *count = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT t.uid, t.duration_ms, o.uid, o.size_bytes, o.format,"
                         " o.bitrate_kbps,"
                         " (SELECT sum(h.size_bytes) FROM files h WHERE h.track_uid = t.uid)"
                         " FROM tracks t LEFT JOIN files o ON o.uid ="
                         "  (SELECT f.uid FROM files f WHERE f.track_uid = t.uid"
                         "   AND f.source_file_uid IS NULL"
                         "   ORDER BY f.source_peer_uid IS NOT NULL, f.uid LIMIT 1)"
                         " ORDER BY t.uid",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  mc_status st = MC_OK;
  cand *items = NULL;
  int n = 0, cap = 0, rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(uid)) continue;
    if (n == cap) {
      int grown = cap ? cap * 2 : 256;
      cand *more = realloc(items, (size_t)grown * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      items = more;
      cap = grown;
    }
    cand *c = &items[n++];
    memset(c, 0, sizeof *c);
    memcpy(c->uid, uid, MC_UID_STRLEN);
    c->duration_ms = sqlite3_column_int(stmt, 1);
    if (c->duration_ms < 0) c->duration_ms = 0;
    if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
      const char *format = (const char *)sqlite3_column_text(stmt, 4);
      c->has_orig = sqlite3_column_type(stmt, 3) != SQLITE_NULL;
      c->orig_size = sqlite3_column_int64(stmt, 3);
      snprintf(c->orig_format, sizeof c->orig_format, "%s", format ? format : "");
      c->orig_bitrate = sqlite3_column_int(stmt, 5);
    }
    c->held = sqlite3_column_int64(stmt, 6);
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  if (st != MC_OK) {
    free(items);
    return st;
  }
  *out = items;
  *count = n;
  return MC_OK;
}

/* Marks the tracks one selected item names. */
static mc_status mark_item(mc_library *lib, const char *kind, const char *ref, cand *cands,
                           int n) {
  const char *sql;
  if (!strcmp(kind, "album"))
    sql = "SELECT uid FROM tracks WHERE album_uid = ?1";
  else if (!strcmp(kind, "artist"))
    sql = "SELECT uid FROM tracks WHERE artist_uid = ?1"
          " OR album_uid IN (SELECT uid FROM albums WHERE artist_uid = ?1)";
  else if (!strcmp(kind, "album_group"))
    sql = "SELECT t.uid FROM album_group_entries e JOIN tracks t ON t.album_uid = e.album_uid"
          " WHERE e.group_uid = ?1";
  else if (!strcmp(kind, "playlist"))
    sql = "SELECT track_uid FROM playlist_entries WHERE playlist_uid = ?1";
  else
    return MC_OK; /* a kind this version doesn't know */
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, ref, -1, SQLITE_STATIC);
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    cand *c = uid ? bsearch(uid, cands, (size_t)n, sizeof *cands, cand_cmp) : NULL;
    if (c) c->wanted = 1;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

/* Marks the tracks of one item, an album or artist merged into another (sync/merge.h)
 * selected as the survivor. */
static mc_status mark_ref(mc_library *lib, const char *kind, const char *ref, cand *cands,
                          int n) {
  char uid[MC_UID_STRLEN];
  memcpy(uid, ref, MC_UID_STRLEN);
  if ((!strcmp(kind, "album") || !strcmp(kind, "artist")) &&
      mc_merge_resolve(lib, ref, uid) != MC_OK)
    memcpy(uid, ref, MC_UID_STRLEN);
  return mark_item(lib, kind, uid, cands, n);
}

/* The stored selection's tracks. */
static mc_status mark_selected(mc_sync_db *sdb, mc_library *lib, const char *device_uid,
                               cand *cands, int n) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "SELECT DISTINCT kind, ref_uid FROM profile_items WHERE device_uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, device_uid, -1, SQLITE_STATIC);
  mc_status st = MC_OK;
  int rc;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const char *kind = (const char *)sqlite3_column_text(stmt, 0);
    const char *ref = (const char *)sqlite3_column_text(stmt, 1);
    if (!kind || !is_uid(ref)) continue;
    st = mark_ref(lib, kind, ref, cands, n);
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

/* A selection in hand (a profile not stored yet). */
static mc_status mark_items(mc_library *lib, const mc_profile_item *items, int count,
                            cand *cands, int n) {
  mc_status st = MC_OK;
  for (int i = 0; st == MC_OK && i < count; i++) {
    const char *kind = mc_profile_kind_str(items[i].kind);
    if (!kind || memchr(items[i].ref_uid, '\0', MC_UID_STRLEN) == NULL ||
        !is_uid(items[i].ref_uid))
      continue;
    st = mark_ref(lib, kind, items[i].ref_uid, cands, n);
  }
  return st;
}

/* --- Sizing --- */

/* What the device should hold of c at kbps (0 for originals), and its size. */
static void size_at(const cand *c, int kbps, mc_transcode_opts *format, int64_t *bytes) {
  format->format = MC_FORMAT_ORIGINAL;
  format->bitrate_kbps = 0;
  if (kbps && c->has_orig &&
      mc_plan_keep_lossy(c->orig_format, c->orig_bitrate, c->orig_size, c->duration_ms, kbps)) {
    *bytes = c->orig_size;
    return;
  }
  if (!kbps && c->has_orig) {
    *bytes = c->orig_size;
    return;
  }
  if (kbps) {
    format->format = MC_FORMAT_OPUS;
    format->bitrate_kbps = kbps;
  }
  if (mc_transcode_estimate_bytes(c->duration_ms, kbps ? kbps : MC_PLAN_ORIGINAL_KBPS, bytes) !=
      MC_OK)
    *bytes = 0;
}

static int64_t total_at(const cand *cands, int n, int kbps) {
  int64_t total = 0;
  for (int i = 0; i < n; i++) {
    if (!cands[i].wanted) continue;
    mc_transcode_opts f;
    int64_t b;
    size_at(&cands[i], kbps, &f, &b);
    total += b;
  }
  return total;
}

/* The profile a plan follows: the stored row's fields, or a caller's. */
typedef struct settings {
  int all, opus, kbps, budget;
  int64_t budget_bytes;
} settings;

/* device_uid's stored profile into s; *have = 0 if it has no profiles row. */
static mc_status read_settings(mc_sync_db *sdb, const char *device_uid, settings *s, int *have) {
  memset(s, 0, sizeof *s);
  s->all = 1;
  *have = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "SELECT mode, format, bitrate_kbps, budget_bytes, budget_enabled"
                         " FROM profiles WHERE device_uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, device_uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    *have = 1;
    const char *mode = (const char *)sqlite3_column_text(stmt, 0);
    const char *format = (const char *)sqlite3_column_text(stmt, 1);
    s->all = !mode || strcmp(mode, "selected") != 0;
    s->opus = format && !strcmp(format, "opus");
    s->kbps = sqlite3_column_int(stmt, 2);
    s->budget_bytes = sqlite3_column_int64(stmt, 3);
    s->budget = sqlite3_column_int(stmt, 4) != 0;
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW || rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

mc_status mc_plan_compute(mc_sync_db *sdb, mc_library *lib, const char *device_uid,
                          int64_t free_bytes, mc_plan **out) {
  return mc_plan_compute_profile(sdb, lib, device_uid, NULL, free_bytes, 0, out);
}

mc_status mc_plan_compute_profile(mc_sync_db *sdb, mc_library *lib, const char *device_uid,
                                  const mc_sync_profile *profile, int64_t free_bytes, int flags,
                                  mc_plan **out) {
  if (out) *out = NULL;
  if (!sdb || !lib || !is_uid(device_uid) || !out) return MC_ERR_INVALID_ARG;
  mc_plan *plan = calloc(1, sizeof *plan);
  if (!plan) return MC_ERR_NOMEM;
  plan->limit_bytes = -1;

  /* The profile. */
  settings set;
  int have = 1;
  mc_status st = MC_OK;
  if (profile) {
    memset(&set, 0, sizeof set);
    set.all = profile->sync_all != 0;
    set.opus = profile->format.format == MC_FORMAT_OPUS;
    set.kbps = profile->format.bitrate_kbps;
    set.budget = profile->budget_enabled != 0;
    set.budget_bytes = profile->budget_bytes;
  } else {
    st = read_settings(sdb, device_uid, &set, &have);
  }
  int archive = st == MC_OK ? mc_archive_is(mc_sync_db_handle(sdb), device_uid) : 0;
  if (st == MC_OK && archive < 0) st = MC_ERR_DB;
  if (st != MC_OK) {
    free(plan);
    return st;
  }
  if (!have) {
    plan->status = MC_PLAN_NO_PROFILE;
    *out = plan;
    return MC_OK;
  }
  plan->archive = archive || (profile && profile->archive);
  int all = set.all, opus = set.opus, kbps = set.kbps, budget = set.budget;
  int64_t budget_bytes = set.budget_bytes;
  if (plan->archive) {
    /* Everything, originals, whatever the rest of the row says. */
    all = 1;
    opus = 0;
    budget = 0;
  }
  /* A budget picks the format (auto), whatever format a row from an older version stored
   * beside it: the walk starts at originals. */
  if (budget) opus = 0;

  /* The ladder: originals, then each Opus bitrate. Without a budget it's the profile's own
   * step; with one, the walk takes the first that fits. */
  int ladder[1 + MC_OPUS_BITRATE_COUNT];
  ladder[0] = 0;
  for (int i = 0; i < MC_OPUS_BITRATE_COUNT; i++) ladder[i + 1] = mc_opus_bitrates[i];
  int start = 0;
  if (opus) {
    start = 1 + MC_OPUS_BITRATE_COUNT - 1; /* an unknown bitrate: the smallest */
    for (int i = 0; i < MC_OPUS_BITRATE_COUNT; i++)
      if (mc_opus_bitrates[i] == kbps) start = i + 1;
  }

  cand *cands = NULL;
  int n = 0;
  st = read_tracks(lib, &cands, &n);
  if (st == MC_OK) {
    if (all)
      for (int i = 0; i < n; i++) cands[i].wanted = 1;
    else if (profile)
      st = mark_items(lib, profile->items, profile->items ? profile->count : 0, cands, n);
    else
      st = mark_selected(sdb, lib, device_uid, cands, n);
  }
  for (int i = 0; st == MC_OK && i < n; i++) plan->wanted += cands[i].wanted;
  if (st == MC_OK && (flags & MC_PLAN_SIZES))
    for (int l = 0; l < MC_PLAN_STEPS; l++) plan->step_bytes[l] = total_at(cands, n, ladder[l]);

  int chosen = start;
  if (st == MC_OK && budget) {
    int64_t limit = budget_bytes > 0 ? budget_bytes : INT64_MAX;
    if (free_bytes >= 0) {
      int64_t held = 0;
      if (!strcmp(device_uid, mc_sync_db_device_uid(sdb)))
        for (int i = 0; i < n; i++)
          if (cands[i].wanted) held += cands[i].held;
      if (free_bytes + held < limit) limit = free_bytes + held;
    }
    if (limit != INT64_MAX) plan->limit_bytes = limit;
    chosen = -1;
    int64_t total = 0;
    for (int l = start; l <= MC_OPUS_BITRATE_COUNT && chosen < 0; l++) {
      total = total_at(cands, n, ladder[l]);
      if (total <= limit) chosen = l;
    }
    if (chosen < 0) {
      plan->status = MC_PLAN_OVER_BUDGET;
      plan->total_bytes = total;
      plan->shortfall_bytes = total - limit;
    }
  }

  if (st == MC_OK && plan->status == MC_PLAN_OK) {
    plan->kbps = ladder[chosen];
    int wanted = plan->wanted;
    plan->tracks = wanted ? malloc((size_t)wanted * sizeof *plan->tracks) : NULL;
    if (wanted && !plan->tracks) st = MC_ERR_NOMEM;
    for (int i = 0; st == MC_OK && i < n; i++) {
      if (!cands[i].wanted) continue;
      mc_plan_track *t = &plan->tracks[plan->count++];
      memcpy(t->track_uid, cands[i].uid, MC_UID_STRLEN);
      t->duration_ms = cands[i].duration_ms;
      size_at(&cands[i], plan->kbps, &t->format, &t->est_bytes);
      plan->total_bytes += t->est_bytes;
    }
  }
  free(cands);
  if (st != MC_OK) {
    mc_plan_free(plan);
    return st;
  }
  *out = plan;
  return MC_OK;
}

void mc_plan_free(mc_plan *plan) {
  if (!plan) return;
  free(plan->tracks);
  free(plan);
}

const mc_plan_track *mc_plan_find(const mc_plan *plan, const char *track_uid) {
  if (!plan || !plan->count || !track_uid) return NULL;
  return bsearch(track_uid, plan->tracks, (size_t)plan->count, sizeof *plan->tracks, track_cmp);
}
