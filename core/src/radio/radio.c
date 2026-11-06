/* See kithcore/radio.h. Every candidate is compared at each pick: a library's results fit in
 * a few MB and picks are few, so there's no index to keep. A mix is a station told to end, so
 * the two pick alike. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* strdup */
#endif

#include "kithcore/radio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/analysis.h"
#include "library/library_internal.h"
#include "radio/groups.h"
#include "util/platform.h"

#define MIN_TRACK_MS 60000 /* shorter is mostly intros and interludes */
#define DEFAULT_MAX_TRACK_MS (8 * 60 * 1000)

enum {
  ARTIST_GAP = 3,      /* an artist comes back after this many others */
  ALBUM_GAP = 20,      /* on a station an album does too, as it has all the time there is */
  DRAW_FROM = 8,       /* how many of the best a pick is drawn from */
  STATION_MEMORY = 64, /* picks a station remembers, for the gaps and for a new round */
  LOAD_CHECK = 256,    /* rows read between asking whether to give up */
  SKIPS_USED = 32      /* skipped tracks a station goes by, the strongest: each is a pass over the pool */
};

/* The rules a pick keeps. A small library has to give them up, in this order. */
enum { KEEP_NOTHING, KEEP_ARTISTS, KEEP_ARTISTS_AND_ALBUMS };

#define SEED_WEIGHT 0.6 /* against the track before: higher stays nearer the seed */
#define LATELY_MS (14.0 * 24 * 3600 * 1000)
#define LATELY_PENALTY 0.15 /* for a track played just now, fading to 0 over LATELY_MS */
#define DRAW_SOFTNESS 0.05  /* how much worse a score can be and still be drawn often */

/* What a skip says. "What skips teach a station" in docs/schema.md has the reasoning. A skip is
 * early from the first of these up to the second. Before it, previous() goes back a
 * track rather than restarting this one (the player's RESTART_MS), and next() pressed a few times
 * running is on its way past: too little was heard to be a judgement. From the second on it's no
 * longer early, which is also where stations.c takes a play to have counted. The shortest
 * candidate is a minute, so that's half way through at worst and a seventh of a usual track. */
#define SKIP_HEARD_MS 3000
#define SKIP_EARLY_MS 30000
#define SKIP_FADE_MS (60.0 * 24 * 3600 * 1000) /* straight down to nothing: half strength at 30 days */
#define SKIP_NUDGE 0.05 /* a fresh skip: DRAW_SOFTNESS, so about a third as likely against its peers */
#define SKIP_REPEATS 3.0 /* the fresh skips of a pairing that count: more says nothing new */
#define SKIP_MOST (SKIP_NUDGE * SKIP_REPEATS) /* however many agree: LATELY_PENALTY, no worse than just played */
/* How far a skip reaches, seed from seed and track from track, falling straight to nothing. On
 * the library's scales, where two random tracks are about 1 apart and a track's nearest few are
 * about MC_RADIO_NEARBY away: those get a quarter, and anything less alike none at all. */
#define SKIP_REACH 0.7
/* A play of the same track this close to where a skipped one left off means it was started again,
 * or gone back to from what came next (which Previous does in the first 3 seconds, so this is
 * that and some room). Neither is a skip. A station never brings a track back so soon itself. */
#define SKIP_BACK_MS 5000

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
  int seed;  /* what the station is: its group, and what skips are told of */
  int start; /* where it started: the first pick, and what the rest are measured from */
  uint32_t random;
  int64_t now_ms; /* 0: the clock, at each pick */
  int *recent;    /* the last picks, a ring of recent_cap */
  int recent_cap;
  int64_t picked; /* since the start, which is what indexes the ring */
  int album_gap;
  int endless; /* starts a new round when the pool runs out, rather than ending */
  double *nudges; /* what skips add to each item's score, before SKIP_MOST. NULL: nothing */
};

static void candidate_free(candidate *c) {
  free(c->artist);
  free(c->album);
  free(c->title);
  free(c->path);
}

