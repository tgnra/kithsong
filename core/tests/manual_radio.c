/* Makes a radio mix (kithcore/radio.h) and saves it in the library as an ordinary playlist,
 * for judging mixes by ear before there's a radio to play them.
 *
 *   manual_radio <library folder> [--seed <text>] [--count <n>] [--name <playlist name>]
 *                [--random <n>] [--max-length <minutes>] [--scan] [--dry-run] [--migrate]
 *
 * --seed searches the library and starts from the best track found; without it the seed is
 * drawn at random. --max-length leaves out longer tracks, a seed too: 8 minutes unless it says,
 * and a negative number for no limit. --scan analyses what has no result yet first, whatever
 * the power, which takes a while on a big library. --dry-run prints the mix and saves nothing.
 *
 * A library of an older schema is refused without --migrate: migrating it is one way, and an
 * installed app older than this build then won't open it. Use a copy unless you mean to.
 * The artist refresh that opening normally starts stays off, so this makes no network calls. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/analysis.h"
#include "kithcore/kithcore.h"
#include "kithcore/playlist.h"
#include "kithcore/radio.h"
#include "kithcore/search.h"
#include "library/library_internal.h"
#include "util/platform.h"

enum { DEFAULT_COUNT = 10, MAX_COUNT = 1000 };

/* The library's schema version without migrating it: 0 if it has no database yet, -1 if it
 * can't be read. */
static int schema_on_disk(const char *dir) {
  char path[2048];
  snprintf(path, sizeof path, "%s/library.db", dir);
  sqlite3 *db = NULL;
  int version = 0;
  int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL);
  if (rc == SQLITE_CANTOPEN) {
    sqlite3_close(db);
    return 0;
  }
  sqlite3_stmt *stmt = NULL;
  if (rc != SQLITE_OK || sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) != SQLITE_OK ||
      sqlite3_step(stmt) != SQLITE_ROW)
    version = -1;
  else
    version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return version;
}

static int scan(const char *dir) {
  mc_analysis_scanner *s = NULL;
  if (mc_analysis_scanner_start_once(dir, &s) != MC_OK) return 0;
  fprintf(stderr, "Analysing...\n");
  int state;
  while ((state = mc_analysis_scanner_state(s)) == 0) mc_sleep_ms(200);
  mc_analysis_scanner_stop(s);
  return state == 1;
}

static void print_pick(mc_library *lib, int place, const mc_radio_pick *pick) {
  sqlite3_stmt *stmt;
  const char *title = "?", *artist = "?";
  int ok = sqlite3_prepare_v2(mc_library_db(lib),
                              "SELECT t.title, ar.name FROM tracks t"
                              " LEFT JOIN albums al ON al.uid = t.album_uid"
                              " LEFT JOIN artists ar ON ar.uid = coalesce(t.artist_uid, al.artist_uid)"
                              " WHERE t.uid = ?1",
                              -1, &stmt, NULL) == SQLITE_OK;
  if (ok) {
    sqlite3_bind_text(stmt, 1, pick->track_uid, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      if (sqlite3_column_text(stmt, 0)) title = (const char *)sqlite3_column_text(stmt, 0);
      if (sqlite3_column_text(stmt, 1)) artist = (const char *)sqlite3_column_text(stmt, 1);
    }
  }
  printf("%3d. %.3f  %s - %s\n", place, pick->distance, artist, title);
  if (ok) sqlite3_finalize(stmt);
}

/* The mix from the best track query finds that can seed one. */
static mc_status mix_from_search(mc_library *lib, const char *query, mc_radio_options *opts,
                                 mc_radio_pick *picks, int *n) {
  mc_search_results *found = NULL;
  mc_status st = mc_library_search(lib, query, 25, &found);
  if (st != MC_OK) return st;
  st = MC_ERR_NOT_FOUND;
  for (int i = 0; i < found->count && st == MC_ERR_NOT_FOUND; i++) {
    if (found->items[i].kind != MC_SEARCH_TRACK) continue;
    opts->seed_track = found->items[i].uid;
    st = mc_radio_make(lib, opts, picks, n);
  }
  opts->seed_track = NULL;
  mc_search_results_free(found);
  return st;
}

