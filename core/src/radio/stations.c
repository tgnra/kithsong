/* See musiccore/radio_stations.h, and "Radio stations" in docs/schema.md for the reasoning.
 *
 * There's a station for each group of radio/groups.h. Its seed starts as the group's centre, and
 * gives way to a track close by that makes a better one: one pass over the library per seed. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* localtime_r */
#endif

#include "musiccore/radio_stations.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "analysis/analysis_internal.h"
#include "library/library_internal.h"
#include "musiccore/analysis.h"
#include "musiccore/i18n.h"
#include "radio/groups.h"
#include "util/platform.h"

/* The same limits as a mix's own, so a seed chosen here is one mc_radio_make() takes. */
#define MIN_TRACK_MS 60000
#define MAX_TRACK_MS 480000

/* A play shorter than this was more likely skipped than counted, which the log can't say. Only
 * for telling what today added to a track's plays. */
#define COUNTED_MS 30000

#define LOAD_CHECK 256 /* rows read between asking whether to give up */


typedef struct track {
  char uid[MC_UID_STRLEN];
  char artist[MC_UID_STRLEN]; /* "" if unknown */
  int64_t plays;              /* up to the start of the day */
  int has_art;
  unsigned hash; /* of the uid and the day: the order when nothing else decides */
} track;

typedef struct pool {
  track *items;
  mc_analysis *results; /* parallel to items, as mc_analysis_stats_from() wants them */
  unsigned *lasting;    /* parallel too: hashes of the uids alone, for mc_radio_groups_find() */
  int count, cap;
} pool;

static void pool_free(pool *p) {
  free(p->items);
  free(p->results);
  free(p->lasting);
}

static mc_status pool_grow(pool *p) {
  if (p->count < p->cap) return MC_OK;
  int cap = p->cap ? p->cap * 2 : 1024;
  track *items = realloc(p->items, (size_t)cap * sizeof *items);
  if (!items) return MC_ERR_NOMEM;
  p->items = items;
  mc_analysis *results = realloc(p->results, (size_t)cap * sizeof *results);
  if (!results) return MC_ERR_NOMEM;
  p->results = results;
  unsigned *lasting = realloc(p->lasting, (size_t)cap * sizeof *lasting);
  if (!lasting) return MC_ERR_NOMEM;
  p->lasting = lasting;
  p->cap = cap;
  return MC_OK;
}

/* FNV-1a, as home_picks.c's: sorting by it is a shuffle that stays put for one day. */
static unsigned uid_hash(const char *uid, unsigned seed) {
  unsigned h = 2166136261u ^ seed;
  for (; *uid; uid++) h = (h ^ (unsigned char)*uid) * 16777619u;
  return h ^ h >> 15;
}

/* The start of the local day now_ms is in. Local, so it doesn't turn over in the middle of an
 * evening's listening somewhere west of Greenwich. */
static int64_t day_start_ms(int64_t now_ms) {
  time_t t = (time_t)(now_ms / 1000);
  struct tm tm;
#ifdef _WIN32
  int ok = localtime_s(&tm, &t) == 0;
#else
  int ok = localtime_r(&t, &tm) != NULL;
#endif
  if (ok) {
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t start = mktime(&tm);
    if (start != (time_t)-1) return (int64_t)start * 1000;
  }
  return now_ms - now_ms % (24 * 60 * 60 * 1000LL);
}

static double column_real(sqlite3_stmt *stmt, int col) {
  return sqlite3_column_type(stmt, col) == SQLITE_NULL ? NAN : sqlite3_column_double(stmt, col);
}

/* Every track that may seed a station, in one query, each track's rows best first as
 * mc_analysis_get() has them. Plays are as of the start of the day, so playing doesn't move the
 * stations while they're showing. */
