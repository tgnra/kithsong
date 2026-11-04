/* See musiccore/radio.h. Every candidate is compared at each pick: a library's results fit in
 * a few MB and picks are few, so there's no index to keep. A mix is a station told to end, so
 * the two pick alike. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* strdup */
#endif

#include "musiccore/radio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/analysis.h"
#include "util/platform.h"

#define MIN_TRACK_MS 60000 /* shorter is mostly intros and interludes */
#define DEFAULT_MAX_TRACK_MS (8 * 60 * 1000)

enum {
  ARTIST_GAP = 3,      /* an artist comes back after this many others */
  ALBUM_GAP = 20,      /* on a station an album does too, as it has all the time there is */
  DRAW_FROM = 8,       /* how many of the best a pick is drawn from */
  STATION_MEMORY = 64, /* picks a station remembers, for the gaps and for a new round */
  LOAD_CHECK = 256     /* rows read between asking whether to give up */
};

/* The rules a pick keeps. A small library has to give them up, in this order. */
enum { KEEP_NOTHING, KEEP_ARTISTS, KEEP_ARTISTS_AND_ALBUMS };

#define SEED_WEIGHT 0.6 /* against the track before: higher stays nearer the seed */
#define LATELY_MS (14.0 * 24 * 3600 * 1000)
#define LATELY_PENALTY 0.15 /* for a track played just now, fading to 0 over LATELY_MS */
#define DRAW_SOFTNESS 0.05  /* how much worse a score can be and still be drawn often */

typedef struct candidate {
  char uid[MC_UID_STRLEN];
  char *artist, *album, *title; /* artist and album may be NULL */
  char *path;                   /* absolute */
  int64_t last_played_ms;       /* 0 if never; a station counts its own picks */
  int used;                     /* this round */
} candidate;

typedef struct pool {
  candidate *items;
  mc_analysis *results; /* parallel to items, as mc_analysis_stats_from() wants them */
  int count, cap;
} pool;

struct mc_radio_station {
  pool pool;
  mc_analysis_stats stats;
  int seed;
  uint32_t random;
  int64_t now_ms; /* 0: the clock, at each pick */
  int *recent;    /* the last picks, a ring of recent_cap */
  int recent_cap;
  int64_t picked; /* since the start, which is what indexes the ring */
  int album_gap;
  int endless; /* starts a new round when the pool runs out, rather than ending */
};

static void pool_free(pool *p) {
  for (int i = 0; i < p->count; i++) {
    free(p->items[i].artist);
    free(p->items[i].album);
    free(p->items[i].title);
    free(p->items[i].path);
  }
  free(p->items);
  free(p->results);
}

static char *column_dup(sqlite3_stmt *stmt, int col) {
  const char *s = (const char *)sqlite3_column_text(stmt, col);
  return s ? strdup(s) : NULL;
}

static mc_status pool_grow(pool *p) {
  if (p->count < p->cap) return MC_OK;
  int cap = p->cap ? p->cap * 2 : 256;
  candidate *items = realloc(p->items, (size_t)cap * sizeof *items);
  if (!items) return MC_ERR_NOMEM;
  p->items = items;
  mc_analysis *results = realloc(p->results, (size_t)cap * sizeof *results);
  if (!results) return MC_ERR_NOMEM;
  p->results = results;
  p->cap = cap;
  return MC_OK;
}

/* Every track a mix may hold. Titles are lowered so one recording on two albums can be told.
 * ?1 is the longest track, negative for no limit. A duration that's 0 or missing is unknown,
 * which counts against a track neither way. The file is the one mc_track_info.path gives. */
