/* See musiccore/radio.h. Every candidate is compared at each pick: a library's results fit in
 * a few MB and a mix is short, so there's no index to keep. */
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

enum {
  ARTIST_GAP = 3, /* an artist comes back after this many others */
  DRAW_FROM = 8   /* how many of the best a pick is drawn from */
};

#define SEED_WEIGHT 0.6 /* against the track before: higher stays nearer the seed */
#define LATELY_MS (14.0 * 24 * 3600 * 1000)
#define LATELY_PENALTY 0.15 /* for a track played just now, fading to 0 over LATELY_MS */
#define DRAW_SOFTNESS 0.05  /* how much worse a score can be and still be drawn often */

typedef struct candidate {
  char uid[MC_UID_STRLEN];
  char *artist, *album, *title; /* artist and album may be NULL */
  int64_t last_played_ms;       /* 0 if never */
  int used;
} candidate;

typedef struct pool {
  candidate *items;
  mc_analysis *results; /* parallel to items, as mc_analysis_stats_from() wants them */
  int count, cap;
} pool;

static void pool_free(pool *p) {
  for (int i = 0; i < p->count; i++) {
    free(p->items[i].artist);
    free(p->items[i].album);
    free(p->items[i].title);
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

/* Every track a mix may hold. Titles are lowered so one recording on two albums can be told. */
static mc_status pool_load(mc_library *lib, pool *p) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT t.uid, coalesce(t.artist_uid, al.artist_uid), t.album_uid,"
                         " lower(t.title),"
                         " (SELECT max(tp.last_played_ms) FROM track_plays tp"
                         "  WHERE tp.track_uid = t.uid)"
                         " FROM tracks t LEFT JOIN albums al ON al.uid = t.album_uid"
                         " WHERE EXISTS (SELECT 1 FROM files f WHERE f.track_uid = t.uid)"
                         " AND EXISTS (SELECT 1 FROM track_analysis a WHERE a.track_uid = t.uid)"
                         " AND NOT " TRACK_IN_MIX
                         " AND coalesce(t.duration_ms, " MC_SQL_NUM(MIN_TRACK_MS) ") >= "
                         MC_SQL_NUM(MIN_TRACK_MS) " ORDER BY t.uid",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  mc_status st = MC_OK;
  int rc = SQLITE_DONE;
  while (st == MC_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if ((st = pool_grow(p)) != MC_OK) break;
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    mc_analysis result;
    if (!uid || mc_analysis_get(lib, uid, &result) != MC_OK ||
        result.version != MC_ANALYSIS_VERSION)
      continue; /* only results of this version compare */
    candidate *c = &p->items[p->count];
    memset(c, 0, sizeof *c);
    strncpy(c->uid, uid, sizeof c->uid - 1);
    c->artist = column_dup(stmt, 1);
    c->album = column_dup(stmt, 2);
    c->title = column_dup(stmt, 3);
    c->last_played_ms = sqlite3_column_int64(stmt, 4);
    p->results[p->count++] = result;
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

/* Whether c may follow the picks so far. Never the same recording twice; keep_apart adds the
 * rules a small library may have to give up. */
static int allowed(const pool *p, int c, const int *picks, int picked, int keep_apart) {
  const candidate *cand = &p->items[c];
  if (cand->used) return 0;
  for (int i = 0; i < picked; i++) {
    const candidate *was = &p->items[picks[i]];
    if (same(cand->artist, was->artist) && same(cand->title, was->title)) return 0;
    if (!keep_apart) continue;
    if (same(cand->album, was->album)) return 0;
    if (i >= picked - ARTIST_GAP && same(cand->artist, was->artist)) return 0;
  }
  return 1;
}

static double score(const pool *p, const mc_analysis_stats *stats, int c, int seed, int prev,
                    int64_t now_ms) {
  double s = SEED_WEIGHT * mc_analysis_distance(&p->results[seed], &p->results[c], stats) +
             (1.0 - SEED_WEIGHT) * mc_analysis_distance(&p->results[prev], &p->results[c], stats);
  int64_t played = p->items[c].last_played_ms;
  if (played > 0) {
    double age = (double)(now_ms - played);
    if (age < LATELY_MS) s += LATELY_PENALTY * (1.0 - (age > 0 ? age : 0) / LATELY_MS);
  }
  return s;
}

/* The next track, or -1 if nothing's left to follow the picks. */
static int pick_next(const pool *p, const mc_analysis_stats *stats, const int *picks, int picked,
                     int64_t now_ms, uint32_t *random) {
  int best[DRAW_FROM];
  double scores[DRAW_FROM];
  int found = 0;
  for (int keep_apart = 1; keep_apart >= 0 && !found; keep_apart--) {
    for (int c = 0; c < p->count; c++) {
      if (!allowed(p, c, picks, picked, keep_apart)) continue;
      double s = score(p, stats, c, picks[0], picks[picked - 1], now_ms);
      if (found == DRAW_FROM && s >= scores[found - 1]) continue;
      int at = found < DRAW_FROM ? found++ : DRAW_FROM - 1;
      for (; at > 0 && scores[at - 1] > s; at--) {
        scores[at] = scores[at - 1];
        best[at] = best[at - 1];
      }
      scores[at] = s;
      best[at] = c;
    }
  }
  if (!found) return -1;
  double total = 0, weights[DRAW_FROM];
  for (int i = 0; i < found; i++) {
    weights[i] = exp((scores[0] - scores[i]) / DRAW_SOFTNESS);
    total += weights[i];
  }
  double draw = random_unit(random) * total;
  for (int i = 0; i < found; i++) {
    draw -= weights[i];
    if (draw < 0) return best[i];
  }
  return best[found - 1];
}

mc_status mc_radio_make(mc_library *lib, const mc_radio_options *opts, mc_radio_pick *out, int *n) {
  if (!lib || !opts || !out || !n || opts->count < 1) return MC_ERR_INVALID_ARG;
  *n = 0;
  pool p = {0};
  int *picks = NULL;
  mc_status st = pool_load(lib, &p);
  if (st == MC_OK && !p.count) st = MC_ERR_NOT_FOUND;
  if (st == MC_OK && !(picks = malloc((size_t)opts->count * sizeof *picks))) st = MC_ERR_NOMEM;
  if (st != MC_OK) goto done;

  uint32_t random = opts->random * 2654435761u + 1u;
  if (!random) random = 1;
  int seed = -1;
  if (!opts->seed_track) {
    seed = (int)(next_random(&random) % (uint32_t)p.count);
  } else {
    for (int i = 0; i < p.count && seed < 0; i++)
      if (!strcmp(p.items[i].uid, opts->seed_track)) seed = i;
    if (seed < 0) {
      st = MC_ERR_NOT_FOUND;
      goto done;
    }
  }

  mc_analysis_stats stats;
  mc_analysis_stats_from(p.results, p.count, &stats);
  int64_t now_ms = opts->now_ms ? opts->now_ms : (int64_t)mc_wall_ms();
  int picked = 0;
  for (int c = seed; c >= 0 && picked < opts->count;) {
    p.items[c].used = 1;
    picks[picked] = c;
    memcpy(out[picked].track_uid, p.items[c].uid, MC_UID_STRLEN);
    out[picked].distance = mc_analysis_distance(&p.results[seed], &p.results[c], &stats);
    picked++;
    if (picked < opts->count) c = pick_next(&p, &stats, picks, picked, now_ms, &random);
  }
  *n = picked;

done:
  free(picks);
  pool_free(&p);
  return st;
}
