/* Runs a real import against MusicBrainz from the command line:
 *   build/<preset>/core/tests/manual_import <library folder> <folder to import> [threshold] [--wider]
 * Prints every item once matching has finished. With --wider, then asks for the extended
 * search on every album still in review that hasn't had one, and prints those again. Items needing review are left alone, so
 * nothing is written for them. Accepted files are copied into the library folder and tagged
 * there; the originals are left alone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "util/platform.h"

static mc_mutex g_lock;
static mc_cond g_cond;
static int g_idle;

static const char *state_name(mc_import_state s) {
  static const char *const names[] = {"pending", "matching", "review", "writing",
                                      "done",    "skipped",  "failed"};
  return names[s];
}

static void print_item(mc_import *imp, int idx) {
  mc_import_item *it;
  if (mc_import_item_get(imp, idx, &it) != MC_OK) return;
  printf("[%d] %-8s %s: %s - %s (%d files)%s%s%s\n", idx, state_name(it->state),
         it->kind == MC_IMPORT_SINGLE ? "single" : "album", it->artist ? it->artist : "?",
         it->album ? it->album : "?", it->file_count, it->searched_wider ? " [searched wider]" : "",
         it->error ? " - " : "", it->error ? it->error : "");
  for (int m = 0; m < it->match_count && m < 3; m++) {
    const mc_import_match *mt = &it->matches[m];
    printf("      %3.0f%%  %s - %s (%d) %s%s, %d tracks, %d extra files\n", mt->confidence * 100,
           mt->album_artist ? mt->album_artist : "?", mt->album ? mt->album : "?", mt->year,
           mt->format ? mt->format : "", mt->is_single ? " [single]" : "", mt->track_count,
           mt->extra_files);
  }
  mc_import_item_free(it);
}

static void on_event(void *user, mc_import_event ev, int arg) {
  (void)user;
  if (ev == MC_IMPORT_EV_SCANNED) printf("scanned: %d items\n", arg);
  if (ev == MC_IMPORT_EV_IDLE) {
    mc_mutex_lock(&g_lock);
    g_idle = 1;
    mc_cond_broadcast(&g_cond);
    mc_mutex_unlock(&g_lock);
  }
}

int main(int argc, char **argv) {
  int wider = argc > 3 && strcmp(argv[argc - 1], "--wider") == 0;
  if (wider) argc--;
  if (argc < 3) {
    fprintf(stderr, "usage: %s <library folder> <folder to import> [threshold] [--wider]\n", argv[0]);
    return 2;
  }
  if (mc_init() != MC_OK) return 1;
  mc_mutex_init(&g_lock);
  mc_cond_init(&g_cond);

  mc_library *lib;
  mc_status st = mc_library_open(argv[1], &lib);
  if (st != MC_OK) {
    fprintf(stderr, "library: %s\n", mc_status_str(st));
    return 1;
  }
  mc_import_options opts = {argc > 3 ? atof(argv[3]) : 0, 1};
  const char *paths[] = {argv[2]};
  mc_import *imp;
  if ((st = mc_import_start(lib, paths, 1, &opts, on_event, NULL, &imp)) != MC_OK) {
    fprintf(stderr, "import: %s\n", mc_status_str(st));
    return 1;
  }

  mc_mutex_lock(&g_lock);
  while (!g_idle) mc_cond_wait(&g_cond, &g_lock);
  mc_mutex_unlock(&g_lock);

  for (int i = 0; i < mc_import_item_count(imp); i++) print_item(imp, i);

  if (wider) {
    int count = mc_import_item_count(imp), asked = 0;
    int *ask = calloc((size_t)count + 1, sizeof *ask);
    mc_mutex_lock(&g_lock);
    g_idle = 0;
    mc_mutex_unlock(&g_lock);
    for (int i = 0; ask && i < count; i++) {
      mc_import_item *it;
      if (mc_import_item_get(imp, i, &it) != MC_OK) continue;
      if (it->kind == MC_IMPORT_ALBUM && it->state == MC_IMPORT_REVIEW && !it->searched_wider &&
          mc_import_search_wider(imp, i) == MC_OK)
        ask[i] = 1, asked++;
      mc_import_item_free(it);
    }
    printf("searching more widely for %d items\n", asked);
    /* Idle can come between two requests; wait until none is left to do. */
    for (int busy = asked; busy;) {
      mc_mutex_lock(&g_lock);
      while (!g_idle) mc_cond_wait(&g_cond, &g_lock);
      mc_mutex_unlock(&g_lock);
      busy = 0;
      for (int i = 0; i < count; i++) {
        mc_import_item *it;
        if (!ask[i] || mc_import_item_get(imp, i, &it) != MC_OK) continue;
        busy += it->state == MC_IMPORT_MATCHING ||
                (it->state == MC_IMPORT_REVIEW && !it->searched_wider && !it->error);
        mc_import_item_free(it);
      }
      if (busy) mc_sleep_ms(500);
    }
    for (int i = 0; i < count; i++)
      if (ask[i]) print_item(imp, i);
    free(ask);
  }
  mc_import_free(imp);

  int64_t albums = 0, tracks = 0;
  mc_library_count(lib, MC_ENTITY_ALBUMS, &albums);
  mc_library_count(lib, MC_ENTITY_TRACKS, &tracks);
  printf("library: %lld albums, %lld tracks\n", (long long)albums, (long long)tracks);
  mc_library_close(lib);
  mc_shutdown();
  return 0;
}
