/* See musiccore/radio_stations.h, and "Radio stations" in docs/schema.md for the reasoning.
 *
 * Choosing takes three steps: leave out the tracks too alone to make a station of, spread seeds
 * as far apart as they'll go over what's left (farthest-point selection), then let each seed give
 * way to a track close by that makes a better one. The first two work on a sample, so what they
 * cost doesn't grow with the library; the last is one pass over all of it per seed. */
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
#include "util/platform.h"

/* The same limits as a mix's own, so a seed chosen here is one mc_radio_make() takes. */
#define MIN_TRACK_MS 60000
#define MAX_TRACK_MS 480000

/* A play shorter than this was more likely skipped than counted, which the log can't say. Only
 * for telling what today added to a track's plays. */
#define COUNTED_MS 30000

enum {
  SAMPLE = 1024, /* tracks compared with each other: the matrix is 4 MB */
  NEIGHBOURS = 8 /* how a track's company is measured: the distance to this many */
};

/* A track further from its neighbours than this many times the usual is too alone to seed a
 * station: a mix from it would straight away be nothing like it. Real libraries have a few, an
 * intro or a noise track, and farthest-point selection goes for exactly those if it's let. */
#define LONELY 1.5
/* But this near, on the library's scales, is company whatever's usual: otherwise a loose genre
 * beside a tight one would be left out whole. */
#define COMPANY 0.7

/* Two seeds closer than this aren't different stations. It's on the fixed scales, as the
 * library's own put any library's tracks about 1 apart, however alike they all sound. In a real
 * library of 700 tracks two tracks of an album were 0.51 apart at the median and any two 0.62, so
 * this asks for about what two tracks picked at random from a varied library have. */
#define MIN_APART 0.6

/* On the library's scales, how close a track must be for a seed to give way to it: about as far
 * as a track's nearest few in that same library. */
#define NEARBY 0.45

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
  int count, cap;
} pool;