static void pool_free(pool *p) {
  for (int i = 0; i < p->count; i++) candidate_free(&p->items[i]);
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
  double value = SEED_WEIGHT * mc_analysis_distance(&p->results[s->start], &p->results[c], &s->stats) +
                 (1.0 - SEED_WEIGHT) * mc_analysis_distance(&p->results[prev], &p->results[c], &s->stats);
  int64_t played = p->items[c].last_played_ms;
  if (played > 0) {
    double age = (double)(now_ms - played);
    if (age < LATELY_MS) value += LATELY_PENALTY * (1.0 - (age > 0 ? age : 0) / LATELY_MS);
  }
  if (s->nudges) value += s->nudges[c] < SKIP_MOST ? s->nudges[c] : SKIP_MOST;
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

/* The start, then whatever follows it; -1 once a mix has run out. */
static int take_pick(mc_radio_station *s, int64_t now_ms) {
  int c = s->start;
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
    out[n].distance = mc_analysis_distance(&s->pool.results[s->start], &s->pool.results[c], &s->stats);
    out[n].path = s->pool.items[c].path;
  }
  return n;
}

/* --- Skips --- */

/* How much of a skip carries across this distance, 1 down to 0. A distance of -1 is two results
 * that can't be compared, which aren't alike. */
static double skip_reach(double distance) {
  return distance >= 0 && distance < SKIP_REACH ? 1.0 - distance / SKIP_REACH : 0.0;
}

static int early_skip(int skipped, int64_t listened_ms) {
  return skipped && listened_ms >= SKIP_HEARD_MS && listened_ms < SKIP_EARLY_MS;
}

/* Pushes down what sounds like a track, that track first, by weight fresh skips. */
static mc_status nudge_around(mc_radio_station *s, const mc_analysis *skipped, double weight) {
  pool *p = &s->pool;
  if (!s->nudges && !(s->nudges = calloc((size_t)p->count, sizeof *s->nudges))) return MC_ERR_NOMEM;
  for (int c = 0; c < p->count; c++)
    s->nudges[c] += SKIP_NUDGE * weight * skip_reach(mc_analysis_distance(skipped, &p->results[c], &s->stats));
  return MC_OK;
}

static int uid_to_item(const void *key, const void *item) {
  return strcmp(key, ((const candidate *)item)->uid);
}

/* uid's item, or -1. A pool read whole is in order of uid. */
static int pool_find(const pool *p, const char *uid) {
  const candidate *c = bsearch(uid, p->items, (size_t)p->count, sizeof *p->items, uid_to_item);
  return c ? (int)(c - p->items) : -1;
}

/* A track's result. It needn't be a candidate: a seed or a skip on another device still says
 * what sounds like it, even if the file isn't here. 0 if it has none that compares. */
static int result_of(const mc_radio_station *s, mc_library *lib, const char *uid, mc_analysis *out) {
  int c = pool_find(&s->pool, uid);
  if (c >= 0) {
    *out = s->pool.results[c];
    return 1;
  }
  return mc_analysis_get(lib, uid, out) == MC_OK && out->version == MC_ANALYSIS_VERSION;
}

/* What a station takes from the log: a track, and how many fresh skips it comes to here. */
typedef struct skip_mark {
  char uid[MC_UID_STRLEN];
  double weight;
} skip_mark;

typedef struct skip_marks {
  skip_mark *items;
  int count, cap;
} skip_marks;

static mc_status marks_add(skip_marks *m, const char *uid, double weight) {
  if (m->count == m->cap) {
    int cap = m->cap ? m->cap * 2 : 64;
    skip_mark *items = realloc(m->items, (size_t)cap * sizeof *items);
    if (!items) return MC_ERR_NOMEM;
    m->items = items;
    m->cap = cap;
  }
  skip_mark *mark = &m->items[m->count++];
  memcpy(mark->uid, uid, sizeof mark->uid);
  mark->weight = weight;
  return MC_OK;
}

static int by_uid(const void *a, const void *b) {
  return strcmp(((const skip_mark *)a)->uid, ((const skip_mark *)b)->uid);
}

/* The strongest first. Uids settle a tie, as the same library has to give the same station. */
static int by_weight(const void *a, const void *b) {
  double x = ((const skip_mark *)a)->weight, y = ((const skip_mark *)b)->weight;
  return x > y ? -1 : x < y ? 1 : by_uid(a, b);
}

