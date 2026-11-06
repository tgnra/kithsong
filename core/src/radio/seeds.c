/* See kithcore/radio_seeds.h. */
#include "kithcore/radio_seeds.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/analysis.h"
#include "kithcore/radio.h"
#include "library/library_internal.h"

/* An artist can have thousands of tracks, and this runs as a menu opens. Each is compared with
 * each, so the cost goes with the square of this: 256 held a menu up for a quarter of a second
 * in a debug build. The middle of this many, spread over the list, is near enough. */
enum { MAX_COMPARED = 64 };

/* Totals closer than this are level: only rounding tells them apart. */
#define LEVEL 1e-9

typedef struct seed {
  char uid[MC_UID_STRLEN];
  mc_analysis result;
  double total; /* distance to the others */
} seed;

/* Over every device. Only a tie-break, so one that can't be read counts as none. */
static int64_t plays_of(mc_library *lib, const char *uid) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib), "SELECT coalesce(sum(play_count), 0) FROM track_plays WHERE track_uid = ?",
                         -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  int64_t plays = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : 0;
  sqlite3_finalize(stmt);
  return plays;
}

/* Adds the tracks of list that can seed a station to seeds, which has room for MAX_COMPARED, and
 * sets *n. With more than that it goes through the list in strides, so what's compared comes from
 * all of it, and it only comes back for the tracks between if those didn't fill it. Asking
 * mc_radio_seed_check() rather than repeating its rules means a seed from here is always one
 * that a station will take. */
static mc_status gather(mc_library *lib, const mc_track_list *list, seed *seeds, int *n) {
  int stride = (list->count + MAX_COMPARED - 1) / MAX_COMPARED;
  *n = 0;
  for (int start = 0; start < stride && *n < MAX_COMPARED; start++) {
    for (int i = start; i < list->count && *n < MAX_COMPARED; i += stride) {
      /* The default limit, as neither a menu nor a station has one of its own. */
      mc_radio_options opts = {.seed_track = list->items[i].uid};
      mc_status st = mc_radio_seed_check(lib, &opts);
      if (st == MC_ERR_NOT_FOUND) continue;
      if (st != MC_OK) return st;
      seed *s = &seeds[*n];
      if ((st = mc_analysis_get(lib, list->items[i].uid, &s->result)) != MC_OK) return st;
      memcpy(s->uid, list->items[i].uid, sizeof s->uid);
      s->total = 0;
      (*n)++;
    }
  }
  return MC_OK;
}

/* The fixed scales, not the library's: the track in the middle doesn't depend on how the rest of
 * the library spreads, and reading all of its results would hold a menu up on a big one. */
static int middle_of(mc_library *lib, seed *seeds, int n) {
  for (int i = 0; i < n; i++) {
    for (int k = 0; k < i; k++) {
      double d = mc_analysis_distance(&seeds[i].result, &seeds[k].result, NULL);
      seeds[i].total += d;
      seeds[k].total += d;
    }
  }
  double least = seeds[0].total;
  for (int i = 1; i < n; i++)
    if (seeds[i].total < least) least = seeds[i].total;
  int best = -1;
  int64_t best_plays = 0;
  for (int i = 0; i < n; i++) {
    if (seeds[i].total > least + LEVEL) continue;
    int64_t plays = plays_of(lib, seeds[i].uid);
    if (best < 0 || plays > best_plays || (plays == best_plays && strcmp(seeds[i].uid, seeds[best].uid) < 0)) {
      best = i;
      best_plays = plays;
    }
  }
  return best;
}

static mc_status seed_from(mc_library *lib, const mc_track_list *list, char out[MC_UID_STRLEN]) {
  seed *seeds = malloc(MAX_COMPARED * sizeof *seeds);
  if (!seeds) return MC_ERR_NOMEM;
  int n = 0;
  mc_status st = gather(lib, list, seeds, &n);
  if (st == MC_OK && !n) st = MC_ERR_NOT_FOUND;
  if (st == MC_OK) memcpy(out, seeds[middle_of(lib, seeds, n)].uid, MC_UID_STRLEN);
  free(seeds);
  return st;
}

mc_status mc_radio_seed_for_album(mc_library *lib, const char *album_uid, char out[MC_UID_STRLEN]) {
  if (!lib || !album_uid || !out) return MC_ERR_INVALID_ARG;
  out[0] = '\0';
  mc_track_list *list = NULL;
  mc_status st = mc_library_list_album_tracks(lib, album_uid, &list);
  if (st == MC_OK) st = seed_from(lib, list, out);
  mc_track_list_free(list);
  return st;
}

mc_status mc_radio_seed_for_artist(mc_library *lib, const char *artist_uid, char out[MC_UID_STRLEN]) {
  if (!lib || !artist_uid || !out) return MC_ERR_INVALID_ARG;
  out[0] = '\0';
  mc_track_list *list = NULL;
  mc_status st = mc_library_list_artist_tracks(lib, artist_uid, &list);
  if (st == MC_OK) st = seed_from(lib, list, out);
  mc_track_list_free(list);
  return st;
}