static mc_status save(mc_library *lib, const char *name, const mc_radio_pick *picks, int n) {
  char uid[MC_UID_STRLEN];
  mc_status st = mc_playlist_create(lib, name, uid);
  for (int i = 0; st == MC_OK && i < n; i++)
    st = mc_playlist_append(lib, uid, picks[i].track_uid, NULL);
  if (st != MC_OK) mc_playlist_delete(lib, uid);
  return st;
}

static int usage(const char *self) {
  fprintf(stderr,
          "usage: %s <library folder> [--seed <text>] [--count <n>] [--name <playlist name>]\n"
          "       [--random <n>] [--max-length <minutes>] [--scan] [--dry-run] [--migrate]\n",
          self);
  return 2;
}

int main(int argc, char **argv) {
  if (argc < 2 || argv[1][0] == '-') return usage(argv[0]);
  const char *dir = argv[1], *seed = NULL, *name = "Radio mix";
  int count = DEFAULT_COUNT, do_scan = 0, dry_run = 0, migrate = 0;
  uint32_t random = (uint32_t)mc_wall_ms();
  int64_t max_track_ms = 0;
  for (int i = 2; i < argc; i++) {
    int more = i + 1 < argc;
    if (!strcmp(argv[i], "--seed") && more) seed = argv[++i];
    else if (!strcmp(argv[i], "--count") && more) count = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--name") && more) name = argv[++i];
    else if (!strcmp(argv[i], "--random") && more) random = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (!strcmp(argv[i], "--max-length") && more) {
      char *end;
      double minutes = strtod(argv[++i], &end);
      if (end == argv[i] || *end) return usage(argv[0]);
      max_track_ms = (int64_t)(minutes * 60000.0);
    } else if (!strcmp(argv[i], "--scan")) do_scan = 1;
    else if (!strcmp(argv[i], "--dry-run")) dry_run = 1;
    else if (!strcmp(argv[i], "--migrate")) migrate = 1;
    else return usage(argv[0]);
  }
  if (count < 1 || count > MAX_COUNT || !*name) return usage(argv[0]);

  if (mc_init() != MC_OK) return 1;
  int version = schema_on_disk(dir);
  if (version < 0) {
    fprintf(stderr, "Can't read the library in %s\n", dir);
    return 1;
  }
  if (version == 0) {
    fprintf(stderr, "No library in %s\n", dir);
    return 1;
  }
  if (version < mc_library_schema_latest() && !migrate) {
    fprintf(stderr,
            "%s is at schema v%d and this build would migrate it to v%d, which an older app\n"
            "can't open. Work on a copy, or pass --migrate if you mean it.\n",
            dir, version, mc_library_schema_latest());
    return 1;
  }

  mc_library *lib = NULL;
  mc_status st = mc_library_open_internal(dir, &lib);
  if (st != MC_OK) {
    fprintf(stderr, "Can't open the library: %d\n", (int)st);
    return 1;
  }
  if (do_scan && !scan(dir)) {
    fprintf(stderr, "The scan gave up\n");
    mc_library_close(lib);
    return 1;
  }

  mc_radio_pick *picks = malloc((size_t)count * sizeof *picks);
  mc_radio_options opts = {NULL, count, random, 0, max_track_ms, NULL};
  int n = 0;
  if (!picks)
    st = MC_ERR_NOMEM;
  else if (seed)
    st = mix_from_search(lib, seed, &opts, picks, &n);
  else
    st = mc_radio_make(lib, &opts, picks, &n);
  if (st == MC_ERR_NOT_FOUND) {
    fprintf(stderr, seed ? "No analysed track matches \"%s\". Has the library been scanned (--scan)?\n"
                         : "No analysed tracks%s. Has the library been scanned (--scan)?\n",
            seed ? seed : "");
  } else if (st != MC_OK) {
    fprintf(stderr, "The mix failed: %d\n", (int)st);
  } else {
    for (int i = 0; i < n; i++) print_pick(lib, i + 1, &picks[i]);
    if (!dry_run) {
      st = save(lib, name, picks, n);
      if (st == MC_OK)
        printf("Saved as the playlist \"%s\", %d tracks.\n", name, n);
      else
        fprintf(stderr, "Saving failed: %d\n", (int)st);
    }
  }
  free(picks);
  mc_library_close(lib);
  return st == MC_OK ? 0 : 1;
}