/* One track skipped on several seeds near this one is one track to compare with, and only
 * the strongest few are. */
static void marks_settle(skip_marks *m) {
  if (!m->count) return;
  qsort(m->items, (size_t)m->count, sizeof *m->items, by_uid);
  int kept = 0;
  for (int i = 1; i < m->count; i++) {
    if (!strcmp(m->items[i].uid, m->items[kept].uid)) m->items[kept].weight += m->items[i].weight;
    else m->items[++kept] = m->items[i];
  }
  m->count = kept + 1;
  qsort(m->items, (size_t)m->count, sizeof *m->items, by_weight);
  if (m->count > SKIPS_USED) m->count = SKIPS_USED;
}

/* The radio rows that can say anything, each pairing's together and in the order they happened.
 * The time range is what keeps this quick however long the log, on play_events_started: what's
 * faded has no bearing, nor has what's yet to happen, a device's clock being wrong.
 *
 * Skipping a seed says nothing of its station, which is built on it. */
#define SKIP_QUERY                                                                    \
  "SELECT context_uid, track_uid, started_ms, listened_ms, ended = 'skipped',"        \
  " ended = 'finished'"                                                                \
  " FROM play_events WHERE started_ms > ?1 AND started_ms <= ?2 AND context = 'radio'" \
  " AND ended <> 'failed' AND context_uid IS NOT NULL"                                 \
  " AND track_uid <> context_uid ORDER BY context_uid, track_uid, started_ms, uid"

/* Reads the pairings of seeds near this station's, each one's skips faded and summed, into m. */
static mc_status marks_read(mc_radio_station *s, mc_library *lib, int64_t now_ms,
                            mc_analysis_cancel_fn cancel, void *user, skip_marks *m) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib), SKIP_QUERY, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  sqlite3_bind_int64(stmt, 1, now_ms - (int64_t)SKIP_FADE_MS);
  sqlite3_bind_int64(stmt, 2, now_ms);
  const char *own = s->pool.items[s->seed].uid;
  const mc_analysis *own_result = &s->pool.results[s->seed];
  char seed[MC_UID_STRLEN] = "", track[MC_UID_STRLEN] = "";
  double reach = 0, skips = 0; /* of the seed, and the pairing so far in fresh skips */
  double before = 0;           /* skips before the last row, if it was an early skip */
  int64_t left_off = 0;        /* where that row ended; 0 if the last row wasn't one */
  mc_status st = MC_OK;
  int rc = SQLITE_DONE;
  for (unsigned rows = 1; st == MC_OK; rows++) {
    if (cancel && rows % LOAD_CHECK == 0 && cancel(user)) {
      st = MC_ERR_IN_USE;
      break;
    }
    rc = sqlite3_step(stmt);
    const char *row_seed = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(stmt, 0) : "";
    const char *row_track = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(stmt, 1) : "";
    if (!row_seed || !row_track || strlen(row_seed) >= sizeof seed || strlen(row_track) >= sizeof track) continue;
    int new_seed = strcmp(row_seed, seed) != 0;
    if (new_seed || strcmp(row_track, track)) { /* that's all of the last pairing */
      if (skips > 0 && strcmp(track, own)) st = marks_add(m, track, skips * reach);
      skips = 0;
      left_off = 0;
      strcpy(track, row_track);
    }
    if (rc != SQLITE_ROW) break;
    if (new_seed) {
      mc_analysis result;
      strcpy(seed, row_seed);
      reach = result_of(s, lib, seed, &result) ? skip_reach(mc_analysis_distance(&result, own_result, &s->stats)) : 0;
    }
    if (reach <= 0) continue;
    int64_t started = sqlite3_column_int64(stmt, 2), listened = sqlite3_column_int64(stmt, 3);
    int skipped = sqlite3_column_int(stmt, 4), finished = sqlite3_column_int(stmt, 5);
    if (left_off && llabs(started - left_off) < SKIP_BACK_MS) skips = before;
    left_off = 0;
    if (early_skip(skipped, listened)) {
      before = skips;
      left_off = started + listened;
      skips += 1.0 - (double)(now_ms - started) / SKIP_FADE_MS;
      if (skips > SKIP_REPEATS) skips = SKIP_REPEATS;
    } else if (finished && listened >= SKIP_EARLY_MS) { /* less got there by seeking */
      skips = skips > 1.0 ? skips - 1.0 : 0;            /* heard out: it does fit after all */
    }
  }
  if (st == MC_OK && rc != SQLITE_DONE) st = MC_ERR_DB;
  sqlite3_finalize(stmt);
  return st;
}