static void pool_free(pool *p) {
  free(p->items);
  free(p->results);
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
static mc_status pool_load(mc_library *lib, int64_t day_ms, pool *p) {
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
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
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
    p->results[p->count++] = result;
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

int mc_radio_station_limit(int tracks) {
  int limit = 0;
  for (int64_t need = MC_RADIO_STATION_MIN_TRACKS; tracks >= need && limit < MC_RADIO_STATIONS_MAX; need *= 2)
    limit++;
  return limit;
}

/* Doublings of a play count, so that one more play seldom outranks anything. */
static int plays_rank(int64_t plays) {
  int rank = 0;
  for (; plays > 0; plays >>= 1) rank++;
  return rank;
}

/* --- Spreading the seeds, over a sample --- */

typedef struct by_hash {
  unsigned hash;
  int index;
} by_hash;

static int by_hash_cmp(const void *a, const void *b) {
  const by_hash *x = a, *y = b;
  if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
  return x->index - y->index;
}

/* Up to SAMPLE tracks by the day's hash, into sample. How many, 0 if out of memory. */
static int draw_sample(const pool *p, int *sample) {
  by_hash *order = malloc((size_t)p->count * sizeof *order);
  if (!order) return 0;
  for (int i = 0; i < p->count; i++) order[i] = (by_hash){p->items[i].hash, i};
  qsort(order, (size_t)p->count, sizeof *order, by_hash_cmp);
  int n = p->count < SAMPLE ? p->count : SAMPLE;
  for (int i = 0; i < n; i++) sample[i] = order[i].index;
  free(order);
  return n;
}

static int float_cmp(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return x < y ? -1 : x > y;
}

/* Sets company[i] for each track of the sample that has some, from the distances between them
 * (n by n). tracks is the size of what it's a sample of. */
static mc_status find_company(const float *dist, int n, int tracks, char *company) {
  /* In a sample as thin as this, the same neighbours are nearer the front of the queue. Below
   * the third it says little. */
  int nth = (int)((int64_t)NEIGHBOURS * n / tracks);
  if (nth < 3) nth = 3;
  if (nth > n - 1) nth = n - 1;
  float *row = malloc((size_t)n * sizeof *row), *nearest = malloc((size_t)n * sizeof *nearest);
  if (!row || !nearest) {
    free(row);
    free(nearest);
    return MC_ERR_NOMEM;
  }
  for (int i = 0; i < n; i++) {
    memcpy(row, &dist[(size_t)i * n], (size_t)n * sizeof *row);
    qsort(row, (size_t)n, sizeof *row, float_cmp);
    nearest[i] = row[nth]; /* row[0] is the track itself */
  }
  memcpy(row, nearest, (size_t)n * sizeof *row);
  qsort(row, (size_t)n, sizeof *row, float_cmp);
  float limit = row[n / 2] * (float)LONELY;
  if (limit < (float)COMPANY) limit = (float)COMPANY;
  for (int i = 0; i < n; i++) company[i] = nearest[i] <= limit;
  free(row);
  free(nearest);
  return MC_OK;
}

/* Where to start in the sample: the most played with company, the day's hash deciding among
 * equals, so a library not played much still moves on from day to day. -1 if there's none. */
static int first_seed(const pool *p, const int *sample, const char *company, int n) {
  int best = -1;
  for (int i = 0; i < n; i++) {
    if (!company[i]) continue;
    if (best < 0) {
      best = i;
      continue;
    }
    const track *t = &p->items[sample[i]], *b = &p->items[sample[best]];
    int rank = plays_rank(t->plays), best_rank = plays_rank(b->plays);
    if (rank > best_rank || (rank == best_rank && t->hash < b->hash)) best = i;
  }
  return best;
}

/* Farthest-point selection: each seed is the track furthest from all the seeds so far, until
 * limit or until nothing is MIN_APART from them all. Fills seeds with indices into the pool and
 * sets *count. */
static mc_status spread_seeds(const pool *p, const mc_analysis_stats *stats, int limit, int *seeds, int *count) {
  *count = 0;
  int cap = p->count < SAMPLE ? p->count : SAMPLE;
  int *sample = malloc((size_t)cap * sizeof *sample);
  float *dist = malloc((size_t)cap * cap * sizeof *dist);
  float *from_seeds = malloc((size_t)cap * sizeof *from_seeds);
  float *fixed_from_seeds = malloc((size_t)cap * sizeof *fixed_from_seeds);
  char *company = malloc((size_t)cap);
  int n = sample ? draw_sample(p, sample) : 0;
  mc_status st = MC_ERR_NOMEM;
  if (n && dist && from_seeds && fixed_from_seeds && company) {
    for (int i = 0; i < n; i++) {
      dist[(size_t)i * n + i] = 0;
      for (int k = 0; k < i; k++)
        dist[(size_t)i * n + k] = dist[(size_t)k * n + i] =
          (float)mc_analysis_distance(&p->results[sample[i]], &p->results[sample[k]], stats);
    }
    st = find_company(dist, n, p->count, company);
  }
  if (st == MC_OK) {
    for (int i = 0; i < n; i++) from_seeds[i] = fixed_from_seeds[i] = FLT_MAX;
    for (int at = first_seed(p, sample, company, n); at >= 0 && *count < limit;) {
      seeds[(*count)++] = sample[at];
      int next = -1;
      for (int i = 0; i < n; i++) {
        float d = dist[(size_t)at * n + i];
        float fixed = (float)mc_analysis_distance(&p->results[sample[at]], &p->results[sample[i]], NULL);
        if (d < from_seeds[i]) from_seeds[i] = d;
        if (fixed < fixed_from_seeds[i]) fixed_from_seeds[i] = fixed;
        if (company[i] && fixed_from_seeds[i] >= MIN_APART && (next < 0 || from_seeds[i] > from_seeds[next]))
          next = i;
      }
      at = next;
    }
  }
  free(sample);
  free(dist);
  free(from_seeds);
  free(fixed_from_seeds);
  free(company);
  return st;
}

/* --- Each seed's best neighbour, over the whole library --- */

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

/* Lets each seed move to a better track near it, if that's still MIN_APART from the others. */
static void settle_seeds(const pool *p, const mc_analysis_stats *stats, int *seeds, int count) {
  for (int s = 0; s < count; s++) {
    int centre = seeds[s], best = centre;
    for (int i = 0; i < p->count; i++) {
      if (i == best || !better(p, i, best, seeds, count, s)) continue;
      if (mc_analysis_distance(&p->results[centre], &p->results[i], stats) > NEARBY) continue;
      int apart = 1;
      for (int k = 0; k < count && apart; k++)
        apart = k == s || mc_analysis_distance(&p->results[seeds[k]], &p->results[i], NULL) >= MIN_APART;
      if (apart) best = i;
    }
    seeds[s] = best;
  }
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

mc_status mc_radio_stations_list(mc_library *lib, int64_t now_ms, mc_radio_station_list **out) {
  if (!lib || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_radio_station_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;

  pool p = {0};
  mc_status st = pool_load(lib, day_start_ms(now_ms ? now_ms : (int64_t)mc_wall_ms()), &p);
  int limit = mc_radio_station_limit(p.count), seeds[MC_RADIO_STATIONS_MAX], count = 0;
  if (st == MC_OK && limit) {
    mc_analysis_stats stats;
    mc_analysis_stats_from(p.results, p.count, &stats);
    st = spread_seeds(&p, &stats, limit, seeds, &count);
    if (st == MC_OK) {
      settle_seeds(&p, &stats, seeds, count);
      sort_seeds(&p, seeds, count);
    }
  }
  char *albums[MC_RADIO_STATIONS_MAX] = {0};
  if (st == MC_OK && count) {
    list->items = calloc((size_t)count, sizeof *list->items);
    if (!list->items) st = MC_ERR_NOMEM;
    for (int i = 0; st == MC_OK && i < count; i++) {
      list->count = i + 1;
      st = read_seed(lib, &p.items[seeds[i]], &list->items[i], &albums[i]);
    }
    if (st == MC_OK) st = finish_stations(list, albums);
  }
  for (int i = 0; i < count; i++) mc_free(albums[i]);
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