#define POOL_QUERY                                                            \
  "SELECT t.uid, coalesce(t.artist_uid, al.artist_uid), t.album_uid,"         \
  " lower(t.title),"                                                          \
  " (SELECT max(tp.last_played_ms) FROM track_plays tp"                       \
  "  WHERE tp.track_uid = t.uid),"                                            \
  " (SELECT f.path FROM files f WHERE f.track_uid = t.uid"                    \
  "  ORDER BY f.source_file_uid IS NOT NULL LIMIT 1)"                         \
  " FROM tracks t LEFT JOIN albums al ON al.uid = t.album_uid"                \
  " WHERE EXISTS (SELECT 1 FROM files f WHERE f.track_uid = t.uid)"           \
  " AND EXISTS (SELECT 1 FROM track_analysis a WHERE a.track_uid = t.uid)"    \
  " AND NOT " TRACK_IN_MIX                                                    \
  " AND coalesce(nullif(t.duration_ms, 0), " MC_SQL_NUM(MIN_TRACK_MS) ") >= " \
  MC_SQL_NUM(MIN_TRACK_MS)                                                    \
  " AND (?1 < 0 OR coalesce(t.duration_ms, 0) <= ?1)"

/* The candidates, or with only_uid, that track if it is one. */
static mc_status pool_load(mc_library *lib, const mc_radio_options *opts, const char *only_uid,
                           mc_analysis_cancel_fn cancel, void *user, pool *p) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         only_uid ? POOL_QUERY " AND t.uid = ?2" : POOL_QUERY " ORDER BY t.uid", -1,
                         &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_int64(stmt, 1, opts->max_track_ms ? opts->max_track_ms : DEFAULT_MAX_TRACK_MS);
  if (only_uid) sqlite3_bind_text(stmt, 2, only_uid, -1, SQLITE_STATIC);
  mc_status st = MC_OK;
  int rc = SQLITE_DONE;
  for (unsigned rows = 1; st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW; rows++) {
    if (cancel && rows % LOAD_CHECK == 0 && cancel(user)) {
      st = MC_ERR_IN_USE;
      break;
    }
    if ((st = pool_grow(p)) != MC_OK) break;
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    const char *file = (const char *)sqlite3_column_text(stmt, 5);
    mc_analysis result;
    if (!uid || !file || mc_analysis_get(lib, uid, &result) != MC_OK ||
        result.version != MC_ANALYSIS_VERSION)
      continue; /* only results of this version compare */
    candidate *c = &p->items[p->count];
    memset(c, 0, sizeof *c);
    strncpy(c->uid, uid, sizeof c->uid - 1);
    c->artist = column_dup(stmt, 1);
    c->album = column_dup(stmt, 2);
    c->title = column_dup(stmt, 3);
    c->path = mc_library_abspath(lib, file);
    c->last_played_ms = sqlite3_column_int64(stmt, 4);
    p->results[p->count++] = result;
    if (!c->path) st = MC_ERR_NOMEM;
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

static int same(const char *a, const char *b) {
  return a && b && !strcmp(a, b);
}

/* xorshift32: a mix needs to be repeatable, not unguessable. */
static uint32_t next_random(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *state = x;
}

static double random_unit(uint32_t *state) {
  return (double)(next_random(state) >> 8) / (double)(1u << 24);
}

/* The pick back places before the next one, 1 being the last. */
static const candidate *picked_back(const mc_radio_station *s, int64_t back) {
  return &s->pool.items[s->recent[(s->picked - back) % s->recent_cap]];
}

/* Whether c may follow the picks so far under the rules in keep. Never what's been used this
 * round, which mark_used() has made every copy of a recording. */
static int allowed(const mc_radio_station *s, int c, int keep) {
  const candidate *cand = &s->pool.items[c];
  if (cand->used) return 0;
  int64_t reach = keep == KEEP_ARTISTS_AND_ALBUMS ? s->album_gap : keep == KEEP_ARTISTS ? ARTIST_GAP : 0;
  if (reach > s->recent_cap) reach = s->recent_cap;
  if (reach > s->picked) reach = s->picked;
  for (int64_t back = 1; back <= reach; back++) {
    const candidate *was = picked_back(s, back);
    if (keep == KEEP_ARTISTS_AND_ALBUMS && same(cand->album, was->album)) return 0;
    if (back <= ARTIST_GAP && same(cand->artist, was->artist)) return 0;
  }
  return 1;
}

/* Takes item c out of the round with any other copy of the recording: the song on an album
 * and on a best-of is one song to a listener. */
static void mark_used(mc_radio_station *s, const candidate *c) {
  pool *p = &s->pool;
  for (int i = 0; i < p->count; i++) {
    candidate *other = &p->items[i];
    if (other == c || (same(other->artist, c->artist) && same(other->title, c->title))) other->used = 1;
  }
}

static double score(const mc_radio_station *s, int c, int prev, int64_t now_ms) {
  const pool *p = &s->pool;
  double value = SEED_WEIGHT * mc_analysis_distance(&p->results[s->seed], &p->results[c], &s->stats) +
                 (1.0 - SEED_WEIGHT) * mc_analysis_distance(&p->results[prev], &p->results[c], &s->stats);
  int64_t played = p->items[c].last_played_ms;
  if (played > 0) {
    double age = (double)(now_ms - played);
    if (age < LATELY_MS) value += LATELY_PENALTY * (1.0 - (age > 0 ? age : 0) / LATELY_MS);
  }
  return value;
}

/* The track to follow the last pick, or -1 if the round has nothing left. */
static int pick_next(mc_radio_station *s, int64_t now_ms) {
  int best[DRAW_FROM];
  double scores[DRAW_FROM];
  int found = 0, prev = s->recent[(s->picked - 1) % s->recent_cap];
  for (int keep = KEEP_ARTISTS_AND_ALBUMS; keep >= KEEP_NOTHING && !found; keep--) {
    for (int c = 0; c < s->pool.count; c++) {
      if (!allowed(s, c, keep)) continue;
      double value = score(s, c, prev, now_ms);
      if (found == DRAW_FROM && value >= scores[found - 1]) continue;
      int at = found < DRAW_FROM ? found++ : DRAW_FROM - 1;
      for (; at > 0 && scores[at - 1] > value; at--) {
        scores[at] = scores[at - 1];
        best[at] = best[at - 1];
      }
      scores[at] = value;
      best[at] = c;
    }
  }
  if (!found) return -1;
  double total = 0, weights[DRAW_FROM];
  for (int i = 0; i < found; i++) {
    weights[i] = exp((scores[0] - scores[i]) / DRAW_SOFTNESS);
    total += weights[i];
  }
  double draw = random_unit(&s->random) * total;
  for (int i = 0; i < found; i++) {
    draw -= weights[i];
    if (draw < 0) return best[i];
  }
  return best[found - 1];
}

/* Everything may come back but the last keep picks, so a round doesn't open with what the last
 * one closed on. */
static void new_round(mc_radio_station *s, int keep) {
  for (int i = 0; i < s->pool.count; i++) s->pool.items[i].used = 0;
  if (keep > s->recent_cap) keep = s->recent_cap;
  if (keep > s->picked) keep = (int)s->picked;
  for (int back = 1; back <= keep; back++) mark_used(s, picked_back(s, back));
}

/* The seed, then whatever follows it; -1 once a mix has run out. */
static int take_pick(mc_radio_station *s, int64_t now_ms) {
  int c = s->seed;
  if (s->picked) {
    c = pick_next(s, now_ms);
    if (c < 0 && s->endless) {
      new_round(s, s->pool.count / 2);
      if ((c = pick_next(s, now_ms)) < 0) { /* those were copies of every recording there is */
        new_round(s, 0);
        c = pick_next(s, now_ms);
      }
    }
    if (c < 0) return -1;
  }
  mark_used(s, &s->pool.items[c]);
  s->pool.items[c].last_played_ms = now_ms;
  s->recent[s->picked++ % s->recent_cap] = c;
  return c;
}

/* Up to count picks; only a mix can make it fewer. Returns how many. */
static int take_picks(mc_radio_station *s, mc_radio_pick *out, int count) {
  int64_t now_ms = s->now_ms ? s->now_ms : (int64_t)mc_wall_ms();
  int n = 0;
  for (; n < count; n++) {
    int c = take_pick(s, now_ms);
    if (c < 0) break;
    memcpy(out[n].track_uid, s->pool.items[c].uid, MC_UID_STRLEN);
    out[n].distance = mc_analysis_distance(&s->pool.results[s->seed], &s->pool.results[c], &s->stats);
    out[n].path = s->pool.items[c].path;
  }
  return n;
}

/* Loads s, which has what's particular to it set and the rest zeroed. */
static mc_status station_start(mc_radio_station *s, mc_library *lib, const mc_radio_options *opts,
                               mc_analysis_cancel_fn cancel, void *user) {
  mc_status st = pool_load(lib, opts, NULL, cancel, user, &s->pool);
  if (st == MC_OK && !s->pool.count) st = MC_ERR_NOT_FOUND;
  if (st == MC_OK && !(s->recent = malloc((size_t)s->recent_cap * sizeof *s->recent))) st = MC_ERR_NOMEM;
  if (st != MC_OK) return st;

  s->random = opts->random * 2654435761u + 1u;
  if (!s->random) s->random = 1;
  s->seed = -1;
  if (!opts->seed_track) {
    s->seed = (int)(next_random(&s->random) % (uint32_t)s->pool.count);
  } else {
    for (int i = 0; i < s->pool.count && s->seed < 0; i++)
      if (!strcmp(s->pool.items[i].uid, opts->seed_track)) s->seed = i;
    if (s->seed < 0) return MC_ERR_NOT_FOUND;
  }
  mc_analysis_stats_from(s->pool.results, s->pool.count, &s->stats);
  s->now_ms = opts->now_ms;
  return MC_OK;
}

static void station_clear(mc_radio_station *s) {
  pool_free(&s->pool);
  free(s->recent);
}

mc_status mc_radio_make(mc_library *lib, const mc_radio_options *opts, mc_radio_pick *out, int *n) {
  if (!lib || !opts || !out || !n || opts->count < 1) return MC_ERR_INVALID_ARG;
  *n = 0;
  /* It remembers every pick, which keeps an album out for the whole mix. */
  mc_radio_station s = {.recent_cap = opts->count, .album_gap = opts->count};
  mc_status st = station_start(&s, lib, opts, NULL, NULL);
  if (st == MC_OK) *n = take_picks(&s, out, opts->count);
  for (int i = 0; i < *n; i++) out[i].path = NULL; /* they go with the pool */
  station_clear(&s);
  return st;
}

mc_status mc_radio_seed_check(mc_library *lib, const mc_radio_options *opts) {
  if (!lib || !opts || !opts->seed_track) return MC_ERR_INVALID_ARG;
  pool p = {0};
  mc_status st = pool_load(lib, opts, opts->seed_track, NULL, NULL, &p);
  if (st == MC_OK && !p.count) st = MC_ERR_NOT_FOUND;
  pool_free(&p);
  return st;
}

mc_status mc_radio_station_open(mc_library *lib, const mc_radio_options *opts, mc_analysis_cancel_fn cancel,
                                void *user, mc_radio_station **out) {
  if (!lib || !opts || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_radio_station *s = calloc(1, sizeof *s);
  if (!s) return MC_ERR_NOMEM;
  s->recent_cap = STATION_MEMORY;
  s->album_gap = ALBUM_GAP;
  s->endless = 1;
  mc_status st = station_start(s, lib, opts, cancel, user);
  if (st != MC_OK) {
    mc_radio_station_close(s);
    return st;
  }
  *out = s;
  return MC_OK;
}

mc_status mc_radio_station_next(mc_radio_station *station, mc_radio_pick *out, int count) {
  if (!station || !out || count < 1) return MC_ERR_INVALID_ARG;
  take_picks(station, out, count);
  return MC_OK;
}

void mc_radio_station_close(mc_radio_station *station) {
  if (!station) return;
  station_clear(station);
  free(station);
}