/* Reads what the play log says about this station. Nothing that applies leaves s->nudges NULL. */
static mc_status skips_load(mc_radio_station *s, mc_library *lib, int64_t now_ms,
                            mc_analysis_cancel_fn cancel, void *user) {
  skip_marks m = {0};
  mc_status st = marks_read(s, lib, now_ms, cancel, user, &m);
  if (st == MC_OK) marks_settle(&m);
  for (int i = 0; st == MC_OK && i < m.count; i++) {
    mc_analysis result;
    if (cancel && cancel(user)) st = MC_ERR_IN_USE;
    else if (result_of(s, lib, m.items[i].uid, &result)) st = nudge_around(s, &result, m.items[i].weight);
  }
  free(m.items);
  return st;
}

/* Lets go of what isn't of the seed's group, leaving the order as it was. The seed and start stay
 * whatever happens: the groups can have moved since a station's start was drawn. A library that
 * doesn't divide has nothing to let go of. */
static mc_status keep_group(mc_radio_station *s, mc_library *lib, mc_analysis_cancel_fn cancel, void *user) {
  pool *p = &s->pool;
  mc_radio_groups groups;
  mc_status st = mc_radio_groups_load(lib, cancel, user, &groups);
  if (st != MC_OK || groups.count < 2) return st;
  int group = mc_radio_group_of(&groups, &p->results[s->seed]), kept = 0, seed = s->seed, start = s->start;
  for (int i = 0; i < p->count; i++) {
    if (i != seed && i != start && mc_radio_group_of(&groups, &p->results[i]) != group) {
      candidate_free(&p->items[i]);
      continue;
    }
    if (i == seed) s->seed = kept;
    if (i == start) s->start = kept;
    p->items[kept] = p->items[i];
    p->results[kept++] = p->results[i];
  }
  p->count = kept;
  return MC_OK;
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
    if (s->seed < 0 && opts->start_track) s->seed = pool_find(&s->pool, opts->start_track);
    if (s->seed < 0) return MC_ERR_NOT_FOUND;
  }
  s->start = opts->start_track ? pool_find(&s->pool, opts->start_track) : s->seed;
  if (s->start < 0) return MC_ERR_NOT_FOUND;
  mc_analysis_stats_from(s->pool.results, s->pool.count, &s->stats);
  /* After the scales, which stay the library's: what's near on a station is what's near anywhere. */
  if ((st = keep_group(s, lib, cancel, user)) != MC_OK) return st;
  s->now_ms = opts->now_ms;
  return skips_load(s, lib, opts->now_ms ? opts->now_ms : (int64_t)mc_wall_ms(), cancel, user);
}

static void station_clear(mc_radio_station *s) {
  pool_free(&s->pool);
  free(s->recent);
  free(s->nudges);
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

mc_status mc_radio_station_heard(mc_radio_station *station, const mc_play_event *e) {
  if (!station || !e) return MC_ERR_INVALID_ARG;
  const char *seed = station->pool.items[station->seed].uid;
  if (e->context != MC_PLAY_CONTEXT_RADIO || strcmp(e->context_uid, seed) || !strcmp(e->track_uid, seed) ||
      !early_skip(e->end == MC_PLAY_END_SKIPPED, e->listened_ms))
    return MC_OK;
  int c = pool_find(&station->pool, e->track_uid);
  return c < 0 ? MC_OK : nudge_around(station, &station->pool.results[c], 1.0);
}

void mc_radio_station_close(mc_radio_station *station) {
  if (!station) return;
  station_clear(station);
  free(station);
}
