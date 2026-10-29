/* Merging libraries, the library side: setting the library aside, and merging the set-aside
 * copy into the kept library once that has arrived. See libmerge.h. */
#include "sync/libmerge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "musiccore/tagger.h"
#include "tagger/dupes.h"
#include "tagger/import_db.h"
#include "util/platform.h"

/* The example smart lists' fixed uids (migration v10) all start with this. */
#define EXAMPLE_PREFIX "5e1ec7ed-0000-4000-8000-"

int mc_libmerge_keeps_first(int64_t tracks_a, int64_t bytes_a, const char *library_a,
                            int64_t tracks_b, int64_t bytes_b, const char *library_b) {
  if (tracks_a != tracks_b) return tracks_a > tracks_b;
  if (bytes_a != bytes_b) return bytes_a > bytes_b;
  return strcmp(library_a ? library_a : "", library_b ? library_b : "") <= 0;
}

/* --- Setting the library aside --- */

/* Days since 1970-01-01 to a civil date (H. Hinnant's algorithm). */
static void civil_date(int64_t days, int64_t *y, int *m, int *d) {
  days += 719468;
  int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  int64_t doe = days - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  *d = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y = yoe + era * 400 + (*m <= 2);
}

/* A name in the library folder that's free, malloc'd. */
static char *backup_name(mc_library *lib) {
  int64_t y;
  int m, d;
  civil_date((int64_t)(mc_wall_ms() / 1000 / 86400), &y, &m, &d);
  char name[96];
  for (int n = 1; n < 1000; n++) {
    if (n == 1)
      snprintf(name, sizeof name, MC_LIBMERGE_PREFIX "%04lld-%02d-%02d.db", (long long)y, m, d);
    else
      snprintf(name, sizeof name, MC_LIBMERGE_PREFIX "%04lld-%02d-%02d-%d.db", (long long)y, m, d,
               n);
    char *abs = mc_library_abspath(lib, name);
    int taken = !abs || mc_file_exists(abs);
    free(abs);
    if (!taken) return mc_strdup(name);
  }
  return NULL;
}

mc_status mc_libmerge_set_aside(mc_library *lib, char **backup_abs) {
  if (backup_abs) *backup_abs = NULL;
  if (!lib) return MC_ERR_INVALID_ARG;
  char *name = backup_name(lib);
  char *abs = name ? mc_library_abspath(lib, name) : NULL;
  mc_status st = abs ? MC_OK : MC_ERR_NOMEM;
  sqlite3_stmt *stmt = NULL;
  if (st == MC_OK &&
      sqlite3_prepare_v2(mc_library_db(lib), "VACUUM INTO ?1", -1, &stmt, NULL) != SQLITE_OK)
    st = MC_ERR_DB;
  if (st == MC_OK) {
    sqlite3_bind_text(stmt, 1, abs, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
      fprintf(stderr, "musiccore: merge: couldn't copy the library aside: %s\n",
              sqlite3_errmsg(mc_library_db(lib)));
      st = MC_ERR_IO;
    }
  }
  sqlite3_finalize(stmt);
  /* Only once the copy is whole is the library emptied. */
  if (st == MC_OK) st = mc_library_reset_for_merge(lib);
  if (st == MC_OK) st = mc_library_meta_set(lib, MC_LIBMERGE_META, name);
  free(name);
  if (st == MC_OK && backup_abs)
    *backup_abs = abs;
  else
    free(abs);
  return st;
}

int mc_libmerge_pending(mc_library *lib, char **backup_abs) {
  if (backup_abs) *backup_abs = NULL;
  char name[256];
  if (!lib || mc_library_meta_get(lib, MC_LIBMERGE_META, name, sizeof name) != MC_OK || !name[0])
    return 0;
  if (backup_abs) *backup_abs = mc_library_abspath(lib, name);
  return 1;
}

/* --- Maps from the copy's uids to the kept library's --- */

typedef struct uid_pair {
  char from[MC_UID_STRLEN];
  char to[MC_UID_STRLEN];
} uid_pair;

typedef struct uid_map {
  uid_pair *v;
  int n, cap, sorted;
} uid_map;

static int pair_cmp(const void *a, const void *b) {
  return strcmp(((const uid_pair *)a)->from, ((const uid_pair *)b)->from);
}

static void map_put(uid_map *m, const char *from, const char *to) {
  if (!from || !to || strlen(from) >= MC_UID_STRLEN || strlen(to) >= MC_UID_STRLEN || !*to) return;
  if (m->n == m->cap) {
    int cap = m->cap ? m->cap * 2 : 64;
    uid_pair *v = realloc(m->v, (size_t)cap * sizeof *v);
    if (!v) return;
    m->v = v;
    m->cap = cap;
  }
  snprintf(m->v[m->n].from, MC_UID_STRLEN, "%s", from);
  snprintf(m->v[m->n].to, MC_UID_STRLEN, "%s", to);
  m->n++;
  m->sorted = 0;
}