static mc_status pool_load(mc_library *lib, int64_t day_ms, mc_analysis_cancel_fn cancel, void *user, pool *p) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT t.uid, a.source, a.analysis_version, a.features, a.lufs, a.lra,"
                         " a.true_peak, coalesce(t.artist_uid, al.artist_uid),"
                         " coalesce(al.art_hash IS NOT NULL, 0),"
                         " coalesce((SELECT sum(tp.play_count) FROM track_plays tp"
                         "  WHERE tp.track_uid = t.uid), 0) -"
                         " (SELECT count(*) FROM play_events e WHERE e.track_uid = t.uid"
                         "  AND e.started_ms >= ?1 AND e.listened_ms >= " MC_SQL_NUM(COUNTED_MS) ")"
                         " FROM tracks t JOIN track_analysis a ON a.track_uid = t.uid"
                         " LEFT JOIN albums al ON al.uid = t.album_uid"
                         " WHERE EXISTS (SELECT 1 FROM files f WHERE f.track_uid = t.uid)"
                         " AND NOT " TRACK_IN_MIX
                         " AND coalesce(t.duration_ms, " MC_SQL_NUM(MIN_TRACK_MS) ") BETWEEN "
                         MC_SQL_NUM(MIN_TRACK_MS) " AND " MC_SQL_NUM(MAX_TRACK_MS)
                         " ORDER BY t.uid, (a.file_rev IS t.file_rev) DESC, a.source,"
                         " a.analysis_version DESC, a.uid",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_int64(stmt, 1, day_ms);
  unsigned day = (unsigned)(day_ms / 1000);
  char last[MC_UID_STRLEN] = "";
  mc_status st = MC_OK;
  int rc;
  for (unsigned rows = 1; st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW; rows++) {
    if (cancel && rows % LOAD_CHECK == 0 && cancel(user)) {
      st = MC_ERR_IN_USE;
      break;
    }
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    if (!uid || strlen(uid) >= MC_UID_STRLEN || !strcmp(uid, last)) continue;
    mc_analysis result;
    memset(&result, 0, sizeof result);
    if (!mc_analysis_unpack(sqlite3_column_blob(stmt, 3), sqlite3_column_bytes(stmt, 3), &result))
      continue; /* a layout this build can't read: the track's next row may do */
    strcpy(last, uid);
    result.version = sqlite3_column_int(stmt, 2);
    if (result.version != MC_ANALYSIS_VERSION) continue; /* only results of this version compare */
    result.source = sqlite3_column_int(stmt, 1) ? MC_ANALYSIS_LOSSY_COPY : MC_ANALYSIS_ORIGINAL;
    result.lufs = column_real(stmt, 4);
    result.lra = column_real(stmt, 5);
    result.true_peak = column_real(stmt, 6);
    if ((st = pool_grow(p)) != MC_OK) break;
    track *t = &p->items[p->count];
    memset(t, 0, sizeof *t);
    strcpy(t->uid, uid);
    const char *artist = (const char *)sqlite3_column_text(stmt, 7);
    if (artist && strlen(artist) < MC_UID_STRLEN) strcpy(t->artist, artist);
    t->has_art = sqlite3_column_int(stmt, 8);
    t->plays = sqlite3_column_int64(stmt, 9);
    t->hash = uid_hash(uid, day);
    p->lasting[p->count] = uid_hash(uid, 0);
    p->results[p->count++] = result;
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

/* Doublings of a play count, so that one more play seldom outranks anything. */
static int plays_rank(int64_t plays) {
  int rank = 0;
  for (; plays > 0; plays >>= 1) rank++;
  return rank;
}

/* --- Each station's seed and start, over the whole library --- */

static int artist_is_new(const pool *p, const track *t, const int *seeds, int count, int self) {
  for (int i = 0; i < count; i++)
    if (i != self && !strcmp(p->items[seeds[i]].artist, t->artist)) return 0;
  return 1;
}

/* Whether a would make a better station than b. A name of its own comes first, as that's how
 * stations are told apart, then a cover, and then what's been played: something familiar is the
 * better way in, but not at the price of two stations of one name. */
static int better(const pool *p, int a, int b, const int *seeds, int count, int self) {
  const track *x = &p->items[a], *y = &p->items[b];
  int x_new = artist_is_new(p, x, seeds, count, self), y_new = artist_is_new(p, y, seeds, count, self);
  if (x_new != y_new) return x_new > y_new;
  if (x->has_art != y->has_art) return x->has_art > y->has_art;
  int x_rank = plays_rank(x->plays), y_rank = plays_rank(y->plays);
  if (x_rank != y_rank) return x_rank > y_rank;
  return x->hash < y->hash;
}

/* Each track's group if it's plainly of one, or -1. The caller frees it. NULL if out of memory. */
static signed char *typical_tracks(const pool *p, const mc_radio_groups *groups) {
  signed char *typical = malloc((size_t)p->count);
  for (int i = 0; typical && i < p->count; i++) {
    int group = mc_radio_group_of(groups, &p->results[i]);
    typical[i] = (signed char)(group >= 0 && mc_radio_group_typical(groups, &p->results[i], group) ? group : -1);
  }
  return typical;
}

/* Each seed starts as its group's centre, which stays put from day to day, and moves to a better
 * track near it that's plainly of the group, so what a station is called can change with the day
 * while what it plays doesn't. */
static void settle_seeds(const pool *p, const mc_radio_groups *groups, const signed char *typical, int *seeds, int count) {
  for (int s = 0; s < count; s++) {
    int centre = seeds[s], best = centre;
    for (int i = 0; i < p->count; i++) {
      if (typical[i] != s || i == best || !better(p, i, best, seeds, count, s)) continue;
      if (mc_analysis_distance(&p->results[centre], &p->results[i], &groups->stats) <= MC_RADIO_NEARBY) best = i;
    }
    seeds[s] = best;
  }
}

/* Where group's station starts this time: any track plainly of it, at random, so it isn't the
 * same every time. The seed if there's none, which a group that's all stragglers can come to. */
static int draw_start(const pool *p, const signed char *typical, int group, int seed) {
  int tracks = 0;
  for (int i = 0; i < p->count; i++) tracks += typical[i] == group;
  uint32_t draw;
  if (mc_random_bytes(&draw, sizeof draw) != MC_OK) draw = (uint32_t)mc_mono_ms() * 2654435761u;
  if (!tracks) return seed;
  int left = (int)(draw % (uint32_t)tracks);
  for (int i = 0; i < p->count; i++)
    if (typical[i] == group && !left--) return i;
  return seed;
}

/* The most played first, as what's familiar is the way in, then the day's order. */
static void sort_seeds(const pool *p, int *seeds, int count) {
  for (int i = 1; i < count; i++) {
    int seed = seeds[i], at = i;
    const track *t = &p->items[seed];
    for (; at > 0; at--) {
      const track *before = &p->items[seeds[at - 1]];
      if (before->plays > t->plays || (before->plays == t->plays && before->hash <= t->hash)) break;
      seeds[at] = seeds[at - 1];
    }
    seeds[at] = seed;
  }
}

/* --- What a station says --- */

static void station_free(mc_radio_station_info *st) {
  mc_free(st->name);
  mc_free(st->sub);
  mc_free(st->seed_title);
  mc_free(st->cover_dir);
}

/* Fills in st from its seed, all but sub, and *album with the title of the seed's album. */
static mc_status read_seed(mc_library *lib, const track *t, mc_radio_station_info *st, char **album) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT t.title, ar.name, al.title, al.art_hash IS NOT NULL,"
                         " (SELECT f.path FROM files f WHERE f.track_uid = t.uid LIMIT 1)"
                         " FROM tracks t LEFT JOIN albums al ON al.uid = t.album_uid"
                         " LEFT JOIN artists ar ON ar.uid = coalesce(t.artist_uid, al.artist_uid)"
                         " WHERE t.uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, t->uid, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return MC_ERR_DB;
  }
  const char *title = (const char *)sqlite3_column_text(stmt, 0);
  const char *artist = (const char *)sqlite3_column_text(stmt, 1);
  const char *album_title = (const char *)sqlite3_column_text(stmt, 2);
  const char *file = (const char *)sqlite3_column_text(stmt, 4);
  if (!title || !*title) title = mc_str(MC_STR_UNTITLED);
  memcpy(st->seed_track, t->uid, sizeof st->seed_track);
  memcpy(st->artist_uid, t->artist, sizeof st->artist_uid);
  st->name = mc_format_alloc(mc_str(MC_STR_HOME_STATION), MC_S(artist && *artist ? artist : title));
  st->seed_title = mc_strdup(title);
  *album = album_title ? mc_strdup(album_title) : NULL;
  if (file && sqlite3_column_int(stmt, 3)) { /* the cover sits with the album's files */
    st->cover_dir = mc_library_abspath(lib, file);
    char *sep = st->cover_dir ? strrchr(st->cover_dir, MC_PATH_SEP) : NULL;
    if (sep) *sep = '\0';
  }
  sqlite3_finalize(stmt);
  return st->name && st->seed_title ? MC_OK : MC_ERR_NOMEM;
}

