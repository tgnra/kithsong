/* Manual check of scrobbling against the real service:
 *   build/<preset>/core/tests/manual_scrobble <library folder> <config dir>
 *       [<api url> <auth url> <api key> <api secret>]
 * Without the last four it's Last.fm with the build's key (-DMUSICAPP_LASTFM_KEY/SECRET);
 * for Libre.fm pass https://libre.fm/2.0/ https://libre.fm/api/auth/ and any key and secret.
 * Signs in (open the printed page and approve), then sends the library's first track as now
 * playing and as a play from five minutes ago. Use scratch folders: the sign-in is saved in
 * the config dir's sync.db. Expected output ends "Scrobbled as <user>". */
#include <stdio.h>
#include <stdlib.h>

#include "musiccore/musiccore.h"
#include "util/platform.h"

/* The first track of the first album, or NULL. */
static char *first_track(mc_library *lib) {
  mc_album_list *albums = NULL;
  mc_track_list *tracks = NULL;
  char *uid = NULL;
  if (mc_library_list_albums(lib, &albums) == MC_OK && albums->count &&
      mc_library_list_album_tracks(lib, albums->items[0].uid, &tracks) == MC_OK && tracks->count) {
    uid = mc_strdup(tracks->items[0].uid);
    printf("Track: %s - %s\n", tracks->items[0].artist ? tracks->items[0].artist : "?",
           tracks->items[0].title);
  }
  mc_track_list_free(tracks);
  mc_album_list_free(albums);
  return uid;
}

int main(int argc, char **argv) {
  if (argc != 3 && argc != 7) {
    fprintf(stderr, "usage: manual_scrobble <library folder> <config dir>"
                    " [<api url> <auth url> <api key> <api secret>]\n");
    return 2;
  }
  if (mc_init() != MC_OK) return 1;
  mc_library *lib = NULL;
  mc_scrobbler *sc = NULL;
  char *url = NULL, *track = NULL;
  mc_status st = mc_library_open(argv[1], &lib);
  if (st == MC_OK) st = mc_scrobbler_new(lib, argv[2], &sc);
  if (st == MC_OK && argc == 7) st = mc_scrobbler_set_service(sc, argv[3], argv[4], argv[5], argv[6]);
  if (st == MC_OK) st = mc_scrobbler_auth_begin(sc, &url);
  if (st == MC_OK) {
    printf("Approve at %s\nWaiting", url);
    fflush(stdout);
    uint64_t end = mc_mono_ms() + 5 * 60 * 1000;
    while ((st = mc_scrobbler_auth_finish(sc)) == MC_ERR_AUTH && mc_mono_ms() < end) {
      putchar('.');
      fflush(stdout);
      mc_sleep_ms(3000);
    }
    putchar('\n');
  }
  if (st == MC_OK && !(track = first_track(lib))) st = MC_ERR_NOT_FOUND;
  if (st == MC_OK) st = mc_scrobbler_now_playing(sc, track);
  if (st == MC_OK) st = mc_scrobbler_scrobble(sc, track, (int64_t)mc_wall_ms() - 5 * 60 * 1000);

  mc_scrobbler_status status = {0};
  if (st == MC_OK) {
    uint64_t end = mc_mono_ms() + 30 * 1000;
    mc_sleep_ms(500);
    while ((st = mc_scrobbler_status_get(sc, &status)) == MC_OK && status.pending &&
           status.last_error == MC_OK && mc_mono_ms() < end) {
      mc_scrobbler_status_clear(&status);
      mc_sleep_ms(200);
    }
  }
  if (st == MC_OK && !status.pending && status.last_error == MC_OK)
    printf("Scrobbled as %s\n", status.username ? status.username : "?");
  else if (st == MC_OK)
    fprintf(stderr, "Not sent: %d pending, %s\n", status.pending, mc_status_str(status.last_error));
  else
    fprintf(stderr, "Failed: %s\n", mc_status_str(st));
  int ok = st == MC_OK && !status.pending && status.last_error == MC_OK;
  mc_scrobbler_status_clear(&status);
  mc_free(url);
  free(track);
  mc_scrobbler_free(sc);
  mc_library_close(lib);
  mc_shutdown();
  return ok ? 0 : 1;
}