static const char *map_get(uid_map *m, const char *from) {
  if (!from || !m->n) return NULL;
  if (!m->sorted) {
    qsort(m->v, (size_t)m->n, sizeof *m->v, pair_cmp);
    m->sorted = 1;
  }
  uid_pair key;
  snprintf(key.from, sizeof key.from, "%s", from);
  const uid_pair *p = bsearch(&key, m->v, (size_t)m->n, sizeof *m->v, pair_cmp);
  return p ? p->to : NULL;
}

/* --- Small helpers --- */

static char *col_dup(sqlite3_stmt *stmt, int col) {
  const char *v = (const char *)sqlite3_column_text(stmt, col);
  return v && *v ? mc_strdup(v) : NULL;
}

static void col_uid(sqlite3_stmt *stmt, int col, char out[MC_UID_STRLEN]) {
  const char *v = (const char *)sqlite3_column_text(stmt, col);
  snprintf(out, MC_UID_STRLEN, "%s", v && strlen(v) < MC_UID_STRLEN ? v : "");
}

/* The first column of the first row of sql (one text parameter) into out; 1 if found. */
static int lookup_uid(sqlite3 *db, const char *sql, const char *arg, char out[MC_UID_STRLEN]) {
  out[0] = '\0';
  sqlite3_stmt *stmt;
  if (!arg || !*arg || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(stmt, 1, arg, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) col_uid(stmt, 0, out);
  sqlite3_finalize(stmt);
  return out[0] != '\0';
}

static int count_rows(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt;
  int n = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static mc_status exec_sql(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? MC_OK : MC_ERR_DB;
}

static const char *extension_of(const char *path) {
  const char *slash = strrchr(path, '/'), *dot = strrchr(path, '.');
  return dot && (!slash || dot > slash) ? dot + 1 : "";
}

/* --- Tracks --- */

typedef struct merge_ctx {
  mc_library *lib;
  sqlite3 *db;  /* lib's */
  sqlite3 *old; /* the set-aside copy, read-only */
  const mc_kept_file *kept;
  int nkept;
  uid_map tracks, albums, playlists, groups, smart;
  /* Tracks given the copy's added_ms afterwards: new uid, then the time. */
  uid_map added; /* to holds the time as text, reusing the map's storage */
  mc_merge_result *r;
} merge_ctx;

static int kept_cmp(const void *a, const void *b) {
  return strcmp(((const mc_kept_file *)a)->track, ((const mc_kept_file *)b)->track);
}

void mc_libmerge_sort_kept(mc_kept_file *kept, int count) {
  if (kept && count > 1) qsort(kept, (size_t)count, sizeof *kept, kept_cmp);
}

static int remote_quality(void *user, const char *track_uid, mc_audio_quality *out) {
  merge_ctx *x = user;
  if (!x->kept || x->nkept <= 0) return 0;
  mc_kept_file key;
  memset(&key, 0, sizeof key);
  snprintf(key.track, sizeof key.track, "%s", track_uid);
  const mc_kept_file *k = bsearch(&key, x->kept, (size_t)x->nkept, sizeof *x->kept, kept_cmp);
  if (!k) return 0;
  memset(out, 0, sizeof *out);
  out->format = k->format;
  out->bitrate_kbps = k->kbps;
  return 1;
}

/* One track of the copy. */
typedef struct old_track {
  char uid[MC_UID_STRLEN];
  char *title, *genre, *mb_trackid, *mb_releasetrackid, *mb_albumid;
  char *artist, *artist_sort, *artist_mbid;
  char *path, *format, *abs;
  int track_no, disc_no, duration_ms, year, kbps;
  int64_t added_ms; /* 0 if unknown */
  int64_t size, mtime;
  int held; /* its file is in the library folder */
} old_track;

static void old_tracks_free(old_track *t, int n) {
  for (int i = 0; i < n; i++) {
    free(t[i].title);
    free(t[i].genre);
    free(t[i].mb_trackid);
    free(t[i].mb_releasetrackid);
    free(t[i].mb_albumid);
    free(t[i].artist);
    free(t[i].artist_sort);
    free(t[i].artist_mbid);
    free(t[i].path);
    free(t[i].format);
    free(t[i].abs);
  }
  free(t);
}

/* An album of the copy (or a lone track with no album, as a single of its artist). */
typedef struct old_album {
  char uid[MC_UID_STRLEN]; /* "" for a lone track */
  char *title, *kind, *mb_albumid, *mb_releasegroupid, *art_hash;
  char *artist, *artist_sort, *artist_mbid;
  int year, original_year, disc_total;
} old_album;

static void old_album_clear(old_album *a) {
  free(a->title);
  free(a->kind);
  free(a->mb_albumid);
  free(a->mb_releasegroupid);
  free(a->art_hash);
  free(a->artist);
  free(a->artist_sort);
  free(a->artist_mbid);
  memset(a, 0, sizeof *a);
}

/* Track columns, then the best file this device had of it: an original imported here, then
 * one received, then a transcode. */
#define OLD_TRACK_SQL \
  "SELECT t.uid, t.title, t.track_no, t.disc_no, t.duration_ms, t.genre, t.mb_trackid," \
  " t.mb_releasetrackid, t.year, t.mb_albumid, t.added_ms, ar.name, ar.sort_name," \
  " ar.mb_artistid, f.path, f.format, f.bitrate_kbps" \
  " FROM tracks t LEFT JOIN artists ar ON ar.uid = t.artist_uid" \
  " LEFT JOIN files f ON f.uid = (SELECT g.uid FROM files g WHERE g.track_uid = t.uid" \
  "  ORDER BY g.source_file_uid IS NOT NULL, g.source_peer_uid IS NOT NULL, g.uid LIMIT 1)"

static mc_status read_tracks(merge_ctx *x, sqlite3_stmt *stmt, old_track **out, int *count) {
  *out = NULL;
  *count = 0;
  old_track *t = NULL;
  int n = 0, cap = 0, rc;
  mc_status st = MC_OK;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if (n == cap) {
      int grown = cap ? cap * 2 : 16;
      old_track *more = realloc(t, (size_t)grown * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      t = more;
      cap = grown;
    }
    old_track *o = &t[n++];
    memset(o, 0, sizeof *o);
    col_uid(stmt, 0, o->uid);
    o->title = col_dup(stmt, 1);
    o->track_no = sqlite3_column_int(stmt, 2);
    o->disc_no = sqlite3_column_int(stmt, 3);
    o->duration_ms = sqlite3_column_int(stmt, 4);
    o->genre = col_dup(stmt, 5);
    o->mb_trackid = col_dup(stmt, 6);
    o->mb_releasetrackid = col_dup(stmt, 7);
    o->year = sqlite3_column_int(stmt, 8);
    o->mb_albumid = col_dup(stmt, 9);
    o->added_ms = sqlite3_column_int64(stmt, 10);
    o->artist = col_dup(stmt, 11);
    o->artist_sort = col_dup(stmt, 12);
    o->artist_mbid = col_dup(stmt, 13);
    o->path = col_dup(stmt, 14);
    o->format = col_dup(stmt, 15);
    o->kbps = sqlite3_column_int(stmt, 16);
    if (o->path) {
      o->abs = mc_library_abspath(x->lib, o->path);
      o->held = o->abs && !mc_is_link(o->abs) &&
                mc_file_info(o->abs, &o->size, &o->mtime) == MC_OK;
    }
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  if (st != MC_OK) {
    old_tracks_free(t, n);
    return st;
  }
  *out = t;
  *count = n;
  return MC_OK;
}

/* The kept library's album of track uid, into out. */
static int album_of(merge_ctx *x, const char *track, char out[MC_UID_STRLEN]) {
  return lookup_uid(x->db, "SELECT album_uid FROM tracks WHERE uid = ?1", track, out);
}

/* A track of the copy with no file here: only its id can tie it to the kept library's. */
static void map_unheld(merge_ctx *x, const old_track *o) {
  char uid[MC_UID_STRLEN];
  if (lookup_uid(x->db, "SELECT uid FROM tracks WHERE mb_releasetrackid = ?1 ORDER BY uid",
                 o->mb_releasetrackid, uid)) {
    map_put(&x->tracks, o->uid, uid);
    x->r->tracks_mapped++;
  } else {
    x->r->tracks_left_out++;
  }
}

static void merge_album(merge_ctx *x, const old_album *a, old_track *t, int n) {
  int held = 0;
  for (int i = 0; i < n; i++) held += t[i].held;
  char album_uid[MC_UID_STRLEN] = "";
  if (held > 0) {
    mc_db_track *rows = calloc((size_t)held, sizeof *rows);
    mc_dupe_in *in = calloc((size_t)held, sizeof *in);
    old_track **src = calloc((size_t)held, sizeof *src);
    if (!rows || !in || !src) {
      x->r->tracks_failed += held;
      free(rows);
      free(in);
      free(src);
      return;
    }
    mc_db_album al;
    memset(&al, 0, sizeof al);
    al.singles = a->kind && !strcmp(a->kind, "singles");
    al.title = a->title;
    al.artist = (mc_db_artist){a->artist, a->artist_sort, a->artist_mbid};
    al.year = a->year;
    al.original_year = a->original_year;
    al.disc_total = a->disc_total;
    al.mb_albumid = a->mb_albumid;
    al.mb_releasegroupid = a->mb_releasegroupid;
    al.art_hash = a->art_hash; /* its cover is still in the folder */

    int k = 0;
    for (int i = 0; i < n; i++) {
      old_track *o = &t[i];
      if (!o->held) continue;
      mc_db_track *row = &rows[k];
      row->path = o->path;
      row->format = o->format ? o->format : extension_of(o->path);
      row->size_bytes = o->size;
      row->mtime_ms = o->mtime;
      row->bitrate_kbps = o->kbps;
      row->title = o->title;
      row->artist = (mc_db_artist){o->artist, o->artist_sort, o->artist_mbid};
      row->track_no = o->track_no;
      row->disc_no = o->disc_no;
      row->duration_ms = o->duration_ms;
      row->year = o->year;
      row->genre = o->genre;
      row->mb_trackid = o->mb_trackid;
      row->mb_releasetrackid = o->mb_releasetrackid;
      row->mb_albumid = o->mb_albumid;
      in[k].row = row;
      in[k].src_path = o->abs;
      in[k].size = o->size;
      in[k].quality = (mc_audio_quality){row->format, o->kbps, 0, 0};
      mc_tags tags;
      if (mc_tags_read(o->abs, &tags) == MC_OK) {
        if (tags.bitrate_kbps) in[k].quality.bitrate_kbps = tags.bitrate_kbps;
        in[k].quality.sample_rate = tags.sample_rate;
        in[k].quality.bits_per_sample = tags.bits_per_sample;
        mc_tags_clear(&tags);
      }
      src[k] = o;
      k++;
    }

    mc_dupe_remote remote = {remote_quality, x};
    mc_dupe_plan plan;
    int planned = mc_dupes_plan_remote(x->lib, &al, in, held, &remote, &plan) == MC_OK;
    if (planned && plan.album_uid[0]) {
      al.existing_uid = plan.album_uid;
      snprintf(album_uid, sizeof album_uid, "%s", plan.album_uid);
    }
    /* Keep the rows to write; skipped files go to the trash. */
    int w = 0;
    for (int i = 0; i < held; i++) {
      mc_dupe_decision *d = planned ? &plan.d[i] : NULL;
      mc_dupe_action act = d ? d->action : MC_DUPE_IMPORT;
      if (act == MC_DUPE_SKIP) {
        if (d->track_uid[0]) map_put(&x->tracks, src[i]->uid, d->track_uid);
        if (mc_dupes_trash(src[i]->abs) != MC_OK)
          fprintf(stderr, "musiccore: merge: couldn't move %s to the trash; left in place\n",
                  src[i]->abs);
        x->r->tracks_skipped++;
        continue;
      }
      mc_db_track row = rows[i];
      if (act != MC_DUPE_IMPORT) row.track_uid = d->track_uid; /* ATTACH, or a SWAP: attach */
      rows[w] = row;
      src[w] = src[i];
      w++;
    }
    if (w > 0 && mc_import_db_write(x->lib, &al, rows, w) != MC_OK) {
      x->r->tracks_failed += w;
      w = 0;
    }
    for (int i = 0; i < w; i++) {
      char uid[MC_UID_STRLEN] = "";
      if (rows[i].track_uid) {
        snprintf(uid, sizeof uid, "%s", rows[i].track_uid);
        /* Whether the kept copy was known to be worse: find the decision again. */
        int better = 0;
        for (int j = 0; planned && j < held; j++)
          if (!strcmp(plan.d[j].track_uid, uid) && plan.d[j].remote) better = 1;
        if (better)
          x->r->tracks_better++;
        else
          x->r->tracks_attached++;
      } else if (lookup_uid(x->db, "SELECT track_uid FROM files WHERE path = ?1", rows[i].path,
                            uid)) {
        x->r->tracks_added++;
        if (src[i]->added_ms > 0) {
          char ms[MC_UID_STRLEN];
          snprintf(ms, sizeof ms, "%lld", (long long)src[i]->added_ms);
          map_put(&x->added, uid, ms);
        }
      }
      if (uid[0]) {
        map_put(&x->tracks, src[i]->uid, uid);
        if (!album_uid[0]) album_of(x, uid, album_uid);
      }
    }
    if (planned) mc_dupe_plan_free(&plan);
    free(rows);
    free(in);
    free(src);
  }
  for (int i = 0; i < n; i++)
    if (!t[i].held) {
      map_unheld(x, &t[i]);
      const char *to = map_get(&x->tracks, t[i].uid);
      if (to && !album_uid[0]) album_of(x, to, album_uid);
    }
  if (a->uid[0] && album_uid[0]) map_put(&x->albums, a->uid, album_uid);
}

static void emit(mc_libmerge_progress_fn fn, void *user, mc_merge_phase phase, int done,
                 int total) {
  if (!fn) return;
  mc_merge_progress p = {phase, done, total};
  fn(user, &p);
}

static mc_status merge_tracks(merge_ctx *x, mc_libmerge_progress_fn fn, void *user) {
  int total = count_rows(x->old, "SELECT count(*) FROM albums") +
              count_rows(x->old, "SELECT count(*) FROM tracks WHERE album_uid IS NULL"
                                 " OR album_uid NOT IN (SELECT uid FROM albums)");
  int done = 0;
  emit(fn, user, MC_MERGE_PHASE_TRACKS, 0, total);
  sqlite3_stmt *albums, *tracks;
  if (sqlite3_prepare_v2(x->old,
                         "SELECT a.uid, a.title, a.kind, a.year, a.original_year, a.disc_total,"
                         " a.mb_albumid, a.mb_releasegroupid, a.art_hash, ar.name, ar.sort_name,"
                         " ar.mb_artistid FROM albums a LEFT JOIN artists ar ON ar.uid = a.artist_uid"
                         " ORDER BY a.uid",
                         -1, &albums, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  if (sqlite3_prepare_v2(x->old,
                         OLD_TRACK_SQL " WHERE t.album_uid = ?1"
                                       " ORDER BY t.disc_no, t.track_no, t.uid",
                         -1, &tracks, NULL) != SQLITE_OK) {
    sqlite3_finalize(albums);
    return MC_ERR_DB;
  }
  mc_status st = MC_OK;
  int rc;
  while (st == MC_OK && (rc = sqlite3_step(albums)) == SQLITE_ROW) {
    old_album a;
    memset(&a, 0, sizeof a);
    col_uid(albums, 0, a.uid);
    a.title = col_dup(albums, 1);
    a.kind = col_dup(albums, 2);
    a.year = sqlite3_column_int(albums, 3);
    a.original_year = sqlite3_column_int(albums, 4);
    a.disc_total = sqlite3_column_int(albums, 5);
    a.mb_albumid = col_dup(albums, 6);
    a.mb_releasegroupid = col_dup(albums, 7);
    a.art_hash = col_dup(albums, 8);
    a.artist = col_dup(albums, 9);
    a.artist_sort = col_dup(albums, 10);
    a.artist_mbid = col_dup(albums, 11);
    sqlite3_reset(tracks);
    sqlite3_bind_text(tracks, 1, a.uid, -1, SQLITE_STATIC);
    old_track *t = NULL;
    int n = 0;
    st = read_tracks(x, tracks, &t, &n);
    if (st == MC_OK && n > 0) merge_album(x, &a, t, n);
    old_tracks_free(t, n);
    old_album_clear(&a);
    emit(fn, user, MC_MERGE_PHASE_TRACKS, ++done, total);
  }
  sqlite3_finalize(tracks);
  sqlite3_finalize(albums);
  if (st != MC_OK) return st;

  /* Tracks with no album: each a single of its artist, as an import of a lone file. */
  if (sqlite3_prepare_v2(x->old,
                         OLD_TRACK_SQL " WHERE t.album_uid IS NULL"
                                       " OR t.album_uid NOT IN (SELECT uid FROM albums)"
                                       " ORDER BY t.uid",
                         -1, &tracks, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  old_track *t = NULL;
  int n = 0;
  st = read_tracks(x, tracks, &t, &n);
  sqlite3_finalize(tracks);
  for (int i = 0; st == MC_OK && i < n; i++) {
    old_album a;
    memset(&a, 0, sizeof a);
    a.kind = mc_strdup("singles");
    a.artist = t[i].artist ? mc_strdup(t[i].artist) : NULL;
    a.artist_sort = t[i].artist_sort ? mc_strdup(t[i].artist_sort) : NULL;
    a.artist_mbid = t[i].artist_mbid ? mc_strdup(t[i].artist_mbid) : NULL;
    merge_album(x, &a, &t[i], 1);
    old_album_clear(&a);
    emit(fn, user, MC_MERGE_PHASE_TRACKS, ++done, total);
  }
  old_tracks_free(t, n);
  return st;
}

/* --- Lists, plays and recents --- */

/* name, or "name (2)", "name (3)" ... if table already has a row by that name (ignoring
 * case). malloc'd; *renamed set when it changed. */
static char *unique_name(sqlite3 *db, const char *table, const char *name, int *renamed) {
  *renamed = 0;
  char sql[128];
  snprintf(sql, sizeof sql, "SELECT 1 FROM %s WHERE name = ?1 COLLATE NOCASE", table);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return mc_strdup(name);
  size_t cap = strlen(name) + 16;
  char *out = malloc(cap);
  for (int n = 1; out && n < 10000; n++) {
    if (n == 1)
      snprintf(out, cap, "%s", name);
    else
      snprintf(out, cap, "%s (%d)", name, n);
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, out, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
      *renamed = n > 1;
      break;
    }
  }
  sqlite3_finalize(stmt);
  return out;
}

/* Playlists or album groups, with their entries pointed at the kept library's tracks or
 * albums (entries whose track or album didn't come along are left out). */
static mc_status merge_lists(merge_ctx *x, int groups) {
  const char *list_sql = groups ? "SELECT uid, name FROM album_groups ORDER BY name, uid"
                                : "SELECT uid, name FROM playlists ORDER BY name, uid";
  const char *entry_sql = groups ? "SELECT album_uid, position FROM album_group_entries"
                                   " WHERE group_uid = ?1 ORDER BY position, uid"
                                 : "SELECT track_uid, position FROM playlist_entries"
                                   " WHERE playlist_uid = ?1 ORDER BY position, uid";
  const char *ins_list = groups ? "INSERT INTO album_groups (uid, name) VALUES (?1, ?2)"
                                : "INSERT INTO playlists (uid, name) VALUES (?1, ?2)";
  const char *ins_entry = groups ? "INSERT INTO album_group_entries (uid, group_uid, album_uid,"
                                   " position) VALUES (?1, ?2, ?3, ?4)"
                                 : "INSERT INTO playlist_entries (uid, playlist_uid, track_uid,"
                                   " position) VALUES (?1, ?2, ?3, ?4)";
  sqlite3_stmt *lists = NULL, *entries = NULL, *il = NULL, *ie = NULL;
  mc_status st = MC_ERR_DB;
  if (sqlite3_prepare_v2(x->old, list_sql, -1, &lists, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(x->old, entry_sql, -1, &entries, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(x->db, ins_list, -1, &il, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(x->db, ins_entry, -1, &ie, NULL) != SQLITE_OK)
    goto done;
  st = MC_OK;
  uid_map *refs = groups ? &x->albums : &x->tracks;
  while (st == MC_OK && sqlite3_step(lists) == SQLITE_ROW) {
    char old_uid[MC_UID_STRLEN], uid[MC_UID_STRLEN];
    col_uid(lists, 0, old_uid);
    const char *name = (const char *)sqlite3_column_text(lists, 1);
    int renamed = 0;
    char *fresh = unique_name(x->db, groups ? "album_groups" : "playlists", name ? name : "",
                              &renamed);
    if (!fresh || (st = mc_uid_new(uid)) != MC_OK) {
      free(fresh);
      st = st == MC_OK ? MC_ERR_NOMEM : st;
      break;
    }
    sqlite3_reset(il);
    sqlite3_bind_text(il, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_text(il, 2, fresh, -1, SQLITE_STATIC);
    if (sqlite3_step(il) != SQLITE_DONE) st = MC_ERR_DB;
    free(fresh);
    if (st != MC_OK) break;
    map_put(groups ? &x->groups : &x->playlists, old_uid, uid);
    if (groups)
      x->r->album_groups++;
    else
      x->r->playlists++;
    x->r->playlists_renamed += renamed;

    sqlite3_reset(entries);
    sqlite3_bind_text(entries, 1, old_uid, -1, SQLITE_STATIC);
    while (st == MC_OK && sqlite3_step(entries) == SQLITE_ROW) {
      const char *to = map_get(refs, (const char *)sqlite3_column_text(entries, 0));
      const char *pos = (const char *)sqlite3_column_text(entries, 1);
      char entry[MC_UID_STRLEN];
      if (!to || !pos) continue;
      if ((st = mc_uid_new(entry)) != MC_OK) break;
      sqlite3_reset(ie);
      sqlite3_bind_text(ie, 1, entry, -1, SQLITE_STATIC);
      sqlite3_bind_text(ie, 2, uid, -1, SQLITE_STATIC);
      sqlite3_bind_text(ie, 3, to, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ie, 4, pos, -1, SQLITE_TRANSIENT);
      if (sqlite3_step(ie) != SQLITE_DONE) st = MC_ERR_DB;
    }
  }
done:
  sqlite3_finalize(lists);
  sqlite3_finalize(entries);
  sqlite3_finalize(il);
  sqlite3_finalize(ie);
  return st;
}

/* s with every occurrence of from replaced by to (same length: uids), in place. */
static void replace_uid(char *s, const char *from, const char *to) {
  size_t n = strlen(from);
  if (n != strlen(to) || !n) return;
  for (char *p = strstr(s, from); p; p = strstr(p + n, from)) memcpy(p, to, n);
}

/* Smart lists the user made (not the examples every library has), with the playlists and
 * groups their rules name pointed at the carried-over ones. */
static mc_status merge_smart(merge_ctx *x) {
  sqlite3_stmt *lists = NULL, *ins = NULL;
  mc_status st = MC_ERR_DB;
  if (sqlite3_prepare_v2(x->old,
                         "SELECT uid, name, subject, rule FROM smart_lists"
                         " WHERE uid NOT LIKE '" EXAMPLE_PREFIX "%' ORDER BY name, uid",
                         -1, &lists, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(x->db,
                         "INSERT INTO smart_lists (uid, name, subject, rule) VALUES (?1, ?2, ?3, ?4)",
                         -1, &ins, NULL) != SQLITE_OK)
    goto done;
  st = MC_OK;
  while (st == MC_OK && sqlite3_step(lists) == SQLITE_ROW) {
    char old_uid[MC_UID_STRLEN], uid[MC_UID_STRLEN];
    col_uid(lists, 0, old_uid);
    const char *name = (const char *)sqlite3_column_text(lists, 1);
    const char *subject = (const char *)sqlite3_column_text(lists, 2);
    char *rule = col_dup(lists, 3);
    int renamed = 0;
    char *fresh = unique_name(x->db, "smart_lists", name ? name : "", &renamed);
    if (!rule || !fresh || !subject || (st = mc_uid_new(uid)) != MC_OK) {
      free(rule);
      free(fresh);
      continue;
    }
    for (int i = 0; i < x->playlists.n; i++)
      replace_uid(rule, x->playlists.v[i].from, x->playlists.v[i].to);
    for (int i = 0; i < x->groups.n; i++)
      replace_uid(rule, x->groups.v[i].from, x->groups.v[i].to);
    sqlite3_reset(ins);
    sqlite3_bind_text(ins, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, fresh, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, subject, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 4, rule, -1, SQLITE_STATIC);
    if (sqlite3_step(ins) != SQLITE_DONE) st = MC_ERR_DB;
    free(rule);
    free(fresh);
    if (st != MC_OK) break;
    map_put(&x->smart, old_uid, uid);
    x->r->smart_lists++;
    x->r->playlists_renamed += renamed;
  }
done:
  sqlite3_finalize(lists);
  sqlite3_finalize(ins);
  return st;
}

/* Play counts and last plays, summed over the copy's devices, onto this device's row of
 * each kept track. */
static mc_status merge_plays(merge_ctx *x) {
  sqlite3_stmt *plays = NULL, *ins = NULL;
  mc_status st = MC_ERR_DB;
  if (sqlite3_prepare_v2(x->old,
                         "SELECT track_uid, sum(play_count), max(last_played_ms) FROM track_plays"
                         " GROUP BY track_uid",
                         -1, &plays, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(x->db,
                         "INSERT INTO track_plays (uid, track_uid, node_uid, play_count,"
                         " last_played_ms) VALUES (?1, ?2, ?3, ?4, ?5)"
                         " ON CONFLICT (track_uid, node_uid) DO UPDATE SET"
                         " play_count = play_count + excluded.play_count,"
                         " last_played_ms = CASE WHEN excluded.last_played_ms >"
                         "  coalesce(last_played_ms, 0) THEN excluded.last_played_ms"
                         "  ELSE last_played_ms END",
                         -1, &ins, NULL) != SQLITE_OK)
    goto done;
  st = MC_OK;
  const char *node = mc_library_node_uid(x->lib);
  while (st == MC_OK && sqlite3_step(plays) == SQLITE_ROW) {
    const char *to = map_get(&x->tracks, (const char *)sqlite3_column_text(plays, 0));
    int64_t count = sqlite3_column_int64(plays, 1);
    int has_last = sqlite3_column_type(plays, 2) != SQLITE_NULL;
    if (!to || (count <= 0 && !has_last)) continue;
    char uid[MC_UID_STRLEN];
    if ((st = mc_uid_new(uid)) != MC_OK) break;
    sqlite3_reset(ins);
    sqlite3_bind_text(ins, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, to, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, node, -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 4, count > 0 ? count : 0);
    if (has_last)
      sqlite3_bind_int64(ins, 5, sqlite3_column_int64(plays, 2));
    else
      sqlite3_bind_null(ins, 5);
    if (sqlite3_step(ins) != SQLITE_DONE) st = MC_ERR_DB;
    else x->r->plays++;
  }
done:
  sqlite3_finalize(plays);
  sqlite3_finalize(ins);
  return st;
}

/* The kept library's artist for one of the copy's: by MusicBrainz id, then by name. */
static int map_artist(merge_ctx *x, const char *old_uid, char out[MC_UID_STRLEN]) {
  out[0] = '\0';
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(x->old, "SELECT name, mb_artistid FROM artists WHERE uid = ?1", -1,
                         &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, old_uid, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    const char *mbid = (const char *)sqlite3_column_text(stmt, 1);
    if (!lookup_uid(x->db, "SELECT uid FROM artists WHERE mb_artistid = ?1 ORDER BY uid", mbid,
                    out))
      lookup_uid(x->db, "SELECT uid FROM artists WHERE name = ?1 COLLATE NOCASE ORDER BY uid",
                 name, out);
  }
  sqlite3_finalize(stmt);
  return out[0] != '\0';
}

static mc_status merge_recents(merge_ctx *x) {
  sqlite3_stmt *rec = NULL, *ins = NULL;
  mc_status st = MC_ERR_DB;
  if (sqlite3_prepare_v2(x->old, "SELECT uid, kind, opened_ms FROM recents", -1, &rec, NULL) !=
        SQLITE_OK ||
      sqlite3_prepare_v2(x->db,
                         "INSERT INTO recents (uid, kind, opened_ms) VALUES (?1, ?2, ?3)"
                         " ON CONFLICT (uid) DO UPDATE SET opened_ms = excluded.opened_ms"
                         " WHERE excluded.opened_ms > recents.opened_ms",
                         -1, &ins, NULL) != SQLITE_OK)
    goto done;
  st = MC_OK;
  while (st == MC_OK && sqlite3_step(rec) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(rec, 0);
    const char *kind = (const char *)sqlite3_column_text(rec, 1);
    if (!uid || !kind) continue;
    char artist[MC_UID_STRLEN];
    const char *to = NULL;
    if (!strcmp(kind, "album")) {
      to = map_get(&x->albums, uid);
    } else if (!strcmp(kind, "artist")) {
      to = map_artist(x, uid, artist) ? artist : NULL;
    } else if (!strcmp(kind, "playlist") || !strcmp(kind, "album_group")) {
      to = map_get(!strcmp(kind, "playlist") ? &x->playlists : &x->groups, uid);
      if (!to) to = map_get(&x->smart, uid);
      if (!to && !strncmp(uid, EXAMPLE_PREFIX, strlen(EXAMPLE_PREFIX))) to = uid;
    }
    if (!to) continue;
    sqlite3_reset(ins);
    sqlite3_bind_text(ins, 1, to, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 3, sqlite3_column_int64(rec, 2));
    if (sqlite3_step(ins) != SQLITE_DONE) st = MC_ERR_DB;
    else x->r->recents++;
  }
done:
  sqlite3_finalize(rec);
  sqlite3_finalize(ins);
  return st;
}

/* New tracks keep the date they were first added to the copy. */
static mc_status restore_added(merge_ctx *x) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(x->db, "UPDATE tracks SET added_ms = ?2 WHERE uid = ?1", -1, &stmt,
                         NULL) != SQLITE_OK)
    return MC_ERR_DB;
  mc_status st = MC_OK;
  for (int i = 0; i < x->added.n && st == MC_OK; i++) {
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, x->added.v[i].from, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, strtoll(x->added.v[i].to, NULL, 10));
    if (sqlite3_step(stmt) != SQLITE_DONE) st = MC_ERR_DB;
  }
  sqlite3_finalize(stmt);
  return st;
}

static mc_status merge_rest(merge_ctx *x, mc_libmerge_progress_fn fn, void *user) {
  emit(fn, user, MC_MERGE_PHASE_LISTS, 0, 1);
  mc_status st = mc_library_begin_write(x->lib);
  if (st != MC_OK) return st;
  if (st == MC_OK) st = merge_lists(x, 0);
  if (st == MC_OK) st = merge_lists(x, 1);
  if (st == MC_OK) st = merge_smart(x);
  if (st == MC_OK) st = merge_plays(x);
  if (st == MC_OK) st = merge_recents(x);
  if (st == MC_OK) st = restore_added(x);
  if (st == MC_OK) st = mc_library_commit(x->lib);
  if (st != MC_OK) {
    fprintf(stderr, "musiccore: merge: lists failed: %s\n", sqlite3_errmsg(x->db));
    mc_library_rollback(x->lib);
  }
  emit(fn, user, MC_MERGE_PHASE_LISTS, 1, 1);
  return st;
}

mc_status mc_libmerge_run(mc_library *lib, const mc_kept_file *kept, int nkept,
                          mc_libmerge_progress_fn progress, void *user, mc_merge_result *out) {
  mc_merge_result scratch;
  mc_merge_result *r = out ? out : &scratch;
  memset(r, 0, sizeof *r);
  r->status = MC_ERR_NOT_FOUND;
  if (!lib) return r->status = MC_ERR_INVALID_ARG;
  char *backup = NULL;
  if (!mc_libmerge_pending(lib, &backup)) return r->status;
  merge_ctx x;
  memset(&x, 0, sizeof x);
  x.lib = lib;
  x.db = mc_library_db(lib);
  x.kept = kept;
  x.nkept = kept ? nkept : 0;
  x.r = r;
  mc_status st = MC_OK;
  if (!backup || sqlite3_open_v2(backup, &x.old, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                                 NULL) != SQLITE_OK) {
    fprintf(stderr, "musiccore: merge: can't open %s\n", backup ? backup : "the set-aside copy");
    st = MC_ERR_IO;
  }
  if (st == MC_OK) st = merge_tracks(&x, progress, user);
  if (st == MC_OK) st = merge_rest(&x, progress, user);
  /* Done, even if some tracks failed: their files stay in the folder, out of the library. */
  if (st == MC_OK) st = mc_library_meta_set(lib, MC_LIBMERGE_META, NULL);
  if (st == MC_OK) exec_sql(x.db, "PRAGMA wal_checkpoint(PASSIVE)");
  sqlite3_close_v2(x.old);
  free(x.tracks.v);
  free(x.albums.v);
  free(x.playlists.v);
  free(x.groups.v);
  free(x.smart.v);
  free(x.added.v);
  free(backup);
  return r->status = st;
}