static int same_album(const char *a, const char *b) {
  return !strcmp(a ? a : "", b ? b : "");
}

/* Stations of one name are told apart by what they're like: the seed's title, and its album if
 * that's not enough. One that can't be goes, as two tiles that read the same aren't two stations
 * to anyone looking at them. */
static mc_status finish_stations(mc_radio_station_list *list, char **albums) {
  int kept = 0, count = list->count;
  mc_status st = MC_OK;
  for (int i = 0; i < count; i++) {
    mc_radio_station_info *s = &list->items[i];
    int same_name = 0, same_title = 0, repeat = 0;
    for (int k = 0; k < count; k++) {
      const mc_radio_station_info *other = &list->items[k];
      if (k == i || !other->name || strcmp(other->name, s->name)) continue;
      same_name = 1;
      if (strcmp(other->seed_title, s->seed_title)) continue;
      same_title = 1;
      if (k < i && same_album(albums[k], albums[i])) repeat = 1;
    }
    if (repeat) {
      /* Not freed yet: later stations are still compared with it. */
      s->sub = NULL;
      continue;
    }
    if (same_title && albums[i])
      s->sub = mc_format_alloc(mc_str(MC_STR_HOME_STATION_LIKE_ON), MC_S(s->seed_title), MC_S(albums[i]));
    else if (same_name)
      s->sub = mc_format_alloc(mc_str(MC_STR_HOME_STATION_LIKE), MC_S(s->seed_title));
    else
      s->sub = mc_strdup(mc_str(MC_STR_HOME_STATION_SUB));
    if (!s->sub) st = MC_ERR_NOMEM;
  }
  for (int i = 0; i < count; i++) {
    if (list->items[i].sub) list->items[kept++] = list->items[i];
    else station_free(&list->items[i]);
  }
  list->count = kept;
  return st;
}

/* The library's groups, with pool's results as they stand. Too little analysed leaves it no
 * groups, as it has no stations. */
static mc_status find_groups(const pool *p, mc_radio_groups *groups) {
  mc_analysis_stats stats;
  memset(groups, 0, sizeof *groups);
  if (p->count < MC_RADIO_STATION_MIN_TRACKS) return MC_OK;
  mc_analysis_stats_from(p->results, p->count, &stats);
  return mc_radio_groups_find(p->results, p->lasting, p->count, &stats, groups);
}

mc_status mc_radio_groups_load(mc_library *lib, mc_analysis_cancel_fn cancel, void *user, mc_radio_groups *out) {
  pool p = {0};
  mc_status st = pool_load(lib, 0, cancel, user, &p);
  if (st == MC_OK) st = find_groups(&p, out);
  pool_free(&p);
  return st;
}

mc_status mc_radio_stations_list(mc_library *lib, int64_t now_ms, mc_radio_station_list **out) {
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_radio_station_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;

  pool p = {0};
  mc_radio_groups groups;
  mc_status st = pool_load(lib, day_start_ms(now_ms ? now_ms : (int64_t)mc_wall_ms()), NULL, NULL, &p);
  if (st == MC_OK) st = find_groups(&p, &groups);
  int seeds[MC_RADIO_STATIONS_MAX], count = 0;
  signed char *typical = NULL;
  if (st == MC_OK && groups.count) {
    if (!(typical = typical_tracks(&p, &groups))) st = MC_ERR_NOMEM;
  }
  if (st == MC_OK && typical) {
    count = groups.count;
    memcpy(seeds, groups.centre, (size_t)count * sizeof *seeds);
    settle_seeds(&p, &groups, typical, seeds, count);
    sort_seeds(&p, seeds, count);
  }
  char *albums[MC_RADIO_STATIONS_MAX] = {0};
  if (st == MC_OK && count) {
    list->items = calloc((size_t)count, sizeof *list->items);
    if (!list->items) st = MC_ERR_NOMEM;
    for (int i = 0; st == MC_OK && i < count; i++) {
      list->count = i + 1;
      st = read_seed(lib, &p.items[seeds[i]], &list->items[i], &albums[i]);
      int start = draw_start(&p, typical, mc_radio_group_of(&groups, &p.results[seeds[i]]), seeds[i]);
      memcpy(list->items[i].start_track, p.items[start].uid, sizeof list->items[i].start_track);
    }
    if (st == MC_OK) st = finish_stations(list, albums);
  }
  for (int i = 0; i < count; i++) mc_free(albums[i]);
  free(typical);
  pool_free(&p);
  if (st != MC_OK) {
    mc_radio_station_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
}

void mc_radio_station_list_free(mc_radio_station_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) station_free(&list->items[i]);
  free(list->items);
  free(list);
}
